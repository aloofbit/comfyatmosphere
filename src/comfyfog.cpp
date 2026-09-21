// comfyfog -- fog control for the 1.12 client.
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
// fogs grass from that mirror, so comfyfog has to be the OUTER hook -- then comfygrass records the
// rewritten values and grass follows the new fog for free. Inner, grass would keep the stock fog and
// stand out bright against fogged terrain. Hence the wait in AttachToDxvk.

#define CINTERFACE // C-style IDirect3DDevice9Vtbl, so slots are patched by name, not by index
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "config.h"

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
    // to set them again -- it may only do that on a zone change.

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
    // landed at, and how many fogged draws went through a vertex shader -- those compute their own fog
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
    };

    Probe    g_probe;
    uint64_t g_frame      = 0;
    DWORD    g_fogEnable  = 0;
    // Each distinct (state, value) the client sends is logged once. It re-sends fog many times a frame,
    // switching between the zone colour and black, so logging every change filled the cap in 5 frames.
    std::set<std::pair<DWORD, DWORD>> g_seenValues;
    void*    g_vshader    = nullptr;
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
    ResetFn         g_oReset         = nullptr;
    SetRSFn         g_oSetRS         = nullptr;
    SetVSFn         g_oSetVS         = nullptr;
    DrawPrimFn      g_oDrawPrim      = nullptr;
    DrawIdxPrimFn   g_oDrawIdxPrim   = nullptr;
    DrawPrimUPFn    g_oDrawPrimUP    = nullptr;
    DrawIdxPrimUPFn g_oDrawIdxPrimUP = nullptr;

    using SetVSConstFFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
    SetVSConstFFn   g_oSetVSConstF   = nullptr;

    // ---------------------------------------------------------------------------------------------
    // vertex-shader fog
    //
    // M2s (trees, doodads, every character) draw through the client's Model2 vertex shaders, which write
    // oFog themselves -- so FOGSTART/FOGEND never reach them, and they stood out clear against fogged
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

    struct OgBlob;
    struct OgBlobVtbl
    {
        HRESULT (STDMETHODCALLTYPE* QueryInterface)(OgBlob*, REFIID, void**);
        ULONG   (STDMETHODCALLTYPE* AddRef)(OgBlob*);
        ULONG   (STDMETHODCALLTYPE* Release)(OgBlob*);
        LPVOID  (STDMETHODCALLTYPE* GetBufferPointer)(OgBlob*);
        SIZE_T  (STDMETHODCALLTYPE* GetBufferSize)(OgBlob*);
    };
    struct OgBlob { const OgBlobVtbl* lpVtbl; };

    using PFN_D3DDisassemble = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, OgBlob**);

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

        static PFN_D3DDisassemble disasm = [] {
            HMODULE comp = GetModuleHandleA("d3dcompiler_47.dll");
            if (!comp)
                comp = LoadLibraryA("d3dcompiler_47.dll");
            return comp ? reinterpret_cast<PFN_D3DDisassemble>(GetProcAddress(comp, "D3DDisassemble")) : nullptr;
        }();

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
        // copied and forwarded with just that one register changed.
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
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
            {
                g_on = !g_on;
                LogDial("toggled");
            }
            else
            {
                LoadSettings(g_iniPath);
                LogDial("reloaded");
            }
            ApplyAll(dev);
        }
        g_reloadDown = reload;

        const bool probe = focused && (GetAsyncKeyState(g_cfg.probeKey) & 0x8000) != 0;
        if (probe && !g_probeDown)
            g_probe.armed = true;
        g_probeDown = probe;
    }

    HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst,
                                        HWND wnd, const RGNDATA* dirty)
    {
        if (g_probe.active)
        {
            Log("--- end frame %llu: %u draws, %u fogged (%u of those through a vertex shader), "
                "%u fog state sets, %u vs constant uploads ---",
                g_frame, g_probe.draws, g_probe.fogged, g_probe.foggedVs, g_probe.fogSets, g_probe.constSets);
            g_probe = Probe();
        }

        PollKeys(dev);

        g_frame++;
        if (g_probe.armed)
        {
            g_probe = Probe();
            g_probe.active = true;
            LogDial("probe");
            Log("--- begin frame %llu capture ---", g_frame);
        }
        return g_oPresent(dev, src, dst, wnd, dirty);
    }

    // Reset puts every render state back to its default, and the client sets what it needs again
    // afterwards -- so the mirror goes back to defaults too, rather than re-pushing stale values.
    HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
    {
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

        default:
            break;
        }
        NoteFogState(st, value, value);
        return g_oSetRS(dev, st, value);
    }

    HRESULT STDMETHODCALLTYPE hkSetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* sh)
    {
        g_vshader = sh;
        DumpShader(sh);
        return g_oSetVS(dev, sh);
    }

    inline void CountDraw()
    {
        if (!g_probe.active)
            return;
        g_probe.draws++;
        if (g_fogEnable)
        {
            g_probe.fogged++;
            if (g_vshader)
                g_probe.foggedVs++;
        }
    }

    HRESULT STDMETHODCALLTYPE hkDrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT sv, UINT pc)
    {
        CountDraw();
        return g_oDrawPrim(dev, prim, sv, pc);
    }

    HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, INT bvi,
                                                     UINT mvi, UINT nv, UINT si, UINT pc)
    {
        CountDraw();
        return g_oDrawIdxPrim(dev, prim, bvi, mvi, nv, si, pc);
    }

    HRESULT STDMETHODCALLTYPE hkDrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT pc,
                                                const void* data, UINT stride)
    {
        CountDraw();
        return g_oDrawPrimUP(dev, prim, pc, data, stride);
    }

    HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT mvi,
                                                       UINT nv, UINT pc, const void* idx, D3DFORMAT fmt,
                                                       const void* data, UINT stride)
    {
        CountDraw();
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
                Log("comfygrass is loaded but never patched within %d ms -- patching anyway. "
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
            HookSlot(reinterpret_cast<void**>(&v->Reset),                  &hkReset,                  reinterpret_cast<void**>(&g_oReset))         &&
            HookSlot(reinterpret_cast<void**>(&v->SetRenderState),         &hkSetRenderState,         reinterpret_cast<void**>(&g_oSetRS))         &&
            HookSlot(reinterpret_cast<void**>(&v->SetVertexShader),        &hkSetVertexShader,        reinterpret_cast<void**>(&g_oSetVS))         &&
            HookSlot(reinterpret_cast<void**>(&v->SetVertexShaderConstantF), &hkSetVertexShaderConstantF, reinterpret_cast<void**>(&g_oSetVSConstF)) &&
            HookSlot(reinterpret_cast<void**>(&v->DrawPrimitive),          &hkDrawPrimitive,          reinterpret_cast<void**>(&g_oDrawPrim))      &&
            HookSlot(reinterpret_cast<void**>(&v->DrawIndexedPrimitive),   &hkDrawIndexedPrimitive,   reinterpret_cast<void**>(&g_oDrawIdxPrim))   &&
            HookSlot(reinterpret_cast<void**>(&v->DrawPrimitiveUP),        &hkDrawPrimitiveUP,        reinterpret_cast<void**>(&g_oDrawPrimUP))    &&
            HookSlot(reinterpret_cast<void**>(&v->DrawIndexedPrimitiveUP), &hkDrawIndexedPrimitiveUP, reinterpret_cast<void**>(&g_oDrawIdxPrimUP));

        Log("device %s (vtable %p, SetRenderState orig=%p)", ok ? "hooked" : "HOOK FAILED", v, g_oSetRS);
    }

    using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);

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
            Log("hook = 0, so nothing is patched -- comfyfog is inert this run");
        }
    }
    return TRUE;
}
