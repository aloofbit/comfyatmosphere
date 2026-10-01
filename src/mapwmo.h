// mapwmo: a building (WMO) read from the client's archives, for the shadow map. See mapwmo.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A doodad of the building that gives light (a candle, a torch, a fireplace), in the building's own space.
struct WmoLight
{
    float    pos[3];     // the flame: near the top of the model
    float    reach;      // yards
    float    colour[3];  // 0..1
    uint16_t set;        // the doodad set it belongs to (0 is shown with every placement)
    char     what[12];   // the keyword it was taken by, for the probe
    bool     fill;       // the building's light under a lamp, for the floor: lights surfaces, draws no glow
};

// A building's floors, for the fog's ground under the terrain (MapFloorHeight): its opaque triangles that lie
// within 60 degrees of flat, in a grid of kCell-yard cells over its own x and y.
struct WmoFloors
{
    static constexpr float kCell = 4.0f;
    std::vector<float>    tris;          // 9 floats each, own space
    std::vector<uint32_t> first, list;   // cell c holds the triangles list[first[c]] .. list[first[c + 1] - 1]
    float                 lo[2] = {};    // the grid's corner, own space
    int                   nx = 0, ny = 0;
};

struct WmoMesh
{
    std::vector<float>    v;     // x y z in the building's own space
    std::vector<uint32_t> idx;
    unsigned groups = 0, groupsRead = 0;   // group files named by the root, and those read
    unsigned opaque = 0, other = 0;        // triangles kept (opaque) and left out (alpha keyed, blended)
    float    lo[3] = {}, hi[3] = {};       // the root's box (MOHD), in the building's own space
    std::vector<float> indoor;             // the indoor groups' boxes (MOGP flag 0x2000), 6 floats each, own space
    std::vector<std::vector<float>> indoorTris;   // each indoor group's triangles, 9 floats each, own space:
                                                  // for the ceiling over a point (MapIndoors)
    std::vector<WmoLight> lights;          // its candles, torches and fires (MODD, by name)
    WmoFloors             floors;
};

struct M2Model;
// A doodad model that gives light, by its file name (upper case, without the folder) or, failing that, by the
// flames it burns (m may be null when the model could not be read): the word it was taken by (FLAME for a flame
// alone), and how far its light reaches. Null for any other model, and for an unlit candle or a broken
// lamppost. The buildings' doodads and the map tiles' own doodads (mapterrain.cpp) both go through it.
const char* LightModelWord(const std::string& file, const M2Model* m, float& reach);
constexpr int kMaxFlames = 6;   // lights one model may give
// Where a light model's flames are, in its own space, and their colour: from what M2Load read. Flames within
// 3 yards of each other are one light (a chandelier's candles); a tower with a fire at each corner gives one at
// each. With no flame in the file, one light in orange near the model's top. Returns how many.
int LightFlames(const M2Model* m, bool byWord, float pos[][3], float colour[3]);

// The root file and its groups into one mesh of the opaque triangles. Loader thread only (mpq.cpp).
bool WmoLoad(const std::string& rootName, WmoMesh& out);
