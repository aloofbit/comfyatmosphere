// comfyfog.ini: one dial (thickness, 0..100) plus the few numbers that say what 100 looks like.
#pragma once

#include <windows.h>

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

    // Colour at 100. The sky is not fogged the way terrain is, so pulling the colour far from the
    // client's leaves a visible seam where fogged terrain meets sky. Keep these modest.
    float desaturate = 0.35f;        // 0 .. 1, toward grey
    float darken     = 0.20f;        // 0 .. 1, toward black
    DWORD tint       = 0x5A6470;     // RGB the colour is pulled toward, by tintAmount
    float tintAmount = 0.0f;         // 0 .. 1

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
    int   size    = 2048;     // texels per side. 4096 took twice the GPU time for the same look
                              // (benchmark, 2026-09-24: 2.35 against 1.66 microseconds a caster)
    float range   = 250.0f;    // yards covered either side of the player
    float depth   = 700.0f;   // yards toward and away from the sun: far enough for a ridge to shade you
    int   copyPerFrame  = 16;     // arena chunks copied into our own buffers per frame. The client
                                  // streams terrain through a buffer it re-fills, so a cached pointer
                                  // into it is worthless; a copy of our own is not. Reading the client's
                                  // memory back is slow, hence a limit per frame (a chunk is 3.5 KB).
    int   copyMax       = 2048;   // how many such copies to hold at once. 2 and 768 until 2026-09-24;
                                  // since then every streamed chunk needs a copy before it casts, and at
                                  // 2 a frame new ground stayed without shade while you walked
    int   mapEvery      = 3;      // rebuild the map every N frames. The map is anchored in the world and
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
    // and 1 (low) replace four of them with cheaper fixed values (ApplyVolumeQuality). Added 2026-09-24
    // because players reported low frame rates with the light on.
    int   quality      = 3;
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
    FogSettings  fog;
    SunSettings  sun;
    ClientSettings client;

    bool  trace       = false;      // F12 then also traces the next 180 frames of the volumetric light:
                                    // what it marched, what the shadow map held, what the cache did. It
                                    // reads buffers back from the GPU, so it is off unless asked for.
    bool  logEnabled  = true;
    bool  hook        = true;       // 0: load, log, patch nothing (bisecting)
    bool  sliders     = true;       // register the CVars the in-game controls set (cvars.cpp)
    int   reloadKey   = VK_F11;     // reload comfyfog.ini; with Shift, toggle the override
    int   probeKey    = VK_F12;     // log one frame of fog state changes and draw counts; with Alt, benchmark
    int   chainWaitMs = 10000;      // how long to wait for comfygrass to finish patching first
    int   minWorldDraws = 16;       // world draws needed before a switch to 2D counts as the end of the
                                    // world (depth, shadows and volumetric light run there)
};

extern Settings g_cfg;

void LoadSettings(const wchar_t* iniPath);

// [volume] quality below 3: the shadow map size, the march's steps and resolution, and how often the map
// is redrawn are replaced by cheaper fixed values. After the in-game controls are laid over the ini.
void ApplyVolumeQuality(Settings& s);
void ResolveIniPath(HMODULE self, wchar_t* out, size_t count);
