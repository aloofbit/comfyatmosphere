// mapterrain: the ground and the buildings read from the client's map files, for the shadow map. See
// mapterrain.cpp.
#pragma once

#include <d3d9.h>

// Once per map redraw, on the render thread: ask for the tiles within `reach` yards of `player` (across
// the ground), take in those the loader has finished, and drop those left far behind.
void MapTerrainUpdate(IDirect3DDevice9* dev, const float player[3], float reach, float fullReach);
// Whether the ground at this point comes from the files: the client's own draw of it is then not needed.
bool MapTerrainCovers(float x, float y);
// Draw every tile that can reach the map into the bound depth target. absToClip: absolute world -> the
// map's clip space; cam: the camera, which the world matrix is relative to. Returns the tiles drawn.
unsigned MapTerrainDraw(IDirect3DDevice9* dev, const D3DMATRIX& absToClip, const float cam[3]);
// The buildings (WMOs) the tiles place, the same way; their opaque parts only.
unsigned MapBuildingsDraw(IDirect3DDevice9* dev, const D3DMATRIX& absToClip, const float cam[3]);
// Whether a fixed-function draw placed here (its world matrix's translation) is a building drawn from the
// files: the client's own draw of it is then not needed.
bool MapBuildingCovers(const float pos[3]);
// The building from the files nearest to `from`, for the probe: its place, its turn (row vectors) and name.
bool MapBuildingNearest(const float from[3], float pos[3], float rot[3][3], char* name, int size);
// The doodads (trees, bushes, fences, rocks) the tiles place: the solid models, or the models with leaves
// (cut by their textures with this alpha test).
unsigned MapDoodadsDraw(IDirect3DDevice9* dev, const D3DMATRIX& absToClip, const float cam[3], bool leaves,
                        DWORD alphaRef, DWORD alphaFunc);
// Whether a model draw whose absolute transform sits here is a doodad drawn from the files: a doodad's place
// within tol yards on each axis.
bool MapDoodadCovers(const float pos[3], float tol = 0.5f);
bool MapAnimatedDoodadAt(const float pos[3], float tol);   // an animated doodad the files place there, left out
unsigned MapFilesVersion();   // changes whenever what the files cover changes
bool MapDoodadNearest(const float from[3], float pos[3]);   // for the probe
// The probe: the doodads from the files within radius yards, with their model, and whether the client drew them.
void MapLogDoodadsNear(const float from[3], float radius);
// A light from a building's doodads (a candle, a torch, a fireplace), in the world.
struct MapLight
{
    float pos[3];
    float reach;
    float colour[3];
    char  what[12];
};
// The lights within radius yards of `at`, up to max; and how many are held.
int MapLightsNear(const float at[3], float radius, MapLight* out, int max);
unsigned MapLightCount();
// Whether a point is inside one of the indoor groups (rooms, cellars) of a building from the files.
bool MapIndoors(const float p[3]);
// The ground's height from the tiles loaded, at a point; false where no tile is held.
bool MapGroundHeight(float x, float y, float& z);
// The surface of a river or the sea at a point (the map files' MCLQ); false where dry or no tile is held.
bool MapWaterHeight(float x, float y, float& z);
// The average ground height over a disc of `radius` yards around `at` (37 points); false with too few.
bool MapGroundBase(const float at[3], float radius, float& z);
void MapTerrainRelease();   // a new device: the GPU copies go, the meshes stay
const char* MapTerrainInfo();   // for the probe
