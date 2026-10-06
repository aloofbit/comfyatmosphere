// comfyatmos.ini: the settings of every effect, one section each.
#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

// The fog (volume.cpp, 2026-09-30): carried by the volumetric light's march, so it draws only while the
// light does. Thick at the ground and thinning upward; it darkens what lies behind it, and the sun (through
// the shadow map) and the sky light it.
struct FogSettings
{
    bool  enabled     = true;
    float density     = 0.0025f;   // fog per yard at the ground: 0.0025 lets through 60% of what is 200 yards away
    float height      = 25.0f;     // yards: the fog thins by e (2.7 times) every this many yards up
    float groundRadius = 150.0f;   // yards around you the ground under the fog is averaged over (the map files)
    float reach       = 200.0f;    // yards: how far a line of sight gathers fog; it fades out over the last 40%
    float skyDistance = 75.0f;     // yards: the same for a line of sight to the sky (0 = the sky gets none)
    float brightness  = 1.2f;      // the sky's light on the fog: the game's fog colour times this
    float sunLight    = 4.0f;      // the sun's light on the fog, against the air's ([volume] strength sets both)
    // The patches (2026-09-30): tiling noise fixed in the world, carried by the wind.
    float patchiness  = 0.5f;      // 0 = even fog; 1 = thick patches with clear air between
    float scale       = 60.0f;     // yards: how big a patch is
    float flatten     = 2.0f;      // patches are this many times wider than tall
    float windDeg     = 45.0f;     // the way the wind blows: 0 north, 90 east
    float windSpeed   = 1.5f;      // yards a second
    // The ground under it (2026-09-30), from the map files: needs [shadow] mapTerrain.
    float follow      = 0.5f;      // 0 = the fog lies on the ground smoothed over smoothRadius; 1 = on the ground
                                   // itself, as thick on a hilltop as in the valley below it
    float smoothRadius = 100.0f;   // yards
    float lowGround   = 1.5f;      // the fog this much thicker again where the ground lies lowDepth below the
    float lowDepth    = 25.0f;     // smoothed ground around it (valleys, hollows)
    float water       = 2.1f;      // this much thicker again over rivers, lakes and the sea
    float morning     = 1.0f;      // this much thicker at dawn (6:00), half of it at dusk (20:00)
    float lampMist    = 0.5f;      // a lamp's glow in the air this much brighter for each time the fog's ground
                                   // density around it (2026-09-30); the fog between you and it dims it either way
    int   debug       = 0;         // 1 = the transmittance (white = clear), 2 = the sky's light on the fog alone
};

// Where the sun is, for the shadow map and the volumetric light (sun.cpp). By default it is the sun the
// client draws in the sky, so both follow the time of day.
struct SunSettings
{
    bool  fixed     = false;        // true: a fixed world direction, azimuth/elevation in degrees (Z up)
    float azimuth   = 45.0f;        // the afternoon sun logged while tuning
    float elevation = 50.0f;
    // Seconds the sun's direction takes to follow the sky (a time constant; 2026-09-30, the Sun Smoothing
    // control). The sky moves its sun in steps of 0.56 degrees once a game minute; each step is followed at a
    // steady speed, over the time since the last step and at least this long. 0 = at once.
    float glide     = 3.0f; 
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
    float rain     = 0.3f;      // 0..0.9: how much darker again while it rains at night (Rain Darkness)
    bool  indoors  = false;     // true = darker inside buildings too
};

// Colour saturation by day and by night (grade.cpp, 2026-10-05). Between the two by [night] dusk, dawn and
// fade. Percent: 100 is the game's own colour, 0 grey; with both at 100 the pass does not run.
struct ColourSettings
{
    bool  enabled = true;       // the Color Effects box: off, the pass is not even considered
    float day   = 100.0f;       // 0..200: the saturation by day (Day Saturation)
    float night = 100.0f;       // 0..200: the saturation at full night (Night Saturation)
};

// A readable depth buffer (depth.cpp), which the volumetric light reads. Off unless enabled.
struct DepthSettings
{
    bool  enabled   = true;
    // A see-through model writes no depth (2026-09-30). A stealthed lion is drawn blended but writing depth,
    // so the sun shadows and the volumetric light, which read depth, shaded and lit its outline and gave it
    // away. comfyatmos.cpp, IsSeeThroughModel.
    bool  seeThrough = true;
    // Not while the camera is this near your own character (2026-09-30). Zoomed in, the client fades your
    // character and draws it as it draws a stealthed unit, a depth pass and then a see-through one; without
    // the depth pass the inside of the head showed through the back of it (the eyes, from behind).
    float seeThroughNear = 4.0f;
    // The same while you ride a mount (2026-10-02): the client fades the larger model from farther off. At 8.9
    // yards a Warhorse was faded, and the far side of it showed through.
    float seeThroughNearMounted = 15.0f;
    bool  waterDepth = true;   // the water writes depth, so the shadows and the light see its surface
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
    float nearRange = 32.0f;   // the near map, for the sun shadows: yards either side (0 = none). The far
                               // map's texel, a quarter of a yard, was too coarse for a trunk or a post
    float midRange  = 100.0f;  // the middle map, for the sun shadows past the near one: yards either side
                               // (0 = none). The far map's slack lost a merlon's shade on the wall behind
                               // it past the near map (2026-10-02)
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
    bool  terrainLow    = true;   // that ground cast from under the client's coarse far mesh (2026-10-03)
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
    bool  snap          = true;   // hold the far map on whole texels of its own grid, so shadow edges do
                                  // not crawl as you walk. On since 2026-10-02, with the held sun (sunStep)
    bool  nearSnap      = true;   // hold the near map on whole texels of its own grid, so shadow edges
                                  // near you do not hop as you walk (2026-10-02)
    float sunStep       = 0.05f;  // degrees: the maps' sun is held, and moved on in steps of this, so the
                                  // grids stand still between steps (2026-10-02). 0 = every frame
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
    float strength     = 25.0f;     // the dial, 0..100
    float maxIntensity = 3.0f;      // gain at 100
    float density      = 0.016f;     // how much the air scatters, per yard (Light Density, in thousandths)
    float maxDistance  = 190.0f;     // yards along each line of sight (the shadow map's reach)
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
    // At night, as % of strength (2026-09-30). The shadows had [night] strength with the rays and the light,
    // 25%: Sun Shadow Strength 20 became 5% at night, and players saw no shadows from the moon.
    float night      = 100.0f;
    // A low sun or moon (2026-09-30): the test's slack grows as the light gets lower (against flat ground
    // shading itself in stripes) and pushes each shadow away from what casts it, 0.5 yards on the near map
    // and 2 on the far one at 10 degrees. Below riseFrom the shadows' light climbs back toward riseTo as
    // the real light sinks, as the owner asked: at the horizon, and with neither sun nor moon up, the
    // shadows are short, as at noon. A steep light needs little slack. Until then they faded out between
    // 6 and 0 degrees.
    float riseFrom   = 12.0f;     // degrees: the real light's height below which the shadows' light climbs
    float riseTo     = 85.0f;     // degrees: the shadows' light with the real light at the horizon or under
    // The characters' shadows darker than the world's (2026-09-30). In Darkshire the buildings' shade took
    // so much of the street that a player's shadow was lost in it: a shaded pixel is shaded once, whatever
    // shades it. Players and creatures are drawn a second time into a map of their own, and their shade
    // darkens by unitStrength more, on top of the world's, inside a building's shade too.
    float unitStrength = 45.0f;   // 0..100: the extra, as Sun Shadow Strength is
    float unitGap    = 0.5f;      // yards along the sun over which the extra fades in from the unit, so a
                                  // character darkens its own back and arms little. Until 2026-09-30 a hard
                                  // gap, on top of the world's slack: with a low sun the dark shadow started
                                  // 1.6 yards from the feet. Since 2026-10-01 on a body only (bodymask.cpp)
    float unitDrop   = 4.0f;      // yards under the unit past which the extra fades out, over 3 more: on the
                                  // water under a bridge, already in the bridge's shade (2026-10-01)
    float bodyShade  = 30.0f;     // 0..100: the shade on a player's or creature's own body, as a share
                                  // (Character Backside Shadow). 100 = all of it, 0 = none (2026-10-01)
    float bias       = 3.0f;      // texels of slack in the depth test at the least; more as the sun gets
                                  // lower (see sunshadows.cpp). Against a surface shading itself
                                  // in bands (of each map: at 2048, a quarter of a yard is 1 texel of the
                                  // far map, 8 of the near one)
    float minGap     = 0.0f;      // yards: a blocker nearer than this along the sun does not shade. Stands
                                  // in for no shadow on itself (an arm on the body, leaves on leaves); on
                                  // both maps alike, where bias is in texels of each
    float sunOffset  = 0.06f;     // yards each point is moved toward the sun before the test, against
                                  // the same. Values tuned in game with /atmos (2026-09-29)
    float normalBias = 4.0f;      // texels of the finest map each point is moved along its rebuilt facing,
                                  // up to 4 times that where the sun grazes the surface, and only where it
                                  // grazes it or is behind it: walls the sun grazes shaded themselves in
                                  // stripes without it. The facing is per triangle on a model, so the bodies
                                  // the mask finds take none of it (2026-10-02). 2 until 2026-10-04: the side
                                  // of Stormwind's gate tower the sun grazed was speckled
    float slope      = 0.0f;      // 0..1: how much of the surface's slope (from the same facing) sets each
                                  // tap's depth. Against stripes on sloped ground at a high softness; on
                                  // a model it made patches where the arm shades the body
    float softness   = 1.0f;      // how far apart the nine taps of the soft edge are, in map texels.
                                  // Each tap blends four texels, so 0 is sharp but not stepped
    float baked      = 0.0f;      // 0..1: how much of the terrain's own baked shadow is kept while these
                                  // draw (terrainshade.cpp). It points one way at every hour. Back in
                                  // full as they fade at dusk
    float terrainShade = 0.6f;    // 0..1: the share of the sun that hills and mountains stop, with
                                  // [shadow] terrainLeaves (2026-10-02; leafShade until then)
    float terrainBias  = 1.5f;    // yards: the least slack in the terrain map's depth test. A hill shades
                                  // from yards away; less, and the ground shades itself in faint bands
    float hillCarry    = 30.0f;   // yards: inside a hill's shade, a tree's or a wall's shade is kept in full
                                  // this far from what it falls on, and gone at twice as far (2026-10-04).
                                  // 0 = no limit
    float water        = 0.4f;    // 0..1: the shade on the water's own surface, as a share of the ground's, soft
                                  // edged (Shadow on Water, 2026-10-06); the bed under it keeps its own
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
    float strength     = 26.0f;     // the dial, 0..50 on its slider (Lamp Glow)
    float maxIntensity = 10.0f;     // the glow in the air: gain at 100. 4 until 2026-09-28, too faint to see
    float surface      = 7.0f;      // a lamppost's light on the surfaces near it: gain at 100 (0 = none).
                                    // About 1 / the night's own light, so a lamp lights like a torch
    // Two kinds of light (2026-10-01), each with its own share of the glow and of the light on surfaces. In a
    // Duskwood camp seven pole torches and a campfire stood within a few yards, and a walkway lamppost beside
    // it, one light four yards up, looked dim next to them.
    float torchLight   = 0.23f;     // fires: torches, braziers, campfires, fireplaces, and the game's own lights
                                    // (those NPCs carry too). The game lights its models with its own lights
                                    // already, so at 1 a model near one is lit twice. Torch Light, in percent
    float lanternLight = 0.69f;     // lamps: lampposts, lanterns, candles, chandeliers, and the buildings' own
                                    // lights. Lantern Light, in percent
    float indoors      = 0.29f;     // all lamps while you are inside a building, eased over half a second at the
                                    // door. The Goldshire inn holds 34 lights within 30 yards, and with every one
                                    // drawn (2026-10-01) the air between them washed out. Indoor Lamps, in percent
    float density      = 0.03f;     // how much the air scatters a lamp's light, per yard
    float day          = 51.0f;     // % of the night strength by day (Lamps by Day), by [night] dusk, dawn and fade
    float maxDistance  = 120.0f;    // yards: a light further away than this adds nothing
    int   maxLights    = 32;        // the most lights one screen tile draws, nearest first, 1..32 (lampglow.cpp)
    float keep         = 2.0f;      // seconds a light on screen may go unseen before it fades out. A light
                                    // off screen is kept: the client draws a lamp's sprite only while the
                                    // lamp is on screen
    float softness     = 0.4f;      // yards: the radius of a light's bright core
    float through      = 1.5f;      // yards the glow runs on past the first surface in the line of sight.
                                    // The client's light sits inside the torch head or the brazier bowl,
                                    // and at 0 the bowl hid it from below or from the side
    bool  sprites      = true;      // also glow around lampposts, found by their glow sprite
    float fogReach     = 2.6f;      // how far into the fog a lamp still glows: the fade (over the far half of
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
    // Seconds the rays and the light take to fade when the sun is covered and come back when it is clear
    // (cover.cpp). 0.5 until 2026-09-30, and after a ship crossed the sun the shafts came back slowly.
    float coverTime   = 0.15f;
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

// The water (water.cpp, 2026-10-02): foam where it is shallow, drawn over each water chunk the client draws.
struct WaterSettings
{
    bool  enabled   = true;
    float foam      = 0.35f;     // 0..1: how white the foam is
    float foamWidth = 1.0f;      // yards of depth: the foam thins out to nothing at this depth
    float edgeLine  = 0.1f;      // 0..1: the thin light line where the water meets the sand (Edge Line)
    float edgeWidth = 0.1f;      // yards along the ground the edge line spans (Edge Line Width)
    float shoreFoam = 0.85f;      // 0..1: the game's foam texture near the shore
    float shoreFoamSize = 5.0f;  // yards one copy of that texture spans
    float foamReach = 4.0f;      // yards out from the waterline: the same across the water, for a gentle beach
    float foamScale = 0.6f;      // yards: the size of a bubble in the foam
    float foamSpeed = 1.0f;      // how fast the foam moves and the bands roll in to the shore
    float ripples   = 0.4f;      // 0..1: the rings round anyone standing in the water
    float rippleDepth = 1.0f;    // 0..4: how deep the rings' waves look round anyone standing still, their foam
                                 // line left as it is (Standing Ripple Depth)
    float rippleDepthMoving = 3.4f;   // 0..4: the same for the rings of anyone walking or swimming (Moving Ripple Depth)
    float rippleSpread = 0.5f;   // 0.1..3: how fast the rings round anyone standing still grow, so how large they get:
                                 // 1 is 1.1 yards a second (Standing Ripple Spread)
    float rippleSpreadMoving = 1.0f;  // 0.1..3: the same for the rings of anyone walking or swimming (Moving Ripple
                                      // Spread): 1 is a third of their speed, 1.1 to 3 yards a second
    float wetSand   = 0.45f;     // 0..1: how much darker the sand is where the water reaches
    // The surface (2026-10-02): the water drawn by us in place of the game's: its colour by the real depth, the
    // sky in it, the sun's glint, small waves, whitecaps far out.
    float surface   = 1.0f;      // 0..1: how much of our water is drawn over the game's (0 = the game's alone)
    float clarity   = 1.0f;      // how clear the water is: the light it absorbs is divided by this
    float colour    = 25.0f;     // 0..100: the colour deep water turns, green (0), teal (50) or blue (100)
    float reflection = 0.15f;    // 0..1: how much of the sky the water shows at a glancing look
    DWORD skyColor  = 0x5A9AD8;  // the sky high up, as the water reflects it; the horizon is the game's fog colour
    float brightness = 0.7f;     // 0.25..2: the water's own colour, lighter or darker (Water Brightness)
    float glint     = 1.0f;      // 0..3: the sun's glint on the waves (Sun Glint)
    float moonGlint = 0.6f;      // 0..3: the moons' glint on the waves by night (Moon Glint)
    float glintSize = 1.0f;      // 0.25..4: how wide the glint spreads: 2 = twice as wide (Glint Size)
    float waves     = 1.0f;      // 0..3: how choppy the small waves are
    float waveHeight = 1.0f;     // yards: how tall the swell rises, out where the water is 6.5 yards deep or more
    float waveScale = 2.0f;      // their size: 2 = twice as long (2026-10-02: 1 was too small)
    float whitecaps = 0.3f;      // 0..1: foam on the waves out in deep water (Open Water Foam Amount)
    // The drawn foam (2026-10-05): shapes made in the shader, in the style of Breath of the Wild, in place of the
    // game's waterfall texture. See NOTES.md.
    bool  foamDrawn = true;      // the drawn foam; off, the game's texture as before (Drawn Foam)
    float foamCell  = 0.9f;      // yards: the size of its blobs (Foam Size)
    float foamLife  = 3.0f;      // yards out from the moving edge it lasts (Foam Reach)
    float wakeFoam  = 0.5f;      // 0..1: the drawn foam's splash at a body moving through the water (Wake Foam)
    float shipWake  = 0.0f;      // 0 off, or 2..20: a ship's wake, as a body's this many times the size (Ship Wake);
                                 // off by default (the owner, 2026-10-06), 10 is a good size
    float shipWakeForward = 15.0f;  // 0..60 yards: how far ahead of the hull's origin the wake starts
    float shipWakeDepth   = 2.0f;   // 0..6: the ship's wake's waves, times a body's (Ship Wake Depth)
    float lakeSwash = 0.0f;      // 0..1: the share of the sea's swash and shore waves a lake, a pond or a river gets
                                 // (Lake Swash)
    float lakeFoam  = 0.3f;      // 0..1: the share of the sea's shore foam it gets (Lake Foam)
    float lakeWaves = 0.0f;      // 0..1: the share of the sea's swell (Wave Height) it gets (Lake Waves)
    float rain      = 1.0f;      // 0..2: the rain's rings on the water, while the game rains (Rain on Water); 0 none
    float objectFoam = 0.6f;     // 0..1: the drawn foam round objects standing in the water (Object Foam)
    float objectFoamWidth = 0.8f;  // yards out from an object the foam reaches (Object Foam Width)
    float foamEdge  = 0.6f;      // 0..0.9: its cut at the water's edge: higher leaves more holes in the youngest
                                 // foam (0.2 made a solid sheet over the swash)
    bool  openFoam  = true;      // foam on the swell's crests out in deep water, at whitecaps (Open Water Foam)
    float swash     = 0.6f;      // above 0: the water runs up the beach and back (Swash, on or off)
    // How far up the shore it goes (Swash Height): yards of height at most, and up a gentle beach 5 times that
    // along the ground. One value since 2026-10-05: swashRun (1.5) and this (0.3) were two limits on one thing.
    float swashHeight = 0.05f;   // the owner's (2026-10-05); 0.3 until then
    float swashLength = 90.0f;   // yards along the shore one surge spans (Swash Length): lower, more and shorter
    float swashSpeed  = 0.3f;    // how fast the surges come (Swash Speed): 1 is one every 11.4 s
    float wake      = 0.85f;     // 0..1: the wake behind anyone moving through the water (Wake)
    float cover     = 0.5f;      // 0..1: how much the water covers a body in it (Underwater Cover)
    float refraction = 0.125f;   // yards: how far the waves bend what lies under the water (Underwater Distortion:
                                 // 0..100 is 0..0.5 yards)
    bool  gameWake  = false;     // the game's own wake and splash round a unit in the water; off, the ripples
                                 // take their place
    DWORD foamColor = 0xEEF4F6;  // the foam's colour by day; the night darkens it
    float fadeEnd   = 150.0f;    // yards: the foam fades out over the last 40% of this distance
    int   debug     = 0;         // 1 = the water as drawn, marked over what lies under it (Debug View 16);
                                 // 2 = the foam alone, white on black; 3 = the wet sand alone; 10 = the depth
                                 // under the water on a doubling scale (Debug View 29)
    int   debugSkip = 0;         // for measuring (2026-10-06), added up: 1 leaves out the sand part's draw, 2 the
                                 // water part's, 4 the bodies' draw
};

// Lighthouses at night (beacon.cpp, 2026-10-05): a beacon in the lamp room and two turning beams.
struct LighthouseSettings
{
    bool  enabled = true;        // the Lighthouses box
    float beacon  = 2.0f;        // 0..3: how bright the beacon is
    float beam    = 0.12f;       // 0..1: how bright the beams are (Lighthouse Beam)
    float lampDrop = 9.0f;       // yards under the tower's tip the lamp is, where the tower is one group
    float day     = 1.0f;        // 0..1: its strength by day, as a share of the night's (1: as by night)
    float lampRise = 20.0f;      // yards over the game's own light's origin (LIGHTHOUSEEFFECT) the lamp is
    float lampShift[2] = { 2.38f, 0.97f };   // yards across from that origin (x, y): the owner's, in Stormwind's harbour
    bool  hideGameLight = true;  // leave out the game's own lighthouse light: ours takes its place
    // Its sizes (2026-10-05, the owner: tunable with /atmos).
    float beaconSize = 2.5f;     // yards: the beacon's radius (never less than about a fifth of a degree on screen)
    float beamWidth  = 0.5f;     // yards: the beam's half width at the lamp
    float beamSpread = 0.035f;   // yards it widens a yard out: 0.035 is about 2 degrees each side
    float beamLength = 400.0f;   // yards each beam reaches (200 at first)
    float beamSpeed  = 12.0f;    // seconds the beams take to go round
    float beamTilt   = -0.03f;   // the beam's slope: below 0 it points a little down
    int   beamCount  = 1;        // 1 beam, or 2 opposite
    float surface      = 1.5f;   // 0..10: its light on the surfaces round it, times beacon (the lamps' pass)
    float surfaceReach = 25.0f;  // yards that light reaches
    float glint        = 2.0f;   // 0..3: its glitter on the water, times beacon (Lighthouse Glint)
    float faceStrength = 1.6f;   // 0..4: the wave faces the beam lights on the water (Wave Face Strength)
    float faceTilt     = 0.05f;  // 0..0.3: the tilt toward the lighthouse a wave face starts to catch it (Wave Face Tilt)
    float faceSoft     = 0.08f;  // 0.005..0.3: how gradual the edge between lit and dark faces is (Wave Face Softness)
    float waterWidth   = 1.5f;   // 0.25..4: the patch of water the beam lights, as a share of the drawn beam's width
                                 // (Beam Width on Water)
    DWORD color   = 0xFFE2A8;    // the light's colour, warm white
};

// The grass in the wind (grass.cpp, from comfygrass, 2026-10-06): the client's grass draws through a vertex shader
// of ours that sways each blade and parts the blades round the player.
struct GrassSettings
{
    bool  enabled         = true;   // the Grass box
    float scale           = 2.0f;   // 0..5: the wind's strength; the parting does not follow it (Grass Wind)
    float directionDeg    = 45.0f;  // the wind's heading in the world's x-y plane, degrees (Grass Wind Direction)
    float speed           = 3.0f;   // yards a second the waves travel (Grass Wave Speed)
    float amplitude       = 0.060f; // yards the first wave sways a blade's tip
    float wavelength      = 18.0f;  // yards from one wave to the next (Grass Wave Length)
    float crossAmplitude  = 0.020f; // yards the second wave, across the first, sways the tip
    float crossWavelength = 6.5f;   // yards
    float crossAngleDeg   = 35.0f;  // degrees between the two waves
    float lean            = 0.35f;  // a constant lean down the wind, as a share of the first wave (Grass Lean)
    float variance        = 0.6f;   // 0..1: how much the sway differs from blade to blade
    float anchor          = 0.70f;  // the share of a blade from its base that does not move, when the vertex
                                    // carries no height (models = 0, or the fill loop is not the expected code)
    bool  worldPhase      = false;  // 1: the wave's phase adds the draw's world offset (continuous across chunks,
                                    // but it slides as the camera moves)
    // The parting round the player.
    bool  parting     = true;
    float radius      = 2.0f;   // yards round the player (Grass Parting Radius)
    float forceCenter = 0.5f;   // the lean at the player (Grass Parting)
    float forceEdge   = 0.0f;   // the lean at the radius
    float centerZ     = 0.0f;   // yards the parting's centre is moved up from the feet
    float zRange      = 4.0f;   // grass more than this many yards above or below the player does not part
    float zFade       = 2.0f;   // yards over which it fades out at that limit
    // The detail models: a height in each vertex, and rigid models (see grass.cpp).
    bool  models      = true;            // patch the client's fill loop
    float rigidHeight = 0.3f;            // yards: a model lower than this does not move
    char  rigidNames[128] = "Roc,Bon";   // a model whose file name holds one of these words does not move
    DWORD fillAddr    = 0x006B26CD;      // the instruction that picks each instance's vertex colour
    // Which draws are grass: fixed function, a translation-only world matrix, and these.
    UINT  stride      = 36;
    UINT  minVerts    = 64;
    UINT  maxVerts    = 0;      // 0: no limit
    int   primType    = 4;      // D3DPT_TRIANGLELIST
    int   debug       = 0;      // 1: the bend as a colour (Debug View 30)
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
    WaterSettings water;
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
    ColourSettings colour;
    LighthouseSettings lighthouse;
    GrassSettings grass;
    ClientSettings client;

    bool  trace       = false;      // F12 then also traces the next 180 frames of the volumetric light:
                                    // what it marched, what the shadow map held, what the cache did. It
                                    // reads buffers back from the GPU, so it is off unless asked for.
    bool  logEnabled  = true;
    bool  hook        = true;       // 0: load, log, patch nothing (bisecting)
    bool  sliders     = true;       // register the CVars the in-game controls set (cvars.cpp)
    bool  master      = true;       // [general] enabled: every effect at once. Off, the game draws as
                                    // stock: light, shadows, rays and lamps all off
    int   reloadKey   = VK_F11;     // reload comfyatmos.ini
    int   probeKey    = VK_F12;     // log one frame of fog state changes and draw counts; with Alt, benchmark
    int   chainWaitMs = 10000;      // how long to wait for an old comfygrass.dll to finish patching first
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
// The ini beside the DLL: comfyatmos.ini, or comfyfog.ini when only the old name is there (then true).
bool ResolveIniPath(HMODULE self, wchar_t* out, size_t count);
