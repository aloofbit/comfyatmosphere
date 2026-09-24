// volume: volumetric light: the fog glowing where the sun reaches it.
//
// A lamp in a foggy room: the beam is not an object but the fog itself, lit. Walk around and it does not
// move, because it is defined by the lamp, the shade and the fog, not by where you stand. Here the lamp
// is the sun, the shade is the shadow map (shadow.cpp) and the fog is what the client already draws.
//
// Per pixel, at reduced resolution:
//   1. Read the scene's depth (depth.cpp's INTZ buffer) and rebuild the point it shows, camera-relative,
//      through the inverse of the client's view-projection. The line of sight runs from the camera to it,
//      clipped at maxDistance (the reach of the shadow map).
//   2. Step along it and ask the shadow map, at each step, whether that point in the air sees the sun.
//      The shadow projection is orthographic, so it is affine: the line's two ends are taken into shadow
//      space once and the steps interpolate between them. Outside the map counts as lit (no known
//      occluder). Each pixel starts its steps at a different offset (interleaved-gradient noise) so
//      banding turns into fine noise, and a small blur takes the noise out.
//   3. Lit length x density x a Henyey-Greenstein phase. Sunlight scatters forward, so the glow is
//      strongest looking toward the sun.
// The march writes the glow to red and the distance to the point it saw to green. The passes after it
// use that distance so the glow of one surface does not spread onto another at a different distance:
//   4. A small blur, whose taps count for less the further their distance is from the centre's.
//   5. Part of the last frame is kept ([volume] smooth). The noise offset turns a little each frame, so
//      the kept frames average the noise out rather than repeat it. The last frame is looked up where
//      the pixel's point was on the last frame's screen, found from the camera's movement, so a turn or
//      a step does not smear.
//   6. Added onto the world at full resolution in the [volume] colour, before glow and UI. Each pixel
//      takes the four low-resolution texels around it, weighted also by how near their distance is to
//      its own: a tree trunk in front of the sky takes the trunk's glow, not the sky's.
//
// The step loop needs Shader Model 3 (ps_2_0 fits about eight steps), and a ps_3_0 has to be paired
// with a vs_3_0, so the march, the temporal pass and the composite share a trivial full-screen vertex
// shader. The blur is ps_2_0 over pre-transformed quads, like the rest of comfyfog.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "common.h"
#include "config.h"
#include "depth.h"
#include "sun.h"
#include "shadow.h"
#include "volume.h"

#include <cmath>
#include <vector>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace
{
    const char* kMarchVsHlsl = R"HLSL(
float4 gHalf : register(c0);   // D3D9 half-pixel offset, in clip units
struct O { float4 pos : POSITION; float2 uv : TEXCOORD0; };
O main(float3 pos : POSITION, float2 uv : TEXCOORD0)
{
    O o;
    o.pos = float4(pos.xy + gHalf.xy, 0.0, 1.0);
    o.uv  = uv;
    return o;
}
)HLSL";

    const char* kMarchPsHlsl = R"HLSL(
sampler2D sDepth  : register(s0);   // the scene's depth (INTZ)
sampler2D sShadow : register(s1);   // the sun's depth (INTZ), border = far
float4 gInv0 : register(c0);        // rows of inverse(camera view-projection): clip -> camera-relative world
float4 gInv1 : register(c1);
float4 gInv2 : register(c2);
float4 gInv3 : register(c3);
float4 gSh0  : register(c4);        // rows of the shadow view-projection: camera-relative world -> shadow clip
float4 gSh1  : register(c5);
float4 gSh2  : register(c6);
float4 gSh3  : register(c7);
float4 gSun  : register(c8);        // direction to the sun, phase anisotropy g
float4 gP    : register(c9);        // debug stage, max distance, density, shadow bias
float4 gZ    : register(c10);       // the world viewport's MinZ, 1 / (MaxZ - MinZ)
float4 gL    : register(c11);       // steps along the ray, 1 / steps, this frame's noise offset
float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR
{
    if (gP.x > 1.5 && gP.x < 2.5)
        return float4(1.0, 0.0, 0.0, 1.0);                                // debug 2: the pass runs
    // Undo the viewport's squeeze; past the world's slice is sky or far horizon, so it clamps to
    // slightly short of far: exactly the far plane reconstructs with w = 0 in some frames, and the NaN that
    // makes, once the client's glow has blurred it over the image, turned whole frames black.
    float  d    = min(saturate((tex2Dlod(sDepth, float4(uv, 0, 0)).r - gZ.x) * gZ.y), 0.99999);
    if (gP.x > 2.5 && gP.x < 3.5)
        return float4(d, 0.0, 0.0, 1.0);                                  // debug 3: the depth it reads
    float2 ndc  = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp   = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    float3 P    = wp.xyz / max(wp.w, 1e-6);
    float  dist = length(P);
    float3 dir  = P / max(dist, 1e-4);
    float  len  = min(dist, gP.y);
    float3 e    = dir * len;
    if (gP.x > 3.5 && gP.x < 4.5)
        return float4(len / gP.y, 0.0, 0.0, 1.0);                         // debug 4: distance marched

    float3 s0 = gSh3.xyz;                                                 // the camera, at the origin
    float3 s1 = e.x * gSh0.xyz + e.y * gSh1.xyz + e.z * gSh2.xyz + gSh3.xyz;
    // Interleaved-gradient noise, turned by a golden-ratio step each frame (gL.z), so the frames the
    // temporal pass keeps each sample the ray at other places.
    float  jit = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))) + gL.z);

    // The bias grows with how steeply the line of sight runs into the map. Looking along a low sun, a
    // ray crosses hardly any of the map while its depth changes a lot, so every sample lands on nearly
    // the same texel and the smallest depth error flips the whole stretch between lit and shaded: that
    // was the flicker around the sun. Across the map, the plain bias is enough.
    float3 ds    = s1 - s0;
    float  slope = abs(ds.z) / max(length(ds.xy), 1e-5);
    float  bias  = gP.w * (1.0 + min(slope, 20.0));

    float  acc = 0.0;
    [loop] for (int i = 0; i < gL.x; ++i)
    {
        float3 s   = lerp(s0, s1, (i + jit) * gL.y);
        float2 suv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
        float  hit = (s.z <= tex2Dlod(sShadow, float4(suv, 0, 0)).r + bias) ? 1.0 : 0.0;
        // The map ends at a hard line, and a caster crossing it used to gain or lose its shade in one
        // frame: flashes in the distance as you walked. Shadowing fades out over the last tenth of the
        // map instead, so a caster dissolves in and out.
        float2 d   = abs(s.xy);
        float  inMap = saturate((1.0 - max(d.x, d.y)) * 10.0);
        acc += lerp(1.0, hit, inMap);
    }

    if (gP.x > 5.5)
        return float4(tex2Dlod(sShadow, float4(uv, 0, 0)).r, 0.0, 0.0, 1.0);   // debug 6: the shadow map
    if (gP.x > 4.5)
        return float4(acc * gL.y, 0.0, 0.0, 1.0);                         // debug 5: share of the ray in sun
    float lit   = acc * gL.y * len * gP.z;
    float c     = dot(dir, gSun.xyz);
    float g     = gSun.w;
    float phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5);
    // Whatever slipped through, nothing but a plain number in 0..16 leaves here: a NaN fails both tests.
    // The same for the distance, which is capped where 16-bit floats still hold it.
    float v     = lit * phase;
    v = (v >= 0.0 && v < 16.0) ? v : 0.0;
    dist = (dist >= 0.0 && dist < 30000.0) ? dist : 30000.0;
    return float4(v, dist, 0.0, 1.0);
}
)HLSL";

    // 5-tap Gaussian along gD (one texel step), run once across and once down. A tap counts for less the
    // further its distance (green) is from the centre's: 5% nearer or further halves it.
    const char* kBlurHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gD : register(c0);
float W(float d, float c, float k)
{
    return k / (1.0 + abs(d - c) / max(c, 1e-3) * 20.0);
}
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float4 c  = tex2D(s0, uv);
    float4 a1 = tex2D(s0, uv + gD.xy);
    float4 b1 = tex2D(s0, uv - gD.xy);
    float4 a2 = tex2D(s0, uv + gD.xy * 2.0);
    float4 b2 = tex2D(s0, uv - gD.xy * 2.0);
    float wa1 = W(a1.g, c.g, 0.25),   wb1 = W(b1.g, c.g, 0.25);
    float wa2 = W(a2.g, c.g, 0.0625), wb2 = W(b2.g, c.g, 0.0625);
    float v = (c.r * 0.375 + a1.r * wa1 + b1.r * wb1 + a2.r * wa2 + b2.r * wb2)
            / (0.375 + wa1 + wb1 + wa2 + wb2);
    return float4(v, c.g, 0.0, 1.0);
}
)HLSL";

    // The temporal pass, at the march's resolution. The pixel's point is rebuilt from its direction and
    // distance, moved into the last camera's frame (the client draws camera-relative, so that is a shift
    // by how far the camera moved), and projected through the last frame's view-projection. The glow
    // found there is clamped to the range of this frame's 3x3 neighbourhood, so a glow the scene no longer
    // has cannot linger, and is dropped where its distance does not match the point's: that point was
    // hidden last frame, and the history there belongs to whatever hid it.
    const char* kTemporalHlsl = R"HLSL(
sampler2D sCur  : register(s0);     // this frame: glow (r), distance (g); point sampled
sampler2D sHist : register(s1);     // the last frame's result, the same layout; bilinear
float4 gInv0  : register(c0);       // rows of inverse(camera view-projection): clip -> camera-relative world
float4 gInv1  : register(c1);
float4 gInv2  : register(c2);
float4 gInv3  : register(c3);
float4 gPrev0 : register(c4);       // rows of the last frame's view-projection
float4 gPrev1 : register(c5);
float4 gPrev2 : register(c6);
float4 gPrev3 : register(c7);
float4 gMove  : register(c8);       // this camera minus the last one (yards), share of the last frame to keep
float4 gT     : register(c9);       // one texel
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float4 cur = tex2Dlod(sCur, float4(uv, 0, 0));
    if (gMove.w <= 0.0)
        return cur;

    float lo = cur.r, hi = cur.r;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
        {
            float n = tex2Dlod(sCur, float4(uv + float2(i, j) * gT.xy, 0, 0)).r;
            lo = min(lo, n);
            hi = max(hi, n);
        }

    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gInv0 + ndc.y * gInv1 + 0.5 * gInv2 + gInv3;
    float3 P   = normalize(wp.xyz / max(wp.w, 1e-6)) * cur.g;
    float3 Q   = P + gMove.xyz;
    float4 clip = Q.x * gPrev0 + Q.y * gPrev1 + Q.z * gPrev2 + gPrev3;
    if (clip.w <= 1e-3)
        return cur;
    float2 puv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    if (puv.x < 0.0 || puv.y < 0.0 || puv.x > 1.0 || puv.y > 1.0)
        return cur;

    float4 h      = tex2Dlod(sHist, float4(puv, 0, 0));
    float  expect = length(Q);
    float  same   = saturate(1.0 - abs(h.g - expect) / max(0.2 * expect, 1.0));
    float  v      = lerp(cur.r, clamp(h.r, lo, hi), gMove.w * same);
    return float4(v, cur.g, 0.0, 1.0);
}
)HLSL";

    // Onto the world at full resolution: the four low-resolution texels around each pixel, weighted as a
    // bilinear filter would, and by how near each texel's distance is to the pixel's own.
    const char* kCompositeHlsl = R"HLSL(
sampler2D sGlow  : register(s0);    // glow (r), distance (g); point sampled
sampler2D sDepth : register(s1);    // the scene's depth (INTZ), full resolution
float4 gInv0 : register(c0);        // rows of inverse(camera view-projection)
float4 gInv1 : register(c1);
float4 gInv2 : register(c2);
float4 gInv3 : register(c3);
float4 gZ    : register(c4);        // the world viewport's MinZ, 1 / (MaxZ - MinZ)
float4 gT    : register(c5);        // the glow's size, and one texel
float4 gC    : register(c6);        // colour x gain
float Tap(float2 base, float2 o, float2 f, float dist, inout float wsum)
{
    float4 s  = tex2Dlod(sGlow, float4((base + o + 0.5) * gT.zw, 0, 0));
    float2 bw = lerp(1.0 - f, f, o);
    float  w  = bw.x * bw.y / (1e-3 + abs(s.g - dist) / max(dist, 1e-3)) + 1e-6;
    wsum += w;
    return s.r * w;
}
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float  d    = min(saturate((tex2Dlod(sDepth, float4(uv, 0, 0)).r - gZ.x) * gZ.y), 0.99999);
    float2 ndc  = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp   = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    float  dist = min(length(wp.xyz / max(wp.w, 1e-6)), 30000.0);
    float2 t    = uv * gT.xy - 0.5;
    float2 base = floor(t);
    float2 f    = t - base;
    float  wsum = 0.0;
    float  sum  = Tap(base, float2(0, 0), f, dist, wsum) + Tap(base, float2(1, 0), f, dist, wsum)
                + Tap(base, float2(0, 1), f, dist, wsum) + Tap(base, float2(1, 1), f, dist, wsum);
    return float4(gC.rgb * (sum / wsum), 0.0);
}
)HLSL";

    // The march's debug stages carry no distance, so they are shown with a plain bilinear stretch.
    const char* kPlainCompositeHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gC : register(c0);      // colour x gain
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    return float4(gC.rgb * tex2D(s0, uv).r, 0.0);
}
)HLSL";

    struct Target
    {
        IDirect3DTexture9* tex  = nullptr;
        IDirect3DSurface9* surf = nullptr;
        UINT w = 0, h = 0;
    };

    Target g_a, g_b;                          // the march result and the blur ping-pong
    Target g_hist[2];                         // the temporal pass's result: last frame's and this frame's
    int    g_histCur = 0;                     // which of the two holds the last result
    bool   g_histValid = false;
    unsigned  g_frameNo   = 0;                // counted at Present
    unsigned  g_histFrame = 0;                // the frame the last result was made in
    D3DMATRIX g_histVP = {};                  // ...its camera view-projection
    float     g_histCam[3] = {};              // ...and its camera position, world yards
    IDirect3DVertexShader9* g_vsMarch = nullptr;
    IDirect3DPixelShader9*  g_psMarch = nullptr;
    IDirect3DPixelShader9*  g_psBlur  = nullptr;
    IDirect3DPixelShader9*  g_psTemporal = nullptr;
    IDirect3DPixelShader9*  g_psComp  = nullptr;
    IDirect3DPixelShader9*  g_psPlain = nullptr;
    bool                    g_shadersTried = false;
    IDirect3DStateBlock9*   g_sb = nullptr;
    bool                    g_failed  = false;
    bool                    g_on      = true;
    bool                    g_logNext = false;

    // Per-frame outcome, logged every kStatFrames frames. A frame that skips the glow makes it blink, and
    // a one-frame probe cannot show which check failed.
    struct Stats
    {
        unsigned frames, calls, drawn;
        unsigned noDepth, noShadow, noMatrix, noSun, noCam, sunDown, noTarget, badMatrix;
    };
    Stats              g_st = {};
    constexpr unsigned kStatFrames = 300;
    int                g_trace = 0;             // frames left to trace after a probe, one line each

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    void ReleaseTarget(Target& t)
    {
        SafeRelease(t.surf);
        SafeRelease(t.tex);
        t.w = t.h = 0;
    }

    void ReleaseDefaultPool()
    {
        ReleaseTarget(g_a);
        ReleaseTarget(g_b);
        ReleaseTarget(g_hist[0]);
        ReleaseTarget(g_hist[1]);
        g_histValid = false;
        SafeRelease(g_sb);
    }

    OgBlob* Compile(const char* src, const char* name, const char* profile)
    {
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        if (!compile)
            return nullptr;
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        const HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", profile, 0, 0, &code, &errs);
        if (FAILED(hr) || !code)
        {
            Log("volume: %s failed to compile hr=0x%08X: %s", name, hr,
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no message)");
            if (code) code->lpVtbl->Release(code);
            code = nullptr;
        }
        if (errs) errs->lpVtbl->Release(errs);
        return code;
    }

    IDirect3DPixelShader9* MakePS(IDirect3DDevice9* dev, const char* src, const char* name, const char* profile)
    {
        OgBlob* code = Compile(src, name, profile);
        if (!code)
            return nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &ps)))
            ps = nullptr;
        code->lpVtbl->Release(code);
        return ps;
    }

    IDirect3DVertexShader9* MakeVS(IDirect3DDevice9* dev, const char* src, const char* name)
    {
        OgBlob* code = Compile(src, name, "vs_3_0");
        if (!code)
            return nullptr;
        IDirect3DVertexShader9* vs = nullptr;
        if (FAILED(dev->lpVtbl->CreateVertexShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &vs)))
            vs = nullptr;
        code->lpVtbl->Release(code);
        return vs;
    }

    bool MakeTarget(IDirect3DDevice9* dev, UINT w, UINT h, Target& t)
    {
        // 16-bit float: the glow is faint and smooth, and 8 bits would band it.
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F,
                                                D3DPOOL_DEFAULT, &t.tex, nullptr);
        if (SUCCEEDED(hr))
            hr = t.tex->lpVtbl->GetSurfaceLevel(t.tex, 0, &t.surf);
        if (FAILED(hr))
        {
            ReleaseTarget(t);
            return false;
        }
        t.w = w; t.h = h;
        return true;
    }

    bool EnsureResources(IDirect3DDevice9* dev, UINT w, UINT h)
    {
        if (!g_shadersTried)
        {
            g_shadersTried = true;
            g_vsMarch = MakeVS(dev, kMarchVsHlsl, "volume_march_vs");
            g_psMarch = MakePS(dev, kMarchPsHlsl, "volume_march", "ps_3_0");
            g_psBlur  = MakePS(dev, kBlurHlsl, "volume_blur", "ps_2_0");
            g_psTemporal = MakePS(dev, kTemporalHlsl, "volume_temporal", "ps_3_0");
            g_psComp  = MakePS(dev, kCompositeHlsl, "volume_composite", "ps_3_0");
            g_psPlain = MakePS(dev, kPlainCompositeHlsl, "volume_plain_composite", "ps_2_0");
            if (g_vsMarch && g_psMarch && g_psBlur && g_psTemporal && g_psComp && g_psPlain)
                Log("volume: shaders compiled");
        }
        if (!g_vsMarch || !g_psMarch || !g_psBlur || !g_psTemporal || !g_psComp || !g_psPlain)
            return false;
        const UINT ds = static_cast<UINT>(g_cfg.volume.downscale);
        const UINT tw = w / ds > 0 ? w / ds : 1, th = h / ds > 0 ? h / ds : 1;
        if (g_a.w == tw && g_a.h == th && g_b.surf && g_sb)
            return true;
        ReleaseDefaultPool();
        if (!MakeTarget(dev, tw, th, g_hist[0]) || !MakeTarget(dev, tw, th, g_hist[1]) ||
            !MakeTarget(dev, tw, th, g_a) || !MakeTarget(dev, tw, th, g_b) ||
            FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)) || !g_sb)
        {
            Log("volume: could not create %ux%u targets or a state block", tw, th);
            ReleaseDefaultPool();
            return false;
        }
        Log("volume: targets built at %ux%u", tw, th);
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    // drawing helpers

    void Mul(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX& out)
    {
        D3DMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        out = r;
    }

    bool Invert(const D3DMATRIX& src, D3DMATRIX& out)
    {
        double a[4][8];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j)
                a[i][j] = j < 4 ? src.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c)
        {
            int p = c;
            for (int r = c + 1; r < 4; ++r)
                if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
            if (fabs(a[p][c]) < 1e-12)
                return false;
            if (p != c)
                for (int j = 0; j < 8; ++j) { const double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
            const double inv = 1.0 / a[c][c];
            for (int j = 0; j < 8; ++j) a[c][j] *= inv;
            for (int r = 0; r < 4; ++r)
                if (r != c && a[r][c] != 0.0)
                {
                    const double f = a[r][c];
                    for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
                }
        }
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out.m[i][j] = static_cast<float>(a[i][j + 4]);
        return true;
    }

    float HalfToFloat(unsigned short h)
    {
        const unsigned s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
        float v = e == 0 ? ldexpf(static_cast<float>(m), -24)
                : e == 31 ? (m ? NAN : INFINITY)
                : ldexpf(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
        return s ? -v : v;
    }

    // Probe only: the march's own output at a few screen points, read back from the GPU.
    void LogMarchSamples(IDirect3DDevice9* dev, bool mean = false)
    {
        IDirect3DSurface9* sys = nullptr;
        if (FAILED(dev->lpVtbl->CreateOffscreenPlainSurface(dev, g_a.w, g_a.h, D3DFMT_A16B16G16R16F,
                                                            D3DPOOL_SYSTEMMEM, &sys, nullptr)))
            return;
        if (SUCCEEDED(dev->lpVtbl->GetRenderTargetData(dev, g_a.surf, sys)))
        {
            D3DLOCKED_RECT lr = {};
            if (SUCCEEDED(sys->lpVtbl->LockRect(sys, &lr, nullptr, D3DLOCK_READONLY)))
            {
                static const float pts[5][2] = { { 0.5f, 0.5f }, { 0.5f, 0.85f }, { 0.5f, 0.15f }, { 0.2f, 0.5f }, { 0.8f, 0.5f } };
                char line[256] = {};
                int n = 0;
                for (const auto& p : pts)
                {
                    const UINT x = static_cast<UINT>(p[0] * (g_a.w - 1)), y = static_cast<UINT>(p[1] * (g_a.h - 1));
                    const auto* px = reinterpret_cast<const unsigned short*>(static_cast<const char*>(lr.pBits) + y * lr.Pitch) + x * 4;
                    n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " (%.2f,%.2f)=%.6g", p[0], p[1], HalfToFloat(px[0]));
                }
                if (mean)
                {
                    double sum = 0.0;
                    unsigned lit = 0;
                    for (UINT y = 0; y < g_a.h; ++y)
                    {
                        const auto* row = reinterpret_cast<const unsigned short*>(static_cast<const char*>(lr.pBits) + y * lr.Pitch);
                        for (UINT x = 0; x < g_a.w; ++x)
                        {
                            const float f = HalfToFloat(row[x * 4]);
                            sum += f;
                            lit += f > 1e-4f;
                        }
                    }
                    const double px = static_cast<double>(g_a.w) * g_a.h;
                    n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "  mean %.5f, lit %.1f%%", sum / px, 100.0 * lit / px);
                }
                sys->lpVtbl->UnlockRect(sys);
                Log("volume: march output (debug %d) at screen points:%s", g_cfg.volume.debug, line);
            }
        }
        sys->lpVtbl->Release(sys);
    }

    // How much the glow changed since the last traced frame, sampled on a grid: an average that holds
    // steady can still hide a patch of the screen flickering, which is what the eye picks up.
    std::vector<float> g_prevFrame;
    char g_changeInfo[200] = {};

    void NoteFrameChange(const std::vector<float>& now, UINT w, UINT h, UINT step)
    {
        if (g_prevFrame.size() != now.size())
        {
            g_prevFrame = now;
            _snprintf_s(g_changeInfo, sizeof(g_changeInfo), _TRUNCATE, "first traced frame");
            return;
        }
        double sum = 0.0;
        float  worst = 0.0f;
        size_t worstAt = 0;
        for (size_t i = 0; i < now.size(); ++i)
        {
            const float d = fabsf(now[i] - g_prevFrame[i]);
            sum += d;
            if (d > worst) { worst = d; worstAt = i; }
        }
        const UINT cols = (w + step - 1) / step;
        _snprintf_s(g_changeInfo, sizeof(g_changeInfo), _TRUNCATE,
                    "mean change %.5f, worst %.5f at (%.2f, %.2f) of the screen",
                    sum / (now.size() ? now.size() : 1), worst,
                    cols ? (worstAt % cols) * step / static_cast<double>(w) : 0.0,
                    cols ? (worstAt / cols) * step / static_cast<double>(h) : 0.0);
        g_prevFrame = now;
    }

    // Mean of a target's red channel, and the share of pixels above zero. Trace only: it reads the
    // target back, which stalls the GPU.
    double TargetMean(IDirect3DDevice9* dev, const Target& t, double& litShare)
    {
        litShare = -1.0;
        IDirect3DSurface9* sys = nullptr;
        if (FAILED(dev->lpVtbl->CreateOffscreenPlainSurface(dev, t.w, t.h, D3DFMT_A16B16G16R16F,
                                                            D3DPOOL_SYSTEMMEM, &sys, nullptr)))
            return -1.0;
        double sum = 0.0;
        unsigned lit = 0;
        D3DLOCKED_RECT lr = {};
        if (SUCCEEDED(dev->lpVtbl->GetRenderTargetData(dev, t.surf, sys)) &&
            SUCCEEDED(sys->lpVtbl->LockRect(sys, &lr, nullptr, D3DLOCK_READONLY)))
        {
            for (UINT y = 0; y < t.h; ++y)
            {
                const auto* row = reinterpret_cast<const unsigned short*>(static_cast<const char*>(lr.pBits) + y * lr.Pitch);
                for (UINT x = 0; x < t.w; ++x)
                {
                    const float f = HalfToFloat(row[x * 4]);
                    sum += f;
                    lit += f > 1e-4f;
                }
            }
            sys->lpVtbl->UnlockRect(sys);
            const double px = static_cast<double>(t.w) * t.h;
            litShare = lit / px;
            sum /= px;
        }
        sys->lpVtbl->Release(sys);
        return sum;
    }

    // For the vs_3_0 march. A plain XYZ position: an XYZW one was read as three floats, which slid the
    // texture coordinate four bytes early: u became w (always 1) and every pixel sampled the depth
    // buffer's right-hand edge.
    struct ClipVertex { float x, y, z, u, v; };
    struct QuadVertex { float x, y, z, rhw, u, v; };      // pre-transformed, for the ps_2_0 passes

    void RhwQuad(IDirect3DDevice9* dev, UINT w, UINT h)
    {
        const float fw = static_cast<float>(w) - 0.5f, fh = static_cast<float>(h) - 0.5f;
        const QuadVertex q[4] = {
            { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
            {  fw,   -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f,  fh,   0.0f, 1.0f, 0.0f, 1.0f },
            {  fw,    fh,   0.0f, 1.0f, 1.0f, 1.0f },
        };
        dev->lpVtbl->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
    }

    // A full-screen quad for the vs_3_0 passes, with D3D9's half-pixel offset for a w x h target.
    void ClipQuad(IDirect3DDevice9* dev, UINT w, UINT h)
    {
        const float half[4] = { -1.0f / w, 1.0f / h, 0.0f, 0.0f };
        dev->lpVtbl->SetVertexShaderConstantF(dev, 0, half, 1);
        const ClipVertex q[4] = {
            { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
            {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
            { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
            {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        };
        dev->lpVtbl->SetFVF(dev, D3DFVF_XYZ | D3DFVF_TEX1);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(ClipVertex));
    }

    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
        D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE,
        D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_BLENDFACTOR,
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);
}

bool VolumeDraw(IDirect3DDevice9* dev)
{
    const bool logThis = g_logNext;
    g_logNext = false;

    const VolumeSettings& v = g_cfg.volume;
    if (!v.enabled || !g_on || g_failed || (v.strength <= 0.0f && !v.debug))
        return false;
    ++g_st.calls;

    IDirect3DTexture9* depth  = DepthWorldTexture();
    IDirect3DTexture9* shadow = ShadowTexture();
    float sunDir[3];
    D3DMATRIX view, proj, shadowVP;
    // The camera the depth was drawn with (see ShadowWorldCamera); sun.cpp's can be the sky's.
    const bool worldCam = ShadowWorldCamera(view, proj);
    const bool haveCam  = worldCam || SunCamera(view, proj);
    unsigned* skip = !depth ? &g_st.noDepth : !shadow ? &g_st.noShadow : !ShadowMatrix(shadowVP) ? &g_st.noMatrix :
                     !SunDirection(sunDir) ? &g_st.noSun : !haveCam ? &g_st.noCam : nullptr;
    if (skip)
    {
        ++*skip;
        if (logThis)
            Log("volume: skipped: %s", !depth ? "no readable depth ([depth] enabled?)" :
                !shadow ? "no shadow map ([shadow] enabled?)" : "no sun or camera yet");
        return false;
    }

    // Faded out as the sun goes down.
    const float sunset = sunDir[2] > 0.0f ? (sunDir[2] < 0.1f ? sunDir[2] / 0.1f : 1.0f) : 0.0f;
    if (sunset <= 0.0f && !v.debug)
    {
        ++g_st.sunDown;
        return false;
    }

    auto* d = dev->lpVtbl;
    IDirect3DSurface9* world = nullptr;
    d->GetRenderTarget(dev, 0, &world);
    if (!world)
    {
        ++g_st.noTarget;
        return false;
    }
    D3DSURFACE_DESC wd = {};
    world->lpVtbl->GetDesc(world, &wd);
    if (!EnsureResources(dev, wd.Width, wd.Height))
    {
        g_failed = true;
        world->lpVtbl->Release(world);
        return false;
    }

    D3DMATRIX camVP, inv;
    Mul(view, proj, camVP);
    bool finite = Invert(camVP, inv);
    for (int r = 0; r < 4 && finite; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(inv.m[r][c]) || !std::isfinite(shadowVP.m[r][c])) { finite = false; break; }
    for (int i = 0; i < 3 && finite; ++i)
        finite = std::isfinite(sunDir[i]);
    if (!finite)
    {
        ++g_st.badMatrix;
        world->lpVtbl->Release(world);
        return false;   // a bad matrix this frame: no glow rather than a NaN the glow would spread over the screen
    }
    const double t0 = Now();

    // --- save ---------------------------------------------------------------------------------------
    IDirect3DSurface9* oldDS = nullptr;
    d->GetDepthStencilSurface(dev, &oldDS);
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

    // --- common -------------------------------------------------------------------------------------
    d->SetDepthStencilSurface(dev, nullptr);   // the scene's depth is read below, so it cannot be bound
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
    for (DWORD s = 0; s < 2; ++s)
    {
        d->SetSamplerState(dev, s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(dev, s, D3DSAMP_SRGBTEXTURE, 0);
    }
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    // Off the shadow map reads as far: lit, with no known occluder.
    d->SetSamplerState(dev, 1, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
    d->SetSamplerState(dev, 1, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
    d->SetSamplerState(dev, 1, D3DSAMP_BORDERCOLOR, 0xFFFFFFFF);

    // --- march --------------------------------------------------------------------------------------
    d->SetRenderTarget(dev, 0, g_a.surf);
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(depth));
    d->SetTexture(dev, 1, reinterpret_cast<IDirect3DBaseTexture9*>(shadow));
    d->SetVertexShader(dev, g_vsMarch);
    d->SetPixelShader(dev, g_psMarch);
    const float half[4] = { -1.0f / g_a.w, 1.0f / g_a.h, 0.0f, 0.0f };
    d->SetVertexShaderConstantF(dev, 0, half, 1);
    const float span = 2.0f * g_cfg.shadow.depth - 1.0f;     // the shadow map's z range, yards
    float pc[48];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            pc[r * 4 + c]      = inv.m[r][c];
            pc[16 + r * 4 + c] = shadowVP.m[r][c];
        }
    pc[32] = sunDir[0]; pc[33] = sunDir[1]; pc[34] = sunDir[2]; pc[35] = v.anisotropy;
    // density / 4pi: the shader's Henyey-Greenstein term is left unnormalised to save the multiply.
    pc[36] = static_cast<float>(v.debug); pc[37] = v.maxDistance; pc[38] = v.density * 0.0795775f; pc[39] = v.bias / span;
    float minZ = 0.0f, maxZ = 1.0f;
    ShadowWorldDepthRange(minZ, maxZ);
    pc[40] = minZ; pc[41] = (maxZ - minZ) > 1e-6f ? 1.0f / (maxZ - minZ) : 1.0f; pc[42] = 0.0f; pc[43] = 0.0f;
    const float steps = static_cast<float>(v.steps);
    // The noise offset turns by the golden ratio each frame, for the temporal pass to average. Without
    // that pass it stays put: noise that changes every frame and is never averaged shimmers.
    const bool temporal = v.smooth > 0.001f && v.debug < 2;
    const float turn = temporal ? static_cast<float>(fmod(g_frameNo * 0.6180339887, 1.0)) : 0.0f;
    pc[44] = steps; pc[45] = 1.0f / steps; pc[46] = turn; pc[47] = 0.0f;
    d->SetPixelShaderConstantF(dev, 0, pc, 12);
    const ClipVertex q[4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
    };
    d->SetFVF(dev, D3DFVF_XYZ | D3DFVF_TEX1);
    d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(ClipVertex));
    double traceDepth = -1.0, traceDepthLit = -1.0, traceSun = -1.0, traceSunLit = -1.0;
    double traceMap = -1.0, traceMapLit = -1.0;
    if (g_trace > 0)
    {
        // The same march in debug 3 (the depth it reads) and debug 5 (share of each ray in sun), into the
        // blur target, which the blur overwrites next: nothing of this reaches the screen.
        for (int k = 0; k < 3; ++k)
        {
            const float pk[4] = { k == 0 ? 3.0f : k == 1 ? 5.0f : 6.0f, pc[37], pc[38], pc[39] };
            d->SetPixelShaderConstantF(dev, 9, pk, 1);
            d->SetRenderTarget(dev, 0, g_b.surf);
            d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(ClipVertex));
            if (k == 0)      traceDepth = TargetMean(dev, g_b, traceDepthLit);
            else if (k == 1) traceSun   = TargetMean(dev, g_b, traceSunLit);
            else             traceMap   = TargetMean(dev, g_b, traceMapLit);
        }
        d->SetPixelShaderConstantF(dev, 9, &pc[36], 1);
    }
    d->SetTexture(dev, 1, nullptr);
    d->SetVertexShader(dev, nullptr);
    if (g_trace > 0)
    {
        --g_trace;
        LogMarchSamples(dev, true);
        {
            // The glow as it reaches the screen, against the last traced frame.
            const Target& shown = g_histValid ? g_hist[g_histCur] : g_a;
            IDirect3DSurface9* sys = nullptr;
            if (SUCCEEDED(dev->lpVtbl->CreateOffscreenPlainSurface(dev, shown.w, shown.h,
                    D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && sys)
            {
                D3DLOCKED_RECT lr = {};
                if (SUCCEEDED(dev->lpVtbl->GetRenderTargetData(dev, shown.surf, sys)) &&
                    SUCCEEDED(sys->lpVtbl->LockRect(sys, &lr, nullptr, D3DLOCK_READONLY)))
                {
                    const UINT step = 4;
                    std::vector<float> grid;
                    grid.reserve((shown.w / step + 1) * (shown.h / step + 1));
                    for (UINT y = 0; y < shown.h; y += step)
                    {
                        const auto* row = reinterpret_cast<const unsigned short*>(
                            static_cast<const char*>(lr.pBits) + y * lr.Pitch);
                        for (UINT x = 0; x < shown.w; x += step)
                            grid.push_back(HalfToFloat(row[x * 4]));
                    }
                    sys->lpVtbl->UnlockRect(sys);
                    NoteFrameChange(grid, shown.w, shown.h, step);
                    Log("volume: trace glow %s", g_changeInfo);
                }
                sys->lpVtbl->Release(sys);
            }
        }
        int sOut = -1;
        unsigned sDrawn = 0, sEntries = 0, sc[5] = {};
        const char* sNew = "";
        ShadowLastReplay(sOut, sDrawn, sEntries, sc, sNew);
        unsigned sChanged = 0;
        const char* sDiff = ShadowChanges(sChanged);
        float sNear = 0.0f, sFar = 0.0f;
        ShadowWorldCameraPlanes(sNear, sFar);
        Log("volume: trace shadow world camera near %.2f far %.0f; %s", sNear, sFar, ShadowFrameInfo());
        Log("volume: trace shadow off-world records dropped %u; inputs changed in %u entries; biggest: %s",
            ShadowOffWorld(), sChanged, sDiff);
        unsigned nOver = 0;
        const char* overInfo = ShadowOverwritten(nOver);
        Log("volume: trace shadow overwritten under us: %u entries.%s", nOver, overInfo);
        Log("volume: trace shadow %s", ShadowMapCentre());
        Log("volume: trace shadow filter dropped:%s", ShadowDropped());
        Log("volume: trace shadow replay outcome %d, drew %u of %u entries; refreshed %u, added %u, evicted "
            "in view %u, aged %u, cap %u; inherited: %s; new:%s", sOut, sDrawn, sEntries, sc[0], sc[1], sc[2], sc[3],
            sc[4], ShadowInherited(), sNew);
        Log("volume: trace t=%.0f ms: depth read mean %.5f (%.1f%% nonzero), share in sun mean %.5f (%.1f%% nonzero), "
            "shadow map mean %.5f (%.1f%% nonzero); world camera %d, depth range %.4f..%.4f, sun (%.6f %.6f %.6f)",
            1000.0 * fmod(Now(), 1000.0), traceDepth, 100.0 * traceDepthLit, traceSun, 100.0 * traceSunLit,
            traceMap, 100.0 * traceMapLit, worldCam ? 1 : 0, minZ, maxZ, sunDir[0], sunDir[1], sunDir[2]);
    }
    if (logThis)
    {
        LogMarchSamples(dev);
        for (int r = 0; r < 4; ++r)
            Log("volume: view[%d] %9.4f %9.4f %9.4f %9.4f   proj[%d] %9.4f %9.4f %9.4f %9.4f   inv[%d] %11.4f %11.4f %11.4f %11.4f",
                r, view.m[r][0], view.m[r][1], view.m[r][2], view.m[r][3], r, proj.m[r][0], proj.m[r][1], proj.m[r][2],
                proj.m[r][3], r, inv.m[r][0], inv.m[r][1], inv.m[r][2], inv.m[r][3]);
        // The same reconstruction on the CPU, screen centre, at test depths.
        for (float dd : { 0.9f, 0.99f, 0.999f })
        {
            float wp[4];
            for (int c = 0; c < 4; ++c)
                wp[c] = dd * inv.m[2][c] + inv.m[3][c];
            const float dist = wp[3] != 0.0f ? sqrtf(wp[0] * wp[0] + wp[1] * wp[1] + wp[2] * wp[2]) / fabsf(wp[3]) : -1.0f;
            Log("volume: CPU check, centre of screen at depth %.3f -> %.2f yards (w %.6g)", dd, dist, wp[3]);
        }
    }

    // --- blur ---------------------------------------------------------------------------------------
    d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetPixelShader(dev, g_psBlur);
    const float across[4] = { 1.0f / g_a.w, 0.0f, 0.0f, 0.0f };
    const float down[4]   = { 0.0f, 1.0f / g_a.h, 0.0f, 0.0f };
    if (v.blur)
    {
        d->SetRenderTarget(dev, 0, g_b.surf);
        d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_a.tex));
        d->SetPixelShaderConstantF(dev, 0, across, 1);
        RhwQuad(dev, g_b.w, g_b.h);
        d->SetRenderTarget(dev, 0, g_a.surf);
        d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_b.tex));
        d->SetPixelShaderConstantF(dev, 0, down, 1);
        RhwQuad(dev, g_a.w, g_a.h);
    }

    // --- keep part of the last frame -----------------------------------------------------------------
    // The march is noisy and the shadow map changes under it. The last frame's result is reprojected
    // through the camera's movement and blended in (see kTemporalHlsl). The history belongs to the frame
    // just before this one, or it is not used: after a skipped frame it is too old to trust.
    const Target* src = &g_a;
    float cam[3] = {};
    const bool camRead = ClientCamera(cam);
    if (temporal)
    {
        const bool reuse = g_histValid && camRead && g_histFrame + 1 == g_frameNo;
        const Target& hist = g_hist[g_histCur];
        const Target& out  = g_hist[1 - g_histCur];
        float tc[40];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
            {
                tc[r * 4 + c]      = inv.m[r][c];
                tc[16 + r * 4 + c] = g_histVP.m[r][c];
            }
        tc[32] = cam[0] - g_histCam[0]; tc[33] = cam[1] - g_histCam[1]; tc[34] = cam[2] - g_histCam[2];
        tc[35] = reuse ? v.smooth : 0.0f;
        tc[36] = 1.0f / g_a.w; tc[37] = 1.0f / g_a.h; tc[38] = 0.0f; tc[39] = 0.0f;

        d->SetRenderTarget(dev, 0, out.surf);
        d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_a.tex));
        d->SetTexture(dev, 1, reinterpret_cast<IDirect3DBaseTexture9*>(hist.tex));
        d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, 1, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, 1, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, 1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, 1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetVertexShader(dev, g_vsMarch);
        d->SetPixelShader(dev, g_psTemporal);
        d->SetPixelShaderConstantF(dev, 0, tc, 10);
        ClipQuad(dev, out.w, out.h);
        d->SetTexture(dev, 1, nullptr);
        d->SetVertexShader(dev, nullptr);

        g_histCur   = 1 - g_histCur;
        g_histValid = camRead;
        g_histFrame = g_frameNo;
        g_histVP    = camVP;
        memcpy(g_histCam, cam, sizeof(g_histCam));
        src = &g_hist[g_histCur];
    }
    else
    {
        g_histValid = false;
    }

    // --- composite onto the world -------------------------------------------------------------------
    // debug replaces the world with the glow alone, white, to see its shape.
    const DWORD col = g_cfg.volume.color;
    const float gain = v.debug ? 1.0f : (v.strength * 0.01f) * v.maxIntensity * sunset;
    const float cc[4] = { v.debug ? gain : ((col >> 16) & 0xFF) / 255.0f * gain,
                          v.debug ? gain : ((col >>  8) & 0xFF) / 255.0f * gain,
                          v.debug ? gain : ((col      ) & 0xFF) / 255.0f * gain, 0.0f };
    d->SetRenderTarget(dev, 0, world);
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(src->tex));
    d->SetRenderState(dev, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                   D3DCOLORWRITEENABLE_BLUE);
    if (!v.debug)
    {
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(dev, D3DRS_SRCBLEND,         D3DBLEND_ONE);
        d->SetRenderState(dev, D3DRS_DESTBLEND,        D3DBLEND_ONE);
        d->SetRenderState(dev, D3DRS_BLENDOP,          D3DBLENDOP_ADD);
    }
    if (v.debug >= 2)
    {
        d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetPixelShader(dev, g_psPlain);
        d->SetPixelShaderConstantF(dev, 0, cc, 1);
        RhwQuad(dev, wd.Width, wd.Height);
    }
    else
    {
        float kc[28];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                kc[r * 4 + c] = inv.m[r][c];
        kc[16] = pc[40]; kc[17] = pc[41]; kc[18] = 0.0f; kc[19] = 0.0f;
        kc[20] = static_cast<float>(src->w); kc[21] = static_cast<float>(src->h);
        kc[22] = 1.0f / src->w;              kc[23] = 1.0f / src->h;
        kc[24] = cc[0]; kc[25] = cc[1]; kc[26] = cc[2]; kc[27] = 0.0f;
        d->SetTexture(dev, 1, reinterpret_cast<IDirect3DBaseTexture9*>(depth));
        for (DWORD st = 0; st < 2; ++st)
        {
            d->SetSamplerState(dev, st, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
            d->SetSamplerState(dev, st, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
            d->SetSamplerState(dev, st, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            d->SetSamplerState(dev, st, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        }
        d->SetVertexShader(dev, g_vsMarch);
        d->SetPixelShader(dev, g_psComp);
        d->SetPixelShaderConstantF(dev, 0, kc, 7);
        ClipQuad(dev, wd.Width, wd.Height);
        d->SetTexture(dev, 1, nullptr);
        d->SetVertexShader(dev, nullptr);
    }

    // --- restore ------------------------------------------------------------------------------------
    for (int i = 0; i < kTouchedCount; ++i)
        d->SetRenderState(dev, kTouched[i], saved[i]);
    d->SetTexture(dev, 0, oldTex0);
    d->SetVertexShader(dev, oldVS);
    d->SetFVF(dev, oldFVF);
    if (oldDecl)
        d->SetVertexDeclaration(dev, oldDecl);
    d->SetRenderTarget(dev, 0, world);
    d->SetDepthStencilSurface(dev, oldDS);
    g_sb->lpVtbl->Apply(g_sb);

    SafeRelease(oldTex0);
    SafeRelease(oldVS);
    SafeRelease(oldDecl);
    SafeRelease(oldDS);
    world->lpVtbl->Release(world);
    ++g_st.drawn;

    if (logThis)
        Log("volume: drawn at %ux%u (%.2f ms CPU to issue), gain %.2f, density %.3f, max distance %.0f yards, "
            "sun (%.2f %.2f %.2f)", g_a.w, g_a.h, 1000.0 * (Now() - t0), gain, v.density, v.maxDistance,
            sunDir[0], sunDir[1], sunDir[2]);
    return true;
}

bool VolumeActive()
{
    const VolumeSettings& v = g_cfg.volume;
    return v.enabled && g_on && !g_failed && (v.strength > 0.0f || v.debug);
}

void VolumeReset()
{
    ReleaseDefaultPool();
    g_failed = false;
}

void VolumeToggle()
{
    g_on = !g_on;
    g_histValid = false;
    Log("--- volume %s ---", g_on ? "ON" : "OFF");
}

void VolumeProbe()
{
    g_logNext = true;
    g_trace   = g_cfg.trace ? 180 : 0;   // [general] trace = 1 for the frame-by-frame one
}

void VolumeFrameEnd()
{
    ++g_frameNo;
    if (!g_cfg.volume.enabled || !g_on)
    {
        g_st = {};
        return;
    }
    if (++g_st.frames < kStatFrames)
        return;
    if (g_cfg.trace)   // [general] trace = 1 only: in normal play it was a file write every few seconds
        Log("volume: %u frames: world end reached %u, drawn %u; skipped: no depth %u, no shadow map %u, "
            "no shadow matrix %u, no sun %u, no camera %u, sun down %u, no target %u, bad matrix %u",
            g_st.frames, g_st.calls, g_st.drawn, g_st.noDepth, g_st.noShadow, g_st.noMatrix, g_st.noSun,
            g_st.noCam, g_st.sunDown, g_st.noTarget, g_st.badMatrix);
    g_st = {};
}
