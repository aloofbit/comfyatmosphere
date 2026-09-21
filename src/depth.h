// A readable depth buffer (INTZ) swapped in for the client's. See depth.cpp.
#pragma once

#include <d3d9.h>

void               DepthBeginScene(IDirect3DDevice9* dev);                // swap in at the start of a frame
IDirect3DSurface9* DepthSubstitute(IDirect3DDevice9* dev, IDirect3DSurface9* clientSurface);  // for SetDepthStencilSurface
void               DepthWorldEnded(IDirect3DDevice9* dev);                // remember which one holds the world
IDirect3DTexture9* DepthWorldTexture();                                   // the world's depth, readable; or null
void               DepthReset(IDirect3DDevice9* dev);                     // before Reset
void               DepthProbe();                                          // log the next frame's state
