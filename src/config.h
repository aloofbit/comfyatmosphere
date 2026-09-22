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

struct RaysSettings
{
    bool  enabled     = true;

    // The dial. 0 = no rays, 100 = maxExposure. The rest describes the look and is not scaled by it.
    float strength    = 35.0f;
    float maxExposure = 2.0f;

    // A pixel casts rays when it is within relThreshold of the brightest pixel in the frame (so the
    // sky gaps in a dim, foggy forest cast as surely as the sky beside the sun), and above threshold,
    // an absolute floor that keeps a dark cave's dim lights from streaking.
    float relThreshold = 0.75f;     // 0..1 of the frame's brightest luminance
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

    // Where the sun is. 0: pinned to the screen at (sunX, sunY), texture space, y down. "12 o'clock"
    // is top centre, directly above the edge. 1: a fixed world direction, azimuth/elevation in degrees
    // (Z up), projected through the client's camera. 2: the sun the client draws in the sky, so the rays
    // come from the visible sun and follow the time of day.
    int   sunMode     = 2;
    float sunX        = 0.5f;
    float sunY        = -0.30f;     // above the top edge; more negative = more parallel, wider rays
    float azimuth     = 45.0f;      // sunMode 1: the afternoon sun logged while tuning
    float elevation   = 50.0f;

    int   debugView   = 0;          // 1 = show the mask, 2 = show the rays alone

    // 0: at the world -> UI boundary, so the UI gets no rays (and loading screens none at all).
    // 1: over the finished frame at Present, UI included: the fallback if the boundary is ever missed.
    int   placement     = 0;
    int   minWorldDraws = 16;       // world draws needed before a switch to 2D counts as the boundary
};

// Where the client keeps the camera and the player (verified for this WoW.exe by comfygrass). Shadows
// and volumetric light read them through client.cpp.
struct ClientSettings
{
    DWORD camAddr      = 0x00C7CF20;
    DWORD objMgrAddr   = 0x00B41414;
    DWORD playerPosOff = 0x9B8;
};

// A readable depth buffer (depth.cpp), the groundwork for volumetric light. Off unless enabled.
struct DepthSettings
{
    bool  enabled   = false;
    float viewRange = 150.0f;     // [rays] debugView = 3: distance, yards, that shows as black
};

// A shadow map from the sun (shadow.cpp): the frame's opaque world draws replayed from the sun.
struct ShadowSettings
{
    bool  enabled = false;
    int   size    = 4096;     // texels per side
    float range   = 250.0f;    // yards covered either side of the player
    float depth   = 700.0f;   // yards toward and away from the sun: far enough for a ridge to shade you
    int   copyPerFrame  = 2;      // arena chunks copied into our own buffers per frame. The client
                                  // streams terrain through a buffer it re-fills, so a cached pointer
                                  // into it is worthless; a copy of our own is not. Reading the client's
                                  // memory back is slow, hence a few per frame.
    int   copyMax       = 768;    // how many such copies to hold at once
    int   mapEvery      = 2;      // rebuild the map every N frames. The map is anchored in the world and
                                  // the light is smoothed over time, so 2 halves the cost of the replay
                                  // for very little: the shade it holds is one frame old.
    bool  horizon       = true;   // keep the far-horizon draws: distant terrain can shade you too. They
                                  // are drawn with a camera of their own, which only matters for the
                                  // shader models, so those are still left out.
    bool  snap          = false;  // hold the map on whole texels of its own grid: steadier standing
                                  // still, but it steps as you walk, which reads worse
    float cacheTime     = 15.0f;  // seconds a caster out of view is kept
    float evictDistance = 20.0f;  // yards: a caster in view this near that was not drawn is gone
};

// Volumetric light (volume.cpp): the fog glowing where the sun reaches it. Needs [depth] and [shadow].
struct VolumeSettings
{
    bool  enabled      = false;
    float strength     = 30.0f;     // the dial, 0..100
    float maxIntensity = 3.0f;      // gain at 100
    float density      = 0.009f;     // how much the air scatters, per yard
    float maxDistance  = 250.0f;     // yards along each line of sight (the shadow map's reach)
    int   steps        = 96;        // samples along each line of sight: more holds up over a long
                                    // maxDistance, where a thin canopy can fall between two samples
    float smooth       = 0.6f;      // 0..0.95: how much of the last frame's glow is kept. The march is
                                    // noisy and the map changes under it, and the light jittered as you
                                    // walked; it is eased back toward 0 while the camera moves, so a turn
                                    // does not smear.
    float anisotropy   = 0.15f;     // 0 = glows the same from every side, toward 1 = only toward the sun
    float bias         = 0.5f;      // yards: shadow-test slack, against speckle on lit surfaces
    int   downscale    = 2;         // work at 1/N resolution per axis
    bool  blur         = true;
    int   debug        = 0;         // 1 = the glow alone, white; 2..5 = one stage of the march (see ini)
};

struct SkySettings
{
    bool clouds = true;   // false: the sky's cloud layer is not drawn
};

struct Settings
{
    SkySettings  sky;
    DepthSettings depth;
    ShadowSettings shadow;
    VolumeSettings volume;
    FogSettings  fog;
    RaysSettings rays;
    ClientSettings client;

    bool  trace       = false;      // F12 then also traces the next 180 frames of the volumetric light:
                                    // what it marched, what the shadow map held, what the cache did. It
                                    // reads buffers back from the GPU, so it is off unless asked for.
    bool  logEnabled  = true;
    bool  hook        = true;       // 0: load, log, patch nothing (bisecting)
    int   reloadKey   = VK_F11;     // reload comfyfog.ini; with Shift, toggle the override
    int   probeKey    = VK_F12;     // log one frame of fog state changes and draw counts
    int   chainWaitMs = 10000;      // how long to wait for comfygrass to finish patching first
};

extern Settings g_cfg;

void LoadSettings(const wchar_t* iniPath);
void ResolveIniPath(HMODULE self, wchar_t* out, size_t count);
