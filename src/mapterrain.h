// mapterrain: the ground read from the client's map files, for the shadow map. See mapterrain.cpp.
#pragma once

#include <d3d9.h>

// Once per map redraw, on the render thread: ask for the tiles within `reach` yards of `player` (across
// the ground), take in those the loader has finished, and drop those left far behind.
void MapTerrainUpdate(IDirect3DDevice9* dev, const float player[3], float reach);
// Whether the ground at this point comes from the files: the client's own draw of it is then not needed.
bool MapTerrainCovers(float x, float y);
// Draw every tile that can reach the map into the bound depth target. absToClip: absolute world -> the
// map's clip space; cam: the camera, which the world matrix is relative to. Returns the tiles drawn.
unsigned MapTerrainDraw(IDirect3DDevice9* dev, const D3DMATRIX& absToClip, const float cam[3]);
void MapTerrainRelease();   // a new device: the GPU copies go, the meshes stay
const char* MapTerrainInfo();   // for the probe
