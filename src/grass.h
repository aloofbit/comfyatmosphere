// The grass in the wind (grass.cpp, from comfygrass, 2026-10-06).
#pragma once

#include <d3d9.h>

// The device's own calls, past every hook, which the grass draws and restores through (comfyatmos.cpp, at attach).
struct GrassCalls
{
    HRESULT (STDMETHODCALLTYPE* drawPrim)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    HRESULT (STDMETHODCALLTYPE* drawIdxPrim)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    HRESULT (STDMETHODCALLTYPE* setVS)(IDirect3DDevice9*, IDirect3DVertexShader9*);
    HRESULT (STDMETHODCALLTYPE* setVSConstF)(IDirect3DDevice9*, UINT, const float*, UINT);
};
void GrassAttach(const GrassCalls& calls);

// One of the client's draws with no vertex shader of its own. The matrices are the client's last.
struct GrassDrawArgs
{
    bool             indexed;
    D3DPRIMITIVETYPE prim;
    INT              bvi;          // indexed: base vertex index
    UINT             mvi, nv, si;  // indexed: min vertex index, vertices, start index
    UINT             sv;           // not indexed: start vertex
    UINT             pc;
    const D3DMATRIX* world;
    const D3DMATRIX* view;
    const D3DMATRIX* proj;
};
// True when the draw is grass and was drawn here, with its result in hr. False: not grass, pass it on.
bool GrassDraw(IDirect3DDevice9* dev, const GrassDrawArgs& d, HRESULT& hr);

void GrassConstants(UINT reg, const float* data, UINT count);   // the client's vertex shader constants
void GrassFrameEnd();          // at Present: the fill loop's patch, and next frame's wind and player
void GrassReset();             // a Reset or a new device: the wind shader and the caches go
void GrassProbe();             // the probe (F12): what the grass did last frame
