// mapwmo: a building (WMO) read from the client's archives, for the shadow map. See mapwmo.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct WmoMesh
{
    std::vector<float>    v;     // x y z in the building's own space
    std::vector<uint32_t> idx;
    unsigned groups = 0, groupsRead = 0;   // group files named by the root, and those read
    unsigned opaque = 0, other = 0;        // triangles kept (opaque) and left out (alpha keyed, blended)
    float    lo[3] = {}, hi[3] = {};       // the root's box (MOHD), in the building's own space
    std::vector<float> indoor;             // the indoor groups' boxes (MOGP flag 0x2000), 6 floats each, own space
};

// The root file and its groups into one mesh of the opaque triangles. Loader thread only (mpq.cpp).
bool WmoLoad(const std::string& rootName, WmoMesh& out);
