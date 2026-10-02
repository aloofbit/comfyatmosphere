// comfyfog: the atmosphere effects for the 1.12 client.
//
// Same shape as comfygrass: loaded by VanillaFixes from dlls.txt, attaches by patching DXVK's shared
// IDirect3DDevice9 vtable in place (found through a throwaway device of our own), and is tuned from an
// ini that reloads in game. See comfygrass/src/README.md for why each of those choices was made.
//
// The game's own fog goes through unchanged (2026-09-30: the old fog, which rewrote it, was removed). Its
// start, end and colour are mirrored, for the passes that fade with it (WorldFog, WorldFogColor).
//
// Load order with comfygrass. Both DLLs patch the same vtable slots, so whichever patches last sits
// outermost and sees the client's calls first. The old fog had to be the OUTER hook, so that comfygrass
// fogged grass from the rewritten fog states. The wait in AttachToDxvk is kept from then.

#define CINTERFACE // C-style IDirect3DDevice9Vtbl, so slots are patched by name, not by index
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "bench.h"
#include "bodymask.h"
#include "client.h"
#include "common.h"
#include "cover.h"
#include "config.h"
#include "cvars.h"
#include "report.h"
#include "depth.h"
#include "lampglow.h"
#include "lamps.h"
#include "rays.h"
#include "shadow.h"
#include "sun.h"
#include "sunshadows.h"
#include "terrainshade.h"
#include "mapterrain.h"
#include "volume.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <unordered_set>
#include <set>
#include <utility>
#include <vector>

namespace
{
    wchar_t g_iniPath[MAX_PATH] = {};
    wchar_t g_logPath[MAX_PATH] = {};

    // The attach thread and the render thread both log; one lock keeps lines whole.
    CRITICAL_SECTION g_lock;
    bool             g_lockReady = false;

    struct Guard
    {
        Guard()  { if (g_lockReady) EnterCriticalSection(&g_lock); }
        ~Guard() { if (g_lockReady) LeaveCriticalSection(&g_lock); }
    };
}

void Log(const char* fmt, ...)
{
    if (!g_cfg.logEnabled)
        return;
    Guard g;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath, L"a") != 0 || !f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

double Now()
{
    static double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * inv;
}

FARPROC CompilerProc(const char* name)
{
    static HMODULE comp = [] {
        HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
        return m ? m : LoadLibraryA("d3dcompiler_47.dll");
    }();
    return comp ? GetProcAddress(comp, name) : nullptr;
}

namespace
{
    inline float D2F(DWORD d) { float f; memcpy(&f, &d, 4); return f; }
    inline DWORD F2D(float f) { DWORD d; memcpy(&d, &f, 4); return d; }
    inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    // Same as comfygrass's, except the write is a compare-exchange: two mods patch these slots, and a
    // plain read-then-write racing another installer can silently drop one of the two hooks.
    bool HookSlot(void** slot, void* hook, void** origOut)
    {
        if (*slot == hook)
            return true;

        DWORD prot = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot))
            return false;
        void* cur = *slot;
        while (cur != hook)
        {
            *origOut = cur;   // set before the swap, so the hook never runs with a null original
            void* prev = InterlockedCompareExchangePointer(slot, hook, cur);
            if (prev == cur)
                break;
            cur = prev;       // someone else got there first; chain onto whatever they installed
        }
        VirtualProtect(slot, sizeof(void*), prot, &prot);
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    // the client's fog, as it asked for it: for the probe

    struct ClientFog
    {
        DWORD start   = F2D(0.0f);   // D3D9 defaults
        DWORD end     = F2D(1.0f);
        DWORD density = F2D(1.0f);
        DWORD color   = 0;
        bool  haveStart = false, haveEnd = false, haveDensity = false, haveColor = false;
    };

    ClientFog g_fog;

    // The fog the world is drawn with, for the lamp glow (lampglow.cpp), which is drawn after the client
    // has parked its fog for the UI (start 0, end 1). Taken from each end the client sets during the world.
    DWORD     g_worldFogStart = 0, g_worldFogEnd = 0;
    bool      g_haveWorldFog  = false;
    DWORD     g_worldFogColor = 0;          // the last fog colour the world set that was not black
    bool      g_haveWorldFogColor = false;


    // ---------------------------------------------------------------------------------------------
    // diagnostics
    //
    // Every change of a raw fog value is logged (up to a cap), which answers whether the client sets fog
    // per zone or per draw. The probe key logs one whole frame: each fog state set, with the draw index it
    // landed at, and how many fogged draws went through a vertex shader. Those compute their own fog
    // and may not follow FOGSTART/FOGEND at all (Model2.bls drives every M2).

    const char* FogStateName(DWORD st)
    {
        switch (st)
        {
        case D3DRS_FOGENABLE:      return "FOGENABLE";
        case D3DRS_FOGCOLOR:       return "FOGCOLOR";
        case D3DRS_FOGTABLEMODE:   return "FOGTABLEMODE";
        case D3DRS_FOGVERTEXMODE:  return "FOGVERTEXMODE";
        case D3DRS_FOGSTART:       return "FOGSTART";
        case D3DRS_FOGEND:         return "FOGEND";
        case D3DRS_FOGDENSITY:     return "FOGDENSITY";
        case D3DRS_RANGEFOGENABLE: return "RANGEFOGENABLE";
        default:                   return nullptr;
        }
    }

    bool IsFloatState(DWORD st)
    {
        return st == D3DRS_FOGSTART || st == D3DRS_FOGEND || st == D3DRS_FOGDENSITY;
    }

    struct Probe
    {
        bool     armed   = false;
        bool     active  = false;
        uint32_t draws   = 0;
        uint32_t fogged  = 0;   // FOGENABLE on
        uint32_t foggedVs = 0;  // ... with a vertex shader bound
        uint32_t fogSets = 0;
        uint32_t constSets = 0;
        uint32_t constLogs = 0;
        uint32_t boundary  = 0xFFFFFFFF;  // draw index the world ended at, for the detail window
        // After our passes (2026-09-30): with the chat hidden (/togglechat) the passes ran and did not show,
        // and the detail window ended just after them. Every later draw is counted, and each one that is not
        // alpha blended or has a pixel shader is listed: one of those could paint over the passes.
        bool     afterPass = false;
        uint32_t lateDraws = 0;
        uint32_t lateListed = 0;
    };

    Probe    g_probe;
    uint64_t g_frame      = 0;
    DWORD    g_fogEnable  = 0;
    // Each distinct (state, value) the client sends is logged once. It re-sends fog many times a frame,
    // switching between the zone colour and black, so logging every change filled the cap in 5 frames.
    std::set<std::pair<DWORD, DWORD>> g_seenValues;
    void*    g_vshader    = nullptr;
    uint32_t g_frameDraws = 0;       // every draw this frame, probe or not; see hkSetTransform
    D3DMATRIX g_world     = {};      // last world/view/projection, unfiltered, for the probe's sky dump
    bool      g_skyPhase  = true;    // until the frame's first depth-writing draw (see IsCloudDraw)
    D3DMATRIX g_viewAll   = {};
    D3DMATRIX g_projAll   = {};

    UINT VertsForPrims(D3DPRIMITIVETYPE prim, UINT primCount)
    {
        switch (prim)
        {
        case D3DPT_POINTLIST:     return primCount;
        case D3DPT_LINELIST:      return primCount * 2;
        case D3DPT_LINESTRIP:     return primCount + 1;
        case D3DPT_TRIANGLELIST:  return primCount * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN:   return primCount + 2;
        default:                  return 0;
        }
    }
    bool     g_lastPersp  = false;
    bool     g_reloadDown = false;
    bool     g_probeDown  = false;

    void NoteFogState(DWORD st, DWORD raw, DWORD sent)
    {
        const char* name = FogStateName(st);
        if (!name)
            return;

        if (g_probe.active)
        {
            g_probe.fogSets++;
            if (IsFloatState(st))
                Log("  [draw %4u] %-14s client=%.3f sent=%.3f", g_probe.draws, name, D2F(raw), D2F(sent));
            else
                Log("  [draw %4u] %-14s client=0x%08X sent=0x%08X", g_probe.draws, name, raw, sent);
        }

        if (g_seenValues.size() < 300 && g_seenValues.insert({ st, raw }).second)
        {
            if (IsFloatState(st))
                Log("fog value %-14s %.3f (sent %.3f), frame %llu", name, D2F(raw), D2F(sent), g_frame);
            else
                Log("fog value %-14s 0x%08X (sent 0x%08X), frame %llu", name, raw, sent, g_frame);
        }
    }

    // ---------------------------------------------------------------------------------------------
    // hooks

    using PresentFn      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using ResetFn        = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    using SetRSFn        = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
    using SetVSFn        = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
    using SetPSFn        = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
    using DrawPrimFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    using DrawIdxPrimFn  = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    using DrawPrimUPFn   = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    using DrawIdxPrimUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                                        const void*, D3DFORMAT, const void*, UINT);

    PresentFn       g_oPresent       = nullptr;

    using BeginSceneFn  = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
    BeginSceneFn    g_oBeginScene    = nullptr;

    using SetDSFn       = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*);
    SetDSFn         g_oSetDS         = nullptr;
    ResetFn         g_oReset         = nullptr;
    SetRSFn         g_oSetRS         = nullptr;
    SetVSFn         g_oSetVS         = nullptr;
    SetPSFn         g_oSetPS         = nullptr;
    DrawPrimFn      g_oDrawPrim      = nullptr;
    DrawIdxPrimFn   g_oDrawIdxPrim   = nullptr;
    DrawPrimUPFn    g_oDrawPrimUP    = nullptr;
    DrawIdxPrimUPFn g_oDrawIdxPrimUP = nullptr;

    using SetVSConstFFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
    SetVSConstFFn   g_oSetVSConstF   = nullptr;

    using SetTransformFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
    SetTransformFn  g_oSetTransform  = nullptr;

    using SetLightFn    = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DLIGHT9*);
    using LightEnableFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, BOOL);
    SetLightFn      g_oSetLight      = nullptr;
    LightEnableFn   g_oLightEnable   = nullptr;

    using SetRTFn       = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    using StretchRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
                                                      IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
    SetRTFn         g_oSetRT         = nullptr;

    StretchRectFn   g_oStretchRect   = nullptr;
    bool            g_inPass         = false;   // our own passes: their calls stay out of the mirrors and the probe

    // ---------------------------------------------------------------------------------------------
    // vertex shader dumps
    //
    // M2s (trees, doodads, every character) draw through the client's Model2 vertex shaders, which write
    // oFog themselves from c30: mad r.w, viewZ, c30.x, c30.y, with c30.x = -1/(end-start) and
    // c30.y = end/(end-start). Each shader is disassembled and logged once, so a different client build can
    // be checked from one log. The probe key logs the small constant uploads of one frame (bone palettes
    // are large, so uploads of more than 4 registers are only counted).

    std::set<void*> g_dumpedShaders;

    void DumpShader(IDirect3DVertexShader9* sh)
    {
        if (!sh || g_dumpedShaders.size() >= 32 || !g_dumpedShaders.insert(sh).second)
            return;

        UINT size = 0;
        if (FAILED(sh->lpVtbl->GetFunction(sh, nullptr, &size)) || !size)
        {
            Log("vertex shader %p: GetFunction gave nothing", sh);
            return;
        }
        std::vector<uint8_t> code(size);
        if (FAILED(sh->lpVtbl->GetFunction(sh, code.data(), &size)))
            return;

        static auto disasm = reinterpret_cast<PFN_D3DDisassemble>(CompilerProc("D3DDisassemble"));

        OgBlob* text = nullptr;
        if (!disasm || FAILED(disasm(code.data(), size, 0, nullptr, &text)) || !text)
        {
            Log("vertex shader %p: %u bytes, could not disassemble", sh, size);
            return;
        }
        Log("=== vertex shader %p (%u bytes), first bound at frame %llu ===\n%s=== end %p ===",
            sh, size, g_frame, static_cast<const char*>(text->lpVtbl->GetBufferPointer(text)), sh);
        text->lpVtbl->Release(text);
    }


    HRESULT STDMETHODCALLTYPE hkSetVertexShaderConstantF(IDirect3DDevice9* dev, UINT reg, const float* data,
                                                         UINT count)
    {
        if (g_inPass)
            return g_oSetVSConstF(dev, reg, data, count);   // our own passes: no fog remap, no recording
        RecordConstants(reg, data, count);
        LampsConstants(reg, data, count);
        if (g_probe.active && data)
        {
            g_probe.constSets++;
            if (count <= 4 && g_probe.constLogs < 400)
            {
                g_probe.constLogs++;
                for (UINT i = 0; i < count; ++i)
                    Log("  [draw %4u] vs=%p c%-3u = %12.4f %12.4f %12.4f %12.4f", g_probe.draws, g_vshader,
                        reg + i, data[4 * i], data[4 * i + 1], data[4 * i + 2], data[4 * i + 3]);
            }
        }

        return g_oSetVSConstF(dev, reg, data, count);
    }

    bool               g_worldEnded = false;    // the world finished drawing this frame
    std::unordered_set<void*> g_waterPs;        // the client's water pixel shaders (IsWaterDraw)
    unsigned g_seeThroughDraws = 0, g_seeThroughLast = 0;   // IsSeeThroughModel's draws this frame, and last frame
    unsigned g_depthOnlySkipped = 0, g_depthOnlyLast = 0;   // IsDepthOnlyModel's, the same way
    bool     g_ownFaded = false;   // the camera within [depth] seeThroughNear of your character (hkBeginScene)
    float    g_ownDist  = 1e9f;    // ... how far, in yards

    // The order of the world's draws, for one frame after each probe (2026-09-30): can our fog go in before
    // the blended draws? Grass drawn blended leaves the sky's depth behind it, and the fog painted full
    // horizon fog over every blade on the skyline. Runs of: O depth-writing, B blended without depth writes,
    // W blended with depth writes, N neither.
    bool        g_orderArm = false, g_orderRec = false;
    bool        g_benchArmed = false;   // /atmos bench: started at the next Present (PollKeys), which has the device
    std::string g_orderRuns;
    char        g_orderLast = 0;
    unsigned    g_orderRun = 0, g_orderIndex = 0, g_orderFirstB = 0, g_orderOAfterB = 0;
    std::string g_orderAfter;   // the first depth-writing draws after the first blended one
    std::string g_orderBlended; // every blended draw, with or without depth writes: the water, a stealthed unit
    unsigned    g_orderBCount = 0;
    std::vector<std::string> g_orderModels;   // the last model draws of the frame, with colour and depth state

    void OrderFlush()
    {
        if (g_orderRun)
        {
            char t[24];
            snprintf(t, sizeof(t), "%s%c%u", g_orderRuns.empty() ? "" : " ", g_orderLast, g_orderRun);
            if (g_orderRuns.size() < 1500)
                g_orderRuns += t;
        }
        g_orderRun = 0;
    }

    void NoteOrder(IDirect3DDevice9* dev, const char* call, UINT pc, UINT nv)
    {
        if (!g_orderRec || g_inPass || g_skyPhase || g_worldEnded)
            return;
        DWORD blend = 0, zwrite = 0, atest = 0;
        dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
        dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zwrite);
        dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &atest);
        const char k = zwrite ? (blend ? 'W' : 'O') : (blend ? 'B' : 'N');
        ++g_orderIndex;
        if (k == 'B' && !g_orderFirstB)
            g_orderFirstB = g_orderIndex;
        // The blended draws that write no depth, each: the water is among them, and the fog cannot see it.
        // Every model draw, last 30 kept: a stealthed unit is drawn in two passes (2026-09-30).
        if (g_vshader)
        {
            DWORD cw = 0, zf = 0, ze = 0, src = 0, dst = 0;
            dev->lpVtbl->GetRenderState(dev, D3DRS_COLORWRITEENABLE, &cw);
            dev->lpVtbl->GetRenderState(dev, D3DRS_ZFUNC, &zf);
            dev->lpVtbl->GetRenderState(dev, D3DRS_ZENABLE, &ze);
            dev->lpVtbl->GetRenderState(dev, D3DRS_SRCBLEND, &src);
            dev->lpVtbl->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
            IDirect3DBaseTexture9* tex = nullptr;
            dev->lpVtbl->GetTexture(dev, 0, &tex);
            char t[200];
            snprintf(t, sizeof(t), "[#%u %c %up %uv tex %p colour 0x%X ztest %u func %u zwrite %u blend %u %u/%u%s]",
                     g_orderIndex, k, pc, nv, tex, cw, ze, zf, zwrite, blend, src, dst, atest ? " atest" : "");
            if (tex) tex->lpVtbl->Release(tex);
            g_orderModels.push_back(t);
            if (g_orderModels.size() > 30)
                g_orderModels.erase(g_orderModels.begin());
        }
        if ((k == 'B' || k == 'W') && !g_vshader && ++g_orderBCount <= 20)
        {
            DWORD src = 0, dst = 0, fvf = 0, fog = 0;
            dev->lpVtbl->GetRenderState(dev, D3DRS_SRCBLEND, &src);
            dev->lpVtbl->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
            dev->lpVtbl->GetRenderState(dev, D3DRS_FOGENABLE, &fog);
            dev->lpVtbl->GetFVF(dev, &fvf);
            IDirect3DBaseTexture9* tex = nullptr;
            dev->lpVtbl->GetTexture(dev, 0, &tex);
            IDirect3DPixelShader9* ps = nullptr;
            dev->lpVtbl->GetPixelShader(dev, &ps);
            char t[260];
            snprintf(t, sizeof(t), " [#%u %c %s %up %uv%s ps %p tex %p fvf 0x%X blend %u/%u%s%s world (%.0f %.0f %.0f)]",
                     g_orderIndex, k, call, pc, nv, g_vshader ? " vs" : " ff", ps, tex, fvf, src, dst, fog ? " fog" : "",
                     atest ? " atest" : "",
                     g_world.m[3][0], g_world.m[3][1], g_world.m[3][2]);
            g_orderBlended += t;
            if (tex) tex->lpVtbl->Release(tex);
            if (ps) ps->lpVtbl->Release(ps);
        }
        if (zwrite && g_orderFirstB)
        {
            ++g_orderOAfterB;
            if (g_orderOAfterB <= 12)
            {
                char t[120];
                snprintf(t, sizeof(t), " [#%u %s %up %uv%s%s%s]", g_orderIndex, call, pc, nv, g_vshader ? " vs" : " ff",
                         atest ? " atest" : "", blend ? " blend" : "");
                g_orderAfter += t;
            }
        }
        if (k != g_orderLast)
        {
            OrderFlush();
            g_orderLast = k;
        }
        ++g_orderRun;
    }
    bool               g_volumePending = false; // the map is ready; the light waits for the first UI draw

    // The world has finished drawing: depth and the shadow map go in now, with the world's render target
    // and depth buffer still bound. The light itself waits for the rays' place before the UI (FireRays):
    // drawn here, it went through the client's Full Screen Glow, which blooms a small copy of the screen,
    // and the light through the leaves was too fine for it. The bloom came out different at every step, and
    // the pattern of light through a tree re-formed as you walked.
    void WorldEnded(IDirect3DDevice9* dev, const char* why)
    {
        if (g_worldEnded)
            return;
        g_worldEnded = true;
        g_inPass = true;
        g_seeThroughLast = g_seeThroughDraws;
        g_seeThroughDraws = 0;
        g_depthOnlyLast = g_depthOnlySkipped;
        g_depthOnlySkipped = 0;
        if (g_orderRec)
        {
            OrderFlush();
            Log("order: the world's %u draws, the first blended one at #%u, %u depth-writing draws after it. Runs "
                "(O depth writes, B blended, W both, N neither): %s", g_orderIndex, g_orderFirstB, g_orderOAfterB,
                g_orderRuns.c_str());
            if (!g_orderAfter.empty())
                Log("order: the first depth-writing draws after it:%s", g_orderAfter.c_str());
            if (!g_orderBlended.empty())
                Log("order: %u blended fixed-function draws (B no depth writes, W with):%s", g_orderBCount,
                    g_orderBlended.c_str());
            for (const std::string& m : g_orderModels)
                Log("order: model draw %s", m.c_str());
            g_orderRec = false;
        }
        if (g_orderArm)
        {
            g_orderArm = false;
            g_orderRec = true;
            g_orderRuns.clear();
            g_orderAfter.clear();
            g_orderBlended.clear();
            g_orderModels.clear();
            g_orderBCount = 0;
            g_orderLast = 0;
            g_orderRun = g_orderIndex = g_orderFirstB = g_orderOAfterB = 0;
        }
        BodyMarkWorldEnded(dev);                // the body mask, from the stencil, before anything is rebound
        DepthWorldEnded(dev, VolumeActive());   // a multisampled depth buffer is resolved only for the light
        if (VolumeActive())          // the map costs more than the light does; it is only for the light
        {
            BenchSectionBegin(dev, kBenchShadow);
            ShadowWorldEnded(dev);
            BenchSectionEnd(dev, kBenchShadow, true);
            g_volumePending = true;
        }
        else
        {
            ShadowNoReplay();        // so the cost report does not keep showing the last one
        }
        LampsWorldEnded();
        g_inPass = false;
        if (g_probe.active)
        {
            Log("  [draw %4u] WORLD END      %s", g_probe.draws, why);
            g_probe.boundary = g_probe.draws;
        }
    }

    // Rays placement. hkSetTransform finds where the world ends (the first switch to a non-perspective
    // projection), but that switch only ARMS the pass. With Full Screen Glow on (ffxGlow, the default) the
    // world is drawn into an off-screen texture, not the back buffer; after the switch the client
    // downsamples and blurs it, then draws world + glow onto the back buffer in one full-screen,
    // pixel-shaded, unblended draw that replaces whatever was there. Rays drawn at the switch read a back
    // buffer holding no world yet and were then painted over. So the pass fires immediately before the
    // first draw that goes to the back buffer with no pixel shader bound: the first UI draw, with glow on
    // or off. A probe showed both:
    //
    //   glow on:  world -> RT A | switch | glow passes into small RTs | composite to BB (ps) | UI (no ps)
    //   glow off: world -> BB   | switch | UI (no ps)
    bool               g_raysArmed = false;
    bool               g_raysDone  = false;
    IDirect3DSurface9* g_bbArmed   = nullptr;   // the back buffer when armed; compared, never dereferenced
    // Where the passes ran in ordinary frames since the last probe (2026-09-30): with the chat hidden they
    // stopped showing, and a probe frame showed them drawn. Logged when a probe opens.
    uint32_t g_placeFrames = 0, g_placeUi = 0, g_placePresent = 0, g_placeUnarmed = 0, g_placeNoWorld = 0;

    void FireRays(IDirect3DDevice9* dev, const char* where)
    {
        g_raysArmed = false;
        g_raysDone  = true;
        g_inPass = true;
        // The whole back buffer (2026-09-30). The passes run just before the first UI draw, with the state the
        // client set for it, viewport included. With the chat hidden (/togglechat) that first draw is the
        // player's portrait, and its viewport is the portrait's box: every pass was drawn inside it and none
        // showed on screen. A probe did not see it, since its read-backs set a target of their own.
        D3DVIEWPORT9 oldVp = {};
        const bool haveVp = SUCCEEDED(dev->lpVtbl->GetViewport(dev, &oldVp));
        {
            IDirect3DSurface9* rt = nullptr;
            dev->lpVtbl->GetRenderTarget(dev, 0, &rt);
            if (rt)
            {
                D3DSURFACE_DESC rd = {};
                rt->lpVtbl->GetDesc(rt, &rd);
                const D3DVIEWPORT9 full = { 0, 0, rd.Width, rd.Height, 0.0f, 1.0f };
                if (g_probe.active && haveVp)
                    Log("  [draw %4u] VIEWPORT       the client's (%lu %lu %lu x %lu), the target %u x %u",
                        g_probe.draws, oldVp.X, oldVp.Y, oldVp.Width, oldVp.Height, rd.Width, rd.Height);
                dev->lpVtbl->SetViewport(dev, &full);
                rt->lpVtbl->Release(rt);
            }
        }
        if (g_volumePending)
        {
            // The light first: the rays streak what is bright on screen, and the light was part of that
            // when it was drawn with the world.
            g_volumePending = false;
            // The shade first: it darkens surfaces, and the light in the air goes over it.
            BenchSectionBegin(dev, kBenchSunShadows);
            const bool shaded = SunShadowsDraw(dev);
            // A sun shadow debug view is shown alone: fog, light and lamps drawn over it made every
            // object a grey shape by its depth, which read as part of the view.
            const bool shadowDebug = shaded && g_cfg.sunShadows.debug != 0;
            BenchSectionEnd(dev, kBenchSunShadows, shaded);
            BenchSectionBegin(dev, kBenchVolume);
            const bool drawn = !shadowDebug && VolumeDraw(dev);
            BenchSectionEnd(dev, kBenchVolume, drawn);
            // The fog around lamps, which reads the same depth and camera, and draws at night too.
            BenchSectionBegin(dev, kBenchLamps);
            const bool lamps = !shadowDebug && LampGlowDraw(dev);
            BenchSectionEnd(dev, kBenchLamps, lamps);
        }
        BenchSectionBegin(dev, kBenchRays);
        const bool ran = RaysBeforeUI(dev);
        BenchSectionEnd(dev, kBenchRays, ran);
        if (haveVp)
            dev->lpVtbl->SetViewport(dev, &oldVp);
        g_inPass = false;
        if (ran && g_probe.active)
            Log("  [draw %4u] RAYS PASS      %s", g_probe.draws, where);
        if (g_probe.active)
        {
            Log("  [draw %4u] PASSES DONE    %s", g_probe.draws, where);
            g_probe.afterPass = true;
        }
    }

    // Called before every draw is forwarded; does nothing unless armed.
    void MaybeFireRays(IDirect3DDevice9* dev)
    {
        if (!g_raysArmed || g_inPass)
            return;
        IDirect3DSurface9*     rt = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        dev->lpVtbl->GetRenderTarget(dev, 0, &rt);
        dev->lpVtbl->GetPixelShader(dev, &ps);
        const bool firstUiDraw = rt && rt == g_bbArmed && !ps;
        if (rt) rt->lpVtbl->Release(rt);
        if (ps) ps->lpVtbl->Release(ps);
        if (firstUiDraw)
            FireRays(dev, "before the first UI draw");
    }

    // The game's fog as the client set it last.
    void LogFog(const char* why)
    {
        Log("--- %s: the game's fog: start %.1f, end %.1f, colour 0x%06X ---", why, D2F(g_fog.start),
            D2F(g_fog.end), g_fog.color & 0xFFFFFF);
    }

    // Keys only count while the client has focus, so typing F11 into another window does nothing here.
    bool ClientFocused()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    void PollKeys(IDirect3DDevice9* dev)
    {
        const bool focused = ClientFocused();

        const bool reload = focused && (GetAsyncKeyState(g_cfg.reloadKey) & 0x8000) != 0;
        if (reload && !g_reloadDown)
        {
            // Any of these changes what the benchmark measures. A plain reload rebuilds the settings from
            // the ini, so the benchmark has nothing to put back.
            const bool plain = !(GetAsyncKeyState(VK_MENU) & 0x8000) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000);
            BenchCancel("F11", !plain);
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
            {
                RaysToggle();
            }
            else if (GetAsyncKeyState(VK_MENU) & 0x8000)
            {
                VolumeToggle();
            }
            else
            {
                LoadSettings(g_iniPath);
                CVarsAfterLoad();
                RaysReload();
                LogFog("reloaded");
            }
        }
        g_reloadDown = reload;

        const bool probe = focused && (GetAsyncKeyState(g_cfg.probeKey) & 0x8000) != 0;
        // Ctrl+F12 belongs to comfytime's clock search. Alt+F12 runs the benchmark; a plain press takes a
        // probe.
        if (probe && !g_probeDown && !(GetAsyncKeyState(VK_CONTROL) & 0x8000))
        {
            if (GetAsyncKeyState(VK_MENU) & 0x8000)
            {
                BenchStart(dev);
            }
            else
            {
                g_probe.armed = true;
                ReportStart("F12");
                CVarsNotice("Probe taken. The frame goes to comfyfog.log. Stand still for a second while the lamps are logged.");
            }
        }
        g_probeDown = probe;
        if (g_benchArmed)
        {
            g_benchArmed = false;
            BenchStart(dev);
        }
    }

    // The client can let its device go and make a new one, with no Reset: measured on 2026-09-25, changing
    // the resolution between 1080p and 1440p did it, with no `device reset` line. The hooks sit in DXVK's
    // shared vtable, so they carry on for the new device, but everything made on the old one stayed in use:
    // the shadow cache's buffers, the depth stand-ins, the light's targets and the state blocks. Used on the
    // new device, they drew the font texture over the whole screen, and in one run WoW stopped with
    // ERROR #124 (SMem3: pointer does not refer to a valid allocated block). All of it is released here,
    // as a Reset releases it. The objects we hold keep the old device alive until then, so its address
    // cannot come back as the new one's.
    IDirect3DDevice9* g_seenDev = nullptr;   // compared only; not referenced

    void CheckDevice(IDirect3DDevice9* dev)
    {
        if (dev == g_seenDev)
            return;
        if (g_seenDev)
        {
            Log("device changed from %p to %p: everything made on the old one is released", g_seenDev, dev);
            DepthReset(nullptr);   // nothing is called on the old device
            ShadowReset();
            VolumeReset();
            RaysReset();
            CoverReset();
            BenchReset();
            LampsReset();
            LampGlowReset();
            SunShadowsReset();
            BodyMaskReset();
            TerrainShadeReset();
            MapTerrainRelease();
            g_waterPs.clear();
            g_fog       = ClientFog();
            g_haveWorldFog = false;
            g_haveWorldFogColor = false;
            g_fogEnable = 0;
            g_vshader   = nullptr;
            g_worldEnded     = false;
            g_volumePending  = false;
            g_raysArmed      = false;
            g_raysDone       = false;
        }
        g_seenDev = dev;
        ReportDevice(dev);
    }

    // The first call of a frame's rendering: the readable depth buffer goes in, and the shadow
    // recording opens.
    HRESULT STDMETHODCALLTYPE hkBeginScene(IDirect3DDevice9* dev)
    {
        CheckDevice(dev);
        if (!g_inPass)
        {
            // [depth] seeThroughNear: the camera this near your character means the client may be fading it.
            // On a ship the player is not read (ClientPlayer), so this is left off, as before.
            float cam[3], pl[3];
            bool onShip = false;
            g_ownDist = ClientCamera(cam) && ClientPlayer(pl, &onShip) && !onShip
                ? sqrtf((cam[0] - pl[0]) * (cam[0] - pl[0]) + (cam[1] - pl[1]) * (cam[1] - pl[1]) +
                        (cam[2] - pl[2]) * (cam[2] - pl[2]))
                : 1e9f;
            g_ownFaded = g_ownDist < g_cfg.depth.seeThroughNear;
            DepthBeginScene(dev);
            if (!g_worldEnded)
            {
                ShadowSetPhase(VolumeActive());
                LampsSetTracking(LampGlowWantsLights());
            }
        }
        return g_oBeginScene(dev);
    }

    // The client binding a depth buffer: hand the device our readable stand-in instead (depth.cpp).
    HRESULT STDMETHODCALLTYPE hkSetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9* s)
    {
        CheckDevice(dev);
        return g_oSetDS(dev, g_inPass ? s : DepthSubstitute(dev, s));
    }

    HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst,
                                        HWND wnd, const RGNDATA* dirty)
    {
        CheckDevice(dev);
        // Between captures, so the pass's own draws and state changes never show up in a probe.
        // Armed but never fired: nothing was drawn to the back buffer after the world (UI hidden, say),
        // so the finished frame is exactly the world and the pass can run here.
        ++g_placeFrames;
        if (g_raysDone)
            ++g_placeUi;
        else if (g_raysArmed)
            ++g_placePresent;
        else if (g_worldEnded)
            ++g_placeUnarmed;
        else
            ++g_placeNoWorld;
        if (g_raysArmed)
            FireRays(dev, "at Present (no UI draw followed)");
        g_inPass = true;
        if (g_cfg.rays.placement == 1)             // over the finished frame, UI included
        {
            BenchSectionBegin(dev, kBenchRays);
            const bool drawn = RaysPresent(dev);
            BenchSectionEnd(dev, kBenchRays, drawn);
        }
        else
        {
            RaysPresent(dev);                      // ends the frame for the rays
        }
        g_inPass = false;
        g_raysArmed = false;
        g_raysDone  = false;
        g_volumePending = false;     // the rays never armed: no place before the UI was found this frame

        if (g_probe.active)
        {
            Log("  after our passes: %u draws, %u of them unblended or pixel-shaded (listed as LATE)",
                g_probe.lateDraws, g_probe.lateListed);
            Log("--- end frame %llu: %u draws, %u fogged (%u of those through a vertex shader), "
                "%u fog state sets, %u vs constant uploads ---",
                g_frame, g_probe.draws, g_probe.fogged, g_probe.foggedVs, g_probe.fogSets, g_probe.constSets);
            g_probe = Probe();
        }

        // What the whole thing costs, measured on ordinary frames: the probe's own readbacks make its
        // numbers meaningless, and a mod that hitches is worse than one that does nothing.
        {
            static double last = 0.0, worstFrame = 0.0, sumFrame = 0.0, sumShadow = 0.0, worstShadow = 0.0;
            static unsigned frames = 0, sumDrawn = 0, sumSkipped = 0;
            const double now = Now();
            BenchFrame(dev, last > 0.0 ? now - last : 0.0);
            if (last > 0.0)
            {
                const double dt = now - last;
                sumFrame += dt;
                if (dt > worstFrame) worstFrame = dt;
                unsigned drawn = 0, skipped = 0;
                const double sh = ShadowReplaySeconds(drawn, skipped);
                sumShadow += sh;
                if (sh > worstShadow) worstShadow = sh;
                sumDrawn += drawn;
                sumSkipped += skipped;
                // Only with [general] trace = 1: every write opens and closes comfyfog.log, and in normal
                // play the benchmark (Alt+F12) gives the same numbers when they are wanted.
                if (++frames >= 300 && g_cfg.trace)
                {
                    unsigned copyFailed = 0;
                    const unsigned copies = ShadowCopies(copyFailed);
                    Log("cost: %.1f fps (%.2f ms/frame, worst %.2f), shadow replay %.2f ms (worst %.2f), "
                        "%u casters drawn, %u outside the map, %u shader registers each, %u copied chunks (%u failed)",
                        frames / sumFrame, 1000.0 * sumFrame / frames, 1000.0 * worstFrame,
                        1000.0 * sumShadow / frames, 1000.0 * worstShadow, sumDrawn / frames,
                        sumSkipped / frames, g_maxConstReg, copies, copyFailed);
                }
                if (frames >= 300)
                {
                    frames = 0; sumFrame = sumShadow = worstFrame = worstShadow = 0.0;
                    sumDrawn = sumSkipped = 0;
                }
            }
            last = now;
        }
        ShadowFrameEnd();
        VolumeFrameEnd();
        LampsFrameEnd();
        BodyMarkFrameEnd(dev);
        g_frameDraws = 0;
        g_lastPersp  = false;
        g_worldEnded = false;
        g_skyPhase  = true;
        SunFrameStart();

        PollKeys(dev);
        // The on-screen stats (/atmos stats), once a second: the frame rate, the shadow cache, the fog.
        {
            static double statsLast = 0.0;
            static unsigned statsFrames = 0;
            ++statsFrames;
            const double now = Now();
            if (statsLast == 0.0)
                statsLast = now;
            if (now - statsLast >= 1.0)
            {
                // As name=value; pairs, which the addon lays out (ComfyAtmosphere.lua, StatsBuild).
                char line[200];
                float pl[3] = {};
                char map[64] = "";
                const bool havePl = ClientPlayer(pl);
                ClientMapName(map, sizeof(map));
                snprintf(line, sizeof(line), "fps=%.0f;x=%.1f;y=%.1f;z=%.1f;pos=%d;map=%s;", statsFrames / (now - statsLast),
                         pl[0], pl[1], pl[2], havePl ? 1 : 0, map);
                std::string text = line;
                ShadowStatsText(text);
                VolumeStatsText(text);
                CVarsStats(text);
                statsLast = now;
                statsFrames = 0;
            }
        }
        if (CVarsPoll())
            BenchCancel("a control in Video > Atmosphere moved", false);

        g_frame++;
        if (g_probe.armed)
        {
            g_probe = Probe();
            g_probe.active = true;
            LogFog("probe");
            Log("passes: %u frames since the last probe: before the UI in %u, at Present in %u, world ended but "
                "never armed in %u, no world end found in %u", g_placeFrames, g_placeUi, g_placePresent,
                g_placeUnarmed, g_placeNoWorld);
            g_placeFrames = g_placeUi = g_placePresent = g_placeUnarmed = g_placeNoWorld = 0;
            float hour = 0.0f;
            if (ClientHour(hour))
                Log("night: game time %02d:%02d, night %.2f, rays and light x %.2f ([night] strength %.0f)",
                    static_cast<int>(hour), static_cast<int>(hour * 60.0f) % 60, NightWeight(hour), NightScale(),
                    g_cfg.night.strength);
            else
                Log("night: no game clock at [client] clockAddr, so the rays and the light keep their day strength");
            Log("depth: last frame, %u see-through model draws had their depth writes turned off and %u depth-only "
                "model passes were skipped ([depth] seeThrough %d); the camera %.1f yd from you, so %s",
                g_seeThroughLast, g_depthOnlyLast, g_cfg.depth.seeThrough ? 1 : 0, g_ownDist,
                g_ownFaded ? "off (your character may be faded)" : "on");
            DepthProbe();
            ShadowProbe();
            VolumeProbe();
            CoverProbe();
            LampsProbe(dev);
            LampGlowProbe();
            SunShadowsProbe();
            IDirect3DSurface9* bb = nullptr;
            if (SUCCEEDED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
            {
                Log("--- back buffer is %p ---", bb);
                bb->lpVtbl->Release(bb);
            }
            Log("--- begin frame %llu capture ---", g_frame);
        }
        return g_oPresent(dev, src, dst, wnd, dirty);
    }

    // Reset puts every render state back to its default, and the client sets what it needs again
    // afterwards, so the mirror goes back to defaults too, rather than re-pushing stale values.
    HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
    {
        CheckDevice(dev);
        DepthReset(dev);   // Reset fails outright while any D3DPOOL_DEFAULT object is alive
        ShadowReset();
        VolumeReset();
        RaysReset();
        CoverReset();
        BenchReset();
        LampsReset();
        LampGlowReset();
        SunShadowsReset();
        BodyMaskReset();
        TerrainShadeReset();
        const HRESULT hr = g_oReset(dev, pp);
        if (SUCCEEDED(hr))
        {
            g_fog      = ClientFog();
            g_haveWorldFog = false;
            g_haveWorldFogColor = false;
            g_fogEnable = 0;
            g_vshader  = nullptr;
            Log("device reset: fog mirror cleared");
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkSetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE st, DWORD value)
    {
        switch (st)
        {
        case D3DRS_FOGSTART:
            g_fog.start = value; g_fog.haveStart = true;
            break;

        case D3DRS_FOGEND:
            g_fog.end = value; g_fog.haveEnd = true;
            if (!g_worldEnded && D2F(value) > 2.0f)
            {
                g_worldFogEnd   = value;
                g_worldFogStart = g_fog.start;
                g_haveWorldFog  = true;
            }
            break;

        case D3DRS_FOGDENSITY:
            g_fog.density = value; g_fog.haveDensity = true;
            break;

        case D3DRS_FOGCOLOR:
            g_fog.color = value; g_fog.haveColor = true;
            if (!g_worldEnded && (value & 0xFFFFFF) != 0)
            {
                g_worldFogColor = value;
                g_haveWorldFogColor = true;
            }
            break;

        case D3DRS_FOGENABLE:
            g_fogEnable = value;
            break;

        case D3DRS_ZENABLE:
            if (g_probe.active)
                Log("  [draw %4u] ZENABLE        %u", g_probe.draws, value);
            break;

        default:
            break;
        }
        NoteFogState(st, value, value);
        return g_oSetRS(dev, st, value);
    }

    // Where the world ends and the UI begins. A probe of this client shows the world drawn under a
    // perspective projection, then (in the same call sequence that parks fog at start 0 / end 1 /
    // magenta) the first switch to an orthographic one, after which it is all UI. With Full Screen Glow
    // off, that first perspective -> non-perspective switch, with some world already drawn, is where the
    // world ends; with it on, hkSetRenderTarget sees the end first. The draw minimum skips a flip the
    // client makes at the top of the frame, before anything is drawn.
    HRESULT STDMETHODCALLTYPE hkSetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE st, const D3DMATRIX* m)
    {
        CheckDevice(dev);
        if (g_inPass)
            return g_oSetTransform(dev, st, m);   // our passes' own transforms: not the client's camera
        SunSetTransform(st, m);
        if (st == D3DTS_WORLD && m)
            g_world = *m;
        if (st == D3DTS_VIEW && m)
            g_viewAll = *m;
        if (st == D3DTS_PROJECTION && m)
            g_projAll = *m;
        if (st == D3DTS_PROJECTION && m)
        {
            const bool persp = fabsf(m->m[2][3] - 1.0f) < 1e-3f && fabsf(m->m[3][3]) < 1e-3f;
            if (g_probe.active && persp != g_lastPersp)
                Log("  [draw %4u] PROJECTION     %s (m33=%.3f)", g_probe.draws,
                    persp ? "perspective" : "NOT perspective", m->m[3][3]);

            if (!persp && g_lastPersp && g_frameDraws >= static_cast<uint32_t>(g_cfg.minWorldDraws))
            {
                WorldEnded(dev, "switch to 2D");   // glow off: the world ends here instead
                // The rays wait for the first UI draw (see FireRays).
                if (!g_raysArmed && !g_raysDone)
                {
                    IDirect3DSurface9* bb = nullptr;
                    if (SUCCEEDED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
                    {
                        g_bbArmed   = bb;
                        g_raysArmed = true;
                        bb->lpVtbl->Release(bb);
                        if (g_probe.active)
                            Log("  [draw %4u] RAYS ARMED     world -> UI switch", g_probe.draws);
                    }
                }
            }
            g_lastPersp = persp;
        }
        return g_oSetTransform(dev, st, m);
    }

    // The terrain's shader, swapped for a copy with less of its baked shadow while the sun shadows draw
    // (terrainshade.cpp). What is kept follows the sun shadows' share, so it is back in full at dusk.
    HRESULT STDMETHODCALLTYPE hkSetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* sh)
    {
        if (!g_inPass && sh)
        {
            const float share = g_cfg.master ? SunShadowsShare() : 0.0f;
            const float keep  = 1.0f - (1.0f - g_cfg.sunShadows.baked) * share;
            sh = TerrainShadeSwap(dev, sh, keep);
        }
        return g_oSetPS(dev, sh);
    }

    HRESULT STDMETHODCALLTYPE hkSetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* sh)
    {
        g_vshader = sh;
        DumpShader(sh);
        return g_oSetVS(dev, sh);
    }

    // Around the world -> UI boundary the probe logs every draw in detail: which render target it lands
    // on, what it samples, and how it blends. That is what shows what the client's glow does after the
    // world ends.
    // Two windows get the detail: the first draws of the frame, where the sky (and the sun disc in it)
    // is drawn, and the draws around the world -> UI boundary. For the sky window each line also carries
    // where the draw sits relative to the camera: this client renders camera-relative, so a sky object's
    // world translation (or, for a draw with an identity world, its first vertex) is a direction out from
    // the camera, logged as azimuth/elevation, the same frame the sun direction is logged in.
    void SkyPosition(IDirect3DDevice9* dev, bool indexed, UINT first, UINT nv, const void* upData, char* out, size_t cap)
    {
        float p[3] = { g_world.m[3][0], g_world.m[3][1], g_world.m[3][2] };
        const char* src = "world";
        if (p[0] * p[0] + p[1] * p[1] + p[2] * p[2] < 1e-6f && nv && nv <= 64)
        {
            // Identity world: the position is in the vertices. Position is at offset 0 in every layout
            // this client uses (see comfygrass's probe); read one vertex, probe frames only.
            float v[3] = {};
            bool ok = false;
            if (upData)
            {
                memcpy(v, upData, sizeof(v));
                ok = true;
            }
            else
            {
                IDirect3DVertexBuffer9* vb = nullptr;
                UINT off = 0, stride = 0;
                if (SUCCEEDED(dev->lpVtbl->GetStreamSource(dev, 0, &vb, &off, &stride)) && vb && stride >= 12)
                {
                    void* ptr = nullptr;
                    if (SUCCEEDED(vb->lpVtbl->Lock(vb, off + first * stride, 12, &ptr, D3DLOCK_READONLY)) && ptr)
                    {
                        memcpy(v, ptr, sizeof(v));
                        vb->lpVtbl->Unlock(vb);
                        ok = true;
                    }
                }
                if (vb) vb->lpVtbl->Release(vb);
            }
            (void)indexed;
            if (ok)
            {
                p[0] = v[0]; p[1] = v[1]; p[2] = v[2];
                src = "vert0";
            }
        }
        const float len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (len < 1e-3f)
        {
            _snprintf_s(out, cap, _TRUNCATE, " at origin");
            return;
        }
        const float az = atan2f(p[1], p[0]) * 57.29578f;
        const float el = asinf(p[2] / len) * 57.29578f;
        _snprintf_s(out, cap, _TRUNCATE, " %s=(%.1f %.1f %.1f) dist=%.1f az=%.1f el=%.1f",
                    src, p[0], p[1], p[2], len, az, el);
    }

    // Row vector times matrix, D3D9's convention.
    void Mul4(const float in[4], const D3DMATRIX& m, float out[4])
    {
        for (int c = 0; c < 4; ++c)
            out[c] = in[0] * m.m[0][c] + in[1] * m.m[1][c] + in[2] * m.m[2][c] + in[3] * m.m[3][c];
    }

    // For the handful of tiny draws at the top of the frame (the sky's sprites): every vertex, the
    // matrices, and where the draw's centre lands on screen through the client's own transforms. The
    // sun disc is whichever of these lands where the sun is seen.
    void DumpSkyDraw(IDirect3DDevice9* dev, UINT first, UINT nv, const void* upData)
    {
        float verts[8][3] = {};
        UINT  stride = 0;
        bool  ok = false;
        if (upData)
        {
            // UP draws carry their own stride; not known here, so only the first vertex is trusted.
            memcpy(verts[0], upData, 12);
            nv = 1;
            ok = true;
        }
        else
        {
            IDirect3DVertexBuffer9* vb = nullptr;
            UINT off = 0;
            if (SUCCEEDED(dev->lpVtbl->GetStreamSource(dev, 0, &vb, &off, &stride)) && vb && stride >= 12)
            {
                void* ptr = nullptr;
                if (SUCCEEDED(vb->lpVtbl->Lock(vb, off + first * stride, nv * stride, &ptr, D3DLOCK_READONLY)) && ptr)
                {
                    for (UINT i = 0; i < nv; ++i)
                        memcpy(verts[i], static_cast<const uint8_t*>(ptr) + i * stride, 12);
                    vb->lpVtbl->Unlock(vb);
                    ok = true;
                }
            }
            if (vb) vb->lpVtbl->Release(vb);
        }
        if (!ok)
        {
            Log("        (vertices unreadable)");
            return;
        }

        float c[4] = { 0, 0, 0, 1 };
        for (UINT i = 0; i < nv; ++i)
        {
            Log("        v%u = (%9.3f %9.3f %9.3f)  stride=%u", i, verts[i][0], verts[i][1], verts[i][2], stride);
            c[0] += verts[i][0] / nv; c[1] += verts[i][1] / nv; c[2] += verts[i][2] / nv;
        }
        const D3DMATRIX* mats[3]  = { &g_world, &g_viewAll, &g_projAll };
        const char*      names[3] = { "world", "view", "proj" };
        for (int mi = 0; mi < 3; ++mi)
            for (int r = 0; r < 4; ++r)
                Log("        %s[%d] = %9.4f %9.4f %9.4f %9.4f", names[mi], r,
                    mats[mi]->m[r][0], mats[mi]->m[r][1], mats[mi]->m[r][2], mats[mi]->m[r][3]);

        float w[4], v[4], p[4];
        Mul4(c, g_world, w);
        Mul4(w, g_viewAll, v);
        Mul4(v, g_projAll, p);
        const float wl = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (p[3] > 1e-5f)
            Log("        centre: world (%.3f %.3f %.3f) az=%.1f el=%.1f -> view (%.3f %.3f %.3f) -> screen uv (%.3f, %.3f)",
                w[0], w[1], w[2], wl > 1e-5f ? atan2f(w[1], w[0]) * 57.29578f : 0.0f,
                wl > 1e-5f ? asinf(w[2] / wl) * 57.29578f : 0.0f, v[0], v[1], v[2],
                p[0] / p[3] * 0.5f + 0.5f, -p[1] / p[3] * 0.5f + 0.5f);
        else
            Log("        centre: world (%.3f %.3f %.3f), behind the camera (w=%.4f)", w[0], w[1], w[2], p[3]);
    }

    void DetailDraw(IDirect3DDevice9* dev, const char* kind, D3DPRIMITIVETYPE prim, UINT pc,
                    bool indexed, UINT first, UINT nv, const void* upData)
    {
        if (g_inPass)
            return;
        const bool early    = g_probe.draws < 300;
        const bool boundary = g_probe.boundary != 0xFFFFFFFF && g_probe.draws <= g_probe.boundary + 40;
        if (!early && !boundary)
        {
            if (!g_probe.afterPass)
                return;
            ++g_probe.lateDraws;
            IDirect3DSurface9*     lrt = nullptr;
            IDirect3DPixelShader9* lps = nullptr;
            IDirect3DBaseTexture9* ltex = nullptr;
            DWORD lblend = 0, lcw = 0;
            dev->lpVtbl->GetRenderTarget(dev, 0, &lrt);
            dev->lpVtbl->GetPixelShader(dev, &lps);
            dev->lpVtbl->GetTexture(dev, 0, &ltex);
            dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &lblend);
            dev->lpVtbl->GetRenderState(dev, D3DRS_COLORWRITEENABLE, &lcw);
            if ((!lblend || lps) && lcw && g_probe.lateListed < 60)
            {
                ++g_probe.lateListed;
                UINT w = 0, h = 0;
                if (ltex && ltex->lpVtbl->GetType(ltex) == D3DRTYPE_TEXTURE)
                {
                    D3DSURFACE_DESC td = {};
                    auto* t2 = reinterpret_cast<IDirect3DTexture9*>(ltex);
                    if (SUCCEEDED(t2->lpVtbl->GetLevelDesc(t2, 0, &td)))
                        w = td.Width, h = td.Height;
                }
                DWORD fvf = 0;
                dev->lpVtbl->GetFVF(dev, &fvf);
                Log("  [draw %4u] LATE %-15s prims=%u verts=%u tex0=%p %ux%u ps=%p vs=%p blend=%u cw=0x%X fvf=0x%X rt=%p",
                    g_probe.draws, kind, pc, nv, ltex, w, h, lps, g_vshader, lblend, lcw, fvf, lrt);
            }
            if (lrt) lrt->lpVtbl->Release(lrt);
            if (lps) lps->lpVtbl->Release(lps);
            if (ltex) ltex->lpVtbl->Release(ltex);
            return;
        }
        IDirect3DSurface9*     rt  = nullptr;
        IDirect3DBaseTexture9* tex = nullptr;
        IDirect3DPixelShader9* ps  = nullptr;
        DWORD blend = 0, src = 0, dst = 0, zen = 0;
        auto* d = dev->lpVtbl;
        d->GetRenderTarget(dev, 0, &rt);
        d->GetTexture(dev, 0, &tex);
        d->GetPixelShader(dev, &ps);
        d->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
        d->GetRenderState(dev, D3DRS_SRCBLEND, &src);
        d->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
        d->GetRenderState(dev, D3DRS_ZENABLE, &zen);
        DWORD zwrite = 0;
        d->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zwrite);
        char where[128] = "";
        if (early)
            SkyPosition(dev, indexed, first, nv, upData, where, sizeof(where));
        Log("  [draw %4u] %-15s prim=%d prims=%u verts=%u tex0=%p ps=%p vs=%p blend=%u src=%u dst=%u z=%u zw=%u rt=%p%s",
            g_probe.draws, kind, static_cast<int>(prim), pc, nv, tex, ps, g_vshader, blend, src, dst, zen, zwrite,
            rt, where);
        if (early && g_probe.draws < 32 && nv && nv <= 8)
            DumpSkyDraw(dev, first, nv, upData);
        // Each texture stage: size, format, and how it combines. For finding the terrain's baked shadow,
        // which is a small texture of its own (2026-09-29).
        {
            char stages[512] = "";
            size_t at = 0;
            DWORD fvf = 0;
            d->GetFVF(dev, &fvf);
            for (DWORD st = 0; st < 4; ++st)
            {
                IDirect3DBaseTexture9* t = nullptr;
                d->GetTexture(dev, st, &t);
                DWORD cop = 0, aop = 0, carg1 = 0, carg2 = 0;
                d->GetTextureStageState(dev, st, D3DTSS_COLOROP, &cop);
                d->GetTextureStageState(dev, st, D3DTSS_ALPHAOP, &aop);
                d->GetTextureStageState(dev, st, D3DTSS_COLORARG1, &carg1);
                d->GetTextureStageState(dev, st, D3DTSS_COLORARG2, &carg2);
                unsigned w = 0, h = 0, fmt = 0;
                if (t && t->lpVtbl->GetType(t) == D3DRTYPE_TEXTURE)
                {
                    D3DSURFACE_DESC td = {};
                    auto* t2 = reinterpret_cast<IDirect3DTexture9*>(t);
                    if (SUCCEEDED(t2->lpVtbl->GetLevelDesc(t2, 0, &td)))
                    {
                        w = td.Width; h = td.Height; fmt = static_cast<unsigned>(td.Format);
                    }
                }
                const int n = snprintf(stages + at, sizeof(stages) - at, " s%lu=%p %ux%u f%u cop%lu(%lu,%lu) aop%lu",
                                       st, t, w, h, fmt, cop, carg1, carg2, aop);
                if (n > 0 && at + n < sizeof(stages))
                    at += n;
                if (t) t->lpVtbl->Release(t);
            }
            Log("        fvf=0x%lX%s", fvf, stages);
        }
        // The pixel shader's bytecode, once each, next to the log: comfyfog_ps_<address>.bin.
        if (ps)
        {
            static void* dumped[32] = {};
            bool seen = false;
            int  free = -1;
            for (int i = 0; i < 32; ++i)
            {
                seen = seen || dumped[i] == ps;
                if (!dumped[i] && free < 0)
                    free = i;
            }
            UINT size = 0;
            if (!seen && free >= 0 && SUCCEEDED(ps->lpVtbl->GetFunction(ps, nullptr, &size)) && size > 0 &&
                size < 65536)
            {
                dumped[free] = ps;
                std::vector<unsigned char> code(size);
                if (SUCCEEDED(ps->lpVtbl->GetFunction(ps, code.data(), &size)))
                {
                    wchar_t path[MAX_PATH];
                    wcscpy_s(path, g_logPath);
                    wchar_t* slash = wcsrchr(path, L'\\');
                    if (slash)
                        swprintf(slash + 1, MAX_PATH - (slash + 1 - path), L"comfyfog_ps_%p.bin", ps);
                    FILE* f = nullptr;
                    if (!_wfopen_s(&f, path, L"wb") && f)
                    {
                        fwrite(code.data(), 1, size, f);
                        fclose(f);
                        Log("        ps %p: %u bytes written to comfyfog_ps_%p.bin", ps, size, ps);
                    }
                }
            }
        }
        if (rt)  rt->lpVtbl->Release(rt);
        if (tex) tex->lpVtbl->Release(tex);
        if (ps)  ps->lpVtbl->Release(ps);
    }

    HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* s)
    {
        CheckDevice(dev);
        // With Full Screen Glow the world is drawn into its own render target, and the first switch away
        // from it, still under the world's perspective projection, is where the world ends.
        if (idx == 0 && !g_inPass && !g_worldEnded && g_lastPersp &&
            g_frameDraws >= static_cast<uint32_t>(g_cfg.minWorldDraws))
        {
            IDirect3DSurface9* cur = nullptr;
            dev->lpVtbl->GetRenderTarget(dev, 0, &cur);
            if (cur && cur != s)
                WorldEnded(dev, "render target switch");
            if (cur) cur->lpVtbl->Release(cur);
        }
        if (g_probe.active && !g_inPass)
            Log("  [draw %4u] SetRenderTarget %u -> %p", g_probe.draws, idx, s);
        return g_oSetRT(dev, idx, s);
    }

    HRESULT STDMETHODCALLTYPE hkStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr,
                                            IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f)
    {
        CheckDevice(dev);
        if (g_probe.active && !g_inPass)
            Log("  [draw %4u] StretchRect     %p -> %p", g_probe.draws, src, dst);
        // With anti-aliasing on, Full Screen Glow cannot draw the world into its texture: a texture cannot be
        // multisampled. The world goes to the multisampled back buffer and is copied out here, and the glow
        // composite later paints over the whole back buffer. Anything drawn after this copy is lost, so a
        // copy of the world's own render target is where the world ends.
        if (!g_inPass && !g_worldEnded && g_lastPersp && src &&
            g_frameDraws >= static_cast<uint32_t>(g_cfg.minWorldDraws))
        {
            IDirect3DSurface9* cur = nullptr;
            dev->lpVtbl->GetRenderTarget(dev, 0, &cur);
            if (cur == src)
                WorldEnded(dev, "world copied out");
            if (cur) cur->lpVtbl->Release(cur);
        }
        return g_oStretchRect(dev, src, sr, dst, dr, f);
    }

    inline void CountDraw(IDirect3DDevice9* dev, const char* kind, D3DPRIMITIVETYPE prim, UINT pc,
                          bool indexed = false, UINT first = 0, UINT nv = 0, const void* upData = nullptr)
    {
        g_frameDraws++;
        if (!g_probe.active)
            return;
        DetailDraw(dev, kind, prim, pc, indexed, first, nv, upData);
        g_probe.draws++;
        if (g_fogEnable)
        {
            g_probe.fogged++;
            if (g_vshader)
                g_probe.foggedVs++;
        }
    }

    // The sun in the sky. A probe of this client shows it as the first draw of the frame: a unit quad
    // (4 vertices, 2-triangle strip, centred on the origin) with an identity world matrix and depth writes
    // off, drawn under a special view matrix whose rotation is fixed and whose TRANSLATION is where the
    // sun sits in camera space. Projected through the ordinary world projection, it lands exactly on the
    // sun disc (measured: view[3] = (0.30, 5.00, 10.91) -> screen (0.52, 0.15), with the sun top centre).
    // So the quad's centre, origin * world * view, is the direction to the sun in camera space.
    //
    // At night the sky starts with a dozen model draws (stars, sky models) and then draws TWO such quads,
    // at draws 12 and 13 (probe, 01:00): Azeroth's two moons. So every quad of the sky phase is handed to
    // sun.cpp, which picks one (PickQuad). Until 2026-09-28 only the first 8 draws were looked at, and at night the light
    // kept the last direction it had by day.
    void NoteSkySun(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT pc, UINT nv)
    {
        if (!g_skyPhase || g_inPass || g_frameDraws >= 48 || prim != D3DPT_TRIANGLESTRIP || pc != 2 || nv != 4)
            return;

        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                if (fabsf(g_world.m[r][c] - (r == c ? 1.0f : 0.0f)) > 1e-3f)
                    return;
        const float* t = g_viewAll.m[3];
        if (t[0] * t[0] + t[1] * t[1] + t[2] * t[2] < 0.25f)
            return;                      // a camera without the sun's offset: not the sprite
        DWORD zwrite = 1;
        dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zwrite);
        if (zwrite)
            return;

        const float origin[4] = { g_world.m[3][0], g_world.m[3][1], g_world.m[3][2], 1.0f };
        float v[4];
        Mul4(origin, g_viewAll, v);
        if (v[2] * v[2] + v[1] * v[1] + v[0] * v[0] < 1e-6f)
            return;
        SunSetView(v);
        if (g_probe.active)
            Log("  [draw %4u] SKY SUN        camera-space (%.3f %.3f %.3f)", g_probe.draws, v[0], v[1], v[2]);
    }

    // The lamp probe (lamps.cpp): the client's lights and its world draws, while a window is open.
    HRESULT STDMETHODCALLTYPE hkSetLight(IDirect3DDevice9* dev, DWORD index, const D3DLIGHT9* light)
    {
        if (!g_inPass)
            LampsSetLight(index, light, !g_worldEnded);
        return g_oSetLight(dev, index, light);
    }

    HRESULT STDMETHODCALLTYPE hkLightEnable(IDirect3DDevice9* dev, DWORD index, BOOL on)
    {
        if (!g_inPass)
            LampsLightEnable(index, on);
        return g_oLightEnable(dev, index, on);
    }

    void NoteLampDraw(IDirect3DDevice9* dev, bool indexed, UINT first, UINT nv, const void* up = nullptr,
                      UINT upStride = 0)
    {
        if (!LampsWanted() || g_inPass || g_skyPhase || g_worldEnded || !g_lastPersp)
            return;
        LampDraw d = { indexed, first, nv, up, upStride, &g_world,
                       static_cast<IDirect3DVertexShader9*>(g_vshader), g_fog.color };
        LampsDraw(dev, d);
    }

    HRESULT STDMETHODCALLTYPE hkDrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT sv, UINT pc)
    {
        NoteSkySun(dev, prim, pc, VertsForPrims(prim, pc));
        NoteOrder(dev, "Draw", pc, VertsForPrims(prim, pc));
        if (!g_inPass)
            RecordDraw(dev, false, prim, static_cast<INT>(sv), 0, 0, 0, pc);
        NoteLampDraw(dev, false, sv, VertsForPrims(prim, pc));
        MaybeFireRays(dev);
        CountDraw(dev, "DrawPrimitive", prim, pc, false, sv, VertsForPrims(prim, pc));
        if (!g_inPass && !g_skyPhase && !g_worldEnded)
            BodyMarkDraw(dev);
        return g_oDrawPrim(dev, prim, sv, pc);
    }

    // The clouds, by elimination. The same probe that found the sun shows the sky as the first three draws
    // of a frame: the sun quad, an untextured additive dome (the sky colour), then one textured,
    // alpha-blended strip of ~177 vertices: the cloud layer. All three leave depth writes off; the first
    // draw that turns them on is terrain, and ends the sky phase. So a cloud is: still in the sky phase,
    // blended, textured, a strip of more than 8 vertices (the sun is 4). The world matrix is deliberately
    // not tested: a first version required identity and never matched: the layer is rotated, or drawn
    // under whatever world the previous draw left behind. With [sky] clouds = 0 it is not forwarded.
    bool IsCloudDraw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT nv)
    {
        if (!g_skyPhase || g_inPass)
            return false;
        DWORD zwrite = 1;
        dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zwrite);
        if (zwrite || g_frameDraws >= 48)      // 16 until the night sky's dozen extra draws (see NoteSkySun)
        {
            g_skyPhase = false;
            return false;
        }
        if (prim != D3DPT_TRIANGLESTRIP || nv <= 8)
            return false;
        DWORD blend = 0;
        dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
        if (!blend)
            return false;
        IDirect3DBaseTexture9* tex = nullptr;
        dev->lpVtbl->GetTexture(dev, 0, &tex);
        if (!tex)
            return false;                          // the untextured dome is the sky colour: keep it
        tex->lpVtbl->Release(tex);
        return true;
    }

    // The water writes depth ([fog] waterDepth, 2026-09-30). The client draws it blended with depth writes
    // off, so every pass that reads depth saw what lay under it: in Stormwind harbour the open sea showed
    // through the fog that covered the ships in front of it. Measured there: the sea is a run of about 400
    // draws, one per chunk, each fixed-function, 81 vertices (a 9 x 9 grid), vertex format 0x212, through
    // the client's water pixel shader, alpha blended. That shader is learnt from the 81-vertex draws, so
    // water in buildings, whose grids have other sizes, is taken too. Depth writes go on for the draw and
    // off after it: what the client draws later behind the surface is then hidden by it, as under murky
    // water.
    bool IsWaterDraw(IDirect3DDevice9* dev, UINT nv)
    {
        if (!g_cfg.depth.waterDepth || g_inPass || g_skyPhase || g_worldEnded || g_vshader || !VolumeActive())
            return false;
        DWORD blend = 0, zwrite = 1, fvf = 0;
        dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zwrite);
        if (zwrite)
            return false;
        dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
        dev->lpVtbl->GetFVF(dev, &fvf);
        if (!blend || fvf != 0x212)
            return false;
        IDirect3DPixelShader9* ps = nullptr;
        dev->lpVtbl->GetPixelShader(dev, &ps);
        if (!ps)
            return false;
        ps->lpVtbl->Release(ps);
        if (nv == 81)
        {
            if (g_waterPs.size() < 16 && g_waterPs.insert(ps).second)
                Log("water: pixel shader %p draws the water; it writes depth now ([fog] waterDepth)", ps);
            return true;
        }
        return g_waterPs.count(ps) != 0;
    }

    // A see-through model ([depth] seeThrough, 2026-09-30): a model (vertex shader) drawn alpha blended with
    // depth writes on. Measured on a stealthed lion: its batches (874, 338, 24 and 14 triangles, one texture)
    // were the only blended model draws writing depth in the frame, and absent with it off screen. Written,
    // its depth made the sun shadows and the light outline it. Depth writes go off for the draw: the passes
    // then see the ground behind it. A model drawn solid and then blended over itself already has its depth
    // from the solid pass. The grass is blended with depth writes too, but fixed-function: not taken.
    bool IsSeeThroughModel(IDirect3DDevice9* dev)
    {
        if (!g_cfg.depth.seeThrough || g_ownFaded || !g_vshader || g_inPass || g_skyPhase || g_worldEnded ||
            !VolumeActive())
            return false;
        DWORD zwrite = 0, blend = 0, src = 0, dst = 0;
        dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zwrite);
        if (!zwrite)
            return false;
        dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
        if (!blend)
            return false;
        dev->lpVtbl->GetRenderState(dev, D3DRS_SRCBLEND, &src);
        dev->lpVtbl->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
        return src == D3DBLEND_SRCALPHA && dst == D3DBLEND_INVSRCALPHA;
    }

    // The first of a see-through model's two passes ([depth] seeThrough, 2026-09-30): the client draws a
    // stealthed unit into depth alone (colour writes 0, depth writes on), then its colour blended over that,
    // so its own parts do not show through each other. Measured on the stealthed lions: each was that pair,
    // 4 draws and 4. The depth pass is what our passes saw, so it is not drawn: the shadows and the light see
    // the ground behind, and the shadow cache never records it. The colour pass, its depth writes off
    // (IsSeeThroughModel), then doubles up a little where a leg crosses the body.
    bool IsDepthOnlyModel(IDirect3DDevice9* dev)
    {
        if (!g_cfg.depth.seeThrough || g_ownFaded || !g_vshader || g_inPass || g_skyPhase || g_worldEnded ||
            !VolumeActive())
            return false;
        DWORD cw = 0xF, zwrite = 0;
        dev->lpVtbl->GetRenderState(dev, D3DRS_COLORWRITEENABLE, &cw);
        if (cw != 0)
            return false;
        dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zwrite);
        return zwrite != 0;
    }

    HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, INT bvi,
                                                     UINT mvi, UINT nv, UINT si, UINT pc)
    {
        if (IsDepthOnlyModel(dev))
        {
            ++g_depthOnlySkipped;
            return S_OK;
        }
        NoteSkySun(dev, prim, pc, nv);
        const bool cloud = IsCloudDraw(dev, prim, nv);
        if (cloud)
        {
            g_inPass = true;
            RaysBeforeClouds(dev);           // [rays] skyOnly: the rays cast from the sky without its clouds
            g_inPass = false;
        }
        if (!g_cfg.sky.clouds && cloud)
        {
            if (g_probe.active)
                Log("  [draw %4u] CLOUDS         skipped (%u vertices)", g_probe.draws, nv);
            CountDraw(dev, "DrawIndexed", prim, pc, true, static_cast<UINT>(bvi) + mvi, nv);
            return S_OK;
        }
        NoteOrder(dev, "Indexed", pc, nv);
        if (!g_inPass)
            RecordDraw(dev, true, prim, bvi, mvi, nv, si, pc);
        NoteLampDraw(dev, true, static_cast<UINT>(bvi) + mvi, nv);
        MaybeFireRays(dev);
        CountDraw(dev, "DrawIndexed", prim, pc, true, static_cast<UINT>(bvi) + mvi, nv);
        if (!g_inPass && !g_skyPhase && !g_worldEnded)
            BodyMarkDraw(dev);   // the stencil mark of a body (bodymask.cpp)
        if (IsSeeThroughModel(dev))
        {
            ++g_seeThroughDraws;
            dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
            const HRESULT hr = g_oDrawIdxPrim(dev, prim, bvi, mvi, nv, si, pc);
            dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
            return hr;
        }
        if (IsWaterDraw(dev, nv))
        {
            dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
            const HRESULT hr = g_oDrawIdxPrim(dev, prim, bvi, mvi, nv, si, pc);
            dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
            return hr;
        }
        return g_oDrawIdxPrim(dev, prim, bvi, mvi, nv, si, pc);
    }

    HRESULT STDMETHODCALLTYPE hkDrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT pc,
                                                const void* data, UINT stride)
    {
        NoteOrder(dev, "UP", pc, VertsForPrims(prim, pc));
        NoteLampDraw(dev, false, 0, VertsForPrims(prim, pc), data, stride);
        MaybeFireRays(dev);
        CountDraw(dev, "DrawPrimitiveUP", prim, pc, false, 0, VertsForPrims(prim, pc), data);
        return g_oDrawPrimUP(dev, prim, pc, data, stride);
    }

    HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT mvi,
                                                       UINT nv, UINT pc, const void* idx, D3DFORMAT fmt,
                                                       const void* data, UINT stride)
    {
        NoteOrder(dev, "IndexedUP", pc, nv);
        NoteLampDraw(dev, true, mvi, nv, data, stride);
        MaybeFireRays(dev);
        CountDraw(dev, "DrawIndexedUP", prim, pc, true, 0, nv, data);
        return g_oDrawIdxPrimUP(dev, prim, mvi, nv, pc, idx, fmt, data, stride);
    }

    // ---------------------------------------------------------------------------------------------
    // attaching to DXVK

    // True once comfygrass has patched the last slot it installs (DrawIndexedPrimitive, see its
    // PatchDevice), so the two installers never have VirtualProtect open on the same page at once and
    // comfyfog lands outermost.
    bool WaitForComfygrass(IDirect3DDevice9Vtbl* v)
    {
        HMODULE grass = GetModuleHandleA("comfygrass.dll");
        if (!grass)
        {
            Log("comfygrass not loaded, patching straight away");
            return true;
        }

        const double t0 = Now();
        for (;;)
        {
            HMODULE owner = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(v->DrawIndexedPrimitive), &owner);
            if (owner == grass)
            {
                Log("comfygrass patched first (waited %.0f ms), chaining on top of it",
                    1000.0 * (Now() - t0));
                return true;
            }
            if ((Now() - t0) * 1000.0 > g_cfg.chainWaitMs)
            {
                Log("comfygrass is loaded but never patched within %d ms; patching anyway. "
                    "Grass may keep the stock fog.", g_cfg.chainWaitMs);
                return false;
            }
            Sleep(20);
        }
    }

    void PatchVtable(IDirect3DDevice9Vtbl* v)
    {
        const bool ok =
            HookSlot(reinterpret_cast<void**>(&v->Present),                &hkPresent,                reinterpret_cast<void**>(&g_oPresent))       &&
            HookSlot(reinterpret_cast<void**>(&v->BeginScene),             &hkBeginScene,             reinterpret_cast<void**>(&g_oBeginScene))    &&
            HookSlot(reinterpret_cast<void**>(&v->SetDepthStencilSurface), &hkSetDepthStencilSurface, reinterpret_cast<void**>(&g_oSetDS))         &&
            HookSlot(reinterpret_cast<void**>(&v->Reset),                  &hkReset,                  reinterpret_cast<void**>(&g_oReset))         &&
            HookSlot(reinterpret_cast<void**>(&v->SetRenderState),         &hkSetRenderState,         reinterpret_cast<void**>(&g_oSetRS))         &&
            HookSlot(reinterpret_cast<void**>(&v->SetTransform),           &hkSetTransform,           reinterpret_cast<void**>(&g_oSetTransform))  &&
            HookSlot(reinterpret_cast<void**>(&v->SetRenderTarget),        &hkSetRenderTarget,        reinterpret_cast<void**>(&g_oSetRT))         &&
            HookSlot(reinterpret_cast<void**>(&v->StretchRect),            &hkStretchRect,            reinterpret_cast<void**>(&g_oStretchRect))   &&
            HookSlot(reinterpret_cast<void**>(&v->SetVertexShader),        &hkSetVertexShader,        reinterpret_cast<void**>(&g_oSetVS))         &&
            HookSlot(reinterpret_cast<void**>(&v->SetPixelShader),         &hkSetPixelShader,         reinterpret_cast<void**>(&g_oSetPS))         &&
            HookSlot(reinterpret_cast<void**>(&v->SetVertexShaderConstantF), &hkSetVertexShaderConstantF, reinterpret_cast<void**>(&g_oSetVSConstF)) &&
            HookSlot(reinterpret_cast<void**>(&v->SetLight),               &hkSetLight,               reinterpret_cast<void**>(&g_oSetLight))      &&
            HookSlot(reinterpret_cast<void**>(&v->LightEnable),            &hkLightEnable,            reinterpret_cast<void**>(&g_oLightEnable))   &&
            HookSlot(reinterpret_cast<void**>(&v->DrawPrimitive),          &hkDrawPrimitive,          reinterpret_cast<void**>(&g_oDrawPrim))      &&
            HookSlot(reinterpret_cast<void**>(&v->DrawIndexedPrimitive),   &hkDrawIndexedPrimitive,   reinterpret_cast<void**>(&g_oDrawIdxPrim))   &&
            HookSlot(reinterpret_cast<void**>(&v->DrawPrimitiveUP),        &hkDrawPrimitiveUP,        reinterpret_cast<void**>(&g_oDrawPrimUP))    &&
            HookSlot(reinterpret_cast<void**>(&v->DrawIndexedPrimitiveUP), &hkDrawIndexedPrimitiveUP, reinterpret_cast<void**>(&g_oDrawIdxPrimUP));

        Log("device %s (vtable %p, SetRenderState orig=%p)", ok ? "hooked" : "HOOK FAILED", v, g_oSetRS);
    }

    using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);

    // Every buffer DXVK hands out shares one class vtable, the same as its devices, so one patch catches
    // every write the client makes. Ours are all read-only locks, and those are left alone.
    using LockVBFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVertexBuffer9*, UINT, UINT, void**, DWORD);
    using LockIBFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DIndexBuffer9*, UINT, UINT, void**, DWORD);
    LockVBFn g_oLockVB = nullptr;
    LockIBFn g_oLockIB = nullptr;

    HRESULT STDMETHODCALLTYPE hkLockVB(IDirect3DVertexBuffer9* self, UINT offset, UINT size, void** data, DWORD flags)
    {
        if (!(flags & D3DLOCK_READONLY))
            ShadowNoteBufferWrite(self, offset, size);
        return g_oLockVB(self, offset, size, data, flags);
    }

    HRESULT STDMETHODCALLTYPE hkLockIB(IDirect3DIndexBuffer9* self, UINT offset, UINT size, void** data, DWORD flags)
    {
        if (!(flags & D3DLOCK_READONLY))
            ShadowNoteBufferWrite(self, offset, size);
        return g_oLockIB(self, offset, size, data, flags);
    }

    void PatchBufferVtables(IDirect3DDevice9* dev)
    {
        IDirect3DVertexBuffer9* vb = nullptr;
        if (SUCCEEDED(dev->lpVtbl->CreateVertexBuffer(dev, 64, 0, 0, D3DPOOL_DEFAULT, &vb, nullptr)) && vb)
        {
            auto* v = const_cast<IDirect3DVertexBuffer9Vtbl*>(vb->lpVtbl);
            HookSlot(reinterpret_cast<void**>(&v->Lock), &hkLockVB, reinterpret_cast<void**>(&g_oLockVB));
            vb->lpVtbl->Release(vb);
        }
        IDirect3DIndexBuffer9* ib = nullptr;
        if (SUCCEEDED(dev->lpVtbl->CreateIndexBuffer(dev, 64, 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, nullptr)) && ib)
        {
            auto* v = const_cast<IDirect3DIndexBuffer9Vtbl*>(ib->lpVtbl);
            HookSlot(reinterpret_cast<void**>(&v->Lock), &hkLockIB, reinterpret_cast<void**>(&g_oLockIB));
            ib->lpVtbl->Release(ib);
        }
        Log("buffer locks %s (vertex orig=%p, index orig=%p)", g_oLockVB && g_oLockIB ? "hooked" : "HOOK FAILED",
            g_oLockVB, g_oLockIB);
    }

    // A throwaway device names DXVK's shared device vtable; see comfygrass's AttachToDxvk for the why.
    bool AttachToDxvk()
    {
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9)
            d3d9 = LoadLibraryA("d3d9.dll");
        if (!d3d9)
        {
            Log("FATAL: no d3d9.dll in this process");
            return false;
        }

        // Our hooks live in DXVK's vtable, so it must never unload under us.
        HMODULE pin = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"d3d9.dll", &pin);

        auto create = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(d3d9, "Direct3DCreate9"));
        if (!create)
        {
            Log("FATAL: d3d9.dll at %p has no Direct3DCreate9", d3d9);
            return false;
        }

        IDirect3D9* d3d = create(D3D_SDK_VERSION);
        if (!d3d)
        {
            Log("FATAL: Direct3DCreate9 returned null");
            return false;
        }

        WNDCLASSEXA wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DefWindowProcA;
        wc.hInstance     = GetModuleHandleA(nullptr);
        wc.lpszClassName = "comfyfog_probe";
        RegisterClassExA(&wc);
        HWND wnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPED, 0, 0, 1, 1,
                                   nullptr, nullptr, wc.hInstance, nullptr);

        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed         = TRUE;
        pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.BackBufferWidth  = 1;
        pp.BackBufferHeight = 1;
        pp.hDeviceWindow    = wnd;

        IDirect3DDevice9* probe = nullptr;
        HRESULT hr = d3d->lpVtbl->CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                               D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                                               D3DCREATE_NOWINDOWCHANGES, &pp, &probe);
        if (FAILED(hr) || !probe)
        {
            Log("FATAL: probe CreateDevice failed hr=0x%08X", hr);
            d3d->lpVtbl->Release(d3d);
            if (wnd) DestroyWindow(wnd);
            return false;
        }

        // The vtable is static data inside the pinned d3d9.dll, so it outlives the device.
        auto* v = const_cast<IDirect3DDevice9Vtbl*>(probe->lpVtbl);
        PatchBufferVtables(probe);

        probe->lpVtbl->Release(probe);
        d3d->lpVtbl->Release(d3d);
        if (wnd)
            DestroyWindow(wnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);

        WaitForComfygrass(v);
        PatchVtable(v);
        return true;
    }

    // Off the loader lock: DllMain must not load libraries or create devices.
    DWORD WINAPI AttachThread(LPVOID)
    {
        const double t0 = Now();
        const bool ok = AttachToDxvk();
        Log("attach %s in %.0f ms", ok ? "succeeded" : "FAILED", 1000.0 * (Now() - t0));
        return 0;
    }
}

bool WorldFogColor(DWORD& color)
{
    if (!g_haveWorldFogColor)
        return false;
    color = g_worldFogColor;
    return true;
}

// /atmos probe: the same as F12, which a key sent to the game sometimes misses.
void ProbeArm()
{
    g_probe.armed = true;
    ReportStart("/atmos probe");
    g_orderArm = true;
}

// /atmos bench: the same as Alt+F12. A second one while it runs stops it, as the key does.
void BenchArm()
{
    g_benchArmed = true;
}

bool WorldFog(float& start, float& end)
{
    if (!g_haveWorldFog)
        return false;
    start = D2F(g_worldFogStart);
    end = D2F(g_worldFogEnd);
    return true;
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
        DisableThreadLibraryCalls(self);

        // Our hooks are function pointers in DXVK's vtable; unloading this image would leave them dangling.
        HMODULE pin = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&g_lock), &pin);
        ResolveIniPath(self, g_iniPath, MAX_PATH);
        wcscpy_s(g_logPath, g_iniPath);
        wcscpy_s(wcsrchr(g_logPath, L'\\') + 1, 16, L"comfyfog.log");
        DeleteFileW(g_logPath);
        LoadSettings(g_iniPath);
        CVarsAfterLoad();
        Log("comfyfog loaded (module=%p, effects %s)", self, g_cfg.master ? "on" : "off");

        if (g_cfg.hook)
        {
            HANDLE t = CreateThread(nullptr, 0, AttachThread, nullptr, 0, nullptr);
            if (t)
                CloseHandle(t);
            else
                Log("FATAL: could not start the attach thread");
        }
        else
        {
            Log("hook = 0, so nothing is patched: comfyfog is inert this run");
        }
    }
    return TRUE;
}
