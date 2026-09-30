// mapm2: doodad models (M2) and their textures (BLP) read from the client's archives, for the shadow map.
// See mapm2.cpp. Loader thread only (mpq.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct M2Model
{
    struct Batch
    {
        uint32_t    start, count;   // into tris
        int         blend;          // 0 opaque, 1 alpha keyed, 2 and up blended
        std::string tex;            // the texture file, or empty
    };
    std::vector<float>    pos;      // x y z, one per view-local vertex
    std::vector<float>    uv;       // u v
    std::vector<uint16_t> tris;     // view-local corners
    std::vector<Batch>    batches;
    bool                  alpha = false;   // any batch alpha keyed: the whole model casts as leaves
};

// The model named as a tile names it (.mdx or .m2); false if it cannot be read.
bool M2Load(const std::string& name, M2Model& out);

struct BlpData
{
    int      format = 0;   // 0 DXT1, 1 DXT3, 2 DXT5, 3 A8R8G8B8
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> levels;   // mip 0 first, ready to copy into a D3D texture
};

bool BlpLoad(const std::string& name, BlpData& out);
