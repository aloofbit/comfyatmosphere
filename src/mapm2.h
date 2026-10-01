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
    bool                  animated = false;   // a bone with 16 or more keyframes: it moves (a gryphon roost)
    // The flame, for a model that gives light (mapwmo.cpp): where its glow quads and its particle emitters are,
    // in the model's own space, and their colour. flame 0 = the model has neither (an unlit candle, or a lamp
    // that glows by its texture alone); haveColour = a colour was read.
    int                   flame = 0;          // 0 none, 1 particle emitters, 2 glow quads
    bool                  haveColour = false;
    std::vector<float>    quadPts;            // each glow quad's centre, x y z
    std::vector<float>    emitPts;            // each additive emitter's place
    std::vector<float>    firePts;            // each additive emitter whose texture is a flame: a model with any
                                              // burns, whatever its name (a pyre, a campfire's wood pile)
    float                 flameColour[3] = {}; // brightest channel 1
    float                 middle[3] = {};      // the median vertex: the body of the model, whatever hangs off it
    float                 top[3] = {};         // the box's centre across, 85% of the way up
    float                 height = 0.0f;       // the box's height
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
