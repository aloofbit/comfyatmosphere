// comfyfog.ini: one dial (thickness, 0..100) plus the few numbers that say what 100 looks like.
#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

struct FogSettings
{
    // Off by default since 2026-09-30: with it off the game's own fog is used, untouched. The owner found
    // the game looked better that way than with ours, which had grown feature by feature (the dial, height
    // fog, fog at the camera, greyer and darker colour, sun glow) and together greyed the open land.
    bool  enabled   = false;

    // The dial. 0 is the client's own fog, untouched; 100 is the heaviest the settings below allow.
    // Everything else in this struct describes the far end of the dial and is scaled by it.
    float thickness = 65.0f;

    // Fog already present right at the camera, at 100 (0..0.9). The client's fog is linear from 0, so
    // near things are nearly clear and it only builds with distance; a haze floor fills in the near
    // field. Done with a negative FOGSTART, which every fog path (grass shader included) handles.
    float haze      = 0.55f;

    // Where fog becomes total at 100, as a fraction of the client's own fog end. Kept well out, so the
    // climb after the haze floor is gentle rather than a wall. Interpolated geometrically in the dial.
    float reach     = 1.40f;

    // Our own fog (mode 1, 2026-09-29): drawn over the picture from the depth, while the volumetric light
    // runs (it needs the depth), with the game's fog moved out of the way. The game's fog is linear from
    // start to end, the same in the terrain and in the tree shaders, so a fog made stronger with it
    // still ended in a wall, and a far tree fogged in full stood out white against the sky behind it.
    // This one is height fog: thick low down and thinning upward, so a line of sight toward the sky
    // meets a known amount of it, and the sky near the horizon is the fog colour. Mode 0, or with the
    // volumetric light off, is the game's fog moved by haze and reach as before.
    int   mode      = 1;
    float density   = 0.013f;   // the height fog: fog per yard at the camera's height (the Ground Haze
                                // control, 0..0.02). Not scaled by the dial: the dial is the distance
                                // fog. 0.02 scaled by the dial was tuned before the distance fog existed,
                                // and with it on top the view distance shrank (2026-09-29)
    float height    = 115.0f;   // yards: the fog thins by e (2.7 times) every this many yards up
    // The height fog anchored to the ground (2026-09-30): density is the fog at the average ground height
    // within groundRadius yards (from the map files), not at the camera. At the camera it was: standing on
    // the cliff over Stormwind harbour, the sea 50 yards below had e^(50/15) = 28 times the fog, and the
    // harbour was one flat haze. Needs [shadow] mapTerrain; without a tile, the camera as before.
    bool  ground    = true;
    float groundRadius = 150.0f;
    float distance  = 0.30f;     // 0..1: how much of the game's distance fog (its start and end, as the dial
                                // moves them) is kept, on a soft curve. Far mountains need it
    float cover     = 0.55f;    // 0..1: past this share of the view distance, things fade in full into
                                // the fog by the view distance, so nothing stops at a hard edge
    float skyDepth  = 0.99999f; // depth at or past which a pixel is the sky itself. Between the world's
                                // slice and this: scenery drawn with the sky (far mountains), fogged in full
    int   debug     = 0;        // 1 = our fog's amount alone (white = all fog); 2 = that far scenery, red

    // The game's fog colour. Not part of the dial: applied as set, with the fog on or off (Shift+F11
    // turns it off with the rest). Until 2026-09-29 it was scaled by the dial, so these are the old
    // values at the default thickness of 60.
    float desaturate = 0.30f;         // 0 .. 1, toward grey. Applied to the game's fog too: 0 leaves it as it is
    float darken     = 0.15f;         // 0 .. 1, toward black
    DWORD tint       = 0x5A6470;     // RGB the colour is pulled toward, by tintAmount
    float tintAmount = 0.0f;         // 0 .. 1
    // Our fog lit by the sun (2026-09-30): warm looking toward it, cool looking away, as haze is. The
    // colours are scaled to a brightness of 1, so they tint and do not darken. Fades at night.
    float sunGlow    = 0.95f;        // 0..1, how much of the tint
    DWORD glowColor  = 0xFFD6A0;     // toward the sun
    DWORD awayColor  = 0xA0BCFF;     // away from it
    float sunBright  = 0.40f;        // the fog brighter by up to this share looking into the sun
    // The sky near the horizon gets the same change as the colour, so that seam does not show: it is
    // what shows between trees past the view distance, and a darker fog alone left it bright
    // (2026-09-29). Needs the depth buffer (Volumetric Light on).
    bool  waterDepth = true;         // the water writes depth, so the fog, the shadows and the light see it
    float skyMatch   = 1.0f;         // 0 .. 1, how far the sky at the horizon fades into the fog colour
    float skyBand    = 0.35f;        // up to this height (sine of the angle above the horizon) it fades out
    bool  skyDebug   = false;        // 1 = the sky it changes shows red

    // The vertex-shader constant the client's M2 shaders fog from: (-1/(end-start), end/(end-start)).
    // Found by disassembly for this WoW.exe (see comfyfog.cpp); -1 leaves M2 fog stock.
    int   shaderReg  = 30;
};

// Where the sun is, for the shadow map and the volumetric light (sun.cpp). By default it is the sun the
// client draws in the sky, so both follow the time of day.
struct SunSettings
{
    bool  fixed     = false;        // true: a fixed world direction, azimuth/elevation in degrees (Z up)
    float azimuth   = 45.0f;        // the afternoon sun logged while tuning
    float elevation = 50.0f;
};

// Where the client keeps the camera and the player (verified for this WoW.exe by comfygrass). Shadows
// and volumetric light read them through client.cpp.
struct ClientSettings
{
    DWORD camAddr      = 0x00C7CF20;
    DWORD objMgrAddr   = 0x00B41414;
    DWORD playerPosOff = 0x9B8;
    DWORD clockAddr    = 0x00CE9B64;  // float, the time of day as a fraction of the day (found by comfytime)
    DWORD mapNameAddr  = 0x00C961A0;  // the current map's folder name, for mapterrain.cpp
};

// Sun rays and volumetric light at night (sun.cpp, NightScale). The client draws its night sky light with
// the same sprite as the sun, high in the sky (logged: 75 to 83 degrees up at 01:00), so the sun's
// height cannot tell night from day. The game clock can.
struct NightSettings
{
    float strength = 25.0f;     // the dial, 0..100: the rays and the light at night, as % of their day strength
    float dusk     = 20.0f;     // hour the change to night starts
    float dawn     = 5.0f;      // hour the change to day starts
    float fade     = 1.5f;      // hours each change takes
    // Night Darkness (2026-09-30): the world darker at night, by the same clock. Drawn in the lamps' pass
    // (lampglow.cpp), so the ground near a lamp keeps the lamp's light.
    float darkness = 0.20f;     // 0..0.9: how much darker the world is at full night
    float tint     = 0.65f;     // 0..1: how far the dark leans toward moonColor
    DWORD moonColor = 0x9CB8FF; // RGB of the moonlight (its hue only: brightness is kept)
    float sky      = 0.5f;      // 0..1: the share of the darkness the sky gets
    bool  indoors  = false;     // true = darker inside buildings too
};

// A readable depth buffer (depth.cpp), which the volumetric light reads. Off unless enabled.
struct DepthSettings
{
    bool  enabled   = true;
    // A see-through model writes no depth (2026-09-30). A stealthed lion is drawn blended but writing depth,
    // so the sun shadows and the volumetric light, which read depth, shaded and lit its outline and gave it
    // away. comfyfog.cpp, IsSeeThroughModel.
    bool  seeThrough = true;
};

// A shadow map from the sun (shadow.cpp): the frame's opaque world draws replayed from the sun.
struct ShadowSettings
{
    bool  enabled = true;
    int   size    = 4096;     // texels per side. 4096 took twice the GPU time and gave the volumetric
                              // light the same look (benchmark, 2026-09-24: 2.35 against 1.66
                              // microseconds a caster); it gives the sun shadows sharper edges. The
                              // Shadow Resolution control sets it: 1024, 2048 or 4096
    float range   = 250.0f;    // yards covered either side of the player
    float nearRange = 64.0f;   // the near map, for the sun shadows: yards either side (0 = none). The far
                               // map's texel, a quarter of a yard, was too coarse for a trunk or a post
    float depth   = 700.0f;   // yards toward and away from the sun: far enough for a ridge to shade you
    // The ground from the map files reaches further toward the sun than the rest (2026-09-30): in Lakeshire the
    // ridge the sun set behind was 790 yards off along the sun, past `depth`, and only its lower slopes were in
    // the map. With mapTerrain the map's box runs this far; buildings and models stay within `depth`.
    float horizonDepth = 1500.0f;
    int   copyPerFrame  = 16;     // arena chunks copied into our own buffers per frame. The client
                                  // streams terrain through a buffer it re-fills, so a cached pointer
                                  // into it is worthless; a copy of our own is not. Reading the client's
                                  // memory back is slow, hence a limit per frame (a chunk is 3.5 KB).
    int   copyMax       = 2048;   // how many such copies to hold at once. 2 and 768 until 2026-09-24;
                                  // since then every streamed chunk needs a copy before it casts, and at
                                  // 2 a frame new ground stayed without shade while you walked
    float nearMargin    = 16.0f;  // yards past the near map a model's reference point may be and still be
                                  // drawn into it (the far map: 40)
    bool  terrainLeaves = true;   // terrain (hills, mountains) casts part shade, as leaves do
    bool  mapTerrain    = true;   // the ground from the map files, at full detail (mapterrain.cpp)
    int   leafAlpha     = 224;    // the alpha test that cuts the leaves of the doodads from the files, 1..255.
                                  // 224 is the client's own for every Elwynn tree; it goes lower only on
                                  // doodads fading in or out at the edge of the view (probe, 2026-09-30)
    bool  leaves        = true;   // the leaves (alpha-tested draws) in maps of their own, so they can let
                                  // part of the sun through ([sunshadows] leafShade; see shadow.cpp)
    int   minTriangles  = 200;    // models with fewer triangles stay out of the far map past 60 yards.
                                  // At 100, 1,400 of 4,457 far-map draws went, and neither 100 nor
                                  // 200 could be told apart in game (2026-09-29)
    int   farEvery      = 2;      // of those rebuilds, the far map is redrawn on every Nth; the near map
                                  // on each. The far map's shade barely changes, and it was half the cost
    int   mapEvery      = 1;      // rebuild the map every N frames, 1..8 (the Shadow Redraw control). The map is anchored in the world and
                                  // the light is smoothed over time, so 3 takes two thirds off the cost
                                  // of the replay for very little: the shade it holds is two frames old.
    bool  horizon       = true;   // keep the far-horizon draws: distant terrain can shade you too. They
                                  // are drawn with a camera of their own, which only matters for the
                                  // shader models, so those are still left out.
    bool  snap          = false;  // hold the map on whole texels of its own grid: steadier standing
                                  // still, but it steps as you walk, which reads worse
    float keepMargin    = 100.0f; // yards past range a caster out of view is kept, across the ground from
                                  // the player: while it can still cast into the map (see shadow.cpp)
    float staleTime     = 8.0f;   // with mapTerrain: seconds a cache entry not drawn is kept at most; 0 = no
                                  // limit. The world comes from the files then, and what the cache holds
                                  // (characters, creatures, the server's objects) has no need to outlast its
                                  // draws: a leaf-edged shade stayed on open ground at Gavin's Naze (2026-09-30)
    float cacheTime     = 0.0f;   // seconds a caster out of view is kept at most; 0 = no limit. Was 8, and
                                  // shadows of trees beside you jumped out 8 seconds after you looked away
    float evictDistance = 20.0f;  // yards: a caster in view this near that was not drawn is gone
    float stillRadius   = 0.3f;   // yards: a model seen again this near where it was stored keeps the
                                  // placement it has. Its position is worked out through the camera, which
                                  // moves by one frame's walk during a frame; at 0.02 every tree was placed
                                  // again at each redraw while walking, a fraction of a texel off, and its
                                  // leaves re-sampled into a new pattern
};

// Volumetric light (volume.cpp): the fog glowing where the sun reaches it. Needs [depth] and [shadow].
struct VolumeSettings
{
    bool  enabled      = true;
    float strength     = 45.0f;     // the dial, 0..100
    float maxIntensity = 3.0f;      // gain at 100
    float density      = 0.012f;     // how much the air scatters, per yard (Light Density, in thousandths)
    float maxDistance  = 140.0f;     // yards along each line of sight (the shadow map's reach)
    int   steps        = 64;        // samples along each line of sight: more holds up over a long
                                    // maxDistance, where a thin canopy can fall between two samples.
                                    // 96 until 2026-09-24; with the noise turned each frame and the
                                    // last frame kept, 64 looked the same in game
    float smooth       = 0.85f;     // 0..0.95: how much of the last frame's glow is kept. The march is
                                    // noisy and the map changes under it, and the light jittered as you
                                    // walked. The last frame is moved with the camera before it is
                                    // blended in, so a turn does not smear. 0 also stops the noise
                                    // pattern from changing each frame.
    float anisotropy   = 0.070f;    // 0 = glows the same from every side, toward 1 = only toward the sun (Light Toward the Sun, thousandths)
    float bias         = 0.5f;      // yards: shadow-test slack, against speckle on lit surfaces
    float leafShade    = 1.0f;      // how much of the sun leaves stop in the air (the ground: [sunshadows]
                                    // leafShade). 1: shafts come through the gaps between the leaves only
    DWORD color        = 0xFFE6BE;  // RGB of the light (default: warm late-morning)
    int   downscale    = 2;         // work at 1/N resolution per axis
    bool  blur         = true;
    int   debug        = 0;         // 1 = the glow alone, white; 2..5 = one stage of the march (see ini)

    // The Volumetric Light Quality control: 3 (high) uses the values in this file as they are; 2 (medium)
    // and 1 (low) replace two of them with cheaper fixed values (ApplyVolumeQuality). Added 2026-09-24
    // because players reported low frame rates with the light on. It set the shadow map's size and how
    // often it is redrawn too, until those got their own controls (2026-09-29).
    int   quality      = 3;

    // Fade the light when the sun's disc is behind terrain on screen (cover.cpp). The shadow map holds only
    // what lies within [shadow] depth, so a sun setting behind the far horizon kept lighting the fog. Uses
    // [rays] occlusionRadius and occlusionFull. Added 2026-09-28.
    bool  occlusion    = true;
};

// Sun shadows on the world (sunshadows.cpp), from the volumetric light's shadow map, so they draw only while
// [volume] draws. Added 2026-09-29.
struct SunShadowSettings
{
    bool  enabled    = true;
    // What casts (2026-09-30): the world (terrain, buildings, trees, doodads) and the units (players,
    // creatures). Off, the world is left out of the near maps and the sun shadows stop reading the far map,
    // which the volumetric light keeps, and the terrain's own baked shadow comes back; units off leaves
    // them out of every map, and the addon gives back the game's round shadow.
    bool  world      = true;
    bool  units      = true;
    // The shadows at a set tilt from straight down (2026-09-30), whatever the time of day, as the client's
    // own baked shadows are. The azimuth stays the sun's (45 degrees in this client, all day). The rays and
    // the volumetric light's glow keep the real sun.
    bool  lock       = false;
    float lockTilt   = 15.0f;     // degrees from straight down
    // Slack in the far map's test that grows with distance (2026-09-30). The terrain casts from the files at
    // full detail, and the client draws the ground past about 100 yards coarser, cutting across the dips: a
    // coarse surface under the true one was in the fine terrain's shade, and soft blobs lay on distant slopes
    // until you came closer and the client drew the fine mesh.
    float lodBias    = 2.0f;      // yards of slack for every 100 yards past lodStart
    float lodStart   = 80.0f;     // yards from the camera where it starts
    // Coloured light (2026-09-30): shade takes the sky's cool colour, sunlight a warm one, where both only
    // darkened and brightened in grey. Scaled to a brightness of 1: they tint, not darken.
    // Indoors (2026-09-30): the buildings come whole from the files, roof and all, so a room was in full
    // shade on top of the game's own dim indoor light, and the inn was very dark. With the player in one of a
    // building's indoor groups, the sun shadows are kept at this share (0: none), faded over half a second.
    float indoor     = 0.0f;
    DWORD shadeColor = 0x7C94C8;
    float shadeTint  = 0.65f;      // 0..1
    DWORD sunColor   = 0xFFE4C0;
    float sunTint    = 0.95f;      // 0..1
    float strength   = 20.0f;     // the dial, 0..100: how much of the light a shaded surface loses
    float bias       = 3.0f;      // texels of slack in the depth test at the least; more as the sun gets
                                  // lower (see sunshadows.cpp). Against a surface shading itself
                                  // in bands (of each map: at 2048, a quarter of a yard is 1 texel of the
                                  // far map, 8 of the near one)
    float minGap     = 0.0f;      // yards: a blocker nearer than this along the sun does not shade. Stands
                                  // in for no shadow on itself (an arm on the body, leaves on leaves); on
                                  // both maps alike, where bias is in texels of each
    float sunOffset  = 0.06f;     // yards each point is moved toward the sun before the test, against
                                  // the same. Values tuned in game with /atmos (2026-09-29)
    float normalBias = 0.0f;      // texels each point is moved along its rebuilt facing, up to 4 times
                                  // that where the sun grazes the surface. The facing is per triangle on
                                  // a model, so any of this put the triangles on the character
    float slope      = 0.0f;      // 0..1: how much of the surface's slope (from the same facing) sets each
                                  // tap's depth. Against stripes on sloped ground at a high softness; on
                                  // a model it made patches where the arm shades the body
    float softness   = 1.0f;      // how far apart the nine taps of the soft edge are, in map texels.
                                  // Each tap blends four texels, so 0 is sharp but not stepped
    float baked      = 0.0f;      // 0..1: how much of the terrain's own baked shadow is kept while these
                                  // draw (terrainshade.cpp). It points one way at every hour. Back in
                                  // full as they fade at dusk
    float leafShade  = 0.6f;      // 0..1: the share of the sun that leaves stop. Under a forest canopy
                                  // everything solid shades the rest, so characters no longer float
    float sunlight   = 0.35f;      // 0..0.5: what the sun reaches is brightened by up to this share (the
                                  // Sunlight control, in percent). A forest in shade was dark all over
    int   debug      = 0;         // 1 = the shade alone (white = lit); 2 = the leaves' shade alone
};

// Lamps, lanterns and torches (lamps.cpp finds them, lampglow.cpp draws): the fog glowing around them, and
// lampposts lighting the surfaces near them as the client's torches do.
// It reads the volumetric light's depth and world camera, so it draws only while [volume] does, but at
// night too, when the sun's light is turned down. Added 2026-09-28.
struct LampSettings
{
    bool  enabled      = true;
    float strength     = 20.0f;     // the dial, 0..100
    float maxIntensity = 10.0f;     // the glow in the air: gain at 100. 4 until 2026-09-28, too faint to see
    float surface      = 7.0f;      // a lamppost's light on the surfaces near it: gain at 100 (0 = none).
                                    // About 1 / the night's own light, so a lamp lights like a torch
    float density      = 0.03f;     // how much the air scatters a lamp's light, per yard
    float day          = 30.0f;     // % of the night strength by day, by [night] dusk, dawn and fade
    float maxDistance  = 120.0f;    // yards: a light further away than this adds nothing
    int   maxLights    = 16;        // the nearest this many are drawn, 1..16
    float keep         = 2.0f;      // seconds a light on screen may go unseen before it fades out. A light
                                    // off screen is kept: the client draws a lamp's sprite only while the
                                    // lamp is on screen
    float softness     = 0.4f;      // yards: the radius of a light's bright core
    float through      = 1.5f;      // yards the glow runs on past the first surface in the line of sight.
                                    // The client's light sits inside the torch head or the brazier bowl,
                                    // and at 0 the bowl hid it from below or from the side
    bool  sprites      = true;      // also glow around lampposts, found by their glow sprite
    float fogReach     = 2.4f;      // how far into the fog a lamp still glows: the fade (over the far half of
                                    // the fog) stretched by this. Lamp Distance, in percent
    bool  files        = true;      // also the candles, torches and fires the buildings' files place
                                    // (mapwmo.cpp), which the client lights with no light of its own
    float spriteReach  = 16.0f;     // yards a lamppost's light reaches; a torch's is 16.7 (the client's
                                    // lights carry their own). 10 until 2026-09-28
    float spriteGain   = 1.5f;      // a lamppost's brightness: its sprite's colour (0.95 0.60 0.22) x this.
                                    // 1.5 is about a torch's (1.40 0.87 0.40)
    int   debug        = 0;         // 1 = the glow alone, over black; 2 = the distance it reads (white = 50 yd);
                                    // 3 = the light on surfaces alone
};

// Sun rays (rays.cpp): a radial blur of the bright sky toward the sun, drawn after the world and before
// the UI. Cheap, and needs neither [depth] nor [shadow].
struct RaysSettings
{
    bool  enabled     = true;

    // The dial. 0 = no rays, 100 = maxExposure, on a square curve: exposure = maxExposure x (strength/100)^2.
    // 5.7 keeps the default of 35 at 0.7, the exposure it had when the dial was linear with a maximum of 2.
    // The rest describes the look and is not scaled by it.
    float strength    = 40.0f;
    float maxExposure = 5.7f;

    // A pixel casts rays when it is within relThreshold of the brightest pixel in the frame (so the
    // sky gaps in a dim, foggy forest cast as surely as the sky beside the sun), and above threshold,
    // an absolute floor that keeps a dark cave's dim lights from streaking.
    float relThreshold = 0.20f;     // 0..1 of the frame's brightest luminance. 0.20 since 2026-09-24,
                                    // tuned in game with skyOnly: at 0.75 only the sun and its halo cast,
                                    // so a ridge in front of the sun gave one smooth fan and no shafts,
                                    // and the kept sky (no glow) only just cast at 0.35
    float threshold   = 0.20f;      // absolute luminance floor, 0..1
    float falloff     = 2.0f;       // exponent on the distance weight: higher keeps casting close to the sun
    float radius      = 0.8f;       // how far from the sun (screen heights) pixels still cast rays;
                                    // weight (1 - d/radius)^falloff
    float length      = 0.85f;      // ray length, as a fraction of the way from each pixel to the sun
    float maxLength   = 0.6f;       // but never more than this many screen heights (a far sun)
    float maxAngle    = 60.0f;      // degrees between view and sun at which rays are gone. 140 streamed rays in
                                    // from the top of the screen with the sun out of view, fed by whatever sky
                                    // was there: the view brightened and darkened as the camera tilted (2026-09-30)
    float viewFalloff = 1.0f;       // shape of the fade from looking at the sun to maxAngle; higher = faster
    float parallel    = 0.0f;       // 0 = rays fan out from the sun, 1 = parallel shafts falling away from it.
                                    // 0 is the geometrically right one: parallel shafts in the world run to the
                                    // sun on screen, and a simulation showed 1 swings MORE as the camera turns
    float adaptTime   = 0.5f;       // seconds the brightness reference takes to follow the scene
    float decay       = 0.96f;      // per-sample falloff along a ray; lower = shorter, softer shafts
    float soften      = 1.0f;       // pixels of the mask (downscale x this on screen): the mask is blurred
                                    // this wide before the rays are drawn, so a gap between leaves a pixel
                                    // wide no longer makes a whole ray blink (2026-09-29). 0 = none
    float smooth      = 0.0f;       // 0..0.95: how much of the last frame's mask is kept, moved with the sun.
                                    // Leaf edges smaller than a pixel flipped between leaf and sky as the
                                    // camera moved, and the rays jittered (2026-09-29). 0 = none
    DWORD color       = 0xFFE6BE;   // RGB tint of the light
    int   passes      = 3;          // blur passes of 16 samples each, 1..3
    int   downscale   = 2;          // work at 1/N resolution per axis, 1..8
    int   debugView   = 0;          // 1 = show the mask, 2 = show the rays alone

    // 0: at the world -> UI boundary, so the UI gets no rays (and loading screens none at all).
    // 1: over the finished frame at Present, UI included: the fallback if the boundary is ever missed.
    int   placement   = 0;

    // 1: only the sky casts rays, not the clouds (the image is kept just before the cloud layer is drawn).
    // That image is also free of the client's glow, so its sky is dimmer than the finished frame's, and
    // relThreshold has to be lower for the sky to cast. debugView 3 shows it. On by default since
    // 2026-09-24: with it off, lit clouds near the sun cast and glowed.
    bool  skyOnly     = true;
    int   mask        = 1;        // 1 = a pixel casts where it shows sky (the depth buffer); 0 = by brightness,
                                  // relative to the brightest pixel in view (changed as the camera tilted)

    // At night the sky has two moons. The volumetric light follows one (sun.cpp, PickQuad); with this
    // on, the other casts rays as well. Each moon costs one mask, blur and composite.
    bool  secondMoon  = true;

    // Rays fade when the sun itself is covered (rays.cpp, kVisDepthHlsl and kVisHlsl): nine points on and
    // around the sun, tested by depth when [depth] gives a readable one, else by comparing the finished
    // frame with the sky kept before the world (needs skyOnly; heavy fog defeats it). Added 2026-09-28:
    // with the sun behind a mountain, the sky above the ridge cast rays straight down over it.
    bool  occlusion       = true;
    float occlusionRadius = 0.08f;   // screen heights around the sun that are tested
    float occlusionFull   = 0.35f;   // share of that sky in view that gives full rays; less fades them.
                                     // Under 1, so a canopy with gaps still casts in full
};

struct SkySettings
{
    bool clouds = false;   // false: the sky's cloud layer is not drawn
};

// The benchmark (bench.cpp): Alt + the probe key runs each feature in turn and logs what it costs.
struct BenchSettings
{
    float settle  = 5.0f;    // seconds each step runs before it is measured: shaders compile, the shadow
                             // cache fills (a few seconds, at [shadow] copyPerFrame a frame)
    float measure = 5.0f;    // seconds each step is measured
};

struct Settings
{
    SkySettings  sky;
    RaysSettings rays;
    BenchSettings bench;
    DepthSettings depth;
    ShadowSettings shadow;
    VolumeSettings volume;
    LampSettings lamps;
    SunShadowSettings sunShadows;
    FogSettings  fog;
    SunSettings  sun;
    NightSettings night;
    ClientSettings client;

    bool  trace       = false;      // F12 then also traces the next 180 frames of the volumetric light:
                                    // what it marched, what the shadow map held, what the cache did. It
                                    // reads buffers back from the GPU, so it is off unless asked for.
    bool  logEnabled  = true;
    bool  hook        = true;       // 0: load, log, patch nothing (bisecting)
    bool  sliders     = true;       // register the CVars the in-game controls set (cvars.cpp)
    bool  master      = true;       // [general] enabled: every effect at once. Off, the game draws as
                                    // stock: fog, its colour, light, shadows, rays and lamps all off
    int   reloadKey   = VK_F11;     // reload comfyfog.ini; with Shift, toggle the override
    int   probeKey    = VK_F12;     // log one frame of fog state changes and draw counts; with Alt, benchmark
    int   chainWaitMs = 10000;      // how long to wait for comfygrass to finish patching first
    int   minWorldDraws = 16;       // world draws needed before a switch to 2D counts as the end of the
                                    // world (depth, shadows and volumetric light run there)
};

extern Settings g_cfg;

void LoadSettings(const wchar_t* iniPath);

// Every ini key LoadSettings read, in the order it read them, with the value it used. /atmos (tune.cpp)
// finds settings here, so a key needs no list of its own to be tunable.
enum ConfigSource { kFromDefault, kFromIni, kFromTune };
struct ConfigKey
{
    std::string  section, key;   // as the code spells them
    std::string  value;          // the value read, before any clamp
    ConfigSource source;
};
const std::vector<ConfigKey>& ConfigKeys();
const wchar_t* ConfigIniPath();                        // the ini LoadSettings last read

// Values set with /atmos, "section.key" -> value. They win over the ini on every LoadSettings, F11 too,
// until cleared or saved into the ini.
std::map<std::string, std::string>& ConfigOverrides();

// [volume] quality below 3: the march's steps and resolution are replaced by cheaper fixed values. After the in-game controls are laid over the ini.
void ApplyVolumeQuality(Settings& s);
void ResolveIniPath(HMODULE self, wchar_t* out, size_t count);
