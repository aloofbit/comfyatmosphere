// beams: light shafts that live in the world, around the player.
//
// The screen-space rays (rays.cpp) are rebuilt from the current image every frame, so as the camera moves
// they move with it: new sky enters the mask, the sun slides across the screen and the whole fan swings.
// Real shafts are objects in the air and stay put while you look around. These are that: soft ribbons,
// each running from about ground level up toward the sun, set on a world grid around the player.
//
//   Placement   A grid of `spacing` yards. A hash of each cell's WORLD coordinates decides whether it
//               holds a beam, and its jitter, height, width, taper, brightness and whether it is a cluster
//               of thin shafts. So a beam belongs to a place, not to the player: walking brings new
//               cells into range (faded in by distance) and leaves others behind.
//   Shape       Each ribbon lies along the sun direction and turns about that axis to face the camera, so
//               it reads as a shaft from any angle. It tapers toward the top (light spreads below a gap),
//               and the pixel shader gives it a soft cross-section, fading it in at the base and out
//               toward the top.
//   Occlusion   Drawn at the end of the world pass, depth-tested against the world's own depth buffer and
//               not writing it, so trees and hills in front hide the shafts. comfyfog.cpp calls in at
//               the moment the world finishes: before the client switches away from its world render
//               target (Full Screen Glow on) or at the switch to 2D (glow off). With glow on, the shafts
//               land in the world texture and get the client's glow like everything else.
//   Blending    "Screen", not add: result = shaft * (1 - background) + background. A shaft is visible
//               against shade and dark trunks and all but vanishes against bright sky, which is how real
//               shafts look, and why they no longer hang in clear sky.
//   Canopy      Shafts need something to cast them: in the open there are none. The top half of the frame
//               tells the two apart: under trees it is dark leaves with a few bright gaps, in the open it
//               is evenly bright sky. So canopy = 1 - mean/peak of its luminance, reduced on the GPU and
//               eased over a few seconds. The beam shader reads it and fades the shafts out in the open.
//               Nothing is read back to the CPU (except on the probe frame, for the log).
//   Brightness  Sunlight scatters forward, so a shaft is brightest seen against the sun and faint seen
//               with the sun behind you. Plus: near the camera (no smear when one passes through you), at
//               the edge of the area, at sunset, and indoors, where the sky's sun sprite is not drawn.
//
// Positions: this client renders camera-relative (see comfygrass's README). The camera's world position
// and the local player come out of the client (addresses verified for this WoW.exe by comfygrass), the
// beams are built in world coordinates, then shifted by -camera and drawn with an identity world matrix
// under the client's own rotation-only view and its projection.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "beams.h"
#include "client.h"
#include "common.h"
#include "config.h"
#include "rays.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    const char* kBeamHlsl = R"HLSL(
sampler2D s0 : register(s0);   // 1x1: how much canopy is overhead, eased
float4 gGain : register(c0);   // overall gain, canopy where shafts start, 1 / canopy range to full
float4 main(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR
{
    // uv.x runs -1..1 across the ribbon, uv.y 0 at the base to 1 at the top.
    float canopy = tex2D(s0, float2(0.5, 0.5)).r;
    float cover  = saturate((canopy - gGain.y) * gGain.z);
    float across = 1.0 - uv.x * uv.x;
    float along  = saturate(uv.y * 5.0) * saturate((1.0 - uv.y) * 1.6);
    return float4(col.rgb * (across * across * along * gGain.x * cover), 0.0);   // adds nothing to alpha
}
)HLSL";

    // Canopy estimate, top half of the frame. First step: 2x2 texels to (mean, max) luminance in (r, g).
    const char* kCanopyFirstHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gT : register(c0);      // source texel size
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    const float3 k = float3(0.299, 0.587, 0.114);
    float a = dot(tex2D(s0, uv + gT.xy * float2(-0.5, -0.5)).rgb, k);
    float b = dot(tex2D(s0, uv + gT.xy * float2( 0.5, -0.5)).rgb, k);
    float c = dot(tex2D(s0, uv + gT.xy * float2(-0.5,  0.5)).rgb, k);
    float d = dot(tex2D(s0, uv + gT.xy * float2( 0.5,  0.5)).rgb, k);
    return float4((a + b + c + d) * 0.25, max(max(a, b), max(c, d)), 0.0, 1.0);
}
)HLSL";

    // Then 4x4 at a time: mean of the means, max of the maxes.
    const char* kCanopyReduceHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gT : register(c0);
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float s = 0.0, m = 0.0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
        {
            float2 v = tex2D(s0, uv + gT.xy * float2(i - 1.5, j - 1.5)).rg;
            s += v.x;
            m = max(m, v.y);
        }
    return float4(s / 16.0, m, 0.0, 1.0);
}
)HLSL";

    // Canopy = 1 - mean/peak, eased into a persistent 1x1 by alpha blending with alpha = the frame's rate.
    const char* kCanopyEaseHlsl = R"HLSL(
sampler2D s0 : register(s0);
float4 gA : register(c0);      // rate
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float2 v = tex2D(s0, float2(0.5, 0.5)).rg;
    float canopy = saturate(1.0 - v.x / max(v.y, 0.02));
    return float4(canopy, canopy, canopy, gA.x);
}
)HLSL";

    struct BeamVertex { float x, y, z; DWORD color; float u, v; };
    constexpr DWORD kBeamFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    struct QuadVertex { float x, y, z, rhw, u, v; };

    struct Target
    {
        IDirect3DTexture9* tex  = nullptr;
        IDirect3DSurface9* surf = nullptr;
        UINT w = 0, h = 0;
    };

    // The canopy chain: the frame's top half at 256x128, then 128x64 (2x2), 32x16, 8x4, 2x1, 1x1 (4x4).
    constexpr int kCanLevels = 5;
    Target g_canTop;
    Target g_can[kCanLevels];
    Target g_canopy;                    // eased, 1x1, 16-bit float so slow easing does not stall
    bool   g_canopyInit = false;

    IDirect3DPixelShader9* g_ps        = nullptr;
    IDirect3DPixelShader9* g_psCanFirst = nullptr;
    IDirect3DPixelShader9* g_psCanRed  = nullptr;
    IDirect3DPixelShader9* g_psCanEase = nullptr;
    bool                   g_shadersTried = false;
    IDirect3DStateBlock9*  g_sb = nullptr;
    bool                   g_failed = false;
    bool                   g_on = true;
    bool                   g_logNext = false;

    float  g_outdoor   = 0.0f;      // eased toward 1 while the sky's sun is being drawn, 0 when not
    double g_lastSeen  = -1e9;
    double g_lastTime  = 0.0;
    float  g_baseZ     = 0.0f;      // ground height the shafts stand on, eased so a jump does not lift them
    bool   g_haveBaseZ = false;

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    inline float Sat(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    void ReleaseTarget(Target& t)
    {
        SafeRelease(t.surf);
        SafeRelease(t.tex);
        t.w = t.h = 0;
    }

    void ReleaseDefaultPool()
    {
        ReleaseTarget(g_canTop);
        for (int i = 0; i < kCanLevels; ++i)
            ReleaseTarget(g_can[i]);
        ReleaseTarget(g_canopy);
        g_canopyInit = false;
        SafeRelease(g_sb);   // a state block counts as a device resource for Reset
    }

    // ---------------------------------------------------------------------------------------------
    // placement

    uint32_t Hash(int32_t x, int32_t y, uint32_t salt)
    {
        uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343u ^ static_cast<uint32_t>(y) * 0xD8163841u ^ salt * 0xCB1AB31Fu;
        h ^= h >> 16; h *= 0x7FEB352Du;
        h ^= h >> 15; h *= 0x846CA68Bu;
        h ^= h >> 16;
        return h;
    }
    float Hash01(int32_t x, int32_t y, uint32_t salt) { return (Hash(x, y, salt) & 0xFFFFFF) / 16777216.0f; }

    struct V3 { float x, y, z; };
    inline V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline V3 Mul(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    inline float Len(V3 a) { return sqrtf(Dot(a, a)); }

    DWORD PackColor(float r, float g, float b)
    {
        auto c = [](float v) { return static_cast<DWORD>(Sat(v) * 255.0f + 0.5f); };
        return 0xFF000000u | (c(r) << 16) | (c(g) << 8) | c(b);
    }

    struct Frame
    {
        V3    cam, sun;
        float cr, cg, cb;
        std::vector<BeamVertex>* out;
    };

    // One camera-facing ribbon from base (width wBase) to top (width wTop), both in world coordinates,
    // with brightness k before the view-dependent terms. False if nothing was emitted.
    bool EmitRibbon(const Frame& f, V3 base, V3 top, float wBase, float wTop, float k)
    {
        const BeamsSettings& b = g_cfg.beams;
        const V3 bRel = Sub(base, f.cam);
        const V3 tRel = Sub(top, f.cam);
        const V3 mid  = Mul(Add(bRel, tRel), 0.5f);
        const float dCam = Len(mid);
        if (dCam < 1e-3f)
            return false;

        // Face the camera about the ribbon's own axis.
        V3 side = Cross(f.sun, mid);
        const float sl = Len(side);
        if (sl < 1e-4f)
            return false;                      // looking straight down the shaft
        side = Mul(side, 1.0f / sl);

        // Forward scattering: bright against the sun, faint with it behind you. And no smear when a
        // shaft passes through the camera.
        const float cosA  = Dot(Mul(mid, 1.0f / dCam), f.sun);
        const float phase = b.backLight + (1.0f - b.backLight) * powf(Sat(cosA), b.forwardPower);
        const float nearF = Sat((dCam - b.nearFade) / (b.nearFade > 0.1f ? b.nearFade : 0.1f));
        k *= phase * nearF;
        if (k < 0.004f)
            return false;

        const DWORD c  = PackColor(f.cr * k, f.cg * k, f.cb * k);
        const V3    sb = Mul(side, 0.5f * wBase);
        const V3    st = Mul(side, 0.5f * wTop);
        const BeamVertex q[4] = {
            { bRel.x - sb.x, bRel.y - sb.y, bRel.z - sb.z, c, -1.0f, 0.0f },
            { bRel.x + sb.x, bRel.y + sb.y, bRel.z + sb.z, c,  1.0f, 0.0f },
            { tRel.x - st.x, tRel.y - st.y, tRel.z - st.z, c, -1.0f, 1.0f },
            { tRel.x + st.x, tRel.y + st.y, tRel.z + st.z, c,  1.0f, 1.0f },
        };
        std::vector<BeamVertex>& out = *f.out;
        out.push_back(q[0]); out.push_back(q[1]); out.push_back(q[2]);
        out.push_back(q[2]); out.push_back(q[1]); out.push_back(q[3]);
        return true;
    }

    // Builds this frame's ribbons, camera-relative, into `out`. Returns how many ribbons were emitted.
    int Build(const V3& cam, const V3& player, const V3& sun, double t, std::vector<BeamVertex>& out)
    {
        const BeamsSettings& b = g_cfg.beams;
        const float cell   = b.spacing > 1.0f ? b.spacing : 1.0f;
        const float radius = b.radius;
        const DWORD col    = g_cfg.rays.color;
        const Frame f = { cam, sun, ((col >> 16) & 0xFF) / 255.0f, ((col >> 8) & 0xFF) / 255.0f,
                          (col & 0xFF) / 255.0f, &out };

        // Along the sun far enough to rise `height` yards, however low it is.
        const float rise = b.height / (sun.z > 0.25f ? sun.z : 0.25f);

        const int x0 = static_cast<int>(floorf((player.x - radius) / cell));
        const int x1 = static_cast<int>(floorf((player.x + radius) / cell));
        const int y0 = static_cast<int>(floorf((player.y - radius) / cell));
        const int y1 = static_cast<int>(floorf((player.y + radius) / cell));

        int count = 0;
        for (int iy = y0; iy <= y1; ++iy)
            for (int ix = x0; ix <= x1; ++ix)
            {
                if (Hash01(ix, iy, 1) >= b.density)
                    continue;

                const V3 base = { (ix + Hash01(ix, iy, 2)) * cell, (iy + Hash01(ix, iy, 3)) * cell,
                                  g_baseZ + b.baseOffset + (Hash01(ix, iy, 4) - 0.5f) * 4.0f };
                const float dx = base.x - player.x, dy = base.y - player.y;
                const float dPlayer = sqrtf(dx * dx + dy * dy);
                if (dPlayer > radius)
                    continue;

                // Per-place character: height, width (weighted toward thin), taper, brightness, breathing.
                const float hScale  = 1.0f - b.heightVar + 2.0f * b.heightVar * Hash01(ix, iy, 8);
                const float wPick   = Hash01(ix, iy, 5);
                const float width   = b.widthMin + (b.widthMax - b.widthMin) * wPick * wPick;
                const float taper   = 0.4f + 0.45f * Hash01(ix, iy, 9);
                const float own     = 0.35f + 0.65f * Hash01(ix, iy, 7);
                const float shimmer = 1.0f - b.shimmer * (0.5f + 0.5f * sinf(static_cast<float>(t) * 0.35f +
                                                                            Hash01(ix, iy, 6) * 6.2832f));
                const float edge    = Sat((radius - dPlayer) / (radius * 0.35f));
                const float k       = own * shimmer * edge;
                const V3    top     = Add(base, Mul(sun, rise * hScale));

                // The shaft itself: narrower at the top, where the gap is.
                if (EmitRibbon(f, base, top, width, width * taper, k))
                    ++count;

                // Some places get a comb of thin shafts beside it, as through a ragged gap. Their offset
                // direction is fixed per place (world, horizontal), so the comb does not turn with the camera.
                if (Hash01(ix, iy, 10) < b.clusterChance)
                {
                    const float ang = Hash01(ix, iy, 11) * 6.2832f;
                    const V3 dir = { cosf(ang), sinf(ang), 0.0f };
                    for (int s = -1; s <= 1; s += 2)
                    {
                        const float off   = s * width * (0.9f + 0.6f * Hash01(ix, iy, 12 + s));
                        const V3    shift = Mul(dir, off);
                        const float hs    = hScale * (0.6f + 0.4f * Hash01(ix, iy, 14 + s));
                        const float w     = width * (0.3f + 0.25f * Hash01(ix, iy, 16 + s));
                        if (EmitRibbon(f, Add(base, shift), Add(Add(base, shift), Mul(sun, rise * hs)),
                                       w, w * taper, k * 0.7f))
                            ++count;
                    }
                }
                if (count >= b.maxBeams)
                    return count;
            }
        return count;
    }

    // ---------------------------------------------------------------------------------------------
    // resources

    IDirect3DPixelShader9* MakePixelShader(IDirect3DDevice9* dev, const char* src, const char* name)
    {
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        if (!compile)
        {
            Log("beams: d3dcompiler_47 unavailable");
            return nullptr;
        }
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        const HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &errs);
        if (FAILED(hr) || !code)
        {
            Log("beams: %s failed to compile hr=0x%08X: %s", name, hr,
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
            Log("beams: CreatePixelShader(%s) failed hr=0x%08X", name, chr);
            return nullptr;
        }
        return ps;
    }

    bool MakeTarget(IDirect3DDevice9* dev, UINT w, UINT h, Target& t, D3DFORMAT fmt = D3DFMT_A8R8G8B8)
    {
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT,
                                                &t.tex, nullptr);
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

    bool EnsureResources(IDirect3DDevice9* dev)
    {
        if (!g_shadersTried)
        {
            g_shadersTried = true;
            g_ps         = MakePixelShader(dev, kBeamHlsl,         "beams");
            g_psCanFirst = MakePixelShader(dev, kCanopyFirstHlsl,  "beams_canopy_first");
            g_psCanRed   = MakePixelShader(dev, kCanopyReduceHlsl, "beams_canopy_reduce");
            g_psCanEase  = MakePixelShader(dev, kCanopyEaseHlsl,   "beams_canopy_ease");
            if (g_ps && g_psCanFirst && g_psCanRed && g_psCanEase)
                Log("beams: shaders compiled");
        }
        if (!g_ps || !g_psCanFirst || !g_psCanRed || !g_psCanEase)
            return false;
        if (g_sb && g_canopy.surf)
            return true;

        ReleaseDefaultPool();
        static const UINT sizes[kCanLevels][2] = { { 128, 64 }, { 32, 16 }, { 8, 4 }, { 2, 1 }, { 1, 1 } };
        bool ok = MakeTarget(dev, 256, 128, g_canTop);
        for (int i = 0; ok && i < kCanLevels; ++i)
            ok = MakeTarget(dev, sizes[i][0], sizes[i][1], g_can[i]);
        ok = ok && (MakeTarget(dev, 1, 1, g_canopy, D3DFMT_A16B16G16R16F) || MakeTarget(dev, 1, 1, g_canopy));
        ok = ok && SUCCEEDED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)) && g_sb;
        if (!ok)
        {
            Log("beams: could not create render targets or the state block; beams off until reset or reload");
            ReleaseDefaultPool();
        }
        return ok;
    }

    void DrawQuad(IDirect3DDevice9* dev, UINT w, UINT h)
    {
        const float fw = static_cast<float>(w) - 0.5f, fh = static_cast<float>(h) - 0.5f;
        const QuadVertex q[4] = {
            { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
            {  fw,   -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f,  fh,   0.0f, 1.0f, 0.0f, 1.0f },
            {  fw,    fh,   0.0f, 1.0f, 1.0f, 1.0f },
        };
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
    }

    void Pass(IDirect3DDevice9* dev, const Target& src, const Target& dst, IDirect3DPixelShader9* ps, const float* c)
    {
        dev->lpVtbl->SetRenderTarget(dev, 0, dst.surf);
        dev->lpVtbl->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(src.tex));
        dev->lpVtbl->SetPixelShader(dev, ps);
        dev->lpVtbl->SetPixelShaderConstantF(dev, 0, c, 1);
        DrawQuad(dev, dst.w, dst.h);
    }

    // The top half of the world image -> canopy, eased. Runs with the world render target bound and
    // leaves the caller to put it back.
    void UpdateCanopy(IDirect3DDevice9* dev, IDirect3DSurface9* world, float dt)
    {
        auto* d = dev->lpVtbl;
        D3DSURFACE_DESC desc = {};
        world->lpVtbl->GetDesc(world, &desc);
        const RECT top = { 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height / 2) };
        d->StretchRect(dev, world, &top, g_canTop.surf, nullptr, D3DTEXF_LINEAR);

        d->SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
        d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

        const Target* src = &g_canTop;
        for (int i = 0; i < kCanLevels; ++i)
        {
            const float tc[4] = { 1.0f / src->w, 1.0f / src->h, 0.0f, 0.0f };
            Pass(dev, *src, g_can[i], i == 0 ? g_psCanFirst : g_psCanRed, tc);
            src = &g_can[i];
        }

        const BeamsSettings& b = g_cfg.beams;
        float rate = 1.0f;
        if (g_canopyInit && b.canopyTime > 0.0f)
            rate = 1.0f - expf(-dt / b.canopyTime);
        g_canopyInit = true;
        const float ac[4] = { rate, 0.0f, 0.0f, 0.0f };
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(dev, D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        d->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(dev, D3DRS_BLENDOP,   D3DBLENDOP_ADD);
        Pass(dev, g_can[kCanLevels - 1], g_canopy, g_psCanEase, ac);
    }

    // Probe only: this frame's instantaneous canopy, read back for the log.
    void LogCanopy(IDirect3DDevice9* dev)
    {
        IDirect3DSurface9* sys = nullptr;
        if (FAILED(dev->lpVtbl->CreateOffscreenPlainSurface(dev, 1, 1, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)))
            return;
        if (SUCCEEDED(dev->lpVtbl->GetRenderTargetData(dev, g_can[kCanLevels - 1].surf, sys)))
        {
            D3DLOCKED_RECT lr = {};
            if (SUCCEEDED(sys->lpVtbl->LockRect(sys, &lr, nullptr, D3DLOCK_READONLY)))
            {
                const DWORD px = *static_cast<const DWORD*>(lr.pBits);
                sys->lpVtbl->UnlockRect(sys);
                const float mean = ((px >> 16) & 0xFF) / 255.0f, peak = ((px >> 8) & 0xFF) / 255.0f;
                const float canopy = Sat(1.0f - mean / (peak > 0.02f ? peak : 0.02f));
                const BeamsSettings& b = g_cfg.beams;
                Log("beams: top half of the frame: mean %.2f, peak %.2f -> canopy %.2f this frame "
                    "(shafts from %.2f, full at %.2f)", mean, peak, canopy, b.canopyStart, b.canopyFull);
            }
        }
        sys->lpVtbl->Release(sys);
    }

    // Render states the draw changes; re-set afterwards through the vtable so other hooks' mirrors stay
    // true, then the state block restores the device exactly (see rays.cpp for the same pattern).
    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE,
        D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_LIGHTING,
        D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);
}

bool BeamsDraw(IDirect3DDevice9* dev, bool sunSeenThisFrame)
{
    const BeamsSettings& b = g_cfg.beams;
    const double now = Now();
    double dt = now - g_lastTime;
    if (dt < 0.0 || dt > 0.5) dt = 0.0;
    g_lastTime = now;

    // Outdoors = the sky's sun sprite has been drawn recently. Eased both ways over about half a second.
    if (sunSeenThisFrame)
        g_lastSeen = now;
    const float target = (now - g_lastSeen) < b.indoorHold ? 1.0f : 0.0f;
    g_outdoor += (target - g_outdoor) * Sat(static_cast<float>(dt) / 0.5f);

    const bool logThis = g_logNext;
    g_logNext = false;

    if (!b.enabled || !g_on || b.strength <= 0.0f || g_failed)
        return false;

    float sunDir[3];
    D3DMATRIX view, proj;
    float camA[3], plA[3];
    if (!RaysSunDirection(sunDir) || !RaysCamera(view, proj) || !ClientCamera(camA))
    {
        if (logThis) Log("beams: skipped: no sun, camera matrices or camera position yet");
        return false;
    }
    const bool havePlayer = ClientPlayer(plA);
    const V3 cam = { camA[0], camA[1], camA[2] };
    const V3 player = havePlayer ? V3{ plA[0], plA[1], plA[2] } : cam;
    const V3 sun = { sunDir[0], sunDir[1], sunDir[2] };

    // Stand the shafts on the player's ground height, eased so jumping or a ledge does not lift them.
    if (!g_haveBaseZ) { g_baseZ = player.z; g_haveBaseZ = true; }
    g_baseZ += (player.z - g_baseZ) * Sat(static_cast<float>(dt) / 1.0f);

    const float sunset = Sat(1.0f + sun.z / 0.087f) * Sat(sun.z / 0.15f + 0.5f);   // gone below the horizon
    const float gain   = (b.strength * 0.01f) * b.maxIntensity * g_outdoor * sunset;

    static std::vector<BeamVertex> verts;
    verts.clear();
    const int n = gain > 0.001f ? Build(cam, player, sun, now, verts) : 0;

    if (!EnsureResources(dev))
    {
        g_failed = true;
        return false;
    }

    auto* d = dev->lpVtbl;
    IDirect3DSurface9* world = nullptr;
    IDirect3DSurface9* depth = nullptr;
    d->GetRenderTarget(dev, 0, &world);
    d->GetDepthStencilSurface(dev, &depth);
    if (!world)
    {
        SafeRelease(depth);
        return false;
    }

    if (logThis)
    {
        D3DSURFACE_DESC rd = {}, dd = {};
        world->lpVtbl->GetDesc(world, &rd);
        if (depth) depth->lpVtbl->GetDesc(depth, &dd);
        Log("beams: %d ribbons, gain %.2f (outdoor %.2f, sunset %.2f), player %s (%.1f %.1f %.1f), camera (%.1f %.1f %.1f)",
            n, gain, g_outdoor, sunset, havePlayer ? "read" : "NOT read, using camera", player.x, player.y,
            player.z, cam.x, cam.y, cam.z);
        Log("beams: drawn into rt=%p %ux%u fmt=%d with depth=%p %ux%u fmt=%d%s", world, rd.Width, rd.Height,
            static_cast<int>(rd.Format), depth, dd.Width, dd.Height, static_cast<int>(dd.Format),
            depth ? "" : "  <-- NO DEPTH BUFFER: shafts will show through everything");
    }

    // --- save -------------------------------------------------------------------------------------
    g_sb->lpVtbl->Capture(g_sb);
    DWORD saved[kTouchedCount];
    for (int i = 0; i < kTouchedCount; ++i)
        d->GetRenderState(dev, kTouched[i], &saved[i]);
    D3DMATRIX oldWorld, oldView, oldProj;
    d->GetTransform(dev, D3DTS_WORLD, &oldWorld);
    d->GetTransform(dev, D3DTS_VIEW, &oldView);
    d->GetTransform(dev, D3DTS_PROJECTION, &oldProj);
    IDirect3DBaseTexture9*       oldTex0 = nullptr;
    IDirect3DVertexShader9*      oldVS   = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD                        oldFVF  = 0;
    d->GetTexture(dev, 0, &oldTex0);
    d->GetVertexShader(dev, &oldVS);
    d->GetVertexDeclaration(dev, &oldDecl);
    d->GetFVF(dev, &oldFVF);

    // Common to both parts.
    d->SetRenderState(dev, D3DRS_ALPHATESTENABLE,   FALSE);
    d->SetRenderState(dev, D3DRS_CULLMODE,          D3DCULL_NONE);
    d->SetRenderState(dev, D3DRS_FOGENABLE,         FALSE);
    d->SetRenderState(dev, D3DRS_LIGHTING,          FALSE);
    d->SetRenderState(dev, D3DRS_STENCILENABLE,     FALSE);
    d->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(dev, D3DRS_COLORWRITEENABLE,  0xF);
    d->SetRenderState(dev, D3DRS_SRGBWRITEENABLE,   FALSE);
    d->SetRenderState(dev, D3DRS_ZWRITEENABLE,      FALSE);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU,    D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV,    D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER,   D3DTEXF_NONE);
    d->SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, 0);
    d->SetVertexShader(dev, nullptr);

    // --- canopy -----------------------------------------------------------------------------------
    UpdateCanopy(dev, world, static_cast<float>(dt));
    if (logThis)
        LogCanopy(dev);
    d->SetRenderTarget(dev, 0, world);   // resets the viewport to the whole target; the state block restores it

    // --- shafts -----------------------------------------------------------------------------------
    if (n)
    {
        static const D3DMATRIX identity = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
        d->SetTransform(dev, D3DTS_WORLD, &identity);
        d->SetTransform(dev, D3DTS_VIEW, &view);
        d->SetTransform(dev, D3DTS_PROJECTION, &proj);

        // Screen blend: shaft * (1 - background) + background.
        d->SetRenderState(dev, D3DRS_ZENABLE,          D3DZB_TRUE);
        d->SetRenderState(dev, D3DRS_ZFUNC,            D3DCMP_LESSEQUAL);
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(dev, D3DRS_SRCBLEND,         D3DBLEND_INVDESTCOLOR);
        d->SetRenderState(dev, D3DRS_DESTBLEND,        D3DBLEND_ONE);
        d->SetRenderState(dev, D3DRS_BLENDOP,          D3DBLENDOP_ADD);
        d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_canopy.tex));
        d->SetFVF(dev, kBeamFVF);
        d->SetPixelShader(dev, g_ps);
        const float span = b.canopyFull - b.canopyStart;
        const float gc[4] = { gain, b.canopyStart, 1.0f / (span > 0.01f ? span : 0.01f), 0.0f };
        d->SetPixelShaderConstantF(dev, 0, gc, 1);
        d->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, static_cast<UINT>(verts.size() / 3), verts.data(),
                           sizeof(BeamVertex));
    }

    // --- restore ----------------------------------------------------------------------------------
    for (int i = 0; i < kTouchedCount; ++i)
        d->SetRenderState(dev, kTouched[i], saved[i]);
    d->SetTransform(dev, D3DTS_WORLD, &oldWorld);
    d->SetTransform(dev, D3DTS_VIEW, &oldView);
    d->SetTransform(dev, D3DTS_PROJECTION, &oldProj);
    d->SetTexture(dev, 0, oldTex0);
    d->SetVertexShader(dev, oldVS);
    d->SetFVF(dev, oldFVF);
    if (oldDecl)
        d->SetVertexDeclaration(dev, oldDecl);
    d->SetRenderTarget(dev, 0, world);
    d->SetDepthStencilSurface(dev, depth);
    g_sb->lpVtbl->Apply(g_sb);

    SafeRelease(oldTex0);
    SafeRelease(oldVS);
    SafeRelease(oldDecl);
    SafeRelease(world);
    SafeRelease(depth);
    return n > 0;
}

void BeamsReset()
{
    ReleaseDefaultPool();
    g_failed = false;
}

void BeamsToggle()
{
    g_on = !g_on;
    Log("--- beams %s ---", g_on ? "ON" : "OFF");
}

void BeamsProbe()
{
    g_logNext = true;
}
