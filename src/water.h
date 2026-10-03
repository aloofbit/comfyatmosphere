// The water's own look: foam where it is shallow. See water.cpp.
#pragma once

#include <d3d9.h>

using WaterDrawFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

// One water chunk as the client draws it, with the transforms it set for it.
struct WaterChunk
{
    D3DPRIMITIVETYPE prim;
    INT              baseVertex;
    UINT             minIndex, numVertices, startIndex, primCount;
    const D3DMATRIX* world;   // camera-relative
    const D3DMATRIX* view;    // rotation only
    const D3DMATRIX* proj;
    bool             city = false;   // water in a building (a WMO's liquid): no depth in its vertices, no swell
};

bool WaterWanted();   // the foam draws this frame
// Before the client's draw of a water chunk: the first call of a frame copies the depth under the water.
void WaterBeforeDraw(IDirect3DDevice9* dev, const WaterChunk& c);
// Whether the client's own draw of the chunk is left out this frame: our pass covers it entirely (see water.cpp).
bool WaterHidesGame();
// Whether the liquid texture bound at stage 0 is water's: blue or grey, not lava's red or slime's green.
// Read once a texture, from its smallest level.
bool WaterTextureIsWater(IDirect3DDevice9* dev);
// A pixel shader of one flat colour, for the debug view of the game's own liquid: red for water, magenta else.
IDirect3DPixelShader9* WaterFlatShader(IDirect3DDevice9* dev, bool water);
// After it: the foam over it. `draw` is the device's DrawIndexedPrimitive below our hook.
void WaterAfterDraw(IDirect3DDevice9* dev, const WaterChunk& c, WaterDrawFn draw);
// The probe: each water draw of the frame (water), and each other blended fixed-function draw through a
// pixel shader (not water), before the client's draw. `index` is the draw's place in the frame.
void WaterProbeDraw(IDirect3DDevice9* dev, const WaterChunk& c, unsigned index, bool water);
// A fixed-function draw about to be made: the game's own wake on the water (see water.cpp)? `c` carries the
// draw's buffers and its world matrix.
bool WaterGameWake(IDirect3DDevice9* dev, const WaterChunk& c);
// The depth under the water this frame (copied before the first water draw), for the sun shadows; or null.
IDirect3DTexture9* WaterUnderDepth();
void WaterFrameEnd();   // at Present
void WaterReset();      // before Reset, and for a new device
void WaterProbe();      // log the next frame's water draws
bool WaterProbing();    // a probe frame is being logged
// The probe: any draw (water or not) using one of the water's textures, logged with its states.
void WaterProbeTexture(IDirect3DDevice9* dev, const char* call, UINT nv, UINT pc, unsigned index, bool water);
