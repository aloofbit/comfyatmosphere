// comfyfog.ini -- one dial (thickness, 0..100) plus the few numbers that say what 100 looks like.
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
    // client's leaves a visible seam where fogged terrain meets sky -- keep these modest.
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

    // A pixel casts rays when it is within relThreshold of the brightest pixel in the frame -- so the
    // sky gaps in a dim, foggy forest cast as surely as the sky beside the sun -- and above threshold,
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

    // Where the sun is. 0: pinned to the screen at (sunX, sunY), texture space, y down -- "12 o'clock"
    // is top centre just above the edge. 1: a fixed world direction, azimuth/elevation in degrees
    // (Z up), projected through the client's camera. 2: the sun the client draws in the sky, so the rays
    // come from the visible sun and follow the time of day.
    int   sunMode     = 2;
    float sunX        = 0.5f;
    float sunY        = -0.30f;     // above the top edge; more negative = more parallel, wider rays
    float azimuth     = 45.0f;      // sunMode 1: the afternoon sun logged while tuning
    float elevation   = 50.0f;

    int   debugView   = 0;          // 1 = show the mask, 2 = show the rays alone

    // 0: at the world -> UI boundary, so the UI gets no rays (and loading screens none at all).
    // 1: over the finished frame at Present, UI included -- the fallback if the boundary is ever missed.
    int   placement     = 0;
    int   minWorldDraws = 16;       // world draws needed before a switch to 2D counts as the boundary
};

// World-space shafts around the player (beams.cpp). They take their colour from [rays] color.
struct BeamsSettings
{
    bool  enabled      = false;     // superseded by [volume]; kept for comparison
    float strength     = 50.0f;     // the dial, 0..100
    float maxIntensity = 0.6f;      // brightness at 100

    float spacing      = 7.0f;      // grid cell, yards: at most one shaft per cell
    float density      = 0.35f;     // 0..1 share of cells that hold a shaft
    float radius       = 35.0f;     // shafts within this many yards of the player
    float height       = 28.0f;     // how high a shaft rises toward the sun, yards
    float baseOffset   = -3.0f;     // where it starts relative to the player's feet, yards
    float widthMin     = 0.5f;      // shaft width range, yards (weighted toward thin)
    float widthMax     = 3.5f;
    float heightVar    = 0.4f;      // 0..1: heights range over (1 -/+ this) x height
    float clusterChance = 0.3f;     // 0..1 share of shafts that come with two thin companions
    float forwardPower = 3.0f;      // how tightly the glow gathers toward the sun
    float backLight    = 0.25f;     // brightness with the sun behind you, 0..1 of facing it
    float nearFade     = 3.0f;      // yards: shafts closer than this to the camera fade out
    float shimmer      = 0.25f;     // 0..1 slow per-shaft breathing
    int   maxBeams     = 96;
    float indoorHold   = 1.5f;      // seconds without the sky's sun before counting as indoors

    // Canopy overhead, 0 (open sky) .. 1 (dense leaves), from the top half of the frame: shafts start at
    // canopyStart and are at full strength by canopyFull. Eased over canopyTime seconds.
    float canopyStart  = 0.35f;
    float canopyFull   = 0.6f;
    float canopyTime   = 2.0f;

    // Where the client keeps the camera and the player (verified for this WoW.exe by comfygrass).
    DWORD camAddr      = 0x00C7CF20;
    DWORD objMgrAddr   = 0x00B41414;
    DWORD playerPosOff = 0x9B8;
};

// The game's time of day, written into the client (timeofday.cpp). Off unless enabled.
struct TimeSettings
{
    bool  enabled      = false;
    float hour         = 13.0f;       // 0..24, fractions allowed (13.5 = 13:30)
    DWORD addrMinutes  = 0x00CE9B60;  // int minutes since midnight   (found by the Ctrl+F12 search)
    DWORD addrFraction = 0x00CE9B64;  // float fraction of the day
    DWORD addrMinutesF = 0x00CE8574;  // float minutes since midnight; 0 = leave alone
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
    int   size    = 2048;     // texels per side
    float range   = 60.0f;    // yards covered either side of the player
    float depth   = 300.0f;   // yards toward and away from the sun
};

// Volumetric light (volume.cpp): the fog glowing where the sun reaches it. Needs [depth] and [shadow].
struct VolumeSettings
{
    bool  enabled      = false;
    float strength     = 50.0f;     // the dial, 0..100
    float maxIntensity = 1.0f;      // gain at 100
    float density      = 0.02f;     // how much the air scatters, per yard
    float maxDistance  = 60.0f;     // yards along each line of sight (the shadow map's reach)
    float anisotropy   = 0.6f;      // 0 = glows the same from every side, toward 1 = only toward the sun
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
    TimeSettings time;
    FogSettings  fog;
    RaysSettings rays;
    BeamsSettings beams;

    bool  logEnabled  = true;
    bool  hook        = true;       // 0: load, log, patch nothing (bisecting)
    int   reloadKey   = VK_F11;     // reload comfyfog.ini; with Shift, toggle the override
    int   probeKey    = VK_F12;     // log one frame of fog state changes and draw counts
    int   chainWaitMs = 10000;      // how long to wait for comfygrass to finish patching first
};

extern Settings g_cfg;

void LoadSettings(const wchar_t* iniPath);
void ResolveIniPath(HMODULE self, wchar_t* out, size_t count);
