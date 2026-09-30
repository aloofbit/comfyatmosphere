// sunshadows: the world shaded where the sun does not reach it.
//
// The client has no shadows of its own beyond the terrain's baked ones and a dark disc under each
// character. The volumetric light already keeps a map of the world as the sun sees it (shadow.cpp): trees,
// buildings and characters, replayed from the sun each few frames. This pass lays that map on the world:
//
//   1. Rebuild the point each pixel shows from the scene's depth, as the volumetric light does.
//   2. Move it [sunshadows] sunOffset yards toward the sun, so a surface does not shade itself, and look
//      it up in the map: nine taps a texel apart times [sunshadows] softness. Each tap tests the four
//      texels around its point and blends the answers by where the point falls between them, so an edge
//      moves smoothly inside a texel. Near the player the near map is used ([shadow] nearRange), blended
//      into the far one toward its edge. The bias is in texels of each map, so the near map gets a finer
//      one. [sunshadows] minGap adds yards to it on both maps: a blocker nearer the point than that,
//      along the sun, does not shade it. That stands in for "no shadow on itself", which would need to
//      know which object each pixel belongs to: an arm is about 0.2 yards from the body it shades, the
//      leaves of a canopy are close to each other, and a canopy is yards above the ground.
//   3. Darken by the share in shade, and brighten what the sun reaches by [sunshadows] sunlight:
//      out = scene x (1 - strength x shaded) x (1 + sunlight x lit), both x (1 - fog). The factor can
//      go above 1, which a shader's output cannot, so it is drawn at half and blended as 2 x modulate
//      (scene x output + output x scene). Added 2026-09-29: Northshire, a forest, was dark all over.
//      Before that: out = scene x (1 - strength x shaded x (1 - fog)). Shade fades out
//      over the last tenth of the far map, where it ends, and with the game's fog at that distance: a
//      fogged pixel is already the fog colour, and the shade is drawn after it. Without that, a shaded
//      tree at the fog wall came out darker than the fog and the sky it should fade into, a hard edge
//      against the gaps where the view distance ends (2026-09-29).
//
// The map alone decides, also for a surface facing away from the sun: the body in front of it, as the
// sun sees it, shades it. Until 2026-09-29 the pass also rebuilt each surface's facing from the depth,
// to shade surfaces facing away and to offset the lookup along the surface. On a model the depth gives
// each triangle's flat facing, where the client lights it with smooth normals, and every use of it put
// the triangles on the character: blocks, facets, a copy of the nose, speckle, patches where the arm
// shades the body. NOTES.md has the steps. The facing is still rebuilt for [sunshadows] normalBias and
// slope, both 0 by default: only when either is above 0.
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
sampler2D sNearL  : register(s3);   // the near map's leaves
sampler2D sFarL   : register(s4);   // the far map's leaves
float4 gInv0 : register(c0);        // rows of inverse(camera view-projection): clip -> camera-relative world
float4 gInv1 : register(c1);
float4 gInv2 : register(c2);
float4 gInv3 : register(c3);
float4 gZ    : register(c4);        // the world viewport's MinZ, 1 / (MaxZ - MinZ), one pixel of the depth (uv)
float4 gSh0  : register(c5);        // rows of the far map's view-projection: camera-relative world -> clip
float4 gSh1  : register(c6);
float4 gSh2  : register(c7);
float4 gSh3  : register(c8);
float4 gSun  : register(c9);        // direction to the sun, strength
float4 gB    : register(c10);       // far map: depth bias (map units), normal offset (yards), one texel (uv), softness
float4 gL    : register(c11);       // sunlight (share added where lit), 1 = debug (drawn as it is, not halved)
float4 gN0   : register(c12);       // rows of the near map's view-projection
float4 gN1   : register(c13);
float4 gN2   : register(c14);
float4 gN3   : register(c15);
float4 gNB   : register(c16);       // near map: depth bias, normal offset, one texel, 1 if there is one
float4 gG    : register(c17);       // the largest slope in each map's units: far, near
float4 gT    : register(c18);       // slope (share used), sunOffset (yards), 1 if the facing is needed
float4 gV    : register(c19);       // the view matrix's third column: camera-relative world -> view depth
float4 gFog  : register(c20);       // the world's fog start, 1 / (end - start), 1 if there is fog
float4 gCh   : register(c21);       // leafShade, 1 if near leaf map, 1 if far leaf map, 1 = read the far map
float4 gLod  : register(c22);       // far map: extra bias (map units) per yard past .y yards from the camera
float4 gShC  : register(c23);       // the shade's colour (brightness 1), how much
float4 gSuC  : register(c24);       // the sunlight's colour (brightness 1), how much
// The surface's slope in a map: how its depth changes per unit of map uv. Two directions along the
// surface are carried into the map, and the plane through them solved for depth against u and v.
float2 Slope(float3 N, float4 m0, float4 m1, float4 m2, float most)
{
    float3 t1 = normalize(cross(N, abs(N.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)));
    float3 t2 = cross(N, t1);
    float3 d1 = (t1.x * m0 + t1.y * m1 + t1.z * m2).xyz;     // clip units per yard along t1
    float3 d2 = (t2.x * m0 + t2.y * m1 + t2.z * m2).xyz;
    float  u1 = d1.x * 0.5, v1 = -d1.y * 0.5, u2 = d2.x * 0.5, v2 = -d2.y * 0.5;
    float  det = u1 * v2 - u2 * v1;
    det = abs(det) < 1e-12 ? 1e-12 : det;
    float2 g = float2(d1.z * v2 - d2.z * v1, u1 * d2.z - u2 * d1.z) / det;
    float  len = length(g);
    return len > most ? g * (most / len) : g;
}
float Test(sampler2D m, float2 uv, float z, float bias)
{
    return (z <= tex2Dlod(m, float4(uv, 0, 0)).r + bias) ? 1.0 : 0.0;
}
// One tap: the four texels around uv tested, each against the surface's depth at that texel (z at uv0,
// plus the slope g across), and the answers blended by where uv falls between them.
float Tap(sampler2D m, float2 uv, float z, float2 uv0, float2 g, float bias, float size, float texel)
{
    float2 t = uv * size - 0.5;
    float2 f = frac(t);
    float2 b = (t - f + 0.5) * texel;           // the centre of the texel up and to the left
    float2 bx = b + float2(texel, 0.0), by = b + float2(0.0, texel), bxy = b + float2(texel, texel);
    float  a = Test(m, b,   z + dot(g, b   - uv0), bias);
    float  c = Test(m, bx,  z + dot(g, bx  - uv0), bias);
    float  d = Test(m, by,  z + dot(g, by  - uv0), bias);
    float  e = Test(m, bxy, z + dot(g, bxy - uv0), bias);
    return lerp(lerp(a, c, f.x), lerp(d, e, f.x), f.y);
}
// The share of nine taps, a texel x softness apart, that sees the sun.
float Lit(sampler2D m, float4 s, float2 g, float bias, float texel)
{
    float2 uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    float  n  = 1.0 / texel;
    float  o  = texel * gB.w;
    float  z  = s.z;
    float lit = Tap(m, uv, z, uv, g, bias, n, texel)
              + Tap(m, uv + float2(-o, -o), z, uv, g, bias, n, texel) + Tap(m, uv + float2(0.0, -o), z, uv, g, bias, n, texel)
              + Tap(m, uv + float2( o, -o), z, uv, g, bias, n, texel) + Tap(m, uv + float2(-o, 0.0), z, uv, g, bias, n, texel)
              + Tap(m, uv + float2( o, 0.0), z, uv, g, bias, n, texel) + Tap(m, uv + float2(-o,  o), z, uv, g, bias, n, texel)
              + Tap(m, uv + float2(0.0,  o), z, uv, g, bias, n, texel) + Tap(m, uv + float2( o,  o), z, uv, g, bias, n, texel);
    return lit / 9.0;
}
// The characters' map, lighter: five taps (the centre and the four corners), since a character's shadow
// is small. Nine more taps in the same branch as the near map's ran the shader out of temp registers.
float Lit5(sampler2D m, float4 s, float2 g, float bias, float texel)
{
    float2 uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    float  n  = 1.0 / texel;
    float  o  = texel * max(gB.w, 0.5);
    float  z  = s.z;
    float lit = Tap(m, uv, z, uv, g, bias, n, texel)
              + Tap(m, uv + float2(-o, -o), z, uv, g, bias, n, texel) + Tap(m, uv + float2( o, -o), z, uv, g, bias, n, texel)
              + Tap(m, uv + float2(-o,  o), z, uv, g, bias, n, texel) + Tap(m, uv + float2( o,  o), z, uv, g, bias, n, texel);
    return lit / 5.0;
}
float Raw(float2 uv)
{
    return saturate((tex2Dlod(sDepth, float4(uv, 0, 0)).r - gZ.x) * gZ.y);
}
// The camera-relative point a pixel shows.
float3 PointAt(float2 uv, float raw)
{
    float  d   = min(raw, 0.99999);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    return wp.xyz / max(wp.w, 1e-6);
}
// The step to the neighbour o away that is nearer in depth, from P.
float3 Near(float2 uv, float raw, float3 P, float2 o)
{
    float a = Raw(uv + o), b = Raw(uv - o);
    return abs(a - raw) < abs(raw - b) ? PointAt(uv + o, a) - P : P - PointAt(uv - o, b);
}
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float  raw = Raw(uv);
    if (raw >= 0.99999)
        return gL.y > 0.5 ? 1.0 : 0.5;                                     // the sky: no change
    float3 P   = PointAt(uv, raw);
    // The facing, for normalBias and slope only: from the neighbours a pixel away, nearer in depth on
    // each axis. The offset along it grows as the sun grazes the surface.
    float3 N     = float3(0.0, 0.0, 1.0);
    float  graze = 1.0;
    [branch] if (gT.z > 0.5)
    {
        float3 dx = Near(uv, raw, P, float2(gZ.z, 0.0));
        float3 dy = Near(uv, raw, P, float2(0.0, gZ.w));
        N = cross(dy, dx);
        N = N / max(length(N), 1e-8);
        N = dot(N, P) > 0.0 ? -N : N;
        float ndl = dot(N, gSun.xyz);
        graze = 1.0 + 3.0 * sqrt(saturate(1.0 - ndl * ndl));
    }

    // The near map where it reaches, blended into the far one over the band from 80% to 90% of its
    // half-width. The far map is read only where the near map does not cover all of the shade.
    float wn = 0.0, litN = 1.0, leafN = 1.0;
    [branch] if (gNB.w > 0.5)
    {
        float3 Qn = P + N * (gNB.y * graze) + gSun.xyz * gT.y;
        float4 sn = Qn.x * gN0 + Qn.y * gN1 + Qn.z * gN2 + gN3;
        float2 en = abs(sn.xy);
        wn = saturate((0.9 - max(en.x, en.y)) * 10.0);
        [branch] if (wn > 0.0)
        {
            float2 g = Slope(N, gN0, gN1, gN2, gG.y) * gT.x;
            litN = Lit(sNear, sn, g, gNB.x, gNB.z);
            [branch] if (gCh.y > 0.5)
                leafN = Lit5(sNearL, sn, g, gNB.x, gNB.z);
        }
    }
    // The far map, fading out over its last tenth, where it ends.
    float litF = 1.0, leafF = 1.0;
    [branch] if (wn < 1.0 && gCh.w > 0.5)
    {
        float3 Qf = P + N * (gB.y * graze) + gSun.xyz * gT.y;
        float4 sf = Qf.x * gSh0 + Qf.y * gSh1 + Qf.z * gSh2 + gSh3;
        float2 ef = abs(sf.xy);
        float  fade = saturate((1.0 - max(ef.x, ef.y)) * 10.0);
        float2 g = Slope(N, gSh0, gSh1, gSh2, gG.x) * gT.x;
        // More slack with distance ([sunshadows] lodBias): the ground the client draws far off is coarser
        // than the terrain in the map.
        float  bF = gB.x + gLod.x * max(length(P) - gLod.y, 0.0);
        litF = lerp(1.0, Lit(sShadow, sf, g, bF, gB.z), fade);
        [branch] if (gCh.z > 0.5)
            leafF = lerp(1.0, Lit5(sFarL, sf, g, bF, gB.z), fade);
    }
    // Solid things stop the sun; leaves stop leafShade of it.
    float leaf  = 1.0 - lerp(leafF, leafN, wn);
    float shade = max(1.0 - lerp(litF, litN, wn), gCh.x * leaf);
    if (gL.y > 1.5)
        return float4(1.0 - leaf, 1.0 - leaf, 1.0 - leaf, 1.0);            // debug 2: the leaves alone
    // The game's fog at this depth: a fogged pixel shows the fog colour, not what the shade falls on.
    float vz    = dot(P, gV.xyz) + gV.w;
    float clear = 1.0 - (gFog.z > 0.5 ? saturate((vz - gFog.x) * gFog.y) : 0.0);
    float  f     = (1.0 - gSun.w * shade * clear) * (1.0 + gL.x * (1.0 - shade) * clear);
    f = (f >= 0.0 && f <= 2.0) ? f : 1.0;
    if (gL.y > 0.5)
        return float4(f, f, f, 1.0);                                       // debug: the shade in grey
    // Shade takes the sky's cool colour and sunlight a warm one ([sunshadows] shadeTint, sunTint).
    float3 c = f * lerp(1.0, gShC.rgb, gShC.w * shade * clear) * lerp(1.0, gSuC.rgb, gSuC.w * (1.0 - shade) * clear);
    return float4(saturate(c * 0.5), 1.0);
}
)HLSL";

    // The sky match: sky pixels (depth at the far plane) faded into the fog's shaped colour, in full at
    // and below the horizon and not at all from skyBand up. At first the sky was scaled by as much as
    // the fog colour was; the game's sky at the horizon is not quite its fog colour, so a far mountain
    // fogged in full and the sky beside it came out in two tones (2026-09-29). Faded into the fog colour
    // itself, they are the same colour at the horizon. Blended by alpha.
    const char* kSkyHlsl = R"HLSL(
sampler2D sDepth : register(s0);
float4 gInv0 : register(c0);
float4 gInv1 : register(c1);
float4 gInv2 : register(c2);
float4 gInv3 : register(c3);
float4 gZ    : register(c4);        // MinZ, 1 / (MaxZ - MinZ), 1 = every pixel is sky (drawn before the world)
float4 gF    : register(c5);        // the fog's shaped colour, band
float4 gA    : register(c6);        // skyMatch
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float raw = gZ.z > 0.5 ? 1.0 : saturate((tex2Dlod(sDepth, float4(uv, 0, 0)).r - gZ.x) * gZ.y);
    if (raw < 0.99999)
        return 0.0;                                     // not sky: nothing
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gInv0 + ndc.y * gInv1 + 0.99 * gInv2 + gInv3;
    float3 dir = normalize(wp.xyz / max(wp.w, 1e-6));
    float  w   = 1.0 - smoothstep(0.0, gF.w, dir.z);
    return float4(gF.rgb, w * gA.x);
}
)HLSL";

    // Our own fog: distance fog, and height fog over it. The distance fog is the game's own, from its
    // start to its end as the dial moves them, but on an S curve (smoothstep) where the game's is a
    // straight ramp: a soft start and a soft arrival. It does not care about height, so far mountains
    // go to the fog colour in one flat layer, as the game draws them; with height fog alone they thinned
    // out at their height and showed every flat face (2026-09-29). The sky gets none of it, as in the
    // game, so a ridge keeps its line against the sky. Far mountains the client draws with the sky, in
    // the sky's depth slice (past the world's, in front of the sky itself), get all of it: they looked
    // like sky to every pass, and stood out as a second ridge behind the fogged one. Then the height fog: fog at the camera's height is gP.x per yard and
    // thins by e every 1/gP.y yards up. Along a line of sight of length L and slope dz, the fog met is
    // x L (1 - e^-k) / k with k = L dz y; toward the sky (no end) it is x / (y dz), and below the horizon
    // all of it. Past cover of the view distance, geometry fades in full into the fog by the view
    // distance. Blended by alpha, into the fog's colour.
    const char* kFogHlsl = R"HLSL(
sampler2D sDepth : register(s0);
float4 gInv0 : register(c0);
float4 gInv1 : register(c1);
float4 gInv2 : register(c2);
float4 gInv3 : register(c3);
float4 gZ    : register(c4);        // MinZ, 1 / (MaxZ - MinZ), the depth of the sky itself, part (see FogDraw)
float4 gC    : register(c5);        // fog colour, 1 = debug
float4 gP    : register(c6);        // density, 1 / height, cover start (yards), 1 / (view distance - cover start)
float4 gD    : register(c7);        // distance fog: start, 1 / (end - start), share (0 = none)
float4 gS    : register(c8);        // direction to the sun, how much the fog is tinted by it ([fog] sunGlow)
float4 gTw   : register(c9);        // the fog's colour toward the sun (brightness 1), brighter by .w into it
float4 gAw   : register(c10);       // ...and away from it
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    // Part 1: drawn before the world, when every pixel is sky.
    float  d0  = (gZ.w > 0.5 && gZ.w < 1.5) ? 1.0 : tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    float  raw = saturate((d0 - gZ.x) * gZ.y);
    bool   sky = raw >= 0.99999;
    // Past the world's depth slice but in front of the sky itself: scenery the client draws with the
    // sky, far mountains. The game's fog covers it in full, so ours does too.
    bool   far = sky && d0 < gZ.z;
    // Part 2: the sky had its fog before the world was drawn.
    if (gZ.w > 1.5 && sky && !far)
        return float4(0.0, 0.0, 0.0, 0.0);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gInv0 + ndc.y * gInv1 + min(raw, 0.99999) * gInv2 + gInv3;
    float3 P   = wp.xyz / max(wp.w, 1e-6);
    float  L   = max(length(P), 1e-3);
    float  dz  = P.z / L;
    float  tau;
    if (sky)
        tau = dz > 0.001 ? gP.x / (gP.y * dz) : 1000.0;
    else
    {
        float k = clamp(L * dz * gP.y, -4.0, 50.0);
        float g = abs(k) < 1e-3 ? 1.0 : (1.0 - exp(-k)) / k;
        tau = gP.x * L * g;
    }
    float fog = 1.0 - exp(-tau);
    if (!sky)
    {
        float dist = smoothstep(0.0, 1.0, saturate((L - gD.x) * gD.y)) * gD.z;
        fog = 1.0 - (1.0 - fog) * (1.0 - dist);
        fog = max(fog, saturate((L - gP.z) * gP.w));
    }
    if (far)
        fog = gD.z > 0.0 ? 1.0 : fog;
    if (gC.w > 1.5)
        return far ? float4(1.0, 0.1, 0.1, 1.0) : float4(0.0, 0.0, 0.0, 1.0);   // debug 2: that scenery
    if (gC.w > 0.5)
        return float4(fog, fog, fog, 1.0);
    // Lit by the sun: warm looking toward it, cool looking away, and brighter looking into it.
    float  cs  = dot(P / L, gS.xyz);
    float  w   = smoothstep(-0.6, 1.0, cs);
    float3 col = gC.rgb * lerp(1.0, lerp(gAw.rgb, gTw.rgb, w), gS.w);
    col *= 1.0 + gTw.w * gS.w * pow(saturate(cs), 8.0);
    return float4(saturate(col), fog);
}
)HLSL";

    // An RGB colour scaled to a brightness (luminance) of 1, as a multiplier: it tints and does not darken.
    void UnitColour(DWORD rgb, float out[3])
    {
        const float r = ((rgb >> 16) & 0xFF) / 255.0f, g = ((rgb >> 8) & 0xFF) / 255.0f, b = (rgb & 0xFF) / 255.0f;
        const float l = 0.299f * r + 0.587f * g + 0.114f * b;
        const float k = l > 1e-3f ? 1.0f / l : 1.0f;
        out[0] = r * k; out[1] = g * k; out[2] = b * k;
    }

    IDirect3DVertexShader9* g_vs = nullptr;
    IDirect3DPixelShader9*  g_ps = nullptr;
    IDirect3DPixelShader9*  g_psSky = nullptr;
    IDirect3DPixelShader9*  g_psFog = nullptr;
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
            if (OgBlob* code = Compile(kSkyHlsl, "skymatch_ps", "ps_3_0"))
            {
                if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_psSky)))
                    g_psSky = nullptr;
                code->lpVtbl->Release(code);
            }
            if (OgBlob* code = Compile(kFogHlsl, "fog_ps", "ps_3_0"))
            {
                if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_psFog)))
                    g_psFog = nullptr;
                code->lpVtbl->Release(code);
            }
            if (g_vs && g_ps)
                Log("sunshadows: shaders compiled%s%s", g_psSky ? "" : " (not the sky match)",
                    g_psFog ? "" : " (not the fog)");
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

namespace
{
    float g_share = 0.0f;
}

float SunShadowsShare()
{
    return g_share;
}

bool SunShadowsDraw(IDirect3DDevice9* dev)
{
    g_share = 0.0f;
    const bool logThis = g_logNext;
    g_logNext = false;
    const SunShadowSettings& ss = g_cfg.sunShadows;
    if (!ss.enabled || g_failed || (ss.strength <= 0.0f && ss.sunlight <= 0.0f && !ss.debug) || !VolumeActive())
        return false;

    IDirect3DTexture9* depth  = DepthWorldTexture();
    IDirect3DTexture9* shadow = ShadowTexture();
    IDirect3DTexture9* nearTex = nullptr;
    D3DMATRIX nearVP = {};
    float nearRange = 0.0f;
    const bool haveNear = ShadowNear(nearTex, nearVP, nearRange);
    IDirect3DTexture9* nearLeaf = haveNear ? ShadowNearLeaves() : nullptr;
    IDirect3DTexture9* farLeaf  = ShadowFarLeaves();
    float sunDir[3], cam[3], player[3];
    D3DMATRIX view, proj, shadowVP;
    const bool haveCam = ShadowWorldCamera(view, proj) || SunCamera(view, proj);
    float realSun[3];   // the dusk fade follows the real sun; the geometry, the shadows' own ([sunshadows] lock)
    if (!depth || !shadow || !ShadowMatrix(shadowVP) || !SunDirection(realSun) || !ShadowSunDirection(sunDir) ||
        !haveCam || !ClientCamera(cam) ||
        !ClientPlayer(player))
    {
        if (logThis)
            Log("sunshadows: skipped: %s", !depth ? "no readable depth" : !shadow ? "no shadow map" :
                "no sun, camera or player");
        return false;
    }

    // As the volumetric light: gone as the sun sets, and [night] strength at night.
    const float sunset = (realSun[2] > 0.0f ? (realSun[2] < 0.1f ? realSun[2] / 0.1f : 1.0f) : 0.0f) * NightScale();
    const float strength = ss.debug ? 1.0f : ss.strength * 0.01f * sunset;
    const float sunlight = ss.debug ? 0.0f : ss.sunlight * sunset;
    // World shadows off: the terrain's baked shadow is kept, the ground's only shade then.
    g_share = ss.debug ? 1.0f : ss.world ? sunset : 0.0f;
    if (strength <= 0.0f && sunlight <= 0.0f)
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
    // 2 x modulate: scene x out + out x scene, so an output of 0.5 leaves the scene as it is.
    d->SetRenderState(dev, D3DRS_SRCBLEND,          D3DBLEND_DESTCOLOR);
    d->SetRenderState(dev, D3DRS_DESTBLEND,         D3DBLEND_SRCCOLOR);
    d->SetRenderState(dev, D3DRS_BLENDOP,           D3DBLENDOP_ADD);
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(depth));
    d->SetTexture(dev, 1, reinterpret_cast<IDirect3DBaseTexture9*>(shadow));
    d->SetTexture(dev, 2, reinterpret_cast<IDirect3DBaseTexture9*>(haveNear ? nearTex : shadow));
    d->SetTexture(dev, 3, reinterpret_cast<IDirect3DBaseTexture9*>(nearLeaf ? nearLeaf : (haveNear ? nearTex : shadow)));
    d->SetTexture(dev, 4, reinterpret_cast<IDirect3DBaseTexture9*>(farLeaf ? farLeaf : shadow));
    for (DWORD st = 0; st < 5; ++st)
    {
        d->SetSamplerState(dev, st, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, st, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(dev, st, D3DSAMP_SRGBTEXTURE, 0);
    }
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    // Off a map reads as far: lit.
    for (DWORD st = 1; st < 5; ++st)
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
    float pc[100] = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            pc[r * 4 + c]      = inv.m[r][c];
            pc[20 + r * 4 + c] = shadowVP.m[r][c];
        }
    pc[16] = minZ; pc[17] = (maxZ - minZ) > 1e-6f ? 1.0f / (maxZ - minZ) : 1.0f;
    D3DSURFACE_DESC dd = {};
    depth->lpVtbl->GetLevelDesc(depth, 0, &dd);
    pc[18] = 1.0f / static_cast<float>(dd.Width ? dd.Width : td.Width);
    pc[19] = 1.0f / static_cast<float>(dd.Height ? dd.Height : td.Height);
    pc[36] = sunDir[0]; pc[37] = sunDir[1]; pc[38] = sunDir[2]; pc[39] = strength;
    // Bias and offset are in texels of each map: yards = texels x the map's width / its size.
    const float size    = static_cast<float>(g_cfg.shadow.size > 0 ? g_cfg.shadow.size : 2048);
    const float farTex  = g_cfg.shadow.range * 2.0f / size;
    const float nearTex_ = nearRange * 2.0f / size;
    // The slack in the depth test: bias texels of the map, plus minGap yards on either map. The map is
    // orthographic, so its depth is yards / span.
    // The bias follows the sun's height (2026-09-29). Flat ground changes depth, as the sun sees it, by
    // 1 / tan(height) texels for every texel across the map, and the taps reach softness + 1.5 texels
    // (the soft edge and the bilinear pair), so less than that and the ground shaded itself in stripes:
    // at a 29-degree sun that is 3.6 texels, over the 3 set. It never goes under [sunshadows] bias, nor
    // over 20. Taken from the sun alone, not from each surface's facing, which is per triangle on a model.
    const float horiz   = sqrtf((std::max)(1.0f - sunDir[2] * sunDir[2], 0.0f));
    const float tanSun  = sunDir[2] / (std::max)(horiz, 1e-3f);
    const float biasTex = (std::min)((std::max)(ss.bias, (ss.softness + 1.5f) / (std::max)(tanSun, 0.1f) * 1.2f),
                                     20.0f);
    pc[40] = (biasTex * farTex + ss.minGap) / span; pc[41] = ss.normalBias * farTex; pc[42] = 1.0f / size; pc[43] = ss.softness;
    pc[44] = sunlight; pc[45] = static_cast<float>(ss.debug);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc[48 + r * 4 + c] = nearVP.m[r][c];
    pc[64] = (biasTex * nearTex_ + ss.minGap) / span; pc[65] = ss.normalBias * nearTex_; pc[66] = 1.0f / size;
    pc[67] = haveNear ? 1.0f : 0.0f;
    // The largest slope, 8 yards of depth a yard, in each map's units: depth (0..1 over span) per uv
    // (0..1 over the map's width).
    pc[68] = 8.0f * g_cfg.shadow.range * 2.0f / span;
    pc[69] = 8.0f * nearRange * 2.0f / span;
    pc[72] = ss.slope;
    pc[73] = ss.sunOffset;
    pc[74] = (ss.slope > 0.0f || ss.normalBias > 0.0f) ? 1.0f : 0.0f;
    pc[76] = view.m[0][2]; pc[77] = view.m[1][2]; pc[78] = view.m[2][2]; pc[79] = view.m[3][2];
    // With our own fog, the fog is drawn after the shade and covers it by itself.
    float fogStart = 0.0f, fogEnd = 0.0f;
    if (!OwnFogActive() && WorldFog(fogStart, fogEnd) && fogEnd > fogStart + 1.0f)
    {
        pc[80] = fogStart; pc[81] = 1.0f / (fogEnd - fogStart); pc[82] = 1.0f;
    }
    pc[84] = ss.leafShade;
    pc[85] = nearLeaf ? 1.0f : 0.0f;
    pc[86] = farLeaf ? 1.0f : 0.0f;
    // World shadows off: the far map holds the world for the volumetric light, and the sun shadows leave
    // it; past the near map nothing is shaded.
    pc[87] = ss.world ? 1.0f : 0.0f;
    pc[88] = ss.lodBias * 0.01f / span;
    pc[89] = ss.lodStart;
    UnitColour(ss.shadeColor, &pc[92]); pc[95] = ss.shadeTint;
    UnitColour(ss.sunColor, &pc[96]);   pc[99] = ss.sunTint;
    d->SetPixelShaderConstantF(dev, 0, pc, 25);

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
    d->SetTexture(dev, 3, nullptr);
    d->SetTexture(dev, 4, nullptr);

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
            "texel, near map %s %.3f yd a texel; bias %.1f texels (set %.1f, by the sun's height), sunOffset %.2f "
            "yd, normalBias %.1f texels, slope %.2f, softness %.1f",
            strength, sunset, sunDir[0], sunDir[1], sunDir[2], farTex, haveNear ? "on," : "off,", nearTex_, biasTex,
            ss.bias, ss.sunOffset, ss.normalBias, ss.slope, ss.softness);
    return true;
}

// Before a Reset, and when the client makes a new device: the shaders too, so a new device gets its own.
void SunShadowsReset()
{
    SafeRelease(g_sb);
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    SafeRelease(g_psSky);
    SafeRelease(g_psFog);
    g_shadersTried = false;
    g_failed = false;
}

void SunShadowsProbe()
{
    g_logNext = true;
}

namespace
{
    // What the sky match did last, logged each time it changes: it runs every frame.
    int g_skyOutcome = -1;

    bool SkyOutcome(int code, const char* what, DWORD client = 0, DWORD shaped = 0)
    {
        if (code != g_skyOutcome)
        {
            g_skyOutcome = code;
            if (code == 0)
                Log("skymatch: drawn, fog 0x%06lX -> 0x%06lX", client & 0xFFFFFF, shaped & 0xFFFFFF);
            else
                Log("skymatch: not drawn: %s (fog 0x%06lX -> 0x%06lX)", what, client & 0xFFFFFF,
                    shaped & 0xFFFFFF);
        }
        return code == 0;
    }
}

bool SkyMatchDraw(IDirect3DDevice9* dev, int part, const D3DMATRIX* viewIn, const D3DMATRIX* projIn)
{
    const FogSettings& f = g_cfg.fog;
    DWORD client = 0, shaped = 0;
    if (f.skyMatch <= 0.0f && !f.skyDebug)
        return SkyOutcome(1, "skyMatch is 0");
    if (g_failed)
        return SkyOutcome(2, "the shaders failed");
    if (!WorldFogColor(client, shaped))
        return SkyOutcome(3, "no fog colour from the world yet");
    if (client == shaped && !f.skyDebug)
        return SkyOutcome(4, "the fog colour is not changed ([fog] darken, desaturate, tint)", client, shaped);

    // The colour the sky fades into: the fog's, as shaped. Debug: red, in full.
    float colour[3];
    for (int i = 0; i < 3; ++i)
        colour[i] = static_cast<float>((shaped >> (16 - 8 * i)) & 0xFF) / 255.0f;
    if (f.skyDebug)
    {
        colour[0] = 1.0f; colour[1] = 0.1f; colour[2] = 0.1f;
    }
    const float amount = f.skyDebug ? 1.0f : f.skyMatch;

    if (part == 2)
        return true;   // the sky was matched before the world, and the sky is all it touches
    IDirect3DTexture9* depth = part == 1 ? nullptr : DepthWorldTexture();
    D3DMATRIX view, proj;
    bool haveCam = false;
    if (viewIn && projIn) { view = *viewIn; proj = *projIn; haveCam = true; }
    else haveCam = ShadowWorldCamera(view, proj) || SunCamera(view, proj);
    if ((!depth && part != 1) || !haveCam)
        return SkyOutcome(6, !depth ? "no readable depth" : "no camera", client, shaped);
    D3DMATRIX camVP, inv;
    Mul(view, proj, camVP);
    if (!Invert(camVP, inv))
        return SkyOutcome(7, "the camera does not invert", client, shaped);
    if (!EnsureResources(dev))
    {
        g_failed = true;
        return SkyOutcome(2, "the shaders failed");
    }
    if (!g_psSky)
        return SkyOutcome(8, "the sky shader did not compile", client, shaped);

    auto* d = dev->lpVtbl;
    IDirect3DSurface9* target = nullptr;
    d->GetRenderTarget(dev, 0, &target);
    if (!target)
        return false;
    D3DSURFACE_DESC td = {};
    target->lpVtbl->GetDesc(target, &td);

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

    d->SetDepthStencilSurface(dev, nullptr);
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
    d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,  TRUE);
    d->SetRenderState(dev, D3DRS_SRCBLEND,          D3DBLEND_SRCALPHA);
    d->SetRenderState(dev, D3DRS_DESTBLEND,         D3DBLEND_INVSRCALPHA);
    d->SetRenderState(dev, D3DRS_BLENDOP,           D3DBLENDOP_ADD);
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(depth));
    d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, 0);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetVertexShader(dev, g_vs);
    d->SetPixelShader(dev, g_psSky);

    float minZ = 0.0f, maxZ = 1.0f;
    ShadowWorldDepthRange(minZ, maxZ);
    float pc[28] = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc[r * 4 + c] = inv.m[r][c];
    pc[16] = minZ; pc[17] = (maxZ - minZ) > 1e-6f ? 1.0f / (maxZ - minZ) : 1.0f;
    pc[18] = part == 1 ? 1.0f : 0.0f;
    pc[20] = colour[0]; pc[21] = colour[1]; pc[22] = colour[2]; pc[23] = f.skyBand;
    pc[24] = amount;
    d->SetPixelShaderConstantF(dev, 0, pc, 7);
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

    if (g_logNext)
        Log("skymatch: fog 0x%06lX -> 0x%06lX, the sky faded into it by %.2f, up to %.2f", client & 0xFFFFFF,
            shaped & 0xFFFFFF, amount, f.skyBand);
    return SkyOutcome(0, "", client, shaped);
}

namespace
{
    int g_fogOutcome = -1;

    bool FogOutcome(int code, const char* what)
    {
        if (code != g_fogOutcome)
        {
            g_fogOutcome = code;
            Log("fog: %s", what);
        }
        return code == 0;
    }
}

// The sky's part and the rest are drawn apart (2026-09-30). Grass and flowers are drawn blended, but they
// write depth over the whole of each card, the see-through parts too, so a pass that finds the sky by depth
// after the world skipped those pixels, and the game's own sky showed through around every blade on a
// skyline: pale halos. The sky is now fogged when the client has drawn it and nothing else (part 1, every
// pixel taken as sky, the frame's own camera given), and the world after it (part 2), so the grass blends
// over our sky as it does over the game's.
bool FogDraw(IDirect3DDevice9* dev, DWORD colour, float density, float distStart, float distEnd, int part,
             const D3DMATRIX* viewIn, const D3DMATRIX* projIn)
{
    const FogSettings& f = g_cfg.fog;
    if (g_failed)
        return FogOutcome(2, "not drawn: the shaders failed");
    IDirect3DTexture9* depth = part == 1 ? nullptr : DepthWorldTexture();
    D3DMATRIX view, proj;
    bool haveCam = false;
    if (viewIn && projIn) { view = *viewIn; proj = *projIn; haveCam = true; }
    else haveCam = ShadowWorldCamera(view, proj) || SunCamera(view, proj);
    if ((!depth && part != 1) || !haveCam)
        return FogOutcome(3, !depth ? "not drawn: no readable depth" : "not drawn: no camera");
    D3DMATRIX camVP, inv;
    Mul(view, proj, camVP);
    if (!Invert(camVP, inv))
        return FogOutcome(4, "not drawn: the camera does not invert");
    if (!EnsureResources(dev))
    {
        g_failed = true;
        return FogOutcome(2, "not drawn: the shaders failed");
    }
    if (!g_psFog)
        return FogOutcome(5, "not drawn: the fog shader did not compile");

    // The view distance is the projection's far plane: m22 = f / (f - n), m32 = -n f / (f - n).
    const float m22 = proj.m[2][2], m32 = proj.m[3][2];
    float farPlane = (fabsf(1.0f - m22) > 1e-6f) ? m32 / (1.0f - m22) : 0.0f;
    if (!(farPlane > 10.0f && farPlane < 100000.0f))
        farPlane = 1000.0f;
    const float coverStart = f.cover * farPlane;

    auto* d = dev->lpVtbl;
    IDirect3DSurface9* target = nullptr;
    d->GetRenderTarget(dev, 0, &target);
    if (!target)
        return false;
    D3DSURFACE_DESC td = {};
    target->lpVtbl->GetDesc(target, &td);

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

    d->SetDepthStencilSurface(dev, nullptr);
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
    d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,  f.debug ? FALSE : TRUE);
    d->SetRenderState(dev, D3DRS_SRCBLEND,          D3DBLEND_SRCALPHA);
    d->SetRenderState(dev, D3DRS_DESTBLEND,         D3DBLEND_INVSRCALPHA);
    d->SetRenderState(dev, D3DRS_BLENDOP,           D3DBLENDOP_ADD);
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(depth));
    d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, 0);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetVertexShader(dev, g_vs);
    d->SetPixelShader(dev, g_psFog);

    float minZ = 0.0f, maxZ = 1.0f;
    ShadowWorldDepthRange(minZ, maxZ);
    float pc[44] = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc[r * 4 + c] = inv.m[r][c];
    pc[16] = minZ; pc[17] = (maxZ - minZ) > 1e-6f ? 1.0f / (maxZ - minZ) : 1.0f;
    pc[20] = ((colour >> 16) & 0xFF) / 255.0f;
    pc[21] = ((colour >>  8) & 0xFF) / 255.0f;
    pc[22] = ((colour      ) & 0xFF) / 255.0f;
    pc[18] = f.skyDepth;
    pc[19] = static_cast<float>(part);
    pc[23] = static_cast<float>(f.debug);
    pc[24] = density;
    pc[25] = 1.0f / f.height;
    pc[26] = coverStart;
    pc[27] = farPlane > coverStart + 1.0f ? 1.0f / (farPlane - coverStart) : 1.0f;
    if (distEnd > distStart + 1.0f && f.distance > 0.0f)
    {
        pc[28] = distStart; pc[29] = 1.0f / (distEnd - distStart); pc[30] = f.distance;
    }
    // Lit by the sun ([fog] sunGlow): the real sun, and less of it at night.
    float sunNow[3];
    if (f.sunGlow > 0.0f && SunDirection(sunNow))
    {
        pc[32] = sunNow[0]; pc[33] = sunNow[1]; pc[34] = sunNow[2];
        pc[35] = f.sunGlow * NightScale();
        UnitColour(f.glowColor, &pc[36]); pc[39] = f.sunBright;
        UnitColour(f.awayColor, &pc[40]);
    }
    d->SetPixelShaderConstantF(dev, 0, pc, 11);
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

    if (g_fogOutcome != 0)
    {
        char line[160];
        snprintf(line, sizeof(line), "drawn: colour 0x%06lX, density %.4f a yard, height %.0f yd, distance fog "
                 "%.0f to %.0f yd, view distance %.0f yd, cover from %.0f yd", colour & 0xFFFFFF, density,
                 f.height, distStart, distEnd, farPlane, coverStart);
        return FogOutcome(0, line);
    }
    return true;
}
