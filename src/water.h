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
};

bool WaterWanted();   // the foam draws this frame
// Before the client's draw of a water chunk: the first call of a frame copies the depth under the water.
void WaterBeforeDraw(IDirect3DDevice9* dev, const WaterChunk& c);
// After it: the foam over it. `draw` is the device's DrawIndexedPrimitive below our hook.
void WaterAfterDraw(IDirect3DDevice9* dev, const WaterChunk& c, WaterDrawFn draw);
// The probe: each water draw of the frame (water), and each other blended fixed-function draw through a
// pixel shader (not water), before the client's draw. `index` is the draw's place in the frame.
void WaterProbeDraw(IDirect3DDevice9* dev, const WaterChunk& c, unsigned index, bool water);
// A fixed-function draw about to be made: the game's own wake on the water (see water.cpp)? `c` carries the
// draw's buffers and its world matrix.
bool WaterGameWake(IDirect3DDevice9* dev, const WaterChunk& c);
void WaterFrameEnd();   // at Present
void WaterReset();      // before Reset, and for a new device
void WaterProbe();      // log the next frame's water draws
bool WaterProbing();    // a probe frame is being logged
