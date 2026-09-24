// rays: sun shafts for the 1.12 client, as a post-process.
//
// GPU Gems 3, "Volumetric Light Scattering as a Post-Process": radial blur toward the sun's position on
// screen. Four steps, all at reduced resolution except the last:
//
//   1. StretchRect the back buffer down into a small render target.
//      With [rays] skyOnly = 1, only the sky casts, not the clouds: comfyfog.cpp calls RaysBeforeClouds
//      just before the client draws its cloud layer. A probe shows the frame's first three draws: the sun
//      sprite, the sky dome (added), then the cloud layer (alpha-blended); the world follows. The image at
//      the cloud draw is kept at the same size, and each pixel then casts from whichever is darker, the
//      finished frame or that sky. Trees and terrain are darker than the sky behind them and still block
//      it; a lit cloud is brighter than the sky behind it and casts no more than that sky. The kept sky is
//      also free of the client's Full-Screen Glow, which brightens everything near the sun in the finished
//      frame, so it is dimmer: at relThreshold 0.45 only the sun disk cast. debugView 3 shows the kept sky.
//   2. Mask: keep bright pixels, weighted by how near they sit to the sun. The occlusion mask is luminance,
//      not depth: sky near the sun is bright and trees are dark, so the silhouette falls out for free (see
//      NOTES.md). The mask does not need [depth], so the rays work with the volumetric light off.
//      "Bright" is relative to the brightest pixel in the frame, found on the GPU by a chain of
//      max-reductions: under a forest canopy with heavy fog nothing reaches a fixed threshold (the fog colour
//      itself sat at 0.33), yet the sky gaps are still the brightest thing in view and are exactly what
//      should cast.
//   3. Radial blur toward the sun, in passes of 16 samples whose step grows by 16x each pass, so three
//      passes cover the ray length with 4096 effective taps and no banding.
//   4. Add the result back over the back buffer.
//
// It runs at the world -> UI boundary: comfyfog.cpp arms it at the first switch from a perspective to an
// orthographic projection each frame, and fires it before the first draw after that which targets the
// back buffer with no pixel shader: after the client's full-screen glow, before any UI. That is
// mid-scene: the client's BeginScene is still open, so the pass does not open one of its own.
// placement = 1 runs it at Present instead, over the UI, as a fallback.
//
// The rays were removed on 2026-09-23, when the volumetric light replaced them, and put back on
// 2026-09-24 because players asked for them. The sun and the camera now come from sun.cpp, which the
// shadow map and the volumetric light share, so [sun] fixed pins the rays too.
//
// The pass touches a lot of device state the client's fixed-function pipeline needs back exactly, so it
// is wrapped in a D3DSBT_ALL state block (captured before, applied after) plus the render target and
// depth surface, which state blocks do not cover. comfygrass and comfyfog both mirror some state out of
// their hooks, and the state block restores the device without going through those hooks. So the
// mirrored pieces are also re-set through the vtable with their saved values, keeping every mirror true.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "common.h"
#include "config.h"
#include "rays.h"
#include "sun.h"

#include <cmath>
#include <cstring>

namespace
{
    constexpr int kSamples = 16;   // per blur pass; the ps_2_0 shader below is unrolled to exactly this

    const char* kMaskHlsl = R"HLSL(
sampler2D s0 : register(s0);   // the scene, downsampled
sampler2D s1 : register(s1);   // 1x1: the brightest luminance in the frame
float4 gSun : register(c0);   // entry point uv.xy, 1/radius, absolute threshold floor
float4 gP   : register(c1);   // unused, aspect (w/h), relative threshold, falloff exponent
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float3 c    = tex2D(s0, uv).rgb;
    float  l    = dot(c, float3(0.299, 0.587, 0.114));
    float  peak = tex2D(s1, float2(0.5, 0.5)).r;
    float  thr  = max(gSun.w, peak * gP.z);
    float  m    = saturate((l - thr) / max(peak - thr, 0.02));
    float2 d = uv - gSun.xy;
    d.x *= gP.y;
    float  f = pow(saturate(1.0 - length(d) * gSun.z), gP.w);
    return float4(c * (m * f), 1.0);
}
)HLSL";

    const char* kBlurHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gB : register(c0);     // radial step, decay, 1 / sum of the weights, unused
float4 gO : register(c1);     // delta offset: -sun * radial step + parallel step (see Run)
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    // delta = (uv - sun) * radialStep + away * parallelStep, folded into one multiply-add
    float2 delta = uv * gB.x + gO.xy;
    float3 sum = 0;
    float  w   = 1.0;
    for (int i = 0; i < 16; ++i)
    {
        sum += tex2D(s0, uv).rgb * w;
        w   *= gB.y;
        uv  -= delta;
    }
    return float4(sum * gB.z, 1.0);
}
)HLSL";

    // Max-reduction toward the frame's brightest luminance. The first step takes 2x2 texels to
    // luminance (a 4x4 version runs out of ps_2_0 temp registers with the dot products); the rest take
    // 4x4 of the already-reduced value. gT.xy is the source texel size; sampling is point-filtered.
    const char* kMaxLumHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gT : register(c0);
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    const float3 k = float3(0.299, 0.587, 0.114);
    float a = dot(tex2D(s0, uv + gT.xy * float2(-0.5, -0.5)).rgb, k);
    float b = dot(tex2D(s0, uv + gT.xy * float2( 0.5, -0.5)).rgb, k);
    float c = dot(tex2D(s0, uv + gT.xy * float2(-0.5,  0.5)).rgb, k);
    float d = dot(tex2D(s0, uv + gT.xy * float2( 0.5,  0.5)).rgb, k);
    float m = max(max(a, b), max(c, d));
    return float4(m, m, m, 1.0);
}
)HLSL";

    const char* kMaxHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gT : register(c0);
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float m = 0.0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
        {
            float x = i - 1.5, y = j - 1.5;
            m = max(m, tex2D(s0, uv + gT.xy * float2(x, y)).r);
        }
    return float4(m, m, m, 1.0);
}
)HLSL";

    // Eases the brightness reference toward this frame's peak instead of jumping to it: drawn into a
    // persistent 1x1 float target with alpha blending, alpha = the per-frame rate.
    // Per pixel, the darker (by luminance) of the finished frame and the sky before the clouds.
    const char* kSkyMinHlsl = R"HLSL(
sampler2D s0 : register(s0);   // the finished frame, downsampled
sampler2D s1 : register(s1);   // the sky before the clouds, downsampled
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    const float3 k = float3(0.299, 0.587, 0.114);
    float3 f = tex2D(s0, uv).rgb;
    float3 s = tex2D(s1, uv).rgb;
    return float4(dot(f, k) < dot(s, k) ? f : s, 1.0);
}
)HLSL";

    const char* kPeakBlendHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gA : register(c0);     // rate, unused...
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float p = tex2D(s0, float2(0.5, 0.5)).r;
    return float4(p, p, p, gA.x);
}
)HLSL";

    const char* kCompositeHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gC : register(c0);     // rgb gain: colour x exposure x fade
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    return float4(tex2D(s0, uv).rgb * gC.rgb, 1.0);
}
)HLSL";

    struct Target
    {
        IDirect3DTexture9* tex  = nullptr;
        IDirect3DSurface9* surf = nullptr;
    };

    Target g_scene, g_ping, g_pong;
    Target g_sky;                       // the sky before the clouds, this frame ([rays] skyOnly)
    bool   g_skyCaptured = false;       // g_sky holds this frame's sky
    UINT   g_rw = 0, g_rh = 0;          // reduced size the targets were built at

    constexpr int kMaxLevels = 10;
    Target g_peak[kMaxLevels];          // max-reduction chain; the last level is 1x1
    UINT   g_peakW[kMaxLevels] = {}, g_peakH[kMaxLevels] = {};
    int    g_peakLevels = 0;
    IDirect3DPixelShader9* g_psMaxLum = nullptr;
    IDirect3DPixelShader9* g_psMax    = nullptr;
    IDirect3DPixelShader9* g_psPeakBlend = nullptr;
    IDirect3DPixelShader9* g_psSkyMin = nullptr;
    Target g_peakSmooth;               // the eased peak, 1x1, 16-bit float so slow easing does not stall
    bool   g_peakSmoothInit = false;    // first frame after (re)creation takes the peak outright
    double g_peakLastTime   = 0.0;
    bool   g_logPeak = false;           // the probe asked for this frame's brightest value

    IDirect3DPixelShader9* g_psMask = nullptr;
    IDirect3DPixelShader9* g_psBlur = nullptr;
    IDirect3DPixelShader9* g_psComp = nullptr;
    bool                   g_shadersTried = false;

    IDirect3DStateBlock9*  g_sb = nullptr;

    bool g_on     = true;   // Ctrl+reload toggles
    bool g_failed = false;  // a resource could not be made: stay out of the way until reset or reload

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    void ReleaseTarget(Target& t)
    {
        SafeRelease(t.surf);
        SafeRelease(t.tex);
    }

    void ReleaseDefaultPool()
    {
        ReleaseTarget(g_scene);
        ReleaseTarget(g_sky);
        g_skyCaptured = false;
        ReleaseTarget(g_ping);
        ReleaseTarget(g_pong);
        for (int i = 0; i < kMaxLevels; ++i)
            ReleaseTarget(g_peak[i]);
        g_peakLevels = 0;
        ReleaseTarget(g_peakSmooth);
        g_peakSmoothInit = false;
        SafeRelease(g_sb);
        g_rw = g_rh = 0;
    }

    IDirect3DPixelShader9* MakePixelShader(IDirect3DDevice9* dev, const char* src, const char* name)
    {
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        if (!compile)
        {
            Log("rays: d3dcompiler_47 unavailable");
            return nullptr;
        }
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        const HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &errs);
        if (FAILED(hr) || !code)
        {
            Log("rays: %s failed to compile hr=0x%08X: %s", name, hr,
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
        if (FAILED(chr))
        {
            Log("rays: CreatePixelShader(%s) failed hr=0x%08X", name, chr);
            return nullptr;
        }
        return ps;
    }

    bool MakeTarget(IDirect3DDevice9* dev, UINT w, UINT h, Target& t, D3DFORMAT fmt = D3DFMT_A8R8G8B8)
    {
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, fmt,
                                                D3DPOOL_DEFAULT, &t.tex, nullptr);
        if (SUCCEEDED(hr))
            hr = t.tex->lpVtbl->GetSurfaceLevel(t.tex, 0, &t.surf);
        if (FAILED(hr))
        {
            Log("rays: render target %ux%u failed hr=0x%08X", w, h, hr);
            ReleaseTarget(t);
            return false;
        }
        return true;
    }

    // Everything the pass needs, built lazily and rebuilt when the back buffer or downscale changes.
    bool EnsureResources(IDirect3DDevice9* dev, UINT bbW, UINT bbH)
    {
        if (!g_shadersTried)
        {
            g_shadersTried = true;
            g_psMask = MakePixelShader(dev, kMaskHlsl,      "rays_mask");
            g_psBlur = MakePixelShader(dev, kBlurHlsl,      "rays_blur");
            g_psComp = MakePixelShader(dev, kCompositeHlsl, "rays_composite");
            g_psMaxLum = MakePixelShader(dev, kMaxLumHlsl,  "rays_maxlum");
            g_psMax    = MakePixelShader(dev, kMaxHlsl,     "rays_max");
            g_psPeakBlend = MakePixelShader(dev, kPeakBlendHlsl, "rays_peakblend");
            g_psSkyMin = MakePixelShader(dev, kSkyMinHlsl, "rays_skymin");
            if (g_psMask && g_psBlur && g_psComp)
                Log("rays: shaders compiled");
        }
        if (!g_psMask || !g_psBlur || !g_psComp || !g_psMaxLum || !g_psMax || !g_psPeakBlend || !g_psSkyMin)
            return false;

        const UINT ds = static_cast<UINT>(g_cfg.rays.downscale);
        const UINT rw = bbW / ds > 0 ? bbW / ds : 1;
        const UINT rh = bbH / ds > 0 ? bbH / ds : 1;
        if (rw == g_rw && rh == g_rh && g_scene.surf && g_ping.surf && g_pong.surf && g_sky.surf && g_sb)
            return true;

        ReleaseDefaultPool();
        if (!MakeTarget(dev, rw, rh, g_scene) || !MakeTarget(dev, rw, rh, g_ping) || !MakeTarget(dev, rw, rh, g_pong) ||
            !MakeTarget(dev, rw, rh, g_sky))
            return false;

        // Halve once (2x2 luminance), then quarter (4x4) down to a single texel.
        UINT pw = (rw + 1) / 2, ph = (rh + 1) / 2;
        for (g_peakLevels = 0; g_peakLevels < kMaxLevels; )
        {
            if (!MakeTarget(dev, pw, ph, g_peak[g_peakLevels]))
                return false;
            g_peakW[g_peakLevels] = pw; g_peakH[g_peakLevels] = ph;
            ++g_peakLevels;
            if (pw == 1 && ph == 1)
                break;
            pw = (pw + 3) / 4; ph = (ph + 3) / 4;
        }
        if (!MakeTarget(dev, 1, 1, g_peakSmooth, D3DFMT_A16B16G16R16F) && !MakeTarget(dev, 1, 1, g_peakSmooth))
            return false;
        if (FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)) || !g_sb)
        {
            Log("rays: CreateStateBlock failed");
            return false;
        }
        g_rw = rw; g_rh = rh;
        Log("rays: targets built at %ux%u for a %ux%u back buffer", rw, rh, bbW, bbH);
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    // where the sun is on screen

    // Row-vector times matrix, D3D9's convention.
    void Transform4(const float in[4], const D3DMATRIX& m, float out[4])
    {
        for (int c = 0; c < 4; ++c)
            out[c] = in[0] * m.m[0][c] + in[1] * m.m[1][c] + in[2] * m.m[2][c] + in[3] * m.m[3][c];
    }

    inline float Sat(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    // Where the blur converges, where "near the sun" is measured from, and how strongly to draw.
    //
    // Shafts are parallel in the world, so on screen they all run through one point: the sun when it is
    // in front of the camera, the point opposite it (the antisolar point) when it is behind. Radial blur
    // only needs that line family, plus which way along it to gather: toward the sun. So a sun behind
    // the camera is blurred about its antisolar point with the gather direction reversed, and the two
    // cases meet seamlessly as the sun crosses the side of the view: both run off to infinity in the same
    // screen direction there. That is what lets rays stream down through a canopy with the sun overhead
    // and out of view, instead of switching off the moment the sun leaves the screen.
    struct SunScreen
    {
        float px, py;        // convergence point, texture space (y down); clamped to kMaxDistance of centre
        float ex, ey;        // where the sun enters the screen; the sun itself when it is on screen
        float gather;        // +1 gather toward (px,py), -1 away from it (sun behind the camera)
        float lengthFrac;    // blur length as a fraction of each pixel's distance to (px,py)
        float fade;          // 0..1
        float awayX, awayY;  // unit direction away from the sun on screen, texture space (aspect-corrected)
        float parallel;      // 0..1 share of the blur that runs along (awayX, awayY) instead of radially
    };

    constexpr float kMaxDistance = 3.0f;   // texture-space units from centre; rays are ~parallel by then

    void Finish(SunScreen& s, float tx, float ty, float aspect)
    {
        const RaysSettings& r = g_cfg.rays;

        // Clamp the convergence point: far enough that the rays are near parallel, near enough that the
        // blur step stays meaningful.
        float dx = s.px - 0.5f, dy = s.py - 0.5f;
        const float d = sqrtf(dx * dx + dy * dy);
        if (d > kMaxDistance)
        {
            s.px = 0.5f + dx * (kMaxDistance / d);
            s.py = 0.5f + dy * (kMaxDistance / d);
            dx = s.px - 0.5f; dy = s.py - 0.5f;
        }

        // The entry point: the sun itself if it is on screen, otherwise where a line from the screen
        // centre toward the sun crosses the edge. (tx, ty) is that toward-the-sun direction.
        const bool onScreen = s.gather > 0.0f && s.px >= 0.0f && s.px <= 1.0f && s.py >= 0.0f && s.py <= 1.0f;
        if (onScreen)
        {
            s.ex = s.px; s.ey = s.py;
        }
        else
        {
            const float ax = fabsf(tx) > 1e-6f ? 0.5f / fabsf(tx) : 1e9f;
            const float ay = fabsf(ty) > 1e-6f ? 0.5f / fabsf(ty) : 1e9f;
            const float t  = ax < ay ? ax : ay;
            s.ex = 0.5f + tx * t;
            s.ey = 0.5f + ty * t;
        }

        // Away from the sun, the way the parallel shafts fall. Measured in screen heights so it is not
        // squashed by the aspect ratio, then back to texture space. With the sun dead centre there is no
        // direction to fall in, so the parallel share fades out there.
        const float hx = tx * aspect, hy = ty;
        const float hl = sqrtf(hx * hx + hy * hy);
        if (hl > 1e-5f)
        {
            s.awayX = -(hx / hl) / aspect;
            s.awayY = -(hy / hl);
        }
        else
        {
            s.awayX = 0.0f; s.awayY = 1.0f;
        }
        s.parallel = r.parallel * Sat(hl / 0.05f);

        // Rays may reach at most maxLength screen heights, measured at the screen centre, however far
        // away the convergence point is. A distant sun otherwise streaks the whole screen.
        const float dh = sqrtf(dx * aspect * dx * aspect + dy * dy);
        s.lengthFrac = r.length;
        if (dh > 1e-4f && r.maxLength / dh < s.lengthFrac)
            s.lengthFrac = r.maxLength / dh;
    }

    bool SunOnScreen(SunScreen& s, float aspect)
    {
        const RaysSettings& r = g_cfg.rays;
        s.fade   = 1.0f;
        s.gather = 1.0f;

        // The sky's sun (or [sun] fixed), and the world camera, both from sun.cpp. A direction, so
        // w = 0: the camera's position (folded into each world matrix by this client, see comfygrass's
        // README) drops out and only the view rotation matters.
        D3DMATRIX view, proj;
        float dir[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (!SunCamera(view, proj) || !SunDirection(dir))
            return false;

        float vs[4], clip[4];
        Transform4(dir, view, vs);
        Transform4(vs, proj, clip);

        // vs[2] is the cosine between the view axis and the sun. Intensity follows the view: full when
        // looking straight at the sun, easing down as you turn away, nothing at maxAngle: one smooth
        // curve, shaped by viewFalloff. And fading as the sun sets, gone 5 degrees below the horizon.
        const float cosMax = cosf(r.maxAngle * 0.01745329f);
        s.fade = powf(Sat((vs[2] - cosMax) / (1.0f - cosMax > 1e-4f ? 1.0f - cosMax : 1e-4f)), r.viewFalloff);
        s.fade *= Sat(1.0f + dir[2] / 0.087f);
        if (s.fade <= 0.001f)
            return false;

        // Toward-the-sun direction on screen, in NDC: clip.xy / |w| in both halves. In front, the sun is
        // that point; behind, the antisolar point is its mirror, gathered away from.
        const float aw = fabsf(clip[3]) > 1e-4f ? fabsf(clip[3]) : 1e-4f;
        const float tnx = clip[0] / aw, tny = clip[1] / aw;
        const float tx = tnx * 0.5f, ty = -tny * 0.5f;          // the same direction in texture space
        if (clip[3] > 0.0f)
        {
            s.px = 0.5f + tx; s.py = 0.5f + ty;
        }
        else
        {
            s.px = 0.5f - tx; s.py = 0.5f - ty;
            s.gather = -1.0f;
        }
        Finish(s, tx, ty, aspect);
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    // the pass

    struct QuadVertex { float x, y, z, rhw, u, v; };

    void DrawQuad(IDirect3DDevice9* dev, UINT w, UINT h)
    {
        // Pre-transformed, with D3D9's half-pixel offset so texels land on pixels exactly.
        const float fw = static_cast<float>(w) - 0.5f, fh = static_cast<float>(h) - 0.5f;
        const QuadVertex q[4] = {
            { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
            {  fw,   -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f,  fh,   0.0f, 1.0f, 0.0f, 1.0f },
            {  fw,    fh,   0.0f, 1.0f, 1.0f, 1.0f },
        };
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
    }

    void Pass(IDirect3DDevice9* dev, IDirect3DTexture9* src, IDirect3DSurface9* dst, UINT w, UINT h,
              IDirect3DPixelShader9* ps, const float* consts, UINT nConsts)
    {
        dev->lpVtbl->SetRenderTarget(dev, 0, dst);
        dev->lpVtbl->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(src));
        dev->lpVtbl->SetPixelShader(dev, ps);
        dev->lpVtbl->SetPixelShaderConstantF(dev, 0, consts, nConsts);
        DrawQuad(dev, w, h);
    }

    // Render states the pass changes. Also the ones re-set afterwards to keep other hooks' mirrors true.
    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE,
        D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_FOGENABLE,
        D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);

    // Probe only: read the 1x1 peak back and log it, with the threshold it produced. A stall, which is
    // why it happens on the probe frame and nowhere else.
    void LogPeak(IDirect3DDevice9* dev)
    {
        g_logPeak = false;
        IDirect3DSurface9* sys = nullptr;
        if (FAILED(dev->lpVtbl->CreateOffscreenPlainSurface(dev, 1, 1, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)))
            return;
        if (SUCCEEDED(dev->lpVtbl->GetRenderTargetData(dev, g_peak[g_peakLevels - 1].surf, sys)))
        {
            D3DLOCKED_RECT lr = {};
            if (SUCCEEDED(sys->lpVtbl->LockRect(sys, &lr, nullptr, D3DLOCK_READONLY)))
            {
                const float peak = ((*static_cast<const DWORD*>(lr.pBits) >> 16) & 0xFF) / 255.0f;
                sys->lpVtbl->UnlockRect(sys);
                const RaysSettings& r = g_cfg.rays;
                const float thr = r.threshold > peak * r.relThreshold ? r.threshold : peak * r.relThreshold;
                Log("rays: brightest pixel this frame %.3f -> casting from luminance %.3f and up%s", peak, thr,
                    thr >= peak ? " (nothing: the brightest pixel is under the absolute threshold)" : "");
            }
        }
        sys->lpVtbl->Release(sys);
    }

    void Run(IDirect3DDevice9* dev, IDirect3DSurface9* bb, UINT bbW, UINT bbH, const SunScreen& sun)
    {
        const RaysSettings& r = g_cfg.rays;
        auto* d = dev->lpVtbl;

        // --- save ---------------------------------------------------------------------------------
        IDirect3DSurface9* oldRT = nullptr;
        IDirect3DSurface9* oldDS = nullptr;
        d->GetRenderTarget(dev, 0, &oldRT);
        d->GetDepthStencilSurface(dev, &oldDS);   // may legitimately be null
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

        // --- scene, downsampled -----------------------------------------------------------------
        d->StretchRect(dev, bb, nullptr, g_scene.surf, nullptr, D3DTEXF_LINEAR);

        const bool began = SUCCEEDED(d->BeginScene(dev));

        d->SetDepthStencilSurface(dev, nullptr);
        d->SetRenderState(dev, D3DRS_ZENABLE,           D3DZB_FALSE);
        d->SetRenderState(dev, D3DRS_ZWRITEENABLE,      FALSE);
        d->SetRenderState(dev, D3DRS_ALPHATESTENABLE,   FALSE);
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,  FALSE);
        d->SetRenderState(dev, D3DRS_CULLMODE,          D3DCULL_NONE);
        d->SetRenderState(dev, D3DRS_FOGENABLE,         FALSE);
        d->SetRenderState(dev, D3DRS_STENCILENABLE,     FALSE);
        d->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetRenderState(dev, D3DRS_COLORWRITEENABLE,  0xF);
        d->SetRenderState(dev, D3DRS_SRGBWRITEENABLE,   FALSE);
        d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, 0);
        d->SetVertexShader(dev, nullptr);
        d->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);

        // --- only the sky casts ([rays] skyOnly) ---------------------------------------------------
        // The darker of the frame and the sky before the clouds, into g_pong, which the blur does not
        // write before the mask has read it.
        const Target* scene = &g_scene;
        if (r.skyOnly && g_skyCaptured)
        {
            d->SetTexture(dev, 1, reinterpret_cast<IDirect3DBaseTexture9*>(g_sky.tex));
            d->SetSamplerState(dev, 1, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
            d->SetSamplerState(dev, 1, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
            d->SetSamplerState(dev, 1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            d->SetSamplerState(dev, 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            d->SetSamplerState(dev, 1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            d->SetSamplerState(dev, 1, D3DSAMP_SRGBTEXTURE, 0);
            const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            Pass(dev, g_scene.tex, g_pong.surf, g_rw, g_rh, g_psSkyMin, none, 1);
            d->SetTexture(dev, 1, nullptr);
            scene = &g_pong;
        }

        // --- brightest pixel -----------------------------------------------------------------------
        d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        {
            IDirect3DTexture9* src = scene->tex;
            UINT sw = g_rw, sh = g_rh;
            for (int i = 0; i < g_peakLevels; ++i)
            {
                const float tc[4] = { 1.0f / sw, 1.0f / sh, 0.0f, 0.0f };
                Pass(dev, src, g_peak[i].surf, g_peakW[i], g_peakH[i], i == 0 ? g_psMaxLum : g_psMax, tc, 1);
                src = g_peak[i].tex;
                sw = g_peakW[i]; sh = g_peakH[i];
            }
        }
        if (g_logPeak)
            LogPeak(dev);

        // Ease the reference: 1 - e^(-dt/adaptTime) of the way to this frame's peak, per frame.
        {
            const double now = Now();
            float rate = 1.0f;
            if (g_peakSmoothInit && r.adaptTime > 0.0f)
            {
                double dt = now - g_peakLastTime;
                if (dt < 0.0) dt = 0.0;
                if (dt > 0.5) dt = 0.5;
                rate = 1.0f - expf(-static_cast<float>(dt) / r.adaptTime);
            }
            g_peakSmoothInit = true;
            g_peakLastTime   = now;

            const float ac[4] = { rate, 0.0f, 0.0f, 0.0f };
            d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
            d->SetRenderState(dev, D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
            d->SetRenderState(dev, D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
            d->SetRenderState(dev, D3DRS_BLENDOP,          D3DBLENDOP_ADD);
            Pass(dev, g_peak[g_peakLevels - 1].tex, g_peakSmooth.surf, 1, 1, g_psPeakBlend, ac, 1);
            d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        }
        d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

        // --- mask ---------------------------------------------------------------------------------
        IDirect3DTexture9* peak = g_peakSmooth.tex;
        d->SetTexture(dev, 1, reinterpret_cast<IDirect3DBaseTexture9*>(peak));
        d->SetSamplerState(dev, 1, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, 1, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, 1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, 1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(dev, 1, D3DSAMP_SRGBTEXTURE, 0);
        const float maskC[8] = {
            sun.ex, sun.ey, 1.0f / (r.radius > 0.05f ? r.radius : 0.05f), r.threshold,
            0.0f, static_cast<float>(bbW) / static_cast<float>(bbH), r.relThreshold, r.falloff,
        };
        Pass(dev, scene->tex, g_ping.surf, g_rw, g_rh, g_psMask, maskC, 2);
        d->SetTexture(dev, 1, nullptr);

        // --- radial blur --------------------------------------------------------------------------
        // Step per sample for pass p is length / 16^(passes - p): the last pass spans the full length
        // in 16 steps, and each earlier pass fills in the gaps between the next one's samples.
        Target* src = &g_ping;
        Target* dst = &g_pong;
        if (r.debugView != 1)
        {
            for (int p = 0; p < r.passes; ++p)
            {
                const bool  last  = (p == r.passes - 1);
                // Radial and parallel shares of this pass's per-sample step. Radial: a fraction of each
                // pixel's way to the sun. Parallel: a fixed stride along the away-from-sun direction,
                // maxLength screen heights over the full ray, the same for every pixel.
                const float k       = powf(static_cast<float>(kSamples), static_cast<float>(r.passes - p));
                const float stepR   = sun.gather * sun.lengthFrac / k * (1.0f - sun.parallel);
                const float stepP   = sun.parallel * r.maxLength / k;
                const float decay   = last ? r.decay : 1.0f;
                float wsum = 0.0f, w = 1.0f;
                for (int i = 0; i < kSamples; ++i) { wsum += w; w *= decay; }
                const float blurC[8] = {
                    stepR, decay, 1.0f / wsum, 0.0f,
                    -sun.px * stepR + sun.awayX * stepP, -sun.py * stepR + sun.awayY * stepP, 0.0f, 0.0f,
                };
                Pass(dev, src->tex, dst->surf, g_rw, g_rh, g_psBlur, blurC, 2);
                Target* t = src; src = dst; dst = t;
            }
        }

        // --- composite ----------------------------------------------------------------------------
        // debugView 1 shows the mask, 2 the rays and 3 the sky kept before the clouds, each replacing the
        // frame, for tuning.
        // The dial is squared. Linear, most of its range changed nothing visible: the sky near the sun is
        // already close to white, and the shafts over dark trees are faint averages, so doubling them was
        // still faint. Squared, the low end stays fine and the top end reaches far brighter shafts.
        const float dial     = r.strength * 0.01f;
        const float exposure = dial * dial * r.maxExposure * sun.fade;
        const DWORD col = r.color;
        float gain[4] = { ((col >> 16) & 0xFF) / 255.0f * exposure,
                          ((col >>  8) & 0xFF) / 255.0f * exposure,
                          ((col      ) & 0xFF) / 255.0f * exposure, 1.0f };
        if (r.debugView)
            gain[0] = gain[1] = gain[2] = 1.0f;

        d->SetRenderTarget(dev, 0, bb);
        if (!r.debugView)
        {
            d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
            d->SetRenderState(dev, D3DRS_SRCBLEND,         D3DBLEND_ONE);
            d->SetRenderState(dev, D3DRS_DESTBLEND,        D3DBLEND_ONE);
            d->SetRenderState(dev, D3DRS_BLENDOP,          D3DBLENDOP_ADD);
        }
        const Target* shown = (r.debugView == 3 && g_skyCaptured) ? &g_sky : src;
        Pass(dev, shown->tex, bb, bbW, bbH, g_psComp, gain, 1);


        if (began)
            d->EndScene(dev);

        // --- restore ------------------------------------------------------------------------------
        // Through the vtable first, so every hook's mirror sees the client's values again; then the
        // state block puts the device back exactly, whatever the hooks did.
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

namespace
{
    bool g_ranThisFrame = false;

    // True when the rays were drawn this frame.
    bool RunIfWanted(IDirect3DDevice9* dev)
    {
        const RaysSettings& r = g_cfg.rays;
        if (!r.enabled || !g_on || g_failed || (r.strength <= 0.0f && !r.debugView))
            return false;

        IDirect3DSurface9* bb = nullptr;
        if (FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
            return false;

        D3DSURFACE_DESC desc = {};
        bb->lpVtbl->GetDesc(bb, &desc);

        SunScreen sun = {};
        const float aspect = desc.Height ? static_cast<float>(desc.Width) / static_cast<float>(desc.Height) : 1.0f;
        // No sun on screen or near it: behind the camera past maxAngle, below the horizon, or not seen yet.
        bool drawn = false;
        if (!SunOnScreen(sun, aspect))
        {
            drawn = false;
        }
        else if (!EnsureResources(dev, desc.Width, desc.Height))
        {
            // Pass through cleanly: a missing effect, never a black screen.
            g_failed = true;
            ReleaseDefaultPool();
            Log("rays: resources unavailable, rays off until the next reload (F11) or device reset");
        }
        else
        {
            Run(dev, bb, desc.Width, desc.Height, sun);
            drawn = true;
        }
        bb->lpVtbl->Release(bb);
        return drawn;
    }
}

void RaysProbe()
{
    g_logPeak = true;
}

bool RaysBeforeUI(IDirect3DDevice9* dev)
{
    if (g_cfg.rays.placement != 0 || g_ranThisFrame)
        return false;
    g_ranThisFrame = true;
    return RunIfWanted(dev);
}

void RaysBeforeClouds(IDirect3DDevice9* dev)
{
    const RaysSettings& r = g_cfg.rays;
    if (!(r.skyOnly || r.debugView == 3) || g_skyCaptured || !r.enabled || !g_on || g_failed ||
        (r.strength <= 0.0f && !r.debugView))
        return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return;
    D3DSURFACE_DESC desc = {};
    bb->lpVtbl->GetDesc(bb, &desc);
    bb->lpVtbl->Release(bb);
    if (!EnsureResources(dev, desc.Width, desc.Height))
        return;                           // the rays pass itself reports and gives up
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(dev->lpVtbl->GetRenderTarget(dev, 0, &rt)) && rt)
    {
        g_skyCaptured = SUCCEEDED(dev->lpVtbl->StretchRect(dev, rt, nullptr, g_sky.surf, nullptr, D3DTEXF_LINEAR));
        rt->lpVtbl->Release(rt);
    }
}

bool RaysPresent(IDirect3DDevice9* dev)
{
    // No boundary this frame (a loading screen, say) means no world to put rays on, so nothing is drawn.
    const bool drawn = g_cfg.rays.placement == 1 && RunIfWanted(dev);
    g_ranThisFrame = false;
    g_skyCaptured  = false;
    return drawn;
}

void RaysReset()
{
    ReleaseDefaultPool();
    g_failed = false;
}

void RaysReload()
{
    g_failed = false;   // retry after a failure; changed sizes are picked up by EnsureResources
}

void RaysToggle()
{
    g_on = !g_on;
    Log("--- rays %s ---", g_on ? "ON" : "OFF");
}
