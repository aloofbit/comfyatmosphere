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

#include "beacon.h"
#include "bench.h"
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
#include "shadercache.h"

#include <algorithm>
#include <intrin.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>
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
float4 gSs : register(c250);   // 1 / waveScale; 1 for water in a building (no depth in its vertices, no swell)
float4 gCellsV : register(c251); // the chunk's wet cells, as the pixel shader's c210 (FullGridDraw)
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
struct O { float4 pos : POSITION; float3 rel : TEXCOORD0; float amp : TEXCOORD1; float gd : TEXCOORD2;
           float z0 : TEXCOORD3; float2 cell : TEXCOORD4; };
// uv.y is the water's depth at the vertex, from the map files: 0.0549 at 8.1 yards and 0.1176 at 17.4 in the
// probe, depth / 148. The swell lifts only water 2.5 yards deep and more, in full from 6.5: the grid has a
// point every 4.2 yards, and lifted next to a steep bank its triangles stood up over the bank as straight lines
// (a pond in Tirisfal, 2026-10-02). Nearer the shore the waves are light only.
O main(float3 p : POSITION, float2 uv : TEXCOORD0)
{
    O o;
    float3 rel = (p.x * gW0 + p.y * gW1 + p.z * gW2 + gW3).xyz;
    // In a building (gSs.y) uv is the texture's, not the map's depth: no swell, and a nominal depth.
    const bool city = gSs.y > 0.5;
    // No swell on a point that no wet cell touches (2026-10-03): the whole grid is drawn for the swash, and some
    // dry points carry a deep water depth in the map files. The swell lifted the water's level in the dry cells,
    // and at each wave top the water ran far up the beach (Longshore). Wet: a cell of the 4 round the point.
    bool touchesWet = false;
    {
        const float2 g = floor(-p.xy * 0.24 + 0.5);   // the point's row and column (4.1667 yards apart)
        [unroll] for (int i = 0; i < 4; ++i)
        {
            const float2 rc = g - float2(i / 2, i % 2);
            if (all(rc >= 0.0) && all(rc <= 7.0))
            {
                const float bits = rc.x < 2.0 ? gCellsV.x : rc.x < 4.0 ? gCellsV.y : rc.x < 6.0 ? gCellsV.z : gCellsV.w;
                touchesWet = touchesWet || fmod(floor(bits * exp2(-(fmod(rc.x, 2.0) * 8.0 + rc.y))), 2.0) > 0.5;
            }
        }
    }
    float  amp = city || !touchesWet ? 0.0 : gSw.w * saturate((uv.y * 148.0 - 2.5) / 4.0);
    // The open sea settles (2026-10-03): far out, real water reads as flat, and the grid's big triangles of
    // swell were the part that looked wrong. Gone between 80 and 220 yards; the light follows (amp).
    amp *= 1.0 - smoothstep(80.0, 220.0, length(rel));
    float  h   = amp * Swell(rel.xy + gSw.xy, gSw.z, gSs.x);
    o.pos = p.x * gM0 + p.y * gM1 + p.z * gM2 + gM3 + h * gUp;
    o.rel = rel + float3(0.0, 0.0, h);
    o.amp = amp;
    o.gd  = city ? 3.0 : uv.y * 148.0;   // the water's depth at the vertex, as the map files give it
    o.z0  = rel.z;          // the flat water's height, camera-relative, before the swell lifts it
    o.cell = p.xy;          // in the chunk: 0 to -33.3 yards, rows along -x, columns along -y
    return o;
}
)HLSL";

    const char* kPsHlsl = R"HLSL(
sampler2D sUnder : register(s15);  // the depth under the water (INTZ), copied before the first water draw
sampler2D sScene : register(s14);  // the screen before the first water draw: what lies under the water
sampler2D sFoamTex : register(s12); // the game's own foam (WATERFOAMLOOP2.blp), tiled over the water
sampler2D sFoamBody : register(s11); // the drawn foam's blobs, made by the DLL (MakeFoamBody): 8 cells across, tiling
sampler2D sLeaves : register(s10);  // foliage and bodies (b) and the ground (r) on the screen before the water (bodymask.cpp,
                                    // BodyMaskLeavesNow)
float4 gZ    : register(c120);     // the projection's m22 and m32; the viewport's MinZ, 1 / (MaxZ - MinZ)
float4 gVz   : register(c121);     // the view's third column: a camera-relative point's view depth
float4 gScr  : register(c122);     // 1 / width, 1 / height, seconds, strength
float4 gFoam : register(c123);     // 1 / foamWidth, 1 / foamScale, speed, debug view
float4 gCol  : register(c124);     // the foam's colour, the distance it is gone at
float4 gFogC : register(c125);     // the game's fog colour, 1 / the distance it fades over
float4 gFog  : register(c126);     // the game's fog start, 1 / (end - start), 1 when known; w 1 with the
                                   // foliage mask (sLeaves)
float4 gCam  : register(c127);     // the camera in the world
float4 gReach : register(c128);    // 1 / foamReach, ripples' strength, wet sand's darkness, rings in use
float4 gRingD[16] : register(c146); // each ripple's way: the way its maker walked (x, y), 0 standing still;
                                    // its noise's shift (z); its waves' depth (w: Standing or Moving Ripple Depth)
float4 gRing[16] : register(c130); // the ripples: where each began (feet, camera-relative); w its age in
                                   // seconds plus 8 x its speed in tenths of a yard a second, negative for none
float4 gSun  : register(c162);     // the way to the sun, its glint's strength
float4 gDeep : register(c163);     // the colour deep water turns, how much of our water is drawn
float4 gAbs  : register(c164);     // the light the water absorbs a yard, by channel; refraction in yards
float4 gSky  : register(c165);     // the sky high up; the waves' strength
float4 gSunC : register(c166);     // the sun's colour; whitecaps
float4 gTrail[32] : register(c170); // the wakes: 4 trails of 8 points, newest first: camera-relative feet, age;
                                    // a ship's trail has -its size in its head's age (Ship Wake) and its depth in
                                    // its head's z (Ship Wake Depth)
                                    // in seconds (negative: no point)
float4 gMoon2 : register(c203);    // the way to the other moon, its glint's strength (0 by day)
float4 gGlint : register(c204);    // 1 / the glint's size squared; y the drawn foam's cut at the water's edge
                                   // ([water] foamEdge; the ripples' depth until 2026-10-05); z the
                                   // swash's highest climb (yards); w the
                                   // edge line's width (yards along the ground)
float4 gI0   : register(c205);     // rows of inverse(view x projection): clip -> camera-relative world
float4 gI1   : register(c206);
float4 gI2   : register(c207);
float4 gI3   : register(c208);
float4 gBright : register(c209);   // the water's brightness (Water Brightness); y the drawn wake's foam (Wake Foam);
                                   // z the foam round objects (Object Foam), w how far out it reaches (yards)
float4 gCells : register(c210);    // the chunk's wet cells: 8 bits a row, two rows in each (FullGridDraw)
float4 gWake : register(c202);     // trails in use, the wake's strength, 1 with the swash, its height (yards)
float4 gFT   : register(c169);     // 1 with the foam texture, 1 / its size in yards, its strength; the edge line
float4 gSw2  : register(c168);     // the shore waves' height (the swell's height x 0.25; 0: none); 1 on a body;
                                   // cover; w 1 for water in a building
float4 gWave : register(c167);     // 1 / waveScale; the part drawn (0 all, 1 the sand, 2 the water); 1 when
                                   // the screen copy is there; the sky reflection's strength
float4 gSwash : register(c211);    // the swash: 30 / its length along the shore (yards), its speed (radians a
                                   // second), as the wet sand pass's gSwashW; z the share of the sea's shore foam
                                   // this chunk gets, w of its swash and shore waves (1 on the sea; Lake Foam and
                                   // Lake Swash on a lake, a pond or a river)
float4 gFD   : register(c212);     // the drawn foam: 1 drawn (0 the game's texture), 1 / its size (yards), 1 / how
                                   // far it lasts from the edge (yards), the open water's foam (0 off)
float4 gPart[8] : register(c213);  // the parting's particles: where each was let go (camera-relative, xyz), w its age
                                   // as a share of its own life, 0..1 (-1: none more)
float4 gLhT  : register(c221);     // the lighthouse's light on the water: the wave faces' strength, the tilt they start
                                   // at, how soft the edge is, the half width of the patch the beam lights (yards a yard out)
float4 gLh   : register(c222);     // the nearest lighthouse's lamp, camera-relative (xyz); w its glint's strength (0 none)
float4 gRain : register(c223);     // the rain: how hard it rains, 0..1 (from the game's own rain draws); Rain on Water;
                                   // zw the nearest lighthouse's beam's way across the ground, 2 long with a second beam

// The camera-relative point the depth under the water shows at a place on the screen: the bed, or what
// stands on it.
float3 BedAt(float2 uvq)
{
    float  r   = tex2Dlod(sUnder, float4(uvq, 0, 0)).r;
    float  d   = min((r - gZ.z) * gZ.w, 0.99999);
    float2 ndc = float2(uvq.x * 2.0 - 1.0, 1.0 - uvq.y * 2.0);
    float4 wp  = ndc.x * gI0 + ndc.y * gI1 + d * gI2 + gI3;
    return wp.xyz / wp.w;
}
)HLSL"
    // Split: MSVC takes no string literal longer than 16 KB (C2026).
    R"HLSL(
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

// The drawn foam (2026-10-05), in the style of Breath of the Wild: flat white shapes with clean, soft edges, round
// scallops and round holes, not a photograph of bubbles. The owner found the game's waterfall texture, laid flat
// over the sea, wrong for it: the same pattern on every beach, drifting sideways whatever the water did.
//
// The foam's body, 0..1: round blobs (the DLL's texture, cellular noise of two sizes), read twice at other sizes
// and angles so its repeat does not show, the whole warped by a slow noise so the edges scallop and wander, and a
// broad noise so some stretches hold more foam than others. p is in cells. The cellular noise was worked out here
// at first: 18 cell lookups a pixel left the shader no temporary registers (ps_3_0 has 32), and the texture's
// mips keep it clean far off.
// The level of a 256-texel texture laid over the water at `scale` repeats a yard, for a pixel `ypp` yards across.
float LodAt(float ypp, float scale)
{
    return log2(max(ypp * scale * 256.0, 1e-6));
}
// lod: the texture's level for the first read, from the yards a pixel covers (2026-10-06): the reads were by the
// pixels beside, which cannot be done inside a branch.
float FoamField(float2 p, float t, float lod)
{
    float2 w  = float2(ValueNoise(p * 0.45 + float2(t * 0.05, 0.0)), ValueNoise(p * 0.45 + float2(7.3, -t * 0.04))) - 0.5;
    float2 q  = p + w * 1.1;
    float2 qr = float2(q.x * 0.8 - q.y * 0.6, q.x * 0.6 + q.y * 0.8);
    float  a  = tex2Dlod(sFoamBody, float4(q * 0.125, 0.0, lod)).r;
    float  b  = tex2Dlod(sFoamBody, float4(qr * 0.093 + 0.37, 0.0, lod - 0.43)).r;   // 0.093 / 0.125
    float  broad = ValueNoise(q * 0.3 - t * 0.02);
    return saturate(a * 0.6 + b * 0.25 + broad * 0.3);
}
// Foam where the body passes th: a clean edge, aa soft. aa is how fast the body changes across a pixel, taken
// once outside any branch (a gradient inside one does not compile).
float FoamCut(float field, float th, float aa)
{
    return smoothstep(th - aa, th + aa, field);
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
// Each train calms with the distance by its own length (2026-10-03): a long wave still shows far out, so the
// pattern gets finer toward the horizon. One value for all of them (half at 60 yards) left the far water one
// flat tone. A train L yards long is at half at 12 L yards.
float3 WaveNormal(float2 p, float t, float strength, float2 swell, float dist)
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
        g += d * (s[i] * cos(k * dot(d, p) - w * t)) / (1.0 + dist * k / (12.0 * 6.2832));
    }
    float2 q = p * 1.6 + float2(t * 0.4, t * 0.25);
    float  n0 = ValueNoise(q);
    g += float2(ValueNoise(q + float2(0.15, 0.0)) - n0, ValueNoise(q + float2(0.0, 0.15)) - n0) * 0.35 /
         (1.0 + dist / 60.0);
    return normalize(float3(-(g * strength + swell), 1.0));
}

)HLSL"
    // Split in two: MSVC takes no string literal longer than 16 KB (C2026).
    R"HLSL(
float4 main(float3 rel : TEXCOORD0, float amp : TEXCOORD1, float gd : TEXCOORD2, float z0 : TEXCOORD3,
            float2 cell : TEXCOORD4, float2 vpos : VPOS) : COLOR
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
    const float depthSeen = depth;   // from the depth copy: negative where something stands in front
    // On a body under the water (a character's legs, drawn in a draw of their own, below): it is seen through
    // the water, never a shore: no foam, no lip, no edge line (2026-10-02).
    const bool onBody = gSw2.y > 0.5;
    if (onBody)
        depth = max(depth, 0.5);
    float  t   = gScr.z * gFoam.z;
    float  dist = length(rel);
    // The sea, or a lake, a pond or a river (2026-10-05, the owner: a pond in Goldshire had the sea's swash and
    // foam): the share of the swash and the shore waves (seaK), and of the shore foam (seaF), this water gets.
    const float seaK = gSwash.w, seaF = gSwash.z;
    // From 40 yards out the map's depth takes over from the depth copy's, by 90 all of it: the far terrain is
    // in a slice of its own (farSlice), and where it met the near terrain the water's colour jumped along a
    // straight line (2026-10-02). So both sides agree well before they meet. Not on a body.
    // Not in a building (gSw2.w): the map has no depth for its water, and the depth copy's is the real one.
    // Not at the waterline either (2026-10-05): the map gives one depth at each corner of a cell 4 yards wide,
    // and where it crossed 0 the water's edge went in steps, off the ground's own line, seen from far off (the
    // owner). The depth copy has the ground at every pixel, so it keeps the first yard or so of water and all
    // the terrain rising out of it; the map's depth takes over in deeper water only.
    const float mapK = farSlice ? 1.0 : smoothstep(40.0, 90.0, dist) * smoothstep(0.3, 1.5, depth)
                                        * (gSw2.y > 0.5 || gSw2.w > 0.5 ? 0.0 : 1.0);
    depth = lerp(depth, gd, mapK);
)HLSL"
    R"HLSL(
    // Debug View 22: the height over the flat water before any cut, as contours every 0.1 yards (a dark line
    // each, a brighter one each yard): green above the water, red below; blue tint on the cells the game leaves
    // dry. The whole grid of every chunk is drawn.
    if (gFoam.w > 6.5 && gFoam.w < 7.5)
    {
        const float2 rc  = clamp(floor(-cell * 0.24), 0.0, 7.0);
        const float  bits = rc.x < 2.0 ? gCells.x : rc.x < 4.0 ? gCells.y : rc.x < 6.0 ? gCells.z : gCells.w;
        const float  wetC = fmod(floor(bits * exp2(-(fmod(rc.x, 2.0) * 8.0 + rc.y))), 2.0);
        const float  h    = -depth;
        const float  band = frac(abs(h) * 10.0);
        const float  ink  = band < 0.12 ? (frac(abs(h)) < 0.1 ? 1.0 : 0.35) : 0.0;
        float3 col = h > 0.0 ? float3(0.1, 0.5 + 0.5 * saturate(1.0 - h / 3.0), 0.1)
                             : float3(0.5 + 0.5 * saturate(1.0 + h / 3.0), 0.1, 0.1);
        col = lerp(col, float3(0, 0, 0), ink);
        col = wetC > 0.5 ? col : lerp(col, float3(0.2, 0.3, 1.0), 0.4);
        return float4(abs(h) < 3.0 ? col : float3(0.05, 0.05, 0.05), 1.0);
    }
    // Two parts (2026-10-02): the water, depth tested and writing depth, and the sand beside it, not tested.
    // Each pixel belongs to one: where our water is drawn (wv > 0), the water part.
    float  wvIs = smoothstep(0.0, 0.03, depth) * gDeep.w * gWave.z;
    // Whether this pixel stays (2026-10-06): each test below once cut it with clip(), and now each lowers keep.
    // A pixel clip() drops is not stopped: DXVK turns it into a helper that runs the rest of the shader, so each
    // of the three draws over a chunk cost the whole shader on every pixel of the chunk, about 3 ms a draw at
    // the harbour. Past the tests a real branch leaves (below).
    float  keep = 1.0;
    if (gWave.y > 0.5)
    {
        // The first 0.15 yards of water go with the sand part, untested: there the water's plane lies a hair
        // under the sand, the depth test turned the water part away, the sand part left it to the water part,
        // and the game's own water showed through as a light line along the shore (2026-10-02). There is no
        // swell that near the shore, so nothing needs the test there.
        const bool waterPart = wvIs > 0.0 && depth > 0.15;
        keep = min(keep, gWave.y > 1.5 ? (waterPart ? 1.0 : -1.0) : (waterPart ? -1.0 : 1.0));
    }
    // Above the water nothing is left to draw here: the foam, the lip, the ripples and the wake are all on the
    // water, and the wet sand and the swash have a pass of their own. So such a pixel leaves now, before the rest:
    // the sand part is drawn without the depth test, and every pixel of every chunk ran the whole shader, dunes
    // and all in front of the sea behind them (2026-10-02: once every chunk was ours, the frame rate fell
    // through the floor). Without the swash the lip reaches 0.4 yards up the sand.
    // With the swash on, up to its highest climb (gGlint.z): the water itself runs up the sand (2026-10-03).
    keep = min(keep, depth + (gWake.z > 0.5 ? gGlint.z + 0.05 : 0.45));
    // And from the depth copy, before the map's depth stood in (2026-10-03). The map knows no hill between you
    // and the water: from 40 yards out the shore, its foam, lip and edge line, drawn without the depth test,
    // showed through the hill in front of it. Not past the world's slice, nor on a body.
    keep = min(keep, farSlice || onBody ? 1.0 : depthSeen + (gWake.z > 0.5 ? gGlint.z + 0.05 : 0.45));
    // A cell the game leaves dry (2026-10-03): the sand part covers the whole grid, for the swash past the last
    // wet cell, but there only above the flat water: the game draws no water in a dry cell, so nothing under its
    // level is ours to fill either.
    {
        const float2 rc  = clamp(floor(-cell * 0.24), 0.0, 7.0);     // row, column (4.1667 yards a cell)
        const float  bits = rc.x < 2.0 ? gCells.x : rc.x < 4.0 ? gCells.y : rc.x < 6.0 ? gCells.z : gCells.w;
        const float  wet = fmod(floor(bits * exp2(-(fmod(rc.x, 2.0) * 8.0 + rc.y))), 2.0);
        keep = min(keep, wet > 0.5 ? 1.0 : -depth);
    }
    clip(keep);

    // The wake runs here, near the start, since 2026-10-05: with the drawn foam the shader ran out of temporary
    // registers in this loop (ps_3_0 has 32), and here few values are held through it. It takes the amount alone;
    // the game's foam texture (the old style) multiplies it later, the same for every trail at a pixel.
    // The wake (2026-10-02): behind anyone moving through the water, along the path they took, a V of two arms
    // at about 20 degrees either side, and churned water just behind the body. The arms are waves (their slope
    // goes into the light, as the ripples' does), with a little of the game's foam on them. Each trail is up to
    // 8 points of the path, newest first; a pixel takes the segment of the path nearest to it.
    float2 wakeSlope = 0.0;
    float  wakeFoam  = 0.0;
    float  part      = 0.0;   // the drawn foam parted along a path through it: 1 clear (2026-10-05, the owner)
    [loop] for (int wi = 0; wi < (int)gWake.x; ++wi)
    {
        // Far from the trail's maker, nothing: a trail is at most some 5 yards long and its arms reach 2 out.
        // A ship's trail (2026-10-06, the owner) is a body's at sc times the size and sc times as slow: its head's
        // age holds -sc.
        float4 head = gTrail[wi * 8];
        float  sc   = head.w < 0.0 ? -head.w : 1.0;
        float  deep = sc > 1.5 ? head.z : 1.0;   // a ship's: its head's height is not needed, as it floats
        float2 headOff = rel.xy - head.xy;
        [branch] if (dot(headOff, headOff) > 18.0 * 18.0 * sc * sc)
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
                bestAge = lerp(max(a.w, 0.0), b.w, tt);
                bestSub = rel.z - lerp(a.z, b.z, tt);
                bestDir = (rel.xy - q) / max(lat, 1e-3);
            }
            sAcc += len;
        }
        bestLat /= sc;
        bestS   /= sc;
        bestAge /= sc;
        // How much of the body is in the water, as for the ripples: feet small, waist the most, under small. A
        // ship floats: all of it.
        float size = sc > 1.5 ? 1.0 : smoothstep(0.05, 0.9, bestSub) * (1.0 - 0.75 * smoothstep(1.1, 2.0, bestSub));
        // Gone within some 4 yards behind the body and 2 seconds (2026-10-02: by age alone it reached 8 yards back).
        float life = exp(-bestAge * 1.4) * smoothstep(0.0, 0.6, bestS) * (1.0 - smoothstep(1.5, 4.0, bestS)) * size;
        // The arms, a crest and the trough inside it, widening with the distance behind.
        float armAt = bestS * 0.36;
        float armW  = 0.22 + 0.06 * bestS;
        float y     = (bestLat - armAt) / armW;
        float arm   = exp(-y * y);
        float dArm  = (1.0 - 2.0 * y * y) * exp(-y * y) / armW;
        // Faded to nothing on the path itself (2026-10-03): bestDir points away from the path, so it turns over
        // there, and the tilt flipped from one side to the other at full strength: a straight line along the
        // path behind a runner, as if the water parted.
        wakeSlope += bestDir * (dArm * life * 0.11 * deep * smoothstep(0.0, armW, bestLat));
        // The churned water close behind the body: foam, narrow, gone in a second and a half.
        float churn = exp(-pow(bestLat / (0.35 + 0.08 * bestS), 2.0)) * exp(-bestAge * 2.5) * size *
                      smoothstep(0.0, 0.4, bestS) * (1.0 - smoothstep(1.0, 2.5, bestS));
        // The amount alone: cut (the drawn foam) or textured (the old style) where the foam is put together.
        // Drawn (2026-10-05, the owner): a splash at the body, and bubbles left along the path that stay where they
        // formed and thin out over 3.5 seconds: the cut rises as they age, so they shrink and break up, then go.
        // Not on the arms: there they filled the channel the parting clears. Wake Foam sets how much.
        float trailW = 0.35 + 0.1 * bestS;
        float bubbles = exp(-(bestLat * bestLat) / (trailW * trailW)) * saturate(1.0 - bestAge / 3.5) * size *
                        smoothstep(0.0, 0.5, bestS);
        wakeFoam = max(wakeFoam, gFD.x > 0.5 ? max(churn * (1.0 - smoothstep(0.3, 1.1, bestS)), bubbles * 0.7) * gBright.y
                                             : arm * 0.5 * life + churn * 0.65);
    }
    wakeSlope *= gWake.y;
    // Rain on the water (2026-10-05, the owner): small rings, each cell of a grid its own place and moment, at two
    // sizes, the more cells firing the harder it rains. As light only, into the wake's slope, so the glint and the
    // sky in the water break up and the light follows them. Each ring stays inside its cell, so one cell is read a
    // size. Faded out from 25 to 45 yards, where a ring is smaller than a pixel.
    [branch] if (gRain.x > 0.0 && gRain.y > 0.0 && dist < 45.0 && !onBody)
    {
        float2 A4 = rel.xy + gCam.xy;
        float  rainK = gRain.y * (1.0 - smoothstep(25.0, 45.0, dist));
        [unroll] for (int li = 0; li < 2; ++li)
        {
            float  cellS = li == 0 ? 0.6 : 0.38;   // 0.9 and 0.55 at first: the owner asked them smaller
            float2 q   = A4 / cellS + (li == 0 ? 0.0 : 17.3);
            float2 ci  = floor(q);
            float  tt  = gScr.z * (li == 0 ? 0.9 : 1.3) + Hash1(ci + 3.1) * 7.0;
            float  cyc = frac(tt);
            float  gen = floor(tt);
            float2 cen = 0.3 + 0.4 * Hash2(ci + gen * 0.731);
            float  on  = Hash1(ci + gen * 1.37 + 5.0) < gRain.x ? 1.0 : 0.0;
            float2 dv  = (q - ci) - cen;
            float  dd  = length(dv);
            float  rr  = cyc * 0.28;
            float  ww  = 0.05 + 0.04 * cyc;
            float  yv  = (dd - rr) / ww;
            float  dh  = (1.0 - 2.0 * yv * yv) * exp(-yv * yv) / ww;
            wakeSlope += dv / max(dd, 1e-3) * (dh * (1.0 - cyc) * (1.0 - cyc) * on * rainK * 0.007 / cellS);   // 0.012 at first: shallower
        }
    }

    // What takes a gradient, worked out before the branch below (2026-10-06), after the wake's loop, which held
    // the most registers: a gradient, or a texture read
    // that takes its level from one, cannot be inside a branch, and the compiler would undo the branch. Each is
    // used further down, where it was worked out before.
    // Yards a pixel, for the foam round objects.
    float  ypp  = max(length(ddx(rel)), length(ddy(rel)));
    // For far terrain, the bed's slope from the pixels beside (see grad below).
    float2 farGrad = 0.0;
    {
        const float dF = depth - (rel.z - z0) * (1.0 - mapK);   // depthF, below
        float2 gx = ddx(rel.xy), gy = ddy(rel.xy);
        float  ex = ddx(dF), ey = ddy(dF);
        float  det = gx.x * gy.y - gx.y * gy.x;
        farGrad = abs(det) > 1e-8 ? float2(ex * gy.y - gx.y * ey, gx.x * ey - gy.x * ex) / det : float2(0, 0);
    }
    // Whether the ground the line of sight meets lies flat (the lip, below).
    float  flat;
    {
        float3 P = rel * (zg / zw);
        float3 n = cross(ddx(P), ddy(P));
        flat = abs(n.z) > 0.85 * length(n) ? 1.0 : 0.0;
    }
    // How fast the drawn foam's body changes across a pixel, for its edges. The body itself is read again below,
    // inside the branch: held through it, it and the values above ran the shader out of registers.
    float  foamAA;
    {
        const float2 swS0 = amp * SwellSlope(rel.xy + gCam.xy, gScr.z, gWave.x);
        const float  f0   = FoamField((rel.xy + gCam.xy + swS0 * 1.5) * gFD.y, t, LodAt(ypp, gFD.y * 0.125));
        foamAA = max(fwidth(f0) * 0.75, 0.015);
    }
    [branch] if (keep < 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    // Foam round objects in the water (2026-10-05, the owner): posts, rocks, piers, cliffs, legs. Points round this
    // pixel on the screen, out to Object Foam Width in yards, are rebuilt from the depth under the water (BedAt);
    // one rising out of the water, not the ground and not foliage, is something standing through the surface,
    // and the foam is the more the nearer it is. It works from what the screen shows: the water behind an object is hidden by
    // it anyway. Yards a pixel from the surface's own change across it, taken outside the branch.
    float objF = 0.0;
    [branch] if (gFD.x > 0.5 && gBright.z > 0.0 && gFog.w > 0.5 && !farSlice && !onBody && dist < 60.0)
    {
        float  rPix = gBright.w / max(ypp, 1e-4);
        // Eight ways at the full width, and the four diagonals at half of it.
        float2 offs[12] = { float2(1, 0), float2(0.7071, 0.7071), float2(0, 1), float2(-0.7071, 0.7071),
                            float2(-1, 0), float2(-0.7071, -0.7071), float2(0, -1), float2(0.7071, -0.7071),
                            float2(0.3536, 0.3536), float2(-0.3536, 0.3536), float2(-0.3536, -0.3536),
                            float2(0.3536, -0.3536) };
        [unroll] for (int oi = 0; oi < 12; ++oi)
        {
            float3 P  = BedAt(uv + offs[oi] * rPix * gScr.xy);   // uvO below
            float  dzO = P.z - rel.z;
            float  dxy = length(P.xy - rel.xy);
            float2 uvO = uv + offs[oi] * rPix * gScr.xy;
            // Not foliage (the owner): reeds and grass in the shallows are no objects to foam round. Nor a player or
            // a creature (2026-10-06, the owner: foam round a character standing still); its wake makes its foam. Not the ground
            // either: the shoreline has the shore's foam, and taken for an object it drew a second line (the owner).
            float2 mk   = gFog.w > 0.5 ? tex2Dlod(sLeaves, float4(uvO, 0, 0)).rb : 0.0;
            // Anything rising out of the water, from a third of a yard under it to 2.5 yards over: a wall going up
            // through the surface crossed a band a third of a yard either side of it in a few pixels alone, and the
            // points seldom fell there (a pillar had none, 2026-10-05). The distance is across the water, so a point
            // on the wall over the water finds its foot.
            float  at  = (dzO > -0.35 && dzO < 2.5 ? 1.0 : 0.0) * saturate(1.0 - dxy / gBright.w) *
                         (mk.x > 0.5 || mk.y > 0.5 ? 0.0 : 1.0);
            objF = max(objF, at);
        }
    }

    // The foam parts as particles do (2026-10-05, the owner): each body at the water that moves lets one go every
    // half yard, where it is; each stays there, grows over the first fifth of its life and then shrinks and fades out, so a body moving
    // leaves a trail of them and one standing still parts nothing. In the swash too: there the feet are over the
    // flat water and no wake trail starts. A following disc and a channel along the wake's path came first.
    [loop] for (int pi = 0; pi < 8; ++pi)
    {
        float4 pp = gPart[pi];
        if (pp.w < 0.0)
            break;
        float  ph = saturate(pp.w);
        float2 po = rel.xy - pp.xy;
        // Its life (the owner): born at size 0, it grows to 0.55 yards (0.7 until the owner asked smaller) over the first tenth of its life, then shrinks
        // and fades out together to nothing. A trail of them opens just behind the body and narrows behind it into
        // a V. Its edge stays clean at any size.
        float  grow   = smoothstep(0.0, 0.1, ph);
        float  shrink = 1.0 - smoothstep(0.1, 1.0, ph);
        float  pr = 0.55 * grow * shrink + 0.01;
        float  pd = length(po);
        float  pz = rel.z - pp.z;
        part = max(part, (1.0 - smoothstep(pr * 0.55, pr, pd)) * shrink * (pz > -1.0 && pz < 2.0 ? 1.0 : 0.0));
    }

)HLSL"
    R"HLSL(
    // How far the waterline is, across the water: the depth over the bed's slope. The slope is how fast the
    // depth grows per yard of the surface, from the pixels beside this one. On a gentle beach the depth stays
    // small for many yards, and the foam went 20 yards out by depth alone (2026-10-02).
    // Measured under the flat water, the swell's lift left out (2026-10-03). The lifted surface is flat
    // triangles, a point every 4.2 yards, and its tilt jumps at each edge: the slope, the way to the shore and
    // the shore waves' facing (shoreSlope) jumped with it, and each triangle had a glint and a sky of its own,
    // cut off along its edges.
    // The map's depth (mapK) is under the flat water already.
    float  depthF = depth - (rel.z - z0) * (1.0 - mapK);
    // And from bed points 8 pixels either side, not from the pixel beside (2026-10-03). The ground under the
    // water is flat triangles too: from one pixel to the next its slope jumps at each of their edges, and the
    // shore waves' place and facing (reach, gdir) jumped with it. Their light, the white patches on the water,
    // was torn along the ground's triangles, the more the higher Wave Height. 8 pixels apart the difference
    // changes smoothly as the pixel moves. Far terrain has a camera of its own: there the pixel beside.
    // Each axis weighs its two sides by how near in height each is to this pixel's own point (2026-10-03).
    // 8 pixels from a character a point lands on its legs, and the steep "beach" it made lit the shore waves
    // on and round the character, every way at once: such a side counts for almost nothing. A weight, not a
    // choice: taking the nearer side flipped where the ground's triangles meet, and the tear came back.
    float2 o8  = gScr.xy * 8.0;
    float3 b0  = BedAt(uv);
    float3 xa  = BedAt(uv + float2(o8.x, 0.0)) - b0, xb = b0 - BedAt(uv - float2(o8.x, 0.0));
    float3 ya  = BedAt(uv + float2(0.0, o8.y)) - b0, yb = b0 - BedAt(uv - float2(0.0, o8.y));
    float  wxa = (xb.z * xb.z + 1e-4) / (xa.z * xa.z + xb.z * xb.z + 2e-4);
    float  wya = (yb.z * yb.z + 1e-4) / (ya.z * ya.z + yb.z * yb.z + 2e-4);
    float3 bX  = xa * wxa + xb * (1.0 - wxa);
    float3 bY  = ya * wya + yb * (1.0 - wya);
    float2 gx  = bX.xy, gy = bY.xy;
    float  ex  = -bX.z;   // the depth grows as the bed goes down
    float  ey  = -bY.z;
    float  det = gx.x * gy.y - gx.y * gy.x;
    float2 grad = farSlice ? farGrad   // from the pixels beside, above the branch
                : abs(det) > 1e-8 ? float2(ex * gy.y - gx.y * ey, gx.x * ey - gy.x * ex) / det : float2(0, 0);
    float  slope = clamp(length(grad), 0.02, 4.0);
    float  reach = depthF / slope;
    // Debug View 23: the slope the swash and the shore waves use: brighter up to 0.3, a dark line every 0.05.
    if (gFoam.w > 7.5 && gFoam.w < 8.5)
    {
        const float sv = length(grad);
        return float4(lerp(saturate(sv / 0.3).xxx * float3(1.0, 0.8, 0.3), 0.0, frac(sv * 20.0) < 0.1 ? 0.8 : 0.0), 1.0);
    }
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
    // (flat, whether that ground lies flat, is worked out above the branch.)
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
    // The noise that makes each ring uneven, once for all of them: per ring it is shifted by the ring's own shift.
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
            // The ring's own, fixed at its start (2026-10-03). It was the slot's (i x 0.618): when the oldest
            // ring ended, every other ring moved down a slot, its shape and its slope jumped, and the glint on
            // them flashed, about once a second round anyone standing in the water.
            float  shift = gRingD[i].z;
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
            // the wake (2026-10-02). A walker's ring is the one with a way (2026-10-05); it was told by its spread,
            // over 1.15 yards a second, until the spread became a slider.
            float walker = dot(way, way) > 0.25 ? 1.0 : 0.0;
            ring = max(ring, fade * exp(-x * x) * lerp(1.0, exp(-age * 3.0), walker));
            // The wave: one crest and the trough after it, wider than the foam's line, its slope along the way
            // out from where it began.
            float  wv2 = 0.18 + 0.25 * ph;
            float  y   = (d - rad) / wv2;
            float  dh  = (1.0 - 2.0 * y * y) * exp(-y * y) / wv2;   // the slope of y * exp(-y^2)
            float2 out2 = (rel.xy - g.xy) / max(d, 1e-3);
            // Its depth (2026-10-05): Standing Ripple Depth or Moving Ripple Depth, by whether its maker moved
            // when it began. Only the waves: the foam line stays as it is.
            ringSlope += out2 * (dh * fade * 0.065 * lerp(1.0, 0.6, walker) * gRingD[i].w);
        }
    }
    ring = saturate(ring * gReach.y) * (0.3 + 0.7 * soft) * (depth > -0.05 ? 1.0 : 0.0);

    // The game's foam texture, for the old style's shore foam and wake. At a level from the yards a pixel covers
    // (2026-10-06): inside the branch above, the pixels beside cannot give it.
    float2 fp  = (rel.xy + gCam.xy + swS * 1.5) * gFT.y;
    float  ftl = LodAt(ypp, gFT.y);
    float  fa1 = tex2Dlod(sFoamTex, float4(fp + float2(t * 0.010, t * 0.006), 0.0, ftl)).a;
    float  fa2 = tex2Dlod(sFoamTex, float4(fp * 0.71 + float2(0.37 - t * 0.007, 0.21 + t * 0.011), 0.0, ftl - 0.49)).a;
    float  ftx = 0.6 * fa1 + 0.4 * fa2;

)HLSL"
    R"HLSL(
    // The drawn foam's body, once a pixel, after the loops (it holds registers): the shore, the wake and the open water all cut it.
    // foamAA, how fast it changes across a pixel, is worked out above the branch.
    float  foamN = FoamField((rel.xy + gCam.xy + swS * 1.5) * gFD.y, t, LodAt(ypp, gFD.y * 0.125));

    // Shore waves (2026-10-02): crests along the shore that roll in toward it. Their phase is the distance to
    // the waterline (reach), so they come about 4 yards apart on any slope: by depth they bunched into thin
    // lines on a steep stretch. They come in sets along the shore (a slow noise), and fade out past 9 yards and
    // past 3.5 yards of depth. Too short for the water's grid (a point every 4.2 yards) to lift: they are
    // drawn as light, sloping toward the shore, and as foam where they break, in the last 3 yards.
    float  sets  = smoothstep(0.25, 0.75, ValueNoise(wp * 0.04 + float2(t * 0.03, -t * 0.02)));
    float  sEnv  = smoothstep(0.1, 1.0, reach) * (1.0 - smoothstep(5.0, 9.0, reach)) *
                   (1.0 - smoothstep(2.0, 3.5, depth)) * sets * seaK;
    float  sPh   = reach * 1.6 + t * 1.4 + soft * 1.2;
    float  sS    = 0.5 + 0.5 * sin(sPh);
    float  crest = sS * sS;
    // None on a body under the water: it has no shore (2026-10-03).
    float2 shoreSlope = onBody || gSw2.w > 0.5 ? 0.0 : gdir * (gSw2.x * sEnv * sS * cos(sPh) * 1.6);
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
    // With the swash on, the foam is placed from the moving edge, not the fixed waterline: the water has run up
    // the sand by up (yards of height, the wet sand pass's swash, the same timing), so here it is that much
    // deeper below the edge (2026-10-02: the foam stayed at the old waterline while the edge moved).
    float  depthS = depth, reachS = reach;
    float  sgS = 0.5;   // the swash's phase here: 0 drained, 1 run up as far as it goes
    {
        float2 A2  = rel.xy + gCam.xy;
        // Each stretch of shore in its own time: the stretch about 30 yards at Swash Length 30 (2026-10-05).
        float2 A2s = A2 * gSwash.x;
        float  ph2 = sin(A2s.x * 0.21 + A2s.y * 0.17) * 1.7 + sin(A2s.x * 0.07 - A2s.y * 0.09) * 2.3;
        float  sg  = 0.5 + 0.5 * sin(gScr.z * gSwash.y + ph2);
        // The foam's reach follows it too, by the same share as the run-up (Lake Swash): at 0 a pond's foam band
        // still breathed in and out with the swash's time, and read as a swash (the owner, 2026-10-05).
        sgS = gWake.z > 0.5 ? lerp(0.5, sg, seaK) : 0.5;
        // No higher than the wet sand (2026-10-05, the owner): at Swash Height 65 the water ran onto dry sand.
        // The wet sand pass's top: fully wet below half of it, fading to dry at it. The swash stops at half of it,
        // where the sand is still fully wet: at 0.75 of it the owner saw the water pass the wet sand.
        float  topS = 0.4 + 0.1 * sin(A2.x * 0.13 + A2.y * 0.11);
        float  upS = min(min(gWake.w * slope, gGlint.z), topS * 0.5) * sg * sg * gWake.z * seaK;   // as the wet pass

        // No swash in a building: it climbed the canals' stone walls.
        depthS = onBody || gSw2.w > 0.5 ? depth : depth + upS;
        reachS = depthS / slope;

    }
    // One shoreline (2026-10-03): the water itself runs up the sand to the moving edge, waves, sky and glint and
    // all. The swash was a flat film of its own in the wet sand pass, and where it met this water, at the flat
    // waterline, it showed as a second shoreline whatever its colour. Above the moving edge, nothing.
    clip(depthS + 0.02);
    [branch] if (depthS + 0.02 < 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);
    float  shoreS = saturate(1.0 - max(depthS * gFoam.x * (1.0 + slope * 4.0), reachS * gReach.x));
    float  bandS  = sin(reachS * 4.0 + t * 1.6) * 0.5 + 0.5;
    float  fshape = shoreS * (0.55 + 0.45 * bandS);
    // Only the texture's denser parts pass, even at the waterline: with the whole of it, the band near the shore
    // was solid white and the texture's own shapes were lost (2026-10-02).
    float  fcut  = 1.0 - 0.7 * fshape;
    float  fgame = smoothstep(fcut, fcut + 0.3, ftx) * saturate(fshape * 2.0);
    // From the edge line on, never onto the sand: it starts where our water does (2026-10-02: started a quarter
    // of a yard out, it left a strip of clear water between the line and the bubbles).
    float  fback = smoothstep(0.0, 0.03, depthS);
    float  ageS = 0.0, drawnS = 0.0;
    if (gFD.x > 0.5)
    {
        // Its age, from the moving edge out: 0 at the edge, 1 at the end of its reach. As the water runs up the
        // foam is fresh and reaches far; as it drains it is older, and only its larger blobs are left.
        ageS = reachS * gFD.z * lerp(1.3, 0.8, sgS) * lerp(2.5, 1.0, seaF);   // a lake's reaches less far
        // At the edge nearly all foam, a sheet with a few holes; further out the cut rises and the holes open,
        // until only the blobs' middles are left, then nothing.
        // At the edge the cut is foamEdge, not near 0 (2026-10-05): from 0.2 the youngest foam was a solid sheet,
        // and the swash read as a band of another colour beside the open water's foam (the owner).
        float th = lerp(gGlint.y, 0.9, saturate(ageS));
        // Parted along a path (the wake loop): no foam in the channel, a little more along its sides, where the
        // foam was pushed.
        th += part * 1.2 - 0.3 * saturate(part * (1.0 - part) * 4.0);
        // A shore wave's crest brings fresh foam where it breaks, in the last 3 yards (the shore waves above).
        th -= 0.3 * crest * sEnv * smoothstep(3.0, 0.5, reach) * (gSw2.x > 0.0 ? 1.0 : 0.0);
        drawnS = FoamCut(foamN, th, foamAA) * (1.0 - smoothstep(0.85, 1.0, ageS)) * fback;
        // Far off the shapes are smaller than a pixel and would flicker: their share of the water instead.
        drawnS = lerp(drawnS, saturate((1.0 - th) * 1.1) * (1.0 - smoothstep(0.85, 1.0, ageS)) * fback,
                      smoothstep(35.0, 70.0, dist));
        // Only over the ground (2026-10-05, the owner): a sunken rock or a building's piece just under the water
        // made it as shallow as a beach, and the shore's foam lay on it. What lies under this pixel is the ground
        // where the mask's red is (bodymask.cpp); objects rising out of the water have their own foam. Without the
        // mask, as before. Not on a body, whose legs the foam never takes anyway.
        float overGround = gFog.w > 0.5 && !onBody ? tex2Dlod(sLeaves, float4(uv, 0, 0)).r : 1.0;
        drawnS *= overGround > 0.5 ? 1.0 : 0.0;
        foam = max(foam, drawnS * gFT.z * lerp(0.4, 1.0, seaF));
    }
    else
        foam = max(foam, fgame * gFT.z * gFT.x * fback * lerp(0.4, 1.0, seaF));
    // Foam on open water (2026-10-05, a toggle): small blobs on the swell's crests out in deep water, drawn
    // longer along the wind, in stretches that come and go. gFD.w is its strength, 0 when off.
    float capsD = 0.0;
    float2 A3    = rel.xy + gCam.xy;
    float2 alongW = float2(dot(A3, float2(0.7071, 0.7071)) * 0.45, dot(A3, float2(-0.7071, 0.7071)));
    float  capN2  = FoamField(alongW * gFD.y * 0.8 + 31.0, t, LodAt(ypp, gFD.y * 0.1));
    if (gFD.w > 0.0)
    {
        float  crestO = smoothstep(0.62, 0.92, Swell(A3, gScr.z, gWave.x));
        float  windy  = smoothstep(0.35, 0.75, ValueNoise(A3 * 0.025 + float2(t * 0.02, t * 0.015)));
        float  want   = crestO * windy * saturate(depth - 1.5) * saturate(gFD.w * 2.0);
        capsD = FoamCut(capN2, 1.0 - want * 0.45, foamAA * 1.2) * saturate(want * 3.0);
        capsD = lerp(capsD, want * 0.25, smoothstep(40.0, 90.0, dist));
        foam = max(foam, capsD * gFD.w * (depth > 0.05 ? 1.0 : 0.0));
    }
    // Round objects: the same shapes, more of them the nearer the object, and parted as the shore's foam is.
    if (gFD.x > 0.5)
    {
        float objFoam = FoamCut(foamN, 1.0 - objF * 0.75 + part * 1.2, foamAA) * saturate(objF * 3.0);
        foam = max(foam, objFoam * gBright.z * gFT.z * (depth > 0.0 ? 1.0 : 0.0));
    }
    // Before the wake (2026-10-05): after it the shader had no temporary registers left for the foam's age.
    if (gFoam.w > 8.5 && gFoam.w < 9.5)   // debug 9: the drawn foam (white) over its age (red), the open water's blue
        return float4(max(foam, saturate(ageS) * 0.6 * (ageS < 1.0 ? 1.0 : 0.0)), foam, max(foam, capsD), 1.0);

)HLSL"
    R"HLSL(
    // Drawn: the same blobs as the shore, more of them where the wake is stronger. foamAA, the edge width, from the
    // shore's foam above.
    if (gFD.x > 0.5)
        wakeFoam = FoamCut(foamN, 1.0 - saturate(wakeFoam * 1.4) * 0.8, foamAA) * saturate(wakeFoam * 4.0);
    else
        wakeFoam *= 0.4 + 0.6 * ftx;
    foam = max(foam, saturate(wakeFoam * gWake.y) * (depth > 0.05 ? 1.0 : 0.0));
    foam = onBody ? 0.0 : foam;
    caps *= 0.0;
    if (gFoam.w > 2.5 && gFoam.w < 3.5)
        clip(-1.0);   // debug 3: the wet sand pass shows alone
    if (gFoam.w > 1.5 && gFoam.w < 2.5)
        return float4(foam.xxx, 1.0);
    if (gFoam.w > 9.5 && gFoam.w < 10.5)
    {
        // debug 10 (Debug View 29), the depth on a doubling scale: from blue through cyan, green and yellow to red
        // over 0 to 256 yards, a dark line at 1, 2, 4, 8 ... 256 yards, hatched where the map's depth is used (mapK
        // over a half). Made while looking for far terrain under the sea (2026-10-06); kept as the owner liked it.
        float  dl2   = log2(1.0 + max(depth, 0.0));
        float  dk    = saturate(dl2 / 8.0);
        float3 ramp  = saturate(1.5 - abs(4.0 * dk - float3(3.0, 2.0, 1.0)));
        float  band  = depth > 0.5 && frac(log2(max(depth, 0.5)) + 0.03) < 0.06 ? 0.35 : 1.0;   // not 'line': HLSL's
        float  hatch = mapK > 0.5 && frac((vpos.x + vpos.y) / 8.0) < 0.5 ? 0.7 : 1.0;
        return depth > -0.05 ? float4(ramp * band * hatch, 1.0) : float4(0, 0, 0, 1);
    }
    if (gFoam.w > 4.5 && gFoam.w < 5.5)   // debug 5: the ripples' foam red, their waves green, the wake blue
        return float4(saturate(ring * gReach.y), saturate(length(ringSlope * gReach.y * 2.5) * 4.0),
                      saturate(wakeFoam * gWake.y + length(wakeSlope) * 4.0), 1.0);
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
    float  wv = smoothstep(0.0, 0.03, depthS) * gDeep.w * gWave.z;
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
    // The waves calm with distance, each train by its length: far out a pixel covers many short ones.
    float3 N   = WaveNormal(rel.xy + gCam.xy, t, gSky.w,
                            swS + shoreSlope + ringSlope * gReach.y * 2.5 + wakeSlope, dist);
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
    // The horizon (2026-10-03). In the last 2 degrees below it the water shows the sky, as real water does at a
    // glancing look, and the waves show in it as streaks. Without it the sea was one dark tone up to a hard
    // line under a lighter sky, and read as flat. Only that band: over the whole far sea the full reflection
    // turned it the pale fog colour, as the sand (2026-10-02, the 0.6 above).
    float  hz   = 1.0 - saturate(-dir.z / 0.035);
    hz *= hz;
    F = max(F, 0.7 * hz);
    float3 R    = reflect(dir, N);
    float3 sky  = lerp(gFogC.rgb, gSky.rgb, saturate(R.z * 2.5 + 0.35));
    // The glint of the sun, or by night of the larger moon, and of the other moon (2026-10-03). Glint Size
    // widens both: the powers fall with its square, so a size of 2 spreads the glint twice as wide.
    float  sd   = saturate(dot(R, gSun.xyz));
    float  sd2  = saturate(dot(R, gMoon2.xyz));
    float  gp1  = 700.0 * gGlint.x, gp2 = 60.0 * gGlint.x;
    float3 glint = gSunC.rgb * (gSun.w * (pow(sd, gp1) * 8.0 + pow(sd, gp2) * 0.25) +
                                gMoon2.w * (pow(sd2, gp1) * 8.0 + pow(sd2, gp2) * 0.25));
    // The lighthouse's glitter (2026-10-05, the owner): the waves catching its lamp, a broken path of light across
    // the water toward you, as the moon's. Narrower than the moon's, warm, and less the further the lamp.
    [branch] if (gLh.w > 0.0)
    {
        float3 toLh = gLh.xyz - rel;
        float  dl   = length(toLh);
        float  sl   = saturate(dot(R, toLh / max(dl, 1e-3)));
        // The beam sweeps it (the owner): bright where the beam points across this water, a trace elsewhere.
        float  bl   = length(gRain.zw);
        float2 away = -toLh.xy / max(length(toLh.xy), 1e-3);
        // The patch the beam lights follows the drawn beam's cone (Beam Width on Water, a share of its width). A
        // power over the beam's angle was here first: its tail lit three times the beam's width at its narrowest
        // (the owner: the slider did nothing).
        float2 bw     = gRain.zw / max(bl, 1e-3);
        float  along  = dot(bw, away);
        along = bl > 1.5 ? abs(along) : along;
        float  across = abs(bw.x * away.y - bw.y * away.x);
        float  off    = across / max(along, 1e-3);   // yards off the beam's line, a yard out
        float  hit    = bl > 0.5 ? (along > 0.0 ? 1.0 - smoothstep(0.3 * gLhT.w, gLhT.w, off) : 0.0) : 1.0;
        float  lhK  = gLh.w / (1.0 + dl / 150.0);
        glint += float3(1.0, 0.886, 0.659) * (lhK * (0.12 + 1.5 * hit) *
                                              (pow(sl, 900.0 * gGlint.x) * 7.0 + pow(sl, 90.0 * gGlint.x) * 0.15)
                                              + lhK * hit * gLhT.x * smoothstep(gLhT.y, gLhT.y + gLhT.z, dot(N.xy, -away)));
        // And the beam's light on the water as glitter of its own (the owner): the faces of the waves that tilt
        // toward the lighthouse catch it, their backs stay dark, wherever you stand, so the wedge the beam sweeps is
        // a moving pattern of lit wave faces. It was a flat 0.04, then 0.25 (a pale wedge that moved, but read as
        // paint); then a reflection toward the eye, which only brightened the glint on the line to you.
    }
    // Water Brightness (2026-10-03): the bed through the water and the sky in it, not the glint or the foam.
    float3 water = lerp(body, sky, F) * gBright.x;
    // The ripples lit as the swell is not: brighter on the side of a ring facing the sun, darker behind it, so
    // they show from any side and not only in the glint (2026-10-02).
    float2 sunXY = gSun.xy / max(length(gSun.xy), 0.2);
    // At most a fifth brighter (2026-10-02: at 0.3 the wake and the ripples glowed).
    water *= 1.0 + clamp(dot(ringSlope * gReach.y * 2.5 + wakeSlope, sunXY) * 2.5, -0.2, 0.18);
    water = lerp(water, gFogC.rgb, fogF);
    // And at the line itself it goes into the haze, as the far land does: no hard edge against the sky.
    water = lerp(water, gFogC.rgb, 0.5 * hz * hz);
    // The glint after the fog and the haze, and through at most half of the fog (2026-10-03). A low moon's
    // glint lies hundreds of yards out, past the fog's end (417 yards at night in Westfall), and the fog took
    // it while the moon itself still showed through the sky: the glint went out before the moon had set.
    water += glint * (1.0 - 0.5 * fogF);
    // The foam on it, lit as the water is: brighter on a slope toward the sun, darker on the back of a wave,
    // a little of the glint; and where it is thin, the water shows through it.
    float  lit   = 0.62 + 0.38 * saturate(dot(N, gSun.xyz) * 1.5) + 0.15 * F;
    float3 fcol  = c * lit + glint * 0.25;
    fcol = lerp(water + 0.12 * c, fcol, saturate(fa * 1.6));
    water = lerp(water, fcol, saturate(fa * 1.25));
    // debug 4 (Debug View 19): what lies under the water, bent and tinted, by where it comes from (2026-10-06):
    // the near world as it is, the far terrain's slice of the depth range red, nothing behind (cleared depth, the
    // sky) blue.
    if (gFoam.w > 3.5 && gFoam.w < 4.5)
    {
        float  rawU = tex2Dlod(sUnder, float4(ruv, 0, 0)).r;
        float  lumU = dot(bed, float3(0.299, 0.587, 0.114));
        float3 src  = rawU > 0.9999 ? float3(0.25, 0.45, 1.0)
                                    : (rawU > gZ.z + 1.0 / gZ.w - 1e-5 ? float3(1.0, 0.3, 0.25) : bed);
        return float4(rawU > gZ.z + 1.0 / gZ.w - 1e-5 ? src * (0.4 + 0.6 * saturate(lumU * 3.0)) : bed, 1.0);
    }
    // The water over the sand's foam and wet sand, mixed as colour times cover: mixed as plain colours, the foam's
    // white came in where our water fades in at the edge even with no foam there, a light line along the shore
    // (2026-10-02).
    float  outA = wv + a * (1.0 - wv);
    float3 outC = (water * wv + land * a * (1.0 - wv)) / max(outA, 1e-4);
    // The edge line ([water] edgeLine): a thin light line where the water meets the sand, in the first few
    // hundredths of a yard of depth, where our water fades in. It began as a fault in this mix (the foam's
    // colour mixed in where our water fades in) and was the best part of the shore, so it is kept on purpose,
    // at 1 exactly as it was (2026-10-02).
    // With the swash on too (2026-10-03): it starts at the moving edge (depthS), so this line rides it. The wet
    // sand pass drew its own line on the sand, and the water showed it through itself, tinted, as a bright cyan
    // line where the flat water ends. Its width is along the ground (Edge Line Width): as the first 0.03 yards of
    // depth it was wide on a gentle beach and a few centimetres on a steep bank.
    float  edgeK = saturate((1.0 - smoothstep(0.0, gGlint.w, depthS / slope)) * gFT.w);
    outC = lerp(outC, c, edgeK);
    float4 res = hideGame ? float4(lerp(lerp(lerp(under0, land, a), water, wv), c, edgeK), 1.0) : float4(outC, outA);
    // debug 1 (Debug View 16, 2026-10-06): the water as drawn, magenta where far terrain lies under it: the game's
    // far terrain slice (not the sky), or in the world's slice a bed over 150 yards from the camera (the water's
    // distance x the bed's view depth / the water's). The owner's target was a smooth untextured ridge far off that
    // View 19 did not show red, so in the world's slice. The water keeps its own cover.
    // Magenta for the far terrain slice. Orange for untextured terrain in the world's slice: the mask's green, the
    // bare draws (bodymask.cpp: no shader and no texture). By distance (a bed over 150 yards off) it took detailed
    // terrain that was only far off and the dock walls too (the owner).
    if (gFoam.w > 0.5 && gFoam.w < 1.5)
    {
        if (farSlice && raw <= 0.9999)
            res.rgb = float3(1.0, 0.0, 1.0);
        float2 maskU = gFog.w > 0.5 ? tex2Dlod(sLeaves, float4(ruv, 0, 0)).rg : 0.0;
        if (!farSlice && maskU.y > 0.5)
            res.rgb = float3(1.0, 0.55, 0.0);
        // Cyan: textured terrain (the ground bit) over 80 yards off, to tell it from the bare draws: a lane down the
        // middle of the owner's untextured ridge carried neither colour.
        if (!farSlice && maskU.x > 0.5 && dist * zg / zw > 80.0)
            res.rgb = float3(0.0, 0.9, 1.0);
    }
    return res;
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
float4 gFoamW : register(c211);    // as the water pass: 1 / foamWidth, 1 / foamReach, foamSpeed; the swash's
                                   // highest climb (yards of height)
float4 gSwashW : register(c212);   // the swash: 30 / its length along the shore (yards), its speed (radians a
                                   // second); z the share of the sea's swash a lake, a pond or a river gets (Lake
                                   // Swash); then
                                   // second), as the water pass's gSwash
sampler2D sFoamW : register(s12);  // the game's foam texture (WATERFOAMLOOP2.blp)
sampler2D sSceneW : register(s14); // the screen before this pass (when gFilm3.z is 1)
// The camera-relative point the depth under the water shows at a place on the screen.
float3 WetPointAt(float2 uvq)
{
    float  r   = tex2Dlod(sUnder, float4(uvq, 0, 0)).r;
    float  d   = min((r - gZ.x) * gZ.y, 0.99999);
    float2 ndc = float2(uvq.x * 2.0 - 1.0, 1.0 - uvq.y * 2.0);
    float4 wp  = ndc.x * gI0 + ndc.y * gI1 + d * gI2 + gI3;
    return wp.xyz / wp.w;
}
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
    float  lv  = -10000.0, seaW = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        float2 lc = tex2Dlod(sLevel, float4((cell + float2(x, y) + 0.5) * gLv.w, 0, 0)).rg;
        lv   = max(lv, lc.r);
        seaW = max(seaW, lc.g);   // the sea if any cell round it is (2026-10-05)
    }
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
    // Swash Length scales the stretches along the shore, Swash Speed the time (2026-10-05).
    float2 As    = A.xy * gSwashW.x;
    float  phase = sin(As.x * 0.21 + As.y * 0.17) * 1.7 + sin(As.x * 0.07 - As.y * 0.09) * 2.3;
    float  surge = 0.5 + 0.5 * sin(gZ.w * gSwashW.y + phase);
    surge = surge * surge;                                           // quick up the beach, slow back
    // From ground points 8 pixels either side, as in the water pass: the sand is flat triangles too, and from
    // the pixel beside its slope jumped at their edges (2026-10-03).
    float2 o8W   = gScr.xy * 8.0;
    float3 bXW   = WetPointAt(uv + float2(o8W.x, 0.0)) - WetPointAt(uv - float2(o8W.x, 0.0));
    float3 bYW   = WetPointAt(uv + float2(0.0, o8W.y)) - WetPointAt(uv - float2(0.0, o8W.y));
    float2 gxW   = bXW.xy, gyW = bYW.xy;
    float  exW   = bXW.z, eyW = bYW.z;
    float  detW  = gxW.x * gyW.y - gxW.y * gyW.x;
    float2 gh    = abs(detW) > 1e-8 ? float2(exW * gyW.y - gxW.y * eyW, gxW.x * eyW - gyW.x * exW) / detW : 0.0;
    float  slopeW = clamp(length(gh), 0.02, 2.0);
    // The run-up is along the ground (2026-10-03): up to gFilm2.w yards of it, its height capped at gFoamW.w.
    // As a height alone (0.06), on a steep bank the film climbed a few centimetres and lay as a thin bright
    // line at the flat waterline while the foam moved; on a gentle beach this is about as it was.
    // No higher than the wet sand (2026-10-05, the owner): half of its top, where it is fully wet, as the water pass.
    float  up    = min(min(gFilm2.w * slopeW, gFoamW.w), top * 0.5) * surge * lerp(gSwashW.z, 1.0, seaW);   // the edge's height above the water now
    // It stops at the waterline (2026-10-03; 0.05 below it until then). The water takes what lies under it from
    // the screen after this pass, so the film below the waterline showed through the first 0.05 yards of water,
    // tinted twice: a bright line fixed at the flat waterline, which never moved with the swash.
    float  film  = (1.0 - smoothstep(up - 0.015, up, h)) * smoothstep(-0.01, 0.0, h) * flat;
    film *= lv > -1000.0 ? 1.0 : 0.0;
    // The lip: the edge line, riding the film's edge. At rest it lies at the waterline, where the water pass's
    // own edge line used to be; that one is off while the swash is on, so there is one shoreline, not two.
    float  ft    = gFilm2.y > 0.5 ? tex2D(sFoamW, A.xy * gFilm2.x + float2(gZ.w * 0.01, 0.0)).a : 0.6;
    // A solid white line on the moving edge (2026-10-03: broken by the foam texture and at half the Edge Line
    // until then), about 0.12 yards wide along the ground whatever the slope.
    float  lipE  = exp(-pow((h - up) / (0.12 * slopeW + 0.003), 2.0)) * gFilm2.z * flat;
    lipE *= lv > -1000.0 ? 1.0 : 0.0;
    // The game's foam on the film: the water pass's shore foam, measured from the moving edge in the same way and
    // with the same texture at the same place and time, so one foam runs from the water up the sand and back.
    // Until 2026-10-03 this was a foam of its own just behind the edge, and the water's band stayed put at the
    // waterline: the foam looked fixed to the flat water while the edge moved.
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
    // The foam on the swash is the water pass's own since it draws the swash (2026-10-03); this one would show
    // twice, through that water and on it.
    lipE = max(lipE, sfoam * 0.0);
    // Two draws (2026-10-02). The first multiplies the sand: wet, and under the film the water's hue, as the
    // sand seen through thin water is in the water pass. Laid on as a colour, the film was darker than the clear
    // water beside it, a dark band inside the foam line. The second lays the foam on.
    if (gFilm3.y < 0.5)
        return float4((1.0 - wa).xxx, 1.0);                          // the wet sand, multiplied
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
        float3 sand = tex2Dlod(sSceneW, float4(uv, 0, 0)).rgb;
        float  lum = dot(sand, float3(0.299, 0.587, 0.114));
        // The water pass's thinnest water (2026-10-03): the bed, 0.7 of it in the water's hue at its own
        // brightness. It was lum x hue x 1.4, measured on the Westfall beach; on a steep bank in Redridge, once
        // the film ran up it, that glowed far brighter than the water beside it. At Swash 55 the film is whole.
        // No film colour since the water pass draws the swash itself (2026-10-03): two of them met at the flat
        // waterline as a second shoreline. Kept for a look back: fC is the water's colour at no depth.
        fA = 0.0;
        fC = lerp(sand, lum * hue, 0.7);
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
    // The drawn foam's blobs (2026-10-05), made here: see MakeFoamBody.
    IDirect3DTexture9*      g_foamBody = nullptr;
    bool                    g_foamBodyFailed = false;
    constexpr DWORD         kFoamBodySampler = 11;
    constexpr DWORD         kLeavesSampler = 10;
    IDirect3DTexture9*      g_leaves = nullptr;     // this frame's foliage mask (bodymask.cpp), not held

    IDirect3DVertexShader9* g_wetVs = nullptr;
    IDirect3DPixelShader9*  g_wetPs = nullptr;
    IDirect3DTexture9*      g_level = nullptr;     // the water level of each map cell round you
    IDirect3DStateBlock9*   g_wetSb = nullptr;
    IDirect3DIndexBuffer9*  g_gridIb = nullptr;   // all 8 x 8 cells of a water chunk (FullGridDraw)
    bool                    g_gridFailed = false;
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
    float              g_psc[90 * 4];           // this frame's pixel constants: c120 to c209

    // The pass's frame state (2026-10-06). Until then each chunk set all of c120 to c223 and our five textures
    // with their sampler states, and took the textures off again after its draws: about 50 calls a chunk, and
    // Stormwind's harbour draws some 400 chunks. These registers and samplers are the water's alone. The client's
    // shaders are ps_2_0 at most, which reads no register past c31 (its water shader reads c0, s0 and s1, the
    // probe's disassembly), and its fixed-function draws have 8 stages. No other pass of the DLL sets them while
    // the world is drawn: the other passes use c71 and s9 at the most, and those that run in the world (the body
    // mask's leaves, the wet sand) put every state back with a state block. So the frame's part is set once, at
    // its first chunk (SetFrameState), and holds through every run of water in the world, a building's after the
    // sea's too. Each chunk sets only its own registers (c167, c168, c210, c211), and only when the value changes
    // (SetChunkReg). The textures go at the world's end, before the body mask is built again into the texture
    // g_leaves is, and at Present for a world that never ended (UnbindFrame). Water is never drawn after the
    // world's end (WaterKind, comfyfog.cpp).
    struct ChunkReg { float v[4]; bool known; };   // what the pass last put in one of a chunk's own registers
    bool     g_frameSet = false;      // the frame's constants and textures are on the device
    unsigned g_boundSamplers = 0;     // our samplers with a texture on them, a bit each
    ChunkReg g_c167 = {}, g_c168 = {}, g_c210 = {}, g_c211 = {};

    // The ripples, in the world. A unit in the water starts one where it stands about once a second, and one
    // each time it has moved a yard and a half: walking leaves a trail. Until 2026-10-02 the rings were drawn
    // round where each unit stood that frame, and moved along with it.
    struct Ring
    {
        float  pos[3];
        double born;
        float  speed;   // yards a second the ring spreads at
        float  dir[2];  // the way its maker walked; 0 standing still
        float  shift;   // 0..1: shifts the noise that makes it uneven, its own for all its life
    };
    std::vector<Ring> g_rings;
    unsigned g_ringCount = 0;   // rings begun: each new one's shift follows from it

    // A building's water (2026-10-04): the canals of Stormwind are a WMO's liquid, which the map files do not
    // hold, so no swimmer there started a ripple or a wake. Each such draw gives its surface: the vertices its
    // triangles use, in the world, and their bounds. Only those: the draw's vertex range holds the unused
    // corners too, and its highest vertex lay 7.5 yards over Stormwind's canal. The ripples of a frame are
    // begun at its first water draw, before the buildings' water is drawn, so they read the last frame's.
    struct CityWater { float lo[3], hi[3]; size_t first, count; };
    std::vector<CityWater> g_cityNow, g_cityLast;
    std::vector<std::array<float, 3>> g_cityPtsNow, g_cityPtsLast;
    constexpr size_t kCityWaters = 64, kCityPts = 16384;

    // The height of a building's water at (x, y): its nearest used vertex within 4 yards (the liquid's grid is
    // 4.17 yards), among the draws whose bounds hold the point and reach from 3 yards under `z` to over it.
    bool CityWaterAt(float x, float y, float z, float& wz)
    {
        float best = 4.0f * 4.0f;
        bool found = false;
        for (const CityWater& w : g_cityLast)
        {
            if (x < w.lo[0] - 4.0f || x > w.hi[0] + 4.0f || y < w.lo[1] - 4.0f || y > w.hi[1] + 4.0f ||
                z > w.hi[2] || z < w.lo[2] - 3.0f)
                continue;
            for (size_t i = w.first; i < w.first + w.count; ++i)
            {
                const auto& q = g_cityPtsLast[i];
                const float d2 = (q[0] - x) * (q[0] - x) + (q[1] - y) * (q[1] - y);
                if (d2 < best)
                {
                    best = d2;
                    wz = q[2];
                    found = true;
                }
            }
        }
        return found;
    }

    // The wakes (2026-10-02): the recent path of each unit moving through the water. A unit is followed from
    // frame to frame by the trail whose newest point is nearest its place (no ids from the object list).
    struct TrailPoint { float pos[3]; double t; };
    struct Trail
    {
        std::vector<TrailPoint> pts;   // newest first; pts[0] follows the unit every frame
        double                  seen = 0.0;
        bool                    ship = false;   // a ship's (Ship Wake): its points further apart, kept longer
    };
    std::vector<Trail> g_trails;
    constexpr int   kTrails = 4, kTrailPts = 8;
    // 1 yard and 4 seconds since 2026-10-05 (0.6 and 2.5 until then): the drawn wake's bubbles stay along the path
    // and thin out over 3.5 s, and 8 points 0.6 yards apart ended them 4 yards behind a runner.
    constexpr float kTrailStep = 1.0f;     // yards between the points kept
    constexpr double kTrailLife = 4.0;     // seconds a point is kept
    // Ships (2026-10-06, the owner): a ship's hull is a building drawn fixed-function with a world matrix of its
    // own, camera-relative, its origin at the sea's level (Stormwind's harbour, two probes: 37 draws, about 38,000
    // triangles, at one origin; its lanterns moved 7 yards in a second). Each frame the distinct origins of the
    // fixed-function draws of 100 triangles or more without a pixel shader are noted (WaterNoteHull); at the frame's
    // end each is followed from the last frame's nearest, and one that moves over 1.5 yards a second at the water is
    // a ship, with a wake of its own. What the camera's reading adds to every origin is taken out: the median move
    // of all of them, when 5 or more are followed (the buildings that stand still are most of them).
    struct Hull
    {
        float  pos[3];
        float  vel[2];
        double seen;
        bool   ship;           // over 1.5 yards a second: its wake (Ship Wake)
        bool   moving = false; // over 0.4: a ship under way, whose parts the shadow cache follows (WaterShipNear)
    };
    constexpr int     kHullsMax = 128;
    float             g_hullRel[kHullsMax][3];   // this frame's origins, camera-relative, as drawn
    int               g_hullRelN = 0;
    std::vector<Hull> g_hulls;
    double            g_hullLast = 0.0;
    // The ships under way this frame, for the shadow cache (2026-10-06): their places, a short list, so the check
    // for each model it files costs nothing when no ship moves.
    std::vector<std::array<float, 3>> g_movingShips;
    float g_bedFade = 0.0f;   // the light the water takes from the bed a yard, by brightness (WaterBedFade)

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
    unsigned    g_pCount = 0, g_pFirst = 0, g_pLast = 0, g_pDetailed = 0, g_pOther = 0, g_pShore = 0;
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

    // A unit in the water: under a building's water drawn last frame, or as far as the map files can say,
    // false only where a tile is held and has neither a river nor the sea there. The shader still checks each
    // ripple against the surface it draws (a tile not loaded yet).
    bool MaybeInWater(const float p[3])
    {
        float wz = 0.0f, gz = 0.0f;
        if (CityWaterAt(p[0], p[1], p[2], wz) && p[2] < wz - 0.05f && p[2] > wz - 3.0f)
            return true;
        if (MapWaterHeight(p[0], p[1], wz))
            return p[2] < wz - 0.05f && p[2] > wz - 3.0f;
        return !MapGroundHeight(p[0], p[1], gz);
    }

    // The rain (2026-10-05): the game draws its rain as a few DrawPrimitive calls through a vertex shader, blended
    // as destination colour x source colour, writing no depth, with a 16 x 128 streak texture: about 43,000
    // triangles a frame at full rain (.wchange 1 1), none dry. Their triangles a frame, eased over about a second,
    // say how hard it rains. Found by comparing a probe's draw groups in rain and dry.
    unsigned g_rainPrims = 0;
    float    g_rain = 0.0f;
    double   g_rainLast = 0.0;

    // The parting's particles (2026-10-05, the owner): the drawn foam opens round each as a particle effect does.
    // Each body at the water that moves faster than a yard a second lets one go where it is, unless one let go
    // within half a yard less than 0.6 s ago; each lives kPartLife and the shader fades it. At the water: from 3
    // yards under its surface to 1 yard over it, so someone in the swash, on the wet sand over the flat water,
    // counts. Up to kParts at once (the shader's c213 to c222), the oldest dropped.
    constexpr int    kParts = 8;    // c213 to c220; c221 and c222 are the lighthouse's, c223 the rain's
    constexpr double kPartLife = 1.4;   // seconds, before each particle's own share (0.7 to 1.3 of it)
    // Each is let go a little off the body's feet, up to 0.35 yards any way, and lives its own share of kPartLife
    // (the owner, 2026-10-05): at the same place and life every time the trail was too even.
    struct PartParticle { float pos[3]; double born; double life; };
    unsigned g_partSeed = 12345u;
    float PartRandom()   // 0..1
    {
        g_partSeed = g_partSeed * 1664525u + 1013904223u;
        return (g_partSeed >> 8) / 16777216.0f;
    }
    std::vector<PartParticle> g_parts;
    float g_partOut[kParts * 4] = {};
    float g_partLast[64][3] = {};       // last frame's bodies at the water, for their speed
    int   g_partLastCount = 0;
    double g_partLastTime = 0.0;

    bool AtWater(const float p[3])
    {
        float wz = 0.0f;
        if (CityWaterAt(p[0], p[1], p[2], wz) || MapWaterHeight(p[0], p[1], wz))
            return p[2] < wz + 1.0f && p[2] > wz - 3.0f;
        return false;
    }

    void PartsUpdate(const float cam[3])
    {
        const double now = Now();
        for (size_t i = 0; i < g_parts.size();)
        {
            if (now - g_parts[i].born > g_parts[i].life)
                g_parts.erase(g_parts.begin() + i);
            else
                ++i;
        }
        const double dt = now - g_partLastTime;
        const bool steady = g_partLastTime > 0.0 && dt > 1e-3 && dt < 0.25;
        float units[256][3];
        const int n = ClientUnits(units, 256);
        float seen[64][3];
        int nSeen = 0;
        for (int u = 0; u < n && nSeen < 64; ++u)
        {
            const float* p = units[u];
            const float dx = p[0] - cam[0], dy = p[1] - cam[1];
            if (dx * dx + dy * dy > 50.0f * 50.0f || !AtWater(p))
                continue;
            memcpy(seen[nSeen++], p, sizeof(seen[0]));
            // Its speed: from last frame's nearest within a yard.
            float speed = 0.0f, bestD2 = 1.0f, way[2] = { 0.0f, 0.0f };
            for (int j = 0; j < g_partLastCount; ++j)
            {
                const float ex = g_partLast[j][0] - p[0], ey = g_partLast[j][1] - p[1];
                const float d2 = ex * ex + ey * ey;
                if (d2 < bestD2)
                {
                    bestD2 = d2;
                    speed = steady ? sqrtf(d2) / static_cast<float>(dt) : 0.0f;
                    const float l = sqrtf(d2);
                    way[0] = l > 1e-4f ? -ex / l : 0.0f;
                    way[1] = l > 1e-4f ? -ey / l : 0.0f;
                }
            }
            if (speed <= 1.0f)
                continue;
            // Let go a little ahead of the body (the owner): let go at the feet, a runner had passed a disc before it
            // was full, and the trail opened behind them; ahead by all a runner covers while it grows (2 yards) was
            // far too far. Half of that, up to 0.75 yards, with the disc growing over a tenth of its life.
            const double life = kPartLife * (0.7 + 0.6 * PartRandom());
            const float lead = (std::min)(0.75f, speed * static_cast<float>(life * 0.1) * 0.5f);
            const float at[2] = { p[0] + way[0] * lead, p[1] + way[1] * lead };
            // One each half yard walking, and further apart the faster (2026-10-05): no more than about 7 a second,
            // so the 11 the shader holds are not used up. Running let one go 14 times a second; the pool was full in
            // under a second and the oldest, still at a fifth of its size, was dropped: it popped out (the owner).
            const float gap = (std::max)(0.5f, speed / 7.0f);
            bool recent = false;
            for (const PartParticle& q : g_parts)
            {
                const float ex = q.pos[0] - at[0], ey = q.pos[1] - at[1];
                if (ex * ex + ey * ey < gap * gap && now - q.born < 0.6)
                {
                    recent = true;
                    break;
                }
            }
            if (recent)
                continue;
            // Full: the oldest makes room only when it is nearly gone, past 85% of its life; else this one waits.
            if (g_parts.size() >= static_cast<size_t>(kParts))
            {
                if (now - g_parts.front().born < g_parts.front().life * 0.85)
                    continue;
                g_parts.erase(g_parts.begin());
            }
            const float ang = PartRandom() * 6.2831853f, off = 0.35f * sqrtf(PartRandom());
            g_parts.push_back({ { at[0] + cosf(ang) * off, at[1] + sinf(ang) * off, p[2] }, now, life });
        }
        memcpy(g_partLast, seen, sizeof(seen[0]) * nSeen);
        g_partLastCount = nSeen;
        g_partLastTime = now;
        for (int i = 0; i < kParts; ++i)
        {
            float* o = g_partOut + i * 4;
            if (i < static_cast<int>(g_parts.size()))
            {
                const PartParticle& q = g_parts[i];
                o[0] = q.pos[0] - cam[0]; o[1] = q.pos[1] - cam[1]; o[2] = q.pos[2] - cam[2];
                o[3] = static_cast<float>((now - q.born) / q.life);
            }
            else
            {
                o[0] = o[1] = o[2] = 0.0f;
                o[3] = -1.0f;
            }
        }
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
                const bool walking = speed > 1.0f;
                // Standing Ripple Spread and Moving Ripple Spread (2026-10-05): how fast the ring grows, so how
                // large it is when it fades out after kRingLife. The owner wanted the rings round someone
                // standing still small.
                const float spread = (walking ? (std::max)(kRingStill, (std::min)(speed / 3.0f, 3.0f)) : kRingStill) *
                                     (walking ? g_cfg.water.rippleSpreadMoving : g_cfg.water.rippleSpread);
                const float shift = static_cast<float>(fmod(++g_ringCount * 0.618034, 1.0));
                g_rings.push_back({ { p[0], p[1], p[2] }, now, spread,
                                    { walking ? way[0] : 0.0f, walking ? way[1] : 0.0f }, shift });
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
                if (tr.ship)
                    continue;
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
        // The ships' wakes: the trail whose head is nearest each ship, or a new one. Size sc: the points sc x 0.67
        // yards apart, so 8 reach the 4 x sc yards the wake fades over, and kept sc x 2.5 seconds.
        const float sc = g_cfg.water.shipWake;
        if (sc > 0.0f)
        {
            for (const Hull& h : g_hulls)
            {
                const float dx = h.pos[0] - cam[0], dy = h.pos[1] - cam[1];
                if (!h.ship || h.seen < now - 0.25 || dx * dx + dy * dy > 300.0f * 300.0f)
                    continue;
                // Ship Wake Forward (2026-10-06, the owner: the wake was only at the back): the trail follows a
                // point that many yards ahead of the hull's origin, along the way it moves.
                const float vl = sqrtf(h.vel[0] * h.vel[0] + h.vel[1] * h.vel[1]);
                const float fw = g_cfg.water.shipWakeForward / (std::max)(vl, 1e-3f);
                const float hp[3] = { h.pos[0] + h.vel[0] * fw, h.pos[1] + h.vel[1] * fw, h.pos[2] };
                Trail* best = nullptr;
                float bestD = 4.0f * 4.0f;
                for (Trail& tr : g_trails)
                {
                    const float ex = tr.pts[0].pos[0] - hp[0], ey = tr.pts[0].pos[1] - hp[1];
                    const float d2 = ex * ex + ey * ey;
                    if (tr.ship && d2 < bestD && tr.seen < now)
                    {
                        bestD = d2;
                        best = &tr;
                    }
                }
                if (!best)
                {
                    Trail tr;
                    tr.ship = true;
                    tr.pts.push_back({ { hp[0], hp[1], hp[2] }, now });
                    tr.pts.push_back({ { hp[0], hp[1], hp[2] }, now });
                    tr.seen = now;
                    g_trails.insert(g_trails.begin(), tr);
                    continue;
                }
                best->seen = now;
                best->pts[0] = { { hp[0], hp[1], hp[2] }, now };
                const float* q = best->pts[1].pos;
                const float sx = hp[0] - q[0], sy = hp[1] - q[1], step = sc * 0.67f;
                if (sx * sx + sy * sy > step * step)
                {
                    best->pts.insert(best->pts.begin() + 1, best->pts[0]);
                    if (best->pts.size() > static_cast<size_t>(kTrailPts))
                        best->pts.pop_back();
                }
            }
        }
        for (size_t i = 0; i < g_trails.size();)
        {
            Trail& tr = g_trails[i];
            const double life = tr.ship ? (std::max)(sc, 1.0f) * 2.5 : kTrailLife;
            while (tr.pts.size() > 2 && now - tr.pts.back().t > life)
                tr.pts.pop_back();
            if (now - tr.seen > (tr.ship ? 1.0 : kTrailLife) || (tr.ship && sc <= 0.0f))
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
                // The depth of its waves: a ring whose maker walked or swam has a way (2026-10-05).
                const bool moved = r.dir[0] != 0.0f || r.dir[1] != 0.0f;
                w[0] = r.dir[0]; w[1] = r.dir[1]; w[2] = r.shift;
                w[3] = moved ? g_cfg.water.rippleDepthMoving : g_cfg.water.rippleDepth;
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
        // c205 to c208: inverse(view x projection), to rebuild the bed at another pixel (BedAt).
        {
            D3DMATRIX vp, inv;
            Mul(*c.view, *c.proj, vp);
            if (Invert(vp, inv))
                memcpy(k + 340, &inv, 64);
        }
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
        PartsUpdate(cam);
        // The wakes (c170 to c201): the 4 trails with the most points that are nearest, 8 points each.
        {
            float* o = k + 200;
            for (int i = 0; i < kTrails * kTrailPts * 4; ++i)
                o[i] = 0.0f;
            for (int i = 0; i < kTrails * kTrailPts; ++i)
                o[i * 4 + 3] = -1.0f;
            int used = 0;
            const double now = Now();
            const float sc = g_cfg.water.shipWake;
            for (int pass = 0; pass < 2; ++pass)   // the ships' first, then the units'
            for (const Trail& tr : g_trails)
            {
                if (used >= kTrails)
                    break;
                if (tr.ship != (pass == 0))
                    continue;
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
                    pt[3] = tr.ship && j == 0 ? -(std::max)(sc, 2.0f) : static_cast<float>(now - tr.pts[j].t);
                    if (tr.ship && j == 0)
                        pt[2] = g_cfg.water.shipWakeDepth;   // a ship's head carries its depth (see the shader)
                }
                ++used;
            }
            k[328] = static_cast<float>(used);
            k[329] = w.wake;
            k[330] = w.swash > 0.0f ? 1.0f : 0.0f;
            k[331] = w.swashHeight * 5.0f;   // the run along the ground: 5 times the height (Swash Height)
            if (g_probeOn)
            {
                Log("water: wakes: %d trails held, %d drawn", static_cast<int>(g_trails.size()), used);
                Log("water: ripples: %d held; %d draws of a building's water last frame",
                    static_cast<int>(g_rings.size()), static_cast<int>(g_cityLast.size()));
                for (const CityWater& w : g_cityLast)
                    Log("    a building's water: %u vertices used, height %.2f to %.2f, x %.1f to %.1f, y %.1f to %.1f",
                        static_cast<unsigned>(w.count), w.lo[2], w.hi[2], w.lo[0], w.hi[0], w.lo[1], w.hi[1]);
                float pl[3], wz = 0.0f;
                if (ClientPlayer(pl))
                {
                    if (CityWaterAt(pl[0], pl[1], pl[2], wz))
                        Log("water: a building's water at %.2f under you, at %.2f", wz, pl[2]);
                    else
                        Log("water: no building's water within 4 yards of you, at %.2f", pl[2]);
                }
            }
        }
        k[35] = static_cast<float>((std::min)(static_cast<int>(g_rings.size()), kRings));

        // The surface (c162 to c167). By night the sky, the water and the sun's light dim.
        const float day = 1.0f - 0.8f * night;
        float sun[3] = { 0.0f, 0.0f, 1.0f };
        const bool haveSun = SunDirection(sun);
        // A disc low in the sky (2026-10-03): its glint went out as its centre crossed the horizon, with half
        // of it still up, since no water can bounce the view down below the horizon. While any of it is up
        // (about 4 degrees of radius), the glint aims at the horizon, 1 degree up, and fades as the top sinks.
        auto lowDisc = [](float d[3]) -> float {
            const float kRadius = 0.0698f;   // sin 4 degrees
            const float kAim    = 0.0175f;   // sin 1 degree
            const float z = d[2];
            const float t = (z + kRadius) / kRadius;
            const float vis = t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t * t * (3.0f - 2.0f * t));
            if (z < kAim)
            {
                const float h = sqrtf(d[0] * d[0] + d[1] * d[1]);
                const float c = sqrtf(1.0f - kAim * kAim);
                d[0] = h > 1e-4f ? d[0] / h * c : c;
                d[1] = h > 1e-4f ? d[1] / h * c : 0.0f;
                d[2] = kAim;
            }
            return vis;
        };
        const float sunVis = lowDisc(sun);
        k[168] = sun[0]; k[169] = sun[1]; k[170] = sun[2];
        // By night the light is the larger moon (sun.cpp), with a glint of its own strength (Moon Glint).
        const bool moonUp = night > 0.5f;
        k[171] = haveSun ? (moonUp ? w.moonGlint : w.glint) * sunVis : 0.0f;
        // The other moon (c203): smaller in the sky (a quad of 1.0 against 1.8), so a smaller glint.
        float moon2[3] = { 0.0f, 0.0f, 1.0f };
        const bool second = moonUp && SunSecondDirection(moon2);
        const float moon2Vis = lowDisc(moon2);
        k[332] = moon2[0]; k[333] = moon2[1]; k[334] = moon2[2];
        k[335] = second ? w.moonGlint * 0.6f * moon2Vis : 0.0f;
        k[336] = 1.0f / (w.glintSize * w.glintSize);
        k[337] = w.foamEdge;      // c204.y: the drawn foam's cut at the water's edge
        k[338] = w.swashHeight;
        k[339] = w.edgeWidth;
        k[356] = w.brightness;   // c209
        k[357] = w.wakeFoam;     // c209.y
        k[358] = w.objectFoam;   // c209.z
        k[359] = w.objectFoamWidth;   // c209.w
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
        // For the sun shadows' shade on the bed (2026-10-06): the fade of the channel the water lets through best, the
        // one that keeps the bed in view, so the shade stays in view as long as the bed does. Their own 0.25 a yard /
        // clarity took it some 2.5 times sooner; weighted by brightness it was still 3 times sooner at Water Colour 25
        // (red, absorbed fastest, counts most): a moored ship's shade on the harbour floor, 45 yards of water off,
        // kept 26% where the floor kept 64% (the owner).
        g_bedFade = (std::min)((std::min)(k[176], k[177]), k[178]);
        k[179] = w.refraction;
        k[180] = ((w.skyColor >> 16) & 0xFF) / 255.0f * day;
        k[181] = ((w.skyColor >> 8) & 0xFF) / 255.0f * day;
        k[182] = (w.skyColor & 0xFF) / 255.0f * day;
        k[183] = w.waves;
        // The moon's colour is the Moonlight Colour's hue ([night] moonColor), at 0.75 in its brightest channel.
        const DWORD mc = g_cfg.night.moonColor;
        const float mrgb[3] = { ((mc >> 16) & 0xFF) / 255.0f, ((mc >> 8) & 0xFF) / 255.0f, (mc & 0xFF) / 255.0f };
        const float mmax = (std::max)((std::max)(mrgb[0], mrgb[1]), (std::max)(mrgb[2], 1e-3f));
        const float sunC[3] = { 1.0f, 0.92f, 0.78f };
        for (int i = 0; i < 3; ++i)
            k[184 + i] = moonUp ? mrgb[i] / mmax * 0.75f : sunC[i];
        k[187] = w.whitecaps;
        k[188] = 1.0f / w.waveScale;
        k[191] = w.reflection;
        k[190] = 0.0f;   // the screen copy: set when it is made (WaterBeforeDraw)
        k[192] = w.waveHeight * 0.25f;
        k[196] = g_foamTex ? 1.0f : 0.0f;
        k[197] = 1.0f / w.shoreFoamSize;
        k[198] = w.shoreFoam;
        k[199] = w.edgeLine;   // at the water's moving edge, with the swash on or off
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

namespace
{
    // A shore chunk (fewer than the 128 triangles of a full grid): every point of its vertex buffer, and the
    // cells its triangles cover. Whether the points of the dry cells carry the water's level (2026-10-03, the
    // swash cut off where the game's mesh ends).
    void ProbeShore(IDirect3DDevice9* dev, const WaterChunk& c, bool full)
    {
        auto* d = dev->lpVtbl;
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9*  ib = nullptr;
        UINT off = 0, stride = 0;
        if (FAILED(d->GetStreamSource(dev, 0, &vb, &off, &stride)) || !vb || stride < 40 ||
            FAILED(d->GetIndices(dev, &ib)) || !ib)
        {
            if (vb) vb->lpVtbl->Release(vb);
            if (ib) ib->lpVtbl->Release(ib);
            Log("water shore chunk: buffers not readable");
            return;
        }
        D3DINDEXBUFFER_DESC idesc = {};
        ib->lpVtbl->GetDesc(ib, &idesc);
        const UINT isz = idesc.Format == D3DFMT_INDEX32 ? 4 : 2;
        // The game draws a strip (prim 5): primCount + 2 indices.
        std::vector<uint32_t> idx(c.prim == D3DPT_TRIANGLESTRIP ? c.primCount + 2 : c.primCount * 3);
        void* ip = nullptr;
        if (SUCCEEDED(ib->lpVtbl->Lock(ib, c.startIndex * isz, static_cast<UINT>(idx.size()) * isz, &ip,
                                       D3DLOCK_READONLY)) && ip)
        {
            for (size_t i = 0; i < idx.size(); ++i)
            {
                if (isz == 4) memcpy(&idx[i], static_cast<const uint8_t*>(ip) + i * 4, 4);
                else { uint16_t v; memcpy(&v, static_cast<const uint8_t*>(ip) + i * 2, 2); idx[i] = v; }
            }
            ib->lpVtbl->Unlock(ib);
        }
        else
            idx.clear();
        const UINT n = c.numVertices;
        std::vector<float> f(n * 10);
        void* vp = nullptr;
        const UINT first = static_cast<UINT>(c.baseVertex + static_cast<INT>(c.minIndex));
        const bool vok = SUCCEEDED(vb->lpVtbl->Lock(vb, off + first * stride, n * stride, &vp, D3DLOCK_READONLY)) && vp;
        if (vok)
        {
            for (UINT i = 0; i < n; ++i)
                memcpy(&f[i * 10], static_cast<const uint8_t*>(vp) + i * stride, 40);
            vb->lpVtbl->Unlock(vb);
        }
        vb->lpVtbl->Release(vb);
        ib->lpVtbl->Release(ib);
        float cam[3] = {};
        ClientCamera(cam);
        Log("water shore chunk: prim %d, %u vertices from %u (base %d min %u), %u triangles from index %u, %u-byte "
            "indices, world translation (%.1f %.1f %.1f), the camera (%.1f %.1f %.1f)", static_cast<int>(c.prim), n,
            first, c.baseVertex, c.minIndex, c.primCount, c.startIndex, isz, c.world->m[3][0], c.world->m[3][1],
            c.world->m[3][2], cam[0], cam[1], cam[2]);
        if (!vok)
            return;
        // Each point's height, apart for the points of drawn cells and the rest.
        std::vector<bool> wetV(n, false);
        const bool strip = c.prim == D3DPT_TRIANGLESTRIP;
        for (size_t t = 0; t + 2 < idx.size(); t += strip ? 1 : 3)
        {
            const uint32_t a = idx[t], b = idx[t + 1], e = idx[t + 2];
            if (a == b || b == e || a == e)
                continue;
            for (uint32_t v : { a, b, e })
                if (v >= c.minIndex && v - c.minIndex < n)
                    wetV[v - c.minIndex] = true;
        }
        float wz0 = 1e9f, wz1 = -1e9f, dz0 = 1e9f, dz1 = -1e9f;
        int nw = 0;
        for (UINT i = 0; i < n; ++i)
        {
            const float z = f[i * 10 + 2];
            if (wetV[i]) { wz0 = (std::min)(wz0, z); wz1 = (std::max)(wz1, z); ++nw; }
            else         { dz0 = (std::min)(dz0, z); dz1 = (std::max)(dz1, z); }
        }
        std::string map;
        for (UINT r = 0; r < 9 && n == 81; ++r)
        {
            map += r ? "/" : "";
            for (UINT k = 0; k < 9; ++k)
                map += wetV[r * 9 + k] ? "#" : ".";
        }
        float gz = 0.0f, mz = 0.0f;
        const float cx = c.world->m[3][0] - 16.7f + cam[0], cy = c.world->m[3][1] - 16.7f + cam[1];
        const bool hasW = MapWaterHeight(cx, cy, mz), hasG = MapGroundHeight(cx, cy, gz);
        Log("    %d points of drawn cells, heights %.3f..%.3f (in the world %.3f..%.3f); the others %.3f..%.3f; "
            "the map at its middle: water %s %.2f, ground %s %.2f; points %s", nw, wz0, wz1, wz0 + c.world->m[3][2]
            + cam[2], wz1 + c.world->m[3][2] + cam[2], dz0, dz1, hasW ? "at" : "none", mz, hasG ? "at" : "unknown", gz,
            map.c_str());
        if (!full)
            return;
        for (UINT i = 0; i < n; ++i)
            Log("    v%2u: (%.2f %.2f %.3f) n (%.2f %.2f %.2f) uv0 (%.4f %.4f) uv1 (%.4f %.4f)", i, f[i * 10],
                f[i * 10 + 1], f[i * 10 + 2], f[i * 10 + 3], f[i * 10 + 4], f[i * 10 + 5], f[i * 10 + 6],
                f[i * 10 + 7], f[i * 10 + 8], f[i * 10 + 9]);
        std::string tri;
        for (uint32_t v : idx)
            tri += " " + std::to_string(v - c.minIndex);
        Log("    indices (vertex numbers from the first):%s", tri.c_str());
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
            // The level in r, and 1 in g where it is the sea (2026-10-05).
            if (FAILED(dev->lpVtbl->CreateTexture(dev, kLevelCells, kLevelCells, 1, 0, D3DFMT_G32R32F, D3DPOOL_MANAGED,
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
                const float wx = (cx + x + 0.5f) * kCell, wy = (cy + y + 0.5f) * kCell;
                row[x * 2]     = MapWaterHeight(wx, wy, z) ? z : -10000.0f;
                row[x * 2 + 1] = MapWaterIsSea(wx, wy) ? 1.0f : 0.0f;
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
        float k[13 * 4] = {};
        memcpy(k, &inv, 64);
        k[16] = g_psc[2]; k[17] = g_psc[3]; k[18] = w.wetSand; k[19] = g_psc[10];
        k[20] = g_psc[8]; k[21] = g_psc[9]; k[22] = static_cast<float>(w.debug);
        k[24] = g_levelX * kCell; k[25] = g_levelY * kCell; k[26] = 1.0f / kCell; k[27] = 1.0f / kLevelCells;
        k[28] = g_psc[28]; k[29] = g_psc[29]; k[30] = g_psc[30];
        // The swash (c208, c209): the water's colour, the film's strength; the foam texture and the run-up.
        k[32] = g_psc[172]; k[33] = g_psc[173]; k[34] = g_psc[174]; k[35] = w.swash;
        k[36] = 1.0f / w.shoreFoamSize; k[37] = g_foamTex ? 1.0f : 0.0f; k[39] = w.swashHeight * 5.0f;
        k[38] = 0.0f;    // no lip here: the water pass draws the edge line at its own moving edge (2026-10-03)
        k[40] = w.shoreFoam * w.foam;
        k[41] = 0.0f;   // the draw: 0 multiplies the sand, 1 lays the foam on
        k[44] = 1.0f / w.foamWidth; k[45] = 1.0f / w.foamReach; k[46] = w.foamSpeed; k[47] = w.swashHeight;
        k[48] = 30.0f / w.swashLength; k[49] = 0.55f * w.swashSpeed;   // c212: as the water pass's c211
        k[50] = w.lakeSwash;   // c212.z

        auto* d = dev->lpVtbl;
        IDirect3DVertexShader9* oldVs = nullptr;
        d->GetVertexShader(dev, &oldVs);
        g_wetSb->lpVtbl->Capture(g_wetSb);
        d->SetVertexShader(dev, g_wetVs);
        d->SetPixelShader(dev, g_wetPs);
        d->SetPixelShaderConstantF(dev, 200, k, 13);
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
            // Where no body or model is marked (either bit of the mask).
            d->SetRenderState(dev, D3DRS_STENCILENABLE, TRUE);
            d->SetRenderState(dev, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
            d->SetRenderState(dev, D3DRS_STENCILREF, 0);
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
    // The drawn foam's blobs (2026-10-05): 256 x 256, tiling, with every mip level. Cellular noise of two sizes,
    // 8 and 21 cells across, wrapped at the texture's edge so it tiles: each pixel is 1 at a cell's point and
    // falls to 0 a little over half a cell away, so the blobs are round. The shader reads it twice at other sizes
    // and angles and warps it, and cuts it at a level that rises with the foam's age.
    float CellsWrapped(float u, float v, int period, unsigned seed)
    {
        const float px = u * period, py = v * period;
        const int ix = static_cast<int>(floorf(px)), iy = static_cast<int>(floorf(py));
        float best = 8.0f;
        for (int gy = -1; gy <= 1; ++gy)
            for (int gx = -1; gx <= 1; ++gx)
            {
                const int cx = ix + gx, cy = iy + gy;
                const unsigned wx = static_cast<unsigned>((cx % period + period) % period);
                const unsigned wy = static_cast<unsigned>((cy % period + period) % period);
                unsigned h = wx * 374761393u + wy * 668265263u + seed * 2246822519u;
                h = (h ^ (h >> 13)) * 1274126177u;
                const float jx = 0.1f + 0.8f * ((h & 0xFFFF) / 65535.0f);
                const float jy = 0.1f + 0.8f * (((h >> 16) & 0xFFFF) / 65535.0f);
                const float dx = cx + jx - px, dy = cy + jy - py;
                best = (std::min)(best, dx * dx + dy * dy);
            }
        return sqrtf(best);
    }

    void MakeFoamBody(IDirect3DDevice9* dev)
    {
        if (g_foamBody || g_foamBodyFailed)
            return;
        constexpr UINT kSize = 256, kLevels = 9;
        if (FAILED(dev->lpVtbl->CreateTexture(dev, kSize, kSize, kLevels, 0, D3DFMT_L8, D3DPOOL_MANAGED, &g_foamBody,
                                              nullptr)))
        {
            g_foamBodyFailed = true;
            g_foamBody = nullptr;
            Log("water: the drawn foam's texture could not be made: no drawn foam");
            return;
        }
        std::vector<uint8_t> img(kSize * kSize);
        for (UINT y = 0; y < kSize; ++y)
            for (UINT x = 0; x < kSize; ++x)
            {
                const float u = (x + 0.5f) / kSize, v = (y + 0.5f) / kSize;
                const float big   = 1.0f - (std::min)(1.0f, CellsWrapped(u, v, 8, 1) * 1.3f);
                const float fine  = 1.0f - (std::min)(1.0f, CellsWrapped(u, v, 21, 2) * 1.3f);
                const float val = (std::min)(1.0f, big * 0.68f + fine * 0.32f);
                img[y * kSize + x] = static_cast<uint8_t>(val * 255.0f + 0.5f);
            }
        UINT w = kSize;
        for (UINT level = 0; level < kLevels; ++level)
        {
            D3DLOCKED_RECT lr;
            if (FAILED(g_foamBody->lpVtbl->LockRect(g_foamBody, level, &lr, nullptr, 0)))
                break;
            for (UINT row = 0; row < w; ++row)
                memcpy(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch, &img[row * w], w);
            g_foamBody->lpVtbl->UnlockRect(g_foamBody, level);
            if (w == 1)
                break;
            // The next level: each pixel the mean of four.
            const UINT h = w / 2;
            std::vector<uint8_t> next(h * h);
            for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < h; ++x)
                    next[y * h + x] = static_cast<uint8_t>((img[(2 * y) * w + 2 * x] + img[(2 * y) * w + 2 * x + 1] +
                                                            img[(2 * y + 1) * w + 2 * x] + img[(2 * y + 1) * w + 2 * x + 1] + 2) / 4);
            img.swap(next);
            w = h;
        }
        Log("water: the drawn foam's texture is made, %ux%u, %u levels", kSize, kSize, kLevels);
    }

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

    // Our textures off the samplers, and the frame's constants to be set again at the next chunk (see g_frameSet).
    void UnbindFrame(IDirect3DDevice9* dev)
    {
        for (DWORD s = 0; s < 16; ++s)
            if (g_boundSamplers & (1u << s))
                dev->lpVtbl->SetTexture(dev, s, nullptr);
        g_boundSamplers = 0;
        g_frameSet = false;
    }
}

namespace
{
    bool g_spanOpen = false;   // kBenchWaterSpan begun this frame
}

void WaterBeforeDraw(IDirect3DDevice9* dev, const WaterChunk& c)
{
    if (g_copied || !WaterWanted())
        return;
    g_copied = true;
    // The water's GPU time (2026-10-06): its copies and the wet sand here, and the span from here to the world's
    // end, with our draws over each chunk in it (and the game's own, which cannot be taken apart from them).
    BenchSectionBegin(dev, kBenchWaterSpan);
    g_spanOpen = true;
    BenchSectionBegin(dev, kBenchWaterPrep);
    struct PrepEnd
    {
        IDirect3DDevice9* dev;
        ~PrepEnd() { BenchSectionEnd(dev, kBenchWaterPrep, true); }
    } prepEnd = { dev };
    // Nothing of ours is bound while the copies below are made into our textures, and the constants made below
    // go up at the next chunk (2026-10-06).
    UnbindFrame(dev);
    g_copyOk = EnsureShaders(dev) && CopyUnder(dev);
    if (!g_copyOk)
        return;
    EnsureFoamTexture(dev);
    MakeFoamBody(dev);
    // The foliage drawn so far, for the foam round objects (2026-10-05): built now, at the first water draw.
    // Whenever the foam is drawn: the shore's foam reads the ground in it too.
    g_leaves = g_cfg.water.foamDrawn ? BodyMaskLeavesNow(dev) : nullptr;
    // Without the mask the ground cannot be told from an object: no foam round objects then (below, gFog.w).
    FrameConstants(c);
    g_psc[27] = g_leaves ? 1.0f : 0.0f;   // c126.w
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
        // The sea or a lake at your feet (2026-10-05), for Lake Swash and Lake Foam.
        float pl[3], wz = 0.0f;
        if (ClientPlayer(pl))
            Log("water: at your feet (%.1f %.1f) the map has %s; Lake Swash %.2f, Lake Foam %.2f", pl[0], pl[1],
                !MapWaterHeight(pl[0], pl[1], wz) ? "no water" : MapWaterIsSea(pl[0], pl[1]) ? "the sea" : "a lake, a pond or a river",
                g_cfg.water.lakeSwash, g_cfg.water.lakeFoam);
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

namespace
{
    const float kAllWet[4] = { 65535.0f, 65535.0f, 65535.0f, 65535.0f };
    constexpr UINT kFrameRegs = 104;   // c120 to c223, the last register ps_3_0 has

    // One of a chunk's own pixel registers: set only when it differs from what the pass last put there. Valid
    // while g_frameSet holds: SetFrameState writes every one of them and notes what it wrote.
    void SetChunkReg(IDirect3DDevice9* dev, UINT reg, ChunkReg& last, const float v[4])
    {
        if (last.known && memcmp(last.v, v, sizeof(last.v)) == 0)
            return;
        dev->lpVtbl->SetPixelShaderConstantF(dev, reg, v, 1);
        memcpy(last.v, v, sizeof(last.v));
        last.known = true;
    }

    void BindSampler(IDirect3DDevice9* dev, DWORD s, IDirect3DTexture9* tex, D3DTEXTUREFILTERTYPE filter,
                     D3DTEXTUREFILTERTYPE mip, D3DTEXTUREADDRESS address)
    {
        auto* d = dev->lpVtbl;
        d->SetTexture(dev, s, reinterpret_cast<IDirect3DBaseTexture9*>(tex));
        d->SetSamplerState(dev, s, D3DSAMP_MINFILTER, filter);
        d->SetSamplerState(dev, s, D3DSAMP_MAGFILTER, filter);
        d->SetSamplerState(dev, s, D3DSAMP_MIPFILTER, mip);
        d->SetSamplerState(dev, s, D3DSAMP_ADDRESSU, address);
        d->SetSamplerState(dev, s, D3DSAMP_ADDRESSV, address);
        d->SetSamplerState(dev, s, D3DSAMP_SRGBTEXTURE, FALSE);
        g_boundSamplers |= 1u << s;
    }

    // The frame's part of the water pass, at its first chunk (see g_frameSet): c120 to c223 in one call, with each
    // chunk's own registers at the sea's values, and our textures with their sampler states. After the wet sand
    // pass, whose state block puts back what it changes (c200 to c212, s12 to s15).
    void SetFrameState(IDirect3DDevice9* dev)
    {
        const WaterSettings& w = g_cfg.water;
        float k[kFrameRegs * 4];
        memcpy(k, g_psc, sizeof(g_psc));                    // c120 to c209 (c167 and c168 the chunk's, below)
        memcpy(k + 90 * 4, kAllWet, sizeof(kAllWet));      // c210: FullGridDraw sets a shore chunk's and puts this back
        // c211, the swash's length along the shore and its speed (2026-10-05): as the wet sand pass's c212. And the
        // share of the shore foam and of the swash, 1 on the sea; a lake's chunk sets its own.
        const float sw[4] = { 30.0f / w.swashLength, 0.55f * w.swashSpeed, 1.0f, 1.0f };
        memcpy(k + 91 * 4, sw, sizeof(sw));
        // c212, the drawn foam (2026-10-05): on, 1 / its size, 1 / its reach, the open water's foam.
        const float fd[4] = { w.foamDrawn && g_foamBody ? 1.0f : 0.0f, 1.0f / w.foamCell, 1.0f / w.foamLife,
                              w.openFoam ? w.whitecaps : 0.0f };
        memcpy(k + 92 * 4, fd, sizeof(fd));
        static_assert(kParts == 8, "the parting's particles are c213 to c220");
        memcpy(k + 93 * 4, g_partOut, sizeof(g_partOut));   // c213 to c220, the parting's particles
        // c221, the lighthouse's light on the water, tuned on the Lamps tab (2026-10-05).
        const LighthouseSettings& ls = g_cfg.lighthouse;
        const float lt[4] = { ls.faceStrength, ls.faceTilt, (std::max)(ls.faceSoft, 0.001f),
                              (std::max)(ls.beamSpread * ls.waterWidth, 0.002f) };
        memcpy(k + 101 * 4, lt, sizeof(lt));
        // c222, the nearest lighthouse's lamp, for its glitter on the water (2026-10-05). The lamps are found at the
        // lighthouse pass, after the world: the same for every chunk.
        float lh[1][3];
        float lc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (BeaconLamps(lh, 1) == 1)
        {
            lc[0] = lh[0][0] - g_psc[28]; lc[1] = lh[0][1] - g_psc[29]; lc[2] = lh[0][2] - g_psc[30];
            lc[3] = BeaconGlint();
        }
        memcpy(k + 102 * 4, lc, sizeof(lc));
        // c223, the rain (eased at Present) and the beam's way. One angle for the whole frame: it was read again
        // for each chunk, a few microseconds apart.
        float way[2] = { 0.0f, 0.0f };
        bool two = false;
        const bool beam = BeaconBeamWay(way, two);
        const float wl = beam ? (two ? 2.0f : 1.0f) : 0.0f;
        const float rain[4] = { g_rain, w.rain, way[0] * wl, way[1] * wl };
        memcpy(k + 103 * 4, rain, sizeof(rain));
        dev->lpVtbl->SetPixelShaderConstantF(dev, kPsReg, k, kFrameRegs);
        auto note = [&](ChunkReg& r, UINT reg) { memcpy(r.v, k + (reg - kPsReg) * 4, sizeof(r.v)); r.known = true; };
        note(g_c167, kPsReg + 47);
        note(g_c168, kPsReg + 48);
        note(g_c210, kPsReg + 90);
        note(g_c211, kPsReg + 91);

        BindSampler(dev, kUnderSampler, g_under, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP);
        if (g_foamTex)
            BindSampler(dev, kFoamSampler, g_foamTex, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTADDRESS_WRAP);
        if (g_leaves)
            BindSampler(dev, kLeavesSampler, g_leaves, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP);
        if (g_foamBody)
            BindSampler(dev, kFoamBodySampler, g_foamBody, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTADDRESS_WRAP);
        if (g_sceneOk)
            BindSampler(dev, kSceneSampler, g_scene, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTADDRESS_CLAMP);
        g_frameSet = true;
    }

    // A building's water draw: its surface for the next frame's ripples and wakes (MaybeInWater). Its indices
    // and vertices are read each frame, as the land's indices are (WetCells): a building's water is a few
    // draws of some hundreds of vertices.
    void NoteCityWater(IDirect3DDevice9* dev, const WaterChunk& c)
    {
        if (g_cityNow.size() >= kCityWaters || !c.numVertices || c.numVertices > 4096 ||
            (c.prim != D3DPT_TRIANGLESTRIP && c.prim != D3DPT_TRIANGLELIST))
            return;
        auto* d = dev->lpVtbl;
        // The vertices the triangles use.
        std::vector<uint8_t> used(c.numVertices, 0);
        {
            IDirect3DIndexBuffer9* ib = nullptr;
            if (FAILED(d->GetIndices(dev, &ib)) || !ib)
                return;
            D3DINDEXBUFFER_DESC desc = {};
            ib->lpVtbl->GetDesc(ib, &desc);
            const UINT isz = desc.Format == D3DFMT_INDEX32 ? 4 : 2;
            const UINT n = c.prim == D3DPT_TRIANGLESTRIP ? c.primCount + 2 : c.primCount * 3;
            void* ip = nullptr;
            if (FAILED(ib->lpVtbl->Lock(ib, c.startIndex * isz, n * isz, &ip, D3DLOCK_READONLY)) || !ip)
            {
                ib->lpVtbl->Release(ib);
                return;
            }
            for (UINT i = 0; i < n; ++i)
            {
                uint32_t v;
                if (isz == 4) memcpy(&v, static_cast<const uint8_t*>(ip) + i * 4, 4);
                else { uint16_t h; memcpy(&h, static_cast<const uint8_t*>(ip) + i * 2, 2); v = h; }
                const int k = static_cast<int>(v) - static_cast<int>(c.minIndex);
                if (k >= 0 && k < static_cast<int>(c.numVertices))
                    used[k] = 1;
            }
            ib->lpVtbl->Unlock(ib);
            ib->lpVtbl->Release(ib);
        }
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0;
        if (FAILED(d->GetStreamSource(dev, 0, &vb, &off, &stride)) || !vb)
            return;
        void* ptr = nullptr;
        const UINT first = static_cast<UINT>(c.baseVertex + static_cast<INT>(c.minIndex));
        if (stride >= 12 &&
            SUCCEEDED(vb->lpVtbl->Lock(vb, off + first * stride, c.numVertices * stride, &ptr, D3DLOCK_READONLY)) && ptr)
        {
            // Camera-relative through the draw's world matrix, then the camera's place (FrameConstants).
            const D3DMATRIX& m = *c.world;
            CityWater w = { { 1e9f, 1e9f, 1e9f }, { -1e9f, -1e9f, -1e9f }, g_cityPtsNow.size(), 0 };
            for (UINT i = 0; i < c.numVertices && g_cityPtsNow.size() < kCityPts; ++i)
            {
                if (!used[i])
                    continue;
                float v[3];
                memcpy(v, static_cast<const uint8_t*>(ptr) + i * stride, 12);
                const std::array<float, 3> q = {
                    v[0] * m.m[0][0] + v[1] * m.m[1][0] + v[2] * m.m[2][0] + m.m[3][0] + g_psc[28],
                    v[0] * m.m[0][1] + v[1] * m.m[1][1] + v[2] * m.m[2][1] + m.m[3][1] + g_psc[29],
                    v[0] * m.m[0][2] + v[1] * m.m[1][2] + v[2] * m.m[2][2] + m.m[3][2] + g_psc[30] };
                for (int a = 0; a < 3; ++a)
                {
                    w.lo[a] = (std::min)(w.lo[a], q[a]);
                    w.hi[a] = (std::max)(w.hi[a], q[a]);
                }
                g_cityPtsNow.push_back(q);
                ++w.count;
            }
            vb->lpVtbl->Unlock(vb);
            if (w.count)
                g_cityNow.push_back(w);
        }
        vb->lpVtbl->Release(vb);
    }

    // A part of the water's CPU time (2026-10-06), while the bench or the frame log runs.
    struct WaterTick
    {
        BenchCpu part;
        unsigned long long t0;
        explicit WaterTick(BenchCpu p) : part(p), t0(BenchTiming() ? __rdtsc() : 0) {}
        ~WaterTick() { if (t0) BenchCpuAddTicks(part, __rdtsc() - t0); }
    };

    // A chunk's wet cells, kept (2026-10-06): reading them locked the game's index buffer for each chunk, each
    // frame, 1 ms a frame at the harbour, for an answer that changes only when the game writes the buffer again.
    struct WetKey
    {
        const void* ib; UINT start, prims, minIndex; D3DPRIMITIVETYPE prim;
        bool operator==(const WetKey& o) const
        {
            return ib == o.ib && start == o.start && prims == o.prims && minIndex == o.minIndex && prim == o.prim;
        }
    };
    struct WetKeyHash
    {
        size_t operator()(const WetKey& k) const
        {
            return std::hash<const void*>()(k.ib) ^ (static_cast<size_t>(k.start) * 2654435761u) ^
                   (static_cast<size_t>(k.prims) << 7) ^ (static_cast<size_t>(k.minIndex) << 17);
        }
    };
    struct WetKept { float cells[4]; unsigned long long seq; };
    std::unordered_map<WetKey, WetKept, WetKeyHash> g_wetKept;

    // The chunk's wet cells, as c210 and c251 take them, into `cells`.
    bool WetCells(IDirect3DDevice9* dev, const WaterChunk& c, float cells[4])
    {
        static bool told = false;
        if (c.prim != D3DPT_TRIANGLESTRIP && c.prim != D3DPT_TRIANGLELIST)
            return false;
        auto* d = dev->lpVtbl;
        IDirect3DIndexBuffer9* ib = nullptr;
        if (FAILED(d->GetIndices(dev, &ib)) || !ib)
            return false;
        const WetKey key = { ib, c.startIndex, c.primCount, c.minIndex, c.prim };
        unsigned long long seq = 0;
        const bool followed = ShadowBufferLastWrite(ib, seq);
        if (followed && !g_probeOn)
        {
            const auto kept = g_wetKept.find(key);
            if (kept != g_wetKept.end() && kept->second.seq == seq)
            {
                ib->lpVtbl->Release(ib);
                memcpy(cells, kept->second.cells, sizeof(kept->second.cells));
                return true;
            }
        }
        D3DINDEXBUFFER_DESC desc = {};
        ib->lpVtbl->GetDesc(ib, &desc);
        const UINT isz = desc.Format == D3DFMT_INDEX32 ? 4 : 2;
        const bool strip = c.prim == D3DPT_TRIANGLESTRIP;
        const UINT n = strip ? c.primCount + 2 : c.primCount * 3;
        void* p = nullptr;
        if (FAILED(ib->lpVtbl->Lock(ib, c.startIndex * isz, n * isz, &p, D3DLOCK_READONLY)) || !p)
        {
            ib->lpVtbl->Release(ib);
            if (!told)
                Log("water: the game's water indices could not be read; the swash stops at its last wet cell");
            told = true;
            return false;
        }
        if (!told)
            Log("water: the game's water indices: pool %d, usage 0x%lX, %u-byte", static_cast<int>(desc.Pool),
                desc.Usage, isz);
        told = true;
        uint32_t rows[8] = {};
        auto at = [&](UINT i) -> int {
            uint32_t v;
            if (isz == 4) memcpy(&v, static_cast<const uint8_t*>(p) + i * 4, 4);
            else { uint16_t h; memcpy(&h, static_cast<const uint8_t*>(p) + i * 2, 2); v = h; }
            return static_cast<int>(v) - static_cast<int>(c.minIndex);
        };
        for (UINT t = 0; t < c.primCount; ++t)
        {
            const UINT i = strip ? t : t * 3;
            const int k[3] = { at(i), at(i + 1), at(i + 2) };
            if (k[0] == k[1] || k[1] == k[2] || k[0] == k[2])
                continue;   // a strip's joins
            int r0 = 8, r1 = -1, c0 = 8, c1 = -1;
            bool ok = true;
            for (int j = 0; j < 3; ++j)
            {
                if (k[j] < 0 || k[j] > 80) { ok = false; break; }
                r0 = (std::min)(r0, k[j] / 9); r1 = (std::max)(r1, k[j] / 9);
                c0 = (std::min)(c0, k[j] % 9); c1 = (std::max)(c1, k[j] % 9);
            }
            if (ok && r1 - r0 == 1 && c1 - c0 == 1)
                rows[r0] |= 1u << c0;
        }
        ib->lpVtbl->Unlock(ib);
        ib->lpVtbl->Release(ib);
        float m[4];
        for (int j = 0; j < 4; ++j)
            m[j] = static_cast<float>(rows[j * 2] | (rows[j * 2 + 1] << 8));
        if (g_probeOn && std::hypot(c.world->m[3][0] - 16.7f, c.world->m[3][1] - 16.7f) < 70.0f)
            Log("water: wet cells of the chunk at (%.1f %.1f %.1f), %u triangles from index %u, base %d: rows %02X %02X "
                "%02X %02X %02X %02X %02X %02X", c.world->m[3][0], c.world->m[3][1], c.world->m[3][2], c.primCount,
                c.startIndex, c.baseVertex, rows[0], rows[1], rows[2], rows[3], rows[4], rows[5], rows[6], rows[7]);
        memcpy(cells, m, sizeof(m));
        if (followed)
        {
            if (g_wetKept.size() >= 8192)
                g_wetKept.clear();
            WetKept& k = g_wetKept[key];
            memcpy(k.cells, m, sizeof(m));
            k.seq = seq;
        }
        return true;
    }

    // The sand part over the chunk's whole grid, the dry cells too (2026-10-03). The game leaves the dry cells
    // out of its strip, and the swash, drawn by this pass since it became one shoreline, stopped at the last wet
    // cell: a straight cut with steps across the sand. All 81 points of a chunk carry the water's level, the dry
    // ones too (measured at Westfall's coast), so the dry cells are the same flat plane, and the shader cuts it
    // away above the moving edge. Rows of 9 points along -x, 4.17 yards apart. A chunk with no water at all is
    // not drawn, so the cut can still show at a chunk's edge.
    void FullGridDraw(IDirect3DDevice9* dev, const WaterChunk& c, WaterDrawFn draw)
    {
        auto* d = dev->lpVtbl;
        if (!g_gridIb && !g_gridFailed)
        {
            if (SUCCEEDED(d->CreateIndexBuffer(dev, 8 * 8 * 6 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                               D3DPOOL_MANAGED, &g_gridIb, nullptr)))
            {
                void* p = nullptr;
                if (SUCCEEDED(g_gridIb->lpVtbl->Lock(g_gridIb, 0, 0, &p, 0)) && p)
                {
                    uint16_t* i = static_cast<uint16_t*>(p);
                    for (uint16_t r = 0; r < 8; ++r)
                        for (uint16_t k = 0; k < 8; ++k)
                        {
                            const uint16_t a = r * 9 + k, b = a + 1, e = a + 9, f = a + 10;
                            *i++ = a; *i++ = e; *i++ = b;
                            *i++ = b; *i++ = e; *i++ = f;
                        }
                    g_gridIb->lpVtbl->Unlock(g_gridIb);
                }
                else
                    SafeRelease(g_gridIb);
            }
            if (!g_gridIb)
            {
                g_gridFailed = true;
                Log("water: no index buffer for the whole grid; the swash stops at the game's last wet cell");
            }
        }
        if (g_cfg.water.debugSkip & 1)
            return;
        float cells[4];
        bool haveCells = false;
        if (g_gridIb && !c.city && c.numVertices == 81)
        {
            WaterTick tick(kCpuWaterCells);
            haveCells = WetCells(dev, c, cells);
        }
        if (!haveCells)
        {
            WaterTick tick(kCpuWaterDraws);
            draw(dev, c.prim, c.baseVertex, c.minIndex, c.numVertices, c.startIndex, c.primCount);
            return;
        }
        // A chunk with every cell wet, the open sea's, is the kAllWet the registers already hold (2026-10-06): only
        // a shore chunk sets its cells, and puts kAllWet back for the water part and the next chunk.
        const bool shore = memcmp(cells, kAllWet, sizeof(cells)) != 0;
        if (shore)
        {
            SetChunkReg(dev, kPsReg + 90, g_c210, cells);
            d->SetVertexShaderConstantF(dev, kVsReg + 11, cells, 1);
        }
        IDirect3DIndexBuffer9* old = nullptr;
        d->GetIndices(dev, &old);
        d->SetIndices(dev, g_gridIb);
        {
            WaterTick tick(kCpuWaterDraws);
            draw(dev, D3DPT_TRIANGLELIST, c.baseVertex + static_cast<INT>(c.minIndex), 0, 81, 0, 128);
        }
        d->SetIndices(dev, old);
        if (old) old->lpVtbl->Release(old);
        if (shore)
        {
            SetChunkReg(dev, kPsReg + 90, g_c210, kAllWet);
            d->SetVertexShaderConstantF(dev, kVsReg + 11, kAllWet, 1);
        }
    }
}

void WaterAfterDraw(IDirect3DDevice9* dev, const WaterChunk& c, WaterDrawFn draw)
{
    if (!g_copyOk || !WaterWanted() || g_cfg.water.debug == 6)
        return;
    if (c.city)
        NoteCityWater(dev, c);

    auto* d = dev->lpVtbl;
    D3DMATRIX wv, wvp;
    Mul(*c.world, *c.view, wv);
    Mul(wv, *c.proj, wvp);
    float vc[12 * 4] = {};
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
    // On a lake, a pond or a river, Lake Waves of it (2026-10-05, the owner): the swell lifted a pond's water near
    // its bank, and its edge crept up and down the sand.
    const float chunkX = c.world->m[3][0] - 16.7f + g_psc[28], chunkY = c.world->m[3][1] - 16.7f + g_psc[29];
    const bool chunkSea = !c.city && MapWaterIsSea(chunkX, chunkY);
    vc[39] = g_sceneOk && g_cfg.water.surface > 0.0f
                 ? g_cfg.water.waveHeight * (chunkSea ? 1.0f : g_cfg.water.lakeWaves) : 0.0f;
    vc[40] = 1.0f / g_cfg.water.waveScale;
    vc[41] = c.city ? 1.0f : 0.0f;
    memcpy(vc + 44, kAllWet, sizeof(kAllWet));   // c251, the chunk's own for the whole grid (FullGridDraw)

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
    float oldVc[12 * 4];
    const bool haveVc = SUCCEEDED(d->GetVertexShaderConstantF(dev, kVsReg, oldVc, 12));
    // Not on a body: the body mask marks every unit's pixels in the stencil as the world is drawn, and the foam
    // passes only where the mark is clear. At a body's edge the foam took the body for flat ground beside the
    // water, and its hips went white (2026-10-02).
    DWORD bit = 0, oldSt[kStencilCount] = {};
    const bool skipBodies = BodyMarkLive(bit);
    if (skipBodies)
    {
        for (int i = 0; i < kStencilCount; ++i)
            d->GetRenderState(dev, kStencil[i], &oldSt[i]);
        // Where no body or model is marked: bit holds both marks (bodymask.cpp), and a model, a reed in the
        // shallows, is skipped as a body is (2026-10-03).
        d->SetRenderState(dev, D3DRS_STENCILENABLE, TRUE);
        d->SetRenderState(dev, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
        d->SetRenderState(dev, D3DRS_STENCILREF, 0);
        d->SetRenderState(dev, D3DRS_STENCILMASK, bit);
        d->SetRenderState(dev, D3DRS_STENCILWRITEMASK, 0);
    }
    d->SetVertexShader(dev, g_vs);
    d->SetPixelShader(dev, g_ps);
    d->SetVertexShaderConstantF(dev, kVsReg, vc, 12);
    // The frame's part: once, at the frame's first chunk (see g_frameSet).
    if (!g_frameSet)
        SetFrameState(dev);
    {
        // c211, the swash's length along the shore and its speed (2026-10-05): as the wet sand pass's c212.
        // And whether this chunk is the sea (its middle, 16.7 yards in from its corner), and the share a lake gets.
        // Set when it differs from the last chunk's: the sea's chunks come in runs.
        const float mx = chunkX, my = chunkY;
        const bool sea = chunkSea;
        const float sw[4] = { 30.0f / g_cfg.water.swashLength, 0.55f * g_cfg.water.swashSpeed,
                              sea ? 1.0f : g_cfg.water.lakeFoam, sea ? 1.0f : g_cfg.water.lakeSwash };
        SetChunkReg(dev, kPsReg + 91, g_c211, sw);
        if (g_probeOn && std::hypot(c.world->m[3][0] - 16.7f, c.world->m[3][1] - 16.7f) < 70.0f)
            Log("water: the chunk with its middle at (%.1f %.1f) is %s: swash x %.2f, shore foam x %.2f, swell %.2f yards",
                mx, my, c.city ? "a building's water" : sea ? "the sea" : "a lake, a pond or a river", sw[3], sw[2], vc[39]);
    }

    // With our surface, two draws (2026-10-02). The swell lifts a chunk over the next one on screen, and drawn
    // without the depth test whichever chunk came later won: bands along every chunk's edge, the game's flat
    // water painted over the raised edge of the one before. So the water part is depth tested and writes its
    // depth: a raised wave hides what lies behind it, the game's next chunk included, and the sun shadows and
    // the fog see the waves. The sand part (wet sand, the lip of foam above the waterline) lies in front of
    // the water's plane and is drawn without the test, as before. Without our surface, one draw does both.
    const bool surface = g_sceneOk && g_cfg.water.surface > 0.0f;
    // c167 and c168 are the chunk's own: set when they differ from what the last draw left there (2026-10-06).
    // The fourth is the sky reflection's strength: until 2026-10-02 this wrote 0 over it, and Sky Reflection
    // did nothing.
    float mode[4] = { g_psc[188], surface ? 1.0f : 0.0f, g_psc[190], g_psc[191] };
    SetChunkReg(dev, kPsReg + 47, g_c167, mode);
    const float cityW = c.city ? 1.0f : 0.0f;
    const float sw2[4] = { g_psc[192], g_psc[193], g_psc[194], cityW };
    SetChunkReg(dev, kPsReg + 48, g_c168, sw2);
    FullGridDraw(dev, c, draw);
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
        SetChunkReg(dev, kPsReg + 47, g_c167, mode);
        if (!(g_cfg.water.debugSkip & 2))
        {
            WaterTick tick(kCpuWaterDraws);
            draw(dev, c.prim, c.baseVertex, c.minIndex, c.numVertices, c.startIndex, c.primCount);
        }
        // Once more on the bodies (the stencil's mark, bodymask.cpp), the water over a character's legs: skipped,
        // the game's own pale water showed there, and the legs under the water looked like a ghost (2026-10-02).
        // The depth test keeps it off what stands above the surface.
        if (skipBodies && !(g_cfg.water.debugSkip & 4))
        {
            WaterTick tick(kCpuWaterDraws);
            // No draw of the pass reads c168 before the next chunk sets its own (2026-10-06): it is not put back.
            const float body[4] = { g_psc[192], 1.0f, g_cfg.water.cover, cityW };
            SetChunkReg(dev, kPsReg + 48, g_c168, body);
            d->SetRenderState(dev, D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);   // marked: a body or a model
            // Without depth writes (2026-10-06): with them, and the shader's clip(), the stencil was tested after the
            // shader ran, so the whole shader ran on every pixel of the chunk, about 3 ms a frame at the harbour.
            // Without, the stencil turns the unmarked pixels away first. Over the legs the depth stays the game's
            // flat water, which its own draw wrote ([depth] waterDepth), not our swell's.
            d->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
            draw(dev, c.prim, c.baseVertex, c.minIndex, c.numVertices, c.startIndex, c.primCount);
            d->SetRenderState(dev, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
        }
        for (int i = 0; i < 4; ++i)
            d->SetRenderState(dev, zs[i], oz[i]);
    }
    ++g_foamDraws;

    // Our textures stay bound until the world's end (WaterWorldEnded); the client's state goes back now.
    if (skipBodies)
        for (int i = 0; i < kStencilCount; ++i)
            d->SetRenderState(dev, kStencil[i], oldSt[i]);
    if (haveVc)
        d->SetVertexShaderConstantF(dev, kVsReg, oldVc, 12);
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
    // Every shore chunk within 70 yards, in short; the first in full.
    if (g_pShore < 40 && !c.city && c.primCount < 128 &&
        std::hypot(c.world->m[3][0] - 16.7f, c.world->m[3][1] - 16.7f) < 70.0f)
    {
        ProbeShore(dev, c, g_pShore == 0);
        ++g_pShore;
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
    // A river's or the sea's surface, or a building's water (2026-10-05): on a fresh login in Stormwind's canals,
    // which are a building's, no batch was ever taken, and the game's V and ring showed beside ours.
    float wz = 0.0f;
    const bool mapWet = MapWaterHeight(p[0], p[1], wz) && fabsf(p[2] - wz) <= 0.6f;
    const bool cityWet = !mapWet && CityWaterAt(p[0], p[1], p[2], wz);
    const bool wet = mapWet || cityWet;
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
    // A batch on the water's grid (2026-10-04): the wake's trail is drawn over the water's cells, and its first
    // vertex is a cell corner (a multiple of 4.1667 yards), not where the unit is. At 6 yards it was taken only
    // once someone stood that near a corner, and the game's wake showed for up to a minute after a start.
    const float kCellYd = 533.33333f / 128.0f;
    const auto onGrid = [&](float a) {
        const float m = fabsf(a / kCellYd - roundf(a / kCellYd)) * kCellYd;
        return m < 0.02f;
    };
    // A building's water has its own grid, not the map's: up to the same 15 yards.
    const float reach = (cityWet || (onGrid(p[0]) && onGrid(p[1]))) && fabsf(p[2] - wz) < 0.1f ? 15.0f : 6.0f;
    for (int i = 0; i < g_unitCount; ++i)
    {
        const float dx = g_units[i][0] - p[0], dy = g_units[i][1] - p[1];
        if (dx * dx + dy * dy < reach * reach)
        {
            g_wakeTex.insert(tex);
            g_wakeNot.erase(tex);
            char info[96];
            TexInfo(tex, info, sizeof(info));
            Log("water: the game's wake is drawn with texture %s: particles at (%.1f %.1f %.1f) by a unit, on %s "
                "at %.1f", info, p[0], p[1], p[2], cityWet ? "a building's water" : "the water", wz);
            ++g_wakeDraws;
            return true;
        }
    }
    return false;
}

float WaterBedFade()
{
    return g_bedFade;
}

bool WaterShipNear(const float p[3])
{
    for (const auto& s : g_movingShips)
    {
        const float dx = p[0] - s[0], dy = p[1] - s[1], dz = p[2] - s[2];
        if (dx * dx + dy * dy < 35.0f * 35.0f && fabsf(dz) < 45.0f)
            return true;
    }
    return false;
}

void WaterNoteHull(IDirect3DDevice9* dev, const D3DMATRIX& world)
{
    // Always, not only with Ship Wake (2026-10-06): the shadow cache files a ship's sails under it (WaterShipNear).
    if (g_hullRelN >= kHullsMax)
        return;
    const float x = world.m[3][0], y = world.m[3][1], z = world.m[3][2];
    for (int i = 0; i < g_hullRelN; ++i)
        if (g_hullRel[i][0] == x && g_hullRel[i][1] == y && g_hullRel[i][2] == z)
            return;
    IDirect3DPixelShader9* ps = nullptr;
    dev->lpVtbl->GetPixelShader(dev, &ps);
    if (ps)
    {
        ps->lpVtbl->Release(ps);
        return;   // the terrain's: drawn through a pixel shader
    }
    g_hullRel[g_hullRelN][0] = x;
    g_hullRel[g_hullRelN][1] = y;
    g_hullRel[g_hullRelN][2] = z;
    ++g_hullRelN;
}

void WaterWorldEnded(IDirect3DDevice9* dev)
{
    UnbindFrame(dev);
    if (g_spanOpen)
    {
        BenchSectionEnd(dev, kBenchWaterSpan, true);
        g_spanOpen = false;
    }
}

void WaterFrameEnd(IDirect3DDevice9* dev)
{
    UnbindFrame(dev);   // a world that never ended (2026-10-06)
    g_spanOpen = false;   // and its span is not timed

    {
        // The ships: this frame's origins followed from the last frame's (see Hull).
        const double now = Now();
        const double dt = now - g_hullLast;
        float cam[3] = {};
        if (g_hullRelN > 0 && ClientCamera(cam) && dt > 1e-4 && dt < 0.5)
        {
            float pos[kHullsMax][3];
            int   match[kHullsMax];
            float mx[kHullsMax], my[kHullsMax];
            int   nm = 0;
            for (int i = 0; i < g_hullRelN; ++i)
            {
                for (int j = 0; j < 3; ++j)
                    pos[i][j] = g_hullRel[i][j] + cam[j];
                match[i] = -1;
                float bestD = 1.5f * 1.5f;
                for (size_t h = 0; h < g_hulls.size(); ++h)
                {
                    const float dx = pos[i][0] - g_hulls[h].pos[0], dy = pos[i][1] - g_hulls[h].pos[1];
                    const float dz = pos[i][2] - g_hulls[h].pos[2];
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < bestD)
                    {
                        bestD = d2;
                        match[i] = static_cast<int>(h);
                    }
                }
                if (match[i] >= 0)
                {
                    mx[nm] = pos[i][0] - g_hulls[match[i]].pos[0];
                    my[nm] = pos[i][1] - g_hulls[match[i]].pos[1];
                    ++nm;
                }
            }
            float cx = 0.0f, cy = 0.0f;
            if (nm >= 5)
            {
                std::nth_element(mx, mx + nm / 2, mx + nm);
                std::nth_element(my, my + nm / 2, my + nm);
                cx = mx[nm / 2];
                cy = my[nm / 2];
            }
            const float ease = static_cast<float>(1.0 - exp(-dt / 0.5));
            const size_t before = g_hulls.size();
            for (int i = 0; i < g_hullRelN; ++i)
            {
                if (match[i] < 0)
                {
                    g_hulls.push_back({ { pos[i][0], pos[i][1], pos[i][2] }, { 0.0f, 0.0f }, now, false, false });
                    continue;
                }
                if (static_cast<size_t>(match[i]) >= before)
                    continue;
                Hull& h = g_hulls[match[i]];
                if (h.seen == now)
                    continue;   // two origins on one: the first keeps it
                const float vx = (pos[i][0] - h.pos[0] - cx) / static_cast<float>(dt);
                const float vy = (pos[i][1] - h.pos[1] - cy) / static_cast<float>(dt);
                h.vel[0] += (vx - h.vel[0]) * ease;
                h.vel[1] += (vy - h.vel[1]) * ease;
                memcpy(h.pos, pos[i], sizeof(h.pos));
                h.seen = now;
                float wz = 0.0f, gz = 0.0f;
                const bool water = MapWaterHeight(h.pos[0], h.pos[1], wz) ? fabsf(h.pos[2] - wz) < 4.0f
                                                                           : !MapGroundHeight(h.pos[0], h.pos[1], gz);
                const bool was = h.ship;
                const float sp2 = h.vel[0] * h.vel[0] + h.vel[1] * h.vel[1];
                h.ship = water && sp2 > 1.5f * 1.5f;
                h.moving = water && sp2 > 0.4f * 0.4f;
                if (h.ship && !was)
                    Log("water: a ship at (%.1f %.1f %.1f), %.1f yards a second: its wake (Ship Wake %.0f)", h.pos[0],
                        h.pos[1], h.pos[2], sqrtf(h.vel[0] * h.vel[0] + h.vel[1] * h.vel[1]), g_cfg.water.shipWake);
            }
        }
        for (size_t h = 0; h < g_hulls.size();)
        {
            if (now - g_hulls[h].seen > 1.0)
                g_hulls.erase(g_hulls.begin() + h);
            else
                ++h;
        }
        if (g_hulls.size() > 1024)
            g_hulls.clear();
        g_movingShips.clear();
        for (const Hull& h : g_hulls)
            if (h.moving && now - h.seen < 0.5)
                g_movingShips.push_back({ h.pos[0], h.pos[1], h.pos[2] });
        if (g_probeOn)
        {
            int ships = 0;
            for (const Hull& h : g_hulls)
                ships += h.ship ? 1 : 0;
            Log("water: ships: %d building origins this frame, %d followed, %d of them ships", g_hullRelN,
                static_cast<int>(g_hulls.size()), ships);
            for (const Hull& h : g_hulls)
                if (h.moving)
                    Log("water:   a ship under way at (%.1f %.1f %.1f), %.1f yards a second: the shadow cache files the "
                        "models within 35 yards of it as its parts", h.pos[0], h.pos[1], h.pos[2],
                        sqrtf(h.vel[0] * h.vel[0] + h.vel[1] * h.vel[1]));
            float me[3];
            if (ClientPlayer(me))
                ClientTransportsLog(me);
        }
        g_hullRelN = 0;
        g_hullLast = now;
    }
    {
        // How hard it rains, from this frame's rain draws (WaterNoteRain), eased over about a second.
        const double now = Now();
        const float target = (std::min)(1.0f, g_rainPrims / 40000.0f);
        const float ease = g_rainLast > 0.0 ? static_cast<float>(1.0 - exp(-(now - g_rainLast) / 0.8)) : 1.0f;
        g_rain += (target - g_rain) * (std::min)(1.0f, ease);
        if (g_rain < 0.005f && target == 0.0f)
            g_rain = 0.0f;
        g_rainLast = now;
        if (g_probeOn)
            Log("water: rain: %u of its triangles this frame, rain on the water %.2f (Rain on Water %.2f)", g_rainPrims,
                g_rain, g_cfg.water.rain);
        g_rainPrims = 0;
    }
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
    g_cityLast.swap(g_cityNow);
    g_cityNow.clear();
    g_cityPtsLast.swap(g_cityPtsNow);
    g_cityPtsNow.clear();
    ++g_frameNo;
    g_foamLast = g_foamDraws;
    g_foamDraws = 0;
    g_copied = false;
    g_copyOk = false;
    g_sceneOk = false;
}

namespace
{
    // Liquid textures already judged (WaterTextureIsWater). Cleared with the device.
    std::map<IDirect3DBaseTexture9*, bool> g_liquidTex;

    float Unpack565(uint16_t c, int ch)
    {
        return ch == 0 ? ((c >> 11) & 31) / 31.0f : ch == 1 ? ((c >> 5) & 63) / 63.0f : (c & 31) / 31.0f;
    }

    // The average colour of a texture's smallest level: one pixel, or a compressed block's two end colours.
    bool TextureColour(IDirect3DTexture9* t, float rgb[3])
    {
        const DWORD levels = t->lpVtbl->GetLevelCount(t);
        if (levels == 0)
            return false;
        D3DSURFACE_DESC desc = {};
        if (FAILED(t->lpVtbl->GetLevelDesc(t, levels - 1, &desc)))
            return false;
        D3DLOCKED_RECT lr = {};
        if (FAILED(t->lpVtbl->LockRect(t, levels - 1, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits)
            return false;
        const uint8_t* p = static_cast<const uint8_t*>(lr.pBits);
        bool ok = true;
        switch (desc.Format)
        {
        case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        {
            const uint8_t* b = desc.Format == D3DFMT_DXT1 ? p : p + 8;   // the colour half of the block
            uint16_t c0, c1;
            memcpy(&c0, b, 2);
            memcpy(&c1, b + 2, 2);
            for (int ch = 0; ch < 3; ++ch)
                rgb[ch] = 0.5f * (Unpack565(c0, ch) + Unpack565(c1, ch));
            break;
        }
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
            rgb[0] = p[2] / 255.0f; rgb[1] = p[1] / 255.0f; rgb[2] = p[0] / 255.0f;
            break;
        case D3DFMT_R5G6B5:
        {
            uint16_t c;
            memcpy(&c, p, 2);
            for (int ch = 0; ch < 3; ++ch)
                rgb[ch] = Unpack565(c, ch);
            break;
        }
        default:
            ok = false;
        }
        t->lpVtbl->UnlockRect(t, levels - 1);
        return ok;
    }
}

// Water in a building comes in the same draws as its lava and slime (2026-10-03, Stormwind's canals): the
// texture tells them apart. Blue at least as strong as red, and green not far over blue: water; red over blue
// is lava, green far over blue slime. Each texture is judged once, and logged.
bool WaterTextureIsWater(IDirect3DDevice9* dev)
{
    IDirect3DBaseTexture9* base = nullptr;
    if (FAILED(dev->lpVtbl->GetTexture(dev, 0, &base)) || !base)
        return false;
    auto it = g_liquidTex.find(base);
    if (it != g_liquidTex.end())
    {
        base->lpVtbl->Release(base);
        return it->second;
    }
    bool water = false;
    float rgb[3] = {};
    if (base->lpVtbl->GetType(base) == D3DRTYPE_TEXTURE &&
        TextureColour(reinterpret_cast<IDirect3DTexture9*>(base), rgb))
    {
        water = rgb[2] >= rgb[0] && rgb[1] <= rgb[2] * 1.5f;
        Log("water: a building's liquid, texture %p, colour %.2f %.2f %.2f: %s", base, rgb[0], rgb[1], rgb[2],
            water ? "water, ours" : "not water, the game's");
    }
    else
        Log("water: a building's liquid, texture %p: its colour could not be read, left to the game", base);
    if (g_liquidTex.size() < 512)
        g_liquidTex[base] = water;
    base->lpVtbl->Release(base);
    return water;
}

void WaterReset()
{
    g_liquidTex.clear();
    g_wetKept.clear();
    g_trails.clear();
    g_hulls.clear();
    g_hullRelN = 0;
    g_movingShips.clear();
    g_cityNow.clear();
    g_cityLast.clear();
    g_cityPtsNow.clear();
    g_cityPtsLast.clear();
    SafeRelease(g_foamTex);
    g_foamTexState = 0;
    SafeRelease(g_foamBody);
    g_foamBodyFailed = false;
    SafeRelease(g_wetSb);
    SafeRelease(g_gridIb);
    g_gridFailed = false;
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
    // Nothing is called on the device here: it may be gone. Present has taken our textures off already.
    g_boundSamplers = 0;
    g_frameSet = false;
}

void WaterProbe()
{
    g_probeOn = true;
    g_pCount = g_pFirst = g_pLast = g_pDetailed = g_pOther = g_pShore = 0;
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

// A draw that may be the game's rain (2026-10-05): its triangles are counted for the frame.
void WaterNoteRain(IDirect3DDevice9* dev, UINT prims)
{
    auto* d = dev->lpVtbl;
    DWORD blend = 0, src = 0, dst = 0, zw = 1;
    d->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
    if (!blend)
        return;
    d->GetRenderState(dev, D3DRS_SRCBLEND, &src);
    d->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
    d->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
    if (src != D3DBLEND_DESTCOLOR || dst != D3DBLEND_SRCCOLOR || zw)
        return;
    IDirect3DBaseTexture9* tex = nullptr;
    d->GetTexture(dev, 0, &tex);
    if (!tex)
        return;
    D3DSURFACE_DESC td = {};
    if (tex->lpVtbl->GetType(tex) == D3DRTYPE_TEXTURE &&
        SUCCEEDED(reinterpret_cast<IDirect3DTexture9*>(tex)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(tex), 0, &td)) &&
        td.Width == 16 && td.Height == 128)
        g_rainPrims += prims;
    tex->lpVtbl->Release(tex);
}

float WaterRainAmount()
{
    return g_rain;
}

// The shaders this pass compiles, as it compiles them, for the cache's worker (shadercache.cpp, 2026-10-06).
void WaterShaderList()
{
    ShaderPrecompile("water_vs", kVsHlsl, "vs_3_0");
    ShaderPrecompile("water_ps", kPsHlsl, "ps_3_0");
    ShaderPrecompile("wet_vs", kWetVsHlsl, "vs_3_0");
    ShaderPrecompile("wet_ps", kWetPsHlsl, "ps_3_0");
}
