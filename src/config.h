// comfyfog.ini: one dial (thickness, 0..100) plus the few numbers that say what 100 looks like.
#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

struct FogSettings
{
    bool  enabled   = true;

    // The dial. 0 is the client's own fog, untouched; 100 is the heaviest the settings below allow.
    // Everything else in this struct describes the far end of the dial and is scaled by it.
    float thickness = 60.0f;

    // Fog already present right at the camera, at 100 (0..0.9). The client's fog is linear from 0, so
    // near things are nearly clear and it only builds with distance; a haze floor fills in the near
    // field. Done with a negative FOGSTART, which every fog path (grass shader included) handles.
    float haze      = 0.35f;

    // Where fog becomes total at 100, as a fraction of the client's own fog end. Kept well out, so the
    // climb after the haze floor is gentle rather than a wall. Interpolated geometrically in the dial.
    float reach     = 0.60f;

    // Our own fog (mode 1, 2026-09-29): drawn over the picture from the depth, while the volumetric light
    // runs (it needs the depth), with the game's fog moved out of the way. The game's fog is linear from
    // start to end, the same in the terrain and in the tree shaders, so a fog made stronger with it
    // still ended in a wall, and a far tree fogged in full stood out white against the sky behind it.
    // This one is height fog: thick low down and thinning upward, so a line of sight toward the sky
    // meets a known amount of it, and the sky near the horizon is the fog colour. Mode 0, or with the
    // volumetric light off, is the game's fog moved by haze and reach as before.
    int   mode      = 1;
    float density   = 0.005f;   // the height fog: fog per yard at the camera's height (the Ground Haze
                                // control, 0..0.02). Not scaled by the dial: the dial is the distance
                                // fog. 0.02 scaled by the dial was tuned before the distance fog existed,
                                // and with it on top the view distance shrank (2026-09-29)
    float height    = 40.0f;    // yards: the fog thins by e (2.7 times) every this many yards up
    float distance  = 1.0f;     // 0..1: how much of the game's distance fog (its start and end, as the dial
                                // moves them) is kept, on a soft curve. Far mountains need it
    float cover     = 0.85f;    // 0..1: past this share of the view distance, things fade in full into
                                // the fog by the view distance, so nothing stops at a hard edge
    float skyDepth  = 0.99999f; // depth at or past which a pixel is the sky itself. Between the world's
                                // slice and this: scenery drawn with the sky (far mountains), fogged in full
    int   debug     = 0;        // 1 = our fog's amount alone (white = all fog); 2 = that far scenery, red

    // The game's fog colour. Not part of the dial: applied as set, with the fog on or off (Shift+F11
    // turns it off with the rest). Until 2026-09-29 it was scaled by the dial, so these are the old
    // values at the default thickness of 60.
    float desaturate = 0.2f;         // 0 .. 1, toward grey
    float darken     = 0.12f;        // 0 .. 1, toward black
    DWORD tint       = 0x5A6470;     // RGB the colour is pulled toward, by tintAmount
    float tintAmount = 0.0f;         // 0 .. 1
    // The sky near the horizon gets the same change as the colour, so that seam does not show: it is
    // what shows between trees past the view distance, and a darker fog alone left it bright
    // (2026-09-29). Needs the depth buffer (Volumetric Light on).
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
};

// Sun rays and volumetric light at night (sun.cpp, NightScale). The client draws its night sky light with
// the same sprite as the sun, high in the sky (logged: 75 to 83 degrees up at 01:00), so the sun's
// height cannot tell night from day. The game clock can.
struct NightSettings
{
    float strength = 100.0f;    // the dial, 0..100: the rays and the light at night, as % of their day strength
    float dusk     = 20.0f;     // hour the change to night starts
    float dawn     = 5.0f;      // hour the change to day starts
    float fade     = 1.5f;      // hours each change takes
};

// A readable depth buffer (depth.cpp), which the volumetric light reads. Off unless enabled.
struct DepthSettings
{
    bool  enabled   = false;
};

// A shadow map from the sun (shadow.cpp): the frame's opaque world draws replayed from the sun.
struct ShadowSettings
{
    bool  enabled = false;
    int   size    = 2048;     // texels per side. 4096 took twice the GPU time and gave the volumetric
                              // light the same look (benchmark, 2026-09-24: 2.35 against 1.66
                              // microseconds a caster); it gives the sun shadows sharper edges. The
                              // Shadow Resolution control sets it: 1024, 2048 or 4096
    float range   = 250.0f;    // yards covered either side of the player
    float nearRange = 32.0f;   // the near map, for the sun shadows: yards either side (0 = none). The far
                               // map's texel, a quarter of a yard, was too coarse for a trunk or a post
    float depth   = 700.0f;   // yards toward and away from the sun: far enough for a ridge to shade you
    int   copyPerFrame  = 16;     // arena chunks copied into our own buffers per frame. The client
                                  // streams terrain through a buffer it re-fills, so a cached pointer
                                  // into it is worthless; a copy of our own is not. Reading the client's
                                  // memory back is slow, hence a limit per frame (a chunk is 3.5 KB).
    int   copyMax       = 2048;   // how many such copies to hold at once. 2 and 768 until 2026-09-24;
                                  // since then every streamed chunk needs a copy before it casts, and at
                                  // 2 a frame new ground stayed without shade while you walked
    int   mapEvery      = 3;      // rebuild the map every N frames, 1..8 (the Shadow Redraw control). The map is anchored in the world and
                                  // the light is smoothed over time, so 3 takes two thirds off the cost
                                  // of the replay for very little: the shade it holds is two frames old.
    bool  horizon       = true;   // keep the far-horizon draws: distant terrain can shade you too. They
                                  // are drawn with a camera of their own, which only matters for the
                                  // shader models, so those are still left out.
    bool  snap          = false;  // hold the map on whole texels of its own grid: steadier standing
                                  // still, but it steps as you walk, which reads worse
    float cacheTime     = 8.0f;   // seconds a caster out of view is kept. Every caster held is drawn
                                  // into the map, and at 15 long play grew the cache to 2,500 entries
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
    bool  enabled      = false;
    float strength     = 30.0f;     // the dial, 0..100
    float maxIntensity = 3.0f;      // gain at 100
    float density      = 0.009f;     // how much the air scatters, per yard
    float maxDistance  = 250.0f;     // yards along each line of sight (the shadow map's reach)
    int   steps        = 64;        // samples along each line of sight: more holds up over a long
                                    // maxDistance, where a thin canopy can fall between two samples.
                                    // 96 until 2026-09-24; with the noise turned each frame and the
                                    // last frame kept, 64 looked the same in game
    float smooth       = 0.85f;     // 0..0.95: how much of the last frame's glow is kept. The march is
                                    // noisy and the map changes under it, and the light jittered as you
                                    // walked. The last frame is moved with the camera before it is
                                    // blended in, so a turn does not smear. 0 also stops the noise
                                    // pattern from changing each frame.
    float anisotropy   = 0.15f;     // 0 = glows the same from every side, toward 1 = only toward the sun
    float bias         = 0.5f;      // yards: shadow-test slack, against speckle on lit surfaces
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
    float strength   = 50.0f;     // the dial, 0..100: how much of the light a shaded surface loses
    float bias       = 3.0f;      // texels of slack in the depth test, against a surface shading itself
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
    int   debug      = 0;         // 1 = the shade alone (white = lit)
};

// Lamps, lanterns and torches (lamps.cpp finds them, lampglow.cpp draws): the fog glowing around them, and
// lampposts lighting the surfaces near them as the client's torches do.
// It reads the volumetric light's depth and world camera, so it draws only while [volume] does, but at
// night too, when the sun's light is turned down. Added 2026-09-28.
struct LampSettings
{
    bool  enabled      = true;
    float strength     = 40.0f;     // the dial, 0..100
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
    float strength    = 35.0f;
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
    float maxAngle    = 140.0f;     // degrees between view and sun at which rays are gone
    float viewFalloff = 1.0f;       // shape of the fade from looking at the sun to maxAngle; higher = faster
    float parallel    = 0.0f;       // 0 = rays fan out from the sun, 1 = parallel shafts falling away from it.
                                    // 0 is the geometrically right one: parallel shafts in the world run to the
                                    // sun on screen, and a simulation showed 1 swings MORE as the camera turns
    float adaptTime   = 0.5f;       // seconds the brightness reference takes to follow the scene
    float decay       = 0.96f;      // per-sample falloff along a ray; lower = shorter, softer shafts
    float soften      = 8.0f;       // pixels of the mask (downscale x this on screen): the mask is blurred
                                    // this wide before the rays are drawn, so a gap between leaves a pixel
                                    // wide no longer makes a whole ray blink (2026-09-29). 0 = none
    float smooth      = 0.7f;       // 0..0.95: how much of the last frame's mask is kept, moved with the sun.
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
    bool clouds = true;   // false: the sky's cloud layer is not drawn
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
