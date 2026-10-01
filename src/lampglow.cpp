// lampglow: the fog glowing around lamps, lanterns and torches.
//
// The lights come from lamps.cpp's tracker: the client's own point lights (torches and braziers) and the
// glow sprites of lampposts. Each is a point in the fog that lights the air around it, and the air scatters
// part of that light to the eye.
//
// For a point light, the light scattered along a line of sight has a closed form, so nothing is marched and
// there is no noise to smooth. With the light at p (camera-relative), the line of sight along the unit
// direction v out to the surface the pixel shows, at distance L:
//
//     t0 = p . v                        how far along the line the light is nearest to it
//     h  = sqrt(|p|^2 - t0^2 + s^2)     how near it comes; s ([lamps] softness) keeps the core finite
//     glow = (atan((L - t0) / h) + atan(t0 / h)) / h
//
// which is the integral of 1 / distance^2 along the line. The scene's depth ends the line, so a wall in
// front of a lamp hides the glow behind it. The line runs on [lamps] through yards past that surface: the
// client's light point sits inside the torch head or the brazier bowl, and from below or from the side the
// bowl hid the light and the glow went out (Darkshire, 2026-09-28). The glow of each light is cut off
// smoothly where h reaches the light's reach, (1 - h^2 / reach^2)^2, so a far light costs one test and adds
// nothing.
//
// Before the glow, every light lights the surfaces near it ([lamps] surface), as the client's torches light
// the models near them. The client lights only its models with its own lights, so the ground and the walls
// get theirs here too (2026-10-01). The surface each pixel shows is rebuilt from the depth,
// and its facing from how that point changes between neighbouring pixels. Its light falls off as a torch's
// does, 1 / (0.7 d + 0.03 d^2) (the attenuation the client sends with its torches), with the same smooth cut
// at the reach, and scales the colour already there: out = scene x (1 + light), so the stones near a lamp
// brighten in their own colour. The client adds a torch's light to the dim light a model already has; a
// scale by 1 + light / (that dim light) is the same thing, so [lamps] surface is about 1 / the night's
// light. There are no shadows: a lamp lights whatever faces it within its reach.
//
// Two passes at full resolution, drawn tile by tile (2026-10-01). The screen is cut into kTilesX x kTilesY
// tiles, and each tile draws only the lights whose reach can show in it: the box round each light's sphere,
// projected, says which. A tile takes up to [lamps] maxLights of them (32 at most), nearest first, through a
// shader built for 4, 8, 16 or 32, so a tile with two candles does not pay for 32. Until then both passes
// covered the whole screen with the nearest 16 lights in front of the camera, and Stormwind's streets and
// inns hold far more than 16.
//
// It uses the volumetric light's readable depth, and the world camera and depth range that the light's
// shadow map settles, so it draws only while the volumetric light does. It does not follow the sun: by
// day it is turned down to [lamps] day, by the game clock ([night] dusk, dawn and fade).
//
// The client fogs a lamp's own glow sprite to black with distance. The glow here fades the same way: over
// the far half of the fog the world was drawn with.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "mapterrain.h"
#include "common.h"
#include "config.h"
#include "depth.h"
#include "lampglow.h"
#include "lamps.h"
#include "shadow.h"
#include "sun.h"
#include "volume.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr int kTileLights = 32;  // the most lights a tile draws; [lamps] maxLights is clamped to this
    constexpr int kTilesX = 8, kTilesY = 6;
    constexpr int kVariants = 5;     // the shaders, by how many lights they loop over
    const int     kVariantLights[kVariants] = { 0, 4, 8, 16, 32 };
    const char*   kVariantText[kVariants]   = { "0", "4", "8", "16", "32" };

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

    const char* kPsHlsl = R"HLSL(
sampler2D sDepth : register(s0);    // the scene's depth (INTZ)
float4 gInv0 : register(c0);        // rows of inverse(camera view-projection): clip -> camera-relative world
float4 gInv1 : register(c1);
float4 gInv2 : register(c2);
float4 gInv3 : register(c3);
float4 gZ    : register(c4);        // the world viewport's MinZ, 1 / (MaxZ - MinZ), -, softness^2
float4 gM    : register(c5);        // yards the line of sight runs on past the surface, debug stage
#if NL > 0
float4 gPos[NL] : register(c8);      // each light: camera-relative position, 1 / reach^2
float4 gCol[NL] : register(c40);     // each light: colour x gain x density x fade
#endif
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    // As in the volumetric light: clamped short of the far plane, where w = 0 makes a NaN.
    float  d   = min(saturate((tex2Dlod(sDepth, float4(uv, 0, 0)).r - gZ.x) * gZ.y), 0.99999);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    float3 P   = wp.xyz / max(wp.w, 1e-6);
    float  len = length(P);
    float3 dir = P / max(len, 1e-4);
    if (gM.y > 1.5)
        return float4(saturate(len / 50.0).xxx, 0.0);                      // debug 2: distance read, white = 50 yd
    len += gM.x;
    float3 sum = 0.0;
    // Unrolled: a loop indexes the arrays by comparing against every element, 32 compares a light. And
    // with no break: an early break after the last light compiled so that the first light's glow was lost,
    // and the nearest lamp never glowed (2026-09-28). An unused slot has a reach too short to pass the test.
#if NL > 0
    [unroll] for (int i = 0; i < NL; ++i)
    {
        float3 p  = gPos[i].xyz;
        float  t0 = dot(p, dir);
        float  h2 = max(dot(p, p) - t0 * t0, 0.0) + gZ.w;
        float  w  = 1.0 - h2 * gPos[i].w;
        [branch] if (w > 0.0)
        {
            float rh = rsqrt(h2);
            float g  = (atan((len - t0) * rh) + atan(t0 * rh)) * rh;
            sum += gCol[i].rgb * (g * w * w);
        }
    }
#endif
    // Nothing but a plain number in 0..16 leaves here: a NaN fails both tests.
    sum = (sum >= 0.0 && sum < 16.0) ? sum : 0.0;
    return float4(sum, 0.0);
}
)HLSL";

    // The light on surfaces, and the night's darkness: blended as scene x (rgb + a), where rgb + a is the
    // darkness factor plus the light. The factor is split so a (one number) carries its smallest channel and
    // rgb the rest: the blend's source is clamped to 0..1, and a lamp's light may take it past 1.
    const char* kSurfPsHlsl = R"HLSL(
sampler2D sDepth : register(s0);    // the scene's depth (INTZ)
float4 gInv0 : register(c0);        // rows of inverse(camera view-projection): clip -> camera-relative world
float4 gInv1 : register(c1);
float4 gInv2 : register(c2);
float4 gInv3 : register(c3);
float4 gZ    : register(c4);        // the world viewport's MinZ, 1 / (MaxZ - MinZ), the night's factor on the
float4 gN    : register(c5);        // world (gZ.zw, gN.x) and on the sky (gN.yzw)
float4 gTexel : register(c6);       // one pixel in uv: 1 / width, 1 / height
#if NL > 0
float4 gPos[NL] : register(c8);      // each light: camera-relative position, 1 / reach^2
float4 gCol[NL] : register(c40);     // each light: colour x gain x fade
#endif
float3 PointAt(float2 uv)
{
    float  d   = min(saturate((tex2Dlod(sDepth, float4(uv, 0, 0)).r - gZ.x) * gZ.y), 0.99999);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    return wp.xyz / max(wp.w, 1e-6);
}
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float  raw = saturate((tex2Dlod(sDepth, float4(uv, 0, 0)).r - gZ.x) * gZ.y);
    float3 P   = PointAt(uv);
    // The surface's facing, from the points the pixels beside it show, turned toward the camera. On each axis
    // the side nearer in depth: across an edge the far side is another surface. Until 2026-10-01 it was
    // ddx/ddy, one value for each 2 x 2 block of pixels, and on a character the lamps' light fell in square
    // facets, with the blocks across its outline lit as if they faced anywhere.
    float3 xr = PointAt(uv + float2(gTexel.x, 0.0)) - P, xl = P - PointAt(uv - float2(gTexel.x, 0.0));
    float3 yd = PointAt(uv + float2(0.0, gTexel.y)) - P, yu = P - PointAt(uv - float2(0.0, gTexel.y));
    float3 ex = dot(xr, xr) < dot(xl, xl) ? xr : xl;
    float3 ey = dot(yd, yd) < dot(yu, yu) ? yd : yu;
    float3 N  = cross(ey, ex);
    N = N / max(length(N), 1e-8);
    N = dot(N, P) > 0.0 ? -N : N;
    float3 dark = float3(gZ.zw, gN.x);
    float3 sky  = gN.yzw;
    float  da   = min(min(min(dark.r, dark.g), dark.b), 1.0);
    float  sa   = min(min(min(sky.r, sky.g), sky.b), 1.0);
    if (raw >= 0.99999)
        return float4(sky - sa, sa);                                       // the sky: nothing to light
    float3 sum = 0.0;
#if NL > 0
    [unroll] for (int i = 0; i < NL; ++i)                                   // no break: see the glow shader
    {
        float3 L  = gPos[i].xyz - P;
        float  d2 = dot(L, L);
        float  w  = 1.0 - d2 * gPos[i].w;
        [branch] if (w > 0.0)
        {
            float dist = sqrt(d2);
            float ndl  = saturate(dot(N, L) / max(dist, 1e-3));
            // A torch's falloff; held at its value half a yard out, so a lamp's own post does not burn white.
            sum += gCol[i].rgb * (ndl * w * w / max(0.7 * dist + 0.03 * d2, 0.36));
        }
    }
#endif
    sum = (sum >= 0.0 && sum < 16.0) ? sum : 0.0;
    return float4(sum + dark - da, da);
}
)HLSL";

    IDirect3DVertexShader9* g_vs = nullptr;
    IDirect3DPixelShader9*  g_ps[kVariants] = {};
    IDirect3DPixelShader9*  g_psSurf[kVariants] = {};
    bool                    g_shadersTried = false;
    IDirect3DStateBlock9*   g_sb = nullptr;
    bool                    g_failed  = false;
    bool                    g_logNext = false;

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    // NL, the lights a shader loops over, as a D3D_SHADER_MACRO list.
    OgBlob* Compile(const char* src, const char* name, const char* profile, const char* lights = "0")
    {
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        if (!compile)
            return nullptr;
        const char* defines[4] = { "NL", lights, nullptr, nullptr };
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        const HRESULT hr = compile(src, strlen(src), name, defines, nullptr, "main", profile, 0, 0, &code, &errs);
        if (FAILED(hr) || !code)
        {
            Log("lampglow: %s failed to compile hr=0x%08X: %s", name, hr,
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
            if (OgBlob* code = Compile(kVsHlsl, "lampglow_vs", "vs_3_0"))
            {
                if (FAILED(dev->lpVtbl->CreateVertexShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_vs)))
                    g_vs = nullptr;
                code->lpVtbl->Release(code);
            }
            bool all = g_vs != nullptr;
            for (int v = 0; v < kVariants; ++v)
            {
                if (OgBlob* code = Compile(kPsHlsl, "lampglow_ps", "ps_3_0", kVariantText[v]))
                {
                    if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_ps[v])))
                        g_ps[v] = nullptr;
                    code->lpVtbl->Release(code);
                }
                if (OgBlob* code = Compile(kSurfPsHlsl, "lampglow_surface_ps", "ps_3_0", kVariantText[v]))
                {
                    if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_psSurf[v])))
                        g_psSurf[v] = nullptr;
                    code->lpVtbl->Release(code);
                }
                all = all && g_ps[v] && g_psSurf[v];
            }
            if (all)
                Log("lampglow: shaders compiled (%d light counts)", kVariants);
        }
        for (int v = 0; v < kVariants; ++v)
            if (!g_ps[v] || !g_psSurf[v])
                return false;
        if (!g_vs)
            return false;
        if (!g_sb && (FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)) || !g_sb))
        {
            Log("lampglow: could not create a state block");
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

    // By day, [lamps] day of the night strength, by the game clock. Without the clock, full strength.
    float DayScale()
    {
        // Inside a building it is dim at any hour: the candles glow as at night (2026-09-30), at [lamps]
        // indoors of that (Indoor Lamps, 2026-10-01). Eased over about half a second at the door.
        static float  inside = 0.0f;
        static double last = 0.0;
        const double now = Now();
        float pl[3];
        const float target = ClientPlayer(pl) && MapIndoors(pl) ? 1.0f : 0.0f;
        const float step = last > 0.0 ? static_cast<float>(1.0 - exp(-(now - last) / 0.15)) : 1.0f;
        inside += (target - inside) * (step < 0.0f ? 0.0f : (step > 1.0f ? 1.0f : step));
        last = now;
        float hour = 0.0f;
        float out = 1.0f;
        if (ClientHour(hour))
        {
            const float day = g_cfg.lamps.day * 0.01f;
            out = day + (1.0f - day) * NightWeight(hour);
        }
        return out + (g_cfg.lamps.indoors - out) * inside;
    }

    // The night's factor on the world (world[3]) and on the sky (sky[3]), each channel 0..~1.4. False when
    // it is 1 everywhere: by day, with [night] darkness and tint at 0, or inside a building. Walking in or
    // out eases over about half a second, so the change does not jump.
    bool NightDark(float world[3], float sky[3])
    {
        const NightSettings& n = g_cfg.night;
        float hour = 0.0f;
        const float w = (n.darkness > 0.0f || n.tint > 0.0f) && ClientHour(hour) ? NightWeight(hour) : 0.0f;
        static float  inside = 0.0f;
        static double last = 0.0;
        const double now = Now();
        float pl[3];
        const float target = !n.indoors && ClientPlayer(pl) && MapIndoors(pl) ? 1.0f : 0.0f;
        const float step = last > 0.0 ? static_cast<float>(1.0 - exp(-(now - last) / 0.15)) : 1.0f;
        inside += (target - inside) * (step < 0.0f ? 0.0f : (step > 1.0f ? 1.0f : step));
        last = now;
        const float k = w * (1.0f - inside);
        if (k <= 1e-3f)
            return false;
        // The moonlight's hue, at the brightness of white.
        float moon[3] = { ((n.moonColor >> 16) & 0xFF) / 255.0f, ((n.moonColor >> 8) & 0xFF) / 255.0f,
                          (n.moonColor & 0xFF) / 255.0f };
        const float lum = 0.2126f * moon[0] + 0.7152f * moon[1] + 0.0722f * moon[2];
        for (float& c : moon)
            c = lum > 1e-3f ? c / lum : 1.0f;
        for (int c = 0; c < 3; ++c)
        {
            const float hue = 1.0f + (moon[c] - 1.0f) * n.tint * k;
            world[c] = (1.0f - n.darkness * k) * hue;
            sky[c]   = (1.0f - n.darkness * n.sky * k) * (1.0f + (moon[c] - 1.0f) * n.tint * n.sky * k);
        }
        return true;
    }
}

// The lamps' own part: the glow and their light on surfaces.
static bool LampsOn()
{
    const LampSettings& l = g_cfg.lamps;
    return l.enabled && (l.strength > 0.0f || l.debug);
}

bool LampGlowActive()
{
    const NightSettings& n = g_cfg.night;
    return !g_failed && (LampsOn() || n.darkness > 0.0f || n.tint > 0.0f) && VolumeActive();
}

bool LampGlowWantsLights()
{
    return LampsOn() && LampGlowActive();
}

namespace
{
    // Which tiles a light's reach can show in: the box round its sphere, projected. With any corner at or
    // behind the near plane, the whole screen. False when it shows in none.
    bool TileSpan(const D3DMATRIX& vp, const float pos[3], float reach, int span[4])
    {
        float lo[2] = { 1.0f, 1.0f }, hi[2] = { -1.0f, -1.0f };
        bool all = false;
        for (int c = 0; c < 8 && !all; ++c)
        {
            const float p[3] = { pos[0] + ((c & 1) ? reach : -reach), pos[1] + ((c & 2) ? reach : -reach),
                                 pos[2] + ((c & 4) ? reach : -reach) };
            float h[4];
            for (int k = 0; k < 4; ++k)
                h[k] = p[0] * vp.m[0][k] + p[1] * vp.m[1][k] + p[2] * vp.m[2][k] + vp.m[3][k];
            if (h[3] <= 0.05f)
            {
                all = true;
                break;
            }
            for (int k = 0; k < 2; ++k)
            {
                const float v = h[k] / h[3];
                lo[k] = (std::min)(lo[k], v);
                hi[k] = (std::max)(hi[k], v);
            }
        }
        if (all)
        {
            span[0] = 0; span[1] = kTilesX - 1; span[2] = 0; span[3] = kTilesY - 1;
            return true;
        }
        if (hi[0] < -1.0f || lo[0] > 1.0f || hi[1] < -1.0f || lo[1] > 1.0f)
            return false;
        // x runs left to right in clip space and in the tiles; y runs up in clip space and down the tiles.
        auto tile = [](float ndc, int count, bool down) {
            const float t = down ? (1.0f - ndc) * 0.5f : (ndc + 1.0f) * 0.5f;
            const int i = static_cast<int>(floorf(t * count));
            return i < 0 ? 0 : (i >= count ? count - 1 : i);
        };
        span[0] = tile(lo[0], kTilesX, false);
        span[1] = tile(hi[0], kTilesX, false);
        span[2] = tile(hi[1], kTilesY, true);
        span[3] = tile(lo[1], kTilesY, true);
        return true;
    }

    // One tile's lights, as indexes into the frame's list, nearest first.
    struct TileList
    {
        int count = 0;
        int index[kTileLights];
    };

    // The shader for this many lights: the smallest that holds them.
    int Variant(int count)
    {
        for (int v = 0; v < kVariants; ++v)
            if (count <= kVariantLights[v])
                return v;
        return kVariants - 1;
    }
}

bool LampGlowDraw(IDirect3DDevice9* dev)
{
    const bool logThis = g_logNext;
    g_logNext = false;
    if (!LampGlowActive())
        return false;

    const LampSettings& l = g_cfg.lamps;
    IDirect3DTexture9* depth = DepthWorldTexture();
    float cam[3], player[3];
    D3DMATRIX view, proj;
    const bool haveCam = ShadowWorldCamera(view, proj) || SunCamera(view, proj);
    // The player's position says the world is in: the character screen draws a scene of its own.
    if (!depth || !haveCam || !ClientCamera(cam) || !ClientPlayer(player))
    {
        if (logThis)
            Log("lampglow: skipped: %s", !depth ? "no readable depth" : "no camera, or not in the world");
        return false;
    }
    D3DMATRIX camVP, inv;
    Mul(view, proj, camVP);

    static LampLight lights[kLampsMax];
    const int found = LampsOn() ? LampsGather(cam, camVP, lights, kLampsMax) : 0;
    // The night's darkness, drawn in the pass that lights surfaces. Not in a debug view: those show one part
    // alone, over black.
    float darkWorld[3] = { 1.0f, 1.0f, 1.0f }, darkSky[3] = { 1.0f, 1.0f, 1.0f };
    const bool dark = !l.debug && NightDark(darkWorld, darkSky);

    const float scale = DayScale();
    // debug shows the same glow, over black, so its shape can be seen as it is drawn.
    const float gain  = (l.strength * 0.01f) * l.maxIntensity * scale;
    const float sgain = (l.strength * 0.01f) * l.surface * scale;
    float fogStart = 0.0f, fogEnd = 0.0f;
    const bool haveFog = WorldFog(fogStart, fogEnd) && fogEnd > 1.0f;
    const float fogDensity = FogDensityNow();   // our fog (volume.cpp), 0 when it is off

    // Each light's constants, with the fog's fade and the gain folded into its colour. A light the fog hides
    // completely is left out of the glow, and one that lights no surface out of the surface pass.
    static float pos[kLampsMax * 4], col[kLampsMax * 4], scol[kLampsMax * 4], vis[kLampsMax], mist[kLampsMax];
    static bool  glows[kLampsMax], shines[kLampsMax];
    for (int i = 0; i < found; ++i)
    {
        const LampLight& L = lights[i];
        float v = 1.0f;
        mist[i] = 0.0f;
        if (haveFog)
        {
            // Faded over the far half of the fog, stretched by [lamps] fogReach (Lamp Distance): in Duskwood's
            // short fog a lamppost 96 yards off was left at 8% and looked unlit (2026-09-30).
            const float end = fogEnd * l.fogReach;
            v = (end - L.dist) / (0.5f * end);
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
        // Our fog (2026-09-30). The glow is added after it, so a lamp behind thick fog shone through it at
        // full strength: it is dimmed by the fog between you and the lamp, taken at the halfway point. And it
        // is brighter in thick mist ([fog] lampMist), where there is more air to light.
        if (fogDensity > 0.0f)
        {
            const float half[3] = { 0.5f * L.pos[0], 0.5f * L.pos[1], 0.5f * L.pos[2] };
            const float at = FogThicknessAt(L.pos);
            v *= expf(-fogDensity * FogThicknessAt(half) * L.dist) * (1.0f + g_cfg.fog.lampMist * at);
            mist[i] = at;
        }
        vis[i] = v;
        // Every light lights surfaces. Until 2026-10-01 the client's own lights did not, since the client lights
        // with them already: but only its models, never the ground or a wall, so the street round a guard's
        // torch or a brazier stayed dark. A model near one of them is lit by both. Fires and lamps each have a
        // share (Torch Light, Lantern Light): at full strength the fires were too bright, and a camp's torches
        // outshone the lampposts beside it.
        const float share = L.fire ? l.torchLight : l.lanternLight;
        const float k = gain * l.density * v * share, ks = sgain * (v < 1.0f ? v : 1.0f) * share;
        glows[i]  = (k > 0.0f || l.debug) && !L.fill;
        shines[i] = ks > 0.0f && l.surface > 0.0f;
        pos[i * 4 + 0] = L.pos[0]; pos[i * 4 + 1] = L.pos[1]; pos[i * 4 + 2] = L.pos[2];
        pos[i * 4 + 3] = 1.0f / (L.reach * L.reach);
        for (int c = 0; c < 3; ++c)
        {
            col[i * 4 + c]  = L.colour[c] * k;
            scol[i * 4 + c] = L.colour[c] * ks;
        }
        col[i * 4 + 3] = scol[i * 4 + 3] = 0.0f;
    }

    // Each tile's lights: the glow's and the surfaces'.
    const int perTile = l.maxLights < kTileLights ? l.maxLights : kTileLights;
    static TileList glowTiles[kTilesX * kTilesY], surfTiles[kTilesX * kTilesY];
    for (int t = 0; t < kTilesX * kTilesY; ++t)
        glowTiles[t].count = surfTiles[t].count = 0;
    int glowing = 0, shining = 0, overflow = 0;
    for (int i = 0; i < found; ++i)
    {
        if (!glows[i] && !shines[i])
            continue;
        int span[4];
        if (!TileSpan(camVP, lights[i].pos, lights[i].reach, span))
            continue;
        glowing += glows[i] ? 1 : 0;
        shining += shines[i] ? 1 : 0;
        for (int ty = span[2]; ty <= span[3]; ++ty)
            for (int tx = span[0]; tx <= span[1]; ++tx)
            {
                const int t = ty * kTilesX + tx;
                if (glows[i])
                {
                    if (glowTiles[t].count < perTile)
                        glowTiles[t].index[glowTiles[t].count++] = i;
                    else
                        ++overflow;
                }
                if (shines[i])
                {
                    if (surfTiles[t].count < perTile)
                        surfTiles[t].index[surfTiles[t].count++] = i;
                    else
                        ++overflow;
                }
            }
    }
    int fullest = 0;
    for (int t = 0; t < kTilesX * kTilesY; ++t)
        fullest = (std::max)(fullest, (std::max)(glowTiles[t].count, surfTiles[t].count));

    if (logThis)
    {
        MapObjectsLog(player, 20.0f);
        if (dark)
            Log("lampglow: night darkness: the world x (%.2f %.2f %.2f), the sky x (%.2f %.2f %.2f)", darkWorld[0],
                darkWorld[1], darkWorld[2], darkSky[0], darkSky[1], darkSky[2]);
        Log("lampglow: %u lights held, %u in the files, %d gathered in view: %d glow, %d light surfaces; the "
            "fullest tile has %d (up to %d), %d left out of full tiles; gain %.2f, on surfaces %.2f (by day "
            "x %.2f), density %.3f, fog %s%.0f..%.0f; our fog %.4f a yard, lampMist %.2f", LampsTracked(),
            MapLightCount(), found, glowing, shining, fullest, perTile, overflow, gain, sgain, scale, l.density,
            haveFog ? "" : "(none) ", fogStart, fogEnd, fogDensity, g_cfg.fog.lampMist);
        for (int i = 0; i < found && i < 40; ++i)
            Log("lampglow:   %s %s %6.1f yd  camera-relative (%.1f %.1f %.1f)  colour (%.2f %.2f %.2f)  reach %.1f  "
                "fog %.2f, mist x%.2f", lights[i].kind ? "sprite" : "light ",
                lights[i].fill ? "floor" : lights[i].fire ? "fire" : "lamp", lights[i].dist, lights[i].pos[0],
                lights[i].pos[1], lights[i].pos[2], lights[i].colour[0], lights[i].colour[1], lights[i].colour[2],
                lights[i].reach, vis[i], mist[i]);
    }
    if (!glowing && !shining && !l.debug && !dark)
        return false;

    bool finite = Invert(camVP, inv);
    for (int r = 0; r < 4 && finite; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(inv.m[r][c])) { finite = false; break; }
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
    d->SetRenderState(dev, D3DRS_BLENDOP,           D3DBLENDOP_ADD);
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(depth));
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, 0);
    d->SetVertexShader(dev, g_vs);

    float pc[24];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc[r * 4 + c] = inv.m[r][c];
    float minZ = 0.0f, maxZ = 1.0f;
    ShadowWorldDepthRange(minZ, maxZ);
    pc[16] = minZ;
    pc[17] = (maxZ - minZ) > 1e-6f ? 1.0f / (maxZ - minZ) : 1.0f;
    pc[18] = 0.0f;
    pc[19] = l.softness * l.softness;
    pc[20] = l.through; pc[21] = static_cast<float>(l.debug); pc[22] = pc[23] = 0.0f;
    d->SetPixelShaderConstantF(dev, 0, pc, 6);

    const float half[4] = { -1.0f / td.Width, 1.0f / td.Height, 0.0f, 0.0f };
    d->SetVertexShaderConstantF(dev, 0, half, 1);
    d->SetFVF(dev, D3DFVF_XYZ | D3DFVF_TEX1);

    // One tile: its lights' constants, the shader for that many, and its quad. A slot of the shader past the
    // tile's lights gets a reach too short to pass the shader's test.
    auto drawTile = [&](int t, const TileList& list, IDirect3DPixelShader9* const* shaders, const float* colours) {
        const int v = Variant(list.count), slots = kVariantLights[v];
        float tp[kTileLights * 4], tc[kTileLights * 4];
        for (int s = 0; s < slots; ++s)
        {
            if (s < list.count)
            {
                memcpy(&tp[s * 4], &pos[list.index[s] * 4], 16);
                memcpy(&tc[s * 4], &colours[list.index[s] * 4], 16);
            }
            else
            {
                tp[s * 4] = tp[s * 4 + 1] = tp[s * 4 + 2] = 0.0f;
                tp[s * 4 + 3] = 1e9f;
                tc[s * 4] = tc[s * 4 + 1] = tc[s * 4 + 2] = tc[s * 4 + 3] = 0.0f;
            }
        }
        d->SetPixelShader(dev, shaders[v]);
        if (slots)
        {
            d->SetPixelShaderConstantF(dev, 8, tp, slots);
            d->SetPixelShaderConstantF(dev, 40, tc, slots);
        }
        const int tx = t % kTilesX, ty = t / kTilesX;
        const float u0 = static_cast<float>(tx) / kTilesX, u1 = static_cast<float>(tx + 1) / kTilesX;
        const float v0 = static_cast<float>(ty) / kTilesY, v1 = static_cast<float>(ty + 1) / kTilesY;
        const ClipVertex q[4] = {
            { u0 * 2.0f - 1.0f, 1.0f - v0 * 2.0f, 0.0f, u0, v0 },
            { u1 * 2.0f - 1.0f, 1.0f - v0 * 2.0f, 0.0f, u1, v0 },
            { u0 * 2.0f - 1.0f, 1.0f - v1 * 2.0f, 0.0f, u0, v1 },
            { u1 * 2.0f - 1.0f, 1.0f - v1 * 2.0f, 0.0f, u1, v1 },
        };
        d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(ClipVertex));
    };
    // The glow, over each tile with a light; in a debug view over every tile, black where there is none.
    auto drawGlow = [&]() {
        d->SetPixelShaderConstantF(dev, 4, pc + 16, 2);
        for (int t = 0; t < kTilesX * kTilesY; ++t)
            if (glowTiles[t].count || l.debug)
                drawTile(t, glowTiles[t], g_ps, col);
    };

    // The surfaces first, as scene x (darkness + light); then the glow in the air, added. debug 1 shows the
    // glow alone and debug 3 the light on surfaces alone, each over black.
    if ((shining || dark) && (l.debug == 0 || l.debug == 3))
    {
        // c4 and c5 carry the darkness in this pass, and are put back for the glow's.
        const float dk[8] = { pc[16], pc[17], darkWorld[0], darkWorld[1], darkWorld[2], darkSky[0], darkSky[1],
                              darkSky[2] };
        d->SetPixelShaderConstantF(dev, 4, dk, 2);
        const float texel[4] = { 1.0f / td.Width, 1.0f / td.Height, 0.0f, 0.0f };
        d->SetPixelShaderConstantF(dev, 6, texel, 1);
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, l.debug ? FALSE : TRUE);
        d->SetRenderState(dev, D3DRS_SRCBLEND,         D3DBLEND_DESTCOLOR);
        d->SetRenderState(dev, D3DRS_DESTBLEND,        D3DBLEND_SRCALPHA);
        for (int t = 0; t < kTilesX * kTilesY; ++t)
            if (surfTiles[t].count || dark || l.debug)
                drawTile(t, surfTiles[t], g_psSurf, scol);
    }
    if (l.debug != 3 && (glowing || l.debug))
    {
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, l.debug ? FALSE : TRUE);
        d->SetRenderState(dev, D3DRS_SRCBLEND,         D3DBLEND_ONE);
        d->SetRenderState(dev, D3DRS_DESTBLEND,        D3DBLEND_ONE);
        drawGlow();
    }

    // Probe only: the glow again into a target of our own, once as the glow and once as the distance it reads
    // (debug 2), read back at the nearest lights' pixels and beside them. It tells a glow the shader never
    // made from one drawn and then painted over.
    if (logThis)
    {
        constexpr int kRead = 16;
        IDirect3DSurface9* rt = nullptr;
        IDirect3DSurface9* sys = nullptr;
        if (SUCCEEDED(d->CreateRenderTarget(dev, td.Width, td.Height, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0,
                                            FALSE, &rt, nullptr)) &&
            SUCCEEDED(d->CreateOffscreenPlainSurface(dev, td.Width, td.Height, D3DFMT_A32B32G32R32F,
                                                     D3DPOOL_SYSTEMMEM, &sys, nullptr)))
        {
            const int nr = found < kRead ? found : kRead;
            d->SetRenderTarget(dev, 0, rt);
            d->Clear(dev, 0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
            d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
            d->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
            float glow[kRead][3][3] = {}, dist[kRead][3] = {};
            int px[kRead][2] = {};
            for (int i = 0; i < nr; ++i)
            {
                const float p4[4] = { pos[i * 4], pos[i * 4 + 1], pos[i * 4 + 2], 1.0f };
                float c[4];
                for (int k = 0; k < 4; ++k)
                    c[k] = p4[0] * camVP.m[0][k] + p4[1] * camVP.m[1][k] + p4[2] * camVP.m[2][k] + p4[3] * camVP.m[3][k];
                px[i][0] = c[3] > 1e-3f ? static_cast<int>((c[0] / c[3] * 0.5f + 0.5f) * td.Width) : -1;
                px[i][1] = c[3] > 1e-3f ? static_cast<int>((0.5f - c[1] / c[3] * 0.5f) * td.Height) : -1;
            }
            for (int pass = 0; pass < 2; ++pass)
            {
                pc[20] = l.through;
                pc[21] = pass ? 2.0f : 0.0f;
                drawGlow();
                D3DLOCKED_RECT lr = {};
                if (FAILED(d->GetRenderTargetData(dev, rt, sys)) ||
                    FAILED(sys->lpVtbl->LockRect(sys, &lr, nullptr, D3DLOCK_READONLY)))
                    break;
                for (int i = 0; i < nr; ++i)
                    for (int s = 0; s < 3; ++s)
                    {
                        const int x = px[i][0] + (s == 0 ? 0 : s == 1 ? 12 : 40), y = px[i][1];
                        if (x < 0 || y < 0 || x >= static_cast<int>(td.Width) || y >= static_cast<int>(td.Height))
                            continue;
                        const float* v = reinterpret_cast<const float*>(static_cast<const char*>(lr.pBits) + y * lr.Pitch) + x * 4;
                        if (pass)
                            dist[i][s] = v[0] * 50.0f;
                        else
                            memcpy(glow[i][s], v, sizeof(glow[i][s]));
                    }
                sys->lpVtbl->UnlockRect(sys);
            }
            for (int i = 0; i < nr; ++i)
                Log("lampglow:   read back %d at pixel (%d %d), %.1f yd: glow (%.3f %.3f %.3f) there, (%.3f %.3f %.3f) "
                    "12 px right, (%.3f %.3f %.3f) 40 px right; distance read %.1f / %.1f / %.1f yd", i, px[i][0],
                    px[i][1], lights[i].dist, glow[i][0][0], glow[i][0][1], glow[i][0][2], glow[i][1][0],
                    glow[i][1][1], glow[i][1][2], glow[i][2][0], glow[i][2][1], glow[i][2][2], dist[i][0],
                    dist[i][1], dist[i][2]);
            d->SetRenderTarget(dev, 0, target);
        }
        else
        {
            Log("lampglow: probe read back: could not create its targets");
        }
        SafeRelease(sys);
        SafeRelease(rt);
    }

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
    return true;
}

// Before a Reset, and when the client makes a new device: the shaders too, so a new device gets its own.
void LampGlowReset()
{
    SafeRelease(g_sb);
    SafeRelease(g_vs);
    for (int v = 0; v < kVariants; ++v)
    {
        SafeRelease(g_ps[v]);
        SafeRelease(g_psSurf[v]);
    }
    g_shadersTried = false;
    g_failed = false;
}

void LampGlowProbe()
{
    g_logNext = true;
}
