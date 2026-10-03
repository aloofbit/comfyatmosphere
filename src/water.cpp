// water: foam where the water is shallow.
//
// The client draws rivers, lakes and the sea as a run of fixed-function chunks through its water pixel shader
// (comfyfog.cpp, IsWaterShaderDraw). Each chunk is drawn a second time here, right after the client's draw,
// through a shader of ours that puts foam on it, alpha blended over the client's water.
//
// How deep the water is. Before the frame's first water draw, the depth buffer holds what lies under the
// water: the lake bed, the shore, a post or a leg standing in it. That depth is copied once a frame (a
// StretchRect of the readable depth, depth.cpp, out of the scene as DXVK wants it). At a water pixel, the line
// of sight goes through the surface at `rel` (camera-relative) and ends on the bed at s x rel, where s is the
// bed's view depth over the surface's. The bed lies rel.z x (1 - s) yards below the surface: the true depth
// at the point the line of sight meets the bed, whatever the angle. Something in front of the water gives
// s < 1, a negative depth, and no foam; so the foam draws without the depth test.
//
// The foam. It is thickest at the waterline and thins out to nothing at [water] foamWidth yards of depth, or
// [water] foamReach yards out from the waterline, whichever comes first. The second is the depth over the bed's
// slope, from the pixels beside each one. Its
// texture is the edges of moving cells (cellular noise, two sizes), which reads as bubbles, broken into
// patches by a slow noise. Bands of it roll in to the shore. All of it is in world coordinates, so it stays
// on the water as the camera moves. It fades out with distance and with the game's fog, and the night
// darkens it.
//
// The shore (2026-10-02). The water's edge runs up the beach and back (the swash), with a thin broken lip of
// foam on it, and the sand it reaches is darker: wet. The water's chunks reach a little past the waterline,
// so the pass draws on the sand just above it too: there the depth is negative, the sand's height over the
// water. Something standing in front of the water also gives a negative depth; it is told apart by how far
// across the ground the line of sight meets lies from the water's point (sep), and gets no wet sand.
//
// Ripples (2026-10-02): rings that spread out from where a unit stands in the water, the player among them.
// Each stays where it began, so walking leaves a trail of them; up to 32 at once.
//
// The probe (F12, /atmos probe) logs the frame's water draws: how many, where in the frame, the states and
// textures of the first ones, their vertices, and the shader's code. With it, lava and slime can be told
// apart from water, if the client draws them with the same shader.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "bodymask.h"
#include "client.h"
#include "common.h"
#include "config.h"
#include "mapm2.h"
#include "mapterrain.h"
#include "shadow.h"
#include "sun.h"
#include "sunshadows.h"
#include "water.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    const D3DFORMAT kINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    constexpr DWORD kUnderSampler = 15;   // samplers the client never uses
    constexpr DWORD kSceneSampler = 14;
    constexpr UINT  kVsReg = 240;          // vertex constants for the foam; the client's are saved around it
    constexpr UINT  kPsReg = 120;          // pixel constants above any ps_2_0 shader (c31) and our passes (c71)
    constexpr int   kRings = 16;           // ripples held at once (32 until 2026-10-02: the frame rate)
    constexpr float kRingLife = 2.6f;      // seconds a ripple spreads before it is gone

    const char* kVsHlsl = R"HLSL(
float4 gM0 : register(c240);   // rows of world x view x projection
float4 gM1 : register(c241);
float4 gM2 : register(c242);
float4 gM3 : register(c243);
float4 gW0 : register(c244);   // rows of the world matrix: to camera-relative world
float4 gW1 : register(c245);
float4 gW2 : register(c246);
float4 gW3 : register(c247);
float4 gUp : register(c248);   // (view x projection)'s third row: a yard up, in clip space
float4 gSw : register(c249);   // the camera's x and y in the world, seconds, the swell's height (0: none)
float4 gSs : register(c250);   // 1 / waveScale
// The swell: three long trains of waves, each its own way and length (24, 17 and 11 yards x waveScale),
// at the speed of waves on deep water. 0 to 1: the surface only ever rises, so the game's flat water under
// it never shows through a trough.
float Swell(float2 p, float t, float inv)
{
    float h = 0.0;
    float a[3] = { 0.45, 0.35, 0.20 };
    float l[3] = { 24.0, 17.0, 11.0 };
    float r[3] = { 0.15, -0.45, 0.7 };
    [unroll] for (int i = 0; i < 3; ++i)
    {
        float  ang = 0.7854 + r[i];
        float2 d   = float2(cos(ang), sin(ang));
        float  k   = 6.2832 / (l[i] / inv);
        h += a[i] * sin(k * dot(d, p) - sqrt(10.7 * k) * t);
    }
    return 0.5 + 0.5 * h;
}
float2 SwellSlope(float2 p, float t, float inv)
{
    float2 g = 0.0;
    float a[3] = { 0.45, 0.35, 0.20 };
    float l[3] = { 24.0, 17.0, 11.0 };
    float r[3] = { 0.15, -0.45, 0.7 };
    [unroll] for (int i = 0; i < 3; ++i)
    {
        float  ang = 0.7854 + r[i];
        float2 d   = float2(cos(ang), sin(ang));
        float  k   = 6.2832 / (l[i] / inv);
        g += d * (0.5 * a[i] * k * cos(k * dot(d, p) - sqrt(10.7 * k) * t));
    }
    return g;
}
struct O { float4 pos : POSITION; float3 rel : TEXCOORD0; float amp : TEXCOORD1; float gd : TEXCOORD2; };
// uv.y is the water's depth at the vertex, from the map files: 0.0549 at 8.1 yards and 0.1176 at 17.4 in the
// probe, depth / 148. The swell lifts only water 2.5 yards deep and more, in full from 6.5: the grid has a
// point every 4.2 yards, and lifted next to a steep bank its triangles stood up over the bank as straight lines
// (a pond in Tirisfal, 2026-10-02). Nearer the shore the waves are light only.
O main(float3 p : POSITION, float2 uv : TEXCOORD0)
{
    O o;
    float3 rel = (p.x * gW0 + p.y * gW1 + p.z * gW2 + gW3).xyz;
    float  amp = gSw.w * saturate((uv.y * 148.0 - 2.5) / 4.0);
    float  h   = amp * Swell(rel.xy + gSw.xy, gSw.z, gSs.x);
    o.pos = p.x * gM0 + p.y * gM1 + p.z * gM2 + gM3 + h * gUp;
    o.rel = rel + float3(0.0, 0.0, h);
    o.amp = amp;
    o.gd  = uv.y * 148.0;   // the water's depth at the vertex, as the map files give it
    return o;
}
)HLSL";

    const char* kPsHlsl = R"HLSL(
sampler2D sUnder : register(s15);  // the depth under the water (INTZ), copied before the first water draw
sampler2D sScene : register(s14);  // the screen before the first water draw: what lies under the water
sampler2D sFoamTex : register(s12); // the game's own foam (WATERFOAMLOOP2.blp), tiled over the water
float4 gZ    : register(c120);     // the projection's m22 and m32; the viewport's MinZ, 1 / (MaxZ - MinZ)
float4 gVz   : register(c121);     // the view's third column: a camera-relative point's view depth
float4 gScr  : register(c122);     // 1 / width, 1 / height, seconds, strength
float4 gFoam : register(c123);     // 1 / foamWidth, 1 / foamScale, speed, debug view
float4 gCol  : register(c124);     // the foam's colour, the distance it is gone at
float4 gFogC : register(c125);     // the game's fog colour, 1 / the distance it fades over
float4 gFog  : register(c126);     // the game's fog start, 1 / (end - start), 1 when known
float4 gCam  : register(c127);     // the camera in the world
float4 gReach : register(c128);    // 1 / foamReach, ripples' strength, wet sand's darkness, rings in use
float4 gRingD[16] : register(c146); // each ripple's way: the way its maker walked (x, y), 0 standing still
float4 gRing[16] : register(c130); // the ripples: where each began (feet, camera-relative); w its age in
                                   // seconds plus 8 x its speed in tenths of a yard a second, negative for none
float4 gSun  : register(c162);     // the way to the sun, its glint's strength
float4 gDeep : register(c163);     // the colour deep water turns, how much of our water is drawn
float4 gAbs  : register(c164);     // the light the water absorbs a yard, by channel; refraction in yards
float4 gSky  : register(c165);     // the sky high up; the waves' strength
float4 gSunC : register(c166);     // the sun's colour; whitecaps
float4 gTrail[32] : register(c170); // the wakes: 4 trails of 8 points, newest first: camera-relative feet, age
                                    // in seconds (negative: no point)
float4 gWake : register(c202);     // trails in use, the wake's strength, 1 with the swash, its height (yards)
float4 gFT   : register(c169);     // 1 with the foam texture, 1 / its size in yards, its strength; the edge line
float4 gSw2  : register(c168);     // the shore waves' height (the swell's height x 0.25; 0: none); 1 on a body
float4 gWave : register(c167);     // 1 / waveScale; the part drawn (0 all, 1 the sand, 2 the water); 1 when
                                   // the screen copy is there; the sky reflection's strength

// Hashes without sin (Dave Hoskins): sin of a large argument loses its precision on a GPU.
float Hash1(float2 i)
{
    float3 p = frac(i.xyx * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}
float2 Hash2(float2 i)
{
    float3 p = frac(i.xyx * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yzx + 33.33);
    return frac((p.xx + p.yz) * p.zy);
}
float ValueNoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(Hash1(i), Hash1(i + float2(1, 0)), f.x),
                lerp(Hash1(i + float2(0, 1)), Hash1(i + float2(1, 1)), f.x), f.y);
}
// The edges of cells round points that drift: 1 on an edge, 0 at a cell's middle.
float Lace(float2 p, float t)
{
    float2 i = floor(p), f = frac(p);
    float d1 = 8.0, d2 = 8.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        float2 g = float2(x, y);
        float2 o = 0.5 + 0.4 * sin(t + 6.2831 * Hash2(i + g));
        float2 r = g + o - f;
        float  d = dot(r, r);
        d2 = d < d1 ? d1 : min(d2, d);
        d1 = min(d1, d);
    }
    return 1.0 - saturate((sqrt(d2) - sqrt(d1)) * 2.5);
}
// The swell: three long trains of waves, each its own way and length (24, 17 and 11 yards x waveScale),
// at the speed of waves on deep water. 0 to 1: the surface only ever rises, so the game's flat water under
// it never shows through a trough.
float Swell(float2 p, float t, float inv)
{
    float h = 0.0;
    float a[3] = { 0.45, 0.35, 0.20 };
    float l[3] = { 24.0, 17.0, 11.0 };
    float r[3] = { 0.15, -0.45, 0.7 };
    [unroll] for (int i = 0; i < 3; ++i)
    {
        float  ang = 0.7854 + r[i];
        float2 d   = float2(cos(ang), sin(ang));
        float  k   = 6.2832 / (l[i] / inv);
        h += a[i] * sin(k * dot(d, p) - sqrt(10.7 * k) * t);
    }
    return 0.5 + 0.5 * h;
}
float2 SwellSlope(float2 p, float t, float inv)
{
    float2 g = 0.0;
    float a[3] = { 0.45, 0.35, 0.20 };
    float l[3] = { 24.0, 17.0, 11.0 };
    float r[3] = { 0.15, -0.45, 0.7 };
    [unroll] for (int i = 0; i < 3; ++i)
    {
        float  ang = 0.7854 + r[i];
        float2 d   = float2(cos(ang), sin(ang));
        float  k   = 6.2832 / (l[i] / inv);
        g += d * (0.5 * a[i] * k * cos(k * dot(d, p) - sqrt(10.7 * k) * t));
    }
    return g;
}

// The small waves, as a facing: six trains, each its own way and length (7 to 0.85 yards), at the speed of
// waves on deep water, and fine noise over them. Heights are never drawn; only the light sees them.
float3 WaveNormal(float2 p, float t, float strength, float2 swell)
{
    float2 g = 0.0;
    float  a[6] = { 0.0, 0.6, -0.5, 1.2, -1.1, 0.3 };      // the way, from the wind's (45 degrees)
    float  l[6] = { 7.0, 4.3, 2.9, 1.9, 1.3, 0.85 };       // the length in yards
    float  s[6] = { 0.10, 0.10, 0.09, 0.08, 0.07, 0.06 };  // the steepness
    [unroll] for (int i = 0; i < 6; ++i)
    {
        float  ang = 0.7854 + a[i];
        float2 d   = float2(cos(ang), sin(ang));
        float  k   = 6.2832 / (l[i] / gWave.x);
        float  w   = sqrt(10.7 * k);
        g += d * (s[i] * cos(k * dot(d, p) - w * t));
    }
    float2 q = p * 1.6 + float2(t * 0.4, t * 0.25);
    float  n0 = ValueNoise(q);
    g += float2(ValueNoise(q + float2(0.15, 0.0)) - n0, ValueNoise(q + float2(0.0, 0.15)) - n0) * 0.35;
    return normalize(float3(-(g * strength + swell), 1.0));
}

)HLSL"
    // Split in two: MSVC takes no string literal longer than 16 KB (C2026).
    R"HLSL(
float4 main(float3 rel : TEXCOORD0, float amp : TEXCOORD1, float gd : TEXCOORD2, float2 vpos : VPOS) : COLOR
{
    // Seen from under the water: no foam.
    clip(-rel.z);
    float2 uv  = (vpos + 0.5) * gScr.xy;
    float  raw = tex2Dlod(sUnder, float4(uv, 0, 0)).r;
    // The client draws the world in a slice of the depth range (0..0.94 here) and its far terrain in another
    // (0.955..0.96) with a camera of its own (shadow.cpp, SettleVotes). A pixel past the water's slice is far
    // terrain or sky, and decoded with the water's camera it gave a depth that was nonsense: the sea bed showed
    // only where the near terrain reached, a pale block with straight sides at the edge of it (Westfall,
    // 2026-10-02). There the map's own depth stands in (gd, the map files' MCLQ at each vertex).
    const bool farSlice = raw > gZ.z + 1.0 / gZ.w - 1e-5;
    float  den = (raw - gZ.z) * gZ.w - gZ.x;
    float  zg  = farSlice ? 1e6 : gZ.y / (abs(den) > 1e-9 ? den : -1e-9);         // the bed's view depth
    float  zw  = max(dot(rel, gVz.xyz), 1e-3);                                     // the surface's
    float  depth = farSlice ? gd : rel.z * (1.0 - zg / zw);                       // yards under the surface
    // On a body under the water (a character's legs, drawn in a draw of their own, below): it is seen through
    // the water, never a shore: no foam, no lip, no edge line (2026-10-02).
    const bool onBody = gSw2.y > 0.5;
    if (onBody)
        depth = max(depth, 0.5);
    float  t   = gScr.z * gFoam.z;
    float  dist = length(rel);
    // From 40 yards out the map's depth takes over from the depth copy's, by 90 all of it: the far terrain is
    // in a slice of its own (farSlice), and where it met the near terrain the water's colour jumped along a
    // straight line (2026-10-02). So both sides agree well before they meet. Not on a body.
    const float mapK = farSlice ? 1.0 : smoothstep(40.0, 90.0, dist) * (gSw2.y > 0.5 ? 0.0 : 1.0);
    depth = lerp(depth, gd, mapK);
    // Two parts (2026-10-02): the water, depth tested and writing depth, and the sand beside it, not tested.
    // Each pixel belongs to one: where our water is drawn (wv > 0), the water part.
    float  wvIs = smoothstep(0.0, 0.03, depth) * gDeep.w * gWave.z;
    if (gWave.y > 0.5)
    {
        // The first 0.15 yards of water go with the sand part, untested: there the water's plane lies a hair
        // under the sand, the depth test turned the water part away, the sand part left it to the water part,
        // and the game's own water showed through as a light line along the shore (2026-10-02). There is no
        // swell that near the shore, so nothing needs the test there.
        const bool waterPart = wvIs > 0.0 && depth > 0.15;
        clip(gWave.y > 1.5 ? (waterPart ? 1.0 : -1.0) : (waterPart ? -1.0 : 1.0));
    }
    // Above the water nothing is left to draw here: the foam, the lip, the ripples and the wake are all on the
    // water, and the wet sand and the swash have a pass of their own. So such a pixel leaves now, before the rest:
    // the sand part is drawn without the depth test, and every pixel of every chunk ran the whole shader, dunes
    // and all in front of the sea behind them (2026-10-02: once every chunk was ours, the frame rate fell
    // through the floor). Without the swash the lip reaches 0.4 yards up the sand.
    clip(depth + (gWake.z > 0.5 ? 0.05 : 0.45));

    // How far the waterline is, across the water: the depth over the bed's slope. The slope is how fast the
    // depth grows per yard of the surface, from the pixels beside this one. On a gentle beach the depth stays
    // small for many yards, and the foam went 20 yards out by depth alone (2026-10-02).
    float2 gx  = ddx(rel.xy), gy = ddy(rel.xy);
    float  ex  = ddx(depth), ey = ddy(depth);
    float  det = gx.x * gy.y - gx.y * gy.x;
    float2 grad = abs(det) > 1e-8 ? float2(ex * gy.y - gx.y * ey, gx.x * ey - gy.x * ex) / det : float2(0, 0);
    float  slope = max(length(grad), 0.02);
    float  reach = depth / slope;
    // The steeper what lies under the water, the less depth the foam takes: a leg or a post gets a thin collar
    // at the surface. Without it a character's legs were white down to the knees (2026-10-02).
    float  shore = saturate(1.0 - max(depth * gFoam.x * (1.0 + slope * 4.0), reach * gReach.x));
    // The foam rides the water (2026-10-02; it lay on the waves like a flat sheet): the swell's slope bends its
    // pattern. No drift toward the shore: the shore's way changes from pixel to pixel, and a drift that grows
    // with time would tear the pattern apart.
    float2 gdir  = grad / max(length(grad), 1e-4);
    float2 swS   = amp * SwellSlope(rel.xy + gCam.xy, gScr.z, gWave.x);
    float2 wp    = ((rel.xy + gCam.xy) + swS * 1.5) * gFoam.y;
    // The texture: soft patches, with bubbles (cell edges, two sizes) in them.
    float  lace  = Lace(wp, t * 0.8) * 0.6 + Lace(wp * 2.3 + 17.0, t * 1.1) * 0.4;
    float  soft  = ValueNoise(wp * 0.25 + t * 0.07) * 0.5 + ValueNoise(wp * 0.6 - t * 0.05) * 0.3 +
                   ValueNoise(wp * 1.7 + t * 0.1) * 0.2;
    float  tex   = soft * 0.7 + lace * 0.3;
    // How much foam there is: most near the waterline, in bands that roll in to the shore.
    float  band  = sin(reach * 4.0 + t * 1.6) * 0.5 + 0.5;
    // At most 0.7, so the foam stays broken at the waterline: at 1 it was a solid white strip (2026-10-02).
    float  cover = shore * (0.45 + 0.25 * band);
    float  foam  = saturate((tex - (1.0 - cover)) * 3.0) * saturate(cover * 2.5);
    foam *= depth > -0.05 ? 1.0 : 0.0;

    // Where the ground the line of sight meets lies, across: near the water's point for the shore, far from it
    // for something standing in front of the water (a character, a post), which gets no wet sand.
    float  sep = length(rel.xy) * saturate(1.0 - zg / zw);
    float  near = sep < 4.0 ? 1.0 : 0.0;
    // Wet sand: darker, from the waterline to 2 yards up the beach, drying out at the top. Only above the
    // water: under it the client's water darkens the sand already. Only on ground that lies flat: a character
    // standing in the water gives the same depth and reach as sand, and was darkened too (2026-10-02). The
    // ground's facing is from the point the line of sight meets, P, and its neighbours. The water's chunks
    // reach onto the sand in steps of a cell (4 yards), and the wet sand ends where they do.
    float3 P    = rel * (zg / zw);
    float3 n    = cross(ddx(P), ddy(P));
    float  flat = abs(n.z) > 0.85 * length(n) ? 1.0 : 0.0;
    // The swash: the water's edge runs up the beach and back, up to 0.7 yards, a little out of step along the
    // shore. A thin broken lip of foam rides on it. Until 2026-10-02 the waterline was a solid white line.
    float  swash = -0.7 * (0.5 + 0.5 * sin(t * 0.7 + soft * 4.0));
    float  lip   = exp(-pow((reach - swash) / 0.18, 2.0)) * smoothstep(0.35, 0.65, tex) * 0.55;
    lip *= near * (depth > -0.4 ? 1.0 : 0.0) * (depth < 0.3 ? 1.0 : 0.0);
    // Above the water only on flat ground: a character standing in the water is above it too, and the lip
    // whitened its hips (2026-10-02).
    lip *= depth > 0.0 ? 1.0 : flat;
    // Only on the water: on the sand the water's chunks end in steps of a cell, and the lip's edge was jagged
    // there (2026-10-02). The wet sand pass draws the sand's part.
    lip *= smoothstep(-0.03, 0.02, depth);
    // With the swash on, its own lip on the sand is the water's edge: this one, fixed at the waterline, was a
    // second shoreline (2026-10-02).
    lip *= 1.0 - gWake.z;
    // The wet sand is a pass of its own since 2026-10-02 (kWetPsHlsl): drawn here, it ended where the water's
    // chunks end, in steps of a cell, and its edge was jagged.
    float  wet  = 0.0;

    // Ripples: thin rings that spread out from where they began and fade. Each stays where it began, so one
    // walking through the water leaves a trail of them (the DLL starts them, RingsUpdate).
    float  surf = rel.z;
    // The strongest ring at each point, not their sum: summed, a walker's overlapping rings filled the V of
    // its wake solid white (2026-10-02).
    float  ring = 0.0;
    // The rings are waves in the surface too (2026-10-02): a small rise and a trough behind it, as light only,
    // so the glint and the sky in the water move where someone walks. ringSlope is their slope, summed.
    float2 ringSlope = 0.0;
    // The noise that makes each ring uneven, once for all of them: per ring it is shifted by the ring's slot.
    // Two noise reads a ring for 32 rings took the game to a frame a minute (2026-10-02).
    float2 wpos  = rel.xy + gCam.xy;
    float  rn1   = ValueNoise(wpos * 1.3);
    float  rn2   = ValueNoise(wpos * 0.8 + 11.0);
    // Only the slots in use (gReach.w), the live rings first: not unrolled, the same count for every pixel.
    [loop] for (int i = 0; i < (int)gReach.w; ++i)
    {
        float4 g   = gRing[i];
        {
            float  sub = surf - g.z;                              // how deep the feet were
            float  on  = (sub > 0.05 ? 1.0 : 0.0) * (sub < 3.0 ? 1.0 : 0.0);
            float  sp  = floor(g.w * 0.125);
            float  age = g.w - sp * 8.0;
            float  ph  = saturate(age * (1.0 / 2.6));
            // How much of the body is in the water sets the ring's size (2026-10-02): feet alone small, waist
            // deep the largest, all under small again. A body is about two yards tall.
            float  size = smoothstep(0.05, 0.9, sub) * (1.0 - 0.75 * smoothstep(1.1, 2.0, sub));
            // Not a perfect circle: its edge wanders and its strength comes and goes along it, each ring its own
            // way (a noise in the world, shifted by the ring's slot).
            float  shift = frac(i * 0.618);
            // A smooth wave of the noise, not frac(): frac jumps from 1 back to 0, and each ring had a hard tear
            // across it where it did (2026-10-02).
            float  d   = length(rel.xy - g.xy) + 0.5 * sin(6.2832 * (rn1 + shift)) * (0.15 + 0.5 * ph);
            float  rad = (0.3 + age * sp * 0.065) * (0.45 + 0.55 * size);
            float  x   = (d - rad) / (0.025 + 0.035 * ph);
            float  fade = on * (1.0 - ph) * (1.0 - ph) * size * (0.35 + 0.65 * (0.5 + 0.5 * sin(6.2832 * (rn2 + shift * 1.7))))
                          * smoothstep(0.0, 0.25, age);   // fades in: it popped in at full strength
            // A walker's ring fades out fast on the side behind it, where it came from, and lasts ahead and to the
            // sides (2026-10-02). Standing still, the way is 0 and the ring is even.
            float2 way  = gRingD[i].xy;
            float2 outw = (rel.xy - g.xy) / max(d, 1e-3);
            float  back = saturate(-dot(outw, way));
            fade *= 1.0 - back * saturate(0.4 + ph * 1.5);
            // A walker's rings lose their foam line fast: left on, a trail of them drew a whitish streak far behind
            // the wake (2026-10-02). Standing still spreads at 1.1 yards a second, sp 11.
            float walker = sp > 11.5 ? 1.0 : 0.0;
            ring = max(ring, fade * exp(-x * x) * lerp(1.0, exp(-age * 3.0), walker));
            // The wave: one crest and the trough after it, wider than the foam's line, its slope along the way
            // out from where it began.
            float  wv2 = 0.18 + 0.25 * ph;
            float  y   = (d - rad) / wv2;
            float  dh  = (1.0 - 2.0 * y * y) * exp(-y * y) / wv2;   // the slope of y * exp(-y^2)
            float2 out2 = (rel.xy - g.xy) / max(d, 1e-3);
            ringSlope += out2 * (dh * fade * 0.065 * lerp(1.0, 0.6, walker));
        }
    }
    ring = saturate(ring * gReach.y) * (0.3 + 0.7 * soft) * (depth > -0.05 ? 1.0 : 0.0);

    // Shore waves (2026-10-02): crests along the shore that roll in toward it. Their phase is the distance to
    // the waterline (reach), so they come about 4 yards apart on any slope: by depth they bunched into thin
    // lines on a steep stretch. They come in sets along the shore (a slow noise), and fade out past 9 yards and
    // past 3.5 yards of depth. Too short for the water's grid (a point every 4.2 yards) to lift: they are
    // drawn as light, sloping toward the shore, and as foam where they break, in the last 3 yards.
    float  sets  = smoothstep(0.25, 0.75, ValueNoise(wp * 0.04 + float2(t * 0.03, -t * 0.02)));
    float  sEnv  = smoothstep(0.1, 1.0, reach) * (1.0 - smoothstep(5.0, 9.0, reach)) *
                   (1.0 - smoothstep(2.0, 3.5, depth)) * sets;
    float  sPh   = reach * 1.6 + t * 1.4 + soft * 1.2;
    float  sS    = 0.5 + 0.5 * sin(sPh);
    float  crest = sS * sS;
    float2 shoreSlope = gdir * (gSw2.x * sEnv * sS * cos(sPh) * 1.6);
    float  brk = crest * crest * sEnv * smoothstep(3.0, 0.5, reach) * (0.4 + 0.6 * lace) * (gSw2.x > 0.0 ? 1.0 : 0.0);
    foam = max(foam, brk * 0.7);

    // Whitecaps: foam in patches on the waves out where the water is deep, drifting with the wind.
    float  capN = ValueNoise(wp * 0.07 + float2(t * 0.03, t * 0.02));
    float  caps = smoothstep(0.62, 0.8, capN * 0.55 + tex * 0.45) * gSunC.w * saturate(depth - 1.5);
    // Only the line at the waterline and the ripples (2026-10-02): the patches near the shore, the breaking shore
    // waves and the whitecaps were a layer on top of the water that did not fit the game's painted look. They
    // are kept here, unused, for the foam in the game's own style (the next step).
    foam = max(lip, ring);
    // The shore foam, in the game's own art (2026-10-02): its foam texture, two layers drifting across each
    // other, shows where our shapes say (near the shore, in the bands rolling in), and only on the water.
    float2 fp  = (rel.xy + gCam.xy + swS * 1.5) * gFT.y;
    float  fa1 = tex2D(sFoamTex, fp + float2(t * 0.010, t * 0.006)).a;
    float  fa2 = tex2D(sFoamTex, fp * 0.71 + float2(0.37 - t * 0.007, 0.21 + t * 0.011)).a;
    float  ftx = 0.6 * fa1 + 0.4 * fa2;
    // With the swash on, the foam is placed from the moving edge, not the fixed waterline: the water has run up
    // the sand by up (yards of height, the wet sand pass's swash, the same timing), so here it is that much
    // deeper below the edge (2026-10-02: the foam stayed at the old waterline while the edge moved).
    float  depthS = depth, reachS = reach;
    {
        float2 A2  = rel.xy + gCam.xy;
        float  ph2 = sin(A2.x * 0.21 + A2.y * 0.17) * 1.7 + sin(A2.x * 0.07 - A2.y * 0.09) * 2.3;
        float  sg  = 0.5 + 0.5 * sin(gScr.z * 0.55 + ph2);
        float  upS = gWake.w * sg * sg * gWake.z;
        depthS = depth + upS;
        reachS = depthS / slope;
    }
    float  shoreS = saturate(1.0 - max(depthS * gFoam.x * (1.0 + slope * 4.0), reachS * gReach.x));
    float  bandS  = sin(reachS * 4.0 + t * 1.6) * 0.5 + 0.5;
    float  fshape = shoreS * (0.55 + 0.45 * bandS);
    // Only the texture's denser parts pass, even at the waterline: with the whole of it, the band near the shore
    // was solid white and the texture's own shapes were lost (2026-10-02).
    float  fcut  = 1.0 - 0.7 * fshape;
    float  fgame = smoothstep(fcut, fcut + 0.3, ftx) * saturate(fshape * 2.0);
    // From the edge line on, never onto the sand: it starts where our water does (2026-10-02: started a quarter
    // of a yard out, it left a strip of clear water between the line and the bubbles).
    float  fback = smoothstep(0.0, 0.03, depth);
    foam = max(foam, fgame * gFT.z * gFT.x * fback);

)HLSL"
    R"HLSL(
    // The wake (2026-10-02): behind anyone moving through the water, along the path they took, a V of two arms
    // at about 20 degrees either side, and churned water just behind the body. The arms are waves (their slope
    // goes into the light, as the ripples' does), with a little of the game's foam on them. Each trail is up to
    // 8 points of the path, newest first; a pixel takes the segment of the path nearest to it.
    float2 wakeSlope = 0.0;
    float  wakeFoam  = 0.0;
    [loop] for (int wi = 0; wi < (int)gWake.x; ++wi)
    {
        // Far from the trail's maker, nothing: a trail is at most some 5 yards long and its arms reach 2 out.
        float2 headOff = rel.xy - gTrail[wi * 8].xy;
        [branch] if (dot(headOff, headOff) > 18.0 * 18.0)
            continue;
        float  bestLat = 1e6, bestS = 0.0, bestAge = 1e6, bestSub = 0.0;
        float2 bestDir = 0.0;
        float  sAcc = 0.0;
        [loop] for (int wj = 0; wj < 7; ++wj)
        {
            float4 a = gTrail[wi * 8 + wj], b = gTrail[wi * 8 + wj + 1];
            if (b.w < 0.0)
                break;
            float2 ab  = b.xy - a.xy;
            float  len = max(length(ab), 1e-3);
            float  tt  = saturate(dot(rel.xy - a.xy, ab) / (len * len));
            float2 q   = a.xy + ab * tt;
            float  lat = length(rel.xy - q);
            if (lat < bestLat)
            {
                bestLat = lat;
                bestS   = sAcc + tt * len;
                bestAge = lerp(a.w, b.w, tt);
                bestSub = surf - lerp(a.z, b.z, tt);
                bestDir = (rel.xy - q) / max(lat, 1e-3);
            }
            sAcc += len;
        }
        // How much of the body is in the water, as for the ripples: feet small, waist the most, under small.
        float size = smoothstep(0.05, 0.9, bestSub) * (1.0 - 0.75 * smoothstep(1.1, 2.0, bestSub));
        // Gone within some 4 yards behind the body and 2 seconds (2026-10-02: by age alone it reached 8 yards back).
        float life = exp(-bestAge * 1.4) * smoothstep(0.0, 0.6, bestS) * (1.0 - smoothstep(1.5, 4.0, bestS)) * size;
        // The arms, a crest and the trough inside it, widening with the distance behind.
        float armAt = bestS * 0.36;
        float armW  = 0.22 + 0.06 * bestS;
        float y     = (bestLat - armAt) / armW;
        float arm   = exp(-y * y);
        float dArm  = (1.0 - 2.0 * y * y) * exp(-y * y) / armW;
        wakeSlope += bestDir * (dArm * life * 0.11);
        // The churned water close behind the body: foam, narrow, gone in a second and a half.
        float churn = exp(-pow(bestLat / (0.35 + 0.08 * bestS), 2.0)) * exp(-bestAge * 2.5) * size *
                      smoothstep(0.0, 0.4, bestS) * (1.0 - smoothstep(1.0, 2.5, bestS));
        wakeFoam = max(wakeFoam, (arm * 0.5 * life + churn * 0.65) * (0.4 + 0.6 * ftx));
    }
    wakeSlope *= gWake.y;
    foam = max(foam, saturate(wakeFoam * gWake.y) * (depth > 0.05 ? 1.0 : 0.0));
    foam = onBody ? 0.0 : foam;
    caps *= 0.0;
    if (gFoam.w > 2.5 && gFoam.w < 3.5)
        clip(-1.0);   // debug 3: the wet sand pass shows alone
    if (gFoam.w > 1.5 && gFoam.w < 2.5)
        return float4(foam.xxx, 1.0);
    if (gFoam.w > 4.5 && gFoam.w < 5.5)   // debug 5: the ripples' foam red, their waves green, the wake blue
        return float4(saturate(ring * gReach.y), saturate(length(ringSlope * gReach.y * 2.5) * 4.0),
                      saturate(wakeFoam * gWake.y + length(wakeSlope) * 4.0), 1.0);
    if (gFoam.w > 0.5 && gFoam.w < 1.5)
    {
        float g = saturate(depth * gFoam.x * 0.25);
        return depth > -0.05 ? float4(g, 0.15, 1.0 - g, 1.0) : float4(0, 0, 0, 1);
    }
    float  fogF = gFog.z > 0.5 ? saturate((dist - gFog.x) * gFog.y) : 0.0;
    float3 c = lerp(gCol.rgb, gFogC.rgb, fogF);
    float fade = saturate((gCol.w - dist) * gFogC.w);
    // One blend for both: the wet sand darkens what is there (black at wetA), and the foam goes over that.
    float  fa = saturate(foam * gScr.w * fade);
    float  wa = saturate(wet * gReach.z * fade);
    float  a  = 1.0 - (1.0 - wa) * (1.0 - fa);
    float3 land = a > 1e-4 ? c * fa / a : c;

    // The surface: drawn by us where the water is (depth > 0), over the game's. Without the screen copy, or
    // with [water] surface 0, the game's water shows with the foam over it, as before.
    float  wv = smoothstep(0.0, 0.03, depth) * gDeep.w * gWave.z;
    // With the swash on, the game's own water is covered where its chunks reach onto the sand and where ours
    // fades in: what lay there before the water (the screen copy: the sand, wet, with the swash film) takes its
    // place. Through ours, the game's pale water showed as a second shoreline (2026-10-02).
    const bool hideGame = gWave.z > 0.5 && gWake.z > 0.5;
    float3 under0 = hideGame ? tex2Dlod(sScene, float4(uv, 0, 0)).rgb : 0.0;
    [branch] if (wv <= 0.0)
        return hideGame ? float4(lerp(under0, land, a), 1.0) : float4(land, a);
)HLSL"
    R"HLSL(
    float3 dir = rel / max(dist, 1e-3);
    // The waves calm with distance: past a few dozen yards a pixel covers many of them, and they shimmered.
    float3 N   = WaveNormal(rel.xy + gCam.xy, t, gSky.w / (1.0 + dist / 60.0),
                            swS + shoreSlope + ringSlope * gReach.y * 2.5 + wakeSlope);
    // What lies under, bent by the waves; not where that would take something in front of the water.
    float2 ruv = uv + N.xy * (gAbs.w * saturate(depth) / max(dist, 2.0)) * float2(1.0, -1.0);
    float  rr  = tex2Dlod(sUnder, float4(ruv, 0, 0)).r;
    float  rd  = (rr - gZ.z) * gZ.w - gZ.x;
    float  zr  = rr > gZ.z + 1.0 / gZ.w - 1e-5 ? 1e6 : gZ.y / (abs(rd) > 1e-9 ? rd : -1e-9);
    // Only onto the bed: not onto something in front of the water, and not onto anything standing well above the
    // bed here. A character's legs under the water are in the screen copy, and the bent look landed on them
    // beside the character: a shifted ghost of it round its edges (2026-10-02).
    // And never on a body: bent, a character's legs under the water showed the bed through them and warped.
    ruv = zr > zw && zr > zg - 0.75 && !onBody ? ruv : uv;
    float3 bed = tex2Dlod(sScene, float4(ruv, 0, 0)).rgb;
    // Through the water the light is absorbed, red first, along the line of sight's path under the surface,
    // and the water's own colour takes its place.
    // Through far terrain's pixels, the map's depth along the line of sight.
    float  path = lerp(dist * max(zg / zw - 1.0, 0.0), gd * dist / max(-rel.z, 0.5), mapK);
    // A body under the water is covered more than the bed beside it ([water] cover, Underwater Cover): at 0.5
    // three times the water it is seen through, and at least half a yard of it. By the bare distance, legs just under the surface showed in their full
    // colour, as if they stood beside the water rather than in it (2026-10-02).
    path = onBody ? max(path * (1.0 + 4.0 * gSw2.z), gSw2.z) : path;   // Underwater Cover: 0.5 = x3, half a yard
    float3 T    = exp(-gAbs.rgb * path);
    // Seen through it, the bed takes the water's hue, keeping its own brightness, more with every yard of water.
    // Filtering alone could not do it: orange sand has hardly any blue to keep, and under blue water it turned
    // olive green whatever the colour was set to (2026-10-02).
    float3 tint = gDeep.rgb / max(dot(gDeep.rgb, float3(0.299, 0.587, 0.114)), 1e-3);
    float  lum  = dot(bed, float3(0.299, 0.587, 0.114));
    // Half of it even in the thinnest water at the very edge: the water has its colour right up to the foam line
    // (2026-10-02: clear at the edge, sandy water came between the blue and the line).
    bed = lerp(bed, lum * tint, 0.7 + 0.2 * (1.0 - exp(-2.5 * path)));
    float3 body = bed * T + gDeep.rgb * (1.0 - T);
    // The sky in it: more at a glancing look (Fresnel). The horizon is the game's fog colour.
    float  cosv = saturate(-dot(N, dir));
    // At most 0.6: with the full 0.98 a low camera saw the far sea as the pale horizon alone, the colour of the
    // sand (2026-10-02). The game's own water keeps its colour to the horizon.
    float  F    = 0.02 + gWave.w * pow(1.0 - cosv, 5.0);
    float3 R    = reflect(dir, N);
    float3 sky  = lerp(gFogC.rgb, gSky.rgb, saturate(R.z * 2.5 + 0.35));
    float  sd   = saturate(dot(R, gSun.xyz));
    float3 glint = gSunC.rgb * gSun.w * (pow(sd, 700.0) * 8.0 + pow(sd, 60.0) * 0.25);
    float3 water = lerp(body, sky, F) + glint;
    // The ripples lit as the swell is not: brighter on the side of a ring facing the sun, darker behind it, so
    // they show from any side and not only in the glint (2026-10-02).
    float2 sunXY = gSun.xy / max(length(gSun.xy), 0.2);
    // At most a fifth brighter (2026-10-02: at 0.3 the wake and the ripples glowed).
    water *= 1.0 + clamp(dot(ringSlope * gReach.y * 2.5 + wakeSlope, sunXY) * 2.5, -0.2, 0.18);
    water = lerp(water, gFogC.rgb, fogF);
    // The foam on it, lit as the water is: brighter on a slope toward the sun, darker on the back of a wave,
    // a little of the glint; and where it is thin, the water shows through it.
    float  lit   = 0.62 + 0.38 * saturate(dot(N, gSun.xyz) * 1.5) + 0.15 * F;
    float3 fcol  = c * lit + glint * 0.25;
    fcol = lerp(water + 0.12 * c, fcol, saturate(fa * 1.6));
    water = lerp(water, fcol, saturate(fa * 1.25));
    if (gFoam.w > 3.5 && gFoam.w < 4.5)
        return float4(bed, 1.0);   // debug 4: what lies under the water, bent and tinted
    // The water over the sand's foam and wet sand, mixed as colour times cover: mixed as plain colours, the foam's
    // white came in where our water fades in at the edge even with no foam there, a light line along the shore
    // (2026-10-02).
    float  outA = wv + a * (1.0 - wv);
    float3 outC = (water * wv + land * a * (1.0 - wv)) / max(outA, 1e-4);
    // The edge line ([water] edgeLine): a thin light line where the water meets the sand, in the first few
    // hundredths of a yard of depth, where our water fades in. It began as a fault in this mix (the foam's
    // colour mixed in where our water fades in) and was the best part of the shore, so it is kept on purpose,
    // at 1 exactly as it was (2026-10-02).
    outC = lerp(outC, c, saturate((1.0 - wv) * gFT.w));
    if (hideGame)
        return float4(lerp(lerp(under0, land, a), water, wv), 1.0);
    return float4(outC, outA);
}
)HLSL";

    // The wet sand (2026-10-02): one full-screen pass before the first water draw. Each pixel's ground point
    // comes from the depth under the water; the water level near it from a texture of the map's water around
    // you (the highest of the 3 x 3 cells round it, so a cell beside the water counts). Sand up to about 0.4
    // yards above that level is darkened, so the band follows a height line on the ground: the shoreline,
    // smooth. Its top rises and falls a little, as the water runs up the beach and back. The water draws over
    // it after, so only the sand above the water keeps it.
    const char* kWetVsHlsl = R"HLSL(
float4 main(float3 pos : POSITION) : POSITION
{
    return float4(pos.xy, 0.0, 1.0);
}
)HLSL";

    const char* kWetPsHlsl = R"HLSL(
sampler2D sUnder : register(s15);  // the depth under the water (INTZ)
sampler2D sLevel : register(s13);  // the water level of each map cell round you (R32F; -10000 where dry)
float4 gI0  : register(c200);      // rows of inverse(view x projection): clip -> camera-relative world
float4 gI1  : register(c201);
float4 gI2  : register(c202);
float4 gI3  : register(c203);
float4 gZ   : register(c204);      // the viewport's MinZ, 1 / (MaxZ - MinZ), strength, seconds
float4 gScr : register(c205);      // 1 / width, 1 / height, debug view
float4 gLv  : register(c206);      // the texture's first cell corner in the world (x, y), 1 / cell, 1 / cells
float4 gCam : register(c207);      // the camera in the world
float4 gFilm : register(c208);     // the swash: the water's colour (by day), its strength
float4 gFilm3 : register(c210);    // the shore foam's strength; the draw (0 the sand, 1 the foam)
float4 gFilm2 : register(c209);    // 1 / the foam texture's size in yards, 1 with the texture, the foam's strength,
                                   // the run-up in yards of height
float4 gFoamW : register(c211);    // as the water pass: 1 / foamWidth, 1 / foamReach, foamSpeed
sampler2D sFoamW : register(s12);  // the game's foam texture (WATERFOAMLOOP2.blp)
sampler2D sSceneW : register(s14); // the screen before this pass (when gFilm3.z is 1)
float4 main(float2 vpos : VPOS) : COLOR
{
    float2 uv  = (vpos + 0.5) * gScr.xy;
    float  raw = tex2Dlod(sUnder, float4(uv, 0, 0)).r;
    clip(gZ.x + 1.0 / gZ.y - 1e-5 - raw);                            // the sky, and far terrain in a slice of its own
    float  d   = (raw - gZ.x) * gZ.y;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gI0 + ndc.y * gI1 + d * gI2 + gI3;
    float3 P   = wp.xyz / wp.w;
    float3 A   = P + gCam.xyz;
    float2 cell = floor((A.xy - gLv.xy) * gLv.z);
    float  lv  = -10000.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        lv = max(lv, tex2Dlod(sLevel, float4((cell + float2(x, y) + 0.5) * gLv.w, 0, 0)).r);
    float  h    = A.z - lv;                                          // yards above the water
    float3 n    = cross(ddx(P), ddy(P));
    float  flat = abs(n.z) > 0.85 * length(n) ? 1.0 : 0.0;           // not a body, a wall or a post
    // Still: uneven along the shore, never moving (2026-10-02: it moved with the time, and wet sand does not).
    float  top  = 0.4 + 0.1 * sin(A.x * 0.13 + A.y * 0.11);
    // Above the water only: under it the water pass shows the bed, and darkened there it made a dark band along
    // the shore, darker than the water further out (2026-10-02).
    float  wet  = (1.0 - smoothstep(top * 0.5, top, h)) * smoothstep(-0.05, 0.02, h) * flat;
    wet *= lv > -1000.0 ? 1.0 : 0.0;
    if (gScr.z > 2.5 && gScr.z < 3.5)
        return float4(wet.xxx, 1.0);                                 // debug 3: the wet sand alone
    float wa = wet * gZ.z;

    // The swash (2026-10-02): a thin film of water runs up the beach and slides back, the sand under it
    // darker and in the water's colour, with a broken lip of the game's foam on its edge. By the sand's height
    // above the water, so its edge follows the shore smoothly; drawn here, over the whole screen, because the
    // water's own chunks end on the sand in steps of a cell. Each stretch of shore runs up in its own time.
    float  phase = sin(A.x * 0.21 + A.y * 0.17) * 1.7 + sin(A.x * 0.07 - A.y * 0.09) * 2.3;
    float  surge = 0.5 + 0.5 * sin(gZ.w * 0.55 + phase);
    surge = surge * surge;                                           // quick up the beach, slow back
    float  up    = gFilm2.w * surge;                                 // the edge's height above the water now
    float  film  = (1.0 - smoothstep(up - 0.015, up, h)) * smoothstep(-0.05, 0.0, h) * flat;
    film *= lv > -1000.0 ? 1.0 : 0.0;
    // The lip: the edge line, riding the film's edge. At rest it lies at the waterline, where the water pass's
    // own edge line used to be; that one is off while the swash is on, so there is one shoreline, not two.
    float  ft    = gFilm2.y > 0.5 ? tex2D(sFoamW, A.xy * gFilm2.x + float2(gZ.w * 0.01, 0.0)).a : 0.6;
    // Soft and broken by the foam texture, not a hard white stroke.
    float  lipE  = exp(-pow((h - up) / 0.009, 2.0)) * (0.2 + 0.8 * smoothstep(0.25, 0.7, ft)) * gFilm2.z * flat;
    lipE *= lv > -1000.0 ? 1.0 : 0.0;
    // The game's foam on the film: the water pass's shore foam, measured from the moving edge in the same way and
    // with the same texture at the same place and time, so one foam runs from the water up the sand and back.
    // Until 2026-10-03 this was a foam of its own just behind the edge, and the water's band stayed put at the
    // waterline: the foam looked fixed to the flat water while the edge moved.
    float2 gxW   = ddx(A.xy), gyW = ddy(A.xy);
    float  exW   = ddx(h), eyW = ddy(h);
    float  detW  = gxW.x * gyW.y - gxW.y * gyW.x;
    float2 gh    = abs(detW) > 1e-8 ? float2(exW * gyW.y - gxW.y * eyW, gxW.x * eyW - gyW.x * exW) / detW : 0.0;
    float  slopeW = clamp(length(gh), 0.02, 2.0);
    float  below = max(up - h, 0.0);                                 // yards of height under the moving edge
    float  reachW = below / slopeW;
    float  shoreW = saturate(1.0 - max(below * gFoamW.x * (1.0 + slopeW * 4.0), reachW * gFoamW.y));
    float  tW    = gZ.w * gFoamW.z;
    float  bandW = sin(reachW * 4.0 + tW * 1.6) * 0.5 + 0.5;
    float  fshW  = shoreW * (0.55 + 0.45 * bandW);
    float2 fpW   = A.xy * gFilm2.x;
    float  ft2   = gFilm2.y > 0.5 ? 0.6 * tex2D(sFoamW, fpW + float2(tW * 0.010, tW * 0.006)).a +
                                    0.4 * tex2D(sFoamW, fpW * 0.71 + float2(0.37 - tW * 0.007, 0.21 + tW * 0.011)).a
                                  : 0.5;
    float  fcut  = 1.0 - 0.7 * fshW;
    float  sfoam = smoothstep(fcut, fcut + 0.3, ft2) * saturate(fshW * 2.0) * gFilm3.x * film;
    lipE = max(lipE, sfoam);
    // Two draws (2026-10-02). The first multiplies the sand: wet, and under the film the water's hue, as the
    // sand seen through thin water is in the water pass. Laid on as a colour, the film was darker than the clear
    // water beside it, a dark band inside the foam line. The second lays the foam on.
    if (gFilm3.y < 0.5)
        return float4((1.0 - wa * (1.0 - film)).xxx, 1.0);           // the wet sand, multiplied; not under the film
    // The film: the sand's brightness in the water's hue, as the thin water gets in the water pass, so the
    // water's colour runs right up to the foam with no step. Multiplied, it could only darken orange sand to
    // olive (2026-10-02). Then the foam over it.
    float  fA = 0.0;
    float3 fC = 0.0;
    if (gFilm3.z > 0.5)
    {
        float3 hue = gFilm.rgb / max(dot(gFilm.rgb, float3(0.299, 0.587, 0.114)), 1e-3);
        // The dry sand's brightness, as the water pass sees the bed beside it: taken after the wet sand's
        // darkening, the film was darker than the water next to it.
        float  lum = dot(tex2Dlod(sSceneW, float4(uv, 0, 0)).rgb, float3(0.299, 0.587, 0.114));
        fA = saturate(film * gFilm.w * 1.2);
        // 1.4: measured on the Westfall beach, the film came out at 0.7 of the shallow water beside it, which
        // the water pass also lights with the sky and the waves (2026-10-02).
        fC = lum * hue * 1.4;
    }
    float  a2 = 1.0 - (1.0 - fA) * (1.0 - lipE);
    float3 c2 = a2 > 1e-4 ? (fC * fA * (1.0 - lipE) + float3(0.92, 0.95, 0.96) * lipE) / a2 : 0.0;
    return float4(c2, a2);
}
)HLSL";

    // The game's own foam texture (2026-10-02), read from the client's archives by the map loader.
    const char* kFoamTexName = "World\\Expansion02\\Doodads\\Generic\\WATERFALLS\\WATERFOAMLOOP2.blp";
    IDirect3DTexture9*      g_foamTex = nullptr;
    int                     g_foamTexState = 0;   // 0 not yet, 1 on the GPU, -1 none
    constexpr DWORD         kFoamSampler = 12;

    IDirect3DVertexShader9* g_wetVs = nullptr;
    IDirect3DPixelShader9*  g_wetPs = nullptr;
    IDirect3DTexture9*      g_level = nullptr;     // the water level of each map cell round you
    IDirect3DStateBlock9*   g_wetSb = nullptr;
    constexpr int           kLevelCells = 64;
    constexpr float         kCell = 1600.0f / 3.0f / 128.0f;   // a map cell: 4.17 yards (mapterrain.cpp)
    int                     g_levelX = 0x7FFFFFFF, g_levelY = 0x7FFFFFFF;   // the first cell, by index
    unsigned                g_levelAge = 0;
    bool                    g_wetFailed = false;
    constexpr DWORD         kLevelSampler = 13;

    IDirect3DVertexShader9* g_vs = nullptr;
    IDirect3DPixelShader9*  g_ps = nullptr;
    bool                    g_tried  = false;
    bool                    g_failed = false;

    IDirect3DTexture9* g_scene     = nullptr;   // the screen before the first water draw, this frame
    IDirect3DSurface9* g_sceneSurf = nullptr;
    UINT               g_sceneW = 0, g_sceneH = 0;
    D3DFORMAT          g_sceneFmt = D3DFMT_UNKNOWN;
    bool               g_sceneOk = false;        // copied this frame
    bool               g_sceneFailLogged = false;
    IDirect3DTexture9* g_under     = nullptr;   // the depth under the water, this frame
    IDirect3DSurface9* g_underSurf = nullptr;
    UINT               g_underW = 0, g_underH = 0;
    bool               g_copied = false;        // tried this frame
    bool               g_copyOk = false;
    bool               g_copyFailLogged = false;
    float              g_psc[83 * 4];           // this frame's pixel constants: c120 to c202

    // The ripples, in the world. A unit in the water starts one where it stands about once a second, and one
    // each time it has moved a yard and a half: walking leaves a trail. Until 2026-10-02 the rings were drawn
    // round where each unit stood that frame, and moved along with it.
    struct Ring
    {
        float  pos[3];
        double born;
        float  speed;   // yards a second the ring spreads at
        float  dir[2];  // the way its maker walked; 0 standing still
    };
    std::vector<Ring> g_rings;

    // The wakes (2026-10-02): the recent path of each unit moving through the water. A unit is followed from
    // frame to frame by the trail whose newest point is nearest its place (no ids from the object list).
    struct TrailPoint { float pos[3]; double t; };
    struct Trail
    {
        std::vector<TrailPoint> pts;   // newest first; pts[0] follows the unit every frame
        double                  seen = 0.0;
    };
    std::vector<Trail> g_trails;
    constexpr int   kTrails = 4, kTrailPts = 8;
    constexpr float kTrailStep = 0.6f;     // yards between the points kept
    constexpr double kTrailLife = 2.5;     // seconds a point is kept
    // Last frame's units, to tell how fast each moves: a unit is the one nearest its place last frame.
    float  g_lastUnits[256][3];
    int    g_lastUnitCount = 0;
    double g_lastUnitTime = 0.0;
    constexpr float kRingStill = 1.1f;   // yards a second a ring round a unit standing still spreads at

    // The game's own wake (2026-10-02): the V of foam that trails a unit wading or swimming. Measured walking
    // through the sea off Westfall: after the water the client draws a few batches of particles, fixed-function
    // (format 0x142: position, colour, one texture), additive (source alpha, one), no depth writes, their
    // vertices camera-relative with an identity world matrix. Four share one texture (14, 8, 98 and 92
    // triangles); the V is the two big ones. A batch is taken as the wake when its first vertex sits at a
    // river's or the sea's surface (the map files) within 6 yards of a unit; its texture is then learnt, and
    // every draw with it is one, anywhere. [water] gameWake says whether the client still draws them.
    std::set<void*> g_wakeTex;
    std::map<void*, unsigned> g_wakeNot;   // textures looked at and not taken, with the frame they were
    unsigned        g_wakeDraws = 0, g_wakeLast = 0;
    unsigned        g_frameNo = 0, g_unitsFrame = ~0u;
    int             g_unitCount = 0;
    float           g_units[256][3];
    unsigned           g_foamDraws = 0, g_foamLast = 0;

    // The probe.
    bool        g_probeOn = false;
    unsigned    g_pCount = 0, g_pFirst = 0, g_pLast = 0, g_pDetailed = 0, g_pOther = 0;
    std::map<UINT, unsigned>        g_pByVerts;
    std::map<void*, std::string>    g_pByTex;    // each texture on stage 0: the draws and the first one's place
    std::map<void*, unsigned>       g_pTexCount;
    std::set<void*>                 g_pShaders;  // shaders disassembled this probe
    std::string                     g_pOthers;   // blended fixed-function draws with a shader that are not water

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
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
            Log("water: %s failed to compile hr=0x%08X: %s", name, hr,
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no message)");
            SafeRelease(code);
        }
        SafeRelease(errs);
        return code;
    }

    bool EnsureShaders(IDirect3DDevice9* dev)
    {
        if (g_tried)
            return g_vs && g_ps;
        g_tried = true;
        if (OgBlob* code = Compile(kVsHlsl, "water_vs", "vs_3_0"))
        {
            if (FAILED(dev->lpVtbl->CreateVertexShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_vs)))
                g_vs = nullptr;
            code->lpVtbl->Release(code);
        }
        if (OgBlob* code = Compile(kPsHlsl, "water_ps", "ps_3_0"))
        {
            if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_ps)))
                g_ps = nullptr;
            code->lpVtbl->Release(code);
        }
        if (!g_vs || !g_ps)
        {
            g_failed = true;
            Log("water: the foam's shaders could not be made: no foam");
            return false;
        }
        Log("water: the foam's shaders compiled");
        return true;
    }

    // The screen as it is before the water, out of the render target bound now: what lies under the water,
    // for the surface. A multisampled target is resolved by the same call. A colour StretchRect may run inside
    // the scene; only a depth one may not.
    bool CopyScene(IDirect3DDevice9* dev)
    {
        if (g_cfg.water.surface <= 0.0f)
            return false;
        IDirect3DSurface9* rt = nullptr;
        if (FAILED(dev->lpVtbl->GetRenderTarget(dev, 0, &rt)) || !rt)
            return false;
        D3DSURFACE_DESC d = {};
        rt->lpVtbl->GetDesc(rt, &d);
        if (!g_scene || g_sceneW != d.Width || g_sceneH != d.Height || g_sceneFmt != d.Format)
        {
            SafeRelease(g_sceneSurf);
            SafeRelease(g_scene);
            HRESULT hr = dev->lpVtbl->CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format,
                                                    D3DPOOL_DEFAULT, &g_scene, nullptr);
            if (SUCCEEDED(hr))
                hr = g_scene->lpVtbl->GetSurfaceLevel(g_scene, 0, &g_sceneSurf);
            if (FAILED(hr))
            {
                if (!g_sceneFailLogged)
                    Log("water: could not create the %ux%u screen copy (format %u, hr=0x%08X): the game's water stays",
                        d.Width, d.Height, static_cast<unsigned>(d.Format), hr);
                g_sceneFailLogged = true;
                SafeRelease(g_sceneSurf);
                SafeRelease(g_scene);
                rt->lpVtbl->Release(rt);
                return false;
            }
            g_sceneW = d.Width;
            g_sceneH = d.Height;
            g_sceneFmt = d.Format;
            Log("water: screen copy %p made, %ux%u, format %u", g_scene, d.Width, d.Height, static_cast<unsigned>(d.Format));
        }
        const HRESULT hr = dev->lpVtbl->StretchRect(dev, rt, nullptr, g_sceneSurf, nullptr, D3DTEXF_NONE);
        rt->lpVtbl->Release(rt);
        if (FAILED(hr))
        {
            if (!g_sceneFailLogged)
                Log("water: copying the screen failed (hr=0x%08X): the game's water stays", hr);
            g_sceneFailLogged = true;
            return false;
        }
        return true;
    }

    // The depth under the water, out of the depth buffer bound now. A multisampled one is resolved by the
    // same call (depth.cpp does the same at the world's end).
    bool CopyUnder(IDirect3DDevice9* dev)
    {
        IDirect3DSurface9* cur = nullptr;
        if (FAILED(dev->lpVtbl->GetDepthStencilSurface(dev, &cur)) || !cur)
            return false;
        D3DSURFACE_DESC d = {};
        cur->lpVtbl->GetDesc(cur, &d);
        if (d.Format != kINTZ)
        {
            if (!g_copyFailLogged)
                Log("water: the depth buffer is not our readable one (format 0x%08X, [depth] enabled?): no foam",
                    static_cast<unsigned>(d.Format));
            g_copyFailLogged = true;
            cur->lpVtbl->Release(cur);
            return false;
        }
        if (!g_under || g_underW != d.Width || g_underH != d.Height)
        {
            SafeRelease(g_underSurf);
            SafeRelease(g_under);
            HRESULT hr = dev->lpVtbl->CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ,
                                                    D3DPOOL_DEFAULT, &g_under, nullptr);
            if (SUCCEEDED(hr))
                hr = g_under->lpVtbl->GetSurfaceLevel(g_under, 0, &g_underSurf);
            if (FAILED(hr))
            {
                Log("water: could not create the %ux%u depth copy (hr=0x%08X): no foam", d.Width, d.Height, hr);
                SafeRelease(g_underSurf);
                SafeRelease(g_under);
                g_failed = true;
                cur->lpVtbl->Release(cur);
                return false;
            }
            g_underW = d.Width;
            g_underH = d.Height;
            Log("water: depth copy %p made, %ux%u, from a %s depth buffer", g_under, d.Width, d.Height,
                d.MultiSampleType != D3DMULTISAMPLE_NONE ? "multisampled" : "plain");
        }
        // DXVK refuses a depth StretchRect inside a scene.
        auto* v = dev->lpVtbl;
        const bool inScene = SUCCEEDED(v->EndScene(dev));
        const HRESULT hr = v->StretchRect(dev, cur, nullptr, g_underSurf, nullptr, D3DTEXF_NONE);
        if (inScene)
            v->BeginScene(dev);
        cur->lpVtbl->Release(cur);
        if (FAILED(hr))
        {
            if (!g_copyFailLogged)
                Log("water: copying the depth failed (hr=0x%08X): no foam", hr);
            g_copyFailLogged = true;
            return false;
        }
        return true;
    }

    // A unit in the water, as far as the map files can say: false only where a tile is held and has neither a
    // river nor the sea there. The shader still checks each ripple against the surface it draws (WMO water,
    // a tile not loaded yet).
    bool MaybeInWater(const float p[3])
    {
        float wz = 0.0f, gz = 0.0f;
        if (MapWaterHeight(p[0], p[1], wz))
            return p[2] < wz - 0.05f && p[2] > wz - 3.0f;
        return !MapGroundHeight(p[0], p[1], gz);
    }

    // Starts the frame's new ripples, drops the old, and writes them as camera-relative feet and an age.
    void RingsUpdate(const float cam[3], float* out)
    {
        const double now = Now();
        for (size_t i = 0; i < g_rings.size();)
        {
            if (now - g_rings[i].born > kRingLife)
                g_rings.erase(g_rings.begin() + i);
            else
                ++i;
        }
        float units[256][3];
        const int n = ClientUnits(units, 256);
        const double dt = now - g_lastUnitTime;
        for (int u = 0; u < n; ++u)
        {
            const float* p = units[u];
            const float dx = p[0] - cam[0], dy = p[1] - cam[1];
            if (dx * dx + dy * dy > 60.0f * 60.0f || !MaybeInWater(p))
                continue;
            // How fast it moves: from its place last frame, the nearest within a yard.
            float speed = 0.0f, way[2] = { 0.0f, 0.0f };
            if (dt > 1e-3 && dt < 0.25)
            {
                float best = 1.0f;
                for (int j = 0; j < g_lastUnitCount; ++j)
                {
                    const float ex = g_lastUnits[j][0] - p[0], ey = g_lastUnits[j][1] - p[1];
                    const float d2 = ex * ex + ey * ey;
                    if (d2 < best)
                    {
                        best = d2;
                        speed = sqrtf(d2) / static_cast<float>(dt);
                        const float l = sqrtf(d2);
                        way[0] = l > 1e-4f ? -ex / l : 0.0f;
                        way[1] = l > 1e-4f ? -ey / l : 0.0f;
                    }
                }
            }
            // A new ring unless one began within a yard of here less than 0.9 s ago: a unit standing still
            // starts one about once a second, a unit walking one each yard. A walker's rings spread at a third
            // of its speed, so they meet in the V of a real wake, about 20 degrees either side.
            bool recent = false;
            for (const Ring& r : g_rings)
            {
                const float ex = r.pos[0] - p[0], ey = r.pos[1] - p[1];
                if (ex * ex + ey * ey < 1.0f && now - r.born < 0.9)
                {
                    recent = true;
                    break;
                }
            }
            if (!recent)
            {
                if (g_rings.size() >= static_cast<size_t>(kRings))
                    g_rings.erase(g_rings.begin());   // the oldest
                const float spread = speed > 1.0f ? (std::max)(kRingStill, (std::min)(speed / 3.0f, 3.0f)) : kRingStill;
                const bool walking = speed > 1.0f;
                g_rings.push_back({ { p[0], p[1], p[2] }, now, spread,
                                    { walking ? way[0] : 0.0f, walking ? way[1] : 0.0f } });
            }
        }
        // The wakes: each moving unit in the water extends the trail that ends nearest it, or starts one.
        for (int u = 0; u < n; ++u)
        {
            const float* p = units[u];
            const float dx = p[0] - cam[0], dy = p[1] - cam[1];
            if (dx * dx + dy * dy > 60.0f * 60.0f || !MaybeInWater(p))
                continue;
            Trail* best = nullptr;
            float bestD = 1.5f * 1.5f;
            for (Trail& tr : g_trails)
            {
                const float ex = tr.pts[0].pos[0] - p[0], ey = tr.pts[0].pos[1] - p[1];
                const float d2 = ex * ex + ey * ey;
                if (d2 < bestD && tr.seen < now)
                {
                    bestD = d2;
                    best = &tr;
                }
            }
            if (!best)
            {
                Trail tr;
                tr.pts.push_back({ { p[0], p[1], p[2] }, now });
                tr.pts.push_back({ { p[0], p[1], p[2] }, now });
                tr.seen = now;
                g_trails.push_back(tr);
                continue;
            }
            best->seen = now;
            best->pts[0] = { { p[0], p[1], p[2] }, now };
            const float* q = best->pts[1].pos;
            const float sx = p[0] - q[0], sy = p[1] - q[1];
            if (sx * sx + sy * sy > kTrailStep * kTrailStep)
            {
                best->pts.insert(best->pts.begin() + 1, best->pts[0]);
                if (best->pts.size() > static_cast<size_t>(kTrailPts))
                    best->pts.pop_back();
            }
        }
        for (size_t i = 0; i < g_trails.size();)
        {
            Trail& tr = g_trails[i];
            while (tr.pts.size() > 2 && now - tr.pts.back().t > kTrailLife)
                tr.pts.pop_back();
            if (now - tr.seen > kTrailLife)
                g_trails.erase(g_trails.begin() + i);
            else
                ++i;
        }
        memcpy(g_lastUnits, units, sizeof(float) * 3 * n);
        g_lastUnitCount = n;
        g_lastUnitTime = now;
        for (int i = 0; i < kRings; ++i)
        {
            float* o = out + i * 4;
            if (i < static_cast<int>(g_rings.size()))
            {
                const Ring& r = g_rings[i];
                o[0] = r.pos[0] - cam[0]; o[1] = r.pos[1] - cam[1]; o[2] = r.pos[2] - cam[2];
                // The age, with the speed in tenths of a yard a second above it: speed x 10 x 8 + age (age < 8).
                o[3] = floorf(r.speed * 10.0f + 0.5f) * 8.0f + static_cast<float>(now - r.born);
                float* w = out + kRings * 4 + i * 4;
                w[0] = r.dir[0]; w[1] = r.dir[1]; w[2] = w[3] = 0.0f;
            }
            else
            {
                o[0] = o[1] = o[2] = 0.0f;
                o[3] = -1.0f;
                float* w = out + kRings * 4 + i * 4;
                w[0] = w[1] = w[2] = w[3] = 0.0f;
            }
        }
    }

    // The pixel constants that hold for the whole frame, from the first chunk's camera.
    void FrameConstants(const WaterChunk& c)
    {
        const WaterSettings& w = g_cfg.water;
        float* k = g_psc;
        memset(g_psc, 0, sizeof(g_psc));
        k[0] = c.proj->m[2][2];
        k[1] = c.proj->m[3][2];
        k[2] = 0.0f;
        k[3] = 1.0f;   // the viewport's depth range: set by the caller
        k[4] = c.view->m[0][2]; k[5] = c.view->m[1][2]; k[6] = c.view->m[2][2]; k[7] = c.view->m[3][2];
        k[8] = g_underW ? 1.0f / g_underW : 0.0f;
        k[9] = g_underH ? 1.0f / g_underH : 0.0f;
        k[10] = static_cast<float>(fmod(Now(), 3600.0));
        k[11] = w.foam;
        k[12] = 1.0f / w.foamWidth;
        k[13] = 1.0f / w.foamScale;
        k[14] = w.foamSpeed;
        k[15] = static_cast<float>(w.debug);
        // The night darkens the foam, as it darkens the water under it.
        float hour = 0.0f;
        const float night = ClientHour(hour) ? NightWeight(hour) : 0.0f;
        const float lum = 1.0f - 0.75f * night;
        k[16] = ((w.foamColor >> 16) & 0xFF) / 255.0f * lum;
        k[17] = ((w.foamColor >> 8) & 0xFF) / 255.0f * lum;
        k[18] = (w.foamColor & 0xFF) / 255.0f * lum;
        k[19] = w.fadeEnd;
        DWORD fc = 0;
        if (WorldFogColor(fc))
        {
            k[20] = ((fc >> 16) & 0xFF) / 255.0f;
            k[21] = ((fc >> 8) & 0xFF) / 255.0f;
            k[22] = (fc & 0xFF) / 255.0f;
        }
        k[23] = 1.0f / (0.4f * w.fadeEnd);
        float fs = 0.0f, fe = 0.0f;
        if (WorldFog(fs, fe) && fe > fs + 1.0f)
        {
            k[24] = fs;
            k[25] = 1.0f / (fe - fs);
            k[26] = 1.0f;
        }
        float cam[3] = {};
        ClientCamera(cam);
        k[28] = cam[0]; k[29] = cam[1]; k[30] = cam[2];
        k[32] = 1.0f / w.foamReach;
        k[33] = w.ripples;
        k[34] = w.wetSand;
        RingsUpdate(cam, k + 40);
        // The wakes (c170 to c201): the 4 trails with the most points that are nearest, 8 points each.
        {
            float* o = k + 200;
            for (int i = 0; i < kTrails * kTrailPts * 4; ++i)
                o[i] = 0.0f;
            for (int i = 0; i < kTrails * kTrailPts; ++i)
                o[i * 4 + 3] = -1.0f;
            int used = 0;
            const double now = Now();
            for (const Trail& tr : g_trails)
            {
                if (used >= kTrails)
                    break;
                // A trail of one place is a unit standing still: no wake.
                const float mx = tr.pts.front().pos[0] - tr.pts.back().pos[0];
                const float my = tr.pts.front().pos[1] - tr.pts.back().pos[1];
                if (tr.pts.size() < 3 || mx * mx + my * my < 0.5f)
                    continue;
                for (size_t j = 0; j < tr.pts.size() && j < static_cast<size_t>(kTrailPts); ++j)
                {
                    float* pt = o + (used * kTrailPts + static_cast<int>(j)) * 4;
                    pt[0] = tr.pts[j].pos[0] - cam[0];
                    pt[1] = tr.pts[j].pos[1] - cam[1];
                    pt[2] = tr.pts[j].pos[2] - cam[2];
                    pt[3] = static_cast<float>(now - tr.pts[j].t);
                }
                ++used;
            }
            k[328] = static_cast<float>(used);
            k[329] = w.wake;
            k[330] = w.swash > 0.0f ? 1.0f : 0.0f;
            k[331] = w.swashHeight;
            if (g_probeOn)
                Log("water: wakes: %d trails held, %d drawn", static_cast<int>(g_trails.size()), used);
        }
        k[35] = static_cast<float>((std::min)(static_cast<int>(g_rings.size()), kRings));

        // The surface (c162 to c167). By night the sky, the water and the sun's light dim.
        const float day = 1.0f - 0.8f * night;
        float sun[3] = { 0.0f, 0.0f, 1.0f };
        const bool haveSun = SunDirection(sun);
        k[168] = sun[0]; k[169] = sun[1]; k[170] = sun[2];
        k[171] = haveSun ? w.glint * (night > 0.5f ? 0.3f : 1.0f) : 0.0f;
        // The colour deep water turns (Water Colour): green, teal and blue, mixed by the slider. The light it
        // absorbs follows it, so shallow water leans the same way: green water keeps more of its green.
        static const float kPalette[3][3] = { { 0.13f, 0.30f, 0.16f }, { 0.06f, 0.33f, 0.32f }, { 0.05f, 0.22f, 0.45f } };
        const float hue = w.colour * 0.02f;   // 0..2
        const int   lo  = hue >= 1.0f ? 1 : 0;
        const float f   = hue - lo;
        float deep[3];
        for (int i = 0; i < 3; ++i)
            deep[i] = kPalette[lo][i] + (kPalette[lo + 1][i] - kPalette[lo][i]) * f;
        k[172] = deep[0] * day;
        k[173] = deep[1] * day;
        k[174] = deep[2] * day;
        k[175] = w.surface;
        // Red is absorbed fastest, then blue, then green: the water turns teal with depth.
        // The light the water absorbs a yard, by channel: the channels furthest from its colour fastest, so what
        // lies under shallow water leans to the water's colour. Until 2026-10-02 red always went first, and the
        // sand under the water turned olive green whatever the colour was set to.
        const float dmax = (std::max)((std::max)(deep[0], deep[1]), deep[2]) + 1e-4f;
        for (int i = 0; i < 3; ++i)
            k[176 + i] = (0.04f + 0.35f * (1.0f - deep[i] / dmax)) / w.clarity;
        k[179] = w.refraction;
        k[180] = ((w.skyColor >> 16) & 0xFF) / 255.0f * day;
        k[181] = ((w.skyColor >> 8) & 0xFF) / 255.0f * day;
        k[182] = (w.skyColor & 0xFF) / 255.0f * day;
        k[183] = w.waves;
        const float sunC[3] = { 1.0f, 0.92f, 0.78f }, moonC[3] = { 0.55f, 0.6f, 0.75f };
        for (int i = 0; i < 3; ++i)
            k[184 + i] = night > 0.5f ? moonC[i] : sunC[i];
        k[187] = w.whitecaps;
        k[188] = 1.0f / w.waveScale;
        k[191] = w.reflection;
        k[190] = 0.0f;   // the screen copy: set when it is made (WaterBeforeDraw)
        k[192] = w.waveHeight * 0.25f;
        k[196] = g_foamTex ? 1.0f : 0.0f;
        k[197] = 1.0f / w.shoreFoamSize;
        k[198] = w.shoreFoam;
        k[199] = w.swash > 0.0f ? 0.0f : w.edgeLine;   // with the swash on, its lip is the edge line
    }

    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_FOGENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
        D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_ALPHATESTENABLE, D3DRS_SEPARATEALPHABLENDENABLE,
    };
    // The body mask's stencil states, for a foam draw that skips the bodies (bodymask.cpp).
    const D3DRENDERSTATETYPE kStencil[] = {
        D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
    };
    constexpr int kStencilCount = sizeof(kStencil) / sizeof(kStencil[0]);
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);
    const DWORD kFoamState[kTouchedCount] = {
        FALSE, FALSE, FALSE, TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, FALSE,
    };

    // A texture's size and format, for the probe.
    void TexInfo(IDirect3DBaseTexture9* t, char* out, size_t cap)
    {
        if (!t)
        {
            snprintf(out, cap, "none");
            return;
        }
        if (t->lpVtbl->GetType(t) == D3DRTYPE_TEXTURE)
        {
            D3DSURFACE_DESC td = {};
            auto* t2 = reinterpret_cast<IDirect3DTexture9*>(t);
            if (SUCCEEDED(t2->lpVtbl->GetLevelDesc(t2, 0, &td)))
            {
                snprintf(out, cap, "%p %ux%u format %u, %lu levels", t, td.Width, td.Height,
                         static_cast<unsigned>(td.Format), t2->lpVtbl->GetLevelCount(t2));
                return;
            }
        }
        snprintf(out, cap, "%p type %d", t, static_cast<int>(t->lpVtbl->GetType(t)));
    }

    void ProbeDetail(IDirect3DDevice9* dev, const WaterChunk& c, unsigned index)
    {
        auto* d = dev->lpVtbl;
        IDirect3DPixelShader9* ps = nullptr;
        d->GetPixelShader(dev, &ps);
        const float* t = c.world->m[3];
        float cam[3] = {};
        ClientCamera(cam);
        const float abs[3] = { t[0] + cam[0], t[1] + cam[1], t[2] + cam[2] };
        float wz = 0.0f, gz = 0.0f;
        const bool wet = MapWaterHeight(abs[0], abs[1], wz);
        const bool ground = MapGroundHeight(abs[0], abs[1], gz);
        Log("water draw #%u (frame draw %u): prim %d, %u vertices, %u triangles, ps %p, world translation "
            "(%.2f %.2f %.2f) = (%.1f %.1f %.1f) in the world; the map there: %s %.2f, ground %s %.2f",
            g_pCount, index, static_cast<int>(c.prim), c.numVertices, c.primCount, ps, t[0], t[1], t[2], abs[0],
            abs[1], abs[2], wet ? "water at" : "no river or sea", wet ? wz : 0.0f, ground ? "at" : "unknown",
            ground ? gz : 0.0f);
        Log("    world[0..2] = (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f); view row 3 (%.3f %.3f %.3f)",
            c.world->m[0][0], c.world->m[0][1], c.world->m[0][2], c.world->m[1][0], c.world->m[1][1], c.world->m[1][2],
            c.world->m[2][0], c.world->m[2][1], c.world->m[2][2], c.view->m[3][0], c.view->m[3][1], c.view->m[3][2]);
        DWORD rs[12] = {};
        const D3DRENDERSTATETYPE names[12] = { D3DRS_ZENABLE, D3DRS_ZFUNC, D3DRS_ZWRITEENABLE, D3DRS_CULLMODE,
            D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_FOGENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF,
            D3DRS_LIGHTING, D3DRS_TEXTUREFACTOR, D3DRS_COLORWRITEENABLE };
        for (int i = 0; i < 12; ++i)
            d->GetRenderState(dev, names[i], &rs[i]);
        Log("    states: z %lu func %lu write %lu, cull %lu, blend %lu/%lu, fog %lu, alpha test %lu ref %lu, "
            "lighting %lu, texture factor 0x%08lX, colour writes 0x%lX",
            rs[0], rs[1], rs[2], rs[3], rs[4], rs[5], rs[6], rs[7], rs[8], rs[9], rs[10], rs[11]);
        for (DWORD st = 0; st < 4; ++st)
        {
            IDirect3DBaseTexture9* tex = nullptr;
            d->GetTexture(dev, st, &tex);
            DWORD cop = 0, ca1 = 0, ca2 = 0, aop = 0, aa1 = 0, aa2 = 0, tci = 0, ttf = 0;
            d->GetTextureStageState(dev, st, D3DTSS_COLOROP, &cop);
            d->GetTextureStageState(dev, st, D3DTSS_COLORARG1, &ca1);
            d->GetTextureStageState(dev, st, D3DTSS_COLORARG2, &ca2);
            d->GetTextureStageState(dev, st, D3DTSS_ALPHAOP, &aop);
            d->GetTextureStageState(dev, st, D3DTSS_ALPHAARG1, &aa1);
            d->GetTextureStageState(dev, st, D3DTSS_ALPHAARG2, &aa2);
            d->GetTextureStageState(dev, st, D3DTSS_TEXCOORDINDEX, &tci);
            d->GetTextureStageState(dev, st, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf);
            char info[96];
            TexInfo(tex, info, sizeof(info));
            Log("    stage %lu: texture %s; colour op %lu (%lu, %lu), alpha op %lu (%lu, %lu), coords 0x%lX, "
                "transform flags %lu", st, info, cop, ca1, ca2, aop, aa1, aa2, tci, ttf);
            if (ttf)
            {
                D3DMATRIX m = {};
                d->GetTransform(dev, static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + st), &m);
                Log("    stage %lu texture transform: (%.3f %.3f) (%.3f %.3f) (%.3f %.3f) (%.3f %.3f)", st,
                    m.m[0][0], m.m[0][1], m.m[1][0], m.m[1][1], m.m[2][0], m.m[2][1], m.m[3][0], m.m[3][1]);
            }
            if (tex) tex->lpVtbl->Release(tex);
        }
        float pc[8 * 4] = {};
        if (SUCCEEDED(d->GetPixelShaderConstantF(dev, 0, pc, 8)))
            for (int i = 0; i < 8; ++i)
                Log("    ps c%d = %.4f %.4f %.4f %.4f", i, pc[i * 4], pc[i * 4 + 1], pc[i * 4 + 2], pc[i * 4 + 3]);
        // The first vertices, as the format says: position, normal, two texture coordinates.
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0;
        if (SUCCEEDED(d->GetStreamSource(dev, 0, &vb, &off, &stride)) && vb && stride >= 40)
        {
            void* ptr = nullptr;
            const UINT first = static_cast<UINT>(c.baseVertex + static_cast<INT>(c.minIndex));
            if (SUCCEEDED(vb->lpVtbl->Lock(vb, off + first * stride, 3 * stride, &ptr, D3DLOCK_READONLY)) && ptr)
            {
                for (int i = 0; i < 3; ++i)
                {
                    float f[10];
                    memcpy(f, static_cast<const uint8_t*>(ptr) + i * stride, sizeof(f));
                    Log("    vertex %d: position (%.3f %.3f %.3f) normal (%.3f %.3f %.3f) uv0 (%.4f %.4f) uv1 (%.4f %.4f)",
                        i, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]);
                }
                vb->lpVtbl->Unlock(vb);
            }
        }
        if (vb) vb->lpVtbl->Release(vb);
        // The shader's code, once a probe.
        if (ps && g_pShaders.insert(ps).second)
        {
            UINT size = 0;
            static auto disasm = reinterpret_cast<PFN_D3DDisassemble>(CompilerProc("D3DDisassemble"));
            if (disasm && SUCCEEDED(ps->lpVtbl->GetFunction(ps, nullptr, &size)) && size && size < 65536)
            {
                std::vector<uint8_t> code(size);
                OgBlob* text = nullptr;
                if (SUCCEEDED(ps->lpVtbl->GetFunction(ps, code.data(), &size)) &&
                    SUCCEEDED(disasm(code.data(), size, 0, nullptr, &text)) && text)
                {
                    Log("=== water pixel shader %p (%u bytes) ===\n%s=== end %p ===", ps, size,
                        static_cast<const char*>(text->lpVtbl->GetBufferPointer(text)), ps);
                    text->lpVtbl->Release(text);
                }
            }
        }
        if (ps) ps->lpVtbl->Release(ps);
    }
}

bool WaterWanted()
{
    const WaterSettings& w = g_cfg.water;
    // Any part drawn: until 2026-10-02 only the foam counted, and foam 0 took the surface and the wet sand too.
    const bool any = w.foam > 0.0f || w.surface > 0.0f || w.wetSand > 0.0f || w.shoreFoam > 0.0f || w.debug;
    return g_cfg.master && w.enabled && any && g_cfg.depth.enabled && !g_failed;
}

namespace
{
    bool EnsureWet(IDirect3DDevice9* dev)
    {
        if (g_wetFailed)
            return false;
        if (!g_wetVs)
        {
            if (OgBlob* code = Compile(kWetVsHlsl, "wet_vs", "vs_3_0"))
            {
                dev->lpVtbl->CreateVertexShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_wetVs);
                code->lpVtbl->Release(code);
            }
            if (OgBlob* code = Compile(kWetPsHlsl, "wet_ps", "ps_3_0"))
            {
                dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_wetPs);
                code->lpVtbl->Release(code);
            }
            if (FAILED(dev->lpVtbl->CreateTexture(dev, kLevelCells, kLevelCells, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED,
                                                  &g_level, nullptr)))
                g_level = nullptr;
            if (FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &g_wetSb)))
                g_wetSb = nullptr;
            if (!g_wetVs || !g_wetPs || !g_level || !g_wetSb)
            {
                Log("water: the wet sand pass could not be made (shaders %s, level texture %s, state block %s)",
                    g_wetVs && g_wetPs ? "ok" : "failed", g_level ? "ok" : "failed", g_wetSb ? "ok" : "failed");
                g_wetFailed = true;
                return false;
            }
            Log("water: the wet sand pass is made");
        }
        return true;
    }

    // The water level of the 64 x 64 map cells round you: filled again when you cross a cell, and once a
    // second, since tiles finish loading after you arrive.
    void UpdateLevels(const float pl[3])
    {
        const int cx = static_cast<int>(floorf(pl[0] / kCell)) - kLevelCells / 2;
        const int cy = static_cast<int>(floorf(pl[1] / kCell)) - kLevelCells / 2;
        if (cx == g_levelX && cy == g_levelY && ++g_levelAge < 60)
            return;
        g_levelX = cx;
        g_levelY = cy;
        g_levelAge = 0;
        D3DLOCKED_RECT r = {};
        if (FAILED(g_level->lpVtbl->LockRect(g_level, 0, &r, nullptr, 0)))
            return;
        for (int y = 0; y < kLevelCells; ++y)
        {
            float* row = reinterpret_cast<float*>(static_cast<uint8_t*>(r.pBits) + y * r.Pitch);
            for (int x = 0; x < kLevelCells; ++x)
            {
                float z = 0.0f;
                row[x] = MapWaterHeight((cx + x + 0.5f) * kCell, (cy + y + 0.5f) * kCell, z) ? z : -10000.0f;
            }
        }
        g_level->lpVtbl->UnlockRect(g_level, 0);
    }

    // The wet sand, once a frame, before the first water draw: every state goes back as it was (a state block),
    // since the client's own draw of the chunk follows at once.
    void DrawWetSand(IDirect3DDevice9* dev, const WaterChunk& c)
    {
        const WaterSettings& w = g_cfg.water;
        float pl[3];
        if ((w.wetSand <= 0.0f && w.swash <= 0.0f && w.debug != 3) || !ClientPlayer(pl) || !EnsureWet(dev))
            return;
        UpdateLevels(pl);
        D3DMATRIX vp, inv;
        Mul(*c.view, *c.proj, vp);
        if (!Invert(vp, inv))
            return;
        float k[12 * 4] = {};
        memcpy(k, &inv, 64);
        k[16] = g_psc[2]; k[17] = g_psc[3]; k[18] = w.wetSand; k[19] = g_psc[10];
        k[20] = g_psc[8]; k[21] = g_psc[9]; k[22] = static_cast<float>(w.debug);
        k[24] = g_levelX * kCell; k[25] = g_levelY * kCell; k[26] = 1.0f / kCell; k[27] = 1.0f / kLevelCells;
        k[28] = g_psc[28]; k[29] = g_psc[29]; k[30] = g_psc[30];
        // The swash (c208, c209): the water's colour, the film's strength; the foam texture and the run-up.
        k[32] = g_psc[172]; k[33] = g_psc[173]; k[34] = g_psc[174]; k[35] = w.swash;
        k[36] = 1.0f / w.shoreFoamSize; k[37] = g_foamTex ? 1.0f : 0.0f; k[39] = w.swashHeight;
        k[38] = w.swash > 0.0f ? w.edgeLine * 0.5f : 0.0f;    // the lip is the edge line
        k[40] = w.shoreFoam * w.foam;
        k[41] = 0.0f;   // the draw: 0 multiplies the sand, 1 lays the foam on
        k[44] = 1.0f / w.foamWidth; k[45] = 1.0f / w.foamReach; k[46] = w.foamSpeed;

        auto* d = dev->lpVtbl;
        IDirect3DVertexShader9* oldVs = nullptr;
        d->GetVertexShader(dev, &oldVs);
        g_wetSb->lpVtbl->Capture(g_wetSb);
        d->SetVertexShader(dev, g_wetVs);
        d->SetPixelShader(dev, g_wetPs);
        d->SetPixelShaderConstantF(dev, 200, k, 12);
        if (g_foamTex)
        {
            d->SetTexture(dev, kFoamSampler, reinterpret_cast<IDirect3DBaseTexture9*>(g_foamTex));
            d->SetSamplerState(dev, kFoamSampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(dev, kFoamSampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(dev, kFoamSampler, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(dev, kFoamSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            d->SetSamplerState(dev, kFoamSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        }
        if (g_psc[190] > 0.5f)
        {
            d->SetTexture(dev, kSceneSampler, reinterpret_cast<IDirect3DBaseTexture9*>(g_scene));
            d->SetSamplerState(dev, kSceneSampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            d->SetSamplerState(dev, kSceneSampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            d->SetSamplerState(dev, kSceneSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            d->SetSamplerState(dev, kSceneSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            d->SetSamplerState(dev, kSceneSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        d->SetFVF(dev, D3DFVF_XYZ);
        d->SetTexture(dev, kUnderSampler, reinterpret_cast<IDirect3DBaseTexture9*>(g_under));
        d->SetTexture(dev, kLevelSampler, reinterpret_cast<IDirect3DBaseTexture9*>(g_level));
        for (DWORD sm : { kUnderSampler, kLevelSampler })
        {
            d->SetSamplerState(dev, sm, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            d->SetSamplerState(dev, sm, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            d->SetSamplerState(dev, sm, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            d->SetSamplerState(dev, sm, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            d->SetSamplerState(dev, sm, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        d->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        d->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
        d->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d->SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        d->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0x7);
        DWORD bit = 0;
        if (BodyMarkLive(bit))
        {
            d->SetRenderState(dev, D3DRS_STENCILENABLE, TRUE);
            d->SetRenderState(dev, D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);
            d->SetRenderState(dev, D3DRS_STENCILREF, bit);
            d->SetRenderState(dev, D3DRS_STENCILMASK, bit);
            d->SetRenderState(dev, D3DRS_STENCILWRITEMASK, 0);
        }
        else
            d->SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
        const float quad[4][3] = { { -1.0f, -1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
        // First the sand, multiplied (scene x out); then the foam, blended over it.
        d->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ZERO);
        d->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
        d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
        const float second[4] = { k[40], 1.0f, g_psc[190], 0.0f };
        d->SetPixelShaderConstantF(dev, 210, second, 1);
        d->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
        g_wetSb->lpVtbl->Apply(g_wetSb);
        // The state block puts the client's vertex shader back without going through our SetVertexShader hook,
        // and comfyfog.cpp's mirror of it (g_vshader) kept ours: every later water chunk of the frame then looked
        // like a model draw, was not taken as water, and the game drew its own water there with none of ours
        // (2026-10-02, found with Debug View 21). Set again through the hook, so the mirror follows.
        d->SetVertexShader(dev, oldVs);
        if (oldVs) oldVs->lpVtbl->Release(oldVs);
    }
}

namespace
{
    void EnsureFoamTexture(IDirect3DDevice9* dev)
    {
        if (g_foamTexState != 0)
            return;
        BlpData b;
        const int r = MapRequestTexture(kFoamTexName, b);
        if (r == 0)
            return;
        if (r < 0)
        {
            g_foamTexState = -1;
            Log("water: the foam texture %s could not be read: the shore has the line alone", kFoamTexName);
            return;
        }
        static const D3DFORMAT kFormats[4] = { D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5, D3DFMT_A8R8G8B8 };
        if (FAILED(dev->lpVtbl->CreateTexture(dev, b.width, b.height, static_cast<UINT>(b.levels.size()), 0,
                                              kFormats[b.format], D3DPOOL_MANAGED, &g_foamTex, nullptr)))
        {
            g_foamTexState = -1;
            return;
        }
        for (UINT i = 0; i < b.levels.size(); ++i)
        {
            const UINT w = (std::max)(1u, b.width >> i), h = (std::max)(1u, b.height >> i);
            const UINT rows = b.format == 3 ? h : (std::max)(1u, (h + 3) / 4);
            const UINT rowBytes = b.format == 3 ? w * 4 : (std::max)(1u, (w + 3) / 4) * (b.format == 0 ? 8 : 16);
            D3DLOCKED_RECT lr;
            if (FAILED(g_foamTex->lpVtbl->LockRect(g_foamTex, i, &lr, nullptr, 0)))
                break;
            for (UINT row = 0; row < rows && (row + 1) * rowBytes <= b.levels[i].size(); ++row)
                memcpy(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch, &b.levels[i][row * rowBytes], rowBytes);
            g_foamTex->lpVtbl->UnlockRect(g_foamTex, i);
        }
        g_foamTexState = 1;
        Log("water: the foam texture is on the GPU, %ux%u, format %d, %u levels", b.width, b.height, b.format,
            static_cast<unsigned>(b.levels.size()));
    }
}

void WaterBeforeDraw(IDirect3DDevice9* dev, const WaterChunk& c)
{
    if (g_copied || !WaterWanted())
        return;
    g_copied = true;
    g_copyOk = EnsureShaders(dev) && CopyUnder(dev);
    if (!g_copyOk)
        return;
    EnsureFoamTexture(dev);
    FrameConstants(c);
    g_psc[190] = g_sceneOk ? 1.0f : 0.0f;
    D3DVIEWPORT9 vp = {};
    if (SUCCEEDED(dev->lpVtbl->GetViewport(dev, &vp)) && vp.MaxZ > vp.MinZ)
    {
        g_psc[2] = vp.MinZ;
        g_psc[3] = 1.0f / (vp.MaxZ - vp.MinZ);
    }
    if (g_probeOn)
    {
        float wmin = 0.0f, wmax = 1.0f;
        ShadowWorldDepthRange(wmin, wmax);
        Log("water: the water draws in depth %.4f..%.4f (past it, far terrain), projection m22 %.5f m32 %.4f; the world's slice %.4f..%.4f",
            vp.MinZ, vp.MaxZ, c.proj->m[2][2], c.proj->m[3][2], wmin, wmax);
    }
    // The wet sand first and the screen copy after it, so the sand seen through thin water is wet too: copied
    // before, it showed dry and bright under the water, a light line along the shore (2026-10-02).
    // The swash film takes the sand's own brightness in the water's hue, as thin water does in the water pass, so
    // the wet sand pass reads the screen too: one copy before it, and one after for the water pass.
    const bool before = g_cfg.water.swash > 0.0f && CopyScene(dev);
    g_psc[190] = before ? 1.0f : 0.0f;
    DrawWetSand(dev, c);
    g_sceneOk = CopyScene(dev);
    g_psc[190] = g_sceneOk ? 1.0f : 0.0f;
}

void WaterAfterDraw(IDirect3DDevice9* dev, const WaterChunk& c, WaterDrawFn draw)
{
    if (!g_copyOk || !WaterWanted() || g_cfg.water.debug == 6)
        return;

    auto* d = dev->lpVtbl;
    D3DMATRIX wv, wvp;
    Mul(*c.world, *c.view, wv);
    Mul(wv, *c.proj, wvp);
    float vc[11 * 4] = {};
    memcpy(vc, &wvp, 64);
    memcpy(vc + 16, c.world, 64);
    // The swell: a yard up in clip space, and its height. Only with our surface: the game's flat water is
    // drawn under it, and the foam alone must lie on that.
    D3DMATRIX vp;
    Mul(*c.view, *c.proj, vp);
    for (int i = 0; i < 4; ++i)
        vc[32 + i] = vp.m[2][i];
    vc[36] = g_psc[28];   // the camera, from the frame's constants
    vc[37] = g_psc[29];
    vc[38] = g_psc[10];   // seconds
    vc[39] = g_sceneOk && g_cfg.water.surface > 0.0f ? g_cfg.water.waveHeight : 0.0f;
    vc[40] = 1.0f / g_cfg.water.waveScale;

    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DPixelShader9*  oldPs = nullptr;
    d->GetVertexShader(dev, &oldVs);
    d->GetPixelShader(dev, &oldPs);
    DWORD old[kTouchedCount];
    for (int i = 0; i < kTouchedCount; ++i)
    {
        d->GetRenderState(dev, kTouched[i], &old[i]);
        if (old[i] != kFoamState[i])
            d->SetRenderState(dev, kTouched[i], kFoamState[i]);
    }
    // The client sets vertex registers up to c255 (the probe, 2026-10-02): its own c240 to c247 go back after.
    float oldVc[11 * 4];
    const bool haveVc = SUCCEEDED(d->GetVertexShaderConstantF(dev, kVsReg, oldVc, 11));
    // Not on a body: the body mask marks every unit's pixels in the stencil as the world is drawn, and the foam
    // passes only where the mark is clear. At a body's edge the foam took the body for flat ground beside the
    // water, and its hips went white (2026-10-02).
    DWORD bit = 0, oldSt[kStencilCount] = {};
    const bool skipBodies = BodyMarkLive(bit);
    if (skipBodies)
    {
        for (int i = 0; i < kStencilCount; ++i)
            d->GetRenderState(dev, kStencil[i], &oldSt[i]);
        d->SetRenderState(dev, D3DRS_STENCILENABLE, TRUE);
        d->SetRenderState(dev, D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);
        d->SetRenderState(dev, D3DRS_STENCILREF, bit);
        d->SetRenderState(dev, D3DRS_STENCILMASK, bit);
        d->SetRenderState(dev, D3DRS_STENCILWRITEMASK, 0);
    }
    d->SetVertexShader(dev, g_vs);
    d->SetPixelShader(dev, g_ps);
    d->SetVertexShaderConstantF(dev, kVsReg, vc, 11);
    d->SetPixelShaderConstantF(dev, kPsReg, g_psc, 83);
    d->SetTexture(dev, kUnderSampler, reinterpret_cast<IDirect3DBaseTexture9*>(g_under));
    d->SetSamplerState(dev, kUnderSampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, kUnderSampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, kUnderSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(dev, kUnderSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, kUnderSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    if (g_foamTex)
    {
        d->SetTexture(dev, kFoamSampler, reinterpret_cast<IDirect3DBaseTexture9*>(g_foamTex));
        d->SetSamplerState(dev, kFoamSampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, kFoamSampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, kFoamSampler, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, kFoamSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        d->SetSamplerState(dev, kFoamSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        d->SetSamplerState(dev, kFoamSampler, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    if (g_sceneOk)
    {
        d->SetTexture(dev, kSceneSampler, reinterpret_cast<IDirect3DBaseTexture9*>(g_scene));
        d->SetSamplerState(dev, kSceneSampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, kSceneSampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, kSceneSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(dev, kSceneSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, kSceneSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, kSceneSampler, D3DSAMP_SRGBTEXTURE, FALSE);
    }

    // With our surface, two draws (2026-10-02). The swell lifts a chunk over the next one on screen, and drawn
    // without the depth test whichever chunk came later won: bands along every chunk's edge, the game's flat
    // water painted over the raised edge of the one before. So the water part is depth tested and writes its
    // depth: a raised wave hides what lies behind it, the game's next chunk included, and the sun shadows and
    // the fog see the waves. The sand part (wet sand, the lip of foam above the waterline) lies in front of
    // the water's plane and is drawn without the test, as before. Without our surface, one draw does both.
    const bool surface = g_sceneOk && g_cfg.water.surface > 0.0f;
    // The fourth is the sky reflection's strength: until 2026-10-02 this wrote 0 over it, and Sky Reflection
    // did nothing.
    float mode[4] = { g_psc[188], surface ? 1.0f : 0.0f, g_psc[190], g_psc[191] };
    d->SetPixelShaderConstantF(dev, kPsReg + 47, mode, 1);
    draw(dev, c.prim, c.baseVertex, c.minIndex, c.numVertices, c.startIndex, c.primCount);
    if (surface)
    {
        DWORD oz[4];
        const D3DRENDERSTATETYPE zs[4] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_DEPTHBIAS };
        for (int i = 0; i < 4; ++i)
            d->GetRenderState(dev, zs[i], &oz[i]);
        // The flat parts lie where the game's water wrote its depth ([depth] waterDepth): drawn a little nearer,
        // so they win against it.
        const float bias = -0.00002f;
        DWORD biasBits;
        memcpy(&biasBits, &bias, 4);
        d->SetRenderState(dev, D3DRS_ZENABLE, TRUE);
        d->SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
        d->SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        d->SetRenderState(dev, D3DRS_DEPTHBIAS, biasBits);
        mode[1] = 2.0f;
        d->SetPixelShaderConstantF(dev, kPsReg + 47, mode, 1);
        draw(dev, c.prim, c.baseVertex, c.minIndex, c.numVertices, c.startIndex, c.primCount);
        // Once more on the bodies (the stencil's mark, bodymask.cpp), the water over a character's legs: skipped,
        // the game's own pale water showed there, and the legs under the water looked like a ghost (2026-10-02).
        // The depth test keeps it off what stands above the surface.
        if (skipBodies)
        {
            float body[4] = { g_psc[192], 1.0f, g_cfg.water.cover, 0.0f };
            d->SetPixelShaderConstantF(dev, kPsReg + 48, body, 1);
            d->SetRenderState(dev, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
            draw(dev, c.prim, c.baseVertex, c.minIndex, c.numVertices, c.startIndex, c.primCount);
            d->SetRenderState(dev, D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);
            body[1] = 0.0f;
            d->SetPixelShaderConstantF(dev, kPsReg + 48, body, 1);
        }
        for (int i = 0; i < 4; ++i)
            d->SetRenderState(dev, zs[i], oz[i]);
    }
    ++g_foamDraws;

    d->SetTexture(dev, kUnderSampler, nullptr);
    if (g_sceneOk)
        d->SetTexture(dev, kSceneSampler, nullptr);
    if (g_foamTex)
        d->SetTexture(dev, kFoamSampler, nullptr);
    if (skipBodies)
        for (int i = 0; i < kStencilCount; ++i)
            d->SetRenderState(dev, kStencil[i], oldSt[i]);
    if (haveVc)
        d->SetVertexShaderConstantF(dev, kVsReg, oldVc, 11);
    d->SetVertexShader(dev, oldVs);
    d->SetPixelShader(dev, oldPs);
    for (int i = 0; i < kTouchedCount; ++i)
        if (old[i] != kFoamState[i])
            d->SetRenderState(dev, kTouched[i], old[i]);
    if (oldVs) oldVs->lpVtbl->Release(oldVs);
    if (oldPs) oldPs->lpVtbl->Release(oldPs);
}

void WaterProbeDraw(IDirect3DDevice9* dev, const WaterChunk& c, unsigned index, bool water)
{
    if (!g_probeOn)
        return;
    IDirect3DBaseTexture9* tex = nullptr;
    dev->lpVtbl->GetTexture(dev, 0, &tex);
    if (!water)
    {
        // A blended fixed-function draw through a pixel shader that is not the water's: lava or slime, if
        // the client draws them with a shader of their own.
        IDirect3DPixelShader9* ps = nullptr;
        dev->lpVtbl->GetPixelShader(dev, &ps);
        DWORD fvf = 0;
        dev->lpVtbl->GetFVF(dev, &fvf);
        if (++g_pOther <= 12)
        {
            char t[160];
            snprintf(t, sizeof(t), " [draw %u: ps %p fvf 0x%lX %uv tex %p at (%.0f %.0f %.0f)]", index, ps, fvf,
                     c.numVertices, tex, c.world->m[3][0], c.world->m[3][1], c.world->m[3][2]);
            g_pOthers += t;
        }
        if (ps) ps->lpVtbl->Release(ps);
        if (tex) tex->lpVtbl->Release(tex);
        return;
    }
    if (!g_pCount)
        g_pFirst = index;
    g_pLast = index;
    ++g_pCount;
    ++g_pByVerts[c.numVertices];
    if (++g_pTexCount[tex] == 1)
    {
        float cam[3] = {};
        ClientCamera(cam);
        const float x = c.world->m[3][0] + cam[0], y = c.world->m[3][1] + cam[1];
        float wz = 0.0f;
        const bool wet = MapWaterHeight(x, y, wz);
        char info[96], t[200];
        TexInfo(tex, info, sizeof(info));
        snprintf(t, sizeof(t), "first at draw %u, (%.0f %.0f), %s", index, x, y,
                 wet ? "a river or the sea in the map" : "no river or sea in the map (a building's water, lava, slime?)");
        g_pByTex[tex] = std::string(info) + ", " + t;
    }
    if (tex) tex->lpVtbl->Release(tex);
    if (g_pDetailed < 3)
    {
        ++g_pDetailed;
        ProbeDetail(dev, c, index);
    }
}

bool WaterGameWake(IDirect3DDevice9* dev, const WaterChunk& c)
{
    auto* d = dev->lpVtbl;
    DWORD blend = 0, zwrite = 1, src = 0, dst = 0, fvf = 0;
    d->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
    if (!blend)
        return false;
    d->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zwrite);
    d->GetRenderState(dev, D3DRS_SRCBLEND, &src);
    d->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
    if (zwrite || src != D3DBLEND_SRCALPHA || dst != D3DBLEND_ONE)
        return false;
    IDirect3DBaseTexture9* tex = nullptr;
    d->GetTexture(dev, 0, &tex);
    if (!tex)
        return false;
    tex->lpVtbl->Release(tex);
    if (g_wakeTex.count(tex))
    {
        ++g_wakeDraws;
        return true;
    }
    // A texture looked at and not taken is looked at again only after 2 seconds of frames: a fire's
    // particles would otherwise cost a buffer read every frame.
    auto seen = g_wakeNot.find(tex);
    if (seen != g_wakeNot.end() && g_frameNo - seen->second < 120 && !g_probeOn)
        return false;
    if (g_wakeTex.size() >= 8 || g_wakeNot.size() > 512)
        return false;
    g_wakeNot[tex] = g_frameNo;
    // The first vertex says where the batch is. Its space is the client's: the particles' world matrix is the
    // identity and their view has no translation, and still their vertices are not camera-relative (the wake's
    // came out 30 yards up). So it goes to the screen through the draw's own world, view and projection, and
    // back through the world camera's.
    static unsigned told = 0;   // the first 30 batches looked at are logged, with why they were not taken
    d->GetFVF(dev, &fvf);
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ)
        return false;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    float v[3] = {};
    bool read = false;
    if (SUCCEEDED(d->GetStreamSource(dev, 0, &vb, &off, &stride)) && vb && stride >= 12)
    {
        void* ptr = nullptr;
        const UINT first = static_cast<UINT>(c.baseVertex + static_cast<INT>(c.minIndex));
        if (SUCCEEDED(vb->lpVtbl->Lock(vb, off + first * stride, 12, &ptr, D3DLOCK_READONLY)) && ptr)
        {
            memcpy(v, ptr, 12);
            vb->lpVtbl->Unlock(vb);
            read = true;
        }
    }
    if (vb) vb->lpVtbl->Release(vb);
    float cam[3];
    D3DMATRIX wv, wvp, camV, camP, camVP, back;
    Mul(*c.world, *c.view, wv);
    Mul(wv, *c.proj, wvp);
    const bool haveCam = (ShadowWorldCamera(camV, camP) || SunCamera(camV, camP));
    if (haveCam)
        Mul(camV, camP, camVP);
    if (!read || !ClientCamera(cam) || !haveCam || !Invert(camVP, back))
    {
        if (read ? g_probeOn : ++told <= 30)
            Log("water: additive batch, texture %p: %s", tex, read ? "no camera" : "its vertices could not be read");
        return false;
    }
    float h[4], r[4];
    for (int k = 0; k < 4; ++k)
        h[k] = v[0] * wvp.m[0][k] + v[1] * wvp.m[1][k] + v[2] * wvp.m[2][k] + wvp.m[3][k];
    for (int k = 0; k < 4; ++k)
        r[k] = h[0] * back.m[0][k] + h[1] * back.m[1][k] + h[2] * back.m[2][k] + h[3] * back.m[3][k];
    if (fabsf(r[3]) < 1e-6f)
        return false;
    const float p[3] = { r[0] / r[3] + cam[0], r[1] / r[3] + cam[1], r[2] / r[3] + cam[2] };
    float wz = 0.0f;
    const bool wet = MapWaterHeight(p[0], p[1], wz);
    float pl[3] = {};
    ClientPlayer(pl);
    if (g_probeOn || ++told <= 30)
        Log("water: additive particles, texture %p, %u triangles, first vertex at (%.1f %.1f %.1f), water there "
            "%s %.2f; you at (%.1f %.1f %.1f); raw vertex (%.2f %.2f %.2f), camera (%.1f %.1f %.1f), view row 3 "
            "(%.2f %.2f %.2f)", tex, c.primCount, p[0], p[1], p[2], wet ? "at" : "none", wz, pl[0], pl[1], pl[2],
            v[0], v[1], v[2], cam[0], cam[1], cam[2], c.view->m[3][0], c.view->m[3][1], c.view->m[3][2]);
    if (!wet || fabsf(p[2] - wz) > 0.6f)
        return false;
    if (g_unitsFrame != g_frameNo)
    {
        g_unitsFrame = g_frameNo;
        g_unitCount = ClientUnits(g_units, 256);
    }
    for (int i = 0; i < g_unitCount; ++i)
    {
        const float dx = g_units[i][0] - p[0], dy = g_units[i][1] - p[1];
        if (dx * dx + dy * dy < 6.0f * 6.0f)
        {
            g_wakeTex.insert(tex);
            g_wakeNot.erase(tex);
            char info[96];
            TexInfo(tex, info, sizeof(info));
            Log("water: the game's wake is drawn with texture %s: particles at (%.1f %.1f %.1f) by a unit, on the "
                "water at %.1f", info, p[0], p[1], p[2], wz);
            ++g_wakeDraws;
            return true;
        }
    }
    return false;
}

void WaterFrameEnd()
{
    if (g_probeOn)
    {
        g_probeOn = false;
        Log("water: %u draws, from frame draw %u to %u (%u other draws between them); the foam drew %u chunks, "
            "the depth copy %s", g_pCount, g_pFirst, g_pLast,
            g_pCount ? g_pLast - g_pFirst + 1 - g_pCount : 0, g_foamDraws,
            !WaterWanted() ? "not wanted (the foam is off)" : g_copyOk ? "worked" : "failed");
        std::string verts;
        for (const auto& v : g_pByVerts)
            verts += " " + std::to_string(v.second) + " x " + std::to_string(v.first);
        Log("water: vertices a draw:%s; the client uses vertex shader registers below c%u, the foam c240 to c250",
            verts.empty() ? " none" : verts.c_str(), g_maxConstReg);
        for (const auto& t : g_pByTex)
            Log("water: texture %s; %u draws", t.second.c_str(), g_pTexCount[t.first]);
        if (g_pOther)
            Log("water: %u blended fixed-function draws through another pixel shader:%s", g_pOther, g_pOthers.c_str());
        // Where you stand, and how deep the water is round you: to pick a spot for a test.
        float pl[3];
        if (ClientPlayer(pl))
        {
            float wz = 0.0f, gz = 0.0f;
            const bool wet = MapWaterHeight(pl[0], pl[1], wz);
            std::string round;
            for (int i = 0; i < 8; ++i)
            {
                const float a = i * 0.7853982f, x = pl[0] + 6.0f * cosf(a), y = pl[1] + 6.0f * sinf(a);
                float g = 0.0f, w = 0.0f;
                char t[48];
                if (MapGroundHeight(x, y, g))
                    snprintf(t, sizeof(t), " %d deg %s%.1f", i * 45, MapWaterHeight(x, y, w) ? "depth " : "dry ",
                             MapWaterHeight(x, y, w) ? w - g : g);
                else
                    snprintf(t, sizeof(t), " %d deg ?", i * 45);
                round += t;
            }
            Log("water: you stand at (%.1f %.1f %.1f); water %s %.2f, ground %.2f; 6 yards out (0 deg = +x):%s",
                pl[0], pl[1], pl[2], wet ? "at" : "none", wz, MapGroundHeight(pl[0], pl[1], gz) ? gz : 0.0f,
                round.c_str());
        }
        Log("water: the game's wake: %u draws this frame, %u textures learnt, %s ([water] gameWake)", g_wakeDraws,
            static_cast<unsigned>(g_wakeTex.size()), g_cfg.water.gameWake ? "drawn" : "hidden");
    }
    g_wakeLast = g_wakeDraws;
    g_wakeDraws = 0;
    ++g_frameNo;
    g_foamLast = g_foamDraws;
    g_foamDraws = 0;
    g_copied = false;
    g_copyOk = false;
    g_sceneOk = false;
}

void WaterReset()
{
    g_trails.clear();
    SafeRelease(g_foamTex);
    g_foamTexState = 0;
    SafeRelease(g_wetSb);
    SafeRelease(g_wetVs);
    SafeRelease(g_wetPs);
    SafeRelease(g_level);
    g_levelX = g_levelY = 0x7FFFFFFF;
    g_wetFailed = false;
    SafeRelease(g_sceneSurf);
    SafeRelease(g_scene);
    g_sceneW = g_sceneH = 0;
    g_sceneOk = false;
    g_sceneFailLogged = false;
    g_wakeTex.clear();
    g_wakeNot.clear();
    SafeRelease(g_underSurf);
    SafeRelease(g_under);
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    g_underW = g_underH = 0;
    g_tried = false;
    g_failed = false;
    g_copied = false;
    g_copyOk = false;
    g_copyFailLogged = false;
}

void WaterProbe()
{
    g_probeOn = true;
    g_pCount = g_pFirst = g_pLast = g_pDetailed = g_pOther = 0;
    g_pByVerts.clear();
    g_pByTex.clear();
    g_pTexCount.clear();
    g_pShaders.clear();
    g_pOthers.clear();
}

void WaterProbeTexture(IDirect3DDevice9* dev, const char* call, UINT nv, UINT pc, unsigned index, bool water)
{
    // The water's textures, learnt from its draws (kept across frames, up to 16).
    static std::set<void*> waterTex;
    auto* d = dev->lpVtbl;
    IDirect3DBaseTexture9* t[2] = {};
    d->GetTexture(dev, 0, &t[0]);
    d->GetTexture(dev, 1, &t[1]);
    if (water)
    {
        for (auto* x : t)
            if (x && waterTex.size() < 16)
                waterTex.insert(x);
    }
    else if (g_probeOn)
    {
        static unsigned frame = ~0u, logged = 0;
        if (frame != g_frameNo) { frame = g_frameNo; logged = 0; }
        if ((waterTex.count(t[0]) || waterTex.count(t[1])) && ++logged <= 20)
        {
            DWORD z = 0, zw = 0, bl = 0, src = 0, dst = 0, fvf = 0;
            d->GetRenderState(dev, D3DRS_ZENABLE, &z);
            d->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
            d->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &bl);
            d->GetRenderState(dev, D3DRS_SRCBLEND, &src);
            d->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
            d->GetFVF(dev, &fvf);
            IDirect3DVertexShader9* vs = nullptr;
            IDirect3DPixelShader9* ps = nullptr;
            d->GetVertexShader(dev, &vs);
            d->GetPixelShader(dev, &ps);
            D3DMATRIX wm = {};
            d->GetTransform(dev, D3DTS_WORLD, &wm);
            Log("water: another draw with the water's texture: #%u %s, %u vertices, %u triangles, vs %p ps %p fvf 0x%lX, "
                "z %lu write %lu, blend %lu %lu/%lu, textures %p %p, world (%.1f %.1f %.1f)", index, call, nv, pc, vs, ps,
                fvf, z, zw, bl, src, dst, t[0], t[1], wm.m[3][0], wm.m[3][1], wm.m[3][2]);
            if (vs) vs->lpVtbl->Release(vs);
            if (ps) ps->lpVtbl->Release(ps);
        }
    }
    for (auto* x : t)
        if (x) x->lpVtbl->Release(x);
}

// The client's own water is not drawn at all while our surface and the swash cover it (2026-10-02): drawn under
// ours, it still showed faintly at the shore, a second shoreline. Our pass then draws every pixel of the chunk:
// the water part opaque and writing depth, the rest from the screen copy. Not with the camera under the water,
// where our pass draws nothing and the game's surface seen from below is all there is.
IDirect3DPixelShader9* WaterFlatShader(IDirect3DDevice9* dev, bool water)
{
    static IDirect3DPixelShader9* shaders[2] = {};
    static bool tried[2] = {};
    const int i = water ? 0 : 1;
    if (!tried[i])
    {
        tried[i] = true;
        const char* src = water ? "float4 main() : COLOR { return float4(1, 0, 0, 1); }"
                                : "float4 main() : COLOR { return float4(1, 0, 1, 1); }";
        if (OgBlob* code = Compile(src, "water_flat", "ps_2_0"))
        {
            dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &shaders[i]);
            code->lpVtbl->Release(code);
        }
    }
    return shaders[i];
}

bool WaterHidesGame()
{
    const WaterSettings& w = g_cfg.water;
    if (!g_copyOk || !g_sceneOk || !WaterWanted() || w.surface <= 0.0f || w.swash <= 0.0f)
        return false;
    float cam[3], wz = 0.0f;
    return !(ClientCamera(cam) && MapWaterHeight(cam[0], cam[1], wz) && cam[2] < wz);
}

IDirect3DTexture9* WaterUnderDepth()
{
    return g_copyOk && WaterWanted() ? g_under : nullptr;
}

bool WaterProbing()
{
    return g_probeOn;
}
