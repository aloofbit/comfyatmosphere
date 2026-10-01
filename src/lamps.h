// Local lights: street lamps, lanterns, torches. Found here (the tracker, and the F12 probe that found
// where to look); the fog glow around them is drawn by lampglow.cpp. See lamps.cpp.
#pragma once

#include <d3d9.h>

// One client draw in the world, for the probe window and the tracker.
struct LampDraw
{
    bool                    indexed;
    UINT                    first;      // first vertex the draw reads (base + min index for indexed draws)
    UINT                    nv;         // how many vertices it reads
    const void*             up;         // DrawPrimitiveUP data, or null
    UINT                    upStride;
    const D3DMATRIX*        world;      // the fixed-function world matrix now set (camera-relative)
    IDirect3DVertexShader9* vs;         // the shader now bound, or null
    DWORD                   fogColor;   // the client's fog colour: black for additive passes
};

// A light for the glow, as LampsGather hands it out: camera-relative, nearest first.
struct LampLight
{
    float pos[3];       // yards, camera-relative
    float colour[3];    // with its fade in and out already applied
    float reach;        // yards
    float dist;         // yards from the camera
    int   kind;         // 0 = a client point light (torch, brazier), 1 = a glow sprite (lamppost)
    bool  fire;         // a torch, brazier or campfire: [lamps] torchLight; otherwise a lamp: lanternLight
};

void LampsProbe(IDirect3DDevice9* dev);                  // F12: open a window of kWindow frames
bool LampsActive();                                      // is a window open?
void LampsSetTracking(bool on);                          // each frame: does the glow want lights?
bool LampsTracking();
bool LampsWanted();                                      // LampsActive() || LampsTracking(): send draws
void LampsConstants(UINT reg, const float* data, UINT count);   // a client vertex-shader constant upload
void LampsSetLight(DWORD index, const D3DLIGHT9* light, bool inWorld);   // a client SetLight
void LampsLightEnable(DWORD index, BOOL on);                    // a client LightEnable
void LampsDraw(IDirect3DDevice9* dev, const LampDraw& d);       // a client draw in the world phase
void LampsWorldEnded();                                  // the world is drawn: this frame's sightings go in
constexpr int kLampsMax = 256;                           // the most lights gathered in a frame
// The lights to draw this frame, nearest first, up to max. cam is the camera's world position, viewProj the
// camera-relative view and projection: only a light whose reach can show on screen counts.
int  LampsGather(const float cam[3], const D3DMATRIX& viewProj, LampLight* out, int max);
unsigned LampsTracked();                                 // how many lights are held, for the probe
void LampsFrameEnd();                                    // at Present: count the frame, report at the end
void LampsReset();                                       // before Reset or a new device
