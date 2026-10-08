// mapterrain: the ground and the buildings read from the client's map files, for the shadow map. See
// mapterrain.cpp.
#pragma once

#include <d3d9.h>

#include <cstdint>
#include <string>
#include <vector>

// Once per map redraw, on the render thread: ask for the tiles within `reach` yards of `player` (across
// the ground), take in those the loader has finished, and drop those left far behind.
void MapTerrainUpdate(IDirect3DDevice9* dev, const float player[3], float reach, float fullReach);
// Whether the ground at this point comes from the files: the client's own draw of it is then not needed.
bool MapTerrainCovers(float x, float y);
// Draw every tile that can reach the map into the bound depth target. absToClip: absolute world -> the
// map's clip space; cam: the camera, which the world matrix is relative to; low: the copy under the
// client's coarse far mesh ([shadow] terrainLow). Returns the tiles drawn.
unsigned MapTerrainDraw(IDirect3DDevice9* dev, const D3DMATRIX& absToClip, const float cam[3], bool low);
// The buildings (WMOs) the tiles place, the same way; their opaque parts only, and of those the groups that
// reach the map.
unsigned MapBuildingsDraw(IDirect3DDevice9* dev, const D3DMATRIX& absToClip, const float cam[3]);
// Whether a fixed-function draw placed here (its world matrix's translation) is a building drawn from the
// files: the client's own draw of it is then not needed.
bool MapBuildingCovers(const float pos[3]);
// The building from the files nearest to `from`, for the probe: its place, its turn (row vectors) and name.
bool MapBuildingNearest(const float from[3], float pos[3], float rot[3][3], char* name, int size);
// The doodads (trees, bushes, fences, rocks) the tiles place: the solid models, or the models with leaves
// (cut by their textures with this alpha test). Each tile's blocks of 66.7 yards that reach the map.
unsigned MapDoodadsDraw(IDirect3DDevice9* dev, const D3DMATRIX& absToClip, const float cam[3], bool leaves,
                        DWORD alphaRef, DWORD alphaFunc);
// Whether a model draw whose absolute transform sits here is a doodad drawn from the files: a doodad's place
// within tol yards on each axis.
bool MapDoodadCovers(const float pos[3], float tol = 0.5f);
// The same test for the probe, without marking the doodad as drawn by the client.
bool MapDoodadAt(const float pos[3], float tol = 0.5f);
// The probe: one line for each tile held, with its doodads (2026-10-03).
void MapLogTiles(const float from[3]);
bool MapAnimatedDoodadAt(const float pos[3], float tol);
// The animated doodads the files place within `radius` yards of `at` (across the ground): their places, up to
// `max`; the count (2026-10-04, the shadow cache's object table). With `nameHas` (upper case), only the ones whose
// model's full name holds it, so the others do not fill `max` first.
int MapAnimatedDoodads(const float at[3], float radius, float (*out)[3], int max, std::string* names = nullptr,
                       const char* nameHas = nullptr);
// The vertex counts of a model file's views (2026-10-04): a draw of the model holds one view's vertices, or a
// whole number of copies of them (a batch). Null while the loader reads it, or if it could not be read.
const std::vector<uint32_t>* MapModelViews(const std::string& name);
// The same model's bones and animations, read with its views; null until then, or if none of its bones moves.
const struct M2Anim* MapModelAnim(const std::string& name);
// A game object's model file, by its display (GameObjectDisplayInfo.dbc), once the loader has read it; else null.
const std::string* MapGameObjectModel(unsigned display);   // an animated doodad the files place there, left out
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
    bool  fill;   // lights surfaces and draws no glow (WmoLight::fill)
};
// The lights within radius yards of `at`, up to max; and how many are held.
int MapLightsNear(const float at[3], float radius, MapLight* out, int max);
unsigned MapLightCount();
// The probe: every game object within radius yards, with its display id, model and the light it gives.
void MapObjectsLog(const float at[3], float radius);
// Whether a point is inside one of the indoor groups (rooms, cellars) of a building from the files. why, if
// given, gets the step that decided, for the stats panel: the building's file, the room, the ceiling's height.
bool MapIndoors(const float p[3], char* why = nullptr, size_t whySize = 0);
// The highest floor of a building from the files at (x, y) that is not above `below`: for the fog's ground
// under the terrain (Ironforge, the Undercity, a mine). False where no building has one.
bool MapFloorHeight(float x, float y, float below, float& z);
// The ground's height from the tiles loaded, at a point; false where no tile is held.
bool MapGroundHeight(float x, float y, float& z);
// The surface of a river or the sea at a point (the map files' MCLQ); false where dry or no tile is held.
bool MapWaterHeight(float x, float y, float& z);
// Whether the water at a point is the sea (the map's MCNK flag 0x8), not a river, a lake or a pond (0x4). False
// where dry or no tile is held.
bool MapWaterIsSea(float x, float y);
// The lighthouses within radius yards (2026-10-05): each building with LIGHTHOUSE in its file name, its lamp in the
// middle of its highest group's box, just under that group (the roof over the lamp room), in the world. Nearest
// first; returns how many. roof, if given, gets each one's highest group's box in the world's height: lo, hi.
int MapLighthouses(const float at[3], float radius, float (*out)[3], int max, float (*roof)[2] = nullptr);
// A point in the nearest lighthouse's own space, and that building's name, for the probe: to place a lamp by hand.
bool MapLighthouseOwn(const float world[3], float own[3], char* name, int size);
// The probe: the nearest lighthouse's groups' boxes and its indoor groups' boxes, in its own space.
void MapLighthouseGroupsLog(const float world[3]);
// The average ground height over a disc of `radius` yards around `at` (37 points), the water's surface where water
// lies over the ground; false with too few.
bool MapGroundBase(const float at[3], float radius, float& z);
void MapTerrainRelease();   // a new device: the GPU copies go, the meshes stay
struct BlpData;
// A texture by name, read on the loader thread: 0 while it loads (or the loader has not started), 1 with the
// texture in out (once), -1 if it could not be read.
int MapRequestTexture(const char* name, BlpData& out);
// The vertex counts of the views of a creature display's model (2026-10-04): CreatureDisplayInfo.dbc gives the
// model's row, CreatureModelData.dbc its file, and the M2 its views. The client draws a unit's model from a
// vertex buffer holding one view's vertices. Null while the loader reads it, or if it could not be read;
// the first call asks for it.
const std::vector<uint32_t>* MapCreatureModelViews(unsigned display);
const char* MapTerrainInfo();   // for the probe
