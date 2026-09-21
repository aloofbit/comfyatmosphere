// volume -- volumetric light: the fog glowing where the sun reaches it.
//
// A lamp in a foggy room: the beam is not an object but the fog itself, lit. Walk around and it does not
// move, because it is defined by the lamp, the shade and the fog -- not by where you stand. Here the lamp
// is the sun, the shade is the shadow map (shadow.cpp) and the fog is what the client already draws.
//
// Per pixel, at reduced resolution:
//   1. Read the scene's depth (depth.cpp's INTZ buffer) and rebuild the point it shows, camera-relative,
//      through the inverse of the client's view-projection. The line of sight runs from the camera to it,
//      clipped at maxDistance -- the reach of the shadow map.
//   2. Step along it and ask the shadow map, at each step, whether that point in the air sees the sun.
//      The shadow projection is orthographic, so it is affine: the line's two ends are taken into shadow
//      space once and the steps interpolate between them. Outside the map counts as lit (no known
//      occluder). Each pixel starts its steps at a different offset (interleaved-gradient noise) so
//      banding turns into fine noise, and a small blur takes the noise out.
//   3. Lit length x density x a Henyey-Greenstein phase -- sunlight scatters forward, so the glow is
//      strongest looking toward the sun.
// Then it is added onto the world image in the [rays] colour, before glow and UI.
//
// The step loop needs Shader Model 3 (ps_2_0 fits about eight steps), and a ps_3_0 has to be paired
// with a vs_3_0, so the march has its own trivial full-screen vertex shader. The blur and the composite
// are ps_2_0 over pre-transformed quads, like the rest of comfyfog.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "common.h"
#include "config.h"
#include "depth.h"
#include "rays.h"
#include "shadow.h"
#include "volume.h"

#include <cmath>
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
float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR
{
    if (gP.x > 1.5 && gP.x < 2.5)
        return float4(1.0, 0.0, 0.0, 1.0);                                // debug 2: the pass runs
    // Undo the viewport's squeeze; past the world's slice is sky or far horizon, so it clamps to far.
    float  d    = saturate((tex2Dlod(sDepth, float4(uv, 0, 0)).r - gZ.x) * gZ.y);
    if (gP.x > 2.5 && gP.x < 3.5)
        return float4(d, 0.0, 0.0, 1.0);                                  // debug 3: the depth it reads
    float2 ndc  = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp   = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    float3 P    = wp.xyz / wp.w;
    float  dist = length(P);
    float3 dir  = P / max(dist, 1e-4);
    float  len  = min(dist, gP.y);
    float3 e    = dir * len;
    if (gP.x > 3.5 && gP.x < 4.5)
        return float4(len / gP.y, 0.0, 0.0, 1.0);                         // debug 4: distance marched

    float3 s0 = gSh3.xyz;                                                 // the camera, at the origin
    float3 s1 = e.x * gSh0.xyz + e.y * gSh1.xyz + e.z * gSh2.xyz + gSh3.xyz;
    float  jit = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));
    float  acc = 0.0;
    [loop] for (int i = 0; i < 32; ++i)
    {
        float3 s   = lerp(s0, s1, (i + jit) / 32.0);
        float2 suv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
        acc += (s.z <= tex2Dlod(sShadow, float4(suv, 0, 0)).r + gP.w) ? 1.0 : 0.0;
    }

    if (gP.x > 4.5)
        return float4(acc / 32.0, 0.0, 0.0, 1.0);                         // debug 5: share of the ray in sun
    float lit   = acc / 32.0 * len * gP.z;
    float c     = dot(dir, gSun.xyz);
    float g     = gSun.w;
    float phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5);
    return float4(lit * phase, 0.0, 0.0, 1.0);
}
)HLSL";

    // 5-tap Gaussian along gD (one texel step), run once across and once down.
    const char* kBlurHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gD : register(c0);
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float v = tex2D(s0, uv).r * 0.375
            + (tex2D(s0, uv + gD.xy).r + tex2D(s0, uv - gD.xy).r) * 0.25
            + (tex2D(s0, uv + gD.xy * 2.0).r + tex2D(s0, uv - gD.xy * 2.0).r) * 0.0625;
    return float4(v, 0.0, 0.0, 1.0);
}
)HLSL";

    const char* kCompositeHlsl = R"HLSL(
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
    IDirect3DVertexShader9* g_vsMarch = nullptr;
    IDirect3DPixelShader9*  g_psMarch = nullptr;
    IDirect3DPixelShader9*  g_psBlur  = nullptr;
    IDirect3DPixelShader9*  g_psComp  = nullptr;
    bool                    g_shadersTried = false;
    IDirect3DStateBlock9*   g_sb = nullptr;
    bool                    g_failed  = false;
    bool                    g_on      = true;
    bool                    g_logNext = false;

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
            g_psComp  = MakePS(dev, kCompositeHlsl, "volume_composite", "ps_2_0");
            if (g_vsMarch && g_psMarch && g_psBlur && g_psComp)
                Log("volume: shaders compiled");
        }
        if (!g_vsMarch || !g_psMarch || !g_psBlur || !g_psComp)
            return false;
        const UINT ds = static_cast<UINT>(g_cfg.volume.downscale);
        const UINT tw = w / ds > 0 ? w / ds : 1, th = h / ds > 0 ? h / ds : 1;
        if (g_a.w == tw && g_a.h == th && g_b.surf && g_sb)
            return true;
        ReleaseDefaultPool();
        if (!MakeTarget(dev, tw, th, g_a) || !MakeTarget(dev, tw, th, g_b) ||
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
    void LogMarchSamples(IDirect3DDevice9* dev)
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
                sys->lpVtbl->UnlockRect(sys);
                Log("volume: march output (debug %d) at screen points:%s", g_cfg.volume.debug, line);
            }
        }
        sys->lpVtbl->Release(sys);
    }

    // For the vs_3_0 march. A plain XYZ position: an XYZW one was read as three floats, which slid the
    // texture coordinate four bytes early -- u became w (always 1) and every pixel sampled the depth
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

    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
        D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE,
        D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);
}

void VolumeDraw(IDirect3DDevice9* dev)
{
    const bool logThis = g_logNext;
    g_logNext = false;

    const VolumeSettings& v = g_cfg.volume;
    if (!v.enabled || !g_on || g_failed || (v.strength <= 0.0f && !v.debug))
        return;

    IDirect3DTexture9* depth  = DepthWorldTexture();
    IDirect3DTexture9* shadow = ShadowTexture();
    float sunDir[3];
    D3DMATRIX view, proj, shadowVP;
    // The camera the depth was drawn with (see ShadowWorldCamera); rays.cpp's can be the sky's.
    const bool haveCam = ShadowWorldCamera(view, proj) || RaysCamera(view, proj);
    if (!depth || !shadow || !ShadowMatrix(shadowVP) || !RaysSunDirection(sunDir) || !haveCam)
    {
        if (logThis)
            Log("volume: skipped -- %s", !depth ? "no readable depth ([depth] enabled?)" :
                !shadow ? "no shadow map ([shadow] enabled?)" : "no sun or camera yet");
        return;
    }

    // Faded out as the sun goes down.
    const float sunset = sunDir[2] > 0.0f ? (sunDir[2] < 0.1f ? sunDir[2] / 0.1f : 1.0f) : 0.0f;
    if (sunset <= 0.0f && !v.debug)
        return;

    auto* d = dev->lpVtbl;
    IDirect3DSurface9* world = nullptr;
    d->GetRenderTarget(dev, 0, &world);
    if (!world)
        return;
    D3DSURFACE_DESC wd = {};
    world->lpVtbl->GetDesc(world, &wd);
    if (!EnsureResources(dev, wd.Width, wd.Height))
    {
        g_failed = true;
        world->lpVtbl->Release(world);
        return;
    }

    D3DMATRIX camVP, inv;
    Mul(view, proj, camVP);
    if (!Invert(camVP, inv))
    {
        world->lpVtbl->Release(world);
        return;
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
    float pc[44];
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
    d->SetPixelShaderConstantF(dev, 0, pc, 11);
    const ClipVertex q[4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
    };
    d->SetFVF(dev, D3DFVF_XYZ | D3DFVF_TEX1);
    d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(ClipVertex));
    d->SetTexture(dev, 1, nullptr);
    d->SetVertexShader(dev, nullptr);
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

    // --- composite onto the world -------------------------------------------------------------------
    // debug replaces the world with the glow alone, white, to see its shape.
    const DWORD col = g_cfg.rays.color;
    const float gain = v.debug ? 1.0f : (v.strength * 0.01f) * v.maxIntensity * sunset;
    const float cc[4] = { v.debug ? gain : ((col >> 16) & 0xFF) / 255.0f * gain,
                          v.debug ? gain : ((col >>  8) & 0xFF) / 255.0f * gain,
                          v.debug ? gain : ((col      ) & 0xFF) / 255.0f * gain, 0.0f };
    d->SetRenderTarget(dev, 0, world);
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_a.tex));
    d->SetPixelShader(dev, g_psComp);
    d->SetPixelShaderConstantF(dev, 0, cc, 1);
    d->SetRenderState(dev, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                   D3DCOLORWRITEENABLE_BLUE);
    if (!v.debug)
    {
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(dev, D3DRS_SRCBLEND,         D3DBLEND_ONE);
        d->SetRenderState(dev, D3DRS_DESTBLEND,        D3DBLEND_ONE);
        d->SetRenderState(dev, D3DRS_BLENDOP,          D3DBLENDOP_ADD);
    }
    RhwQuad(dev, wd.Width, wd.Height);

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

    if (logThis)
        Log("volume: drawn at %ux%u (%.2f ms CPU to issue), gain %.2f, density %.3f, max distance %.0f yards, "
            "sun (%.2f %.2f %.2f)", g_a.w, g_a.h, 1000.0 * (Now() - t0), gain, v.density, v.maxDistance,
            sunDir[0], sunDir[1], sunDir[2]);
}

void VolumeReset()
{
    ReleaseDefaultPool();
    g_failed = false;
}

void VolumeToggle()
{
    g_on = !g_on;
    Log("--- volume %s ---", g_on ? "ON" : "OFF");
}

void VolumeProbe()
{
    g_logNext = true;
}
