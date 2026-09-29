// sunshadows: the world shaded where the sun does not reach it.
//
// The client has no shadows of its own beyond the terrain's baked ones and a dark disc under each
// character. The volumetric light already keeps a map of the world as the sun sees it (shadow.cpp): trees,
// buildings and characters, replayed from the sun each few frames. This pass lays that map on the world:
//
//   1. Rebuild the point each pixel shows from the scene's depth, as the volumetric light does, and the
//      surface's facing from how that point changes between neighbouring pixels.
//   2. Move the point a little along its facing ([sunshadows] normalBias) and look it up in the map, nine
//      taps a texel apart times [sunshadows] softness, so the edge is soft rather than stepped. The offset
//      keeps a surface from shading itself. Near the player the near map is used ([shadow] nearRange, a
//      texel of 0.03 yards against the far map's 0.24), blended into the far one toward its edge. Bias and
//      offset are in texels of each map, so the near map gets finer ones.
//   3. Darken by the share in shade: out = scene x (1 - strength x shaded). A surface that faces away from
//      the sun is in shade by definition and gets [sunshadows] backShade, with no map test; one facing it
//      gets what the map says, and the facing blends the two. At first a surface facing away got no shade,
//      on the idea that the client's own lighting had darkened it already; it does not for models, which
//      it lights almost evenly all round, and backlit trunks stayed bright in the canopy's shade. Then it
//      was tested against the map, and the back of a walking character flickered (both 2026-09-29).
//      backShade is under 1, so that terrain facing away, which the client does darken, is not darkened
//      twice. Shade fades out over the last tenth of the far map, where it ends.
//
// It draws only while the volumetric light does, since the map is built for it. It follows the sun's
// height and [night] strength as the light does (the map follows the moon at night).

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "common.h"
#include "config.h"
#include "depth.h"
#include "shadow.h"
#include "sun.h"
#include "sunshadows.h"
#include "volume.h"

#include <cmath>
#include <cstring>

namespace
{
    const char* kVsHlsl = R"HLSL(
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

    // Blended as scene x this.
    const char* kPsHlsl = R"HLSL(
sampler2D sDepth  : register(s0);   // the scene's depth (INTZ)
sampler2D sShadow : register(s1);   // the sun's depth (INTZ), border = far: the far map
sampler2D sNear   : register(s2);   // the near map, the same way
float4 gInv0 : register(c0);        // rows of inverse(camera view-projection): clip -> camera-relative world
float4 gInv1 : register(c1);
float4 gInv2 : register(c2);
float4 gInv3 : register(c3);
float4 gZ    : register(c4);        // the world viewport's MinZ, 1 / (MaxZ - MinZ)
float4 gSh0  : register(c5);        // rows of the far map's view-projection: camera-relative world -> clip
float4 gSh1  : register(c6);
float4 gSh2  : register(c7);
float4 gSh3  : register(c8);
float4 gSun  : register(c9);        // direction to the sun, strength
float4 gB    : register(c10);       // far map: depth bias (map units), normal offset (yards), one texel (uv), softness
float4 gD    : register(c11);       // debug, shade on surfaces facing away from the sun
float4 gN0   : register(c12);       // rows of the near map's view-projection
float4 gN1   : register(c13);
float4 gN2   : register(c14);
float4 gN3   : register(c15);
float4 gNB   : register(c16);       // near map: depth bias, normal offset, one texel, 1 if there is one
float Tap(sampler2D m, float2 uv, float z, float bias)
{
    return (z <= tex2Dlod(m, float4(uv, 0, 0)).r + bias) ? 1.0 : 0.0;
}
// The share of nine taps, a texel x softness apart, that sees the sun.
float Lit(sampler2D m, float4 s, float bias, float texel)
{
    float2 uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    float2 o  = texel * gB.w;
    float lit = Tap(m, uv, s.z, bias)
              + Tap(m, uv + float2(-o.x, -o.y), s.z, bias) + Tap(m, uv + float2(0.0, -o.y), s.z, bias)
              + Tap(m, uv + float2( o.x, -o.y), s.z, bias) + Tap(m, uv + float2(-o.x,  0.0), s.z, bias)
              + Tap(m, uv + float2( o.x,  0.0), s.z, bias) + Tap(m, uv + float2(-o.x,  o.y), s.z, bias)
              + Tap(m, uv + float2( 0.0,  o.y), s.z, bias) + Tap(m, uv + float2( o.x,  o.y), s.z, bias);
    return lit / 9.0;
}
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float  raw = saturate((tex2Dlod(sDepth, float4(uv, 0, 0)).r - gZ.x) * gZ.y);
    float  d   = min(raw, 0.99999);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    float3 P   = wp.xyz / max(wp.w, 1e-6);
    // The facing, from the neighbours, before any branch (see lampglow.cpp).
    float3 N   = cross(ddy(P), ddx(P));
    N = N / max(length(N), 1e-8);
    N = dot(N, P) > 0.0 ? -N : N;
    if (raw >= 0.99999)
        return 1.0;                                                        // the sky: nothing to shade
    float facing = saturate(dot(N, gSun.xyz) * 4.0);
    if (gD.x > 1.5)
        return float4(facing.xxx, 1.0);                                    // debug 2: facing the sun

    // The far map, fading out over its last tenth, where it ends.
    float3 Qf  = P + N * gB.y;
    float4 sf  = Qf.x * gSh0 + Qf.y * gSh1 + Qf.z * gSh2 + gSh3;
    float2 ef  = abs(sf.xy);
    float  lit = lerp(1.0, Lit(sShadow, sf, gB.x, gB.z), saturate((1.0 - max(ef.x, ef.y)) * 10.0));
    // The near map where it reaches, blended in over the band from 80% to 90% of its half-width.
    [branch] if (gNB.w > 0.5)
    {
        float3 Qn = P + N * gNB.y;
        float4 sn = Qn.x * gN0 + Qn.y * gN1 + Qn.z * gN2 + gN3;
        float2 en = abs(sn.xy);
        float  wn = saturate((0.9 - max(en.x, en.y)) * 10.0);
        [branch] if (wn > 0.0)
            lit = lerp(lit, Lit(sNear, sn, gNB.x, gNB.z), wn);
    }
    // A surface that faces away from the sun cannot see it, so it takes gD.y of the shade with no test:
    // the client lights models almost evenly all round, and a backlit trunk stayed bright in the canopy's
    // shade. Tested there, the back of a walking character read the map at a grazing angle against its
    // own body and flickered (2026-09-29). Between the two, the map's answer blends in with the facing.
    float  shade = lerp(gD.y, 1.0 - lit, facing);
    float  f     = 1.0 - gSun.w * shade;
    f = (f >= 0.0 && f <= 1.0) ? f : 1.0;
    return float4(f, f, f, 1.0);
}
)HLSL";

    IDirect3DVertexShader9* g_vs = nullptr;
    IDirect3DPixelShader9*  g_ps = nullptr;
    bool                    g_shadersTried = false;
    IDirect3DStateBlock9*   g_sb = nullptr;
    bool                    g_failed  = false;
    bool                    g_logNext = false;

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
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
            Log("sunshadows: %s failed to compile hr=0x%08X: %s", name, hr,
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no message)");
            if (code) code->lpVtbl->Release(code);
            code = nullptr;
        }
        if (errs) errs->lpVtbl->Release(errs);
        return code;
    }

    bool EnsureResources(IDirect3DDevice9* dev)
    {
        if (!g_shadersTried)
        {
            g_shadersTried = true;
            if (OgBlob* code = Compile(kVsHlsl, "sunshadows_vs", "vs_3_0"))
            {
                if (FAILED(dev->lpVtbl->CreateVertexShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_vs)))
                    g_vs = nullptr;
                code->lpVtbl->Release(code);
            }
            if (OgBlob* code = Compile(kPsHlsl, "sunshadows_ps", "ps_3_0"))
            {
                if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_ps)))
                    g_ps = nullptr;
                code->lpVtbl->Release(code);
            }
            if (g_vs && g_ps)
                Log("sunshadows: shaders compiled");
        }
        if (!g_vs || !g_ps)
            return false;
        if (!g_sb && (FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)) || !g_sb))
        {
            Log("sunshadows: could not create a state block");
            return false;
        }
        return true;
    }

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

    struct ClipVertex { float x, y, z, u, v; };

    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
        D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE,
        D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);
}

bool SunShadowsDraw(IDirect3DDevice9* dev)
{
    const bool logThis = g_logNext;
    g_logNext = false;
    const SunShadowSettings& ss = g_cfg.sunShadows;
    if (!ss.enabled || g_failed || (ss.strength <= 0.0f && !ss.debug) || !VolumeActive())
        return false;

    IDirect3DTexture9* depth  = DepthWorldTexture();
    IDirect3DTexture9* shadow = ShadowTexture();
    IDirect3DTexture9* nearTex = nullptr;
    D3DMATRIX nearVP = {};
    float nearRange = 0.0f;
    const bool haveNear = ShadowNear(nearTex, nearVP, nearRange);
    float sunDir[3], cam[3], player[3];
    D3DMATRIX view, proj, shadowVP;
    const bool haveCam = ShadowWorldCamera(view, proj) || SunCamera(view, proj);
    if (!depth || !shadow || !ShadowMatrix(shadowVP) || !SunDirection(sunDir) || !haveCam || !ClientCamera(cam) ||
        !ClientPlayer(player))
    {
        if (logThis)
            Log("sunshadows: skipped: %s", !depth ? "no readable depth" : !shadow ? "no shadow map" :
                "no sun, camera or player");
        return false;
    }

    // As the volumetric light: gone as the sun sets, and [night] strength at night.
    const float sunset = (sunDir[2] > 0.0f ? (sunDir[2] < 0.1f ? sunDir[2] / 0.1f : 1.0f) : 0.0f) * NightScale();
    const float strength = ss.debug ? 1.0f : ss.strength * 0.01f * sunset;
    if (strength <= 0.0f)
        return false;

    D3DMATRIX camVP, inv;
    Mul(view, proj, camVP);
    bool finite = Invert(camVP, inv);
    for (int r = 0; r < 4 && finite; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(inv.m[r][c]) || !std::isfinite(shadowVP.m[r][c]) ||
                (haveNear && !std::isfinite(nearVP.m[r][c]))) { finite = false; break; }
    if (!finite)
        return false;

    if (!EnsureResources(dev))
    {
        g_failed = true;
        return false;
    }
    auto* d = dev->lpVtbl;
    IDirect3DSurface9* target = nullptr;
    d->GetRenderTarget(dev, 0, &target);
    if (!target)
        return false;
    D3DSURFACE_DESC td = {};
    target->lpVtbl->GetDesc(target, &td);

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

    // --- draw ---------------------------------------------------------------------------------------
    d->SetDepthStencilSurface(dev, nullptr);   // the scene's depth is read, so it cannot be bound
    d->SetRenderState(dev, D3DRS_ZENABLE,           D3DZB_FALSE);
    d->SetRenderState(dev, D3DRS_ZWRITEENABLE,      FALSE);
    d->SetRenderState(dev, D3DRS_ALPHATESTENABLE,   FALSE);
    d->SetRenderState(dev, D3DRS_CULLMODE,          D3DCULL_NONE);
    d->SetRenderState(dev, D3DRS_FOGENABLE,         FALSE);
    d->SetRenderState(dev, D3DRS_STENCILENABLE,     FALSE);
    d->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(dev, D3DRS_SRGBWRITEENABLE,   FALSE);
    d->SetRenderState(dev, D3DRS_COLORWRITEENABLE,  D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                    D3DCOLORWRITEENABLE_BLUE);
    // scene x shade; debug shows the shade alone, white = lit.
    d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,  ss.debug ? FALSE : TRUE);
    d->SetRenderState(dev, D3DRS_SRCBLEND,          D3DBLEND_ZERO);
    d->SetRenderState(dev, D3DRS_DESTBLEND,         D3DBLEND_SRCCOLOR);
    d->SetRenderState(dev, D3DRS_BLENDOP,           D3DBLENDOP_ADD);
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(depth));
    d->SetTexture(dev, 1, reinterpret_cast<IDirect3DBaseTexture9*>(shadow));
    d->SetTexture(dev, 2, reinterpret_cast<IDirect3DBaseTexture9*>(haveNear ? nearTex : shadow));
    for (DWORD st = 0; st < 3; ++st)
    {
        d->SetSamplerState(dev, st, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, st, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(dev, st, D3DSAMP_SRGBTEXTURE, 0);
    }
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    // Off a map reads as far: lit.
    for (DWORD st = 1; st < 3; ++st)
    {
        d->SetSamplerState(dev, st, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
        d->SetSamplerState(dev, st, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
        d->SetSamplerState(dev, st, D3DSAMP_BORDERCOLOR, 0xFFFFFFFF);
    }
    d->SetVertexShader(dev, g_vs);
    d->SetPixelShader(dev, g_ps);

    const float span = 2.0f * g_cfg.shadow.depth - 1.0f;     // the shadow map's z range, yards
    float minZ = 0.0f, maxZ = 1.0f;
    ShadowWorldDepthRange(minZ, maxZ);
    float pc[68] = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            pc[r * 4 + c]      = inv.m[r][c];
            pc[20 + r * 4 + c] = shadowVP.m[r][c];
        }
    pc[16] = minZ; pc[17] = (maxZ - minZ) > 1e-6f ? 1.0f / (maxZ - minZ) : 1.0f;
    pc[36] = sunDir[0]; pc[37] = sunDir[1]; pc[38] = sunDir[2]; pc[39] = strength;
    // Bias and offset are in texels of each map: yards = texels x the map's width / its size.
    const float size    = static_cast<float>(g_cfg.shadow.size > 0 ? g_cfg.shadow.size : 2048);
    const float farTex  = g_cfg.shadow.range * 2.0f / size;
    const float nearTex_ = nearRange * 2.0f / size;
    pc[40] = ss.bias * farTex / span; pc[41] = ss.normalBias * farTex; pc[42] = 1.0f / size; pc[43] = ss.softness;
    pc[44] = static_cast<float>(ss.debug); pc[45] = ss.backShade;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc[48 + r * 4 + c] = nearVP.m[r][c];
    pc[64] = ss.bias * nearTex_ / span; pc[65] = ss.normalBias * nearTex_; pc[66] = 1.0f / size;
    pc[67] = haveNear ? 1.0f : 0.0f;
    d->SetPixelShaderConstantF(dev, 0, pc, 17);

    const float half[4] = { -1.0f / td.Width, 1.0f / td.Height, 0.0f, 0.0f };
    d->SetVertexShaderConstantF(dev, 0, half, 1);
    const ClipVertex q[4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
    };
    d->SetFVF(dev, D3DFVF_XYZ | D3DFVF_TEX1);
    d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(ClipVertex));
    d->SetTexture(dev, 1, nullptr);
    d->SetTexture(dev, 2, nullptr);

    // --- restore ------------------------------------------------------------------------------------
    for (int i = 0; i < kTouchedCount; ++i)
        d->SetRenderState(dev, kTouched[i], saved[i]);
    d->SetTexture(dev, 0, oldTex0);
    d->SetVertexShader(dev, oldVS);
    d->SetFVF(dev, oldFVF);
    if (oldDecl)
        d->SetVertexDeclaration(dev, oldDecl);
    d->SetDepthStencilSurface(dev, oldDS);
    g_sb->lpVtbl->Apply(g_sb);

    SafeRelease(oldTex0);
    SafeRelease(oldVS);
    SafeRelease(oldDecl);
    SafeRelease(oldDS);
    target->lpVtbl->Release(target);

    if (logThis)
        Log("sunshadows: drawn, strength %.2f (sun height x night %.2f), sun (%.2f %.2f %.2f); far map %.3f yd a "
            "texel, near map %s %.3f yd a texel; bias %.1f and offset %.1f texels, softness %.1f, backShade %.2f",
            strength, sunset, sunDir[0], sunDir[1], sunDir[2], farTex, haveNear ? "on," : "off,", nearTex_, ss.bias,
            ss.normalBias, ss.softness, ss.backShade);
    return true;
}

// Before a Reset, and when the client makes a new device: the shaders too, so a new device gets its own.
void SunShadowsReset()
{
    SafeRelease(g_sb);
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    g_shadersTried = false;
    g_failed = false;
}

void SunShadowsProbe()
{
    g_logNext = true;
}
