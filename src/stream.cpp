// The command stream (perf branch, 2026-10-21).
//
// The game's thread spends 15 to 25% of a frame inside DXVK's D3D9 front end (comfyprof, 2026-10-20: Elwynn,
// Goldshire, the harbour, Ironforge at 1152 x 864). Here the game's thread records each D3D9 call into a queue and
// returns, and a thread of our own (the replay thread) runs the queue in order on DXVK. The idea is Northlight's
// CommandStream (github.com/lepola/northlight, MIT); this is our own code, built for vtable hooks: the game keeps
// DXVK's real objects, and every slot of DXVK's device and resource vtables is patched in place.
//
// - Order. The wrappers sit innermost: installed with the probe device before comfyatmos's own hooks, so the hooks
//   still run on the game's thread and their calls are recorded with the game's.
// - One thread in DXVK at a time. The replay thread holds the gate (an SRW lock) while it runs a batch of commands.
//   A call that must run now on the game's thread (Create*, a query's result, the cursor) takes the gate and calls
//   DXVK directly. So DXVK never sees two threads at once, with or without D3DCREATE_MULTITHREADED.
// - Get* calls are answered from a mirror of the device's state, kept by the recorder. comfyatmos's hooks ask for
//   state hundreds of times a frame; the mirror answers without DXVK.
// - Locks. A buffer or texture lock returns memory of ours (a staging block, or for a DYNAMIC buffer a whole-buffer
//   shadow), and the unlock records an upload: the replay thread locks the real resource then and copies. A
//   texture level locked a second time is read back once into a shadow, so a lock that writes only part of its
//   rect (the font's glyph pages) keeps the rest. READONLY locks wait for the queue (StreamDrain) and lock directly.
// - Release. The game's thread never decrements a reference: Release adds one and records two releases, which the
//   replay thread runs in order, after every command that used the object. So no object dies under a queued
//   command, and DXVK destroys objects only on the replay thread. The value returned is the count less the two.
// - State blocks of type ALL are ours (comfyatmos makes them): Capture copies the mirror, Apply records the states
//   that differ.
// - Present records the frame's end and waits until the replay thread has presented the frame before: the game
//   runs at most one frame ahead.
//
// [stream] enabled = 1 turns it on at the next start. [stream] stats = 1 logs the calls a frame every 5 s.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.h"
#include "config.h"
#include "stream.h"

namespace
{
    // ------------------------------------------------------------------------------------------------ settings, ids

    bool  g_installed = false;
    bool  g_exactRelease = false;   // [stream] exactRelease: Release returns the true count (an AddRef each)
    bool  g_stats     = false;
    int   g_ahead     = 1;          // frames the game may run ahead of the replay

    IDirect3DDevice9* g_dev  = nullptr;      // the game's device, set at its first Present
    volatile bool     g_active = false;
    DWORD             g_gameTid = 0, g_replayTid = 0;

    inline DWORD Tid() { return __readfsdword(0x24); }

    // DXVK's own functions, copied from the vtables before they were patched.
    IDirect3DDevice9Vtbl        g_dx;
    IDirect3DVertexBuffer9Vtbl  g_vb;
    IDirect3DIndexBuffer9Vtbl   g_ib;
    IDirect3DTexture9Vtbl       g_tex;
    IDirect3DCubeTexture9Vtbl   g_cube;
    IDirect3DVolumeTexture9Vtbl g_vol;
    IDirect3DSurface9Vtbl       g_surf;
    IDirect3DQuery9Vtbl         g_query;
    IDirect3DSwapChain9Vtbl     g_swap;
    IDirect3DStateBlock9Vtbl    g_sbReal;

    // ------------------------------------------------------------------------------------------------ the gate

    SRWLOCK           g_gate = SRWLOCK_INIT;
    std::atomic<long> g_gateWant{0};

    struct Gate
    {
        Gate()  { g_gateWant.fetch_add(1); AcquireSRWLockExclusive(&g_gate); }
        ~Gate() { ReleaseSRWLockExclusive(&g_gate); g_gateWant.fetch_sub(1); }
    };

    // ------------------------------------------------------------------------------------------------ staging blocks

    // Blocks for lock data and big payloads, in power-of-two classes from 256 bytes; freed blocks are kept per class.
    struct BlockHead { SLIST_ENTRY link; uint32_t cls; uint32_t pad; };
    constexpr int kClasses = 20;   // 256 B .. 128 MB
    SLIST_HEADER g_free[kClasses];

    uint8_t* AllocBlock(size_t bytes)
    {
        int c = 0;
        while ((size_t(256) << c) < bytes && c < kClasses - 1)
            ++c;
        if ((size_t(256) << c) < bytes)
            return nullptr;
        if (SLIST_ENTRY* e = InterlockedPopEntrySList(&g_free[c]))
            return reinterpret_cast<uint8_t*>(e) + sizeof(BlockHead);
        const size_t total = (size_t(256) << c) + sizeof(BlockHead);
        void* p = total >= 65536 ? VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
                                 : _aligned_malloc(total, MEMORY_ALLOCATION_ALIGNMENT);
        if (!p)
            return nullptr;
        auto* h = static_cast<BlockHead*>(p);
        h->cls = c;
        return reinterpret_cast<uint8_t*>(h) + sizeof(BlockHead);
    }

    void FreeBlock(uint8_t* data)
    {
        if (!data)
            return;
        auto* h = reinterpret_cast<BlockHead*>(data - sizeof(BlockHead));
        InterlockedPushEntrySList(&g_free[h->cls], &h->link);
    }

    // ------------------------------------------------------------------------------------------------ the queue

    // Commands: a header, up to 15 argument words, then a payload, 4-byte aligned, in chunks of 1 MB. A command
    // never crosses a chunk; the producer writes kNext at a chunk's end. One producer (the game's thread), one
    // consumer (the replay thread).
    enum Op : uint16_t
    {
        kNext = 1, kSetRenderState, kSetSamplerState, kSetTextureStageState, kSetTexture, kSetTransform,
        kMultiplyTransform, kSetViewport, kSetScissorRect, kSetMaterial, kSetLight, kLightEnable, kSetClipPlane,
        kSetVSConstF, kSetVSConstI, kSetVSConstB, kSetPSConstF, kSetPSConstI, kSetPSConstB, kSetStreamSource,
        kSetStreamSourceFreq, kSetIndices, kSetVertexDeclaration, kSetFVF, kSetVertexShader, kSetPixelShader,
        kSetRenderTarget, kSetDepthStencilSurface, kSetNPatchMode, kSetSoftwareVP, kSetCurrentTexturePalette,
        kSetClipStatus, kBeginScene, kEndScene, kClear, kDrawPrimitive, kDrawIndexedPrimitive, kDrawPrimitiveUP,
        kDrawIndexedPrimitiveUP, kStretchRect, kColorFill, kUpdateSurface, kUpdateTexture, kGetRenderTargetData,
        kPresent, kSwapPresent, kSetGammaRamp, kEvictManaged, kBufUpload, kTexUpload, kTexDirtyRect, kTexGenMips,
        kTexSetLOD, kQueryIssue, kRelease, kOpCount
    };

    const char* const kOpNames[kOpCount] = {
        "", "next", "SetRenderState", "SetSamplerState", "SetTextureStageState", "SetTexture", "SetTransform",
        "MultiplyTransform", "SetViewport", "SetScissorRect", "SetMaterial", "SetLight", "LightEnable", "SetClipPlane",
        "SetVSConstF", "SetVSConstI", "SetVSConstB", "SetPSConstF", "SetPSConstI", "SetPSConstB", "SetStreamSource",
        "SetStreamSourceFreq", "SetIndices", "SetVertexDeclaration", "SetFVF", "SetVertexShader", "SetPixelShader",
        "SetRenderTarget", "SetDepthStencilSurface", "SetNPatchMode", "SetSoftwareVP", "SetCurrentTexturePalette",
        "SetClipStatus", "BeginScene", "EndScene", "Clear", "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP",
        "DrawIndexedPrimitiveUP", "StretchRect", "ColorFill", "UpdateSurface", "UpdateTexture", "GetRenderTargetData",
        "Present", "SwapPresent", "SetGammaRamp", "EvictManaged", "BufUpload", "TexUpload", "TexDirtyRect", "TexGenMips",
        "TexSetLOD", "QueryIssue", "Release",
    };

    struct Cmd
    {
        uint16_t op;
        uint16_t nargs;
        uint32_t bytes;      // the whole command, header included
        uint32_t a[1];       // nargs words, then the payload
    };

    // A state block being recorded (BeginStateBlock): its Set commands, in the queue's format.
    bool                 g_sbRec = false;
    DWORD                g_sbRecTid = 0;
    IDirect3DDevice9*    g_sbRecDev = nullptr;
    std::vector<uint8_t> g_sbBuf;
    bool                 g_sbPending = false;   // the last Reserve went into g_sbBuf
    size_t               g_sbLast = 0;
    void ExecuteDev(const struct Cmd* c, const IDirect3DDevice9Vtbl& t, IDirect3DDevice9* d);

    constexpr size_t kChunk = 1 << 20;
    SLIST_HEADER     g_freeChunks;

    uint8_t* NewChunk()
    {
        if (SLIST_ENTRY* e = InterlockedPopEntrySList(&g_freeChunks))
            return reinterpret_cast<uint8_t*>(e);
        return static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }

    // The producer's side.
    uint8_t*  g_wChunk = nullptr;
    size_t    g_wOff = 0;
    // The counters the two threads share are 32-bit (2026-10-21): a 64-bit atomic store on x86 is a locked
    // instruction, and Commit's took 6% of the game's thread. They wrap; the game's thread keeps the 64-bit count
    // and compares by difference.
    uint64_t  g_recorded = 0;                   // commands written (the game's thread only)
    std::atomic<uint32_t> g_published{0};       // commands the consumer may read
    // The consumer's side.
    uint8_t*  g_rChunk = nullptr;
    size_t    g_rOff = 0;
    std::atomic<uint32_t> g_done{0};            // commands the replay thread has finished
    std::atomic<uint32_t> g_presentsDone{0};
    uint32_t  g_presentsRecorded = 0;

    HANDLE            g_workEvent = nullptr;    // the producer wakes the consumer
    HANDLE            g_doneEvent = nullptr;    // the consumer wakes a producer that waits
    std::atomic<long> g_sleeping{0};
    std::atomic<uint32_t> g_waitFor{0};         // the producer waits until g_done reaches this
    std::atomic<long>     g_waiting{0};         // and says so here
    std::atomic<HRESULT>  g_presentHr{D3D_OK};

    // Room for a command with n argument words and extra bytes of payload. The command is not published until
    // Commit.
    Cmd* Reserve(Op op, int n, size_t extra)
    {
        const size_t bytes = (offsetof(Cmd, a) + 4 * n + extra + 3) & ~size_t(3);
        if (g_sbRec && op >= kSetRenderState && op <= kSetClipStatus && op != kSetRenderTarget && op != kSetDepthStencilSurface)
        {
            g_sbLast = g_sbBuf.size();
            g_sbBuf.resize(g_sbLast + bytes);
            Cmd* c = reinterpret_cast<Cmd*>(g_sbBuf.data() + g_sbLast);
            c->op = op;
            c->nargs = static_cast<uint16_t>(n);
            c->bytes = static_cast<uint32_t>(bytes);
            g_sbPending = true;
            return c;
        }
        if (g_wOff + bytes + 16 > kChunk)
        {
            uint8_t* next = NewChunk();
            Cmd* c = reinterpret_cast<Cmd*>(g_wChunk + g_wOff);
            c->op = kNext;
            c->nargs = 1;
            c->bytes = static_cast<uint32_t>(offsetof(Cmd, a) + 4);
            c->a[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(next));
            ++g_recorded;
            g_wChunk = next;
            g_wOff = 0;
        }
        Cmd* c = reinterpret_cast<Cmd*>(g_wChunk + g_wOff);
        c->op = op;
        c->nargs = static_cast<uint16_t>(n);
        c->bytes = static_cast<uint32_t>(bytes);
        g_wOff += bytes;
        return c;
    }

    inline uint8_t* Payload(Cmd* c) { return reinterpret_cast<uint8_t*>(&c->a[c->nargs]); }
    inline const uint8_t* Payload(const Cmd* c) { return reinterpret_cast<const uint8_t*>(&c->a[c->nargs]); }

    // Statistics (game's thread).
    uint64_t g_opCount[kOpCount] = {};
    uint64_t g_frames = 0, g_lockCalls = 0, g_lockBytes = 0, g_lockCopied = 0, g_readbacks = 0, g_drains = 0,
             g_gated = 0, g_dynLocks = 0, g_texLocks = 0, g_relock = 0, g_sbApplies = 0, g_dynWhole = 0;
    double   g_waitSec = 0.0, g_drainSec = 0.0, g_statsT0 = 0.0;
    std::atomic<uint64_t> g_replayTicks{0};

    constexpr uint32_t kPublishEvery = 256;

    void Publish()
    {
        g_published.store(static_cast<uint32_t>(g_recorded), std::memory_order_release);
        if (g_sleeping.load(std::memory_order_acquire) && g_sleeping.exchange(0))
            SetEvent(g_workEvent);
    }

    void Commit()
    {
        if (g_sbPending)
        {
            g_sbPending = false;
            // Before the stream starts, DXVK records the state block too.
            if (!g_active)
                ExecuteDev(reinterpret_cast<const Cmd*>(g_sbBuf.data() + g_sbLast), g_dx, g_sbRecDev);
            return;
        }
        ++g_recorded;
        // Published every kPublishEvery commands, and at once where the game waits (Publish): each publish could
        // wake the replay thread, and a thread that wakes and spins for every command took power the game's
        // thread needed on this laptop (2026-10-21, Elwynn 13% slower with the stream).
        if (static_cast<uint32_t>(g_recorded) - g_published.load(std::memory_order_relaxed) >= kPublishEvery)
            Publish();
    }

    // The replay thread's count as a 64-bit number, from the game's thread: it is never ahead of g_recorded.
    inline uint64_t DoneNow()
    {
        const uint32_t d = g_done.load(std::memory_order_acquire);
        return g_recorded - static_cast<uint32_t>(static_cast<uint32_t>(g_recorded) - d);
    }

    // Waits until the replay thread has finished command number seq.
    void WaitDone(uint64_t seq)
    {
        if (DoneNow() >= seq)
            return;
        Publish();
        const double t0 = Now();
        for (int spin = 0; spin < 4000 && DoneNow() < seq; ++spin)
            _mm_pause();
        while (DoneNow() < seq)
        {
            g_waitFor.store(static_cast<uint32_t>(seq));
            g_waiting.store(1);
            if (DoneNow() >= seq)
                break;
            WaitForSingleObject(g_doneEvent, 2);
        }
        g_waiting.store(0);
        g_waitSec += Now() - t0;
    }

    // ------------------------------------------------------------------------------------------------ the mirror

    constexpr int kSamplers = 21;   // 0..15, the displacement map's (256) and the four vertex samplers (257..260)
    inline int SamplerIndex(DWORD s)
    {
        if (s < 16) return static_cast<int>(s);
        if (s >= D3DDMAPSAMPLER && s <= D3DVERTEXTEXTURESAMPLER3) return static_cast<int>(16 + s - D3DDMAPSAMPLER);
        return -1;
    }
    inline DWORD SamplerOf(int i) { return i < 16 ? static_cast<DWORD>(i) : static_cast<DWORD>(D3DDMAPSAMPLER + i - 16); }

    struct Stream { IDirect3DVertexBuffer9* vb; UINT off, stride, freq; };
    struct Light  { D3DLIGHT9 l; BOOL set, on; };

    struct Mirror
    {
        DWORD rs[256];
        DWORD ss[kSamplers][14];
        DWORD tss[8][33];
        IDirect3DBaseTexture9* tex[kSamplers];
        D3DMATRIX xf[512];
        D3DVIEWPORT9 vp;
        RECT scissor;
        D3DMATERIAL9 mat;
        float clip[6][4];
        float vsF[256][4];
        int   vsI[16][4];
        BOOL  vsB[16];
        float psF[224][4];
        int   psI[16][4];
        BOOL  psB[16];
        Stream st[16];
        IDirect3DIndexBuffer9* ib;
        IDirect3DVertexDeclaration9* decl;
        DWORD fvf;
        IDirect3DVertexShader9* vs;
        IDirect3DPixelShader9* ps;
        IDirect3DSurface9* rt[4];
        IDirect3DSurface9* ds;
        float npatch;
        BOOL  swvp;
        UINT  palette;
        Light lights[32];
    };
    Mirror* g_m = nullptr;

    template <class T> void DropRef(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); }
    }

    // The device's state read from DXVK, with the replay thread stopped or not yet started.
    void LoadMirror(IDirect3DDevice9* d)
    {
        Mirror& m = *g_m;
        memset(&m, 0, sizeof(m));
        for (DWORD i = 0; i < 256; ++i)
            if (FAILED(g_dx.GetRenderState(d, static_cast<D3DRENDERSTATETYPE>(i), &m.rs[i])))
                m.rs[i] = 0;
        for (int s = 0; s < kSamplers; ++s)
        {
            for (DWORD t = 1; t < 14; ++t)
                g_dx.GetSamplerState(d, SamplerOf(s), static_cast<D3DSAMPLERSTATETYPE>(t), &m.ss[s][t]);
            IDirect3DBaseTexture9* tx = nullptr;
            g_dx.GetTexture(d, SamplerOf(s), &tx);
            m.tex[s] = tx;
            DropRef(tx);
        }
        for (DWORD st = 0; st < 8; ++st)
            for (DWORD t = 1; t < 33; ++t)
                g_dx.GetTextureStageState(d, st, static_cast<D3DTEXTURESTAGESTATETYPE>(t), &m.tss[st][t]);
        for (DWORD i = 0; i < 512; ++i)
            if (FAILED(g_dx.GetTransform(d, static_cast<D3DTRANSFORMSTATETYPE>(i), &m.xf[i])))
                memset(&m.xf[i], 0, sizeof(D3DMATRIX));
        g_dx.GetViewport(d, &m.vp);
        g_dx.GetScissorRect(d, &m.scissor);
        g_dx.GetMaterial(d, &m.mat);
        for (DWORD i = 0; i < 6; ++i)
            g_dx.GetClipPlane(d, i, m.clip[i]);
        g_dx.GetVertexShaderConstantF(d, 0, &m.vsF[0][0], 256);
        g_dx.GetVertexShaderConstantI(d, 0, &m.vsI[0][0], 16);
        g_dx.GetVertexShaderConstantB(d, 0, m.vsB, 16);
        g_dx.GetPixelShaderConstantF(d, 0, &m.psF[0][0], 224);
        g_dx.GetPixelShaderConstantI(d, 0, &m.psI[0][0], 16);
        g_dx.GetPixelShaderConstantB(d, 0, m.psB, 16);
        for (UINT i = 0; i < 16; ++i)
        {
            IDirect3DVertexBuffer9* vb = nullptr;
            g_dx.GetStreamSource(d, i, &vb, &m.st[i].off, &m.st[i].stride);
            m.st[i].vb = vb;
            DropRef(vb);
            g_dx.GetStreamSourceFreq(d, i, &m.st[i].freq);
        }
        IDirect3DIndexBuffer9* ib = nullptr;
        g_dx.GetIndices(d, &ib);
        m.ib = ib;
        DropRef(ib);
        IDirect3DVertexDeclaration9* decl = nullptr;
        g_dx.GetVertexDeclaration(d, &decl);
        m.decl = decl;
        DropRef(decl);
        g_dx.GetFVF(d, &m.fvf);
        IDirect3DVertexShader9* vs = nullptr;
        g_dx.GetVertexShader(d, &vs);
        m.vs = vs;
        DropRef(vs);
        IDirect3DPixelShader9* ps = nullptr;
        g_dx.GetPixelShader(d, &ps);
        m.ps = ps;
        DropRef(ps);
        for (DWORD i = 0; i < 4; ++i)
        {
            IDirect3DSurface9* s = nullptr;
            g_dx.GetRenderTarget(d, i, &s);
            m.rt[i] = s;
            DropRef(s);
        }
        IDirect3DSurface9* ds = nullptr;
        g_dx.GetDepthStencilSurface(d, &ds);
        m.ds = ds;
        DropRef(ds);
        m.npatch = g_dx.GetNPatchMode(d);
        m.swvp = g_dx.GetSoftwareVertexProcessing(d);
        g_dx.GetCurrentTexturePalette(d, &m.palette);
        for (DWORD i = 0; i < 32; ++i)
        {
            m.lights[i].set = SUCCEEDED(g_dx.GetLight(d, i, &m.lights[i].l));
            if (m.lights[i].set)
                g_dx.GetLightEnable(d, i, &m.lights[i].on);
        }
    }

    // ------------------------------------------------------------------------------------------------ modes

    // kSbRec: between BeginStateBlock and EndStateBlock on the thread that began it. The Set calls go into the
    // state block's list of commands, not to the stream (and before the stream starts, to DXVK as well).
    enum Mode { kDirect, kRecord, kGated, kSbRec };

    inline Mode ModeFor(IDirect3DDevice9* d)
    {
        if (g_sbRec && d && d == g_sbRecDev && Tid() == g_sbRecTid)
            return kSbRec;
        if (!g_active || (d && d != g_dev))
            return kDirect;
        const DWORD t = Tid();
        if (t == g_replayTid)
            return kDirect;
        if (t == g_gameTid)
            return kRecord;
        return kGated;
    }
    inline Mode ModeNow() { return ModeFor(nullptr); }

    void FlushReleases();

    // Waits until the replay thread has run every command so far, the gathered releases included (Reset needs the
    // game's released resources gone).
    void Drain()
    {
        if (!g_active || Tid() != g_gameTid)
            return;
        FlushReleases();
        const double t0 = Now();
        ++g_drains;
        WaitDone(g_recorded);
        g_drainSec += Now() - t0;
    }

    // ------------------------------------------------------------------------------------------------ release

    // A release recorded: the replay thread runs it after every command that used the object.
    // Releases are gathered and sent as one command of up to kReleaseBatch (2026-10-21): 5,700 a frame at Elwynn,
    // mostly the references comfyatmos's hooks take with Get* calls. Sent later than they were made, never earlier,
    // so no object dies before a command that uses it.
    constexpr int kReleaseBatch = 1024;
    uint32_t g_relBatch[kReleaseBatch];
    int      g_relCount = 0;

    void FlushReleases()
    {
        if (!g_relCount)
            return;
        Cmd* c = Reserve(kRelease, 0, static_cast<size_t>(g_relCount) * 4);
        memcpy(Payload(c), g_relBatch, static_cast<size_t>(g_relCount) * 4);
        g_relCount = 0;
        Commit();
    }

    inline void RecordRelease(IUnknown* o, int times)
    {
        for (int i = 0; i < times; ++i)
        {
            if (g_relCount == kReleaseBatch)
                FlushReleases();
            g_relBatch[g_relCount++] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o));
        }
        ++g_opCount[kRelease];
    }

    template <class Obj, class Vtbl>
    ULONG ReleaseVia(Obj* o, const Vtbl& real)
    {
        const Mode m = ModeNow();
        if (m == kDirect)
            return real.Release(o);
        if (m == kGated)
        {
            Gate g;
            return real.Release(o);
        }
        // One release recorded, and 1 returned (2026-10-21): the count is not known here. Microsoft's documentation
        // gives Release's value for tests only, and neither the game nor comfyatmos reads it. An AddRef to learn the
        // count was one more atomic step on a count the replay thread also changes, for each of some 6,000 releases a
        // frame.
        if (g_exactRelease)
        {
            const ULONG n = real.AddRef(o);
            RecordRelease(reinterpret_cast<IUnknown*>(o), 2);
            return n - 2;
        }
        RecordRelease(reinterpret_cast<IUnknown*>(o), 1);
        return 1;
    }

    ULONG STDMETHODCALLTYPE W_VbRelease(IDirect3DVertexBuffer9* o)  { return ReleaseVia(o, g_vb); }
    ULONG STDMETHODCALLTYPE W_IbRelease(IDirect3DIndexBuffer9* o)   { return ReleaseVia(o, g_ib); }
    ULONG STDMETHODCALLTYPE W_TexRelease(IDirect3DTexture9* o)      { return ReleaseVia(o, g_tex); }
    ULONG STDMETHODCALLTYPE W_CubeRelease(IDirect3DCubeTexture9* o) { return ReleaseVia(o, g_cube); }
    ULONG STDMETHODCALLTYPE W_VolRelease(IDirect3DVolumeTexture9* o){ return ReleaseVia(o, g_vol); }
    ULONG STDMETHODCALLTYPE W_SurfRelease(IDirect3DSurface9* o)     { return ReleaseVia(o, g_surf); }
    ULONG STDMETHODCALLTYPE W_QueryRelease(IDirect3DQuery9* o)      { return ReleaseVia(o, g_query); }
    ULONG STDMETHODCALLTYPE W_SwapRelease(IDirect3DSwapChain9* o)   { return ReleaseVia(o, g_swap); }
    ULONG STDMETHODCALLTYPE W_SbRealRelease(IDirect3DStateBlock9* o){ return ReleaseVia(o, g_sbReal); }

    // Shaders and declarations: their vtables are patched at their first creation.
    IDirect3DVertexShader9Vtbl      g_vsReal;
    IDirect3DPixelShader9Vtbl       g_psReal;
    IDirect3DVertexDeclaration9Vtbl g_declReal;
    bool g_vsPatched = false, g_psPatched = false, g_declPatched = false;
    ULONG STDMETHODCALLTYPE W_VsRelease(IDirect3DVertexShader9* o)        { return ReleaseVia(o, g_vsReal); }
    ULONG STDMETHODCALLTYPE W_PsRelease(IDirect3DPixelShader9* o)         { return ReleaseVia(o, g_psReal); }
    ULONG STDMETHODCALLTYPE W_DeclRelease(IDirect3DVertexDeclaration9* o) { return ReleaseVia(o, g_declReal); }

    void PatchSlot(void** slot, void* fn)
    {
        DWORD prot = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot))
            return;
        InterlockedExchangePointer(slot, fn);
        VirtualProtect(slot, sizeof(void*), prot, &prot);
    }

    template <class Vtbl, class Obj>
    void PatchReleaseOnce(Obj* o, Vtbl& copy, bool& done, void* fn)
    {
        if (done || !o)
            return;
        auto* v = const_cast<Vtbl*>(o->lpVtbl);
        if (v->Release == fn)
        {
            done = true;
            return;
        }
        memcpy(&copy, v, sizeof(Vtbl));
        PatchSlot(reinterpret_cast<void**>(&v->Release), fn);
        done = true;
    }

    // ------------------------------------------------------------------------------------------------ recording helpers

    inline uint32_t P(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
    template <class T> inline T* Q(uint32_t v) { return reinterpret_cast<T*>(static_cast<uintptr_t>(v)); }
    inline uint32_t F(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
    inline float    Fl(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

    template <class... A>
    inline void Rec(Op op, A... args)
    {
        constexpr int n = sizeof...(A);
        Cmd* c = Reserve(op, n, 0);
        const uint32_t v[n + 1] = { static_cast<uint32_t>(args)..., 0 };
        for (int i = 0; i < n; ++i)
            c->a[i] = v[i];
        ++g_opCount[op];
        Commit();
    }

    // A command with a copied payload.
    inline Cmd* RecP(Op op, int n, const void* data, size_t bytes)
    {
        Cmd* c = Reserve(op, n, bytes);
        if (bytes)
            memcpy(Payload(c), data, bytes);
        ++g_opCount[op];
        return c;
    }

    UINT VertsForPrims(D3DPRIMITIVETYPE prim, UINT n)
    {
        switch (prim)
        {
        case D3DPT_POINTLIST:     return n;
        case D3DPT_LINELIST:      return n * 2;
        case D3DPT_LINESTRIP:     return n + 1;
        case D3DPT_TRIANGLELIST:  return n * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN:   return n + 2;
        default:                  return 0;
        }
    }

    // ------------------------------------------------------------------------------------------------ device wrappers

#define DEV IDirect3DDevice9* d
#define STD STDMETHODCALLTYPE

    HRESULT STD W_QueryInterface(DEV, REFIID r, void** p) { return g_dx.QueryInterface(d, r, p); }
    ULONG   STD W_AddRef(DEV) { return g_dx.AddRef(d); }
    ULONG   STD W_Release(DEV)
    {
        const Mode m = ModeFor(d);
        if (m == kRecord)
        {
            const ULONG n = g_dx.AddRef(d);
            if (n > 3)
            {
                RecordRelease(reinterpret_cast<IUnknown*>(d), 2);
                return n - 2;
            }
            // The device's last references: the game is closing. Run the queue out and stop recording.
            Drain();
            g_active = false;
            Gate g;
            g_dx.Release(d);
            return g_dx.Release(d);
        }
        if (m == kGated) { Gate g; return g_dx.Release(d); }
        return g_dx.Release(d);
    }

    HRESULT STD W_TestCooperativeLevel(DEV)
    {
        const Mode m = ModeFor(d);
        if (m == kRecord)
        {
            const HRESULT hr = g_presentHr.load();
            return hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET ? hr : D3D_OK;
        }
        if (m == kGated) { Gate g; return g_dx.TestCooperativeLevel(d); }
        return g_dx.TestCooperativeLevel(d);
    }

    // Calls that run on the game's thread at once, under the gate: they read what does not change with the stream.
#define GATED(ret, name, params, args)                                         \
    ret STD W_##name params                                                   \
    {                                                                         \
        if (ModeFor(d) == kDirect) return g_dx.name args;                     \
        ++g_gated;                                                            \
        Gate g;                                                               \
        return g_dx.name args;                                                \
    }
    // Calls that need every earlier command to have run first.
#define SYNCED(ret, name, params, args)                                        \
    ret STD W_##name params                                                   \
    {                                                                         \
        const Mode m = ModeFor(d);                                            \
        if (m == kDirect) return g_dx.name args;                              \
        if (m == kRecord) Drain();                                            \
        Gate g;                                                               \
        return g_dx.name args;                                                \
    }

    GATED(UINT,    GetAvailableTextureMem, (DEV), (d))
    GATED(HRESULT, GetDirect3D, (DEV, IDirect3D9** p), (d, p))
    GATED(HRESULT, GetDeviceCaps, (DEV, D3DCAPS9* p), (d, p))
    GATED(HRESULT, GetDisplayMode, (DEV, UINT s, D3DDISPLAYMODE* p), (d, s, p))
    GATED(HRESULT, GetCreationParameters, (DEV, D3DDEVICE_CREATION_PARAMETERS* p), (d, p))
    SYNCED(HRESULT, SetCursorProperties, (DEV, UINT x, UINT y, IDirect3DSurface9* s), (d, x, y, s))
    GATED(BOOL,    ShowCursor, (DEV, BOOL b), (d, b))
    GATED(HRESULT, CreateAdditionalSwapChain, (DEV, D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** p), (d, pp, p))
    GATED(HRESULT, GetSwapChain, (DEV, UINT i, IDirect3DSwapChain9** p), (d, i, p))
    GATED(UINT,    GetNumberOfSwapChains, (DEV), (d))
    GATED(HRESULT, GetBackBuffer, (DEV, UINT s, UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9** p), (d, s, i, t, p))
    GATED(HRESULT, GetRasterStatus, (DEV, UINT s, D3DRASTER_STATUS* p), (d, s, p))
    GATED(HRESULT, SetDialogBoxMode, (DEV, BOOL b), (d, b))
    SYNCED(HRESULT, GetFrontBufferData, (DEV, UINT s, IDirect3DSurface9* p), (d, s, p))
    SYNCED(HRESULT, ValidateDevice, (DEV, DWORD* p), (d, p))
    SYNCED(HRESULT, GetClipStatus, (DEV, D3DCLIPSTATUS9* p), (d, p))
    SYNCED(HRESULT, SetPaletteEntries, (DEV, UINT n, const PALETTEENTRY* p), (d, n, p))
    SYNCED(HRESULT, GetPaletteEntries, (DEV, UINT n, PALETTEENTRY* p), (d, n, p))
    SYNCED(HRESULT, ProcessVertices, (DEV, UINT a, UINT b, UINT c, IDirect3DVertexBuffer9* vb, IDirect3DVertexDeclaration9* dc, DWORD f), (d, a, b, c, vb, dc, f))
    SYNCED(HRESULT, DrawRectPatch, (DEV, UINT h, const float* s, const D3DRECTPATCH_INFO* i), (d, h, s, i))
    SYNCED(HRESULT, DrawTriPatch, (DEV, UINT h, const float* s, const D3DTRIPATCH_INFO* i), (d, h, s, i))
    SYNCED(HRESULT, DeletePatch, (DEV, UINT h), (d, h))

    void STD W_SetCursorPosition(DEV, int x, int y, DWORD f)
    {
        if (ModeFor(d) == kDirect) { g_dx.SetCursorPosition(d, x, y, f); return; }
        Gate g;
        g_dx.SetCursorPosition(d, x, y, f);
    }

    void STD W_GetGammaRamp(DEV, UINT s, D3DGAMMARAMP* r)
    {
        const Mode m = ModeFor(d);
        if (m == kDirect) { g_dx.GetGammaRamp(d, s, r); return; }
        if (m == kRecord) Drain();
        Gate g;
        g_dx.GetGammaRamp(d, s, r);
    }

    void STD W_SetGammaRamp(DEV, UINT s, DWORD f, const D3DGAMMARAMP* r)
    {
        const Mode m = ModeFor(d);
        if (m == kDirect) { g_dx.SetGammaRamp(d, s, f, r); return; }
        if (m == kGated) { Gate g; g_dx.SetGammaRamp(d, s, f, r); return; }
        Cmd* c = RecP(kSetGammaRamp, 2, r, sizeof(D3DGAMMARAMP));
        c->a[0] = s;
        c->a[1] = f;
        Commit();
    }

    HRESULT STD W_EvictManagedResources(DEV)
    {
        const Mode m = ModeFor(d);
        if (m == kDirect) return g_dx.EvictManagedResources(d);
        if (m == kGated) { Gate g; return g_dx.EvictManagedResources(d); }
        Rec(kEvictManaged);
        return D3D_OK;
    }

    std::unordered_map<void*, DWORD> g_declFvf;   // see DeclFvf

    // Creation: under the gate, at once. The new object is not in any queued command yet.
#define CREATE(name, params, args, post)                                       \
    HRESULT STD W_##name params                                               \
    {                                                                         \
        if (ModeFor(d) == kDirect) { const HRESULT hr = g_dx.name args; post; return hr; } \
        ++g_gated;                                                            \
        HRESULT hr;                                                           \
        { Gate g; hr = g_dx.name args; }                                      \
        post;                                                                 \
        return hr;                                                            \
    }

    void Forget(void* o);   // below, with the lock tables
#define FORGET if (SUCCEEDED(hr) && o && *o && ModeFor(d) == kRecord) Forget(*o)

    CREATE(CreateTexture, (DEV, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** o, HANDLE* s), (d, w, h, l, u, f, p, o, s), FORGET)
    CREATE(CreateVolumeTexture, (DEV, UINT w, UINT h, UINT dp, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** o, HANDLE* s), (d, w, h, dp, l, u, f, p, o, s), (void)0)
    CREATE(CreateCubeTexture, (DEV, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** o, HANDLE* s), (d, e, l, u, f, p, o, s), (void)0)
    CREATE(CreateVertexBuffer, (DEV, UINT n, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** o, HANDLE* s), (d, n, u, fvf, p, o, s), FORGET)
    CREATE(CreateIndexBuffer, (DEV, UINT n, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** o, HANDLE* s), (d, n, u, f, p, o, s), FORGET)
    CREATE(CreateRenderTarget, (DEV, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL l, IDirect3DSurface9** o, HANDLE* s), (d, w, h, f, ms, q, l, o, s), (void)0)
    CREATE(CreateDepthStencilSurface, (DEV, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL dc, IDirect3DSurface9** o, HANDLE* s), (d, w, h, f, ms, q, dc, o, s), (void)0)
    CREATE(CreateOffscreenPlainSurface, (DEV, UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** o, HANDLE* s), (d, w, h, f, p, o, s), FORGET)
    CREATE(CreateVertexDeclaration, (DEV, const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** o), (d, e, o),
           if (SUCCEEDED(hr) && o) { PatchReleaseOnce(*o, g_declReal, g_declPatched, reinterpret_cast<void*>(&W_DeclRelease));
                                     g_declFvf.erase(*o); })
    CREATE(CreateVertexShader, (DEV, const DWORD* fn, IDirect3DVertexShader9** o), (d, fn, o),
           if (SUCCEEDED(hr) && o) PatchReleaseOnce(*o, g_vsReal, g_vsPatched, reinterpret_cast<void*>(&W_VsRelease)))
    CREATE(CreatePixelShader, (DEV, const DWORD* fn, IDirect3DPixelShader9** o), (d, fn, o),
           if (SUCCEEDED(hr) && o) PatchReleaseOnce(*o, g_psReal, g_psPatched, reinterpret_cast<void*>(&W_PsRelease)))
    CREATE(CreateQuery, (DEV, D3DQUERYTYPE t, IDirect3DQuery9** o), (d, t, o), (void)0)

    HRESULT STD W_Reset(DEV, D3DPRESENT_PARAMETERS* pp)
    {
        const Mode m = ModeFor(d);
        if (m == kDirect) return g_dx.Reset(d, pp);
        if (m == kRecord) Drain();
        Gate g;
        const HRESULT hr = g_dx.Reset(d, pp);
        LoadMirror(d);
        g_presentHr.store(D3D_OK);
        Log("stream: device reset (hr 0x%08X), the mirror read again", static_cast<unsigned>(hr));
        return hr;
    }

    void Activate(IDirect3DDevice9* d);

    HRESULT STD W_Present(DEV, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
    {
        if (g_installed && !g_active && !g_dev && !src && !dst && !dirty)
            Activate(d);
        const Mode m = ModeFor(d);
        if (m == kDirect) return g_dx.Present(d, src, dst, wnd, dirty);
        if (m == kGated || src || dst || dirty)
        {
            if (m == kRecord) Drain();
            Gate g;
            return g_dx.Present(d, src, dst, wnd, dirty);
        }
        FlushReleases();
        Rec(kPresent, P(wnd));
        Publish();
        ++g_presentsRecorded;
        ++g_frames;
        // At most g_ahead frames ahead: wait for the replay of the Present g_ahead frames back.
        if (g_presentsRecorded > static_cast<uint32_t>(g_ahead))
        {
            const uint32_t want = g_presentsRecorded - g_ahead;
            auto behind = [&] { return static_cast<int32_t>(g_presentsDone.load(std::memory_order_acquire) - want) < 0; };
            if (behind())
            {
                const double t0 = Now();
                while (behind())
                {
                    g_waitFor.store(g_done.load());   // any progress wakes us
                    g_waiting.store(1);
                    if (!behind())
                        break;
                    WaitForSingleObject(g_doneEvent, 1);
                }
                g_waiting.store(0);
                g_waitSec += Now() - t0;
            }
        }
        StreamFrameStats();
        const HRESULT hr = g_presentHr.load();
        return hr == D3DERR_DEVICELOST ? hr : D3D_OK;
    }

    // ---- state setters: the mirror, then the command

    HRESULT STD W_SetRenderState(DEV, D3DRENDERSTATETYPE s, DWORD v)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetRenderState(d, s, v); } return g_dx.SetRenderState(d, s, v); }
        if (static_cast<DWORD>(s) < 256) g_m->rs[s] = v;
        Rec(kSetRenderState, s, v);
        return D3D_OK;
    }
    HRESULT STD W_GetRenderState(DEV, D3DRENDERSTATETYPE s, DWORD* v)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetRenderState(d, s, v); } return g_dx.GetRenderState(d, s, v); }
        if (!v || static_cast<DWORD>(s) >= 256) return D3DERR_INVALIDCALL;
        *v = g_m->rs[s];
        return D3D_OK;
    }

    HRESULT STD W_SetSamplerState(DEV, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetSamplerState(d, s, t, v); } return g_dx.SetSamplerState(d, s, t, v); }
        const int i = SamplerIndex(s);
        if (i < 0 || static_cast<DWORD>(t) >= 14) return D3DERR_INVALIDCALL;
        g_m->ss[i][t] = v;
        Rec(kSetSamplerState, s, t, v);
        return D3D_OK;
    }
    HRESULT STD W_GetSamplerState(DEV, DWORD s, D3DSAMPLERSTATETYPE t, DWORD* v)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetSamplerState(d, s, t, v); } return g_dx.GetSamplerState(d, s, t, v); }
        const int i = SamplerIndex(s);
        if (i < 0 || !v || static_cast<DWORD>(t) >= 14) return D3DERR_INVALIDCALL;
        *v = g_m->ss[i][t];
        return D3D_OK;
    }

    HRESULT STD W_SetTextureStageState(DEV, DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetTextureStageState(d, st, t, v); } return g_dx.SetTextureStageState(d, st, t, v); }
        if (st >= 8 || static_cast<DWORD>(t) >= 33) return D3DERR_INVALIDCALL;
        g_m->tss[st][t] = v;
        Rec(kSetTextureStageState, st, t, v);
        return D3D_OK;
    }
    HRESULT STD W_GetTextureStageState(DEV, DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD* v)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetTextureStageState(d, st, t, v); } return g_dx.GetTextureStageState(d, st, t, v); }
        if (st >= 8 || !v || static_cast<DWORD>(t) >= 33) return D3DERR_INVALIDCALL;
        *v = g_m->tss[st][t];
        return D3D_OK;
    }

    HRESULT STD W_SetTexture(DEV, DWORD s, IDirect3DBaseTexture9* t)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetTexture(d, s, t); } return g_dx.SetTexture(d, s, t); }
        const int i = SamplerIndex(s);
        if (i < 0) return D3DERR_INVALIDCALL;
        g_m->tex[i] = t;
        Rec(kSetTexture, s, P(t));
        return D3D_OK;
    }
    HRESULT STD W_GetTexture(DEV, DWORD s, IDirect3DBaseTexture9** t)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetTexture(d, s, t); } return g_dx.GetTexture(d, s, t); }
        const int i = SamplerIndex(s);
        if (i < 0 || !t) return D3DERR_INVALIDCALL;
        *t = g_m->tex[i];
        if (*t) (*t)->lpVtbl->AddRef(*t);
        return D3D_OK;
    }

    HRESULT STD W_SetTransform(DEV, D3DTRANSFORMSTATETYPE s, const D3DMATRIX* x)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetTransform(d, s, x); } return g_dx.SetTransform(d, s, x); }
        if (!x || static_cast<DWORD>(s) >= 512) return D3DERR_INVALIDCALL;
        g_m->xf[s] = *x;
        Cmd* c = RecP(kSetTransform, 1, x, sizeof(D3DMATRIX));
        c->a[0] = s;
        Commit();
        return D3D_OK;
    }
    HRESULT STD W_GetTransform(DEV, D3DTRANSFORMSTATETYPE s, D3DMATRIX* x)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetTransform(d, s, x); } return g_dx.GetTransform(d, s, x); }
        if (!x || static_cast<DWORD>(s) >= 512) return D3DERR_INVALIDCALL;
        *x = g_m->xf[s];
        return D3D_OK;
    }
    HRESULT STD W_MultiplyTransform(DEV, D3DTRANSFORMSTATETYPE s, const D3DMATRIX* x)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.MultiplyTransform(d, s, x); } return g_dx.MultiplyTransform(d, s, x); }
        if (!x || static_cast<DWORD>(s) >= 512) return D3DERR_INVALIDCALL;
        D3DMATRIX r, cur = g_m->xf[s];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = x->m[i][0] * cur.m[0][j] + x->m[i][1] * cur.m[1][j] + x->m[i][2] * cur.m[2][j] + x->m[i][3] * cur.m[3][j];
        return W_SetTransform(d, s, &r);
    }

    HRESULT STD W_SetViewport(DEV, const D3DVIEWPORT9* v)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetViewport(d, v); } return g_dx.SetViewport(d, v); }
        if (!v) return D3DERR_INVALIDCALL;
        g_m->vp = *v;
        Cmd* c = RecP(kSetViewport, 0, v, sizeof(*v));
        (void)c;
        Commit();
        return D3D_OK;
    }
    HRESULT STD W_GetViewport(DEV, D3DVIEWPORT9* v)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetViewport(d, v); } return g_dx.GetViewport(d, v); }
        if (!v) return D3DERR_INVALIDCALL;
        *v = g_m->vp;
        return D3D_OK;
    }

    HRESULT STD W_SetScissorRect(DEV, const RECT* r)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetScissorRect(d, r); } return g_dx.SetScissorRect(d, r); }
        if (!r) return D3DERR_INVALIDCALL;
        g_m->scissor = *r;
        RecP(kSetScissorRect, 0, r, sizeof(*r));
        Commit();
        return D3D_OK;
    }
    HRESULT STD W_GetScissorRect(DEV, RECT* r)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetScissorRect(d, r); } return g_dx.GetScissorRect(d, r); }
        if (!r) return D3DERR_INVALIDCALL;
        *r = g_m->scissor;
        return D3D_OK;
    }

    HRESULT STD W_SetMaterial(DEV, const D3DMATERIAL9* x)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetMaterial(d, x); } return g_dx.SetMaterial(d, x); }
        if (!x) return D3DERR_INVALIDCALL;
        g_m->mat = *x;
        RecP(kSetMaterial, 0, x, sizeof(*x));
        Commit();
        return D3D_OK;
    }
    HRESULT STD W_GetMaterial(DEV, D3DMATERIAL9* x)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetMaterial(d, x); } return g_dx.GetMaterial(d, x); }
        if (!x) return D3DERR_INVALIDCALL;
        *x = g_m->mat;
        return D3D_OK;
    }

    HRESULT STD W_SetLight(DEV, DWORD i, const D3DLIGHT9* l)
    {
        const Mode m = ModeFor(d);
        if ((m != kRecord && m != kSbRec) || i >= 32) { if (m == kRecord) Drain(); if (m != kDirect) { Gate g; return g_dx.SetLight(d, i, l); } return g_dx.SetLight(d, i, l); }
        if (!l) return D3DERR_INVALIDCALL;
        g_m->lights[i].l = *l;
        g_m->lights[i].set = TRUE;
        Cmd* c = RecP(kSetLight, 1, l, sizeof(*l));
        c->a[0] = i;
        Commit();
        return D3D_OK;
    }
    HRESULT STD W_GetLight(DEV, DWORD i, D3DLIGHT9* l)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord || i >= 32) { if (m == kRecord) Drain(); if (m != kDirect) { Gate g; return g_dx.GetLight(d, i, l); } return g_dx.GetLight(d, i, l); }
        if (!l || !g_m->lights[i].set) return D3DERR_INVALIDCALL;
        *l = g_m->lights[i].l;
        return D3D_OK;
    }
    HRESULT STD W_LightEnable(DEV, DWORD i, BOOL on)
    {
        const Mode m = ModeFor(d);
        if ((m != kRecord && m != kSbRec) || i >= 32) { if (m == kRecord) Drain(); if (m != kDirect) { Gate g; return g_dx.LightEnable(d, i, on); } return g_dx.LightEnable(d, i, on); }
        if (!g_m->lights[i].set)
        {
            // D3D9 makes a default light when an unset one is enabled.
            memset(&g_m->lights[i].l, 0, sizeof(D3DLIGHT9));
            g_m->lights[i].l.Type = D3DLIGHT_DIRECTIONAL;
            g_m->lights[i].l.Diffuse.r = g_m->lights[i].l.Diffuse.g = g_m->lights[i].l.Diffuse.b = 1.0f;
            g_m->lights[i].l.Direction.z = 1.0f;
            g_m->lights[i].set = TRUE;
        }
        g_m->lights[i].on = on ? TRUE : FALSE;
        Rec(kLightEnable, i, on);
        return D3D_OK;
    }
    HRESULT STD W_GetLightEnable(DEV, DWORD i, BOOL* on)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord || i >= 32) { if (m == kRecord) Drain(); if (m != kDirect) { Gate g; return g_dx.GetLightEnable(d, i, on); } return g_dx.GetLightEnable(d, i, on); }
        if (!on || !g_m->lights[i].set) return D3DERR_INVALIDCALL;
        *on = g_m->lights[i].on ? 128 : 0;   // DXVK and D3D9 return 128 for an enabled light
        return D3D_OK;
    }

    HRESULT STD W_SetClipPlane(DEV, DWORD i, const float* p)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetClipPlane(d, i, p); } return g_dx.SetClipPlane(d, i, p); }
        if (!p || i >= 6) return D3DERR_INVALIDCALL;
        memcpy(g_m->clip[i], p, 16);
        Cmd* c = RecP(kSetClipPlane, 1, p, 16);
        c->a[0] = i;
        Commit();
        return D3D_OK;
    }
    HRESULT STD W_GetClipPlane(DEV, DWORD i, float* p)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetClipPlane(d, i, p); } return g_dx.GetClipPlane(d, i, p); }
        if (!p || i >= 6) return D3DERR_INVALIDCALL;
        memcpy(p, g_m->clip[i], 16);
        return D3D_OK;
    }

    // Shader constants.
#define CONSTS(Name, OpId, T, field, limit, width)                                                    \
    HRESULT STD W_Set##Name(DEV, UINT r, const T* p, UINT n)                                          \
    {                                                                                                 \
        const Mode m = ModeFor(d);                                                                    \
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.Set##Name(d, r, p, n); } return g_dx.Set##Name(d, r, p, n); } \
        if (!p || r + n > (limit)) { Drain(); Gate g; return g_dx.Set##Name(d, r, p, n); }             \
        memcpy(&g_m->field[r], p, n * (width) * sizeof(T));                                            \
        Cmd* c = RecP(OpId, 2, p, n * (width) * sizeof(T));                                            \
        c->a[0] = r;                                                                                  \
        c->a[1] = n;                                                                                  \
        Commit();                                                                                     \
        return D3D_OK;                                                                                \
    }                                                                                                 \
    HRESULT STD W_Get##Name(DEV, UINT r, T* p, UINT n)                                                \
    {                                                                                                 \
        const Mode m = ModeFor(d);                                                                    \
        if (m != kRecord || !p || r + n > (limit)) { if (m == kRecord) Drain(); if (m != kDirect) { Gate g; return g_dx.Get##Name(d, r, p, n); } return g_dx.Get##Name(d, r, p, n); } \
        memcpy(p, &g_m->field[r], n * (width) * sizeof(T));                                            \
        return D3D_OK;                                                                                \
    }

    CONSTS(VertexShaderConstantF, kSetVSConstF, float, vsF, 256u, 4)
    CONSTS(VertexShaderConstantI, kSetVSConstI, int, vsI, 16u, 4)
    CONSTS(VertexShaderConstantB, kSetVSConstB, BOOL, vsB, 16u, 1)
    CONSTS(PixelShaderConstantF, kSetPSConstF, float, psF, 224u, 4)
    CONSTS(PixelShaderConstantI, kSetPSConstI, int, psI, 16u, 4)
    CONSTS(PixelShaderConstantB, kSetPSConstB, BOOL, psB, 16u, 1)

    HRESULT STD W_SetStreamSource(DEV, UINT i, IDirect3DVertexBuffer9* vb, UINT off, UINT stride)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetStreamSource(d, i, vb, off, stride); } return g_dx.SetStreamSource(d, i, vb, off, stride); }
        if (i >= 16) return D3DERR_INVALIDCALL;
        g_m->st[i].vb = vb;
        g_m->st[i].off = off;
        g_m->st[i].stride = stride;
        Rec(kSetStreamSource, i, P(vb), off, stride);
        return D3D_OK;
    }
    HRESULT STD W_GetStreamSource(DEV, UINT i, IDirect3DVertexBuffer9** vb, UINT* off, UINT* stride)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetStreamSource(d, i, vb, off, stride); } return g_dx.GetStreamSource(d, i, vb, off, stride); }
        if (i >= 16 || !vb || !off || !stride) return D3DERR_INVALIDCALL;
        *vb = g_m->st[i].vb;
        if (*vb) (*vb)->lpVtbl->AddRef(*vb);
        *off = g_m->st[i].off;
        *stride = g_m->st[i].stride;
        return D3D_OK;
    }
    HRESULT STD W_SetStreamSourceFreq(DEV, UINT i, UINT f)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetStreamSourceFreq(d, i, f); } return g_dx.SetStreamSourceFreq(d, i, f); }
        if (i >= 16) return D3DERR_INVALIDCALL;
        g_m->st[i].freq = f;
        Rec(kSetStreamSourceFreq, i, f);
        return D3D_OK;
    }
    HRESULT STD W_GetStreamSourceFreq(DEV, UINT i, UINT* f)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetStreamSourceFreq(d, i, f); } return g_dx.GetStreamSourceFreq(d, i, f); }
        if (i >= 16 || !f) return D3DERR_INVALIDCALL;
        *f = g_m->st[i].freq;
        return D3D_OK;
    }

    HRESULT STD W_SetIndices(DEV, IDirect3DIndexBuffer9* ib)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetIndices(d, ib); } return g_dx.SetIndices(d, ib); }
        g_m->ib = ib;
        Rec(kSetIndices, P(ib));
        return D3D_OK;
    }
    HRESULT STD W_GetIndices(DEV, IDirect3DIndexBuffer9** ib)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetIndices(d, ib); } return g_dx.GetIndices(d, ib); }
        if (!ib) return D3DERR_INVALIDCALL;
        *ib = g_m->ib;
        if (*ib) (*ib)->lpVtbl->AddRef(*ib);
        return D3D_OK;
    }

    // The FVF DXVK reports for a declaration (2026-10-21): DXVK's GetFVF gives the FVF that matches the bound
    // declaration, and WoW binds declarations. The mirror gave 0 at first, and comfyatmos took no draw for water
    // (it looks for FVF 0x212): with the stream on, the game's own water showed. Asked of DXVK once for each
    // declaration, under the gate: bound for a moment and put back.
    DWORD DeclFvf(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* v)
    {
        if (!v)
            return 0;
        auto it = g_declFvf.find(v);
        if (it != g_declFvf.end())
            return it->second;
        DWORD fvf = 0;
        {
            Gate g;
            IDirect3DVertexDeclaration9* saved = nullptr;
            g_dx.GetVertexDeclaration(d, &saved);
            g_dx.SetVertexDeclaration(d, v);
            g_dx.GetFVF(d, &fvf);
            g_dx.SetVertexDeclaration(d, saved);
            if (saved)
                saved->lpVtbl->Release(saved);
        }
        g_declFvf[v] = fvf;
        return fvf;
    }

    HRESULT STD W_SetVertexDeclaration(DEV, IDirect3DVertexDeclaration9* v)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetVertexDeclaration(d, v); } return g_dx.SetVertexDeclaration(d, v); }
        g_m->decl = v;
        g_m->fvf = g_active ? DeclFvf(d, v) : 0;
        Rec(kSetVertexDeclaration, P(v));
        return D3D_OK;
    }
    HRESULT STD W_GetVertexDeclaration(DEV, IDirect3DVertexDeclaration9** v)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetVertexDeclaration(d, v); } return g_dx.GetVertexDeclaration(d, v); }
        if (!v) return D3DERR_INVALIDCALL;
        *v = g_m->decl;
        if (*v) (*v)->lpVtbl->AddRef(*v);
        return D3D_OK;
    }
    HRESULT STD W_SetFVF(DEV, DWORD f)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetFVF(d, f); } return g_dx.SetFVF(d, f); }
        g_m->fvf = f;
        g_m->decl = nullptr;
        Rec(kSetFVF, f);
        return D3D_OK;
    }
    HRESULT STD W_GetFVF(DEV, DWORD* f)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetFVF(d, f); } return g_dx.GetFVF(d, f); }
        if (!f) return D3DERR_INVALIDCALL;
        *f = g_m->fvf;
        return D3D_OK;
    }

    HRESULT STD W_SetVertexShader(DEV, IDirect3DVertexShader9* s)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetVertexShader(d, s); } return g_dx.SetVertexShader(d, s); }
        g_m->vs = s;
        Rec(kSetVertexShader, P(s));
        return D3D_OK;
    }
    HRESULT STD W_GetVertexShader(DEV, IDirect3DVertexShader9** s)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetVertexShader(d, s); } return g_dx.GetVertexShader(d, s); }
        if (!s) return D3DERR_INVALIDCALL;
        *s = g_m->vs;
        if (*s) (*s)->lpVtbl->AddRef(*s);
        return D3D_OK;
    }
    HRESULT STD W_SetPixelShader(DEV, IDirect3DPixelShader9* s)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetPixelShader(d, s); } return g_dx.SetPixelShader(d, s); }
        g_m->ps = s;
        Rec(kSetPixelShader, P(s));
        return D3D_OK;
    }
    HRESULT STD W_GetPixelShader(DEV, IDirect3DPixelShader9** s)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetPixelShader(d, s); } return g_dx.GetPixelShader(d, s); }
        if (!s) return D3DERR_INVALIDCALL;
        *s = g_m->ps;
        if (*s) (*s)->lpVtbl->AddRef(*s);
        return D3D_OK;
    }

    HRESULT STD W_SetRenderTarget(DEV, DWORD i, IDirect3DSurface9* s)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.SetRenderTarget(d, i, s); } return g_dx.SetRenderTarget(d, i, s); }
        if (i >= 4 || (i == 0 && !s)) return D3DERR_INVALIDCALL;
        g_m->rt[i] = s;
        if (i == 0)
        {
            // D3D9 sets the viewport and the scissor rect to the new target's size.
            D3DSURFACE_DESC desc = {};
            g_surf.GetDesc(s, &desc);
            g_m->vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
            g_m->scissor = { 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
        }
        Rec(kSetRenderTarget, i, P(s));
        return D3D_OK;
    }
    HRESULT STD W_GetRenderTarget(DEV, DWORD i, IDirect3DSurface9** s)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetRenderTarget(d, i, s); } return g_dx.GetRenderTarget(d, i, s); }
        if (i >= 4 || !s) return D3DERR_INVALIDCALL;
        *s = g_m->rt[i];
        if (!*s) return D3DERR_NOTFOUND;
        (*s)->lpVtbl->AddRef(*s);
        return D3D_OK;
    }
    HRESULT STD W_SetDepthStencilSurface(DEV, IDirect3DSurface9* s)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.SetDepthStencilSurface(d, s); } return g_dx.SetDepthStencilSurface(d, s); }
        g_m->ds = s;
        Rec(kSetDepthStencilSurface, P(s));
        return D3D_OK;
    }
    HRESULT STD W_GetDepthStencilSurface(DEV, IDirect3DSurface9** s)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetDepthStencilSurface(d, s); } return g_dx.GetDepthStencilSurface(d, s); }
        if (!s) return D3DERR_INVALIDCALL;
        *s = g_m->ds;
        if (!*s) return D3DERR_NOTFOUND;
        (*s)->lpVtbl->AddRef(*s);
        return D3D_OK;
    }

    HRESULT STD W_SetNPatchMode(DEV, float n)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetNPatchMode(d, n); } return g_dx.SetNPatchMode(d, n); }
        g_m->npatch = n;
        Rec(kSetNPatchMode, F(n));
        return D3D_OK;
    }
    float STD W_GetNPatchMode(DEV)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetNPatchMode(d); } return g_dx.GetNPatchMode(d); }
        return g_m->npatch;
    }
    HRESULT STD W_SetSoftwareVertexProcessing(DEV, BOOL b)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetSoftwareVertexProcessing(d, b); } return g_dx.SetSoftwareVertexProcessing(d, b); }
        g_m->swvp = b;
        Rec(kSetSoftwareVP, b);
        return D3D_OK;
    }
    BOOL STD W_GetSoftwareVertexProcessing(DEV)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetSoftwareVertexProcessing(d); } return g_dx.GetSoftwareVertexProcessing(d); }
        return g_m->swvp;
    }
    HRESULT STD W_SetCurrentTexturePalette(DEV, UINT p)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetCurrentTexturePalette(d, p); } return g_dx.SetCurrentTexturePalette(d, p); }
        g_m->palette = p;
        Rec(kSetCurrentTexturePalette, p);
        return D3D_OK;
    }
    HRESULT STD W_GetCurrentTexturePalette(DEV, UINT* p)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetCurrentTexturePalette(d, p); } return g_dx.GetCurrentTexturePalette(d, p); }
        if (!p) return D3DERR_INVALIDCALL;
        *p = g_m->palette;
        return D3D_OK;
    }
    HRESULT STD W_SetClipStatus(DEV, const D3DCLIPSTATUS9* s)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord && m != kSbRec) { if (m != kDirect) { Gate g; return g_dx.SetClipStatus(d, s); } return g_dx.SetClipStatus(d, s); }
        if (!s) return D3DERR_INVALIDCALL;
        RecP(kSetClipStatus, 0, s, sizeof(*s));
        Commit();
        return D3D_OK;
    }

    // ---- scene and draws

    HRESULT STD W_BeginScene(DEV)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.BeginScene(d); } return g_dx.BeginScene(d); }
        Rec(kBeginScene);
        return D3D_OK;
    }
    HRESULT STD W_EndScene(DEV)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.EndScene(d); } return g_dx.EndScene(d); }
        Rec(kEndScene);
        return D3D_OK;
    }
    HRESULT STD W_Clear(DEV, DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR col, float z, DWORD st)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.Clear(d, n, r, flags, col, z, st); } return g_dx.Clear(d, n, r, flags, col, z, st); }
        const DWORD count = r ? n : 0;
        Cmd* c = RecP(kClear, 5, r, count * sizeof(D3DRECT));
        c->a[0] = count;
        c->a[1] = flags;
        c->a[2] = col;
        c->a[3] = F(z);
        c->a[4] = st;
        Commit();
        return D3D_OK;
    }
    HRESULT STD W_DrawPrimitive(DEV, D3DPRIMITIVETYPE p, UINT start, UINT n)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.DrawPrimitive(d, p, start, n); } return g_dx.DrawPrimitive(d, p, start, n); }
        Rec(kDrawPrimitive, p, start, n);
        return D3D_OK;
    }
    HRESULT STD W_DrawIndexedPrimitive(DEV, D3DPRIMITIVETYPE p, INT base, UINT minI, UINT nv, UINT start, UINT n)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.DrawIndexedPrimitive(d, p, base, minI, nv, start, n); } return g_dx.DrawIndexedPrimitive(d, p, base, minI, nv, start, n); }
        Rec(kDrawIndexedPrimitive, p, base, minI, nv, start, n);
        return D3D_OK;
    }
    HRESULT STD W_DrawPrimitiveUP(DEV, D3DPRIMITIVETYPE p, UINT n, const void* data, UINT stride)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.DrawPrimitiveUP(d, p, n, data, stride); } return g_dx.DrawPrimitiveUP(d, p, n, data, stride); }
        const size_t bytes = static_cast<size_t>(VertsForPrims(p, n)) * stride;
        if (!data || !bytes) return D3DERR_INVALIDCALL;
        Cmd* c;
        if (bytes <= 256 * 1024)
        {
            c = RecP(kDrawPrimitiveUP, 4, data, bytes);
            c->a[3] = 0;
        }
        else
        {
            uint8_t* b = AllocBlock(bytes);
            if (!b) { Drain(); Gate g; return g_dx.DrawPrimitiveUP(d, p, n, data, stride); }
            memcpy(b, data, bytes);
            c = RecP(kDrawPrimitiveUP, 4, nullptr, 0);
            c->a[3] = P(b);
        }
        c->a[0] = p;
        c->a[1] = n;
        c->a[2] = stride;
        Commit();
        // D3D9 unbinds stream 0 after an UP draw.
        g_m->st[0].vb = nullptr;
        g_m->st[0].off = 0;
        g_m->st[0].stride = 0;
        return D3D_OK;
    }
    HRESULT STD W_DrawIndexedPrimitiveUP(DEV, D3DPRIMITIVETYPE p, UINT minI, UINT nv, UINT n, const void* idx,
                                         D3DFORMAT fmt, const void* data, UINT stride)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.DrawIndexedPrimitiveUP(d, p, minI, nv, n, idx, fmt, data, stride); } return g_dx.DrawIndexedPrimitiveUP(d, p, minI, nv, n, idx, fmt, data, stride); }
        const size_t ib = static_cast<size_t>(VertsForPrims(p, n)) * (fmt == D3DFMT_INDEX32 ? 4 : 2);
        const size_t vbBytes = static_cast<size_t>(minI + nv) * stride;
        if (!idx || !data || !ib || !vbBytes) return D3DERR_INVALIDCALL;
        const size_t ibAligned = (ib + 3) & ~size_t(3);
        Cmd* c;
        if (ibAligned + vbBytes <= 256 * 1024)
        {
            c = Reserve(kDrawIndexedPrimitiveUP, 8, ibAligned + vbBytes);
            memcpy(Payload(c), idx, ib);
            memcpy(Payload(c) + ibAligned, data, vbBytes);
            c->a[7] = 0;
        }
        else
        {
            uint8_t* b = AllocBlock(ibAligned + vbBytes);
            if (!b) { Drain(); Gate g; return g_dx.DrawIndexedPrimitiveUP(d, p, minI, nv, n, idx, fmt, data, stride); }
            memcpy(b, idx, ib);
            memcpy(b + ibAligned, data, vbBytes);
            c = Reserve(kDrawIndexedPrimitiveUP, 8, 0);
            c->a[7] = P(b);
        }
        ++g_opCount[kDrawIndexedPrimitiveUP];
        c->a[0] = p;
        c->a[1] = minI;
        c->a[2] = nv;
        c->a[3] = n;
        c->a[4] = fmt;
        c->a[5] = stride;
        c->a[6] = static_cast<uint32_t>(ibAligned);
        Commit();
        g_m->st[0].vb = nullptr;
        g_m->st[0].off = 0;
        g_m->st[0].stride = 0;
        g_m->ib = nullptr;
        return D3D_OK;
    }

    // ---- copies between resources

    HRESULT STD W_StretchRect(DEV, IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* t, const RECT* tr, D3DTEXTUREFILTERTYPE f)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.StretchRect(d, s, sr, t, tr, f); } return g_dx.StretchRect(d, s, sr, t, tr, f); }
        RECT rr[2] = {};
        if (sr) rr[0] = *sr;
        if (tr) rr[1] = *tr;
        Cmd* c = RecP(kStretchRect, 5, rr, sizeof(rr));
        c->a[0] = P(s);
        c->a[1] = P(t);
        c->a[2] = f;
        c->a[3] = sr ? 1 : 0;
        c->a[4] = tr ? 1 : 0;
        Commit();
        return D3D_OK;
    }
    HRESULT STD W_ColorFill(DEV, IDirect3DSurface9* s, const RECT* r, D3DCOLOR col)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.ColorFill(d, s, r, col); } return g_dx.ColorFill(d, s, r, col); }
        RECT rr = {};
        if (r) rr = *r;
        Cmd* c = RecP(kColorFill, 3, &rr, sizeof(rr));
        c->a[0] = P(s);
        c->a[1] = col;
        c->a[2] = r ? 1 : 0;
        Commit();
        return D3D_OK;
    }
    HRESULT STD W_UpdateSurface(DEV, IDirect3DSurface9* s, const RECT* r, IDirect3DSurface9* t, const POINT* pt)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.UpdateSurface(d, s, r, t, pt); } return g_dx.UpdateSurface(d, s, r, t, pt); }
        struct { RECT r; POINT p; } x = {};
        if (r) x.r = *r;
        if (pt) x.p = *pt;
        Cmd* c = RecP(kUpdateSurface, 4, &x, sizeof(x));
        c->a[0] = P(s);
        c->a[1] = P(t);
        c->a[2] = r ? 1 : 0;
        c->a[3] = pt ? 1 : 0;
        Commit();
        return D3D_OK;
    }
    HRESULT STD W_UpdateTexture(DEV, IDirect3DBaseTexture9* s, IDirect3DBaseTexture9* t)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.UpdateTexture(d, s, t); } return g_dx.UpdateTexture(d, s, t); }
        Rec(kUpdateTexture, P(s), P(t));
        return D3D_OK;
    }
    HRESULT STD W_GetRenderTargetData(DEV, IDirect3DSurface9* rt, IDirect3DSurface9* sys)
    {
        const Mode m = ModeFor(d);
        if (m != kRecord) { if (m != kDirect) { Gate g; return g_dx.GetRenderTargetData(d, rt, sys); } return g_dx.GetRenderTargetData(d, rt, sys); }
        Rec(kGetRenderTargetData, P(rt), P(sys));
        return D3D_OK;
    }

    // ---- state blocks
    //
    // Two kinds of our own. Of type ALL (comfyatmos's): Capture copies the mirror, Apply records the states that
    // differ. comfyatmos captures and applies within one call of a hook, so the objects in the copy cannot be
    // released in between and the copy holds no references. Recorded between BeginStateBlock and EndStateBlock
    // (the game's): a list of Set commands, in the queue's format; Apply runs them through our wrappers, Capture
    // reads their values from the mirror again. The game records its state blocks before the stream starts, as
    // DXVK's own objects: their commands are kept beside them (g_sbLists) and used from then on.

    IDirect3DDevice9Vtbl g_wt;   // our wrappers, for a state block's Apply
    std::unordered_map<void*, std::vector<uint8_t>> g_sbLists;   // DXVK's state blocks recorded before the stream

    void ApplyList(const std::vector<uint8_t>& list, IDirect3DDevice9* d)
    {
        for (size_t off = 0; off < list.size();)
        {
            const Cmd* c = reinterpret_cast<const Cmd*>(list.data() + off);
            ExecuteDev(c, g_wt, d);
            off += c->bytes;
        }
    }

    void CaptureList(std::vector<uint8_t>& list)
    {
        const Mirror& m = *g_m;
        for (size_t off = 0; off < list.size();)
        {
            Cmd* c = reinterpret_cast<Cmd*>(list.data() + off);
            uint32_t* a = c->a;
            uint8_t* pl = Payload(c);
            switch (c->op)
            {
            case kSetRenderState:       if (a[0] < 256) a[1] = m.rs[a[0]]; break;
            case kSetSamplerState:      { const int i = SamplerIndex(a[0]); if (i >= 0 && a[1] < 14) a[2] = m.ss[i][a[1]]; break; }
            case kSetTextureStageState: if (a[0] < 8 && a[1] < 33) a[2] = m.tss[a[0]][a[1]]; break;
            case kSetTexture:           { const int i = SamplerIndex(a[0]); if (i >= 0) a[1] = P(m.tex[i]); break; }
            case kSetTransform:         if (a[0] < 512) memcpy(pl, &m.xf[a[0]], sizeof(D3DMATRIX)); break;
            case kSetViewport:          memcpy(pl, &m.vp, sizeof(m.vp)); break;
            case kSetScissorRect:       memcpy(pl, &m.scissor, sizeof(m.scissor)); break;
            case kSetMaterial:          memcpy(pl, &m.mat, sizeof(m.mat)); break;
            case kSetLight:             if (a[0] < 32) memcpy(pl, &m.lights[a[0]].l, sizeof(D3DLIGHT9)); break;
            case kLightEnable:          if (a[0] < 32) a[1] = m.lights[a[0]].on; break;
            case kSetClipPlane:         if (a[0] < 6) memcpy(pl, m.clip[a[0]], 16); break;
            case kSetVSConstF:          memcpy(pl, &m.vsF[a[0]], a[1] * 16); break;
            case kSetVSConstI:          memcpy(pl, &m.vsI[a[0]], a[1] * 16); break;
            case kSetVSConstB:          memcpy(pl, &m.vsB[a[0]], a[1] * 4); break;
            case kSetPSConstF:          memcpy(pl, &m.psF[a[0]], a[1] * 16); break;
            case kSetPSConstI:          memcpy(pl, &m.psI[a[0]], a[1] * 16); break;
            case kSetPSConstB:          memcpy(pl, &m.psB[a[0]], a[1] * 4); break;
            case kSetStreamSource:      if (a[0] < 16) { a[1] = P(m.st[a[0]].vb); a[2] = m.st[a[0]].off; a[3] = m.st[a[0]].stride; } break;
            case kSetStreamSourceFreq:  if (a[0] < 16) a[1] = m.st[a[0]].freq; break;
            case kSetIndices:           a[0] = P(m.ib); break;
            case kSetVertexDeclaration: a[0] = P(m.decl); break;
            case kSetFVF:               a[0] = m.fvf; break;
            case kSetVertexShader:      a[0] = P(m.vs); break;
            case kSetPixelShader:       a[0] = P(m.ps); break;
            case kSetNPatchMode:        a[0] = F(m.npatch); break;
            case kSetSoftwareVP:        a[0] = m.swvp; break;
            case kSetCurrentTexturePalette: a[0] = m.palette; break;
            default: break;
            }
            off += c->bytes;
        }
    }

    struct VSB;
    struct VSBVtbl
    {
        HRESULT (STD* QueryInterface)(VSB*, REFIID, void**);
        ULONG   (STD* AddRef)(VSB*);
        ULONG   (STD* Release)(VSB*);
        HRESULT (STD* GetDevice)(VSB*, IDirect3DDevice9**);
        HRESULT (STD* Capture)(VSB*);
        HRESULT (STD* Apply)(VSB*);
    };
    struct VSB
    {
        const VSBVtbl* lpVtbl;
        LONG refs;
        IDirect3DDevice9* dev;
        Mirror* m;                     // type ALL: the copy
        std::vector<uint8_t> list;     // recorded: the commands
    };

    HRESULT STD V_QueryInterface(VSB*, REFIID, void** p) { if (p) *p = nullptr; return E_NOINTERFACE; }
    ULONG   STD V_AddRef(VSB* s) { return static_cast<ULONG>(InterlockedIncrement(&s->refs)); }
    ULONG   STD V_Release(VSB* s)
    {
        const LONG n = InterlockedDecrement(&s->refs);
        if (!n)
        {
            delete s->m;
            delete s;
        }
        return static_cast<ULONG>(n);
    }
    HRESULT STD V_GetDevice(VSB* s, IDirect3DDevice9** d) { if (!d) return D3DERR_INVALIDCALL; *d = s->dev; s->dev->lpVtbl->AddRef(s->dev); return D3D_OK; }
    HRESULT STD V_Capture(VSB* s)
    {
        if (s->m)
            *s->m = *g_m;
        else
            CaptureList(s->list);
        return D3D_OK;
    }
    HRESULT STD V_Apply(VSB* s)
    {
        IDirect3DDevice9* d = s->dev;
        if (!s->m)
        {
            ApplyList(s->list, d);
            return D3D_OK;
        }
        const Mirror& a = *s->m;
        Mirror& m = *g_m;
        for (DWORD i = 0; i < 256; ++i)
            if (a.rs[i] != m.rs[i]) W_SetRenderState(d, static_cast<D3DRENDERSTATETYPE>(i), a.rs[i]);
        for (int si = 0; si < kSamplers; ++si)
        {
            for (DWORD t = 1; t < 14; ++t)
                if (a.ss[si][t] != m.ss[si][t]) W_SetSamplerState(d, SamplerOf(si), static_cast<D3DSAMPLERSTATETYPE>(t), a.ss[si][t]);
            if (a.tex[si] != m.tex[si]) W_SetTexture(d, SamplerOf(si), a.tex[si]);
        }
        for (DWORD st = 0; st < 8; ++st)
            for (DWORD t = 1; t < 33; ++t)
                if (a.tss[st][t] != m.tss[st][t]) W_SetTextureStageState(d, st, static_cast<D3DTEXTURESTAGESTATETYPE>(t), a.tss[st][t]);
        for (DWORD i = 0; i < 512; ++i)
            if (memcmp(&a.xf[i], &m.xf[i], sizeof(D3DMATRIX)) != 0) W_SetTransform(d, static_cast<D3DTRANSFORMSTATETYPE>(i), &a.xf[i]);
        if (memcmp(&a.vp, &m.vp, sizeof(a.vp)) != 0) W_SetViewport(d, &a.vp);
        if (memcmp(&a.scissor, &m.scissor, sizeof(a.scissor)) != 0) W_SetScissorRect(d, &a.scissor);
        if (memcmp(&a.mat, &m.mat, sizeof(a.mat)) != 0) W_SetMaterial(d, &a.mat);
        for (DWORD i = 0; i < 6; ++i)
            if (memcmp(a.clip[i], m.clip[i], 16) != 0) W_SetClipPlane(d, i, a.clip[i]);
        for (DWORD i = 0; i < 32; ++i)
        {
            if (!a.lights[i].set) continue;
            if (!m.lights[i].set || memcmp(&a.lights[i].l, &m.lights[i].l, sizeof(D3DLIGHT9)) != 0) W_SetLight(d, i, &a.lights[i].l);
            if (a.lights[i].on != m.lights[i].on) W_LightEnable(d, i, a.lights[i].on);
        }
        // Constants: runs of registers that differ, each one call.
        auto runs = [&](const float (*from)[4], const float (*cur)[4], UINT count, auto set) {
            UINT i = 0;
            while (i < count)
            {
                if (memcmp(from[i], cur[i], 16) == 0) { ++i; continue; }
                UINT j = i + 1;
                while (j < count && memcmp(from[j], cur[j], 16) != 0) ++j;
                set(i, &from[i][0], j - i);
                i = j;
            }
        };
        runs(a.vsF, m.vsF, 256, [&](UINT r, const float* p, UINT n) { W_SetVertexShaderConstantF(d, r, p, n); });
        runs(a.psF, m.psF, 224, [&](UINT r, const float* p, UINT n) { W_SetPixelShaderConstantF(d, r, p, n); });
        if (memcmp(a.vsI, m.vsI, sizeof(a.vsI)) != 0) W_SetVertexShaderConstantI(d, 0, &a.vsI[0][0], 16);
        if (memcmp(a.vsB, m.vsB, sizeof(a.vsB)) != 0) W_SetVertexShaderConstantB(d, 0, a.vsB, 16);
        if (memcmp(a.psI, m.psI, sizeof(a.psI)) != 0) W_SetPixelShaderConstantI(d, 0, &a.psI[0][0], 16);
        if (memcmp(a.psB, m.psB, sizeof(a.psB)) != 0) W_SetPixelShaderConstantB(d, 0, a.psB, 16);
        for (UINT i = 0; i < 16; ++i)
        {
            if (a.st[i].vb != m.st[i].vb || a.st[i].off != m.st[i].off || a.st[i].stride != m.st[i].stride)
                W_SetStreamSource(d, i, a.st[i].vb, a.st[i].off, a.st[i].stride);
            if (a.st[i].freq != m.st[i].freq) W_SetStreamSourceFreq(d, i, a.st[i].freq);
        }
        if (a.ib != m.ib) W_SetIndices(d, a.ib);
        // A declaration as it was bound; an FVF only where SetFVF bound it (the mirror's decl is null then).
        if (a.decl)
        {
            if (a.decl != m.decl) W_SetVertexDeclaration(d, a.decl);
        }
        else if (a.fvf != m.fvf || m.decl)
            W_SetFVF(d, a.fvf);
        if (a.vs != m.vs) W_SetVertexShader(d, a.vs);
        if (a.ps != m.ps) W_SetPixelShader(d, a.ps);
        if (a.npatch != m.npatch) W_SetNPatchMode(d, a.npatch);
        return D3D_OK;
    }
    const VSBVtbl g_vsbVtbl = { V_QueryInterface, V_AddRef, V_Release, V_GetDevice, V_Capture, V_Apply };

    HRESULT STD W_CreateStateBlock(DEV, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** out)
    {
        const Mode m = ModeFor(d);
        if (m == kRecord && t == D3DSBT_ALL && out)
        {
            VSB* s = new VSB{ &g_vsbVtbl, 1, d, new Mirror(*g_m), {} };
            *out = reinterpret_cast<IDirect3DStateBlock9*>(s);
            return D3D_OK;
        }
        if (m == kDirect) return g_dx.CreateStateBlock(d, t, out);
        if (m == kRecord) Drain();
        Gate g;
        return g_dx.CreateStateBlock(d, t, out);
    }

    Mirror* g_sbSave = nullptr;   // the mirror at BeginStateBlock: recording changes no state

    HRESULT STD W_BeginStateBlock(DEV)
    {
        if (Tid() == g_replayTid || g_sbRec)
            return g_dx.BeginStateBlock(d);
        if (g_active && d == g_dev && Tid() != g_gameTid)
        {
            Gate g;
            return g_dx.BeginStateBlock(d);
        }
        if (!g_active)
        {
            const HRESULT hr = g_dx.BeginStateBlock(d);
            if (FAILED(hr))
                return hr;
        }
        if (!g_sbSave)
            g_sbSave = new Mirror;
        *g_sbSave = *g_m;
        g_sbBuf.clear();
        g_sbRecDev = d;
        g_sbRecTid = Tid();
        g_sbRec = true;
        return D3D_OK;
    }
    HRESULT STD W_EndStateBlock(DEV, IDirect3DStateBlock9** out)
    {
        if (!g_sbRec || Tid() != g_sbRecTid || d != g_sbRecDev)
        {
            if (g_active && d == g_dev && Tid() != g_replayTid) { Gate g; return g_dx.EndStateBlock(d, out); }
            return g_dx.EndStateBlock(d, out);
        }
        g_sbRec = false;
        *g_m = *g_sbSave;
        if (!out)
            return D3DERR_INVALIDCALL;
        if (!g_active)
        {
            const HRESULT hr = g_dx.EndStateBlock(d, out);
            if (SUCCEEDED(hr) && *out)
                g_sbLists[*out] = g_sbBuf;
            Log("stream: a state block recorded before the stream (%u bytes of commands), kept", static_cast<unsigned>(g_sbBuf.size()));
            return hr;
        }
        VSB* s = new VSB{ &g_vsbVtbl, 1, d, nullptr, g_sbBuf };
        *out = reinterpret_cast<IDirect3DStateBlock9*>(s);
        return D3D_OK;
    }
    // DXVK's own state blocks: one recorded before the stream applies from its kept commands. Any other waits for
    // the queue, applies, and reads the mirror again.
    HRESULT STD W_SbRealApply(IDirect3DStateBlock9* s)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return g_sbReal.Apply(s);
        if (m == kRecord)
        {
            auto it = g_sbLists.find(s);
            if (it != g_sbLists.end())
            {
                ApplyList(it->second, g_dev);
                return D3D_OK;
            }
            Drain();
            ++g_sbApplies;
        }
        Gate g;
        const HRESULT hr = g_sbReal.Apply(s);
        if (m == kRecord) LoadMirror(g_dev);
        return hr;
    }
    HRESULT STD W_SbRealCapture(IDirect3DStateBlock9* s)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return g_sbReal.Capture(s);
        if (m == kRecord)
        {
            auto it = g_sbLists.find(s);
            if (it != g_sbLists.end())
            {
                CaptureList(it->second);
                return D3D_OK;
            }
            Drain();
        }
        Gate g;
        return g_sbReal.Capture(s);
    }

    // ------------------------------------------------------------------------------------------------ resources: locks

    // Bytes in a row of 4x4 blocks or of pixels, and the rows, for a format; 0 when not known.
    bool FormatLayout(D3DFORMAT f, UINT w, UINT h, UINT& rowBytes, UINT& rows)
    {
        UINT bpp = 0, block = 0;
        switch (f)
        {
        case D3DFMT_DXT1: block = 8; break;
        case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: block = 16; break;
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
        case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_R32F: case D3DFMT_G16R16F:
        case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32: case D3DFMT_INDEX32: bpp = 4; break;
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        case D3DFMT_A8L8: case D3DFMT_L16: case D3DFMT_V8U8: case D3DFMT_R16F: case D3DFMT_D16: case D3DFMT_INDEX16: bpp = 2; break;
        case D3DFMT_L8: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_A4L4: bpp = 1; break;
        case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: bpp = 8; break;
        case D3DFMT_A32B32G32R32F: bpp = 16; break;
        default: return false;
        }
        if (block)
        {
            rowBytes = ((w + 3) / 4) * block;
            rows = (h + 3) / 4;
        }
        else
        {
            rowBytes = w * bpp;
            rows = h;
        }
        return true;
    }

    // ---- buffers

    struct BufInfo
    {
        UINT     size = 0;
        DWORD    usage = 0;
        uint8_t* shadow = nullptr;   // DYNAMIC buffers: the whole buffer as the game wrote it
        struct Pending { UINT off, size; DWORD flags; uint8_t* block; bool direct; };
        std::vector<Pending> locks;
    };
    std::unordered_map<void*, BufInfo> g_bufs;

    // Checked at every lock: DXVK gives a freed buffer's address to a new one, so a kept entry can be another
    // buffer's (2026-10-21: a 0.8 MB upload into the block of a smaller buffer at the same address).
    template <class Buf, class Vtbl, class Desc>
    BufInfo& Info(Buf* b, const Vtbl& real)
    {
        BufInfo& i = g_bufs[b];
        Desc desc = {};
        real.GetDesc(b, &desc);
        if (i.size != desc.Size || i.usage != desc.Usage)
        {
            free(i.shadow);
            i.shadow = nullptr;
            i.size = desc.Size;
            i.usage = desc.Usage;
        }
        return i;
    }

    template <class Buf, class Vtbl, class Desc>
    HRESULT LockBuffer(Buf* b, const Vtbl& real, UINT off, UINT size, void** out, DWORD flags)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return real.Lock(b, off, size, out, flags);
        if (m == kGated) { Gate g; return real.Lock(b, off, size, out, flags); }
        if (!out) return D3DERR_INVALIDCALL;
        BufInfo& i = Info<Buf, Vtbl, Desc>(b, real);
        ++g_lockCalls;
        if (flags & D3DLOCK_READONLY)
        {
            static int logged = 0;
            if (++logged <= 20)
                Log("stream: a read-only lock of buffer %p, %u bytes, usage 0x%X", b, i.size, static_cast<unsigned>(i.usage));
            Drain();
            ++g_readbacks;
            HRESULT hr;
            { Gate g; hr = real.Lock(b, off, size, out, flags); }
            if (SUCCEEDED(hr)) i.locks.push_back({ off, size, flags, nullptr, true });
            return hr;
        }
        if (off > i.size) return D3DERR_INVALIDCALL;
        if (!size || off + size > i.size)
            size = i.size - off;
        g_lockBytes += size;
        // Every lock gets a block of its own, sent whole at the unlock: the game writes the range it locks. A dynamic
        // buffer locked whole without DISCARD could be written in part; counted, and the first ones logged, to see
        // whether the game does it (it would need a copy of the whole buffer).
        if (i.usage & D3DUSAGE_DYNAMIC)
        {
            ++g_dynLocks;
            if (!(flags & D3DLOCK_DISCARD) && off == 0 && size == i.size)
            {
                static int logged = 0;
                ++g_dynWhole;
                if (++logged <= 10)
                    Log("stream: a dynamic buffer %p (%u bytes) locked whole with flags 0x%X", b, i.size, static_cast<unsigned>(flags));
            }
        }
        uint8_t* block = AllocBlock(size ? size : 1);
        if (!block) { Drain(); Gate g; return real.Lock(b, off, size, out, flags); }
        *out = block;
        i.locks.push_back({ off, size, flags, block, false });
        return D3D_OK;
    }

    template <class Buf, class Vtbl>
    HRESULT UnlockBuffer(Buf* b, const Vtbl& real, bool isIb)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return real.Unlock(b);
        if (m == kGated) { Gate g; return real.Unlock(b); }
        auto it = g_bufs.find(b);
        if (it == g_bufs.end() || it->second.locks.empty())
        {
            Gate g;
            return real.Unlock(b);
        }
        BufInfo& i = it->second;
        const BufInfo::Pending p = i.locks.back();
        i.locks.pop_back();
        if (p.direct) { Gate g; return real.Unlock(b); }
        uint8_t* block = p.block;
        if (!block)
        {
            block = AllocBlock(p.size ? p.size : 1);
            if (!block) { Drain(); Gate g; void* ptr = nullptr; if (SUCCEEDED(real.Lock(b, p.off, p.size, &ptr, p.flags))) { memcpy(ptr, i.shadow + p.off, p.size); real.Unlock(b); } return D3D_OK; }
            memcpy(block, i.shadow + p.off, p.size);
            g_lockCopied += p.size;
        }
        Cmd* c = Reserve(kBufUpload, 6, 0);
        c->a[0] = P(b);
        c->a[1] = isIb ? 1 : 0;
        c->a[2] = p.off;
        c->a[3] = p.size;
        c->a[4] = p.flags;
        c->a[5] = P(block);
        ++g_opCount[kBufUpload];
        Commit();
        return D3D_OK;
    }

    HRESULT STD W_VbLock(IDirect3DVertexBuffer9* b, UINT off, UINT size, void** out, DWORD flags)
    { return LockBuffer<IDirect3DVertexBuffer9, IDirect3DVertexBuffer9Vtbl, D3DVERTEXBUFFER_DESC>(b, g_vb, off, size, out, flags); }
    HRESULT STD W_VbUnlock(IDirect3DVertexBuffer9* b) { return UnlockBuffer(b, g_vb, false); }
    HRESULT STD W_IbLock(IDirect3DIndexBuffer9* b, UINT off, UINT size, void** out, DWORD flags)
    { return LockBuffer<IDirect3DIndexBuffer9, IDirect3DIndexBuffer9Vtbl, D3DINDEXBUFFER_DESC>(b, g_ib, off, size, out, flags); }
    HRESULT STD W_IbUnlock(IDirect3DIndexBuffer9* b) { return UnlockBuffer(b, g_ib, true); }

    // ---- textures and surfaces

    enum TexKind : uint32_t { kTex2D = 0, kSurface = 1, kCubeFace = 2 };

    struct LevelLock
    {
        RECT     rect;
        UINT     rowBytes, rows;
        DWORD    flags;
        uint8_t* block;
        bool     direct;
    };
    struct LevelKey
    {
        void* obj; uint32_t sub;
        bool operator==(const LevelKey& o) const { return obj == o.obj && sub == o.sub; }
    };
    struct LevelKeyHash { size_t operator()(const LevelKey& k) const { return std::hash<void*>()(k.obj) ^ (k.sub * 0x9E3779B9u); } };
    struct LevelInfo
    {
        bool     seen = false;
        uint8_t* shadow = nullptr;     // the whole level, rows packed, once it has been locked twice
        UINT     rowBytes = 0, rows = 0, width = 0, height = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
        D3DPOOL  pool = D3DPOOL_DEFAULT;
        DWORD    usage = 0;
        std::vector<LevelLock> locks;
    };
    std::unordered_map<LevelKey, LevelInfo, LevelKeyHash> g_levels;

    // A new object at an address an old one had: what was kept for the old one goes.
    void Forget(void* o)
    {
        auto b = g_bufs.find(o);
        if (b != g_bufs.end())
        {
            free(b->second.shadow);
            g_bufs.erase(b);
        }
        for (uint32_t sub = 0; sub < 16; ++sub)
        {
            auto l = g_levels.find({ o, sub });
            if (l != g_levels.end())
            {
                free(l->second.shadow);
                g_levels.erase(l);
            }
        }
    }

    // Real lock and unlock of one level, for the replay thread and for the read back.
    HRESULT RealLockRect(TexKind k, void* o, uint32_t sub, D3DLOCKED_RECT* lr, const RECT* r, DWORD flags)
    {
        switch (k)
        {
        case kTex2D:    return g_tex.LockRect(static_cast<IDirect3DTexture9*>(o), sub, lr, r, flags);
        case kSurface:  return g_surf.LockRect(static_cast<IDirect3DSurface9*>(o), lr, r, flags);
        default:        return g_cube.LockRect(static_cast<IDirect3DCubeTexture9*>(o), static_cast<D3DCUBEMAP_FACES>(sub >> 16), sub & 0xFFFF, lr, r, flags);
        }
    }
    HRESULT RealUnlockRect(TexKind k, void* o, uint32_t sub)
    {
        switch (k)
        {
        case kTex2D:    return g_tex.UnlockRect(static_cast<IDirect3DTexture9*>(o), sub);
        case kSurface:  return g_surf.UnlockRect(static_cast<IDirect3DSurface9*>(o));
        default:        return g_cube.UnlockRect(static_cast<IDirect3DCubeTexture9*>(o), static_cast<D3DCUBEMAP_FACES>(sub >> 16), sub & 0xFFFF);
        }
    }

    HRESULT LockLevel(TexKind k, void* o, uint32_t sub, D3DLOCKED_RECT* lr, const RECT* r, DWORD flags)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return RealLockRect(k, o, sub, lr, r, flags);
        if (m == kGated) { Gate g; return RealLockRect(k, o, sub, lr, r, flags); }
        if (!lr) return D3DERR_INVALIDCALL;
        ++g_texLocks;
        LevelInfo& li = g_levels[{ o, sub }];
        D3DSURFACE_DESC desc = {};
        HRESULT hr;
        switch (k)
        {
        case kTex2D:   hr = g_tex.GetLevelDesc(static_cast<IDirect3DTexture9*>(o), sub, &desc); break;
        case kSurface: hr = g_surf.GetDesc(static_cast<IDirect3DSurface9*>(o), &desc); break;
        default:       hr = g_cube.GetLevelDesc(static_cast<IDirect3DCubeTexture9*>(o), sub & 0xFFFF, &desc); break;
        }
        if (FAILED(hr)) return hr;
        // A kept entry can be another object's at a reused address: checked at every lock.
        if (li.width != desc.Width || li.height != desc.Height || li.format != desc.Format || li.pool != desc.Pool ||
            li.usage != desc.Usage)
        {
            free(li.shadow);
            li.shadow = nullptr;
            li.seen = false;
            li.width = desc.Width;
            li.height = desc.Height;
            li.format = desc.Format;
            li.pool = desc.Pool;
            li.usage = desc.Usage;
            if (!FormatLayout(desc.Format, desc.Width, desc.Height, li.rowBytes, li.rows))
                li.rowBytes = 0;
        }
        // READONLY, a format not known, or memory the game may read (system memory, a render target, the back
        // buffer): wait for the queue and lock DXVK's own. Only managed and dynamic textures are staged.
        const bool staged = li.pool == D3DPOOL_MANAGED || (li.pool == D3DPOOL_DEFAULT && (li.usage & D3DUSAGE_DYNAMIC));
        if ((flags & D3DLOCK_READONLY) || !li.rowBytes || !staged)
        {
            static int logged = 0;
            if (++logged <= 40)
                Log("stream: a direct lock of %s %p level %u: %ux%u format %d pool %d usage 0x%X flags 0x%X", k == kTex2D ? "texture" :
                    k == kSurface ? "surface" : "cube face", o, sub & 0xFFFF, li.width, li.height, static_cast<int>(li.format),
                    static_cast<int>(li.pool), static_cast<unsigned>(li.usage), static_cast<unsigned>(flags));
            Drain();
            ++g_readbacks;
            HRESULT hr;
            { Gate g; hr = RealLockRect(k, o, sub, lr, r, flags); }
            if (SUCCEEDED(hr)) li.locks.push_back({ {}, 0, 0, flags, nullptr, true });
            return hr;
        }
        RECT rc = r ? *r : RECT{ 0, 0, static_cast<LONG>(li.width), static_cast<LONG>(li.height) };
        UINT rowBytes = 0, rows = 0;
        FormatLayout(li.format, rc.right - rc.left, rc.bottom - rc.top, rowBytes, rows);
        const bool block4 = li.format == D3DFMT_DXT1 || li.format == D3DFMT_DXT2 || li.format == D3DFMT_DXT3 ||
                            li.format == D3DFMT_DXT4 || li.format == D3DFMT_DXT5;
        const UINT unit = block4 ? 4 : 1;
        const UINT bytesPerUnit = li.rowBytes / std::max<UINT>((li.width + unit - 1) / unit, 1);
        uint8_t* block = AllocBlock(static_cast<size_t>(rowBytes) * rows);
        if (!block) { Drain(); Gate g; return RealLockRect(k, o, sub, lr, r, flags); }
        // A level locked before, not discarded: its shadow keeps what the game does not write this time.
        const bool discard = (flags & D3DLOCK_DISCARD) != 0;
        if (li.seen && !discard && !li.shadow)
        {
            // Read back once into a shadow.
            ++g_relock;
            Drain();
            ++g_readbacks;
            li.shadow = static_cast<uint8_t*>(malloc(static_cast<size_t>(li.rowBytes) * li.rows));
            if (li.shadow)
            {
                Gate g;
                D3DLOCKED_RECT rl = {};
                if (SUCCEEDED(RealLockRect(k, o, sub, &rl, nullptr, D3DLOCK_READONLY)))
                {
                    for (UINT y = 0; y < li.rows; ++y)
                        memcpy(li.shadow + y * li.rowBytes, static_cast<uint8_t*>(rl.pBits) + y * rl.Pitch, li.rowBytes);
                    RealUnlockRect(k, o, sub);
                }
                else
                    memset(li.shadow, 0, static_cast<size_t>(li.rowBytes) * li.rows);
            }
        }
        if (li.shadow && !discard)
        {
            const UINT x0 = (rc.left / unit) * bytesPerUnit, y0 = rc.top / unit;
            for (UINT y = 0; y < rows; ++y)
                memcpy(block + y * rowBytes, li.shadow + (y0 + y) * li.rowBytes + x0, rowBytes);
        }
        li.seen = true;
        lr->pBits = block;
        lr->Pitch = static_cast<INT>(rowBytes);
        li.locks.push_back({ rc, rowBytes, rows, flags, block, false });
        g_lockBytes += static_cast<uint64_t>(rowBytes) * rows;
        return D3D_OK;
    }

    HRESULT UnlockLevel(TexKind k, void* o, uint32_t sub)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return RealUnlockRect(k, o, sub);
        if (m == kGated) { Gate g; return RealUnlockRect(k, o, sub); }
        auto it = g_levels.find({ o, sub });
        if (it == g_levels.end() || it->second.locks.empty())
        {
            Gate g;
            return RealUnlockRect(k, o, sub);
        }
        LevelInfo& li = it->second;
        const LevelLock l = li.locks.back();
        li.locks.pop_back();
        if (l.direct) { Gate g; return RealUnlockRect(k, o, sub); }
        if (li.shadow)
        {
            const bool block4 = li.format == D3DFMT_DXT1 || li.format == D3DFMT_DXT2 || li.format == D3DFMT_DXT3 ||
                                li.format == D3DFMT_DXT4 || li.format == D3DFMT_DXT5;
            const UINT unit = block4 ? 4 : 1;
            const UINT bytesPerUnit = li.rowBytes / std::max<UINT>((li.width + unit - 1) / unit, 1);
            const UINT x0 = (l.rect.left / unit) * bytesPerUnit, y0 = l.rect.top / unit;
            for (UINT y = 0; y < l.rows; ++y)
                memcpy(li.shadow + (y0 + y) * li.rowBytes + x0, l.block + y * l.rowBytes, l.rowBytes);
        }
        Cmd* c = Reserve(kTexUpload, 11, 0);
        c->a[0] = k;
        c->a[1] = P(o);
        c->a[2] = sub;
        c->a[3] = static_cast<uint32_t>(l.rect.left);
        c->a[4] = static_cast<uint32_t>(l.rect.top);
        c->a[5] = static_cast<uint32_t>(l.rect.right);
        c->a[6] = static_cast<uint32_t>(l.rect.bottom);
        c->a[7] = l.flags;
        c->a[8] = l.rowBytes;
        c->a[9] = P(l.block);
        c->a[10] = l.rows;
        ++g_opCount[kTexUpload];
        Commit();
        return D3D_OK;
    }

    HRESULT STD W_TexLockRect(IDirect3DTexture9* t, UINT l, D3DLOCKED_RECT* lr, const RECT* r, DWORD f) { return LockLevel(kTex2D, t, l, lr, r, f); }
    HRESULT STD W_TexUnlockRect(IDirect3DTexture9* t, UINT l) { return UnlockLevel(kTex2D, t, l); }
    HRESULT STD W_SurfLockRect(IDirect3DSurface9* s, D3DLOCKED_RECT* lr, const RECT* r, DWORD f) { return LockLevel(kSurface, s, 0, lr, r, f); }
    HRESULT STD W_SurfUnlockRect(IDirect3DSurface9* s) { return UnlockLevel(kSurface, s, 0); }
    HRESULT STD W_CubeLockRect(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES face, UINT l, D3DLOCKED_RECT* lr, const RECT* r, DWORD f)
    { return LockLevel(kCubeFace, t, (static_cast<uint32_t>(face) << 16) | l, lr, r, f); }
    HRESULT STD W_CubeUnlockRect(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES face, UINT l)
    { return UnlockLevel(kCubeFace, t, (static_cast<uint32_t>(face) << 16) | l); }

    HRESULT STD W_SurfGetDC(IDirect3DSurface9* s, HDC* dc)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return g_surf.GetDC(s, dc);
        if (m == kRecord) Drain();
        Gate g;
        return g_surf.GetDC(s, dc);
    }
    HRESULT STD W_SurfReleaseDC(IDirect3DSurface9* s, HDC dc)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return g_surf.ReleaseDC(s, dc);
        Gate g;
        return g_surf.ReleaseDC(s, dc);
    }

    HRESULT STD W_TexAddDirtyRect(IDirect3DTexture9* t, const RECT* r)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return g_tex.AddDirtyRect(t, r);
        if (m == kGated) { Gate g; return g_tex.AddDirtyRect(t, r); }
        RECT rr = {};
        if (r) rr = *r;
        Cmd* c = RecP(kTexDirtyRect, 2, &rr, sizeof(rr));
        c->a[0] = P(t);
        c->a[1] = r ? 1 : 0;
        Commit();
        return D3D_OK;
    }
    void STD W_TexGenerateMipSubLevels(IDirect3DTexture9* t)
    {
        const Mode m = ModeNow();
        if (m == kDirect) { g_tex.GenerateMipSubLevels(t); return; }
        if (m == kGated) { Gate g; g_tex.GenerateMipSubLevels(t); return; }
        Rec(kTexGenMips, P(t));
    }
    DWORD STD W_TexSetLOD(IDirect3DTexture9* t, DWORD lod)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return g_tex.SetLOD(t, lod);
        if (m == kGated) { Gate g; return g_tex.SetLOD(t, lod); }
        const DWORD old = g_tex.GetLOD(t);
        Rec(kTexSetLOD, P(t), lod);
        return old;
    }

    HRESULT STD W_VolLockBox(IDirect3DVolumeTexture9* t, UINT l, D3DLOCKED_BOX* b, const D3DBOX* box, DWORD f)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return g_vol.LockBox(t, l, b, box, f);
        if (m == kRecord) Drain();
        Gate g;
        return g_vol.LockBox(t, l, b, box, f);
    }
    HRESULT STD W_VolUnlockBox(IDirect3DVolumeTexture9* t, UINT l)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return g_vol.UnlockBox(t, l);
        Gate g;
        return g_vol.UnlockBox(t, l);
    }

    // ---- queries

    std::unordered_map<void*, uint64_t> g_issued;   // the command number of a query's last Issue

    HRESULT STD W_QueryIssue(IDirect3DQuery9* q, DWORD f)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return g_query.Issue(q, f);
        if (m == kGated) { Gate g; return g_query.Issue(q, f); }
        Rec(kQueryIssue, P(q), f);
        g_issued[q] = g_recorded;
        return D3D_OK;
    }
    HRESULT STD W_QueryGetData(IDirect3DQuery9* q, void* data, DWORD size, DWORD f)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return g_query.GetData(q, data, size, f);
        if (m == kRecord)
        {
            auto it = g_issued.find(q);
            if (it != g_issued.end())
            {
                if (DoneNow() < it->second)
                {
                    Publish();   // a game that polls must see the replay move on
                    return S_FALSE;
                }
                g_issued.erase(it);
            }
        }
        Gate g;
        return g_query.GetData(q, data, size, f);
    }

    // ---- the swap chain's Present (the game uses the device's; kept for completeness)

    HRESULT STD W_SwapPresent(IDirect3DSwapChain9* s, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f)
    {
        const Mode m = ModeNow();
        if (m == kDirect) return g_swap.Present(s, a, b, w, r, f);
        if (m == kRecord) Drain();
        Gate g;
        return g_swap.Present(s, a, b, w, r, f);
    }

    // ------------------------------------------------------------------------------------------------ the replay thread

    // A device command through a table: DXVK's (the replay) or our wrappers' (a state block's Apply).
    void ExecuteDev(const Cmd* c, const IDirect3DDevice9Vtbl& t, IDirect3DDevice9* d)
    {
        const uint32_t* a = c->a;
        switch (c->op)
        {
        case kSetRenderState:        t.SetRenderState(d, static_cast<D3DRENDERSTATETYPE>(a[0]), a[1]); break;
        case kSetSamplerState:       t.SetSamplerState(d, a[0], static_cast<D3DSAMPLERSTATETYPE>(a[1]), a[2]); break;
        case kSetTextureStageState:  t.SetTextureStageState(d, a[0], static_cast<D3DTEXTURESTAGESTATETYPE>(a[1]), a[2]); break;
        case kSetTexture:            t.SetTexture(d, a[0], Q<IDirect3DBaseTexture9>(a[1])); break;
        case kSetTransform:          t.SetTransform(d, static_cast<D3DTRANSFORMSTATETYPE>(a[0]), reinterpret_cast<const D3DMATRIX*>(Payload(c))); break;
        case kSetViewport:           t.SetViewport(d, reinterpret_cast<const D3DVIEWPORT9*>(Payload(c))); break;
        case kSetScissorRect:        t.SetScissorRect(d, reinterpret_cast<const RECT*>(Payload(c))); break;
        case kSetMaterial:           t.SetMaterial(d, reinterpret_cast<const D3DMATERIAL9*>(Payload(c))); break;
        case kSetLight:              t.SetLight(d, a[0], reinterpret_cast<const D3DLIGHT9*>(Payload(c))); break;
        case kLightEnable:           t.LightEnable(d, a[0], a[1]); break;
        case kSetClipPlane:          t.SetClipPlane(d, a[0], reinterpret_cast<const float*>(Payload(c))); break;
        case kSetVSConstF:           t.SetVertexShaderConstantF(d, a[0], reinterpret_cast<const float*>(Payload(c)), a[1]); break;
        case kSetVSConstI:           t.SetVertexShaderConstantI(d, a[0], reinterpret_cast<const int*>(Payload(c)), a[1]); break;
        case kSetVSConstB:           t.SetVertexShaderConstantB(d, a[0], reinterpret_cast<const BOOL*>(Payload(c)), a[1]); break;
        case kSetPSConstF:           t.SetPixelShaderConstantF(d, a[0], reinterpret_cast<const float*>(Payload(c)), a[1]); break;
        case kSetPSConstI:           t.SetPixelShaderConstantI(d, a[0], reinterpret_cast<const int*>(Payload(c)), a[1]); break;
        case kSetPSConstB:           t.SetPixelShaderConstantB(d, a[0], reinterpret_cast<const BOOL*>(Payload(c)), a[1]); break;
        case kSetStreamSource:       t.SetStreamSource(d, a[0], Q<IDirect3DVertexBuffer9>(a[1]), a[2], a[3]); break;
        case kSetStreamSourceFreq:   t.SetStreamSourceFreq(d, a[0], a[1]); break;
        case kSetIndices:            t.SetIndices(d, Q<IDirect3DIndexBuffer9>(a[0])); break;
        case kSetVertexDeclaration:  t.SetVertexDeclaration(d, Q<IDirect3DVertexDeclaration9>(a[0])); break;
        case kSetFVF:                t.SetFVF(d, a[0]); break;
        case kSetVertexShader:       t.SetVertexShader(d, Q<IDirect3DVertexShader9>(a[0])); break;
        case kSetPixelShader:        t.SetPixelShader(d, Q<IDirect3DPixelShader9>(a[0])); break;
        case kSetRenderTarget:       t.SetRenderTarget(d, a[0], Q<IDirect3DSurface9>(a[1])); break;
        case kSetDepthStencilSurface:t.SetDepthStencilSurface(d, Q<IDirect3DSurface9>(a[0])); break;
        case kSetNPatchMode:         t.SetNPatchMode(d, Fl(a[0])); break;
        case kSetSoftwareVP:         t.SetSoftwareVertexProcessing(d, a[0]); break;
        case kSetCurrentTexturePalette: t.SetCurrentTexturePalette(d, a[0]); break;
        case kSetClipStatus:         t.SetClipStatus(d, reinterpret_cast<const D3DCLIPSTATUS9*>(Payload(c))); break;
        case kBeginScene:            t.BeginScene(d); break;
        case kEndScene:              t.EndScene(d); break;
        case kClear:                 t.Clear(d, a[0], a[0] ? reinterpret_cast<const D3DRECT*>(Payload(c)) : nullptr, a[1], a[2], Fl(a[3]), a[4]); break;
        default: break;
        }
    }

    void Execute(const Cmd* c)
    {
        IDirect3DDevice9* d = g_dev;
        const uint32_t* a = c->a;
        if (c->op >= kSetRenderState && c->op <= kClear)   // the setters, BeginScene, EndScene and Clear
        {
            ExecuteDev(c, g_dx, d);
            return;
        }
        switch (c->op)
        {
        case kDrawPrimitive:         g_dx.DrawPrimitive(d, static_cast<D3DPRIMITIVETYPE>(a[0]), a[1], a[2]); break;
        case kDrawIndexedPrimitive:  g_dx.DrawIndexedPrimitive(d, static_cast<D3DPRIMITIVETYPE>(a[0]), static_cast<INT>(a[1]), a[2], a[3], a[4], a[5]); break;
        case kDrawPrimitiveUP:
        {
            const uint8_t* data = a[3] ? Q<uint8_t>(a[3]) : Payload(c);
            g_dx.DrawPrimitiveUP(d, static_cast<D3DPRIMITIVETYPE>(a[0]), a[1], data, a[2]);
            if (a[3]) FreeBlock(Q<uint8_t>(a[3]));
            break;
        }
        case kDrawIndexedPrimitiveUP:
        {
            const uint8_t* data = a[7] ? Q<uint8_t>(a[7]) : Payload(c);
            g_dx.DrawIndexedPrimitiveUP(d, static_cast<D3DPRIMITIVETYPE>(a[0]), a[1], a[2], a[3], data,
                                        static_cast<D3DFORMAT>(a[4]), data + a[6], a[5]);
            if (a[7]) FreeBlock(Q<uint8_t>(a[7]));
            break;
        }
        case kStretchRect:
        {
            const RECT* rr = reinterpret_cast<const RECT*>(Payload(c));
            g_dx.StretchRect(d, Q<IDirect3DSurface9>(a[0]), a[3] ? &rr[0] : nullptr, Q<IDirect3DSurface9>(a[1]),
                             a[4] ? &rr[1] : nullptr, static_cast<D3DTEXTUREFILTERTYPE>(a[2]));
            break;
        }
        case kColorFill:             g_dx.ColorFill(d, Q<IDirect3DSurface9>(a[0]), a[2] ? reinterpret_cast<const RECT*>(Payload(c)) : nullptr, a[1]); break;
        case kUpdateSurface:
        {
            struct X { RECT r; POINT p; };
            const X* x = reinterpret_cast<const X*>(Payload(c));
            g_dx.UpdateSurface(d, Q<IDirect3DSurface9>(a[0]), a[2] ? &x->r : nullptr, Q<IDirect3DSurface9>(a[1]), a[3] ? &x->p : nullptr);
            break;
        }
        case kUpdateTexture:         g_dx.UpdateTexture(d, Q<IDirect3DBaseTexture9>(a[0]), Q<IDirect3DBaseTexture9>(a[1])); break;
        case kGetRenderTargetData:   g_dx.GetRenderTargetData(d, Q<IDirect3DSurface9>(a[0]), Q<IDirect3DSurface9>(a[1])); break;
        case kPresent:
        {
            const HRESULT hr = g_dx.Present(d, nullptr, nullptr, Q<HWND__>(a[0]), nullptr);
            g_presentHr.store(hr);
            g_presentsDone.fetch_add(1, std::memory_order_release);
            break;
        }
        case kSetGammaRamp:          g_dx.SetGammaRamp(d, a[0], a[1], reinterpret_cast<const D3DGAMMARAMP*>(Payload(c))); break;
        case kEvictManaged:          g_dx.EvictManagedResources(d); break;
        case kBufUpload:
        {
            void* ptr = nullptr;
            uint8_t* block = Q<uint8_t>(a[5]);
            if (a[1])
            {
                auto* b = Q<IDirect3DIndexBuffer9>(a[0]);
                if (SUCCEEDED(g_ib.Lock(b, a[2], a[3], &ptr, a[4])) && ptr) { memcpy(ptr, block, a[3]); g_ib.Unlock(b); }
            }
            else
            {
                auto* b = Q<IDirect3DVertexBuffer9>(a[0]);
                if (SUCCEEDED(g_vb.Lock(b, a[2], a[3], &ptr, a[4])) && ptr) { memcpy(ptr, block, a[3]); g_vb.Unlock(b); }
            }
            FreeBlock(block);
            break;
        }
        case kTexUpload:
        {
            const TexKind k = static_cast<TexKind>(a[0]);
            void* o = Q<void>(a[1]);
            const RECT rc = { static_cast<LONG>(a[3]), static_cast<LONG>(a[4]), static_cast<LONG>(a[5]), static_cast<LONG>(a[6]) };
            const UINT rowBytes = a[8];
            uint8_t* block = Q<uint8_t>(a[9]);
            D3DLOCKED_RECT lr = {};
            if (SUCCEEDED(RealLockRect(k, o, a[2], &lr, &rc, a[7])) && lr.pBits)
            {
                const UINT rows = a[10];   // of 4 pixels in a block format, else of one
                for (UINT y = 0; y < rows; ++y)
                    memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, block + y * rowBytes, rowBytes);
                RealUnlockRect(k, o, a[2]);
            }
            FreeBlock(block);
            break;
        }
        case kTexDirtyRect:          g_tex.AddDirtyRect(Q<IDirect3DTexture9>(a[0]), a[1] ? reinterpret_cast<const RECT*>(Payload(c)) : nullptr); break;
        case kTexGenMips:            g_tex.GenerateMipSubLevels(Q<IDirect3DTexture9>(a[0])); break;
        case kTexSetLOD:             g_tex.SetLOD(Q<IDirect3DTexture9>(a[0]), a[1]); break;
        case kQueryIssue:            g_query.Issue(Q<IDirect3DQuery9>(a[0]), a[1]); break;
        case kRelease:
        {
            // Through each object's own vtable: our Release wrappers see the replay thread and call DXVK's.
            const uint32_t* list = reinterpret_cast<const uint32_t*>(Payload(c));
            const size_t n = (c->bytes - offsetof(Cmd, a)) / 4;
            for (size_t i = 0; i < n; ++i)
            {
                IUnknown* o = Q<IUnknown>(list[i]);
                o->lpVtbl->Release(o);
            }
            break;
        }
        default:
            break;
        }
    }

    DWORD WINAPI ReplayThread(LPVOID)
    {
        g_replayTid = Tid();
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        using SetDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (auto fn = reinterpret_cast<SetDescFn>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "SetThreadDescription")))
            fn(GetCurrentThread(), L"comfy-replay");
        uint32_t done = 0;
        for (;;)
        {
            uint32_t pub = g_published.load(std::memory_order_acquire);
            if (pub == done)
            {
                for (int spin = 0; spin < 200 && (pub = g_published.load(std::memory_order_acquire)) == done; ++spin)
                    _mm_pause();
                if (pub == done)
                {
                    g_sleeping.store(1, std::memory_order_release);
                    pub = g_published.load(std::memory_order_acquire);
                    if (pub == done)
                        WaitForSingleObject(g_workEvent, 5);
                    g_sleeping.store(0);
                    continue;
                }
            }
            const unsigned long long t0 = __rdtsc();
            AcquireSRWLockExclusive(&g_gate);
            int n = 0;
            while (done != pub && n < 256)
            {
                const Cmd* c = reinterpret_cast<const Cmd*>(g_rChunk + g_rOff);
                if (c->op == kNext)
                {
                    uint8_t* next = Q<uint8_t>(c->a[0]);
                    InterlockedPushEntrySList(&g_freeChunks, reinterpret_cast<SLIST_ENTRY*>(g_rChunk));
                    g_rChunk = next;
                    g_rOff = 0;
                }
                else
                {
                    Execute(c);
                    g_rOff += c->bytes;
                }
                ++done;
                ++n;
                if (g_gateWant.load(std::memory_order_relaxed) > 0)
                    break;
            }
            ReleaseSRWLockExclusive(&g_gate);
            g_replayTicks.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
            g_done.store(done, std::memory_order_release);
            if (g_waiting.load(std::memory_order_acquire) &&
                static_cast<int32_t>(done - g_waitFor.load(std::memory_order_acquire)) >= 0)
                SetEvent(g_doneEvent);
        }
    }

    void Activate(IDirect3DDevice9* d)
    {
        g_dev = d;
        g_gameTid = Tid();
        LoadMirror(d);
        g_wChunk = g_rChunk = NewChunk();
        g_wOff = g_rOff = 0;
        g_workEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = CreateThread(nullptr, 0, ReplayThread, nullptr, 0, nullptr);
        if (!t)
        {
            Log("stream: the replay thread could not start; the stream stays off");
            return;
        }
        CloseHandle(t);
        while (!g_replayTid)
            Sleep(0);
        g_statsT0 = Now();
        g_active = true;
        Log("stream: on, device %p, the game's thread %u, the replay thread %u, %d frame%s ahead", d, g_gameTid,
            g_replayTid, g_ahead, g_ahead == 1 ? "" : "s");
    }

    // ------------------------------------------------------------------------------------------------ install

    template <class Vtbl> void Copy(Vtbl& dst, const void* src) { memcpy(&dst, src, sizeof(Vtbl)); }
#define PATCH(vt, field, fn) PatchSlot(reinterpret_cast<void**>(&(vt)->field), reinterpret_cast<void*>(fn))
}

void StreamInstall(IDirect3DDevice9* probe)
{
    const wchar_t* ini = ConfigIniPath();
    if (!ini || !GetPrivateProfileIntW(L"stream", L"enabled", 0, ini))
        return;
    g_stats = GetPrivateProfileIntW(L"stream", L"stats", 0, ini) != 0;
    g_exactRelease = GetPrivateProfileIntW(L"stream", L"exactRelease", 0, ini) != 0;
    g_ahead = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"stream", L"framesAhead", 1, ini)), 1, 3);
    for (auto& h : g_free)
        InitializeSListHead(&h);
    InitializeSListHead(&g_freeChunks);

    auto* v = const_cast<IDirect3DDevice9Vtbl*>(probe->lpVtbl);
    Copy(g_dx, v);
    // Every slot that is not DXVK's own already: another mod hooked before us. Logged; its hook then runs on the
    // replay thread.
    HMODULE dxvk = GetModuleHandleA("d3d9.dll");
    MODULEINFO mi = {};
    if (dxvk && GetModuleInformation(GetCurrentProcess(), dxvk, &mi, sizeof(mi)))
    {
        const uintptr_t lo = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll), hi = lo + mi.SizeOfImage;
        void** slots = reinterpret_cast<void**>(v);
        for (size_t i = 0; i < sizeof(IDirect3DDevice9Vtbl) / sizeof(void*); ++i)
        {
            const auto a = reinterpret_cast<uintptr_t>(slots[i]);
            if (a < lo || a >= hi)
                Log("stream: device slot %u is hooked already (%p), before us", static_cast<unsigned>(i), slots[i]);
        }
    }

    PATCH(v, QueryInterface, W_QueryInterface); PATCH(v, AddRef, W_AddRef); PATCH(v, Release, W_Release);
    PATCH(v, TestCooperativeLevel, W_TestCooperativeLevel); PATCH(v, GetAvailableTextureMem, W_GetAvailableTextureMem);
    PATCH(v, EvictManagedResources, W_EvictManagedResources); PATCH(v, GetDirect3D, W_GetDirect3D);
    PATCH(v, GetDeviceCaps, W_GetDeviceCaps); PATCH(v, GetDisplayMode, W_GetDisplayMode);
    PATCH(v, GetCreationParameters, W_GetCreationParameters); PATCH(v, SetCursorProperties, W_SetCursorProperties);
    PATCH(v, SetCursorPosition, W_SetCursorPosition); PATCH(v, ShowCursor, W_ShowCursor);
    PATCH(v, CreateAdditionalSwapChain, W_CreateAdditionalSwapChain); PATCH(v, GetSwapChain, W_GetSwapChain);
    PATCH(v, GetNumberOfSwapChains, W_GetNumberOfSwapChains); PATCH(v, Reset, W_Reset); PATCH(v, Present, W_Present);
    PATCH(v, GetBackBuffer, W_GetBackBuffer); PATCH(v, GetRasterStatus, W_GetRasterStatus);
    PATCH(v, SetDialogBoxMode, W_SetDialogBoxMode); PATCH(v, SetGammaRamp, W_SetGammaRamp); PATCH(v, GetGammaRamp, W_GetGammaRamp);
    PATCH(v, CreateTexture, W_CreateTexture); PATCH(v, CreateVolumeTexture, W_CreateVolumeTexture);
    PATCH(v, CreateCubeTexture, W_CreateCubeTexture); PATCH(v, CreateVertexBuffer, W_CreateVertexBuffer);
    PATCH(v, CreateIndexBuffer, W_CreateIndexBuffer); PATCH(v, CreateRenderTarget, W_CreateRenderTarget);
    PATCH(v, CreateDepthStencilSurface, W_CreateDepthStencilSurface); PATCH(v, UpdateSurface, W_UpdateSurface);
    PATCH(v, UpdateTexture, W_UpdateTexture); PATCH(v, GetRenderTargetData, W_GetRenderTargetData);
    PATCH(v, GetFrontBufferData, W_GetFrontBufferData); PATCH(v, StretchRect, W_StretchRect); PATCH(v, ColorFill, W_ColorFill);
    PATCH(v, CreateOffscreenPlainSurface, W_CreateOffscreenPlainSurface); PATCH(v, SetRenderTarget, W_SetRenderTarget);
    PATCH(v, GetRenderTarget, W_GetRenderTarget); PATCH(v, SetDepthStencilSurface, W_SetDepthStencilSurface);
    PATCH(v, GetDepthStencilSurface, W_GetDepthStencilSurface); PATCH(v, BeginScene, W_BeginScene); PATCH(v, EndScene, W_EndScene);
    PATCH(v, Clear, W_Clear); PATCH(v, SetTransform, W_SetTransform); PATCH(v, GetTransform, W_GetTransform);
    PATCH(v, MultiplyTransform, W_MultiplyTransform); PATCH(v, SetViewport, W_SetViewport); PATCH(v, GetViewport, W_GetViewport);
    PATCH(v, SetMaterial, W_SetMaterial); PATCH(v, GetMaterial, W_GetMaterial); PATCH(v, SetLight, W_SetLight);
    PATCH(v, GetLight, W_GetLight); PATCH(v, LightEnable, W_LightEnable); PATCH(v, GetLightEnable, W_GetLightEnable);
    PATCH(v, SetClipPlane, W_SetClipPlane); PATCH(v, GetClipPlane, W_GetClipPlane); PATCH(v, SetRenderState, W_SetRenderState);
    PATCH(v, GetRenderState, W_GetRenderState); PATCH(v, CreateStateBlock, W_CreateStateBlock);
    PATCH(v, BeginStateBlock, W_BeginStateBlock); PATCH(v, EndStateBlock, W_EndStateBlock); PATCH(v, SetClipStatus, W_SetClipStatus);
    PATCH(v, GetClipStatus, W_GetClipStatus); PATCH(v, GetTexture, W_GetTexture); PATCH(v, SetTexture, W_SetTexture);
    PATCH(v, GetTextureStageState, W_GetTextureStageState); PATCH(v, SetTextureStageState, W_SetTextureStageState);
    PATCH(v, GetSamplerState, W_GetSamplerState); PATCH(v, SetSamplerState, W_SetSamplerState); PATCH(v, ValidateDevice, W_ValidateDevice);
    PATCH(v, SetPaletteEntries, W_SetPaletteEntries); PATCH(v, GetPaletteEntries, W_GetPaletteEntries);
    PATCH(v, SetCurrentTexturePalette, W_SetCurrentTexturePalette); PATCH(v, GetCurrentTexturePalette, W_GetCurrentTexturePalette);
    PATCH(v, SetScissorRect, W_SetScissorRect); PATCH(v, GetScissorRect, W_GetScissorRect);
    PATCH(v, SetSoftwareVertexProcessing, W_SetSoftwareVertexProcessing); PATCH(v, GetSoftwareVertexProcessing, W_GetSoftwareVertexProcessing);
    PATCH(v, SetNPatchMode, W_SetNPatchMode); PATCH(v, GetNPatchMode, W_GetNPatchMode); PATCH(v, DrawPrimitive, W_DrawPrimitive);
    PATCH(v, DrawIndexedPrimitive, W_DrawIndexedPrimitive); PATCH(v, DrawPrimitiveUP, W_DrawPrimitiveUP);
    PATCH(v, DrawIndexedPrimitiveUP, W_DrawIndexedPrimitiveUP); PATCH(v, ProcessVertices, W_ProcessVertices);
    PATCH(v, CreateVertexDeclaration, W_CreateVertexDeclaration); PATCH(v, SetVertexDeclaration, W_SetVertexDeclaration);
    PATCH(v, GetVertexDeclaration, W_GetVertexDeclaration); PATCH(v, SetFVF, W_SetFVF); PATCH(v, GetFVF, W_GetFVF);
    PATCH(v, CreateVertexShader, W_CreateVertexShader); PATCH(v, SetVertexShader, W_SetVertexShader);
    PATCH(v, GetVertexShader, W_GetVertexShader); PATCH(v, SetVertexShaderConstantF, W_SetVertexShaderConstantF);
    PATCH(v, GetVertexShaderConstantF, W_GetVertexShaderConstantF); PATCH(v, SetVertexShaderConstantI, W_SetVertexShaderConstantI);
    PATCH(v, GetVertexShaderConstantI, W_GetVertexShaderConstantI); PATCH(v, SetVertexShaderConstantB, W_SetVertexShaderConstantB);
    PATCH(v, GetVertexShaderConstantB, W_GetVertexShaderConstantB); PATCH(v, SetStreamSource, W_SetStreamSource);
    PATCH(v, GetStreamSource, W_GetStreamSource); PATCH(v, SetStreamSourceFreq, W_SetStreamSourceFreq);
    PATCH(v, GetStreamSourceFreq, W_GetStreamSourceFreq); PATCH(v, SetIndices, W_SetIndices); PATCH(v, GetIndices, W_GetIndices);
    PATCH(v, CreatePixelShader, W_CreatePixelShader); PATCH(v, SetPixelShader, W_SetPixelShader); PATCH(v, GetPixelShader, W_GetPixelShader);
    PATCH(v, SetPixelShaderConstantF, W_SetPixelShaderConstantF); PATCH(v, GetPixelShaderConstantF, W_GetPixelShaderConstantF);
    PATCH(v, SetPixelShaderConstantI, W_SetPixelShaderConstantI); PATCH(v, GetPixelShaderConstantI, W_GetPixelShaderConstantI);
    PATCH(v, SetPixelShaderConstantB, W_SetPixelShaderConstantB); PATCH(v, GetPixelShaderConstantB, W_GetPixelShaderConstantB);
    PATCH(v, DrawRectPatch, W_DrawRectPatch); PATCH(v, DrawTriPatch, W_DrawTriPatch); PATCH(v, DeletePatch, W_DeletePatch);
    PATCH(v, CreateQuery, W_CreateQuery);
    Copy(g_wt, v);   // our wrappers, before comfyatmos hooks the same slots
    g_m = new Mirror;
    memset(g_m, 0, sizeof(Mirror));

    // The resource classes, from throwaway objects of the probe device (each DXVK class has one vtable).
    IDirect3DVertexBuffer9* vb = nullptr;
    if (SUCCEEDED(g_dx.CreateVertexBuffer(probe, 64, 0, 0, D3DPOOL_DEFAULT, &vb, nullptr)) && vb)
    {
        auto* t = const_cast<IDirect3DVertexBuffer9Vtbl*>(vb->lpVtbl);
        Copy(g_vb, t);
        PATCH(t, Release, W_VbRelease); PATCH(t, Lock, W_VbLock); PATCH(t, Unlock, W_VbUnlock);
        g_vb.Release(vb);
    }
    IDirect3DIndexBuffer9* ib = nullptr;
    if (SUCCEEDED(g_dx.CreateIndexBuffer(probe, 64, 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, nullptr)) && ib)
    {
        auto* t = const_cast<IDirect3DIndexBuffer9Vtbl*>(ib->lpVtbl);
        Copy(g_ib, t);
        PATCH(t, Release, W_IbRelease); PATCH(t, Lock, W_IbLock); PATCH(t, Unlock, W_IbUnlock);
        g_ib.Release(ib);
    }
    IDirect3DTexture9* tex = nullptr;
    if (SUCCEEDED(g_dx.CreateTexture(probe, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) && tex)
    {
        auto* t = const_cast<IDirect3DTexture9Vtbl*>(tex->lpVtbl);
        Copy(g_tex, t);
        PATCH(t, Release, W_TexRelease); PATCH(t, LockRect, W_TexLockRect); PATCH(t, UnlockRect, W_TexUnlockRect);
        PATCH(t, AddDirtyRect, W_TexAddDirtyRect); PATCH(t, GenerateMipSubLevels, W_TexGenerateMipSubLevels);
        PATCH(t, SetLOD, W_TexSetLOD);
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(g_tex.GetSurfaceLevel(tex, 0, &s)) && s)
        {
            auto* st = const_cast<IDirect3DSurface9Vtbl*>(s->lpVtbl);
            Copy(g_surf, st);
            PATCH(st, Release, W_SurfRelease); PATCH(st, LockRect, W_SurfLockRect); PATCH(st, UnlockRect, W_SurfUnlockRect);
            PATCH(st, GetDC, W_SurfGetDC); PATCH(st, ReleaseDC, W_SurfReleaseDC);
            g_surf.Release(s);
        }
        g_tex.Release(tex);
    }
    IDirect3DCubeTexture9* cube = nullptr;
    if (SUCCEEDED(g_dx.CreateCubeTexture(probe, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr)) && cube)
    {
        auto* t = const_cast<IDirect3DCubeTexture9Vtbl*>(cube->lpVtbl);
        Copy(g_cube, t);
        PATCH(t, Release, W_CubeRelease); PATCH(t, LockRect, W_CubeLockRect); PATCH(t, UnlockRect, W_CubeUnlockRect);
        g_cube.Release(cube);
    }
    IDirect3DVolumeTexture9* vol = nullptr;
    if (SUCCEEDED(g_dx.CreateVolumeTexture(probe, 4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vol, nullptr)) && vol)
    {
        auto* t = const_cast<IDirect3DVolumeTexture9Vtbl*>(vol->lpVtbl);
        Copy(g_vol, t);
        PATCH(t, Release, W_VolRelease); PATCH(t, LockBox, W_VolLockBox); PATCH(t, UnlockBox, W_VolUnlockBox);
        g_vol.Release(vol);
    }
    IDirect3DQuery9* q = nullptr;
    if (SUCCEEDED(g_dx.CreateQuery(probe, D3DQUERYTYPE_EVENT, &q)) && q)
    {
        auto* t = const_cast<IDirect3DQuery9Vtbl*>(q->lpVtbl);
        Copy(g_query, t);
        PATCH(t, Release, W_QueryRelease); PATCH(t, Issue, W_QueryIssue); PATCH(t, GetData, W_QueryGetData);
        g_query.Release(q);
    }
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(g_dx.GetSwapChain(probe, 0, &sc)) && sc)
    {
        auto* t = const_cast<IDirect3DSwapChain9Vtbl*>(sc->lpVtbl);
        Copy(g_swap, t);
        PATCH(t, Release, W_SwapRelease); PATCH(t, Present, W_SwapPresent);
        g_swap.Release(sc);
    }
    IDirect3DStateBlock9* sb = nullptr;
    if (SUCCEEDED(g_dx.CreateStateBlock(probe, D3DSBT_ALL, &sb)) && sb)
    {
        auto* t = const_cast<IDirect3DStateBlock9Vtbl*>(sb->lpVtbl);
        Copy(g_sbReal, t);
        PATCH(t, Release, W_SbRealRelease); PATCH(t, Apply, W_SbRealApply); PATCH(t, Capture, W_SbRealCapture);
        g_sbReal.Release(sb);
    }
    g_installed = true;
    Log("stream: installed (stats %s, %d frame%s ahead); it starts at the game's first Present", g_stats ? "on" : "off",
        g_ahead, g_ahead == 1 ? "" : "s");
}

bool StreamActive() { return g_active; }

void StreamDrain() { Drain(); }

void StreamFrameStats()
{
    if (!g_stats || !g_active)
        return;
    const double now = Now();
    if (now - g_statsT0 < 5.0 || !g_frames)
        return;
    const double secs = now - g_statsT0;
    const double f = static_cast<double>(g_frames);
    static double tscHz = 0.0;
    if (tscHz == 0.0)
    {
        LARGE_INTEGER q0, q1, fq;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&q0);
        const unsigned long long c0 = __rdtsc();
        Sleep(20);
        QueryPerformanceCounter(&q1);
        tscHz = static_cast<double>(__rdtsc() - c0) / (static_cast<double>(q1.QuadPart - q0.QuadPart) / fq.QuadPart);
    }
    const double replayMs = static_cast<double>(g_replayTicks.exchange(0)) / tscHz * 1000.0 / f;
    Log("stream: %.0f frames in %.1f s (%.1f fps); replay %.2f ms a frame; the game waited %.2f ms a frame (drains "
        "%.2f ms, %.1f a frame); gated calls %.1f, locks %.1f (%.1f dynamic, %.1f texture, %.0f KB, %.0f KB copied), "
        "read backs %.2f, relocked levels %.2f, real state block applies %.2f, whole dynamic locks %.2f a frame", f, secs, f / secs, replayMs, 1000.0 * g_waitSec / f,
        1000.0 * g_drainSec / f, g_drains / f, g_gated / f, g_lockCalls / f, g_dynLocks / f, g_texLocks / f,
        g_lockBytes / f / 1024.0, g_lockCopied / f / 1024.0, g_readbacks / f, g_relock / f, g_sbApplies / f, g_dynWhole / f);
    std::vector<std::pair<uint64_t, int>> ops;
    for (int i = 2; i < kOpCount; ++i)
        if (g_opCount[i])
            ops.emplace_back(g_opCount[i], i);
    std::sort(ops.rbegin(), ops.rend());
    char line[1024];
    size_t at = 0;
    uint64_t total = 0;
    for (auto& [n, i] : ops)
        total += n;
    at += snprintf(line + at, sizeof(line) - at, "stream: %.0f commands a frame:", total / f);
    for (size_t k = 0; k < ops.size() && k < 14 && at < sizeof(line) - 64; ++k)
        at += snprintf(line + at, sizeof(line) - at, " %s %.0f", kOpNames[ops[k].second], ops[k].first / f);
    Log("%s", line);
    memset(g_opCount, 0, sizeof(g_opCount));
    g_frames = g_lockCalls = g_lockBytes = g_lockCopied = g_readbacks = g_drains = g_gated = g_dynLocks = g_texLocks = g_relock = g_sbApplies = g_dynWhole = 0;
    g_waitSec = g_drainSec = 0.0;
    g_statsT0 = now;
}
