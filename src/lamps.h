// A probe for local lights: street lamps, lanterns, torches. See lamps.cpp.
#pragma once

#include <d3d9.h>

// One client draw in the world, for the probe window.
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

void LampsProbe(IDirect3DDevice9* dev);                  // F12: open a window of kWindow frames
bool LampsActive();                                      // is a window open? everything else is a no-op if not
void LampsConstants(UINT reg, const float* data, UINT count);   // a client vertex-shader constant upload
void LampsSetLight(DWORD index, const D3DLIGHT9* light);        // a client SetLight
void LampsLightEnable(DWORD index, BOOL on);                    // a client LightEnable
void LampsDraw(IDirect3DDevice9* dev, const LampDraw& d);       // a client draw in the world phase
void LampsFrameEnd();                                    // at Present: count the frame, report at the end
void LampsReset();                                       // before Reset or a new device
