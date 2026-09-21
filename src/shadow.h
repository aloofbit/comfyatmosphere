// A sun shadow map, made by replaying the frame's opaque world draws from the sun. See shadow.cpp.
#pragma once

#include <d3d9.h>

void RecordConstants(UINT reg, const float* data, UINT count);          // every client upload in the world
void RecordDraw(IDirect3DDevice9* dev, bool indexed, D3DPRIMITIVETYPE prim, INT baseVertex, UINT minIndex,
                UINT numVertices, UINT startIndex, UINT primCount);     // before a client draw is forwarded
void ShadowWorldEnded(IDirect3DDevice9* dev);                           // replay into the shadow map
void ShadowFrameEnd();                                                  // at Present: drop anything unreplayed
void ShadowSetPhase(bool recording);                                    // the world phase begins / ends
IDirect3DTexture9* ShadowTexture();                                     // the sun's depth, or null
bool ShadowMatrix(D3DMATRIX& camRelToShadowClip);                       // camera-relative world -> shadow clip
bool ShadowWorldCamera(D3DMATRIX& view, D3DMATRIX& proj);               // the camera the world's depth was drawn with
void ShadowWorldDepthRange(float& minZ, float& maxZ);                   // ...and the viewport depth range it used
void ShadowReset();                                                     // before Reset
void ShadowProbe();                                                     // log the next replay
