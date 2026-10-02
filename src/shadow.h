// A sun shadow map, made by replaying the frame's opaque world draws from the sun. See shadow.cpp.
#pragma once

#include <d3d9.h>

#include <string>

void RecordConstants(UINT reg, const float* data, UINT count);          // every client upload in the world
void RecordDraw(IDirect3DDevice9* dev, bool indexed, D3DPRIMITIVETYPE prim, INT baseVertex, UINT minIndex,
                UINT numVertices, UINT startIndex, UINT primCount);     // before a client draw is forwarded
void ShadowWorldEnded(IDirect3DDevice9* dev);                           // replay into the shadow map
void ShadowFrameEnd();                                                  // at Present: drop anything unreplayed
// The world phase begins / ends. votesOnly: no map, only the world camera and depth slice (the fog alone).
void ShadowSetPhase(bool recording, bool votesOnly = false);
void ShadowVotesEnded();                                                // the end of a votesOnly world phase
IDirect3DTexture9* ShadowTexture();                                     // the sun's depth, or null
bool ShadowMatrix(D3DMATRIX& camRelToShadowClip);                       // camera-relative world -> shadow clip
// The near map ([shadow] nearRange either side of the player), for the sun shadows; false if there is none.
bool ShadowNear(IDirect3DTexture9*& tex, D3DMATRIX& camRelToShadowClip, float& range);
// The middle map ([shadow] midRange either side), solid only; false if there is none.
bool ShadowMid(IDirect3DTexture9*& tex, D3DMATRIX& camRelToShadowClip, float& range);
IDirect3DTexture9* ShadowNearLeaves();   // the near map's leaves (alpha-tested draws), same camera; or null
IDirect3DTexture9* ShadowFarLeaves();    // the far map's leaves, same camera as the far map; or null
IDirect3DTexture9* ShadowFarTerrain();   // hills and mountains alone, same camera as the far map; or null
IDirect3DTexture9* ShadowNearUnits();    // players and creatures alone, the near map's camera and size; or null
bool ShadowIsUnitDraw(IDirect3DDevice9* dev);   // the draw about to be made is a model the cache has at a unit
// This frame's replay, for the volume trace: how it ended (0 = drawn) and how many entries it drew.
// counts: refreshed, added, evicted in view, aged out, over the cap. newInfo: the first new entries.
void ShadowWorldCameraPlanes(float& nearZ, float& farZ);   // the camera the replay is using
// A buffer the client has written to since it was created: its contents are not ours to keep.
void ShadowNoteBufferWrite(const void* buffer, UINT offset, UINT size);
const char* ShadowMapCentre();   // where the map sits this frame, and the sun it uses
const char* ShadowFrameInfo();   // the frame's first M2 entry: its absolute transform and the view
const char* ShadowOverwritten(unsigned& count);   // entries the client overwrote under us
const char* ShadowNearChanges();                  // the trace: what was added and dropped near you, and why
// The on-screen stats (/atmos stats): the cache's figures over the time since the last call, as name=value;.
void ShadowStatsText(std::string& out);
extern UINT g_maxConstReg;   // how many shader registers the client uses, so the replay sends no more
unsigned ShadowCopies(unsigned& failed);   // chunks copied out of the client's arena, and failures
void ShadowNoReplay();   // the map was not built this frame
double ShadowReplaySeconds(unsigned& drawn, unsigned& skipped);   // what this frame's replay cost
// For the benchmark: time the recording, the cache upkeep and the replay (CPU seconds), and count the
// casters drawn and the entries held, summed over the frames since the last take.
void ShadowTiming(bool on);
// ...and within the cache upkeep: Merge's share, and how many entries were seen again and how many of
// those had not moved, since the last take.
void ShadowTakeCacheSplit(double& merge, unsigned& still, unsigned& refreshed);
void ShadowTakeTimes(double& record, double& cache, double& replay, unsigned& drawn, unsigned& entries,
                     unsigned& frames, unsigned& replays);
const char* ShadowDropped();   // what the world filter threw away this frame
unsigned ShadowOffWorld();   // records dropped this frame: not drawn with the world's camera and slice
const char* ShadowChanges(unsigned& changed);   // refreshed entries whose inputs changed, and the biggest
const char* ShadowInherited();   // the geometry states the client left at this frame's replay
void ShadowLastReplay(int& outcome, unsigned& drawn, unsigned& entries, unsigned counts[5], const char*& newInfo);
bool ShadowWorldCamera(D3DMATRIX& view, D3DMATRIX& proj);
float ShadowMapDepth();   // yards the shadow map runs toward the sun and away from it: its z range is twice this               // the camera the world's depth was drawn with
void ShadowWorldDepthRange(float& minZ, float& maxZ);                   // ...and the viewport depth range it used
void ShadowReset();                                                     // before Reset
void ShadowProbe();                                                     // log the next replay
