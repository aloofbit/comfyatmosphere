// comfyfog: fog control for the 1.12 client.
//
// Same shape as comfygrass: loaded by VanillaFixes from dlls.txt, attaches by patching DXVK's shared
// IDirect3DDevice9 vtable in place (found through a throwaway device of our own), and is tuned from an
// ini that reloads in game. See comfygrass/src/README.md for why each of those choices was made.
//
// The effect itself is small. The client hands its fog to the fixed-function pipeline through
// SetRenderState, so the FOGSTART / FOGEND / FOGCOLOR / FOGDENSITY values are rewritten on the way
// through, scaled by one 0..100 dial. Nothing is drawn, allocated or read back.
//
// Load order with comfygrass matters. Both DLLs patch the same vtable slots, so whichever patches last
// sits outermost and sees the client's calls first. comfygrass mirrors the fog states it is handed and
// fogs grass from that mirror, so comfyfog has to be the OUTER hook. Then comfygrass records the
// rewritten values and grass follows the new fog for free. Inner, grass would keep the stock fog and
// stand out bright against fogged terrain. Hence the wait in AttachToDxvk.

#define CINTERFACE // C-style IDirect3DDevice9Vtbl, so slots are patched by name, not by index
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "bench.h"
#include "common.h"
#include "config.h"
#include "cvars.h"
#include "depth.h"
#include "rays.h"
#include "shadow.h"
#include "sun.h"
#include "volume.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
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
    // the client's fog, as it asked for it
    //
    // The raw values are kept so the dial can be re-applied on a reload without waiting for the client
    // to set them again. It may only do that on a zone change.

    struct ClientFog
    {
        DWORD start   = F2D(0.0f);   // D3D9 defaults
        DWORD end     = F2D(1.0f);
        DWORD density = F2D(1.0f);
        DWORD color   = 0;
        bool  haveStart = false, haveEnd = false, haveDensity = false, haveColor = false;
    };

    ClientFog g_fog;
    bool      g_on = true;   // Shift+reload toggles; independent of [fog] enabled in the ini

    bool Active()
    {
        return g_on && g_cfg.fog.enabled && g_cfg.fog.thickness > 0.0f;
    }

    float Dial() { return g_cfg.fog.thickness * 0.01f; }

    // Geometric in the dial, so 0->10 feels about as big a step as 90->100.
    float EndMul() { return powf(g_cfg.fog.reach, Dial()); }

    // The whole curve, in one place, so the fixed-function states and the shader constant (c30, below)
    // are remapped identically and M2s match terrain.
    //
    // End is scaled by the dial. Start is expressed as a fraction of end, and that fraction slides from
    // the client's own toward a negative one. Linear fog amount at distance d is (d - start) / (end -
    // start), so a start of -h/(1-h) of end puts fog amount h at the camera and still reaches full fog
    // at end: a haze floor with a gentler slope behind it, rather than clear air that thickens into a wall.
    void Remap(float s, float e, float& sOut, float& eOut)
    {
        if (!Active())
        {
            sOut = s; eOut = e;
            return;
        }
        eOut = e * EndMul();
        const float stockFrac = e > 1e-3f ? s / e : 0.0f;
        const float h         = g_cfg.fog.haze;
        const float hazeFrac  = -h / (1.0f - h);
        const float frac      = stockFrac + (hazeFrac - stockFrac) * Dial();
        sOut = eOut * frac;
    }

    DWORD OutEnd()
    {
        float s, e;
        Remap(D2F(g_fog.start), D2F(g_fog.end), s, e);
        return Active() ? F2D(e) : g_fog.end;
    }

    DWORD OutStart()
    {
        float s, e;
        Remap(D2F(g_fog.start), D2F(g_fog.end), s, e);
        return Active() ? F2D(s) : g_fog.start;
    }

    // Fog amount right at the camera for the values being sent, for the log.
    float OutHaze()
    {
        const float s = D2F(OutStart()), e = D2F(OutEnd());
        return (e - s) > 1e-6f ? Clamp01(-s / (e - s)) : 0.0f;
    }

    // Exponential fog modes carry no start/end; the density is scaled to match the linear case.
    DWORD OutDensity()
    {
        return Active() ? F2D(D2F(g_fog.density) / EndMul()) : g_fog.density;
    }

    DWORD OutColor()
    {
        // Black fog is how additive passes (glows, particles) are kept from brightening into the fog,
        // and the client switches to it several times a frame. Tinting it would haze those effects.
        if (!Active() || (g_fog.color & 0xFFFFFF) == 0)
            return g_fog.color;
        const FogSettings& f = g_cfg.fog;
        const float t = Dial();

        float c[3] = { ((g_fog.color >> 16) & 0xFF) / 255.0f,
                       ((g_fog.color >>  8) & 0xFF) / 255.0f,
                       ((g_fog.color      ) & 0xFF) / 255.0f };
        const float tint[3] = { ((f.tint >> 16) & 0xFF) / 255.0f,
                                ((f.tint >>  8) & 0xFF) / 255.0f,
                                ((f.tint      ) & 0xFF) / 255.0f };

        const float lum = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
        const float ds = f.desaturate * t, ta = f.tintAmount * t, dk = 1.0f - f.darken * t;

        DWORD out = g_fog.color & 0xFF000000;
        for (int i = 0; i < 3; ++i)
        {
            float v = c[i] + (lum - c[i]) * ds;
            v = v + (tint[i] - v) * ta;
            v = Clamp01(v * dk);
            out |= static_cast<DWORD>(v * 255.0f + 0.5f) << (16 - 8 * i);
        }
        return out;
    }

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
    DrawPrimFn      g_oDrawPrim      = nullptr;
    DrawIdxPrimFn   g_oDrawIdxPrim   = nullptr;
    DrawPrimUPFn    g_oDrawPrimUP    = nullptr;
    DrawIdxPrimUPFn g_oDrawIdxPrimUP = nullptr;

    using SetVSConstFFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
    SetVSConstFFn   g_oSetVSConstF   = nullptr;

    using SetTransformFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
    SetTransformFn  g_oSetTransform  = nullptr;

    using SetRTFn       = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    using StretchRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
                                                      IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
    SetRTFn         g_oSetRT         = nullptr;

    StretchRectFn   g_oStretchRect   = nullptr;
    bool            g_inPass         = false;   // our own passes: their calls stay out of the mirrors and the probe

    // ---------------------------------------------------------------------------------------------
    // vertex-shader fog
    //
    // M2s (trees, doodads, every character) draw through the client's Model2 vertex shaders, which write
    // oFog themselves, so FOGSTART/FOGEND never reach them, and they stood out clear against fogged
    // terrain. Disassembling them (below, logged once per shader) shows all 19 that fog do it the same way:
    //
    //     mad r.w, viewZ, c30.x, c30.y      max 0, min 1  ->  oFog
    //
    // which is linear fog folded into one multiply-add: c30.x = -1/(end-start), c30.y = end/(end-start).
    // Measured: with FOGSTART 104.167 / FOGEND 416.667 the client uploaded c30 = (-0.0032, 1.3333).
    // c30 is read for nothing else in any shader. So the client's start/end are recovered from c30 itself
    // and pushed through the same Remap as the render states, and M2s fog exactly like terrain.
    //
    // The probe key still logs the small constant uploads of one frame (bone palettes are large, so
    // uploads of more than 4 registers are only counted), and the dumps stay in, so a different client
    // build can be re-checked from one log.

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

    float g_c30[4]    = {};      // the client's last c30, as it sent it
    bool  g_haveC30   = false;

    // Rewrites a c30 fog constant in place. A non-negative slope is not a linear fog (the client parks
    // fog this way when it is off), so it is passed through untouched.
    void RemapC30(float* c)
    {
        const float a = c[0], b = c[1];
        if (!(a < -1e-9f) || !Active())
            return;
        const float range = -1.0f / a;
        const float e = b * range, s0 = e - range;
        float s, e2;
        Remap(s0, e, s, e2);
        const float span = e2 - s;
        if (!(span > 1e-4f))
            return;
        c[0] = -1.0f / span;
        c[1] = e2 / span;
    }

    HRESULT STDMETHODCALLTYPE hkSetVertexShaderConstantF(IDirect3DDevice9* dev, UINT reg, const float* data,
                                                         UINT count)
    {
        if (g_inPass)
            return g_oSetVSConstF(dev, reg, data, count);   // our own passes: no fog remap, no recording
        RecordConstants(reg, data, count);                  // the client's values, before the c30 remap
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

        const int fr = g_cfg.fog.shaderReg;
        if (fr < 0 || !data || static_cast<UINT>(fr) < reg || static_cast<UINT>(fr) >= reg + count)
            return g_oSetVSConstF(dev, reg, data, count);

        // c30 often arrives inside a larger upload alongside the bone palette, so the whole range is
        // copied and forwarded with only that one register changed.
        const UINT at = static_cast<UINT>(fr) - reg;
        memcpy(g_c30, data + 4 * at, sizeof(g_c30));
        g_haveC30 = true;

        static std::vector<float> buf;
        buf.assign(data, data + 4 * count);
        RemapC30(&buf[4 * at]);

        if (g_probe.active)
            Log("  [draw %4u] c%d fog client=(%.5f %.4f) sent=(%.5f %.4f)", g_probe.draws, fr,
                g_c30[0], g_c30[1], buf[4 * at], buf[4 * at + 1]);
        return g_oSetVSConstF(dev, reg, buf.data(), count);
    }

    bool               g_sunSeen   = false;     // the sky's sun sprite was found this frame
    bool               g_worldEnded = false;    // the world finished drawing this frame

    // The world has finished drawing: depth, shadows and volumetric light go in now, with the world's
    // render target and depth buffer still bound.
    void WorldEnded(IDirect3DDevice9* dev, const char* why)
    {
        if (g_worldEnded)
            return;
        g_worldEnded = true;
        DepthWorldEnded(dev);
        g_inPass = true;
        if (VolumeActive())          // the map costs more than the light does; it is only for the light
        {
            BenchSectionBegin(dev, kBenchShadow);
            ShadowWorldEnded(dev);
            BenchSectionEnd(dev, kBenchShadow, true);
            BenchSectionBegin(dev, kBenchVolume);
            const bool drawn = VolumeDraw(dev);
            BenchSectionEnd(dev, kBenchVolume, drawn);
        }
        else
        {
            ShadowNoReplay();        // so the cost report does not keep showing the last one
        }
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

    void FireRays(IDirect3DDevice9* dev, const char* where)
    {
        g_raysArmed = false;
        g_raysDone  = true;
        g_inPass = true;
        BenchSectionBegin(dev, kBenchRays);
        const bool ran = RaysBeforeUI(dev);
        BenchSectionEnd(dev, kBenchRays, ran);
        g_inPass = false;
        if (ran && g_probe.active)
            Log("  [draw %4u] RAYS PASS      %s", g_probe.draws, where);
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

    // Pushes the current dial onto the device for every fog value the client has set, so a reload or a
    // toggle shows at once instead of on the next zone change.
    void ApplyAll(IDirect3DDevice9* dev)
    {
        if (g_fog.haveEnd)     g_oSetRS(dev, D3DRS_FOGEND,     OutEnd());
        if (g_fog.haveStart)   g_oSetRS(dev, D3DRS_FOGSTART,   OutStart());
        if (g_fog.haveDensity) g_oSetRS(dev, D3DRS_FOGDENSITY, OutDensity());
        if (g_fog.haveColor)   g_oSetRS(dev, D3DRS_FOGCOLOR,   OutColor());
        if (g_haveC30 && g_cfg.fog.shaderReg >= 0)
        {
            float c[4];
            memcpy(c, g_c30, sizeof(c));
            RemapC30(c);
            g_oSetVSConstF(dev, static_cast<UINT>(g_cfg.fog.shaderReg), c, 1);
        }
    }

    void LogDial(const char* why)
    {
        const FogSettings& f = g_cfg.fog;
        Log("--- %s: override %s, thickness %.0f, client end %.1f -> %.1f, start %.1f -> %.1f "
            "(%.0f%% fog at the camera), colour 0x%06X -> 0x%06X ---",
            why, Active() ? "ON" : "OFF", f.thickness,
            D2F(g_fog.end), D2F(OutEnd()), D2F(g_fog.start), D2F(OutStart()), 100.0f * OutHaze(),
            g_fog.color & 0xFFFFFF, OutColor() & 0xFFFFFF);
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
            const bool plain = !(GetAsyncKeyState(VK_MENU) & 0x8000) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                               !(GetAsyncKeyState(VK_CONTROL) & 0x8000);
            BenchCancel("F11", !plain);
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
            {
                RaysToggle();
            }
            else if (GetAsyncKeyState(VK_MENU) & 0x8000)
            {
                VolumeToggle();
            }
            else if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
            {
                g_on = !g_on;
                LogDial("toggled");
                ApplyAll(dev);
            }
            else
            {
                LoadSettings(g_iniPath);
                CVarsAfterLoad();
                RaysReload();
                LogDial("reloaded");
                ApplyAll(dev);
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
                ApplyAll(dev);
            }
            else
            {
                g_probe.armed = true;
            }
        }
        g_probeDown = probe;
    }

    // The first call of a frame's rendering: the readable depth buffer goes in, and the shadow
    // recording opens.
    HRESULT STDMETHODCALLTYPE hkBeginScene(IDirect3DDevice9* dev)
    {
        if (!g_inPass)
        {
            DepthBeginScene(dev);
            if (!g_worldEnded)
                ShadowSetPhase(VolumeActive());
        }
        return g_oBeginScene(dev);
    }

    // The client binding a depth buffer: hand the device our readable stand-in instead (depth.cpp).
    HRESULT STDMETHODCALLTYPE hkSetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9* s)
    {
        return g_oSetDS(dev, g_inPass ? s : DepthSubstitute(dev, s));
    }

    HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst,
                                        HWND wnd, const RGNDATA* dirty)
    {
        // Between captures, so the pass's own draws and state changes never show up in a probe.
        // Armed but never fired: nothing was drawn to the back buffer after the world (UI hidden, say),
        // so the finished frame is exactly the world and the pass can run here.
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

        if (g_probe.active)
        {
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
            if (BenchFrame(dev, last > 0.0 ? now - last : 0.0))
                ApplyAll(dev);   // the benchmark moved to its next step, or finished
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
        g_frameDraws = 0;
        g_lastPersp  = false;
        g_sunSeen = false;
        g_worldEnded = false;
        g_skyPhase  = true;

        PollKeys(dev);
        if (CVarsPoll())
        {
            BenchCancel("a control in Video > Shaders moved", false);
            ApplyAll(dev);   // a moved slider shows this frame, not on the next zone change
        }

        g_frame++;
        if (g_probe.armed)
        {
            g_probe = Probe();
            g_probe.active = true;
            LogDial("probe");
            DepthProbe();
            ShadowProbe();
            VolumeProbe();
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
        DepthReset(dev);   // Reset fails outright while any D3DPOOL_DEFAULT object is alive
        ShadowReset();
        VolumeReset();
        RaysReset();
        BenchReset();
        const HRESULT hr = g_oReset(dev, pp);
        if (SUCCEEDED(hr))
        {
            g_fog      = ClientFog();
            g_haveC30  = false;
            g_fogEnable = 0;
            g_vshader  = nullptr;
            Log("device reset: fog mirror cleared");
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkSetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE st, DWORD value)
    {
        HRESULT hr;
        switch (st)
        {
        case D3DRS_FOGSTART:
            g_fog.start = value; g_fog.haveStart = true;
            hr = g_oSetRS(dev, st, OutStart());
            NoteFogState(st, value, OutStart());
            return hr;

        case D3DRS_FOGEND:
            // Start is derived from end, so a new end re-sends start too.
            g_fog.end = value; g_fog.haveEnd = true;
            hr = g_oSetRS(dev, st, OutEnd());
            if (g_fog.haveStart && Active())
                g_oSetRS(dev, D3DRS_FOGSTART, OutStart());
            NoteFogState(st, value, OutEnd());
            return hr;

        case D3DRS_FOGDENSITY:
            g_fog.density = value; g_fog.haveDensity = true;
            hr = g_oSetRS(dev, st, OutDensity());
            NoteFogState(st, value, OutDensity());
            return hr;

        case D3DRS_FOGCOLOR:
            g_fog.color = value; g_fog.haveColor = true;
            hr = g_oSetRS(dev, st, OutColor());
            NoteFogState(st, value, OutColor());
            return hr;

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
            return;
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
        if (early && g_probe.draws < 8 && nv && nv <= 8)
            DumpSkyDraw(dev, first, nv, upData);
        if (rt)  rt->lpVtbl->Release(rt);
        if (tex) tex->lpVtbl->Release(tex);
        if (ps)  ps->lpVtbl->Release(ps);
    }

    HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* s)
    {
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
        if (g_probe.active && !g_inPass)
            Log("  [draw %4u] StretchRect     %p -> %p", g_probe.draws, src, dst);
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
    // So the quad's centre, origin * world * view, is the direction to the sun in camera space. Only the
    // first few draws of a frame are looked at, and only until the sprite is found.
    void NoteSkySun(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT pc, UINT nv)
    {
        if (g_sunSeen || g_inPass || g_frameDraws >= 8 || prim != D3DPT_TRIANGLESTRIP || pc != 2 || nv != 4)
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
        g_sunSeen = true;
        if (g_probe.active)
            Log("  [draw %4u] SKY SUN        camera-space (%.3f %.3f %.3f)", g_probe.draws, v[0], v[1], v[2]);
    }

    HRESULT STDMETHODCALLTYPE hkDrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT sv, UINT pc)
    {
        NoteSkySun(dev, prim, pc, VertsForPrims(prim, pc));
        if (!g_inPass)
            RecordDraw(dev, false, prim, static_cast<INT>(sv), 0, 0, 0, pc);
        MaybeFireRays(dev);
        CountDraw(dev, "DrawPrimitive", prim, pc, false, sv, VertsForPrims(prim, pc));
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
        if (zwrite || g_frameDraws >= 16)
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

    HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, INT bvi,
                                                     UINT mvi, UINT nv, UINT si, UINT pc)
    {
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
        if (!g_inPass)
            RecordDraw(dev, true, prim, bvi, mvi, nv, si, pc);
        MaybeFireRays(dev);
        CountDraw(dev, "DrawIndexed", prim, pc, true, static_cast<UINT>(bvi) + mvi, nv);
        return g_oDrawIdxPrim(dev, prim, bvi, mvi, nv, si, pc);
    }

    HRESULT STDMETHODCALLTYPE hkDrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT pc,
                                                const void* data, UINT stride)
    {
        MaybeFireRays(dev);
        CountDraw(dev, "DrawPrimitiveUP", prim, pc, false, 0, VertsForPrims(prim, pc), data);
        return g_oDrawPrimUP(dev, prim, pc, data, stride);
    }

    HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT mvi,
                                                       UINT nv, UINT pc, const void* idx, D3DFORMAT fmt,
                                                       const void* data, UINT stride)
    {
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
            HookSlot(reinterpret_cast<void**>(&v->SetVertexShaderConstantF), &hkSetVertexShaderConstantF, reinterpret_cast<void**>(&g_oSetVSConstF)) &&
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
        Log("comfyfog loaded (module=%p, thickness=%.0f, enabled=%d)",
            self, g_cfg.fog.thickness, g_cfg.fog.enabled ? 1 : 0);

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
