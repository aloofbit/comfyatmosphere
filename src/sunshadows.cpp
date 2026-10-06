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
// slope, only when either is above 0. normalBias is 2 since 2026-10-02, off the bodies the mask finds:
// walls the sun grazes shaded themselves in stripes without it.
//
// It draws only while the volumetric light does, since the map is built for it. It follows the sun's
// height as the light does (the map follows the moon at night), and at night [sunshadows] night.

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
#include "shadow.h"
#include "sun.h"
#include "water.h"
#include "sunshadows.h"
#include "bodymask.h"
#include "volume.h"
#include "shadercache.h"

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

    // Blended as scene x this. In two literals: MSVC takes at most 16 KB in one.
    const char* kPsHlsl = R"HLSL(
sampler2D sDepth  : register(s0);   // the scene's depth (INTZ)
sampler2D sShadow : register(s1);   // the sun's depth (INTZ), border = far: the far map
sampler2D sNear   : register(s2);   // the near map, the same way
sampler2D sNearL  : register(s3);   // the near map's leaves
sampler2D sFarL   : register(s4);   // the far map's leaves
sampler2D sUnit   : register(s5);   // the units alone, the near map's camera and size
sampler2D sBody   : register(s6);   // the screen: 1 where a player or a creature shows (bodymask.cpp)
sampler2D sMid    : register(s7);   // the middle map, solid only ([shadow] midRange)
sampler2D sTerr   : register(s8);   // hills and mountains alone, the far map's camera
sampler2D sUnder  : register(s9);   // the depth under the water (water.cpp), copied before the water drew
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
float4 gU    : register(c25);       // units' map: extra strength, depth bias (map units), one texel (uv), 1 if there is one
float4 gU2   : register(c26);       // units' map: 1 / unitGap (map units), yards down per map unit, unitDrop and
                                    // unitDrop + 3 (yards)
float4 gBody : register(c27);       // the bodies: bodyShade (0..1), 1 if there is a mask, sunOffset (map units)
float4 gM0   : register(c28);       // rows of the middle map's view-projection
float4 gM1   : register(c29);
float4 gM2   : register(c30);
float4 gM3   : register(c31);
float4 gMB   : register(c32);       // middle map: depth bias, normal offset, one texel, 1 if there is one
float4 gTr   : register(c33);       // the terrain: terrainShade, 1 if there is a map of it, its least depth bias
float4 gWt   : register(c34);       // the water: 1 if the depth under it is there, light absorbed a yard;
                                    // w Shadow on Water (0..1)
                                    // (map units)
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
// The shade on the water's own surface (2026-10-06, the owner): solid things and leaves (a ship's sails are alpha
// tested) from the near map, else the far one, with no facing, slope or hill check (the surface is flat), its taps
// three texels apart for a soft edge. 1 shaded, 0 lit.
float SurfaceShade(float3 Q)
{
    Q += gSun.xyz * gT.y;
    float sh = 0.0, wn = 0.0;
    [branch] if (gNB.w > 0.5)
    {
        float4 sn = Q.x * gN0 + Q.y * gN1 + Q.z * gN2 + gN3;
        float2 en = abs(sn.xy);
        wn = saturate((0.9 - max(en.x, en.y)) * 10.0);
        [branch] if (wn > 0.0)
        {
            float leaf = gCh.y > 0.5 ? 1.0 - Lit5(sNearL, sn, 0.0, gNB.x, gNB.z * 3.0) : 0.0;
            sh = 1.0 - Lit(sNear, sn, 0.0, gNB.x, gNB.z * 3.0) * (1.0 - gCh.x * leaf);
        }
    }
    [branch] if (wn < 1.0 && gCh.w > 0.5)
    {
        float4 sf   = Q.x * gSh0 + Q.y * gSh1 + Q.z * gSh2 + gSh3;
        float2 ef   = abs(sf.xy);
        float  fade = saturate((1.0 - max(ef.x, ef.y)) * 10.0);
        float  leaf = gCh.z > 0.5 ? 1.0 - Lit5(sFarL, sf, 0.0, gB.x, gB.z * 3.0) : 0.0;
        float  shF  = (1.0 - Lit(sShadow, sf, 0.0, gB.x, gB.z * 3.0) * (1.0 - gCh.x * leaf)) * fade;
        sh = lerp(shF, sh, wn);
    }
    return sh;
}
static bool g_underWater = false;   // this pixel's point is the bed under the water: its neighbours are too
static bool g_onWater    = false;   // the water drew its own depth over what was there (with a bed under it or not)
float Raw(float2 uv)
{
    float r = saturate((tex2Dlod(sDepth, float4(uv, 0, 0)).r - gZ.x) * gZ.y);
    if (g_underWater)
    {
        float u = saturate((tex2Dlod(sUnder, float4(uv, 0, 0)).r - gZ.x) * gZ.y);
        r = u < 0.99999 ? max(r, u) : r;
    }
    return r;
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
)HLSL" R"HLSL(
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float  raw = Raw(uv);
    if (raw >= 0.99999)
        return gL.y > 0.5 ? 1.0 : 0.5;                                     // the sky: no change
    float3 P   = PointAt(uv, raw);
    const float3 Ps = P;   // what the depth shows: under the water, its surface (the Sunlight and the sun tint go there)
    // Under the water the shade falls on the bed, seen through it, and not on the surface (2026-10-02: in
    // Westfall's shallows a character's shadow lay on the water, away from where it reaches the sand under
    // it). The point is the bed's, which holds still; the shade fades as the water gets deep and hides the
    // bed. A point mixed between the surface and the bed moved with the swell, and the shade flickered.
    // Past the world's slice the copy holds far terrain (water.cpp): there the surface keeps it.
    float seen = 1.0;
    [branch] if (gWt.x > 0.5)
    {
        float rawU = saturate((tex2Dlod(sUnder, float4(uv, 0, 0)).r - gZ.x) * gZ.y);
        g_onWater = rawU > raw + 1e-6;
        if (rawU > raw && rawU < 0.99999)
        {
            float3 Pb = PointAt(uv, rawU);
            seen = exp(-gWt.y * length(Pb - P));
            P = Pb;
            raw = rawU;
            g_underWater = true;
        }
    }
    // debug 7 (Debug View 28, 2026-10-06): where the pass finds the water's own depth over a bed (blue), so the shade
    // goes on the bed and fades with the water; grey where it does not, so the shade falls on what the depth shows.
    // The owner's ridge under the sea took the sun's tint and Sunlight as if it stood in the open.
    if (gL.y > 6.5)
        return g_underWater ? float4(0.1, 0.45, 1.0, 1.0) : float4(0.35, 0.35, 0.35, 1.0);
    // The facing, for normalBias and slope only: from the neighbours a pixel away, nearer in depth on
    // each axis. The offset along it grows as the sun grazes the surface, and is only there where the sun
    // is within about 20 degrees of the surface or behind it (2026-10-02): on a bridge's deck, facing a
    // sun 47 degrees up, it lifted each point off the deck and cut the parapet's shade to a strip. A face
    // the sun reaches well has no stripes to stop.
    float3 N     = float3(0.0, 0.0, 1.0);
    float  graze = 1.0;
    float  ndl   = 1.0;
    [branch] if (gT.z > 0.5)
    {
        float3 dx = Near(uv, raw, P, float2(gZ.z, 0.0));
        float3 dy = Near(uv, raw, P, float2(0.0, gZ.w));
        N = cross(dy, dx);
        N = N / max(length(N), 1e-8);
        N = dot(N, P) > 0.0 ? -N : N;
        ndl = dot(N, gSun.xyz);
        graze = (1.0 + 3.0 * sqrt(saturate(1.0 - ndl * ndl))) * (1.0 - smoothstep(0.1, 0.35, ndl));
    }

    // On a player or a creature, as the client drew it (bodymask.cpp): 1 on a body, 0 elsewhere, between at
    // the edge of one (2026-10-01).
    // With no mask every pixel takes a body's slack (the safe side: no shade of a surface on itself) and
    // nothing is scaled.
    float2 bodyM = gBody.y > 0.5 ? tex2Dlod(sBody, float4(uv, 0, 0)).rg : float2(0.0, 1.0);
    float body  = bodyM.x;
    float bodyS = gBody.y > 0.5 ? body : 1.0;
    // The normal offset stays off the bodies (2026-10-02): a body's facing from the depth is per triangle,
    // and the offset put its triangles on the character. Walls and the ground take all of it.
    float offK = 1.0 - body;
    // Which way the offset goes (2026-10-05). Along the facing where the sun reaches the surface. On a face
    // turned from the sun, straight away from the sun instead: the back of a stump in Elwynn leans in toward
    // its top, so its facing points a little up, and near the top edge the offset lifted the point over the
    // stump's top into the sun: a lit strip along the edge. Away from the sun, the stump stays between the
    // point and the sun to the edge.
    float3 offDir = normalize(lerp(N, -gSun.xyz, saturate(-ndl * 5.0)));

    // The near map where it reaches, blended into the far one over the band from 80% to 90% of its
    // half-width. The far map is read only where the near map does not cover all of the shade.
    float wn = 0.0, litN = 1.0, leafN = 1.0;
    float4 sn = 0.0;
    [branch] if (gNB.w > 0.5)
    {
        float3 Qn = P + offDir * (gNB.y * graze * offK) + gSun.xyz * gT.y;
        sn = Qn.x * gN0 + Qn.y * gN1 + Qn.z * gN2 + gN3;
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
    // The middle map ([shadow] midRange, 2026-10-02) where the near map does not cover all of the shade,
    // blended into the far one the same way. Solid only: the leaves there come from the far leaf map.
    float wm = 0.0, litM = 1.0;
    [branch] if (wn < 1.0 && gMB.w > 0.5)
    {
        float3 Qm = P + offDir * (gMB.y * graze * offK) + gSun.xyz * gT.y;
        float4 sm = Qm.x * gM0 + Qm.y * gM1 + Qm.z * gM2 + gM3;
        float2 em = abs(sm.xy);
        wm = saturate((0.9 - max(em.x, em.y)) * 10.0);
        [branch] if (wm > 0.0)
        {
            float2 g = Slope(N, gM0, gM1, gM2, gG.z) * gT.x;
            litM = Lit(sMid, sm, g, gMB.x, gMB.z);
        }
    }
)HLSL" R"HLSL(
    // The far map, fading out over its last tenth, where it ends. Its solid map only where the near and
    // middle maps leave some of the shade to it; its leaves wherever the near map does; the terrain's map,
    // under the same camera, everywhere.
    float litF = 1.0, leafF = 1.0, terr = 0.0, through = 0.0, throughS = 0.0, gateV = 0.0;
    float3 depthsV = 0.0;   // debug 6: the depths the check compares
    // Ground and walls that face away from the sun (not models: green in the mask, the facing of a leaf card
    // from the depth is noise). A ray from such a face runs just under the surface and stays inside the
    // terrain's slack a long way, so the terrain's shade does not say the hill is in front of it.
    float away = saturate(-ndl * 5.0) * (1.0 - bodyM.y);
    [branch] if (gCh.w > 0.5 && (wn < 1.0 || gTr.y > 0.5))
    {
        float3 Qf = P + offDir * (gB.y * graze * offK) + gSun.xyz * gT.y;
        float4 sf = Qf.x * gSh0 + Qf.y * gSh1 + Qf.z * gSh2 + gSh3;
        float2 ef = abs(sf.xy);
        float  fade = saturate((1.0 - max(ef.x, ef.y)) * 10.0);
        float2 g = Slope(N, gSh0, gSh1, gSh2, gG.x) * gT.x;
        // More slack with distance ([sunshadows] lodBias): the ground the client draws far off is coarser
        // than the terrain in the map.
        float  bF = gB.x + gLod.x * max(length(P) - gLod.y, 0.0);
        [branch] if (wn < 1.0 && wm < 1.0)
            litF = lerp(1.0, Lit(sShadow, sf, g, bF, gB.z), fade);
        [branch] if (wn < 1.0 && gCh.z > 0.5)
            leafF = lerp(1.0, Lit5(sFarL, sf, g, bF, gB.z), fade);
        [branch] if (gTr.y > 0.5)
            terr = 1.0 - lerp(1.0, Lit5(sTerr, sf, g, max(bF, gTr.z), gB.z), fade);
        // A tree's shade through a hill (2026-10-03): with the sun low behind a ridge, the trees on its crest
        // printed their shapes on the shaded face, through the ground. Along the sun's line the order is
        // the tree, then the hill, then the point: the hill's shade alone. A tree in a valley under a far
        // hill (the hill, then the tree) keeps its shade. The depths are the nearest of five taps, so the
        // soft edge of the tree's shade goes with it. The terrain's shade on the point already says the hill
        // lies between it and the sun, so the margin is small: 0.1 of terrainBias (0.15 yards), full at
        // twice that. With the whole of terrainBias the foot of a trunk on the crest still showed: the
        // ground there is that close behind it. Solid things (a trunk drawn by the game) the same way.
        // On a face turned away from the sun too, where the terrain's shade misses the hill (above). Only
        // where the terrain lies nearer the sun than the point by the same margin: until 2026-10-03 a face
        // turned away lost all cast shade, and the back walls of Darnassus's buildings, shaded by the
        // buildings themselves, showed lit. No hill stands in front of a back wall.
        // The hill, then the caster, then the point: kept only near the point ([sunshadows] hillCarry,
        // 2026-10-04). On the Barrens side of the Ashenvale border, with the sun at 29 degrees behind the
        // ridges, trees standing in the far hills' shade laid their trunks' shade 100 yards and more down a
        // slope that faced away from the sun: two long dark strips through the hill's shade. No sun reaches
        // there to cast them. A fence or a tree beside you in a mountain's shade keeps its outline; the
        // shade fades out from hillCarry yards between the caster and the point to twice that.
        [branch] if (gTr.y > 0.5 && max(terr, away) > 0.0)
        {
            float2 uvf = float2(sf.x * 0.5 + 0.5, 0.5 - sf.y * 0.5);
            float  o   = gB.z * max(gB.w, 0.5) * 2.0;
            float  dT  = tex2Dlod(sTerr, float4(uvf, 0, 0)).r;
            float  m   = 1.0 / (gTr.z * 0.1);
            float  gate = max(terr, away) * saturate((sf.z - dT) * m - 1.0);
            gateV = gate;
            [branch] if (gCh.z > 0.5)
            {
                float dL = min(min(tex2Dlod(sFarL, float4(uvf, 0, 0)).r,
                                   min(tex2Dlod(sFarL, float4(uvf + float2(-o, -o), 0, 0)).r,
                                       tex2Dlod(sFarL, float4(uvf + float2( o, -o), 0, 0)).r)),
                               min(tex2Dlod(sFarL, float4(uvf + float2(-o,  o), 0, 0)).r,
                                   tex2Dlod(sFarL, float4(uvf + float2( o,  o), 0, 0)).r));
                through = max(saturate((dT - dL) * m - 1.0), saturate((sf.z - dL) / gTr.w - 1.0)) * gate;
            }
            float dS = min(min(tex2Dlod(sShadow, float4(uvf, 0, 0)).r,
                               min(tex2Dlod(sShadow, float4(uvf + float2(-o, -o), 0, 0)).r,
                                   tex2Dlod(sShadow, float4(uvf + float2( o, -o), 0, 0)).r)),
                           min(tex2Dlod(sShadow, float4(uvf + float2(-o,  o), 0, 0)).r,
                               tex2Dlod(sShadow, float4(uvf + float2( o,  o), 0, 0)).r));
            throughS = max(saturate((dT - dS) * m - 1.0), saturate((sf.z - dS) / gTr.w - 1.0)) * gate;
            // In units of the margin (0.1 of terrainBias): 100 of them full.
            depthsV = float3(saturate((dT - dS) * m * 0.01), saturate((dS - dT) * m * 0.01), saturate((sf.z - dT) * m * 0.01));
        }
    }
    // Solid things stop the sun; leaves stop leafShade of it, and hills terrainShade. They multiply
    // (2026-10-02): a tree's shade shows inside a mountain's, as a fence's does. The larger of the two was
    // taken until then, and with trees and terrain in one map a tree under a mountain's shade added nothing.
    float leaf  = (1.0 - lerp(leafF, leafN, wn)) * (1.0 - through);
    float shade = 1.0 - lerp(lerp(lerp(litF, litM, wm), litN, wn), 1.0, throughS) * (1.0 - gCh.x * leaf) * (1.0 - gTr.x * terr);
    // Debug 4 and 5 (2026-10-03): where the shade comes from, and where the check above takes it away.
    if (gL.y > 5.5)
        return float4(depthsV, 1.0);                                       // debug 6: caster before the hill, behind it, point behind it
    if (gL.y > 4.5)
        return float4(throughS, through, gateV, 1.0);                      // debug 5: dropped solid, leaves, the check on
    if (gL.y > 3.5)
        return float4(1.0 - lerp(lerp(litF, litM, wm), litN, wn), 1.0 - lerp(leafF, leafN, wn), terr, 1.0);   // debug 4: solid, leaves, hills
    if (gL.y > 2.5)
        return float4(body, body, body, 1.0);                              // debug 3: the bodies it finds
    // The units' own shade, darkened again on top of the world's. The map holds no ground, so it needs none
    // of the ground's slack. The extra fades in over unitGap from the unit along the sun instead: a unit
    // adds little to its own back, and the shadow stays on its feet. Where the centre sees no unit (the
    // outer edge of the soft shadow), it counts in full.
    // It fades out again where the shade lies more than unitDrop yards under the unit (2026-10-01): a shadow
    // on the ground lies within the unit's height, whatever the sun's, and the water under a bridge, already
    // in the bridge's shade, showed the player's shape darker. At the outer edge the nearest unit among the
    // corner taps is taken, so no ring is left.
    float unit = 0.0, unitShade = 0.0;
    [branch] if (gU.w > 0.5 && wn > 0.0)
    {
        // The unit's depth from the four texels around the point, blended by where it falls between them,
        // leaving out texels with no unit (2026-10-01). One texel alone stepped away, below and along at
        // each texel of the half-size map: blocks on the character's shaded side.
        float2 uu   = float2(sn.x * 0.5 + 0.5, 0.5 - sn.y * 0.5);
        float2 ut   = uu / gU.z - 0.5;
        float2 uf   = frac(ut);
        float2 ub   = (ut - uf + 0.5) * gU.z;
        float4 d4   = float4(tex2Dlod(sUnit, float4(ub, 0, 0)).r, tex2Dlod(sUnit, float4(ub + float2(gU.z, 0.0), 0, 0)).r,
                             tex2Dlod(sUnit, float4(ub + float2(0.0, gU.z), 0, 0)).r, tex2Dlod(sUnit, float4(ub + gU.zz, 0, 0)).r);
        float4 w4   = float4((1.0 - uf.x) * (1.0 - uf.y), uf.x * (1.0 - uf.y), (1.0 - uf.x) * uf.y, uf.x * uf.y)
                    * step(d4, 0.99999);
        float  wsum = dot(w4, 1.0);
        float  du   = wsum > 1e-4 ? dot(w4, d4) / wsum : 1.0;
        float  away = du >= 0.99999 ? 1.0 : lerp(1.0, smoothstep(0.0, 1.0, saturate((sn.z - du) * gU2.x)), bodyS);
        [branch] if (du >= 0.99999)
        {
            float o = gU.z * max(gB.w, 0.5);
            du = min(min(tex2Dlod(sUnit, float4(uu + float2(-o, -o), 0, 0)).r, tex2Dlod(sUnit, float4(uu + float2(o, -o), 0, 0)).r),
                     min(tex2Dlod(sUnit, float4(uu + float2(-o,  o), 0, 0)).r, tex2Dlod(sUnit, float4(uu + float2(o,  o), 0, 0)).r));
        }
        float below = du >= 0.99999 ? 0.0 : (sn.z - du) * gU2.y;
        // The units' shade, from their map alone: the near map leaves them out while this map is drawn, so
        // a character's shadow has one outline (2026-10-01). On a body it takes the near map's slack and
        // sunOffset, against a surface shading itself. Off a body it takes a quarter of a texel and no
        // sunOffset: the map holds no ground, so the ground cannot shade itself in it, and the shadow
        // reaches the feet, where the near map's slack (5.5 texels with a low sun) left a gap.
        float4 su = float4(sn.xy, sn.z + gBody.z * (1.0 - bodyS), sn.w);
        float  us = 1.0 - Lit5(sUnit, su, float2(0.0, 0.0), lerp(gU.y * 0.25, gNB.x, bodyS), gU.z);
        unitShade = us * wn;
        unit = us * wn * away * (1.0 - smoothstep(gU2.z, gU2.w, below));
        // Nor on a surface in the shade of the world behind the unit (2026-10-01): the underside of a bridge's
        // deck, a yard under the player standing on it, and the side of the bridge under its edge. The facing
        // comes from the depth, which on a model gives each triangle's flat facing (see the top of this file):
        // applied to every surface facing away from the sun, it put blocks on the character's back. So the
        // facing counts only where the surface cannot be the unit's own:
        // - it faces down, half a yard or more under the unit (the deck's underside);
        // - it faces away from the sun, 1.5 yards or more behind the unit along the sun, more than a
        //   character is thick (the side of the bridge).
        // The ground faces up and keeps it.
        float along = below / max(gSun.z, 0.1);
        [branch] if (unit > 0.0 && (below > 0.3 || along > 1.5))
        {
            float3 Nu = N;
            [branch] if (gT.z < 0.5)
            {
                Nu = cross(Near(uv, raw, P, float2(0.0, gZ.w)), Near(uv, raw, P, float2(gZ.z, 0.0)));
                Nu = Nu / max(length(Nu), 1e-8);
                Nu = dot(Nu, P) > 0.0 ? -Nu : Nu;
            }
            float off = max(smoothstep(0.5, 0.8, -Nu.z) * smoothstep(0.3, 0.8, below),
                            (1.0 - smoothstep(-0.02, 0.03, dot(Nu, gSun.xyz))) * smoothstep(1.5, 2.5, along));
            unit *= 1.0 - off;
        }
    }
    // Character Backside Shadow ([sunshadows] bodyShade, 2026-10-01): the shade on a body, scaled. Only the
    // units' own (2026-10-04): the world's maps leave the units out, so their shade on a body is a hill's, a
    // house's or a tree's, and stays in full. Scaled as a whole, a horse, a trough and a fence post within 3
    // yards of the player (a unit's, by the Charger's rule) came out lit inside a hill's shade.
    {
        float k = lerp(1.0, gBody.x, body);
        shade = max(shade, unitShade * k);
        unit *= k;
    }
    if (gL.y > 1.5)
        return float4(1.0 - leaf, 1.0 - leaf, 1.0 - leaf, 1.0);            // debug 2: the leaves alone
    // The game's fog at this depth: a fogged pixel shows the fog colour, not what the shade falls on.
    float vz    = dot(P, gV.xyz) + gV.w;
    float clear = (1.0 - (gFog.z > 0.5 ? saturate((vz - gFog.x) * gFog.y) : 0.0)) * seen;
    // Shadow on Water (gWt.w, 2026-10-06, the owner: a ship's shadow lay on the open sea as hard and dark as on the
    // ground, and over the harbour's bed a dock cast none on the water beside it). Water with no bed in view: the
    // shade found is the surface's own, scaled. With a bed: the bed keeps its shade through the water, and the
    // surface's own (SurfaceShade) is added at that strength.
    float sS = 0.0;
    [branch] if (g_underWater && gWt.w > 0.0)
        sS = SurfaceShade(Ps) * gWt.w;
    if (g_onWater && !g_underWater)
    {
        shade *= gWt.w;
        unit  *= gWt.w;
    }
    float  dark  = max(shade, unit);
    // Under the water only the bed's shade fades with the water (seen); the Sunlight and the sun tint fall on the
    // surface, so they go by the surface's fog, and by the bed's shade only as far as the bed is seen (2026-10-06).
    // Faded with the shade, they left the water over a ridge without them while the open sea beside it, with no bed
    // under it, had them in full: an edge in the ridge's shape (Debug View 28, the owner). Away from the water seen
    // is 1 and the surface is the point, as before.
    float vzS    = dot(Ps, gV.xyz) + gV.w;
    float clearS = 1.0 - (gFog.z > 0.5 ? saturate((vzS - gFog.x) * gFog.y) : 0.0);
    float sunK   = lerp(1.0, 1.0 - dark, seen) * (1.0 - sS);   // the share lit
    float  f     = (1.0 - gSun.w * shade * clear) * (1.0 - gU.x * unit * clear) * (1.0 - gSun.w * sS * clearS) *
                   (1.0 + gL.x * sunK * clearS);
    f = (f >= 0.0 && f <= 2.0) ? f : 1.0;
    if (gL.y > 0.5)
        return float4(f, f, f, 1.0);                                       // debug: the shade in grey
    // Shade takes the sky's cool colour and sunlight a warm one ([sunshadows] shadeTint, sunTint).
    float3 c = f * lerp(1.0, gShC.rgb, gShC.w * max(dark * clear, sS * clearS)) * lerp(1.0, gSuC.rgb, gSuC.w * sunK * clearS);
    return float4(saturate(c * 0.5), 1.0);
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
    if (!ss.enabled || g_failed || (ss.strength <= 0.0f && ss.sunlight <= 0.0f && !ss.debug) || !VolumeLightActive())
    {
        if (logThis)
            Log("sunshadows: not drawn: %s", !ss.enabled ? "off" : g_failed ? "failed earlier" :
                !VolumeLightActive() ? "the volumetric light is not drawing" : "strength and sunlight 0");
        return false;
    }

    IDirect3DTexture9* depth  = DepthWorldTexture();
    IDirect3DTexture9* shadow = ShadowTexture();
    IDirect3DTexture9* nearTex = nullptr;
    D3DMATRIX nearVP = {};
    float nearRange = 0.0f;
    const bool haveNear = ShadowNear(nearTex, nearVP, nearRange);
    IDirect3DTexture9* midTex = nullptr;
    D3DMATRIX midVP = {};
    float midRange = 0.0f;
    const bool haveMid = ShadowMid(midTex, midVP, midRange);
    IDirect3DTexture9* terrMap = ShadowFarTerrain();
    IDirect3DTexture9* nearLeaf = haveNear ? ShadowNearLeaves() : nullptr;
    IDirect3DTexture9* farLeaf  = ShadowFarLeaves();
    IDirect3DTexture9* unitMap  = haveNear ? ShadowNearUnits() : nullptr;
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

    // Not faded as the sun or the moon sets: below [sunshadows] riseFrom the shadows' light climbs back up
    // instead (ShadowSunDirection), and with neither up the shadows are short, as at noon. With riseFrom 0
    // they fade over the last 6 degrees, as before. At night [sunshadows] night, not [night] strength.
    const float sunset = (ss.riseFrom > 0.0f || ss.lock ? 1.0f :
                          realSun[2] > 0.0f ? (realSun[2] < 0.1f ? realSun[2] / 0.1f : 1.0f) : 0.0f) *
                         NightScale(ss.night);
    // Indoors ([sunshadows] indoor): faded to that share over half a second while the player is in one of a
    // building's indoor groups.
    static float  inside = 0.0f;
    static double insideAt = 0.0;
    {
        float pl[3];
        const bool in = ClientPlayer(pl) && MapIndoors(pl);
        const double now = Now();
        const float step = insideAt > 0.0 ? static_cast<float>((std::min)(now - insideAt, 0.5) / 0.5) : 1.0f;
        insideAt = now;
        inside += ((in ? 1.0f : 0.0f) - inside) * step;
    }
    const float keep     = 1.0f - inside * (1.0f - ss.indoor);
    const float strength = ss.debug ? 1.0f : ss.strength * 0.01f * sunset * keep;
    const float sunlight = ss.debug ? 0.0f : ss.sunlight * sunset * keep;
    // World shadows off: the terrain's baked shadow is kept, the ground's only shade then.
    g_share = ss.debug ? 1.0f : ss.world ? sunset : 0.0f;
    if (strength <= 0.0f && sunlight <= 0.0f)
    {
        if (logThis)
            Log("sunshadows: not drawn: strength 0 (indoors %.2f, kept %.2f; sun height and night %.2f)", inside, keep,
                sunset);
        return false;
    }

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
    d->SetTexture(dev, 5, reinterpret_cast<IDirect3DBaseTexture9*>(unitMap ? unitMap : shadow));
    IDirect3DTexture9* bodyMask = BodyMaskTexture();
    d->SetTexture(dev, 6, reinterpret_cast<IDirect3DBaseTexture9*>(bodyMask ? bodyMask : depth));
    d->SetTexture(dev, 7, reinterpret_cast<IDirect3DBaseTexture9*>(haveMid ? midTex : shadow));
    d->SetTexture(dev, 8, reinterpret_cast<IDirect3DBaseTexture9*>(terrMap ? terrMap : shadow));
    d->SetTexture(dev, 9, reinterpret_cast<IDirect3DBaseTexture9*>(WaterUnderDepth() ? WaterUnderDepth() : depth));
    d->SetSamplerState(dev, 6, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 6, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    for (DWORD st = 0; st < 10; ++st)
    {
        d->SetSamplerState(dev, st, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, st, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(dev, st, D3DSAMP_SRGBTEXTURE, 0);
    }
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    // Off a map reads as far: lit.
    for (DWORD st = 1; st < 9; ++st)
    {
        if (st == 6)
            continue;   // the body mask, clamped above
        d->SetSamplerState(dev, st, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
        d->SetSamplerState(dev, st, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
        d->SetSamplerState(dev, st, D3DSAMP_BORDERCOLOR, 0xFFFFFFFF);
    }
    d->SetVertexShader(dev, g_vs);
    d->SetPixelShader(dev, g_ps);

    const float span = 2.0f * ShadowMapDepth() - 1.0f;       // the shadow map's z range, yards
    float minZ = 0.0f, maxZ = 1.0f;
    ShadowWorldDepthRange(minZ, maxZ);
    float pc[140] = {};
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
    // The normal offset is one distance on every map (2026-10-02): normalBias texels of the finest map. In
    // texels of each map it grew 3 times on the middle map and 8 on the far one, and moved a point near the
    // top of a merlon's shaded side clear of the merlon: its shade went missing past the near map.
    const float midRangeNow = haveMid ? midRange : 0.0f;
    float finest = farTex;
    if (haveNear) finest = (std::min)(finest, nearRange * 2.0f / size);
    if (haveMid)  finest = (std::min)(finest, midRangeNow * 2.0f / size);
    const float offYards = ss.normalBias * finest;
    pc[40] = (biasTex * farTex + ss.minGap) / span; pc[41] = offYards; pc[42] = 1.0f / size; pc[43] = ss.softness;
    pc[44] = sunlight; pc[45] = static_cast<float>(ss.debug);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc[48 + r * 4 + c] = nearVP.m[r][c];
    pc[64] = (biasTex * nearTex_ + ss.minGap) / span; pc[65] = offYards; pc[66] = 1.0f / size;
    pc[67] = haveNear ? 1.0f : 0.0f;
    // The largest slope, 8 yards of depth a yard, in each map's units: depth (0..1 over span) per uv
    // (0..1 over the map's width).
    pc[68] = 8.0f * g_cfg.shadow.range * 2.0f / span;
    pc[69] = 8.0f * nearRange * 2.0f / span;
    pc[70] = 8.0f * midRange * 2.0f / span;
    pc[72] = ss.slope;
    pc[73] = ss.sunOffset;
    pc[74] = (ss.slope > 0.0f || ss.normalBias > 0.0f) ? 1.0f : 0.0f;
    pc[76] = view.m[0][2]; pc[77] = view.m[1][2]; pc[78] = view.m[2][2]; pc[79] = view.m[3][2];
    // The shade fades with the game's fog, so a shaded tree at the fog wall does not come out darker than it.
    float fogStart = 0.0f, fogEnd = 0.0f;
    if (WorldFog(fogStart, fogEnd) && fogEnd > fogStart + 1.0f)
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
    UnitColour(ss.shadeColor, &pc[92]); pc[95] = ss.debug ? 0.0f : ss.shadeTint * keep;
    UnitColour(ss.sunColor, &pc[96]);   pc[99] = ss.debug ? 0.0f : ss.sunTint * keep;
    // The units' map: the near map's size and texel (half its size until 2026-10-01).
    const float unitTex = nearRange * 2.0f / size;
    pc[100] = ss.debug ? 0.0f : ss.unitStrength * 0.01f * sunset * keep;
    pc[101] = unitTex / span;                                   // one texel: no ground in this map
    pc[102] = 1.0f / size;                                      // one texel (uv): the near map's size
    pc[103] = unitMap ? 1.0f : 0.0f;
    pc[104] = ss.unitGap > 0.0f ? span / ss.unitGap : 1e6f;
    pc[105] = span * sunDir[2];                                 // yards straight down per map unit along the sun
    pc[106] = ss.unitDrop; pc[107] = ss.unitDrop + 3.0f;
    pc[108] = ss.bodyShade * 0.01f;
    pc[109] = bodyMask ? 1.0f : 0.0f;
    pc[110] = ss.sunOffset / span;                              // undone off a body, in the units' map
    // The middle map: its texel, and the same bias and normal offset in texels of it.
    const float midTex_ = midRange * 2.0f / size;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc[112 + r * 4 + c] = midVP.m[r][c];
    pc[128] = (biasTex * midTex_ + ss.minGap) / span; pc[129] = offYards; pc[130] = 1.0f / size;
    pc[131] = haveMid ? 1.0f : 0.0f;
    pc[132] = ss.terrainShade;
    pc[133] = terrMap ? 1.0f : 0.0f;
    // The terrain's slack: at least [sunshadows] terrainBias yards. A hill shades from yards away, and at the
    // far map's texel the ground near you shaded itself in faint bands (2026-10-02).
    pc[134] = ss.terrainBias / span;
    // How far a caster's shade carries inside a hill's ([sunshadows] hillCarry, 2026-10-04); 0 is no limit.
    pc[135] = (ss.hillCarry > 0.0f ? ss.hillCarry : 1.0e6f) / span;
    // The water (c34): the depth under it, and how fast it hides the bed: as the water shader hides the bed itself
    // (2026-10-06; 0.25 a yard / Water Clarity until then, some 2.5 times sooner: a ship's shade on the sea floor
    // in Stormwind's harbour was too faint to see while the floor showed clearly).
    IDirect3DTexture9* under = WaterUnderDepth();
    pc[136] = under ? 1.0f : 0.0f;
    pc[137] = WaterBedFade() > 0.0f ? WaterBedFade() : 0.25f / g_cfg.water.clarity;
    pc[139] = ss.water;                                              // Shadow on Water
    d->SetPixelShaderConstantF(dev, 0, pc, 35);

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
    d->SetTexture(dev, 5, nullptr);
    d->SetTexture(dev, 6, nullptr);
    d->SetTexture(dev, 7, nullptr);
    d->SetTexture(dev, 8, nullptr);
    d->SetTexture(dev, 9, nullptr);

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
            "texel, near map %s %.3f yd a texel, middle map %s %.3f yd a texel; bias %.1f texels (set %.1f, by the "
            "sun's height), sunOffset %.2f yd, normalBias %.1f texels (%.3f yd on every map), slope %.2f, softness %.1f; units' map %s, %.2f "
            "more (fades in over %.2f yd)",
            strength, sunset, sunDir[0], sunDir[1], sunDir[2], farTex, haveNear ? "on," : "off,", nearTex_,
            haveMid ? "on," : "off,", midTex_, biasTex, ss.bias, ss.sunOffset, ss.normalBias, offYards, ss.slope, ss.softness,
            unitMap ? "on" : "off", pc[100], ss.unitGap);
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

// The shaders this pass compiles, as it compiles them, for the cache's worker (shadercache.cpp, 2026-10-06).
void SunShadowsShaderList()
{
    ShaderPrecompile("sunshadows_vs", kVsHlsl, "vs_3_0");
    ShaderPrecompile("sunshadows_ps", kPsHlsl, "ps_3_0");
}
