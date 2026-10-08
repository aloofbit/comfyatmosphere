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
// The fog (2026-09-30). The same march carries a fog: thick at the ground and thinning upward, which
// darkens what lies behind it (transmittance) and is lit by the sun (through the shadow map, as the air
// above) and by the sky. The air of [volume] density only adds light, as before, so with the fog off the
// picture is the light alone, as it was. The target holds the sun's part (r), the sky's part (g), the
// transmittance (b) and the distance (a); the composite multiplies the world by the transmittance and adds
// the two parts in their colours. Past maxDistance the rest of each line of sight is fogged in one closed
// form, unshadowed; the sky and the far scenery drawn with it end at [fog] skyDistance.
//
// The fog is broken into patches by a tiling 3D noise (made once on the CPU, 64 texels a side), fixed in
// the world and carried along by the wind ([fog] windDeg, windSpeed), with a slow rise so the patches
// change shape as they go. Patchiness 0 is an even fog; at 1 the thick patches hold twice the fog and the
// gaps between them none, so the fog on average stays as thick as Fog Density says.
//
// The ground under the fog (2026-09-30): a texture of 128 x 128 cells of 8 yards around you, made on the CPU
// from the map files and remade as you move: the surface (the ground, or water over it), the water, and the
// surface smoothed over [fog] smoothRadius. The fog lies on a height between the smoothed surface and the
// surface itself ([fog] follow), so it fills valleys and still thins over hilltops; it is thicker where the
// surface lies below its surroundings ([fog] lowGround) or over water ([fog] water), by the larger of the two
// (2026-10-04; both multiplied until then). [fog] morning
// thickens it around dawn, and less around dusk.
//
// Under the terrain (2026-10-01): Ironforge, the Undercity and mines are buildings under the map's ground, and
// the map files hold only the terrain over them (in Ironforge, the mountain top 238 yards over the city). The
// fog thickens below its ground, so it filled the city. The texture's fourth channel holds the floor of the
// building under the terrain, where there is one: the highest floor no more than 4 yards over your feet. A point
// of the march 4 to 16 yards and more under the terrain takes that floor as its ground. Each point decides for
// itself, so walking out of the gate changes nothing at once.
//
// A point under the terrain with no floor found takes its own height as its ground, and a floor above the point
// counts as at the point (2026-10-05). Until then such a point kept the mountain top as its ground, 100 yards and
// more over it, and took the fog's cap. Seen from the road outside Ironforge's gate, the city filled with fog to
// the brim: until the city's building file loads there is no floor at all, and once it has loaded, the hall and
// the auction house stand higher than the 4 yards over your feet the floor is looked for. The same made the
// auction house braziers glow 24 times as bright through the hall's walls (FogThicknessAt, Lamps in Mist).
//
// The step loop needs Shader Model 3 (ps_2_0 fits about eight steps), and a ps_3_0 has to be paired
// with a vs_3_0, so the march, the temporal pass and the composite share a trivial full-screen vertex
// shader. The blur is ps_2_0 over pre-transformed quads, like the rest of comfyatmos.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "common.h"
#include "config.h"
#include "cover.h"
#include "depth.h"
#include "sun.h"
#include "mapterrain.h"
#include "shadow.h"
#include "sunshadows.h"
#include "volume.h"
#include "water.h"
#include "shadercache.h"

#include <algorithm>
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
sampler2D sShadow : register(s1);   // the sun's depth (INTZ), border = far: the solid things
sampler2D sLeaf   : register(s2);   // the leaves' map, the same camera
sampler2D sTerr   : register(s5);   // hills and mountains alone, the same camera (when gG.w is 1)
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
float4 gZ    : register(c10);       // the world viewport's MinZ, 1 / (MaxZ - MinZ); the game's fog start and end
float4 gL    : register(c11);       // steps along the ray, 1 / steps, this frame's noise offset, leafShade (0 = no leaf map)
float4 gF    : register(c12);       // fog: per yard at the ground, 1 / height, the ground's height (camera-relative), sky distance
float4 gG    : register(c13);       // fog: its sun scattering per unit of fog (/4pi), share of the far part in sun, reach,
                                    // 1 if there is a terrain map
float4 gN    : register(c14);       // the patches: where the camera is in the noise (wind included), 1 / tile size in yards
float4 gM    : register(c15);       // the patches: patchiness, how much flatter they are than wide
sampler3D sNoise : register(s3);    // the patches: tiling noise, wrapped; r large shapes, g small
sampler2D sGround : register(s4);   // the ground: surface (r), water (g), smoothed surface (b), from a reference height
float4 gGr   : register(c16);       // the ground texture: where the camera is in it (uv), 1 / its size in yards,
                                    // its reference height less the camera's (0 in .z: no texture, gF.z instead)
float4 gW    : register(c17);       // follow, 1 / lowDepth, lowGround, water
float4 gFg   : register(c18);       // the fog's phase anisotropy (Fog Toward the Sun), its glow along the horizon, the
                                    // water's horizon (the sine of its height from level, below negative), the sea's
                                    // height from the camera

// The patches at P (camera-relative): 1 on average; 0..2 at patchiness 1.
float Patches(float3 P)
{
    float3 q = P * gN.w * float3(1.0, 1.0, gM.y) + gN.xyz;
    float4 n = tex3Dlod(sNoise, float4(q, 0.0));
    float  v = n.r * 0.7 + n.g * 0.3;
    return lerp(1.0, 2.0 * smoothstep(0.3, 0.7, v), gM.x);
}

// The fog's extinction per yard at P (camera-relative). Below its base it is capped at 4 heights deep.
float FogAt(float3 P)
{
    float base = gF.z, mult = 1.0;
    [branch] if (gGr.z > 0.0)
    {
        float4 g = tex2Dlod(sGround, float4(P.xy * gGr.z + gGr.xy, 0, 0));
        base = lerp(g.b, g.r, gW.x) + gGr.w;
        // The larger of the two, not both (2026-10-04): low ground and water are one mist gathering. Multiplied,
        // the canal outside Stormwind's gate, water 30 yards under the city, took 2.5 x 3.1 and hid its far bank.
        mult = max(1.0 + gW.z * saturate((g.b - g.r) * gW.y), 1.0 + gW.w * g.g);
        // Under the terrain, on the floor of the building there (.a; the same as .r where there is none). Never
        // a floor above the point, and with no floor found the point's own height (2026-10-05): see the top.
        float under   = saturate((g.r + gGr.w - P.z - 4.0) * (1.0 / 12.0));
        float floorAt = (g.r - g.a >= 1.0) ? min(g.a + gGr.w, P.z) : P.z;
        base = lerp(base, floorAt, under);
        mult = lerp(mult, 1.0, under);
        // None under the water (2026-10-07): the water pass gives what lies under it its colour and its fade. A
        // creature under the sea writes the depth there (the water's draw over a body writes none), and the march
        // went on under the surface into the mist's thickest part, its cap times the water's boost: the creature
        // showed flat and pale through the water, and more with more fog. Gone over the first half yard down.
        mult *= 1.0 - g.g * saturate((g.r + gGr.w - P.z) * 2.0);
    }
    return gF.x * mult * exp(min((base - P.z) * gF.y, 4.0));
}

// How much more fog the ground at P gathers than the plain fog: for debug 7.
float Collects(float3 P)
{
    float4 g = tex2Dlod(sGround, float4(P.xy * gGr.z + gGr.xy, 0, 0));
    return (gGr.z > 0.0) ? max(1.0 + gW.z * saturate((g.b - g.r) * gW.y), 1.0 + gW.w * g.g) : 1.0;
}
)HLSL"
    // Split: MSVC takes no string literal longer than 16 KB (C2026).
    R"HLSL(
float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR
{
    if (gP.x > 1.5 && gP.x < 2.5)
        return float4(1.0, 0.0, 0.0, 1.0);                                // debug 2: the pass runs
    // Undo the viewport's squeeze; past the world's slice is sky or far horizon, so it clamps to
    // slightly short of far: exactly the far plane reconstructs with w = 0 in some frames, and the NaN that
    // makes, once the client's glow has blurred it over the image, turned whole frames black.
    float  raw  = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    float  d    = min(saturate((raw - gZ.x) * gZ.y), 0.99999);
    // The far horizon: drawn by a camera of its own (467 to 2112 yards here) into 0.955..0.96, past the
    // world's slice and short of the sky's clear 1. Its distance cannot be read with the world's camera, and it
    // is land, not sky: it takes the fog's full reach (2026-10-03). Taken for sky, its fog went by its height on
    // the screen once the sky's fog grew toward the horizon, and changed along the ridgeline.
    const bool farLand = raw > gZ.x + 1.0 / gZ.y + 1e-5 && raw < 0.99;
    // The sky: past the world's slice, and not far land (2026-10-05). It was d >= 0.9999, and with the world's
    // camera (near 0.1 yards, far 777) d passes 0.9999 at about 437 yards: everything past that took the sky's
    // fog, 75 yards and not the reach. In eastern Elwynn the fog stepped where a ridge's face crossed 450 yards
    // (0.88 of the light through above, 0.75 below), and the ridgeline showed twice.
    // Or the cleared depth itself, for a world slice that fills the whole range.
    const bool sky = (raw > gZ.x + 1.0 / gZ.y + 1e-5 && !farLand) || raw >= 0.999999;
    if (gP.x > 2.5 && gP.x < 3.5)
        return float4(d, raw, farLand ? 1.0 : 0.0, sky ? 1.0 : 0.0);      // debug 3: the depth it reads (the probe's
                                                                          // columns: raw, far land, sky)
    float2 ndc  = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp   = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    float3 P    = wp.xyz / max(wp.w, 1e-6);
    float  dist = length(P);
    float3 dir  = P / max(dist, 1e-4);
    // Into the game's fog (2026-10-03): a point going into it is the game's fog colour, and the game shows no
    // more of it. Its line of sight is carried on through our fog as far land's is, from halfway into the
    // game's fog to its end: the fog in front of it is then the fog above it, and it fades out as in the game.
    // A line stopped at a ridge 170 yards out gathered less fog and glow than the line above it to 200, and
    // a range of spires the game hides stood out as a dark second ridgeline.
    float  carry = (gZ.w > gZ.z + 1.0 && !sky && !farLand) ? smoothstep(lerp(gZ.z, gZ.w, 0.5), gZ.w, dist) : 0.0;
    float  distM = lerp(dist, max(dist, gG.z), carry);
    float  len  = min(distM, gP.y);
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

    // How far this line of sight gathers fog (2026-09-30): [fog] reach, and for the sky (and the far scenery
    // drawn with it, past the world's depth slice) the nearer of that and [fog] skyDistance. Over the last
    // 40% of it the fog fades out; past it the game's own fog is the far wall. At the ground the fog is as
    // thick all the way out, and at sea level the view ended a few hundred yards out.
    // Near the horizon the sky takes as much as the ground (2026-10-03): from the full reach at the horizon to
    // skyDistance 6 degrees up. With 75 yards on the sky and 200 on the sea just below it, the sea's fog, lit
    // from behind by a low moon, glowed as a bright line along the horizon with a hard edge above it.
    float  skyEnd   = gF.w > 0.0 ? lerp(gG.z, min(gF.w, gG.z), smoothstep(0.0, 0.1, dir.z)) : 0.0;
    float  reachEnd = sky ? skyEnd : gG.z;
    float  fadeK    = 1.0 / max(0.4 * reachEnd, 1.0);

    float  stepLen = len * gL.y;
    float  T   = 1.0;      // how much of what lies behind gets through the fog so far
    float  sun = 0.0;      // sunlight scattered toward the camera, before the phase: the light's
    float  sunF = 0.0;     // the same, the fog's, with its own phase (Fog Toward the Sun)
    float  amb = 0.0;      // sky light scattered toward the camera
    float  acc = 0.0;
    [loop] for (int i = 0; i < gL.x; ++i)
    {
        float  f   = (i + jit) * gL.y;
        float3 s   = lerp(s0, s1, f);
        float2 suv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
        float  hit = (s.z <= tex2Dlod(sShadow, float4(suv, 0, 0)).r + bias) ? 1.0 : 0.0;
        // Leaves stop [volume] leafShade of the sun: all of it by default (2026-09-30), so the shafts under a
        // canopy come through its gaps. At the ground's 0.6, 40% came through every leaf and the air under the
        // canopy was lit almost evenly.
        // Not where the solid map shades the step already (2026-10-07, perf-1): the leaves and hills only scale hit.
        [branch] if (gL.w > 0.0 && hit > 0.0)
            hit *= 1.0 - gL.w * ((s.z <= tex2Dlod(sLeaf, float4(suv, 0, 0)).r + bias) ? 0.0 : 1.0);
        // Hills and mountains, in a map of their own since 2026-10-02; until then in the leaves' map. The same
        // share, so the light is as it was.
        [branch] if (gG.w > 0.5 && hit > 0.0)
            hit *= 1.0 - gL.w * ((s.z <= tex2Dlod(sTerr, float4(suv, 0, 0)).r + bias) ? 0.0 : 1.0);
        // The map ends at a hard line, and a caster crossing it used to gain or lose its shade in one
        // frame: flashes in the distance as you walked. Shadowing fades out over the last tenth of the
        // map instead, so a caster dissolves in and out.
        float2 d   = abs(s.xy);
        float  inMap = saturate((1.0 - max(d.x, d.y)) * 10.0);
        float  lit = lerp(1.0, hit, inMap);
        acc += lit;
        // The fog in this step: w is the step's length as the fog within it lets through, so a thick step
        // does not add more light than it can.
        float  sf  = FogAt(dir * (f * len)) * saturate((reachEnd - f * len) * fadeK);
        [branch] if (sf > 0.0 && gM.x > 0.0)
            sf *= Patches(dir * (f * len));
        float  tr  = exp(-sf * stepLen);
        float  w   = sf > 1e-6 ? (1.0 - tr) / sf : stepLen;
        sun  += lit * gP.z * stepLen * T;
        sunF += lit * sf * gG.x * w * T;
        amb += sf * w * T;
        T   *= tr;
    }

    // Past maxDistance, the rest of the line of sight up to the reach: the integral of the height fog in
    // closed form, taken as lit by the sun (gG.y), with the fade-out taken at its middle.
    float tEnd = min((sky || farLand) ? reachEnd : distM, reachEnd);
    [branch] if (gF.x > 0.0 && tEnd > len)
    {
        // Taken as exponential between its two ends, which it is over flat ground.
        float fa  = FogAt(dir * len), fb = FogAt(dir * tEnd);
        float r   = fa / max(fb, 1e-12);
        float tau = (abs(r - 1.0) < 1e-3 || fa <= 0.0) ? 0.5 * (fa + fb) * (tEnd - len)
                                                        : (fa - fb) * (tEnd - len) / log(r);
        tau *= saturate((reachEnd - 0.5 * (len + tEnd)) * fadeK);
        float a   = 1.0 - exp(-max(tau, 0.0));
        sunF += gG.x * gG.y * a * T;
        amb += a * T;
        T   *= 1.0 - a;
    }

    if (gP.x > 6.5)
        return float4(saturate((Collects(dir * min(dist, gG.z)) - 1.0) / max(max(gW.z, gW.w), 1e-3)), 0.0, 0.0, 1.0);
                                                                          // debug 7: where the mist collects
    if (gP.x > 5.5)
        return float4(tex2Dlod(sShadow, float4(uv, 0, 0)).r, 0.0, 0.0, 1.0);   // debug 6: the shadow map
    if (gP.x > 4.5)
        return float4(acc * gL.y, 0.0, 0.0, 1.0);                         // debug 5: share of the ray in sun
    float c     = dot(dir, gSun.xyz);
    float g     = gSun.w;
    float phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5);
    // The fog's own phase (2026-10-08, the owner: the fog toward a low sun should be brightest): mist sends most of
    // the sunlight on forward. It took the light's (Light Toward the Sun, 0.07 there), next to even all round.
    float gf    = gFg.x;
    float phaseF = (1.0 - gf * gf) / pow(max(1.0 + gf * gf - 2.0 * gf * c, 1e-4), 1.5);
    // And a narrow glow right round the sun, half at 3 degrees, as mist's forward peak (2026-10-08, the owner: a
    // bright line under a sun on the sea's horizon). The game's own aura round the disc is about that size; on the
    // sky the fog is cleared over it (the composite), and under the horizon the fog over the water stayed an even
    // grey, so the aura ended in a flat bright edge. Now the fog under the sun glows as the aura does above it.
    // Where the sun glows (2026-10-08): over water, far things (the far land, the sky, past some 500 yards), and not over
    // land or what stands near by. The fog in front of land 200 yards off lies past the shadow's reach, counts as lit,
    // and showed the sun through an island that hid it (the owner). The ground texture marks the wet cells round the
    // camera (.g, a point off it counts as far); a point more than a yard over the water's surface (.r) is a boat, a
    // pier or a buoy, not the sea.
    // Only the far mesh and the sky count as far; until 2026-10-08 anything past 250 yards did, and the glow lay over
    // Stormwind's lighthouse (the owner). Past the ground texture (some 150 yards) a point is the sea only within a
    // yard or two of the sea's height (gFg.w, camera-relative; far under when no sea was found).
    float  farW   = (sky || farLand) ? 1.0 : 0.0;
    float  glowOn = 1.0;
    [branch] if (!sky && !farLand)
    {
        float2 gu  = P.xy * gGr.z + gGr.xy;
        float  wet;
        if (gGr.z > 0.0 && all(gu > 0.0) && all(gu < 1.0))
        {
            float4 gc = tex2Dlod(sGround, float4(gu, 0, 0));
            wet = saturate(gc.g * 1.5) * saturate(1.0 - (P.z - (gc.r + gGr.w) - 1.0) * 0.5);
        }
        else
            wet = saturate(1.0 - (P.z - gFg.w - 1.0) * 0.5);
        glowOn = wet;
    }
    phaseF += 50.0 * gf * pow(saturate(c), 500.0) * glowOn;
    // The glow along the water's horizon with the sun low (2026-10-08, the owner's reference of a sunset at sea): the
    // fog lights up along the top of the water across the whole horizon, brightest under the sun. Added for a view
    // near the water's horizon, by its bearing to the sun (full toward it, a little to the sides and behind),
    // with the sun 14 degrees up or less (Fog Horizon Glow).
    {
        float2 hv  = dir.xy / max(length(dir.xy), 1e-4);
        float2 hs  = gSun.xy / max(length(gSun.xy), 1e-4);
        float  az  = saturate(dot(hv, hs) * 0.5 + 0.5);
        float  low = saturate(1.0 - gSun.z / 0.25);
        // On the water's horizon, not on eye level (2026-10-08, the owner: the glow lay over a cliff above the sea's
        // edge). It fades out within 1.5 degrees above that line and over 7 below it, so it hugs the top of the water.
        // Until then it was 7 degrees each way round eye level, and its upper half lay on the sky and on the land.
        float  up  = dir.z - gFg.z;
        // Over far things (the far land, the sky, past some 500 yards) it reaches 7 degrees up, as it did round eye
        // level (2026-10-08): the game's far mesh, land across the sea 2000 yards off, stands over the water's horizon,
        // and with the tight edge it showed as a dark strip under the sky. Near terrain keeps the 1.5 degree edge.
        float  hzv = up > 0.0 ? saturate(1.0 - up / lerp(0.026, 0.12, farW)) : saturate(1.0 + up / 0.12);
        // Over water and far things, not over land or boats near by (glowOn above; the owner: the line went through
        // the beach, the cliffs and the boats).
        hzv *= glowOn;
        // Three times its first strength (2026-10-08): at 200% it was a faint thin line.
        phaseF += gFg.y * low * low * hzv * hzv * (1.0 + 6.0 * pow(az, 6.0));
    }
    // Whatever slipped through, nothing but a plain number in 0..16 leaves here: a NaN fails both tests.
    // The same for the distance, which is capped where 16-bit floats still hold it.
    float v     = sun * phase + sunF * phaseF;
    v   = (v >= 0.0) ? min(v, 15.9) : 0.0;   // capped, not zeroed: the glow round the sun can pass 16 (2026-10-08)
    amb = (amb >= 0.0 && amb < 16.0) ? amb : 0.0;
    T   = (T >= 0.0 && T <= 1.0) ? T : 1.0;
    dist = (dist >= 0.0 && dist < 30000.0) ? dist : 30000.0;
    return float4(v, amb, T, dist);
}
)HLSL";

    // 5-tap Gaussian along gD (one texel step), run once across and once down. A tap counts for less the
    // further its distance (alpha) is from the centre's: 5% nearer or further halves it.
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
    float wa1 = W(a1.a, c.a, 0.25),   wb1 = W(b1.a, c.a, 0.25);
    float wa2 = W(a2.a, c.a, 0.0625), wb2 = W(b2.a, c.a, 0.0625);
    float3 v = (c.rgb * 0.375 + a1.rgb * wa1 + b1.rgb * wb1 + a2.rgb * wa2 + b2.rgb * wb2)
             / (0.375 + wa1 + wb1 + wa2 + wb2);
    return float4(v, c.a);
}
)HLSL";

    // The temporal pass, at the march's resolution. The pixel's point is rebuilt from its direction and
    // distance, moved into the last camera's frame (the client draws camera-relative, so that is a shift
    // by how far the camera moved), and projected through the last frame's view-projection. The glow
    // found there is clamped to the range of this frame's 3x3 neighbourhood, so a glow the scene no longer
    // has cannot linger, and is dropped where its distance does not match the point's: that point was
    // hidden last frame, and the history there belongs to whatever hid it.
    const char* kTemporalHlsl = R"HLSL(
sampler2D sCur  : register(s0);     // this frame: sun (r), sky (g), transmittance (b), distance (a); point sampled
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

    float3 lo = cur.rgb, hi = cur.rgb;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
        {
            float3 n = tex2Dlod(sCur, float4(uv + float2(i, j) * gT.xy, 0, 0)).rgb;
            lo = min(lo, n);
            hi = max(hi, n);
        }

    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gInv0 + ndc.y * gInv1 + 0.5 * gInv2 + gInv3;
    float3 P   = normalize(wp.xyz / max(wp.w, 1e-6)) * cur.a;
    float3 Q   = P + gMove.xyz;
    float4 clip = Q.x * gPrev0 + Q.y * gPrev1 + Q.z * gPrev2 + gPrev3;
    if (clip.w <= 1e-3)
        return cur;
    float2 puv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    if (puv.x < 0.0 || puv.y < 0.0 || puv.x > 1.0 || puv.y > 1.0)
        return cur;

    float4 h      = tex2Dlod(sHist, float4(puv, 0, 0));
    float  expect = length(Q);
    float  same   = saturate(1.0 - abs(h.a - expect) / max(0.2 * expect, 1.0));
    float3 v      = lerp(cur.rgb, clamp(h.rgb, lo, hi), gMove.w * same);
    return float4(v, cur.a);
}
)HLSL";

    // Onto the world at full resolution: the four low-resolution texels around each pixel, weighted as a
    // bilinear filter would, and by how near each texel's distance is to the pixel's own.
    const char* kCompositeHlsl = R"HLSL(
sampler2D sGlow  : register(s0);    // sun (r), sky (g), transmittance (b), distance (a); point sampled
sampler2D sDepth : register(s1);    // the scene's depth (INTZ), full resolution
sampler2D sCover : register(s2);    // 1x1: how much of the sun is in view on screen (cover.cpp)
sampler2D sScene : register(s3);    // the screen before this pass, when gW.z is 1
float4 gInv0 : register(c0);        // rows of inverse(camera view-projection)
float4 gInv1 : register(c1);
float4 gInv2 : register(c2);
float4 gInv3 : register(c3);
float4 gZ    : register(c4);        // the world viewport's MinZ, 1 / (MaxZ - MinZ)
float4 gT    : register(c5);        // the glow's size, and one texel
float4 gC    : register(c6);        // the sun's colour x gain; a = 1 when sCover is bound
float4 gA    : register(c7);        // the sky's colour on the fog; a = fog debug (1 transmittance, 2 sky light)
float4 gDisc0 : register(c8);       // the way to the sun (by night the larger moon), 1 when known
float4 gDisc1 : register(c9);       // the way to the other moon, 1 by night when known
float4 gH    : register(c10);       // the sky's colour just over the horizon, at full brightness; w 1 when known
float4 gW    : register(c11);       // the water's height from the camera (yards), 1 when known, 1 when sScene is bound
float3 Tap(float2 base, float2 o, float2 f, float dist, inout float wsum)
{
    float4 s  = tex2Dlod(sGlow, float4((base + o + 0.5) * gT.zw, 0, 0));
    float2 bw = lerp(1.0 - f, f, o);
    float  w  = bw.x * bw.y / (1e-3 + abs(s.a - dist) / max(dist, 1e-3)) + 1e-6;
    wsum += w;
    return s.rgb * w;
}
float3 PointAt(float raw, float2 ndc)   // the point at the depth raw along the pixel's line of sight, camera-relative
{
    float  d  = min(saturate((raw - gZ.x) * gZ.y), 0.99999);
    float4 wp = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    return wp.xyz / max(wp.w, 1e-6);
}
float DistAt(float raw, float2 ndc)    // yards to it
{
    return min(length(PointAt(raw, ndc)), 30000.0);
}
bool OnWater(float raw, float2 ndc)    // whether the point at raw lies on the water's surface
{
    float3 p = PointAt(raw, ndc);
    return gW.y > 0.5 && abs(p.z - gW.x) < 2.0 + 0.01 * length(p);
}
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float  raw  = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    // A crack in the game's water (2026-10-08, the owner: white dots on the sea, sprinkled as the camera moved): a
    // pixel or two where the sky shows between its chunks. As the sky it took the far fog's full brightness, among
    // water that has the near fog's. And in the near water (the owner: the tips of the terrain), the sea bed through
    // a crack: farther than the water, it took thicker fog, a light dot. A pixel with something at least a fifth
    // nearer within 2 pixels on both sides of it, above and below or left and right, takes the nearer depth of the
    // two sides (past the world's slice: the world's slice on both sides). And the fog covers it whole (crack, at the
    // end): its own colour is the sky's or the sea bed's, a light dot through the near fog too.
    // Only in the water: the nearer side lies on the water's surface (gW). In a notch of a lighthouse's roof, the sky
    // between two parts of it was covered the same way, the fog's light alone, darker than the lighthouse behind its
    // fog: dark specks along its edges (the owner).
    // With the screen's copy (gW.z) a crack takes the colour 2 pixels off on its farther side, through the fog as
    // there: the fog's light alone was darker than lit water, a dark line where a lighthouse's rock met the sea.
    const float worldEnd = gZ.x + 1.0 / gZ.y + 1e-5;
    const float2 px = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    const float2 ndc0 = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float rawIn = raw;
    float2 fillAt = uv;   // where a crack takes its colour
    {
        float up = 1.0, dn = 1.0, lf = 1.0, rt = 1.0;
        for (int i = 1; i <= 2; ++i)
        {
            up = min(up, tex2Dlod(sDepth, float4(uv.x, uv.y - i * px.y, 0, 0)).r);
            dn = min(dn, tex2Dlod(sDepth, float4(uv.x, uv.y + i * px.y, 0, 0)).r);
            lf = min(lf, tex2Dlod(sDepth, float4(uv.x - i * px.x, uv.y, 0, 0)).r);
            rt = min(rt, tex2Dlod(sDepth, float4(uv.x + i * px.x, uv.y, 0, 0)).r);
        }
        const float sv = max(up, dn), sh = max(lf, rt);   // the farther of the two sides' nearest
        // The distances only where both sides of a pair are nearer: most pixels leave here.
        [branch] if (sv < raw || sh < raw)
        {
            const float own = DistAt(raw, ndc0);
            if (sv < raw && (raw > worldEnd ? sv < worldEnd : own > 1.25 * DistAt(sv, ndc0) + 1.0) && OnWater(sv, ndc0))
            {
                raw = sv;
                fillAt = uv + float2(0.0, (up >= dn ? -2.0 : 2.0) * px.y);
            }
            else if (sh < raw && (raw > worldEnd ? sh < worldEnd : own > 1.25 * DistAt(sh, ndc0) + 1.0) && OnWater(sh, ndc0))
            {
                raw = sh;
                fillAt = uv + float2((lf >= rt ? -2.0 : 2.0) * px.x, 0.0);
            }
        }
    }
    float  d    = min(saturate((raw - gZ.x) * gZ.y), 0.99999);
    float2 ndc  = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp   = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    float  dist = min(length(wp.xyz / max(wp.w, 1e-6)), 30000.0);
    float2 t    = uv * gT.xy - 0.5;
    float2 base = floor(t);
    float2 f    = t - base;
    float  wsum = 0.0;
    float3 sum  = Tap(base, float2(0, 0), f, dist, wsum) + Tap(base, float2(1, 0), f, dist, wsum)
                + Tap(base, float2(0, 1), f, dist, wsum) + Tap(base, float2(1, 1), f, dist, wsum);
    float3 m    = sum / wsum;
    float  T    = saturate(m.b);
    // The sun and the moons through the fog on the sky (2026-10-04): the fog dims what lies behind it, and the
    // discs went with it, a low moon most (the sky near the horizon takes the full reach). Round each disc the
    // fog lets the light through, 2.5 degrees in full, gone by 4, so the disc keeps its brightness in a foggy sky.
    // It was 4 in full and gone by 9 until 2026-10-08: with the sun on the sea's horizon (Tirisfal Glades) the
    // clear sky round the disc showed as a bright round halo in the fog, and as the rule takes only the sky, it
    // ended in a straight line on the water. The disc is some 2 degrees across (14 pixels a degree, 1152 wide).
    if (raw >= 0.99)   // the sky, not land past 437 yards or the far horizon (see the march)
    {
        const float3 vd = normalize(wp.xyz / max(wp.w, 1e-6));
        const float  k  = max(smoothstep(0.99756, 0.99905, dot(vd, gDisc0.xyz)) * gDisc0.w,
                              smoothstep(0.99756, 0.99905, dot(vd, gDisc1.xyz)) * gDisc1.w);
        T = lerp(T, 1.0, k);
    }
    if (gA.w > 2.5 && raw < rawIn)
        return float4(1.0, 0.0, 1.0, 0.0);                                 // fog debug 3: the cracks, magenta
    if (gA.w > 1.5 && gA.w < 2.5)
        return float4(gA.rgb * m.g, 1.0);                                  // fog debug 2: the sky light alone
    if (gA.w > 0.5 && gA.w < 1.5)
        return float4(T, T, T, 1.0);                                       // fog debug 1: the transmittance
    // Blended as ONE, SRCALPHA: the world times the transmittance, plus the light.
    // Far off, the fog takes the sky's colour just over the horizon at full brightness, not Fog Brightness's
    // (2026-10-08, the owner: a dark strip of fog on the open sea at the horizon). The far sea past our water, the far
    // land and the sky lie past the world's slice; nearer, from 300 yards to full at 1000. Fog Brightness (55 there)
    // made the thick fog on the far sea darker than the sky over it and the water under it.
    float  farV = raw > worldEnd ? 1.0 : smoothstep(300.0, 1000.0, dist);
    float3 skyA = lerp(gA.rgb, gH.rgb, farV * gH.w);
    float3 rgb  = gC.rgb * m.r * lerp(1.0, tex2Dlod(sCover, float4(0.5, 0.5, 0, 0)).r, gC.a) + skyA * m.g;
    // A crack: the colour beside it through the fog, or without the copy the fog's light alone.
    [branch] if (raw < rawIn)
        return float4(rgb + (gW.z > 0.5 ? T * tex2Dlod(sScene, float4(fillAt, 0, 0)).rgb : 0.0), 0.0);
    return float4(rgb, T);
}
)HLSL";

    // The march's debug stages carry no distance, so they are shown with a plain bilinear stretch.
    const char* kPlainCompositeHlsl = R"HLSL(
sampler2D s0 : register(s0);
sampler2D sCover : register(s2);   // 1x1: how much of the sun is in view on screen (cover.cpp)
float4 gC : register(c0);      // colour x gain; a = 1 when sCover is bound
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    return float4(gC.rgb * tex2D(s0, uv).r * lerp(1.0, tex2D(sCover, float2(0.5, 0.5)).r, gC.a), 0.0);
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
    Target g_scene;                           // the screen before the composite, for the cracks in the water
    D3DFORMAT g_sceneFmt = D3DFMT_UNKNOWN;
    bool   g_sceneFailed = false;             // the copy could not be made: not tried again until Reset
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

    // The fog's patches: a tiling 3D noise, made once (the CPU copy is kept, so a new device gets the same).
    constexpr int kNoise = 64;
    std::vector<uint32_t>     g_noiseData;
    IDirect3DVolumeTexture9*  g_noise = nullptr;
    bool                      g_noiseFailed = false;
    // The ground under the fog: 128 x 128 cells of 8 yards around you (see the top of the file).
    constexpr int   kGround     = 128;
    constexpr float kGroundCell = 8.0f;
    IDirect3DTexture9* g_ground = nullptr;
    bool   g_groundFailed = false;
    bool   g_groundValid  = false;
    float  g_groundAt[3]  = {};      // its centre, and the reference height its values are from
    double g_groundBuilt  = 0.0;
    int    g_groundMissing = 0;      // cells no tile covered when it was made
    int    g_groundWet     = 0;
    int    g_groundFloor   = 0;      // cells with a building's floor under the terrain
    float  g_groundSmooth  = 0.0f;   // the smoothRadius it was made with
    unsigned g_groundFiles = 0;      // MapFilesVersion when it was made
    std::vector<float> g_gSurf, g_gSmooth, g_gWet, g_gFloor;   // the texture's values, kept for FogThicknessAt
    // This frame's fog, for FogThicknessAt (the lamps draw after the fog, in the same frame).
    float  g_fogCam[3]   = {};
    bool   g_fogCamOk    = false;
    float  g_fogMorning  = 1.0f;
    double g_wind[3] = {};      // how far the wind has carried the patches, yards (kept small: wrapped by the tile)
    double g_windLast = 0.0;
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

    // Tiling gradient noise: the lattice wraps every `period` cells.
    float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

    uint32_t Hash(int x, int y, int z, uint32_t seed)
    {
        uint32_t h = seed ^ (static_cast<uint32_t>(x) * 0x8DA6B343u) ^ (static_cast<uint32_t>(y) * 0xD8163841u) ^
                     (static_cast<uint32_t>(z) * 0xCB1AB31Fu);
        h ^= h >> 13; h *= 0x5BD1E995u; h ^= h >> 15;
        return h;
    }

    float Grad(int x, int y, int z, int period, uint32_t seed, float fx, float fy, float fz)
    {
        const uint32_t h = Hash(((x % period) + period) % period, ((y % period) + period) % period,
                                ((z % period) + period) % period, seed) % 12u;
        static const float g[12][3] = { {1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},{1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},
                                        {0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1} };
        return g[h][0] * fx + g[h][1] * fy + g[h][2] * fz;
    }

    float Perlin(float x, float y, float z, int period, uint32_t seed)
    {
        const int xi = static_cast<int>(floorf(x)), yi = static_cast<int>(floorf(y)), zi = static_cast<int>(floorf(z));
        const float fx = x - xi, fy = y - yi, fz = z - zi;
        const float u = Fade(fx), v = Fade(fy), w = Fade(fz);
        float c[2][2][2];
        for (int k = 0; k < 2; ++k)
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i)
                    c[k][j][i] = Grad(xi + i, yi + j, zi + k, period, seed, fx - i, fy - j, fz - k);
        auto L = [](float a, float b, float t) { return a + (b - a) * t; };
        return L(L(L(c[0][0][0], c[0][0][1], u), L(c[0][1][0], c[0][1][1], u), v),
                 L(L(c[1][0][0], c[1][0][1], u), L(c[1][1][0], c[1][1][1], u), v), w);
    }

    // Octaves of the noise at base frequency f (cells across the tile), stretched to 0..1 over the tile.
    void Octaves(std::vector<float>& out, int f, int count, uint32_t seed)
    {
        out.assign(kNoise * kNoise * kNoise, 0.0f);
        float lo = 1e9f, hi = -1e9f;
        for (int z = 0; z < kNoise; ++z)
            for (int y = 0; y < kNoise; ++y)
                for (int x = 0; x < kNoise; ++x)
                {
                    float v = 0.0f, amp = 1.0f;
                    int fr = f;
                    for (int o = 0; o < count; ++o, fr *= 2, amp *= 0.5f)
                    {
                        const float k = static_cast<float>(fr) / kNoise;
                        v += amp * Perlin(x * k, y * k, z * k, fr, seed + o * 7919u);
                    }
                    out[(z * kNoise + y) * kNoise + x] = v;
                    lo = (std::min)(lo, v);
                    hi = (std::max)(hi, v);
                }
        for (float& v : out)
            v = hi > lo ? (v - lo) / (hi - lo) : 0.5f;
    }

    bool EnsureNoise(IDirect3DDevice9* dev)
    {
        if (g_noise)
            return true;
        if (g_noiseFailed)
            return false;
        if (g_noiseData.empty())
        {
            const double t0 = Now();
            std::vector<float> large, wisps;
            Octaves(large, 4, 3, 0x1234567u);    // the patches
            Octaves(wisps, 16, 2, 0x89ABCDEu);   // the wisps in them
            g_noiseData.resize(large.size());
            for (size_t i = 0; i < large.size(); ++i)
                g_noiseData[i] = 0xFF000000u | (static_cast<uint32_t>(large[i] * 255.0f + 0.5f) << 16) |
                                 (static_cast<uint32_t>(wisps[i] * 255.0f + 0.5f) << 8);
            Log("fog: patch noise made, %d texels a side, in %.0f ms", kNoise, 1000.0 * (Now() - t0));
        }
        if (FAILED(dev->lpVtbl->CreateVolumeTexture(dev, kNoise, kNoise, kNoise, 1, 0, D3DFMT_A8R8G8B8,
                                                     D3DPOOL_MANAGED, &g_noise, nullptr)) || !g_noise)
        {
            g_noise = nullptr;
            g_noiseFailed = true;
            Log("fog: could not create the patch noise texture: the fog is even");
            return false;
        }
        D3DLOCKED_BOX box = {};
        if (FAILED(g_noise->lpVtbl->LockBox(g_noise, 0, &box, nullptr, 0)))
        {
            SafeRelease(g_noise);
            g_noiseFailed = true;
            Log("fog: could not fill the patch noise texture: the fog is even");
            return false;
        }
        for (int z = 0; z < kNoise; ++z)
            for (int y = 0; y < kNoise; ++y)
                memcpy(static_cast<char*>(box.pBits) + z * box.SlicePitch + y * box.RowPitch,
                       &g_noiseData[(z * kNoise + y) * kNoise], kNoise * 4);
        g_noise->lpVtbl->UnlockBox(g_noise, 0);
        return true;
    }

    unsigned short FloatToHalf(float f)
    {
        uint32_t x;
        memcpy(&x, &f, 4);
        const uint32_t sign = (x >> 16) & 0x8000u;
        int e = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
        uint32_t m = x & 0x7FFFFFu;
        if (e <= 0)
            return static_cast<unsigned short>(sign);                 // too small: 0
        if (e >= 31)
            return static_cast<unsigned short>(sign | 0x7BFFu);       // too large: the largest
        return static_cast<unsigned short>(sign | ((static_cast<uint32_t>(e) << 10) + ((m + 0x1000u) >> 13)));
    }

    // Box blur of a kGround x kGround grid, radius r cells, separable.
    void Blur(std::vector<float>& g, int r)
    {
        if (r <= 0)
            return;
        std::vector<float> t(g.size());
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int j = 0; j < kGround; ++j)
                for (int i = 0; i < kGround; ++i)
                {
                    float s = 0.0f;
                    int n = 0;
                    for (int k = -r; k <= r; ++k)
                    {
                        const int ii = pass ? i : (std::min)((std::max)(i + k, 0), kGround - 1);
                        const int jj = pass ? (std::min)((std::max)(j + k, 0), kGround - 1) : j;
                        s += g[jj * kGround + ii];
                        ++n;
                    }
                    t[j * kGround + i] = s / n;
                }
            g.swap(t);
        }
    }

    // Makes the ground texture around `at` when it is missing, when you have moved a quarter of its width
    // from its centre or 6 yards up or down (the floor taken under the terrain goes by your height), or, while
    // some of it had no tile or more buildings came in, every second (tiles load on their own thread).
    void UpdateGround(IDirect3DDevice9* dev, const float at[3], float fallback)
    {
        if (g_groundFailed || !g_cfg.shadow.mapTerrain)
        {
            g_groundValid = false;
            return;
        }
        const double now = Now();
        const float half = 0.5f * kGround * kGroundCell;
        const float dx = at[0] - g_groundAt[0], dy = at[1] - g_groundAt[1];
        const bool moved = dx * dx + dy * dy > (0.25f * half) * (0.25f * half) ||
                           fabsf(at[2] - g_groundAt[2]) > 6.0f;
        const bool stale = g_groundMissing > 0 || MapFilesVersion() != g_groundFiles;
        if (g_groundValid && !moved && g_groundSmooth == g_cfg.fog.smoothRadius && !(stale && now - g_groundBuilt > 1.0))
            return;
        if (!g_ground && (FAILED(dev->lpVtbl->CreateTexture(dev, kGround, kGround, 1, 0, D3DFMT_A16B16G16R16F,
                                                             D3DPOOL_MANAGED, &g_ground, nullptr)) || !g_ground))
        {
            g_ground = nullptr;
            g_groundFailed = true;
            Log("fog: could not create the ground texture: the fog lies on the average ground around you");
            return;
        }
        const double t0 = Now();
        std::vector<float> surf(kGround * kGround), wet(kGround * kGround, 0.0f), flo(kGround * kGround);
        std::vector<char> hasFloor(kGround * kGround, 0);
        const unsigned files = MapFilesVersion();
        int missing = 0, nWet = 0, nFloor = 0;
        for (int j = 0; j < kGround; ++j)
            for (int i = 0; i < kGround; ++i)
            {
                const float x = at[0] + (i + 0.5f - 0.5f * kGround) * kGroundCell;
                const float y = at[1] + (j + 0.5f - 0.5f * kGround) * kGroundCell;
                float z, w, f;
                if (!MapGroundHeight(x, y, z))
                {
                    z = fallback;
                    ++missing;
                }
                // A floor counts when it is well under the terrain: a house on the ground is not a cave.
                else if (MapFloorHeight(x, y, at[2] + 4.0f, f) && f < z - 12.0f)
                {
                    flo[j * kGround + i] = f;
                    hasFloor[j * kGround + i] = 1;
                    ++nFloor;
                }
                if (MapWaterHeight(x, y, w) && w > z)
                {
                    z = w;
                    wet[j * kGround + i] = 1.0f;
                    ++nWet;
                }
                surf[j * kGround + i] = z;
            }
        // Cells under the terrain with no floor found (over lava, which is not a triangle, and along walls)
        // take the average of their neighbours' floors, up to 4 cells out, so the texture's filtering does not
        // mix a floor with the mountain top over it.
        for (int pass = 0; pass < 4; ++pass)
        {
            std::vector<char> grown = hasFloor;
            for (int j = 0; j < kGround; ++j)
                for (int i = 0; i < kGround; ++i)
                {
                    const int k = j * kGround + i;
                    if (hasFloor[k])
                        continue;
                    float sum = 0.0f;
                    int n = 0;
                    for (const int o : { k - 1, k + 1, k - kGround, k + kGround })
                        if (o >= 0 && o < kGround * kGround && (o % kGround == i || o / kGround == j) && hasFloor[o])
                        {
                            sum += flo[o];
                            ++n;
                        }
                    if (n > 0 && sum / n < surf[k] - 12.0f)
                    {
                        flo[k] = sum / n;
                        grown[k] = 1;
                    }
                }
            hasFloor.swap(grown);
        }
        for (int k = 0; k < kGround * kGround; ++k)
            if (!hasFloor[k])
                flo[k] = surf[k];   // none: the shader takes .a equal to .r as no floor
        std::vector<float> smooth = surf;
        const int smoothR = static_cast<int>(g_cfg.fog.smoothRadius / kGroundCell + 0.5f);
        Blur(smooth, smoothR);
        // Tall ground beside a point counts as lowDepth yards above it, no more (2026-10-04). Outside
        // Ironforge's gate the mountain the city lies under rose 100 yards and more within smoothRadius: the
        // smoothed ground sat far over the road, the fog's floor was lifted half of that ([fog] follow), the
        // whole road took the full low-ground boost, and the trees 60 yards off were lost. Stormwind's walls over
        // its canal did the same. A valley up to lowDepth deep still fills. Each cell with ground higher than
        // that in its window is averaged again with the heights cut; the rest keep the blur.
        const double cutFrom = Now();
        if (smoothR > 0)
        {
            const float cap = g_cfg.fog.lowDepth;
            std::vector<float> hi = surf, t(surf.size());
            for (int pass = 0; pass < 2; ++pass)   // the highest ground within the window: a separable max
            {
                for (int j = 0; j < kGround; ++j)
                    for (int i = 0; i < kGround; ++i)
                    {
                        float m = -1e30f;
                        for (int o = -smoothR; o <= smoothR; ++o)
                        {
                            const int ii = pass ? i : (std::min)((std::max)(i + o, 0), kGround - 1);
                            const int jj = pass ? (std::min)((std::max)(j + o, 0), kGround - 1) : j;
                            m = (std::max)(m, hi[jj * kGround + ii]);
                        }
                        t[j * kGround + i] = m;
                    }
                hi.swap(t);
            }
            // Every second cell of the window: a quarter of the work (the whole window took 25 to 30 ms at the
            // mountain, where most cells need it), and over 100 yards the average hardly moves.
            for (int j = 0; j < kGround; ++j)
                for (int i = 0; i < kGround; ++i)
                {
                    const int k = j * kGround + i;
                    const float top = surf[k] + cap;
                    if (hi[k] <= top)
                        continue;
                    float s = 0.0f;
                    int n = 0;
                    for (int dj = -smoothR; dj <= smoothR; dj += 2)
                    {
                        const int jj = (std::min)((std::max)(j + dj, 0), kGround - 1);
                        for (int di = -smoothR; di <= smoothR; di += 2)
                        {
                            const int ii = (std::min)((std::max)(i + di, 0), kGround - 1);
                            s += (std::min)(surf[jj * kGround + ii], top);
                            ++n;
                        }
                    }
                    smooth[k] = s / n;
                }
        }
        const double cutMs = 1000.0 * (Now() - cutFrom);
        Blur(wet, 2);   // a soft shore
        D3DLOCKED_RECT lr = {};
        if (FAILED(g_ground->lpVtbl->LockRect(g_ground, 0, &lr, nullptr, 0)))
            return;
        for (int j = 0; j < kGround; ++j)
        {
            auto* row = reinterpret_cast<unsigned short*>(static_cast<char*>(lr.pBits) + j * lr.Pitch);
            for (int i = 0; i < kGround; ++i)
            {
                const int k = j * kGround + i;
                row[i * 4 + 0] = FloatToHalf(surf[k] - at[2]);
                row[i * 4 + 1] = FloatToHalf(wet[k]);
                row[i * 4 + 2] = FloatToHalf(smooth[k] - at[2]);
                row[i * 4 + 3] = FloatToHalf(flo[k] - at[2]);
            }
        }
        g_ground->lpVtbl->UnlockRect(g_ground, 0);
        g_gSurf.swap(surf);
        g_gSmooth.swap(smooth);
        g_gWet.swap(wet);
        g_gFloor.swap(flo);
        memcpy(g_groundAt, at, sizeof(g_groundAt));
        g_groundValid   = missing < kGround * kGround;
        g_groundBuilt   = now;
        g_groundMissing = missing;
        g_groundWet     = nWet;
        g_groundFloor   = nFloor;
        g_groundFiles   = files;
        g_groundSmooth  = g_cfg.fog.smoothRadius;
        if (g_logNext || g_cfg.trace)
            Log("fog: ground texture made around (%.0f %.0f) in %.1f ms (%.1f of them cutting tall ground): %d of %d cells "
                "had no tile, %d wet, %d with a floor under the terrain", at[0], at[1], 1000.0 * (Now() - t0), cutMs,
                missing, kGround * kGround, nWet, nFloor);
    }

    // Dawn and dusk ([fog] morning): the fog thicker by up to `morning` at 6:00 and half of that at 20:00.
    float MorningScale()
    {
        float hour = 0.0f;
        if (g_cfg.fog.morning <= 0.0f || !ClientHour(hour))
            return 1.0f;
        auto bump = [hour](float at, float width)
        {
            float d = fabsf(hour - at);
            d = (std::min)(d, 24.0f - d);
            return expf(-(d / width) * (d / width));
        };
        return 1.0f + g_cfg.fog.morning * (bump(6.0f, 2.0f) + 0.5f * bump(20.0f, 1.5f));
    }

    // The screen's copy for the composite (2026-10-08), made again when the screen's size or format changes.
    bool EnsureScene(IDirect3DDevice9* dev, const D3DSURFACE_DESC& wd)
    {
        if (g_scene.tex && g_scene.w == wd.Width && g_scene.h == wd.Height && g_sceneFmt == wd.Format)
            return true;
        if (g_sceneFailed)
            return false;
        ReleaseTarget(g_scene);
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, wd.Width, wd.Height, 1, D3DUSAGE_RENDERTARGET, wd.Format,
                                                D3DPOOL_DEFAULT, &g_scene.tex, nullptr);
        if (SUCCEEDED(hr))
            hr = g_scene.tex->lpVtbl->GetSurfaceLevel(g_scene.tex, 0, &g_scene.surf);
        if (FAILED(hr))
        {
            Log("volume: no %ux%u screen copy (format %u, hr=0x%08X): a crack in the water shows the fog's light alone",
                wd.Width, wd.Height, static_cast<unsigned>(wd.Format), hr);
            ReleaseTarget(g_scene);
            g_sceneFailed = true;
            return false;
        }
        g_scene.w = wd.Width;
        g_scene.h = wd.Height;
        g_sceneFmt = wd.Format;
        return true;
    }

    void ReleaseDefaultPool()
    {
        ReleaseTarget(g_a);
        ReleaseTarget(g_b);
        ReleaseTarget(g_hist[0]);
        ReleaseTarget(g_hist[1]);
        ReleaseTarget(g_scene);
        g_sceneFailed = false;
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
                Log("volume: march output (debug %d) at screen points:%s", g_cfg.volume.debug, line);
                // Three columns down the middle of the screen (2026-10-05): what gets through (T) and the distance
                // at each point, to find where the fog steps across a ridgeline. In debug 0 only.
                if (g_cfg.volume.debug == 0 || g_cfg.volume.debug == 3)
                    for (float cx : { 0.35f, 0.55f, 0.75f })
                    {
                        char col[1024] = {};
                        int k = 0;
                        for (float cy = 0.20f; cy < 0.651f; cy += 0.015f)
                        {
                            const UINT x = static_cast<UINT>(cx * (g_a.w - 1)), y = static_cast<UINT>(cy * (g_a.h - 1));
                            const auto* px = reinterpret_cast<const unsigned short*>(static_cast<const char*>(lr.pBits) +
                                                                                    y * lr.Pitch) + x * 4;
                            if (g_cfg.volume.debug == 3)   // the depth, raw, and whether it is far land or sky
                                k += _snprintf_s(col + k, sizeof(col) - k, _TRUNCATE, " %.3f:%.5f%s%s", cy,
                                                 HalfToFloat(px[1]), HalfToFloat(px[2]) > 0.5f ? " far" : "",
                                                 HalfToFloat(px[3]) > 0.5f ? " sky" : "");
                            else
                                k += _snprintf_s(col + k, sizeof(col) - k, _TRUNCATE, " %.3f:T%.3f/%.0fyd", cy,
                                                 HalfToFloat(px[2]), HalfToFloat(px[3]));
                        }
                        Log("volume: column x %.2f, down the screen (y: what gets through / distance):%s", cx, col);
                    }
                sys->lpVtbl->UnlockRect(sys);
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

    // The sun's light on the fog when the fog draws alone: [volume] strength 25 times maxIntensity 3.0.
    constexpr float kFogOnlyGain = 0.75f;
    float g_fogSeaRel = -10000.0f, g_fogHorizon = 0.0f;   // the glows' sea height and horizon, for the probe

    bool FogOn()
    {
        return g_cfg.fog.enabled && g_cfg.fog.density > 0.0f;
    }

    // The ground under the fog, camera-relative: the average ground height within [fog] groundRadius of
    // you, from the map files, easing over 3 s, so walking over a ridge does not pop the fog. Without a
    // tile, your feet; without those, 2 yards under the camera.
    //
    // Under the map's ground (2026-10-01; see the top of the file): more than kUnderEnter yards under the
    // terrain where you stand, this ground is your feet; less than kUnderLeave, the map files again. The gap
    // keeps it from switching on a slope. It counts only where the ground texture is off: the texture holds
    // the floor under the terrain itself.
    constexpr float kUnderEnter = 12.0f, kUnderLeave = 6.0f;
    float g_fogBase = 0.0f;
    bool  g_fogHaveBase = false;
    bool  g_fogUnder = false;
    const char* g_fogBaseFrom = "none";

    float FogGround(const float cam[3])
    {
        static double last = 0.0;
        float pl[3], ground, here;
        const bool havePl = ClientPlayer(pl);
        const float* at = havePl ? pl : cam;
        const double now = Now();
        if (havePl && g_cfg.shadow.mapTerrain && MapGroundHeight(pl[0], pl[1], here))
            g_fogUnder = here - pl[2] > (g_fogUnder ? kUnderLeave : kUnderEnter);
        else if (!havePl)
            g_fogUnder = false;
        if (g_fogUnder)
        {
            if (!g_fogHaveBase || now - last > 2.0 || fabsf(pl[2] - g_fogBase) > 200.0f)
                g_fogBase = pl[2];
            else
                g_fogBase += (pl[2] - g_fogBase) * static_cast<float>(1.0 - exp(-(now - last) / 3.0));
            g_fogHaveBase = true;
            g_fogBaseFrom = "your feet, under the map's ground";
            last = now;
        }
        else if (g_cfg.shadow.mapTerrain && MapGroundBase(at, g_cfg.fog.groundRadius, ground))
        {
            if (!g_fogHaveBase || now - last > 2.0 || fabsf(ground - g_fogBase) > 200.0f)
                g_fogBase = ground;
            else
                g_fogBase += (ground - g_fogBase) * static_cast<float>(1.0 - exp(-(now - last) / 3.0));
            g_fogHaveBase = true;
            g_fogBaseFrom = "the map files, the water's surface over water";
            last = now;
        }
        else if (!g_fogHaveBase || now - last > 2.0)
        {
            g_fogBase = havePl ? pl[2] : cam[2] - 2.0f;
            g_fogBaseFrom = havePl ? "your feet" : "the camera";
        }
        return g_fogBase - cam[2];
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
    const bool fogOn = FogOn();
    if (!VolumeActive() || (v.strength <= 0.0f && !v.debug && !fogOn))
        return false;   // nothing to draw: no glow and no fog
    // The fog alone (Volumetric Light off, 2026-10-02): no shadow map is drawn, and every point of the fog is
    // taken as in the sun.
    const bool fogOnly = !VolumeLightActive();
    ++g_st.calls;

    IDirect3DTexture9* depth  = DepthWorldTexture();
    IDirect3DTexture9* shadow = fogOnly ? nullptr : ShadowTexture();
    float sunDir[3];
    D3DMATRIX view, proj, shadowVP = {};
    // The camera the depth was drawn with (see ShadowWorldCamera); sun.cpp's can be the sky's.
    const bool worldCam = ShadowWorldCamera(view, proj);
    const bool haveCam  = worldCam || SunCamera(view, proj);
    // With the fog alone, the matrix puts every point well off the map (x and y at 4), where the shader
    // takes it as lit and reads no texel.
    bool haveMatrix = true;
    if (fogOnly)
    {
        shadowVP.m[3][0] = shadowVP.m[3][1] = 4.0f;
        shadowVP.m[3][3] = 1.0f;
    }
    else
        haveMatrix = shadow && ShadowMatrix(shadowVP);
    unsigned* skip = !depth ? &g_st.noDepth : (!fogOnly && !shadow) ? &g_st.noShadow : !haveMatrix ? &g_st.noMatrix :
                     !SunDirection(sunDir) ? &g_st.noSun : !haveCam ? &g_st.noCam : nullptr;
    if (skip)
    {
        ++*skip;
        if (logThis)
            Log("volume: skipped: %s", !depth ? "no readable depth ([depth] enabled?)" :
                !shadow ? "no shadow map ([shadow] enabled?)" : "no sun or camera yet");
        return false;
    }

    // Faded out as the sun goes down, and turned down at night by [night] strength.
    const float sunset = (sunDir[2] > 0.0f ? (sunDir[2] < 0.1f ? sunDir[2] / 0.1f : 1.0f) : 0.0f) * NightScale();
    if (sunset <= 0.0f && !v.debug && !fogOn)
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
    for (DWORD s = 0; s < 3; ++s)
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
    d->SetSamplerState(dev, 2, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
    d->SetSamplerState(dev, 2, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
    d->SetSamplerState(dev, 2, D3DSAMP_BORDERCOLOR, 0xFFFFFFFF);
    const bool noMaps = fogOnly || (g_cfg.shadow.debugSkip & 2);
    IDirect3DTexture9* leaves = noMaps ? nullptr : ShadowFarLeaves();
    IDirect3DTexture9* terrMap = (fogOnly || !leaves) ? nullptr : ShadowFarTerrain();
    d->SetSamplerState(dev, 5, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
    d->SetSamplerState(dev, 5, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
    d->SetSamplerState(dev, 5, D3DSAMP_BORDERCOLOR, 0xFFFFFFFF);
    d->SetSamplerState(dev, 5, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 5, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 5, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(dev, 5, D3DSAMP_SRGBTEXTURE, 0);

    // --- march --------------------------------------------------------------------------------------
    d->SetRenderTarget(dev, 0, g_a.surf);
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(depth));
    d->SetTexture(dev, 1, reinterpret_cast<IDirect3DBaseTexture9*>(shadow));
    d->SetTexture(dev, 2, reinterpret_cast<IDirect3DBaseTexture9*>(leaves ? leaves : shadow));
    d->SetTexture(dev, 5, reinterpret_cast<IDirect3DBaseTexture9*>(terrMap));
    d->SetVertexShader(dev, g_vsMarch);
    d->SetPixelShader(dev, g_psMarch);
    const float half[4] = { -1.0f / g_a.w, 1.0f / g_a.h, 0.0f, 0.0f };
    d->SetVertexShaderConstantF(dev, 0, half, 1);
    const float span = 2.0f * ShadowMapDepth() - 1.0f;       // the shadow map's z range, yards
    float pc[56];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            pc[r * 4 + c]      = inv.m[r][c];
            pc[16 + r * 4 + c] = noMaps && !fogOnly ? 0.0f : shadowVP.m[r][c];
        }
    if (noMaps && !fogOnly)
    {
        // [shadow] debugSkip 2: the march as with the fog alone, every point off the map and lit.
        pc[28] = pc[29] = 4.0f;
        pc[31] = 1.0f;
    }
    pc[32] = sunDir[0]; pc[33] = sunDir[1]; pc[34] = sunDir[2]; pc[35] = v.anisotropy;
    // density / 4pi: the shader's Henyey-Greenstein term is left unnormalised to save the multiply.
    pc[36] = static_cast<float>(v.debug); pc[37] = v.maxDistance; pc[38] = fogOnly ? 0.0f : v.density * 0.0795775f; pc[39] = v.bias / span;
    float minZ = 0.0f, maxZ = 1.0f;
    ShadowWorldDepthRange(minZ, maxZ);
    pc[40] = minZ; pc[41] = (maxZ - minZ) > 1e-6f ? 1.0f / (maxZ - minZ) : 1.0f; pc[42] = 0.0f; pc[43] = 0.0f;
    // The game's fog start and end (c10.zw): a line of sight into it is carried on to the reach.
    {
        float gs = 0.0f, ge = 0.0f;
        if (WorldFog(gs, ge) && ge > gs + 1.0f)
        {
            pc[42] = gs;
            pc[43] = ge;
        }
    }
    const float steps = static_cast<float>(v.steps);
    // The noise offset turns by the golden ratio each frame, for the temporal pass to average. Without
    // that pass it stays put: noise that changes every frame and is never averaged shimmers.
    const bool temporal = v.smooth > 0.001f && v.debug < 2;
    const float turn = temporal ? static_cast<float>(fmod(g_frameNo * 0.6180339887, 1.0)) : 0.0f;
    pc[44] = steps; pc[45] = 1.0f / steps; pc[46] = turn; pc[47] = leaves ? g_cfg.volume.leafShade : 0.0f;
    const FogSettings& fs = g_cfg.fog;
    float cam[3] = {};
    const bool camRead = ClientCamera(cam);
    const float groundRel = camRead ? FogGround(cam) : -2.0f;
    const float morning = fogOn ? MorningScale() : 1.0f;
    g_fogMorning = morning;
    g_fogCamOk   = camRead;
    memcpy(g_fogCam, cam, sizeof(g_fogCam));
    pc[48] = fogOn ? fs.density * morning : 0.0f; pc[49] = 1.0f / fs.height; pc[50] = groundRel; pc[51] = fs.skyDistance;
    pc[52] = fs.sunLight * 0.0795775f; pc[53] = 1.0f; pc[54] = fs.reach; pc[55] = terrMap ? 1.0f : 0.0f;
    // The fog's sunlight and the light's own, each at its own gain (2026-10-08, the owner, looking into a low sun over
    // the sea in Tirisfal Glades). The march adds the two (gP.z and gG.x) and the composite multiplies the sum by one
    // gain. That gain was the light's: Light Strength (5 there), and faded out below a sun height of 0.1. So the fog
    // toward a low sun, lit from behind, showed next to none of its light, where it should be brightest. The fog now
    // takes its own gain times Fog Sunlight whatever Light Strength is, faded only as the sun goes under (below). The
    // light keeps its own strength and fade.
    const float lightGain = fogOnly ? 0.0f : (v.strength * 0.01f) * v.maxIntensity * sunset;
    // 0.25, not kFogOnlyGain (2026-10-08): with the light's tie and fade gone and the fog's own phase toward the sun,
    // 0.75 made the haze round a low sun some 50 times what it was, and washed the cliffs out (the owner).
    // Full until the sun's centre is 1 degree under the horizon, none at 2.6 under (2026-10-08): with it half set over
    // Stormwind's harbour (-0.010) the fog's light was down to a fifth, and the glows with it, while the game still
    // showed half the disc and its aura. It was full at 1 degree up and none at 1 under.
    const float fogSunGain = fogOn ? 0.25f * (std::min)((std::max)((sunDir[2] + 0.045f) / 0.028f, 0.0f), 1.0f) *
                                     NightScale() : 0.0f;
    const float sunGain = (std::max)(lightGain, fogSunGain);
    if (!v.debug)
    {
        pc[38] *= sunGain > 0.0f ? lightGain / sunGain : 0.0f;
        pc[52] *= sunGain > 0.0f ? fogSunGain / sunGain : 0.0f;
    }
    // The patches: the wind carries them; they rise slowly too, so they change shape as they go. Where the
    // camera is in the tiling noise is worked out here in doubles, so far from the world's origin the
    // shader still gets small numbers.
    const bool patches = fogOn && fs.patchiness > 0.0f && EnsureNoise(dev);
    float pn[8] = {};
    {
        const double now = Now();
        const double dt = g_windLast > 0.0 ? (std::min)(now - g_windLast, 0.25) : 0.0;
        g_windLast = now;
        const double a = fs.windDeg * 3.14159265358979 / 180.0;
        const double tile = 4.0 * fs.scale;   // the large octave has 4 cells across the tile
        // 0 degrees blows north (+x), 90 east (-y). The noise moves with the air, so it is read against it.
        g_wind[0] = fmod(g_wind[0] - cos(a) * fs.windSpeed * dt, tile);
        g_wind[1] = fmod(g_wind[1] + sin(a) * fs.windSpeed * dt, tile);
        g_wind[2] = fmod(g_wind[2] - 0.08 * fs.windSpeed * dt, tile);
        for (int i = 0; i < 3; ++i)
        {
            const double at = ((camRead ? cam[i] : 0.0) * (i == 2 ? fs.flatten : 1.0) + g_wind[i]) / tile;
            pn[i] = static_cast<float>(at - floor(at));
        }
        pn[3] = static_cast<float>(1.0 / tile);
        pn[4] = patches ? fs.patchiness : 0.0f;
        pn[5] = fs.flatten;
    }
    // The ground under the fog, around you (or the camera).
    float gr[8] = {};
    if (fogOn && camRead)
    {
        float pl[3];
        const float* at = ClientPlayer(pl) ? pl : cam;
        UpdateGround(dev, at, g_fogBase);
        if (g_groundValid)
        {
            const float span = kGround * kGroundCell;
            gr[0] = (cam[0] - g_groundAt[0]) / span + 0.5f;
            gr[1] = (cam[1] - g_groundAt[1]) / span + 0.5f;
            gr[2] = 1.0f / span;
            gr[3] = g_groundAt[2] - cam[2];
        }
    }
    gr[4] = fs.follow; gr[5] = 1.0f / fs.lowDepth; gr[6] = fs.lowGround; gr[7] = fs.water;
    d->SetPixelShaderConstantF(dev, 16, gr, 2);
    d->SetTexture(dev, 4, reinterpret_cast<IDirect3DBaseTexture9*>(gr[2] > 0.0f ? g_ground : nullptr));
    d->SetSamplerState(dev, 4, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 4, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 4, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(dev, 4, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(dev, 4, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(dev, 4, D3DSAMP_SRGBTEXTURE, 0);
    d->SetPixelShaderConstantF(dev, 0, pc, 14);
    d->SetPixelShaderConstantF(dev, 14, pn, 2);
    // The water's horizon as the camera sees it: the water toward the sun, 150 yards out (or 300, or under the
    // camera), seen 2000 yards off, where the game's far mesh meets the sky (it reaches 2112). Not at the game's fog
    // end, where our water ends (2026-10-08): the glow then sat on the near water, under a dark strip of far sea.
    // Level when no water is found.
    float horizon = 0.0f, seaRel = -10000.0f;   // seaRel: the sea's height from the camera, for the shader's glow
    if (camRead)
    {
        float sxy[2] = { sunDir[0], sunDir[1] };
        const float sl = sqrtf(sxy[0] * sxy[0] + sxy[1] * sxy[1]);
        if (sl > 1e-3f) { sxy[0] /= sl; sxy[1] /= sl; } else { sxy[0] = 1.0f; sxy[1] = 0.0f; }
        float wz = 0.0f;
        const bool wet = MapWaterHeight(cam[0] + sxy[0] * 150.0f, cam[1] + sxy[1] * 150.0f, wz) ||
                         MapWaterHeight(cam[0] + sxy[0] * 300.0f, cam[1] + sxy[1] * 300.0f, wz) ||
                         MapWaterHeight(cam[0], cam[1], wz);
        const float reachTo = 2000.0f;
        if (wet)
        {
            const float dz = wz - cam[2];
            seaRel = dz;
            horizon = dz / sqrtf(dz * dz + reachTo * reachTo);
            horizon = (std::min)((std::max)(horizon, -0.3f), 0.3f);
        }
    }
    g_fogSeaRel = seaRel; g_fogHorizon = horizon;
    const float fg[4] = { fs.toward, fs.horizonGlow, horizon, seaRel };
    d->SetPixelShaderConstantF(dev, 18, fg, 1);
    d->SetTexture(dev, 3, reinterpret_cast<IDirect3DBaseTexture9*>(patches ? g_noise : nullptr));
    for (DWORD k = D3DSAMP_ADDRESSU; k <= D3DSAMP_ADDRESSW; ++k)
        d->SetSamplerState(dev, 3, static_cast<D3DSAMPLERSTATETYPE>(k), D3DTADDRESS_WRAP);
    d->SetSamplerState(dev, 3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(dev, 3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(dev, 3, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(dev, 3, D3DSAMP_SRGBTEXTURE, 0);
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
    d->SetTexture(dev, 2, nullptr);
    d->SetTexture(dev, 3, nullptr);
    d->SetTexture(dev, 4, nullptr);
    d->SetTexture(dev, 5, nullptr);
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
        Log("volume: trace shadow near you:%s", ShadowNearChanges());
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

    // --- how much of the sun is in view on screen ([volume] occlusion, cover.cpp) -------------------
    // The shadow map holds only what lies within [shadow] depth of you, so a sun setting behind the far
    // horizon kept lighting the fog. With the sun on screen, the depth buffer shows whether terrain covers
    // it, however far away. Off screen, or in a debug view, it is not tested and counts as in view.
    IDirect3DTexture9* cover = nullptr;
    {
        float s4[4] = { sunDir[0], sunDir[1], sunDir[2], 0.0f }, clip[4];
        for (int c = 0; c < 4; ++c)
            clip[c] = s4[0] * camVP.m[0][c] + s4[1] * camVP.m[1][c] + s4[2] * camVP.m[2][c];
        const float w = clip[3];
        const float nx = w > 1e-4f ? clip[0] / w : 9.0f, ny = w > 1e-4f ? clip[1] / w : 9.0f;
        CoverInput in = {};
        in.px = 0.5f + 0.5f * nx;
        in.py = 0.5f - 0.5f * ny;
        in.test   = v.occlusion && !v.debug && fabsf(nx) <= 1.0f && fabsf(ny) <= 1.0f;
        in.aspect = wd.Height ? static_cast<float>(wd.Width) / static_cast<float>(wd.Height) : 1.0f;
        in.depth  = depth;
        cover = CoverMeasure(dev, kCoverVolume, in, g_cfg.rays.coverTime);
    }

    // --- composite onto the world -------------------------------------------------------------------
    // debug replaces the world with the glow alone, white, to see its shape.
    const DWORD col = g_cfg.volume.color;
    // With the sun low the light takes the sun's colour as the sky shows it (2026-10-08): the hue of the glow just over
    // the horizon (the sky dome, water.cpp), from the sun 14 degrees up to the horizon. Its brightest channel is 1, so
    // the light's strength is as it was.
    float lc[3] = { ((col >> 16) & 0xFF) / 255.0f, ((col >> 8) & 0xFF) / 255.0f, (col & 0xFF) / 255.0f };
    {
        float glow[3];
        const float low = (std::min)((std::max)(1.0f - sunDir[2] / 0.25f, 0.0f), 1.0f);
        if (low > 0.0f && SkyGlowColour(glow))
        {
            const float gm = (std::max)((std::max)(glow[0], glow[1]), (std::max)(glow[2], 1e-3f));
            for (int i = 0; i < 3; ++i)
                lc[i] += (glow[i] / gm - lc[i]) * low;
        }
    }
    // With the fog alone the light's dial is off the page, so the sun on the fog is what the light's
    // defaults give it (strength 25 x 3.0); Fog Sunlight sets it from there.
    const float gain = v.debug ? 1.0f : sunGain;   // the light's and the fog's parts weighted above (pc[38], pc[52])
    const float cc[4] = { v.debug ? gain : lc[0] * gain, v.debug ? gain : lc[1] * gain, v.debug ? gain : lc[2] * gain,
                          cover ? 1.0f : 0.0f };
    d->SetRenderTarget(dev, 0, world);
    d->SetTexture(dev, 2, reinterpret_cast<IDirect3DBaseTexture9*>(cover));
    d->SetSamplerState(dev, 2, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 2, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 2, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 2, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(dev, 2, D3DSAMP_SRGBTEXTURE, 0);
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(src->tex));
    d->SetRenderState(dev, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                   D3DCOLORWRITEENABLE_BLUE);
    // The world times the transmittance, plus the light (see kCompositeHlsl). With the fog off the
    // transmittance is 1, and this is the plain addition it was.
    const int fogDebug = fogOn && !v.debug ? fs.debug : 0;
    if (!v.debug && (!fogDebug || fogDebug == 3))   // 3, the cracks marked, over the picture as drawn
    {
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(dev, D3DRS_SRCBLEND,         D3DBLEND_ONE);
        d->SetRenderState(dev, D3DRS_DESTBLEND,        D3DBLEND_SRCALPHA);
        d->SetRenderState(dev, D3DRS_BLENDOP,          D3DBLENDOP_ADD);
    }
    // The sky's light on the fog: the game's own fog colour, which it sets for the zone and the time of
    // day, by [fog] brightness. The light's debug view shows the light alone.
    DWORD fogCol = 0x808080;
    const bool haveFogCol = WorldFogColor(fogCol);
    // Lifted toward the sky's glow where the game's fog colour is dark (2026-10-07, the owner: no mist on the sea in
    // Tirisfal Glades at 20:00). The game's fog there is 0x222226, near black, under a green-grey sky (346456 at 3.7
    // degrees, 2C7C56 at 16.8): the mist was there, as dense as anywhere, and as dark as the sea under it. Below a
    // luma of 0.25 the light moves toward the glow just over the horizon (1 to 6 degrees, read from the sky dome),
    // fully at 0.10, when the glow is the brighter. Above it, as in most zones by day, it is the game's fog colour.
    // The water's own fade to the fog colour is left as the game's, which the far land fades to as well.
    float fogRgb[3] = { ((fogCol >> 16) & 0xFF) / 255.0f, ((fogCol >> 8) & 0xFF) / 255.0f, (fogCol & 0xFF) / 255.0f };
    {
        const auto luma = [](const float* c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; };
        float glow[3];
        const float lf = luma(fogRgb);
        if (haveFogCol && SkyGlowColour(glow) && luma(glow) > lf)
        {
            const float t = (std::min)((std::max)((0.25f - lf) / 0.15f, 0.0f), 1.0f);
            for (int i = 0; i < 3; ++i)
                fogRgb[i] += (glow[i] - fogRgb[i]) * t;
        }
    }
    const DWORD fogLit = (static_cast<DWORD>(fogRgb[0] * 255.0f + 0.5f) << 16) |
                         (static_cast<DWORD>(fogRgb[1] * 255.0f + 0.5f) << 8) | static_cast<DWORD>(fogRgb[2] * 255.0f + 0.5f);
    const float amb = v.debug ? 0.0f : fs.brightness;
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
        float kc[48] = {};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                kc[r * 4 + c] = inv.m[r][c];
        kc[16] = pc[40]; kc[17] = pc[41]; kc[18] = 0.0f; kc[19] = 0.0f;
        kc[20] = static_cast<float>(src->w); kc[21] = static_cast<float>(src->h);
        kc[22] = 1.0f / src->w;              kc[23] = 1.0f / src->h;
        kc[24] = cc[0]; kc[25] = cc[1]; kc[26] = cc[2]; kc[27] = cc[3];
        kc[28] = fogRgb[0] * amb;
        kc[29] = fogRgb[1] * amb;
        kc[30] = fogRgb[2] * amb;
        kc[31] = static_cast<float>(fogDebug);
        // c8, c9: the sun (or the larger moon) and the other moon, for their discs through the fog.
        float disc[3];
        if (SunDirection(disc))
        {
            kc[32] = disc[0]; kc[33] = disc[1]; kc[34] = disc[2]; kc[35] = 1.0f;
        }
        if (SunSecondDirection(disc))
        {
            kc[36] = disc[0]; kc[37] = disc[1]; kc[38] = disc[2]; kc[39] = 1.0f;
        }
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
        // c10: the sky's colour just over the horizon, for the far fog (2026-10-08); the fog's own colour, unscaled by
        // Fog Brightness, when the sky has not been read.
        {
            float glow[3];
            const bool haveGlow = SkyGlowColour(glow);
            for (int i = 0; i < 3; ++i)
                kc[40 + i] = haveGlow ? (std::max)(glow[i], fogRgb[i]) : fogRgb[i];
            kc[43] = v.debug ? 0.0f : 1.0f;
        }
        // c11: the water's height from the camera, for the cracks in it (2026-10-08), and whether the screen's copy is
        // bound: a crack takes the colour beside it from there.
        kc[44] = g_fogSeaRel; kc[45] = g_fogSeaRel > -9999.0f ? 1.0f : 0.0f;
        const bool scene = (!fogDebug || fogDebug == 3) && kc[45] > 0.5f && EnsureScene(dev, wd) &&
                           SUCCEEDED(d->StretchRect(dev, world, nullptr, g_scene.surf, nullptr, D3DTEXF_NONE));
        kc[46] = scene ? 1.0f : 0.0f;
        d->SetTexture(dev, 3, reinterpret_cast<IDirect3DBaseTexture9*>(scene ? g_scene.tex : nullptr));
        d->SetSamplerState(dev, 3, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, 3, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
        d->SetSamplerState(dev, 3, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, 3, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(dev, 3, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(dev, 3, D3DSAMP_SRGBTEXTURE, 0);
        d->SetPixelShaderConstantF(dev, 0, kc, 12);
        ClipQuad(dev, wd.Width, wd.Height);
        d->SetTexture(dev, 3, nullptr);
        d->SetTexture(dev, 1, nullptr);
        d->SetVertexShader(dev, nullptr);
    }

    d->SetTexture(dev, 2, nullptr);

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
    {
        Log("volume: drawn at %ux%u (%.2f ms CPU to issue)%s, gain %.2f, density %.3f, max distance %.0f yards, "
            "sun (%.2f %.2f %.2f)", g_a.w, g_a.h, 1000.0 * (Now() - t0), fogOnly ? ", the fog alone (no shadow map)" : "",
            gain, fogOnly ? 0.0f : v.density, v.maxDistance, sunDir[0], sunDir[1], sunDir[2]);
        if (fogOn)
        {
            Log("fog: %.4f a yard at the ground, height %.0f yd, the ground at %.1f (%.1f yd under the camera, from "
                "%s), reach %.0f yd, on the sky %.0f yd, sun %.2f, sky light 0x%06lX (the game's 0x%06lX)%s x %.2f, debug %d",
                fs.density, fs.height, g_fogBase, -groundRel, g_fogBaseFrom, fs.reach, fs.skyDistance, fs.sunLight, fogLit,
                fogCol & 0xFFFFFF,
                haveFogCol ? "" : " (no game fog colour yet)", fs.brightness, fogDebug);
            Log("fog: the glows: the sea %.2f yards from the camera (-10000: none found), its horizon %.4f, sun height %.3f",
                g_fogSeaRel, g_fogHorizon, sunDir[2]);
            Log("fog: patches %s: patchiness %.2f, %.0f yd across, %.2f as tall, wind %.1f yd/s toward %.0f deg; "
                "the camera at (%.3f %.3f %.3f) in the noise", patches ? "on" : g_noiseFailed ? "off (no texture)" : "off",
                fs.patchiness, fs.scale, 1.0f / fs.flatten, fs.windSpeed, fs.windDeg, pn[0], pn[1], pn[2]);
            Log("fog: ground %s around (%.0f %.0f): %d cells with no tile, %d wet, %d with a floor under the terrain; "
                "follow %.2f, low ground x%.2f (full at %.0f yd below the %.0f yd average), water x%.2f; dawn and "
                "dusk x%.2f now", g_groundValid ? "texture" : g_groundFailed ? "off (no texture)" : "off (no tiles)",
                g_groundAt[0], g_groundAt[1], g_groundMissing, g_groundWet, g_groundFloor, fs.follow,
                1.0f + fs.lowGround, fs.lowDepth, fs.smoothRadius, 1.0f + fs.water, morning);
        }
        else
            Log("fog: off ([fog] enabled %d, density %.4f)", fs.enabled ? 1 : 0, fs.density);
    }
    return true;
}

float FogDensityNow()
{
    return FogOn() && VolumeActive() ? g_cfg.fog.density * g_fogMorning : 0.0f;
}

float FogThicknessAt(const float rel[3])
{
    const FogSettings& fs = g_cfg.fog;
    if (!FogOn() || !g_fogCamOk)
        return 0.0f;
    const float p[3] = { rel[0] + g_fogCam[0], rel[1] + g_fogCam[1], rel[2] + g_fogCam[2] };
    float base = g_fogBase, mult = 1.0f;
    if (g_groundValid && g_gSurf.size() == static_cast<size_t>(kGround * kGround))
    {
        const int i = static_cast<int>(floorf((p[0] - g_groundAt[0]) / kGroundCell + 0.5f * kGround));
        const int j = static_cast<int>(floorf((p[1] - g_groundAt[1]) / kGroundCell + 0.5f * kGround));
        const int k = (std::min)((std::max)(j, 0), kGround - 1) * kGround + (std::min)((std::max)(i, 0), kGround - 1);
        base = g_gSmooth[k] + (g_gSurf[k] - g_gSmooth[k]) * fs.follow;
        const float low = (g_gSmooth[k] - g_gSurf[k]) / fs.lowDepth;
        mult = (std::max)(1.0f + fs.lowGround * (low < 0.0f ? 0.0f : (low > 1.0f ? 1.0f : low)), 1.0f + fs.water * g_gWet[k]);   // as the shader
        // Under the terrain, as the shader does.
        float u = (g_gSurf[k] - p[2] - 4.0f) / 12.0f;
        u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
        if (u > 0.0f)
        {
            const bool known = g_gFloor.size() == g_gSurf.size() && g_gSurf[k] - g_gFloor[k] >= 1.0f;
            const float floorAt = known ? (std::min)(g_gFloor[k], p[2]) : p[2];
            base += (floorAt - base) * u;
            mult += (1.0f - mult) * u;
        }
        // None under the water, as the shader does.
        float under = (g_gSurf[k] - p[2]) * 2.0f;
        under = under < 0.0f ? 0.0f : (under > 1.0f ? 1.0f : under);
        mult *= 1.0f - g_gWet[k] * under;
    }
    const float up = (base - p[2]) / fs.height;
    return mult * expf(up < 4.0f ? up : 4.0f);
}

void VolumeStatsText(std::string& out)
{
    const FogSettings& fs = g_cfg.fog;
    char line[256];
    snprintf(line, sizeof(line), "fog=%d;fogd=%.4f;morning=%.2f;ground=%s;notile=%d;wet=%d;patches=%d;", FogOn() ? 1 : 0,
             fs.density, g_fogMorning, g_groundValid ? "map files" : "average", g_groundMissing, g_groundWet,
             g_noise && fs.patchiness > 0.0f ? 1 : 0);
    out += line;
}

bool VolumeLightActive()
{
    // The box alone (2026-10-02): the shadow map is the sun shadows' and the lamps' too. Until then it also
    // needed Strength above 0 or the fog on, and at Strength 0 with the fog off the sun shadows went.
    const VolumeSettings& v = g_cfg.volume;
    return v.enabled && g_on && !g_failed;
}

bool VolumeActive()
{
    return VolumeLightActive() || (!g_cfg.volume.enabled && g_on && !g_failed && FogOn());
}

void VolumeReset()
{
    if (g_ground)
    {
        g_ground->lpVtbl->Release(g_ground);
        g_ground = nullptr;
    }
    g_groundValid = false;
    if (g_noise)
    {
        g_noise->lpVtbl->Release(g_noise);
        g_noise = nullptr;
    }
    ReleaseDefaultPool();
    g_failed = false;
}

bool VolumeToggle()
{
    g_on = !g_on;
    g_histValid = false;
    Log("--- volume %s ---", g_on ? "ON" : "OFF");
    return g_on;
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
            "no shadow matrix %u, no sun %u, no camera %u, sun down or night %u, no target %u, bad matrix %u",
            g_st.frames, g_st.calls, g_st.drawn, g_st.noDepth, g_st.noShadow, g_st.noMatrix, g_st.noSun,
            g_st.noCam, g_st.sunDown, g_st.noTarget, g_st.badMatrix);
    g_st = {};
}

// The shaders this pass compiles, as it compiles them, for the cache's worker (shadercache.cpp, 2026-10-06).
void VolumeShaderList()
{
    ShaderPrecompile("volume_march_vs", kMarchVsHlsl, "vs_3_0");
    ShaderPrecompile("volume_march", kMarchPsHlsl, "ps_3_0");
    ShaderPrecompile("volume_blur", kBlurHlsl, "ps_2_0");
    ShaderPrecompile("volume_temporal", kTemporalHlsl, "ps_3_0");
    ShaderPrecompile("volume_composite", kCompositeHlsl, "ps_3_0");
    ShaderPrecompile("volume_plain_composite", kPlainCompositeHlsl, "ps_2_0");
}
