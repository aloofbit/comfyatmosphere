// cover: how much of the sun is in view on screen, for the sun rays and the volumetric light.
//
// Nine points on and around the sun's disc on screen, [rays] occlusionRadius across. By depth when there
// is a readable depth buffer: the sky writes none and keeps the clear value 1, while the world is squeezed
// into a slice below 0.955 and the far horizon into 0.955..0.96 (see volume.cpp). Else by brightness: the
// finished frame against the sky kept before the world (rays.cpp, [rays] skyOnly), which heavy fog
// defeats, because a distant mountain takes the fog colour and near the sun that is nearly as bright as
// the sky.
//
// Why it exists. The rays: the mask keeps only sky, but the blur carries that sky toward the sun, so
// with the sun behind a mountain the sky above the ridge cast rays straight down over it. The light: its
// shadow map holds only what lies within [shadow] depth of you, so a sun setting behind the far horizon
// kept lighting the fog. Both added 2026-09-28.
//
// The share of the sky still in view is divided by [rays] occlusionFull and squared. Linear, a sun 80%
// behind a ridge still gave 57% of the rays and showed through the top of the mountain; squared it gives
// 33%, and a canopy open by occlusionFull or more still counts in full. The result is eased over time
// into one 1x1 texture per user, so a trunk walked past does not blink anything, and it stays on the GPU:
// the passes that use it sample it, and nothing waits on a read back.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "common.h"
#include "config.h"
#include "cover.h"
#include "shadercache.h"

#include <cmath>
#include <cstring>

namespace
{
    const char* kBrightHlsl = R"HLSL(
sampler2D s0 : register(s0);   // the finished frame, downsampled
sampler2D s1 : register(s1);   // the sky before the clouds, downsampled
float4 gV    : register(c0);   // sun uv.xy, 1 / the share that counts as fully in view, 1 = no test
float4 gW    : register(c1);   // unused here: the depth test's weighting
float4 gO[8] : register(c2);   // the other eight points, as offsets from the sun
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    const float3 k = float3(0.299, 0.587, 0.114);
    float ls  = dot(tex2D(s1, gV.xy).rgb, k);
    float all = ls;
    float kept = min(dot(tex2D(s0, gV.xy).rgb, k), ls);
    for (int i = 0; i < 8; ++i)
    {
        float2 p = gV.xy + gO[i].xy;
        ls    = dot(tex2D(s1, p).rgb, k);
        all  += ls;
        kept += min(dot(tex2D(s0, p).rgb, k), ls);
    }
    float v = saturate(kept / max(all, 0.001) * gV.z);
    v = max(v * v, gV.w);
    return float4(v, v, v, 1.0);
}
)HLSL";

    // Each point is weighted by the kept sky's brightness there, so the sun's own disc counts most.
    const char* kDepthHlsl = R"HLSL(
sampler2D s0 : register(s0);   // the world's depth (INTZ), full resolution
sampler2D s1 : register(s1);   // the sky before the clouds, downsampled: the weights
float4 gV    : register(c0);   // sun uv.xy, 1 / the share that counts as fully in view, 1 = no test
float4 gW    : register(c1);   // 1 = weight by the sky's brightness, 0 = all points the same
float4 gO[8] : register(c2);   // the other eight points, as offsets from the sun
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    const float3 k = float3(0.299, 0.587, 0.114);
    float w    = lerp(1.0, dot(tex2D(s1, gV.xy).rgb, k) + 0.05, gW.x);
    float all  = w;
    float kept = w * step(0.99, tex2D(s0, gV.xy).r);
    for (int i = 0; i < 8; ++i)
    {
        float2 p = gV.xy + gO[i].xy;
        w     = lerp(1.0, dot(tex2D(s1, p).rgb, k) + 0.05, gW.x);
        all  += w;
        kept += w * step(0.99, tex2D(s0, p).r);
    }
    float v = saturate(kept / all * gV.z);
    v = max(v * v, gV.w);
    return float4(v, v, v, 1.0);
}
)HLSL";

    // Moves the eased value toward this frame's: alpha blending with alpha = the per-frame rate.
    const char* kEaseHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gA : register(c0);     // rate, unused...
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float v = tex2D(s0, float2(0.5, 0.5)).r;
    return float4(v, v, v, gA.x);
}
)HLSL";

    const char* kSlotName[kCoverSlots] = { "rays, sun", "rays, second moon", "volumetric light" };

    struct Target
    {
        IDirect3DTexture9* tex  = nullptr;
        IDirect3DSurface9* surf = nullptr;
    };

    IDirect3DPixelShader9* g_psBright = nullptr;
    IDirect3DPixelShader9* g_psDepth  = nullptr;
    IDirect3DPixelShader9* g_psEase   = nullptr;
    bool   g_shadersTried = false;
    Target g_raw;                          // this measure, before easing
    Target g_eased[kCoverSlots];
    bool   g_init[kCoverSlots] = {};       // the first measure after (re)creation is taken outright
    double g_last[kCoverSlots] = {};
    bool   g_log[kCoverSlots]  = {};
    bool   g_failed = false;               // a target could not be made: give up until Reset

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    void ReleaseTarget(Target& t)
    {
        SafeRelease(t.surf);
        SafeRelease(t.tex);
    }

    IDirect3DPixelShader9* MakePixelShader(IDirect3DDevice9* dev, const char* src, const char* name,
                                           const char* profile)
    {
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        if (!compile)
            return nullptr;
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        const HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", profile, 0, 0, &code, &errs);
        if (FAILED(hr) || !code)
        {
            Log("cover: %s (%s) failed to compile hr=0x%08X: %s", name, profile, hr,
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no message)");
            if (errs) errs->lpVtbl->Release(errs);
            if (code) code->lpVtbl->Release(code);
            return nullptr;
        }
        if (errs) errs->lpVtbl->Release(errs);
        IDirect3DPixelShader9* ps = nullptr;
        const HRESULT chr = dev->lpVtbl->CreatePixelShader(
            dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &ps);
        code->lpVtbl->Release(code);
        return SUCCEEDED(chr) ? ps : nullptr;
    }

    // Nine points take most of ps_2_0's 64 arithmetic slots; ps_2_b has more.
    IDirect3DPixelShader9* MakeEither(IDirect3DDevice9* dev, const char* src, const char* name)
    {
        IDirect3DPixelShader9* ps = MakePixelShader(dev, src, name, "ps_2_0");
        return ps ? ps : MakePixelShader(dev, src, name, "ps_2_b");
    }

    bool MakeTarget(IDirect3DDevice9* dev, Target& t, D3DFORMAT fmt)
    {
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, 1, 1, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT,
                                                &t.tex, nullptr);
        if (SUCCEEDED(hr))
            hr = t.tex->lpVtbl->GetSurfaceLevel(t.tex, 0, &t.surf);
        if (FAILED(hr))
            ReleaseTarget(t);
        return SUCCEEDED(hr);
    }

    bool Ensure(IDirect3DDevice9* dev)
    {
        if (!g_shadersTried)
        {
            g_shadersTried = true;
            g_psBright = MakeEither(dev, kBrightHlsl, "cover_bright");
            // ps_2_b only (2026-10-07): it takes 81 arithmetic slots, past ps_2_0's 64, so that try always failed.
            g_psDepth  = MakePixelShader(dev, kDepthHlsl, "cover_depth", "ps_2_b");
            g_psEase   = MakePixelShader(dev, kEaseHlsl, "cover_ease", "ps_2_0");
        }
        if (!g_psBright || !g_psDepth || !g_psEase || g_failed)
            return false;
        if (g_raw.surf)
            return true;
        bool ok = MakeTarget(dev, g_raw, D3DFMT_A8R8G8B8);
        for (int i = 0; i < kCoverSlots && ok; ++i)
        {
            ok = MakeTarget(dev, g_eased[i], D3DFMT_A16B16G16R16F) || MakeTarget(dev, g_eased[i], D3DFMT_A8R8G8B8);
            g_init[i] = false;
        }
        if (!ok)
        {
            Log("cover: 1x1 targets could not be made; the rays and the light are not faded for a covered sun");
            CoverReset();
            g_failed = true;
        }
        return ok;
    }

    struct QuadVertex { float x, y, z, rhw, u, v; };

    void Pass(IDirect3DDevice9* dev, IDirect3DTexture9* src, IDirect3DSurface9* dst, IDirect3DPixelShader9* ps,
              const float* consts, UINT nConsts)
    {
        auto* d = dev->lpVtbl;
        d->SetRenderTarget(dev, 0, dst);
        d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(src));
        d->SetPixelShader(dev, ps);
        d->SetPixelShaderConstantF(dev, 0, consts, nConsts);
        const QuadVertex q[4] = {
            { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
            {  0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f,  0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
            {  0.5f,  0.5f, 0.0f, 1.0f, 1.0f, 1.0f },
        };
        d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
    }

    void SamplerClamp(IDirect3DDevice9* dev, DWORD s, D3DTEXTUREFILTERTYPE filter)
    {
        auto* d = dev->lpVtbl;
        d->SetSamplerState(dev, s, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, s, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, s, D3DSAMP_MINFILTER, filter);
        d->SetSamplerState(dev, s, D3DSAMP_MAGFILTER, filter);
        d->SetSamplerState(dev, s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(dev, s, D3DSAMP_SRGBTEXTURE, 0);
    }

    // Probe only: this measure, read back. A stall, so only on the probe frame.
    float ReadRaw(IDirect3DDevice9* dev)
    {
        float v = -1.0f;
        IDirect3DSurface9* sys = nullptr;
        if (FAILED(dev->lpVtbl->CreateOffscreenPlainSurface(dev, 1, 1, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)))
            return v;
        D3DLOCKED_RECT lr = {};
        if (SUCCEEDED(dev->lpVtbl->GetRenderTargetData(dev, g_raw.surf, sys)) &&
            SUCCEEDED(sys->lpVtbl->LockRect(sys, &lr, nullptr, D3DLOCK_READONLY)))
        {
            v = ((*static_cast<const DWORD*>(lr.pBits) >> 16) & 0xFF) / 255.0f;
            sys->lpVtbl->UnlockRect(sys);
        }
        sys->lpVtbl->Release(sys);
        return v;
    }
}

IDirect3DTexture9* CoverMeasure(IDirect3DDevice9* dev, CoverSlot slot, const CoverInput& in, float easeTime)
{
    if (!Ensure(dev))
        return nullptr;
    auto* d = dev->lpVtbl;
    const RaysSettings& r = g_cfg.rays;

    const bool byDepth = in.depth != nullptr;
    const bool test = in.test && (byDepth || (in.sky && in.scene));
    float c[4 * 10] = { in.px, in.py, 1.0f / r.occlusionFull, test ? 0.0f : 1.0f,
                        in.sky ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 8; ++i)
    {
        // Four points at half the radius, four at the full radius between them.
        const float a = (i < 4 ? i * 90.0f : i * 90.0f + 45.0f) * 0.01745329f;
        const float k = (i < 4 ? 0.5f : 1.0f) * r.occlusionRadius;
        c[8 + 4 * i]     = cosf(a) * k / (in.aspect > 0.0f ? in.aspect : 1.0f);
        c[8 + 4 * i + 1] = sinf(a) * k;
    }

    d->SetVertexShader(dev, nullptr);
    d->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
    d->SetTexture(dev, 1, reinterpret_cast<IDirect3DBaseTexture9*>(in.sky ? in.sky : in.scene));
    SamplerClamp(dev, 1, D3DTEXF_LINEAR);
    SamplerClamp(dev, 0, byDepth ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    Pass(dev, byDepth ? in.depth : in.scene, g_raw.surf, byDepth ? g_psDepth : g_psBright, c, 10);
    d->SetTexture(dev, 1, nullptr);

    if (g_log[slot])
    {
        g_log[slot] = false;
        Log("cover: %s: sun at screen (%.2f, %.2f), %s: in view %.2f (1 = full)", kSlotName[slot], in.px, in.py,
            !in.test ? "not tested (off screen or a debug view)" : byDepth ? "by depth" :
            test ? "by brightness" : "not tested (no depth and no kept sky)", ReadRaw(dev));
    }

    // Ease: 1 - e^(-dt/easeTime) of the way to this measure.
    const double now = Now();
    float rate = 1.0f;
    if (g_init[slot] && easeTime > 0.0f)
    {
        double dt = now - g_last[slot];
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.5) dt = 0.5;
        rate = 1.0f - expf(-static_cast<float>(dt) / easeTime);
    }
    g_init[slot] = true;
    g_last[slot] = now;
    const float ec[4] = { rate, 0.0f, 0.0f, 0.0f };
    SamplerClamp(dev, 0, D3DTEXF_POINT);
    d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(dev, D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
    d->SetRenderState(dev, D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
    d->SetRenderState(dev, D3DRS_BLENDOP,          D3DBLENDOP_ADD);
    Pass(dev, g_raw.tex, g_eased[slot].surf, g_psEase, ec, 1);
    d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetTexture(dev, 0, nullptr);
    return g_eased[slot].tex;
}

void CoverProbe()
{
    for (int i = 0; i < kCoverSlots; ++i)
        g_log[i] = true;
}

void CoverReset()
{
    ReleaseTarget(g_raw);
    for (int i = 0; i < kCoverSlots; ++i)
    {
        ReleaseTarget(g_eased[i]);
        g_init[i] = false;
    }
    g_failed = false;
}

// The shaders this pass compiles, as it compiles them, for the cache's worker (shadercache.cpp, 2026-10-06).
void CoverShaderList()
{
    ShaderPrecompile("cover_bright", kBrightHlsl, "ps_2_0");
    ShaderPrecompile("cover_depth", kDepthHlsl, "ps_2_b");
    ShaderPrecompile("cover_ease", kEaseHlsl, "ps_2_0");
}
