// grade: the colour saturation of the world, one value by day and one by night (2026-10-05).
//
// [colour] day and night are percentages: 100 is the game's own colour, 0 is grey, 200 twice as strong. The
// change between the two follows the game clock with the same weight as Night Darkness (sun.cpp,
// NightWeight: [night] dusk, dawn and fade), eased with a smoothstep so it starts and ends gently. The
// value in use also moves toward that target over about a second of real time, so a jump of the clock
// (comfytime, .time) or a slider moved does not change the screen in one frame.
//
// The Color Effects box ([colour] enabled) turns the whole pass off, the clock read included.
//
// The pass runs at the world -> UI boundary, after the rays (comfyatmos.cpp, FireRays), so the UI keeps its
// colour. It copies the back buffer into a texture of the same size and draws it back over the back
// buffer with the saturation applied: out = lum + (colour - lum) x saturation, with Rec. 709 luminance.
// Alpha is not written. With the saturation at 1 the pass does not run and costs nothing. It does not run
// in a debug view, whose colours mean something, or outside the world (the login and character screens).
//
// The device state is saved and put back as in rays.cpp: a D3DSBT_ALL state block, plus the render
// states the hooks mirror re-set through the vtable.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "common.h"
#include "config.h"
#include "grade.h"
#include "sun.h"
#include "shadercache.h"

#include <cmath>
#include <cstring>

namespace
{
    const char* kGradeHlsl = R"HLSL(
sampler2D sScene : register(s0);
float4 gS : register(c0);   // x: the saturation, 1 = the game's own

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float3 c = tex2D(sScene, uv).rgb;
    float  l = dot(c, float3(0.2126, 0.7152, 0.0722));
    return float4(saturate(l + (c - l) * gS.x), 1.0);
}
)HLSL";

    IDirect3DTexture9*     g_copy = nullptr;
    IDirect3DSurface9*     g_copySurf = nullptr;
    UINT                   g_copyW = 0, g_copyH = 0;
    D3DFORMAT              g_copyFmt = D3DFMT_UNKNOWN;
    IDirect3DPixelShader9* g_ps = nullptr;
    IDirect3DStateBlock9*  g_sb = nullptr;
    bool                   g_failed = false;   // a resource could not be made: off until the next Reset

    float  g_now = 1.0f;     // the saturation in use, eased toward the target
    double g_last = 0.0;     // when g_now was last moved

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    // The saturation the clock asks for now, 1 = the game's own.
    float Target()
    {
        const ColourSettings& c = g_cfg.colour;
        const float day = c.day * 0.01f, night = c.night * 0.01f;
        float hour = 0.0f;
        if (day == night || !ClientHour(hour))
            return day;
        const float w = NightWeight(hour);
        const float s = w * w * (3.0f - 2.0f * w);   // smoothstep: gentle at both ends of the change
        return day + (night - day) * s;
    }

    // Moves g_now toward the target with a time constant of 0.4 s: a full change ends in under 2 s.
    void Ease()
    {
        const double now = Now();
        const float target = Target();
        if (g_last <= 0.0 || now - g_last > 2.0)
            g_now = target;   // the first frame, or after a pause (a loading screen): no slow fade in
        else
            g_now += (target - g_now) * static_cast<float>(1.0 - exp(-(now - g_last) / 0.4));
        // The last 1% cannot be seen. Without the snap, the pass ran on for seconds at 1.007 after a change
        // back to 100 (the saturation test, 2026-10-05).
        if (fabsf(g_now - target) < 0.01f)
            g_now = target;
        g_last = now;
    }

    bool DebugViewOn()
    {
        const Settings& s = g_cfg;
        return s.volume.debug || s.sunShadows.debug || s.lamps.debug || s.rays.debugView || s.fog.debug ||
               s.water.debug;
    }

    bool MakeShader(IDirect3DDevice9* dev)
    {
        if (g_ps)
            return true;
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        bool ok = false;
        if (!compile || FAILED(compile(kGradeHlsl, strlen(kGradeHlsl), "grade", nullptr, nullptr, "main", "ps_2_0",
                                       0, 0, &code, &errs)) || !code)
            Log("grade: the shader failed to compile: %s",
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no compiler)");
        else if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)),
                                                       &g_ps)) || !g_ps)
            Log("grade: the shader could not be created");
        else
            ok = true;
        if (code) code->lpVtbl->Release(code);
        if (errs) errs->lpVtbl->Release(errs);
        return ok;
    }

    // The copy of the back buffer, made again when its size or format changes.
    bool MakeCopy(IDirect3DDevice9* dev, const D3DSURFACE_DESC& d)
    {
        if (g_copy && g_copyW == d.Width && g_copyH == d.Height && g_copyFmt == d.Format)
            return true;
        SafeRelease(g_copySurf);
        SafeRelease(g_copy);
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format,
                                                D3DPOOL_DEFAULT, &g_copy, nullptr);
        if (SUCCEEDED(hr))
            hr = g_copy->lpVtbl->GetSurfaceLevel(g_copy, 0, &g_copySurf);
        if (FAILED(hr))
        {
            Log("grade: could not create the %ux%u screen copy (format %u, hr=0x%08X): no saturation",
                d.Width, d.Height, static_cast<unsigned>(d.Format), hr);
            SafeRelease(g_copySurf);
            SafeRelease(g_copy);
            return false;
        }
        g_copyW = d.Width;
        g_copyH = d.Height;
        g_copyFmt = d.Format;
        Log("grade: screen copy made, %ux%u, format %u", d.Width, d.Height, static_cast<unsigned>(d.Format));
        return true;
    }

    struct QuadVertex { float x, y, z, rhw, u, v; };

    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE,
        D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_FOGENABLE,
        D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);

    void Run(IDirect3DDevice9* dev, IDirect3DSurface9* bb, UINT w, UINT h, float saturation)
    {
        auto* d = dev->lpVtbl;

        // --- save ---------------------------------------------------------------------------------
        IDirect3DSurface9* oldRT = nullptr;
        IDirect3DSurface9* oldDS = nullptr;
        d->GetRenderTarget(dev, 0, &oldRT);
        d->GetDepthStencilSurface(dev, &oldDS);   // may be null
        g_sb->lpVtbl->Capture(g_sb);
        DWORD saved[kTouchedCount];
        for (int i = 0; i < kTouchedCount; ++i)
            d->GetRenderState(dev, kTouched[i], &saved[i]);
        IDirect3DBaseTexture9*       oldTex0 = nullptr;
        IDirect3DVertexShader9*      oldVS   = nullptr;
        IDirect3DVertexDeclaration9* oldDecl = nullptr;
        DWORD                        oldFVF  = 0;
        d->GetTexture(dev, 0, &oldTex0);
        d->GetVertexShader(dev, &oldVS);
        d->GetVertexDeclaration(dev, &oldDecl);
        d->GetFVF(dev, &oldFVF);

        // --- the pass -----------------------------------------------------------------------------
        // A multisampled back buffer is resolved by the same call.
        d->StretchRect(dev, bb, nullptr, g_copySurf, nullptr, D3DTEXF_NONE);
        const bool began = SUCCEEDED(d->BeginScene(dev));   // fails harmlessly mid-scene
        d->SetRenderTarget(dev, 0, bb);
        d->SetDepthStencilSurface(dev, nullptr);
        d->SetRenderState(dev, D3DRS_ZENABLE,           D3DZB_FALSE);
        d->SetRenderState(dev, D3DRS_ZWRITEENABLE,      FALSE);
        d->SetRenderState(dev, D3DRS_ALPHATESTENABLE,   FALSE);
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,  FALSE);
        d->SetRenderState(dev, D3DRS_CULLMODE,          D3DCULL_NONE);
        d->SetRenderState(dev, D3DRS_FOGENABLE,         FALSE);
        d->SetRenderState(dev, D3DRS_STENCILENABLE,     FALSE);
        d->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetRenderState(dev, D3DRS_COLORWRITEENABLE,  D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                        D3DCOLORWRITEENABLE_BLUE);
        d->SetRenderState(dev, D3DRS_SRGBWRITEENABLE,   FALSE);
        d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, 0);
        d->SetVertexShader(dev, nullptr);
        d->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
        d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_copy));
        d->SetPixelShader(dev, g_ps);
        const float k[4] = { saturation, 0.0f, 0.0f, 0.0f };
        d->SetPixelShaderConstantF(dev, 0, k, 1);
        // Pre-transformed, with D3D9's half-pixel offset so texels land on pixels exactly.
        const float fw = static_cast<float>(w) - 0.5f, fh = static_cast<float>(h) - 0.5f;
        const QuadVertex q[4] = {
            { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
            {  fw,   -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f,  fh,   0.0f, 1.0f, 0.0f, 1.0f },
            {  fw,    fh,   0.0f, 1.0f, 1.0f, 1.0f },
        };
        d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
        if (began)
            d->EndScene(dev);

        // --- restore ------------------------------------------------------------------------------
        // Through the vtable first, so every hook's mirror sees the client's values again; then the state
        // block puts the device back exactly.
        for (int i = 0; i < kTouchedCount; ++i)
            d->SetRenderState(dev, kTouched[i], saved[i]);
        d->SetTexture(dev, 0, oldTex0);
        d->SetVertexShader(dev, oldVS);
        d->SetFVF(dev, oldFVF);
        if (oldDecl)
            d->SetVertexDeclaration(dev, oldDecl);   // after SetFVF, which would otherwise replace it
        d->SetRenderTarget(dev, 0, oldRT);
        d->SetDepthStencilSurface(dev, oldDS);
        g_sb->lpVtbl->Apply(g_sb);
        SafeRelease(oldRT);
        SafeRelease(oldDS);
        SafeRelease(oldTex0);
        SafeRelease(oldVS);
        SafeRelease(oldDecl);
    }
}

float GradeSaturationNow()
{
    return g_now;
}

bool GradeBeforeUI(IDirect3DDevice9* dev)
{
    // The Color Effects box off: nothing at all, not even the clock read.
    if (!g_cfg.colour.enabled)
        return false;
    Ease();
    float pl[3];
    if (!g_cfg.master || g_failed || fabsf(g_now - 1.0f) < 0.002f || DebugViewOn() || !ClientPlayer(pl))
        return false;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return false;
    D3DSURFACE_DESC desc = {};
    bb->lpVtbl->GetDesc(bb, &desc);
    bool drawn = false;
    if (!MakeShader(dev) || !MakeCopy(dev, desc) ||
        (!g_sb && (FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)) || !g_sb)))
    {
        // A missing effect, never a black screen.
        g_failed = true;
        Log("grade: resources unavailable: no saturation until the next device reset");
    }
    else
    {
        Run(dev, bb, desc.Width, desc.Height, g_now);
        drawn = true;
    }
    bb->lpVtbl->Release(bb);
    return drawn;
}

void GradeReset()
{
    SafeRelease(g_copySurf);
    SafeRelease(g_copy);
    SafeRelease(g_sb);
    SafeRelease(g_ps);
    g_copyW = g_copyH = 0;
    g_copyFmt = D3DFMT_UNKNOWN;
    g_failed = false;
}

// The shaders this pass compiles, as it compiles them, for the cache's worker (shadercache.cpp, 2026-10-06).
void GradeShaderList()
{
    ShaderPrecompile("grade", kGradeHlsl, "ps_2_0");
}
