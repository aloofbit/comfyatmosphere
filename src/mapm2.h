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
    bool                  alpha = false;   // alpha-keyed batches a quarter of its triangles or more: the whole model
                                           // casts as leaves (a tree); else those batches alone do (2026-10-04)
    bool                  animated = false;   // a vertex on a bone with 16 or more keyframes: it moves (a gryphon roost)
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

// A model's bones and their animations (2026-10-04), for the object table to turn a doodad's parts while the
// client does not draw them (shadow.cpp, AnimateObjects). Version 256 and 257 only.
struct M2Anim
{
    struct Track
    {
        int16_t interp = 0, globalSeq = -1;
        std::vector<uint32_t> ranges;   // first and last key of each sequence, two words a sequence
        std::vector<uint32_t> times;    // ms, across all the sequences
        std::vector<float>    values;   // 3 floats a key (4 for a rotation: x y z w); tangents dropped
    };
    struct Bone
    {
        int      parent = -1;
        uint32_t flags = 0;
        float    pivot[3] = {};
        Track    t, r, s;
        bool     moves = false;   // keys that change inside a sequence, or a parent that moves
    };
    struct Sequence { uint16_t id = 0; uint32_t start = 0, end = 0; int16_t freq = 0; };
    struct Sub { uint32_t tris = 0; std::vector<uint16_t> bones; };   // a submesh: triangles, and its palette
    struct View { uint32_t verts = 0; std::vector<Sub> subs; };
    std::vector<Bone>     bones;
    std::vector<Sequence> seqs;
    std::vector<View>     views;
    float                 size = 1.0f;   // the farthest pivot from the origin, at least 1: the error's scale
    bool                  moves = false;
};

// From a model file's bytes; false if it is not a model of this version.
bool M2AnimRead(const std::vector<uint8_t>& d, M2Anim& out);
// Bone `bone`'s matrix in the model's space for sequence `seq` at `ms` (absolute, as the file's times are):
// rows of a 3x4, the parents' included. A track on a global sequence is read at its first key.
void M2AnimBone(const M2Anim& m, int bone, int seq, float ms, float out[12]);

struct BlpData
{
    int      format = 0;   // 0 DXT1, 1 DXT3, 2 DXT5, 3 A8R8G8B8
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> levels;   // mip 0 first, ready to copy into a D3D texture
};

bool BlpLoad(const std::string& name, BlpData& out);
