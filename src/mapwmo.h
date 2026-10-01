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
};

// The root file and its groups into one mesh of the opaque triangles. Loader thread only (mpq.cpp).
bool WmoLoad(const std::string& rootName, WmoMesh& out);
