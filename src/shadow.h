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
// This frame's replay, for the volume trace: how it ended (0 = drawn) and how many entries it drew.
// counts: refreshed, added, evicted in view, aged out, over the cap. newInfo: the first new entries.
void ShadowWorldCameraPlanes(float& nearZ, float& farZ);   // the camera the replay is using
// A buffer the client has written to since it was created: its contents are not ours to keep.
void ShadowNoteBufferWrite(const void* buffer, UINT offset, UINT size);
const char* ShadowBufferCheck(unsigned& checked, unsigned& changed);   // do cached buffers still hold
                                                                       // what they held? (trace only)
const char* ShadowMapCentre();   // where the map sits this frame, and the sun it uses
const char* ShadowFrameInfo();   // the frame's first M2 entry: its absolute transform and the view
const char* ShadowOverwritten(unsigned& count);   // entries the client overwrote under us
extern UINT g_maxConstReg;   // how many shader registers the client uses, so the replay sends no more
unsigned ShadowCopies(unsigned& failed);   // chunks copied out of the client's arena, and failures
void ShadowNoReplay();   // the map was not built this frame
double ShadowReplaySeconds(unsigned& drawn, unsigned& skipped);   // what this frame's replay cost
const char* ShadowDropped();   // what the world filter threw away this frame
unsigned ShadowOffWorld();   // records dropped this frame: not drawn with the world's camera and slice
const char* ShadowChanges(unsigned& changed);   // refreshed entries whose inputs changed, and the biggest
const char* ShadowInherited();   // the geometry states the client left at this frame's replay
void ShadowLastReplay(int& outcome, unsigned& drawn, unsigned& entries, unsigned counts[5], const char*& newInfo);
bool ShadowWorldCamera(D3DMATRIX& view, D3DMATRIX& proj);               // the camera the world's depth was drawn with
void ShadowWorldDepthRange(float& minZ, float& maxZ);                   // ...and the viewport depth range it used
void ShadowReset();                                                     // before Reset
void ShadowProbe();                                                     // log the next replay
