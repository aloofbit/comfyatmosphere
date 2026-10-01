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
// Before the glow, the lampposts light the surfaces near them ([lamps] surface), as the client's torches
// light the models near them: a lamppost has no light of its own. The client's own lights are left out of
// this, since the client already lights with them. The surface each pixel shows is rebuilt from the depth,
// and its facing from how that point changes between neighbouring pixels. Its light falls off as a torch's
// does, 1 / (0.7 d + 0.03 d^2) (the attenuation the client sends with its torches), with the same smooth cut
// at the reach, and scales the colour already there: out = scene x (1 + light), so the stones near a lamp
// brighten in their own colour. The client adds a torch's light to the dim light a model already has; a
// scale by 1 + light / (that dim light) is the same thing, so [lamps] surface is about 1 / the night's
// light. There are no shadows: a lamp lights whatever faces it within its reach.
//
// Two full-screen passes at full resolution, each over the nearest [lamps] maxLights lights.
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

#include <cmath>
#include <cstring>

namespace
{
    constexpr int kMaxLights = 16;   // the shader's arrays; [lamps] maxLights is clamped to this

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
float4 gPos[16] : register(c8);     // each light: camera-relative position, 1 / reach^2
float4 gCol[16] : register(c24);    // each light: colour x gain x density x fade
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
    [unroll] for (int i = 0; i < 16; ++i)
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
float4 gN    : register(c5);        // world (gZ.zw, gN.x) and on the sky (gN.yzw). Not c6 and up: the glow
                                    // shader keeps its own constants there
float4 gPos[16] : register(c8);     // each light: camera-relative position, 1 / reach^2
float4 gCol[16] : register(c24);    // each light: colour x gain x fade
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float  raw = saturate((tex2Dlod(sDepth, float4(uv, 0, 0)).r - gZ.x) * gZ.y);
    float  d   = min(raw, 0.99999);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    float3 P   = wp.xyz / max(wp.w, 1e-6);
    // The surface's facing, from the points its neighbours show, turned toward the camera. Worked out
    // before any branch: the neighbours' values are only there outside one.
    float3 N  = cross(ddy(P), ddx(P));
    N = N / max(length(N), 1e-8);
    N = dot(N, P) > 0.0 ? -N : N;
    float3 dark = float3(gZ.zw, gN.x);
    float3 sky  = gN.yzw;
    float  da   = min(min(min(dark.r, dark.g), dark.b), 1.0);
    float  sa   = min(min(min(sky.r, sky.g), sky.b), 1.0);
    if (raw >= 0.99999)
        return float4(sky - sa, sa);                                       // the sky: nothing to light
    float3 sum = 0.0;
    [unroll] for (int i = 0; i < 16; ++i)                                  // no break: see the glow shader
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
    sum = (sum >= 0.0 && sum < 16.0) ? sum : 0.0;
    return float4(sum + dark - da, da);
}
)HLSL";

    IDirect3DVertexShader9* g_vs = nullptr;
    IDirect3DPixelShader9*  g_ps = nullptr;
    IDirect3DPixelShader9*  g_psSurf = nullptr;
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
            if (OgBlob* code = Compile(kPsHlsl, "lampglow_ps", "ps_3_0"))
            {
                if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_ps)))
                    g_ps = nullptr;
                code->lpVtbl->Release(code);
            }
            if (OgBlob* code = Compile(kSurfPsHlsl, "lampglow_surface_ps", "ps_3_0"))
            {
                if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_psSurf)))
                    g_psSurf = nullptr;
                code->lpVtbl->Release(code);
            }
            if (g_vs && g_ps && g_psSurf)
                Log("lampglow: shaders compiled");
        }
        if (!g_vs || !g_ps || !g_psSurf)
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
        float hour = 0.0f;
        if (!ClientHour(hour))
            return 1.0f;
        // Inside a building it is dim at any hour: the candles glow in full (2026-09-30).
        float pl[3];
        if (ClientPlayer(pl) && MapIndoors(pl))
            return 1.0f;
        const float day = g_cfg.lamps.day * 0.01f;
        return day + (1.0f - day) * NightWeight(hour);
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

    LampLight lights[kMaxLights];
    // The way the camera looks: the view's third column (the view is a turn alone here).
    float fwd[3] = { view.m[0][2], view.m[1][2], view.m[2][2] };
    {
        const float len = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        for (float& f : fwd)
            f = len > 1e-4f ? f / len : 0.0f;
    }
    const int found = LampsOn() ? LampsGather(cam, fwd, lights, l.maxLights < kMaxLights ? l.maxLights : kMaxLights)
                                : 0;
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
    float mist[kMaxLights] = {};

    // Each light's constants, with the fog's fade and the gain folded into its colour. A light the fog
    // hides completely is left out.
    // An unused slot: 1 / reach^2 so large that no pixel passes the shader's reach test.
    float pos[kMaxLights * 4] = {}, col[kMaxLights * 4] = {}, scol[kMaxLights * 4] = {};
    for (int i = 0; i < kMaxLights; ++i)
        pos[i * 4 + 3] = 1e9f;
    float vis[kMaxLights] = {};
    int n = 0;
    for (int i = 0; i < found; ++i)
    {
        const LampLight& L = lights[i];
        float v = 1.0f;
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
        // Only a lamppost lights surfaces: the client lights with its own lights already.
        const float k = gain * l.density * v, ks = L.kind == 1 ? sgain * (v < 1.0f ? v : 1.0f) : 0.0f;
        if (k <= 0.0f && ks <= 0.0f && !l.debug)
            continue;
        pos[n * 4 + 0] = L.pos[0]; pos[n * 4 + 1] = L.pos[1]; pos[n * 4 + 2] = L.pos[2];
        pos[n * 4 + 3] = 1.0f / (L.reach * L.reach);
        col[n * 4 + 0] = L.colour[0] * k; col[n * 4 + 1] = L.colour[1] * k; col[n * 4 + 2] = L.colour[2] * k;
        for (int c = 0; c < 3; ++c)
            scol[n * 4 + c] = L.colour[c] * ks;
        ++n;
    }
    if (logThis)
    {
        if (dark)
            Log("lampglow: night darkness: the world x (%.2f %.2f %.2f), the sky x (%.2f %.2f %.2f)", darkWorld[0],
                darkWorld[1], darkWorld[2], darkSky[0], darkSky[1], darkSky[2]);
        Log("lampglow: %u lights held, %d gathered, %d drawn; gain %.2f, on surfaces %.2f (by day x %.2f), "
            "density %.3f, fog %s%.0f..%.0f; our fog %.4f a yard, lampMist %.2f", LampsTracked(), found, n, gain,
            sgain, scale, l.density, haveFog ? "" : "(none) ", fogStart, fogEnd, fogDensity, g_cfg.fog.lampMist);
        for (int i = 0; i < found; ++i)
            Log("lampglow:   %s %6.1f yd  camera-relative (%.1f %.1f %.1f)  colour (%.2f %.2f %.2f)  reach %.1f  "
                "fog %.2f, mist x%.2f", lights[i].kind ? "sprite" : "light ", lights[i].dist, lights[i].pos[0],
                lights[i].pos[1], lights[i].pos[2], lights[i].colour[0], lights[i].colour[1], lights[i].colour[2],
                lights[i].reach, vis[i], mist[i]);
    }
    if (!n && !l.debug && !dark)
        return false;

    D3DMATRIX camVP, inv;
    Mul(view, proj, camVP);
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
    d->SetPixelShaderConstantF(dev, 8, pos, kMaxLights);

    const float half[4] = { -1.0f / td.Width, 1.0f / td.Height, 0.0f, 0.0f };
    d->SetVertexShaderConstantF(dev, 0, half, 1);
    const ClipVertex q[4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
    };
    d->SetFVF(dev, D3DFVF_XYZ | D3DFVF_TEX1);

    // The surfaces first, as scene x (darkness + light); then the glow in the air, added. debug 1 shows the
    // glow alone and debug 3 the light on surfaces alone, each over black.
    if (((l.surface > 0.0f && n) || dark) && (l.debug == 0 || l.debug == 3))
    {
        // c4 and c5 carry the darkness in this pass, and are put back for the glow's.
        const float dk[8] = { pc[16], pc[17], darkWorld[0], darkWorld[1], darkWorld[2], darkSky[0], darkSky[1],
                              darkSky[2] };
        d->SetPixelShaderConstantF(dev, 4, dk, 2);
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, l.debug ? FALSE : TRUE);
        d->SetRenderState(dev, D3DRS_SRCBLEND,         D3DBLEND_DESTCOLOR);
        d->SetRenderState(dev, D3DRS_DESTBLEND,        D3DBLEND_SRCALPHA);
        d->SetPixelShader(dev, g_psSurf);
        d->SetPixelShaderConstantF(dev, 24, scol, kMaxLights);
        d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(ClipVertex));
    }
    d->SetPixelShader(dev, g_ps);
    d->SetPixelShaderConstantF(dev, 4, pc + 16, 2);
    d->SetPixelShaderConstantF(dev, 24, col, kMaxLights);
    if (l.debug != 3 && (n || l.debug))
    {
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, l.debug ? FALSE : TRUE);
        d->SetRenderState(dev, D3DRS_SRCBLEND,         D3DBLEND_ONE);
        d->SetRenderState(dev, D3DRS_DESTBLEND,        D3DBLEND_ONE);
        d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(ClipVertex));
    }

    // Probe only: the same pass again into a target of our own, once as the glow and once as the distance
    // it reads (debug 2), read back at each light's pixel and beside it. It tells a glow the shader never
    // made from one drawn and then painted over.
    if (logThis)
    {
        IDirect3DSurface9* rt = nullptr;
        IDirect3DSurface9* sys = nullptr;
        if (SUCCEEDED(d->CreateRenderTarget(dev, td.Width, td.Height, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0,
                                            FALSE, &rt, nullptr)) &&
            SUCCEEDED(d->CreateOffscreenPlainSurface(dev, td.Width, td.Height, D3DFMT_A32B32G32R32F,
                                                     D3DPOOL_SYSTEMMEM, &sys, nullptr)))
        {
            d->SetRenderTarget(dev, 0, rt);
            d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
            d->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
            float glow[kMaxLights][3][3] = {}, dist[kMaxLights][3] = {};
            int px[kMaxLights][2] = {};
            for (int i = 0; i < n; ++i)
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
                const float m[4] = { l.through, pass ? 2.0f : 0.0f, 0.0f, 0.0f };
                d->SetPixelShaderConstantF(dev, 5, m, 1);
                d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(ClipVertex));
                D3DLOCKED_RECT lr = {};
                if (FAILED(d->GetRenderTargetData(dev, rt, sys)) ||
                    FAILED(sys->lpVtbl->LockRect(sys, &lr, nullptr, D3DLOCK_READONLY)))
                    break;
                for (int i = 0; i < n; ++i)
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
            for (int i = 0; i < n; ++i)
                Log("lampglow:   read back %d at pixel (%d %d), %.1f yd: glow (%.3f %.3f %.3f) there, (%.3f %.3f %.3f) "
                    "12 px right, (%.3f %.3f %.3f) 40 px right; distance read %.1f / %.1f / %.1f yd", i, px[i][0],
                    px[i][1], sqrtf(pos[i * 4] * pos[i * 4] + pos[i * 4 + 1] * pos[i * 4 + 1] + pos[i * 4 + 2] * pos[i * 4 + 2]),
                    glow[i][0][0], glow[i][0][1], glow[i][0][2], glow[i][1][0], glow[i][1][1], glow[i][1][2],
                    glow[i][2][0], glow[i][2][1], glow[i][2][2], dist[i][0], dist[i][1], dist[i][2]);
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
    SafeRelease(g_ps);
    SafeRelease(g_psSurf);
    g_shadersTried = false;
    g_failed = false;
}

void LampGlowProbe()
{
    g_logNext = true;
}
