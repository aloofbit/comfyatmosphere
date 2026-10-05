// A readable depth buffer (INTZ) swapped in for the client's. See depth.cpp.
#pragma once

#include <d3d9.h>

void               DepthBeginScene(IDirect3DDevice9* dev);                // swap in at the start of a frame
IDirect3DSurface9* DepthSubstitute(IDirect3DDevice9* dev, IDirect3DSurface9* clientSurface);  // for SetDepthStencilSurface
void               DepthWorldEnded(IDirect3DDevice9* dev, bool resolve);  // remember which one holds the world;
                                                                          // resolve = resolve a multisampled one now
IDirect3DTexture9* DepthWorldTexture();                                   // the world's depth, readable; or null
// A stealthed unit's scratch depth (2026-10-04): bind a scratch the size of the world's depth instead of it,
// first copying the world's depth into it when copy is set. False: none could be made, nothing is bound.
bool               DepthScratchBegin(IDirect3DDevice9* dev, bool copy);
void               DepthScratchEnd(IDirect3DDevice9* dev);                // the world's depth bound again
void               DepthReset(IDirect3DDevice9* dev);                     // before Reset
void               DepthProbe();                                          // log the next frame's state
