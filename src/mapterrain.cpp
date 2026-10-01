// mapterrain: the ground and the buildings read from the client's map files, for the shadow map.
//
// The shadow cache (shadow.cpp) holds what the client drew, and the client draws a terrain chunk past
// about 250 yards with a coarse mesh: 41 vertices and 64 triangles in place of 145 and 256. A mountain
// seen only from afar then cast its shadow from that mesh, a plain triangle where the ridge should be
// (2026-09-29). The files hold every chunk at full detail wherever you stand, so the ground within the
// map's reach is built from them instead, and the client's own terrain draws are left out wherever a
// tile from the files covers the ground. Past the tiles loaded, and on a map without tiles (a dungeon is
// buildings only), the client's draws still cast as before.
//
//   Files      World\Maps\<map>\<map>_<a>_<b>.adt, one tile of 533.33 yards a side, 16 x 16 chunks. The
//              map's name is the client's own, read at 0x00C961A0 ([client] mapNameAddr): the buffer the
//              client formats the tile names with ("%s\%s_%d_%d.adt", found by disassembly).
//              a = floor(32 - y / 533.33), b = floor(32 - x / 533.33).
//   Chunks     Each MCNK holds its corner (the largest x and y it covers, and a base height) and 145
//              heights in MCVT: 9 x 9 outer vertices with 8 x 8 inner ones between them, rows along -x,
//              columns along -y, 4.17 yards apart. Each cell is 4 triangles meeting at its inner vertex.
//              Checked against the Northshire tile: neighbouring chunks meet with no gap (the other axis
//              order is out by 18 yards on average), and the height under a logged player position was
//              81.51 against the player's 81.5.
//   Water      (added 2026-09-30) A chunk with a river or the sea (MCNK flags 0x4, 0x8) holds an MCLQ at the
//              header's +0x60: its tag, a size of 0 (the header's +0x64 gives 812), then a height range
//              (two floats), 9 x 9 vertices of 8 bytes and 8 x 8 cell flags. A cell is wet unless its flags'
//              low four bits are 0x0F. The vertex heights of dry cells are FLT_MAX, so the surface is taken
//              as the range's top: 0 for the sea, 30.88 for a stream in Westfall (checked offline, 34 tiles).
//              Kept as 128 x 128 cells a tile, the cells of the chunks, for the fog's mist over water.
//   Holes      A 4 x 4 mask over the 8 x 8 cells, where a cave mouth or a building's cellar goes into the
//              ground: bit (row / 2) * 4 + (column / 2). A cell in a hole is left out.
//   Buildings  (added 2026-09-30) Each tile places its WMOs in MODF, 64 bytes each: the name's index
//              (through MWID into MWMO), a unique id, a position, a rotation in degrees, and a box. A
//              building crossing tiles is placed by each of them under the same id, and drawn once. The
//              placement is the core's vmap maths (tools/vmap_extractor, src/game/vmap/ModelInstance.cpp):
//              world = (17066.67 - p.z, 17066.67 - p.x, p.y), turned by Rz(r.y) Ry(r.x) Rx(r.z) and then a
//              half turn about the vertical. mapwmo.cpp reads the building; the client's own draw of it
//              (fixed-function, its place the placement's) is then left out, except its alpha-keyed parts.
//   Doodads    (added 2026-09-30) Trees, bushes, fences and rocks: MDDF, 36 bytes each (the name's index
//              through MMID into MMDX, a unique id, position, rotation, scale as a number over 1024), placed
//              with the same maths as a building, times the scale. mapm2.cpp reads the models. A doodad is
//              built into the tile that holds its position, so one on a tile's edge is built once. Each
//              tile's doodads are built on the loader thread into two buffers, world space: the solid
//              models, and the models with an alpha-keyed part (trees, bushes: leaves, the trunk with
//              them), grouped by texture, so a tile is a handful of draws where the cache made one for
//              each model. Around Northshire: 9,180 doodads from 383 models, 1.6 million triangles.
//   Loading    On a thread of its own, nearest tile first, so no frame waits on a file: about 2 MB of
//              zlib a tile. The terrain meshes stay in memory while their tile is in reach (0.8 MB each);
//              a building's mesh goes once it is on the GPU. Each goes to the GPU as one vertex and one
//              index buffer in the managed pool, which lives through a device Reset.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "common.h"
#include "mapterrain.h"
#include "mapm2.h"
#include "mapwmo.h"
#include "mpq.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr float kTile  = 1600.0f / 3.0f;     // 533.33 yards
    constexpr float kChunk = kTile / 16.0f;
    constexpr float kUnit  = kChunk / 8.0f;
    constexpr float kMid   = 32.0f * kTile;      // 17066.67: the world's centre in placement coordinates

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    // A building placed by a tile. World (row vector) = model * rot + pos.
    struct Placement
    {
        uint32_t    uid = 0;
        uint16_t    doodadSet = 0;   // which of the building's doodad sets this placement shows
        std::string name;        // upper case: tiles spell the same file alike, but not by rule
        float       rot[3][3] = {};
        float       pos[3] = {};
        float       lo[3] = {}, hi[3] = {};   // its box in the world
    };

    struct Mesh
    {
        int                    a = 0, b = 0;
        unsigned               gen = 0;
        bool                   found = false;   // the file exists and was read
        bool                   groundOnly = false;   // read for its ground alone: no doodads, no buildings
        std::vector<float>     v;               // x y z, x and y relative to the tile's corner
        std::vector<float>     grid;            // 129 x 129 outer heights, rows along -x (NaN: no chunk)
        std::vector<float>     water;           // 128 x 128 cells: the water's surface (NaN: dry)
        std::vector<uint16_t>  idx;
        float                  minZ = 0.0f, maxZ = 0.0f;
        std::vector<Placement> wmos;
        double                 ms = 0.0;        // time to read and build

        // The doodads, world space relative to the tile's corner. Leaf batches: a texture to cut the
        // shape by, or none (a tree's trunk, which casts with its leaves).
        struct Batch { std::string tex; uint32_t start, count; };
        std::vector<float>     dSolid;          // x y z
        std::vector<uint32_t>  dSolidIdx;
        std::vector<float>     dLeaf;           // x y z u v
        std::vector<uint32_t>  dLeafIdx;
        std::vector<Batch>     dBatches;
        float                  dMinZ = 0.0f, dMaxZ = 0.0f;
        std::vector<float>     dPos;            // each doodad's place, x y z: to know the client's own draws
        std::vector<float>     dAnim;           // the places of the animated ones, left out: the client's
        std::vector<std::string> dAnimName;     // draws of them cast instead (a gryphon roost)
        std::vector<MapLight>  dLights;         // the doodads that give light, in the world (LightModelWord)
        std::vector<std::string> dName;         // ...its model and scale, for the probe
        std::vector<float>     dScale;
        std::string            archives;        // the archives that hold the tile, the one read first
        unsigned               doodads = 0, doodadsMissing = 0;
        double                 dMs = 0.0;
    };

    struct Tile
    {
        bool                    pending = true;
        bool                    upgrading = false;   // held for its ground, being read again in full
        Mesh                    mesh;
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9*  ib = nullptr;
        IDirect3DVertexBuffer9* dSolidVb = nullptr;   // the doodads
        IDirect3DIndexBuffer9*  dSolidIb = nullptr;
        IDirect3DVertexBuffer9* dLeafVb = nullptr;
        IDirect3DIndexBuffer9*  dLeafIb = nullptr;
        bool                    dOnGpu = false;
    };

    // A leaf texture, shared by every tile that cuts leaves with it.
    struct Tex
    {
        enum State { kWant, kAsked, kLoaded, kReady, kFailed } state = kWant;
        BlpData             data;
        IDirect3DTexture9*  tex = nullptr;
        bool                used = true;
    };

    // A building's model, shared by every placement of it.
    struct Model
    {
        enum State { kWant, kAsked, kLoaded, kReady, kFailed } state = kWant;
        WmoMesh                 mesh;
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9*  ib = nullptr;
        UINT                    nv = 0, ntri = 0;
        float                   lo[3] = {}, hi[3] = {};   // the root's box, own space: kept when the mesh goes
        std::vector<float>      indoor;                    // its indoor groups' boxes, own space: kept too
        std::vector<std::vector<float>> indoorTris;        // and their triangles, for the ceiling test
        std::vector<WmoLight>   lights;                    // its lights, own space: kept too
        bool                    used = true;
    };

    // The corner of tile (a, b): the largest x and y it covers.
    float CornerX(int b) { return (32.0f - b) * kTile; }
    float CornerY(int a) { return (32.0f - a) * kTile; }

    uint32_t U32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; }

    // Rz(a) Ry(b) Rx(c), column vectors: G3D's fromEulerAnglesZYX, which the core's vmap code uses.
    void EulerZYX(float a, float b, float c, float r[3][3])
    {
        const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
        const float z[3][3] = { { ca, -sa, 0 }, { sa, ca, 0 }, { 0, 0, 1 } };
        const float y[3][3] = { { cb, 0, sb }, { 0, 1, 0 }, { -sb, 0, cb } };
        const float x[3][3] = { { 1, 0, 0 }, { 0, cc, -sc }, { 0, sc, cc } };
        float yx[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                yx[i][j] = y[i][0] * x[0][j] + y[i][1] * x[1][j] + y[i][2] * x[2][j];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i][j] = z[i][0] * yx[0][j] + z[i][1] * yx[1][j] + z[i][2] * yx[2][j];
    }

    // The tile's MODF into placements.
    void Buildings(const std::vector<uint8_t>& d, size_t mwmo, size_t mwmoSize, size_t mwid, size_t mwidSize,
                   size_t modf, size_t modfSize, std::vector<Placement>& out)
    {
        const float deg = 3.14159265f / 180.0f;
        for (size_t o = modf; o + 64 <= modf + modfSize; o += 64)
        {
            const uint32_t nameId = U32(d, o);
            if (static_cast<size_t>(nameId) * 4 + 4 > mwidSize)
                continue;
            const uint32_t nameOff = U32(d, mwid + nameId * 4);
            if (nameOff >= mwmoSize)
                continue;
            Placement p;
            p.uid = U32(d, o + 4);
            for (size_t k = mwmo + nameOff; k < mwmo + mwmoSize && d[k]; ++k)
                p.name += static_cast<char>(d[k] >= 'a' && d[k] <= 'z' ? d[k] - 32 : d[k]);
            float raw[3], rotDeg[3], lo[3], hi[3];
            memcpy(raw, &d[o + 8], 12);
            memcpy(rotDeg, &d[o + 20], 12);
            memcpy(lo, &d[o + 32], 12);
            memcpy(hi, &d[o + 44], 12);
            float r[3][3];
            EulerZYX(rotDeg[1] * deg, rotDeg[0] * deg, rotDeg[2] * deg, r);
            // The half turn negates the world's x and y; the row-vector matrix is the transpose.
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    p.rot[i][j] = (j < 2 ? -1.0f : 1.0f) * r[j][i];
            p.pos[0] = kMid - raw[2];
            p.pos[1] = kMid - raw[0];
            p.pos[2] = raw[1];
            p.lo[0] = kMid - (std::max)(lo[2], hi[2]); p.hi[0] = kMid - (std::min)(lo[2], hi[2]);
            p.lo[1] = kMid - (std::max)(lo[0], hi[0]); p.hi[1] = kMid - (std::min)(lo[0], hi[0]);
            p.lo[2] = (std::min)(lo[1], hi[1]);        p.hi[2] = (std::max)(lo[1], hi[1]);
            memcpy(&p.doodadSet, &d[o + 58], 2);
            if (!p.name.empty())
                out.push_back(std::move(p));
        }
    }

    // --- doodads, on the loader thread ---

    std::unordered_map<std::string, std::shared_ptr<M2Model>> g_m2;   // loader thread only
    unsigned g_m2Gen = 0;

    // The tile's MDDF into its two doodad buffers.
    void Doodads(const std::vector<uint8_t>& d, Mesh& m, unsigned gen)
    {
        if (gen != g_m2Gen)
        {
            g_m2.clear();   // another map
            g_m2Gen = gen;
        }
        size_t mmdx = 0, mmdxSize = 0, mmid = 0, mmidSize = 0, mddf = 0, mddfSize = 0;
        for (size_t o = 0; o + 8 <= d.size();)
        {
            const uint32_t tag = U32(d, o), size = U32(d, o + 4);
            if (o + 8 + static_cast<size_t>(size) > d.size())
                break;
            if (tag == 0x4D4D4458) { mmdx = o + 8; mmdxSize = size; }   // "MMDX"
            if (tag == 0x4D4D4944) { mmid = o + 8; mmidSize = size; }   // "MMID"
            if (tag == 0x4D444446) { mddf = o + 8; mddfSize = size; }   // "MDDF"
            o += 8 + static_cast<size_t>(size);
        }
        if (!mmdxSize || !mmidSize || !mddfSize)
            return;
        const double t0 = Now();
        const float x1 = CornerX(m.b), y1 = CornerY(m.a);
        const float deg = 3.14159265f / 180.0f;
        std::unordered_map<std::string, std::vector<uint32_t>> groups;   // leaf indices by texture
        m.dMinZ = 1e9f;
        m.dMaxZ = -1e9f;
        for (size_t o = mddf; o + 36 <= mddf + mddfSize; o += 36)
        {
            const uint32_t nameId = U32(d, o);
            if (static_cast<size_t>(nameId) * 4 + 4 > mmidSize)
                continue;
            const uint32_t nameOff = U32(d, mmid + nameId * 4);
            if (nameOff >= mmdxSize)
                continue;
            float raw[3], rotDeg[3];
            memcpy(raw, &d[o + 8], 12);
            memcpy(rotDeg, &d[o + 20], 12);
            uint16_t scale16;
            memcpy(&scale16, &d[o + 32], 2);
            const float pos[3] = { kMid - raw[2], kMid - raw[0], raw[1] };
            // Built by the tile that holds it: a doodad near an edge is listed by both tiles.
            if (!(pos[0] <= x1 && pos[0] > x1 - kTile && pos[1] <= y1 && pos[1] > y1 - kTile))
                continue;
            std::string name;
            for (size_t k = mmdx + nameOff; k < mmdx + mmdxSize && d[k]; ++k)
                name += static_cast<char>(d[k] >= 'a' && d[k] <= 'z' ? d[k] - 32 : d[k]);
            const size_t dot = name.find_last_of('.');
            if (dot != std::string::npos)
                name.erase(dot);   // X.MDX and X.M2 are one model
            auto it = g_m2.find(name);
            if (it == g_m2.end())
            {
                auto model = std::make_shared<M2Model>();
                if (!M2Load(name + ".m2", *model))
                    model.reset();
                it = g_m2.emplace(name, model).first;
            }
            // A doodad that gives light: a torch on a pole, a lamppost, a campfire in the open (2026-10-01).
            // Until then only the buildings' doodads were read, and a lamp outside them glowed only when the
            // client showed a light or a glow sprite for it. Taken before the model is known to be readable:
            // LightFlames has an answer either way. One light for each group of flames.
            float lightReach = 0.0f;
            if (const char* word = LightModelWord(name.substr(name.find_last_of('\\') + 1), it->second.get(), lightReach))
            {
                float r[3][3], fp[kMaxFlames][3], colour[3];
                EulerZYX(rotDeg[1] * deg, rotDeg[0] * deg, rotDeg[2] * deg, r);
                const float sc = scale16 / 1024.0f;
                const int nf = LightFlames(it->second.get(), strcmp(word, "FLAME") != 0, fp, colour);
                for (int f = 0; f < nf; ++f)
                {
                    MapLight L = {};
                    memcpy(L.colour, colour, sizeof(colour));
                    for (int j = 0; j < 3; ++j)
                        L.pos[j] = (j < 2 ? -1.0f : 1.0f) * sc *
                                   (fp[f][0] * r[j][0] + fp[f][1] * r[j][1] + fp[f][2] * r[j][2]) + pos[j];
                    L.reach = lightReach * (std::min)((std::max)(sc, 0.5f), 2.0f);
                    strncpy_s(L.what, word, _TRUNCATE);
                    m.dLights.push_back(L);
                }
            }
            if (!it->second)
            {
                ++m.doodadsMissing;
                continue;
            }
            const M2Model& md = *it->second;
            // An animated doodad is left to the client's draws, which show it as it moves: baked here it
            // cast in its resting pose, and the gryphons at a flight master stood still in their shade
            // while they moved in game (2026-09-30).
            if (md.animated)
            {
                m.dAnim.insert(m.dAnim.end(), pos, pos + 3);
                m.dAnimName.push_back(name.substr(name.find_last_of('\\') + 1));
                continue;
            }
            float r[3][3], rot[3][3];
            EulerZYX(rotDeg[1] * deg, rotDeg[0] * deg, rotDeg[2] * deg, r);
            const float sc = scale16 / 1024.0f;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    rot[i][j] = (j < 2 ? -1.0f : 1.0f) * r[j][i] * sc;
            const size_t nv = md.pos.size() / 3;
            std::vector<float>& vout = md.alpha ? m.dLeaf : m.dSolid;
            const uint32_t base = static_cast<uint32_t>(vout.size() / (md.alpha ? 5 : 3));
            for (size_t i = 0; i < nv; ++i)
            {
                const float* p = &md.pos[i * 3];
                float w[3];
                for (int j = 0; j < 3; ++j)
                    w[j] = p[0] * rot[0][j] + p[1] * rot[1][j] + p[2] * rot[2][j] + pos[j];
                vout.push_back(w[0] - x1);
                vout.push_back(w[1] - y1);
                vout.push_back(w[2]);
                if (md.alpha)
                {
                    vout.push_back(md.uv[i * 2]);
                    vout.push_back(md.uv[i * 2 + 1]);
                }
                m.dMinZ = (std::min)(m.dMinZ, w[2]);
                m.dMaxZ = (std::max)(m.dMaxZ, w[2]);
            }
            for (const M2Model::Batch& b : md.batches)
            {
                if (b.blend > 1)
                    continue;   // blended: the client's draw of it casts nothing either
                std::vector<uint32_t>& idx = md.alpha ? groups[b.blend == 1 ? b.tex : std::string()] : m.dSolidIdx;
                for (uint32_t k = b.start; k < b.start + b.count; ++k)
                    idx.push_back(base + md.tris[k]);
            }
            m.dPos.insert(m.dPos.end(), pos, pos + 3);
            m.dName.push_back(name.substr(name.find_last_of('\\') + 1));
            m.dScale.push_back(sc);
            ++m.doodads;
        }
        for (auto& g : groups)
        {
            m.dBatches.push_back({ g.first, static_cast<uint32_t>(m.dLeafIdx.size()), static_cast<uint32_t>(g.second.size()) });
            m.dLeafIdx.insert(m.dLeafIdx.end(), g.second.begin(), g.second.end());
        }
        m.dMs = 1000.0 * (Now() - t0);
    }

    // One tile's file into a mesh. False if the file is not a tile this reader understands.
    bool Build(const std::vector<uint8_t>& d, Mesh& m)
    {
        const float ox = CornerX(m.b), oy = CornerY(m.a);
        m.v.clear();
        m.idx.clear();
        m.wmos.clear();
        m.grid.assign(129 * 129, NAN);
        m.water.assign(128 * 128, NAN);
        m.minZ = 1e9f;
        m.maxZ = -1e9f;
        unsigned chunks = 0;
        size_t mwmo = 0, mwmoSize = 0, mwid = 0, mwidSize = 0, modf = 0, modfSize = 0;
        for (size_t o = 0; o + 8 <= d.size();)
        {
            const uint32_t tag = U32(d, o), size = U32(d, o + 4);
            if (o + 8 + static_cast<size_t>(size) > d.size())
                break;
            if (tag == 0x4D574D4F) { mwmo = o + 8; mwmoSize = size; }   // "MWMO"
            if (tag == 0x4D574944) { mwid = o + 8; mwidSize = size; }   // "MWID"
            if (tag == 0x4D4F4446) { modf = o + 8; modfSize = size; }   // "MODF"
            if (tag == 0x4D434E4B && size >= 0x80 && chunks < 256)   // "MCNK"
            {
                const size_t h = o + 8;
                const uint32_t ofsH = U32(d, h + 0x14);
                uint16_t holes;
                memcpy(&holes, &d[h + 0x3C], 2);
                float pos[3];
                memcpy(pos, &d[h + 0x68], 12);
                // A river or the sea: the wet cells, at the top of the liquid's height range.
                const uint32_t flags = U32(d, h), ofsL = U32(d, h + 0x60);
                const size_t lq = o + ofsL;
                if ((flags & 0x0C) && ofsL && lq + 8 + 8 + 648 + 64 <= o + 8 + size && U32(d, lq) == 0x4D434C51 &&
                    std::isfinite(pos[0]) && std::isfinite(pos[1]))   // "MCLQ"
                {
                    float range[2];
                    memcpy(range, &d[lq + 8], 8);
                    if (std::isfinite(range[1]) && fabsf(range[1]) < 10000.0f)
                    {
                        const int gx0 = static_cast<int>(floorf((ox - pos[0]) / kUnit + 0.5f));
                        const int gy0 = static_cast<int>(floorf((oy - pos[1]) / kUnit + 0.5f));
                        for (int r = 0; r < 8; ++r)
                            for (int c = 0; c < 8; ++c)
                            {
                                const int gx = gx0 + r, gy = gy0 + c;
                                if ((d[lq + 8 + 8 + 648 + r * 8 + c] & 0x0F) != 0x0F && gx >= 0 && gx < 128 &&
                                    gy >= 0 && gy < 128)
                                    m.water[gx * 128 + gy] = range[1];
                            }
                    }
                }
                const size_t hm = o + ofsH;
                if (ofsH && hm + 8 + 145 * 4 <= o + 8 + size && U32(d, hm) == 0x4D435654)   // "MCVT"
                {
                    float height[145];
                    memcpy(height, &d[hm + 8], sizeof(height));
                    bool ok = std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
                    for (float f : height)
                        ok = ok && std::isfinite(f);
                    if (ok)
                    {
                        const size_t first = m.v.size() / 3;
                        for (int k = 0; k < 145; ++k)
                        {
                            const int   row = k / 17, rem = k % 17;
                            const float r = rem < 9 ? static_cast<float>(row) : row + 0.5f;
                            const float c = rem < 9 ? static_cast<float>(rem) : rem - 9 + 0.5f;
                            const float z = pos[2] + height[k];
                            if (rem < 9)   // an outer vertex: into the height grid
                            {
                                const int gx = static_cast<int>(floorf((ox - pos[0]) / kUnit + 0.5f)) + row;
                                const int gy = static_cast<int>(floorf((oy - pos[1]) / kUnit + 0.5f)) + rem;
                                if (gx >= 0 && gx < 129 && gy >= 0 && gy < 129)
                                    m.grid[gx * 129 + gy] = z;
                            }
                            m.v.push_back(pos[0] - r * kUnit - ox);
                            m.v.push_back(pos[1] - c * kUnit - oy);
                            m.v.push_back(z);
                            m.minZ = (std::min)(m.minZ, z);
                            m.maxZ = (std::max)(m.maxZ, z);
                        }
                        for (int r = 0; r < 8; ++r)
                            for (int c = 0; c < 8; ++c)
                            {
                                if (holes & (1u << ((r >> 1) * 4 + (c >> 1))))
                                    continue;
                                const uint16_t tl = static_cast<uint16_t>(first + r * 17 + c);
                                const uint16_t tr = static_cast<uint16_t>(tl + 1);
                                const uint16_t bl = static_cast<uint16_t>(first + (r + 1) * 17 + c);
                                const uint16_t br = static_cast<uint16_t>(bl + 1);
                                const uint16_t mid = static_cast<uint16_t>(first + r * 17 + 9 + c);
                                const uint16_t tri[12] = { tl, tr, mid, tr, br, mid, br, bl, mid, bl, tl, mid };
                                m.idx.insert(m.idx.end(), tri, tri + 12);
                            }
                    }
                }
                ++chunks;
            }
            o += 8 + static_cast<size_t>(size);
        }
        if (mwmoSize && mwidSize && modfSize)
            Buildings(d, mwmo, mwmoSize, mwid, mwidSize, modf, modfSize, m.wmos);
        return chunks > 0;
    }

    // --- the loader thread ---------------------------------------------------------------------------

    enum JobKind { kJobTile, kJobBuilding, kJobTexture, kJobObject };
    struct Job { JobKind kind; std::string name; int a, b; unsigned gen; bool groundOnly = false; };   // name: the map, the WMO, the BLP
                                                                                                    // a: a game object's display id
    struct Loaded { std::string name; unsigned gen; bool ok; WmoMesh mesh; double ms; };
    struct LoadedTex { std::string name; unsigned gen; bool ok; BlpData data; };

    // What a game object's model gives as light (2026-10-01): read once for each display id, by the loader.
    struct ObjectLight
    {
        unsigned    display = 0;
        bool        done = false;       // the loader has answered; false while it is asked for
        std::string model;              // from GameObjectDisplayInfo.dbc; empty if the row was not found
        const char* word = nullptr;     // null: no light
        float       reach = 0.0f;
        int         count = 0;
        float       pos[kMaxFlames][3] = {};
        float       colour[3] = {};
    };

    std::mutex              g_mx;
    std::condition_variable g_cv;
    std::deque<Job>         g_jobs;
    std::deque<Mesh>        g_done;
    std::deque<Loaded>      g_doneWmo;
    std::deque<LoadedTex>   g_doneTex;
    std::deque<ObjectLight> g_doneObj;
    bool                    g_started = false;
    unsigned                g_gen = 1;   // bumped at a map change: older results are thrown away

    void Loader()
    {
        bool open = false, tried = false;
        std::vector<uint8_t> file;
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(g_mx);
                g_cv.wait(lock, [] { return !g_jobs.empty(); });
                job = g_jobs.front();
                g_jobs.pop_front();
            }
            if (!tried)
            {
                open = MpqOpen();
                tried = true;
            }
            const double t0 = Now();
            if (job.kind == kJobObject)
            {
                // GameObjectDisplayInfo.dbc, read once: the id is field 0 and the model's name field 1, an
                // offset into the strings after the records.
                static std::unordered_map<unsigned, std::string> displays;
                static bool read = false;
                if (!read && open)
                {
                    read = true;
                    std::vector<uint8_t> dbc;
                    if (MpqRead("DBFilesClient\\GameObjectDisplayInfo.dbc", dbc) && dbc.size() >= 20 &&
                        U32(dbc, 0) == 0x43424457)   // "WDBC"
                    {
                        const uint32_t rows = U32(dbc, 4), size = U32(dbc, 12), strSize = U32(dbc, 16);
                        const size_t strs = 20 + static_cast<size_t>(rows) * size;
                        if (size >= 8 && strs + strSize <= dbc.size())
                            for (uint32_t r = 0; r < rows; ++r)
                            {
                                const uint32_t off = U32(dbc, 20 + static_cast<size_t>(r) * size + 4);
                                std::string name;
                                for (size_t k = strs + off; off < strSize && k < dbc.size() && dbc[k]; ++k)
                                    name += static_cast<char>(dbc[k] >= 'a' && dbc[k] <= 'z' ? dbc[k] - 32 : dbc[k]);
                                displays[U32(dbc, 20 + static_cast<size_t>(r) * size)] = name;
                            }
                    }
                    Log("map terrain: GameObjectDisplayInfo.dbc: %u display rows", static_cast<unsigned>(displays.size()));
                }
                ObjectLight o;
                o.display = static_cast<unsigned>(job.a);
                o.done = true;
                auto it = displays.find(o.display);
                if (it != displays.end() && !it->second.empty())
                {
                    o.model = it->second;
                    M2Model m;
                    const bool ok = M2Load(o.model, m);
                    float reach = 0.0f;
                    if (const char* word = LightModelWord(o.model.substr(o.model.find_last_of('\\') + 1),
                                                          ok ? &m : nullptr, reach))
                    {
                        o.word  = word;
                        o.reach = reach;
                        o.count = LightFlames(ok ? &m : nullptr, strcmp(word, "FLAME") != 0, o.pos, o.colour);
                    }
                }
                std::lock_guard<std::mutex> lock(g_mx);
                g_doneObj.push_back(std::move(o));
                continue;
            }
            if (job.kind == kJobTexture)
            {
                LoadedTex t;
                t.name = job.name;
                t.gen = job.gen;
                t.ok = open && BlpLoad(job.name, t.data);
                std::lock_guard<std::mutex> lock(g_mx);
                g_doneTex.push_back(std::move(t));
                continue;
            }
            if (job.kind == kJobBuilding)
            {
                Loaded w;
                w.name = job.name;
                w.gen = job.gen;
                w.ok = open && WmoLoad(job.name, w.mesh);
                w.ms = 1000.0 * (Now() - t0);
                std::lock_guard<std::mutex> lock(g_mx);
                g_doneWmo.push_back(std::move(w));
                continue;
            }
            Mesh m;
            m.a = job.a;
            m.b = job.b;
            m.gen = job.gen;
            m.groundOnly = job.groundOnly;
            if (open)
            {
                char name[160];
                _snprintf_s(name, sizeof(name), _TRUNCATE, "World\\Maps\\%s\\%s_%d_%d.adt", job.name.c_str(),
                            job.name.c_str(), job.a, job.b);
                m.archives = MpqHolders(name);
                if (MpqRead(name, file))
                {
                    m.found = Build(file, m);
                    if (m.groundOnly)
                        m.wmos.clear();
                    else if (m.found)
                        Doodads(file, m, job.gen);
                }
                if (!m.found)
                {
                    m.v.clear();
                    m.idx.clear();
                    m.wmos.clear();
                }
            }
            m.ms = 1000.0 * (Now() - t0);
            std::lock_guard<std::mutex> lock(g_mx);
            g_done.push_back(std::move(m));
        }
    }

    // --- the render thread's side --------------------------------------------------------------------

    std::unordered_map<int, Tile>          g_tiles;
    std::unordered_map<std::string, Model> g_models;
    std::unordered_map<std::string, Tex>   g_texs;
    // The game objects' lights (RebuildObjectLights): each display id's, and those in the world.
    std::unordered_map<unsigned, ObjectLight> g_objectLights;   // by display id
    std::vector<MapLight>                  g_objLights;
    std::unordered_map<long long, std::vector<float>> g_doodadGrid;   // doodads drawn from the files, 4-yard cells
    unsigned                               g_dDrawnLast[2] = {};    // solid, leaf batches drawn into the last map
    unsigned                               g_texRead = 0, g_texFailed = 0;
    unsigned                               g_filesVersion = 0;   // bumped whenever what the files cover changes
    double                                 g_dMs = 0.0;
    unsigned                               g_dTiles = 0;
    // lo, hi: the box culled by, once the model is ready: the tile's box and the model's own box turned
    // into place, together. The tile's box takes in the furniture and the props (measured: the abbey's is 3
    // yards bigger, Stormwind's 196); the model's own can be the bigger where a patch changed the building.
    struct Inst { const Placement* p; Model* m; float lo[3], hi[3]; };
    std::vector<Inst>                      g_insts;      // every building in the tiles held, once
    bool                                   g_instDirty = true;
    std::unordered_map<long long, std::vector<const Placement*>> g_coverGrid;   // ready buildings, 4-yard cells
    std::string                   g_map;
    IDirect3DDevice9*             g_dev = nullptr;
    unsigned                      g_drawnLast = 0, g_loadedTotal = 0, g_missingTotal = 0;
    unsigned                      g_wmoDrawnLast = 0, g_wmoRead = 0, g_wmoFailed = 0;
    double                        g_loadMs = 0.0, g_wmoMs = 0.0;
    char                          g_info[1000] = {};

    int Key(int a, int b) { return (a << 8) | (b & 0xFF); }
    long long CellKey(long long cx, long long cy) { return (cx << 32) ^ (cy & 0xFFFFFFFFll); }

    void DropGpu(Tile& t)
    {
        SafeRelease(t.vb);
        SafeRelease(t.ib);
        SafeRelease(t.dSolidVb);
        SafeRelease(t.dSolidIb);
        SafeRelease(t.dLeafVb);
        SafeRelease(t.dLeafIb);
        t.dOnGpu = false;
    }

    void DropGpu(Tex& t)
    {
        SafeRelease(t.tex);
        if (t.state == Tex::kReady)
            t.state = Tex::kWant;
    }

    void DropGpu(Model& m)
    {
        SafeRelease(m.vb);
        SafeRelease(m.ib);
        if (m.state == Model::kReady)
            m.state = Model::kWant;   // its mesh was let go: read it again
    }

    void DropAll()
    {
        for (auto& kv : g_tiles)
            DropGpu(kv.second);
        g_tiles.clear();
        for (auto& kv : g_models)
            DropGpu(kv.second);
        g_models.clear();
        for (auto& kv : g_texs)
            DropGpu(kv.second);
        g_texs.clear();
        g_insts.clear();
        g_coverGrid.clear();
        g_doodadGrid.clear();
        g_instDirty = true;
        // A display id still asked for loses its job here: forget it, so the next walk asks again.
        for (auto it = g_objectLights.begin(); it != g_objectLights.end();)
            it = it->second.done ? std::next(it) : g_objectLights.erase(it);
        g_objLights.clear();
        std::lock_guard<std::mutex> lock(g_mx);
        g_jobs.clear();
        ++g_gen;
    }

    template <typename VB, typename IB>
    bool Upload(IDirect3DDevice9* dev, const std::vector<float>& v, const void* idx, UINT idxBytes, D3DFORMAT fmt,
                VB*& vb, IB*& ib, DWORD fvf = D3DFVF_XYZ)
    {
        auto* d = dev->lpVtbl;
        const UINT vBytes = static_cast<UINT>(v.size() * sizeof(float));
        void* p = nullptr;
        if (FAILED(d->CreateVertexBuffer(dev, vBytes, D3DUSAGE_WRITEONLY, fvf, D3DPOOL_MANAGED, &vb, nullptr)) ||
            FAILED(vb->lpVtbl->Lock(vb, 0, 0, &p, 0)))
        {
            SafeRelease(vb);
            return false;
        }
        memcpy(p, v.data(), vBytes);
        vb->lpVtbl->Unlock(vb);
        if (FAILED(d->CreateIndexBuffer(dev, idxBytes, D3DUSAGE_WRITEONLY, fmt, D3DPOOL_MANAGED, &ib, nullptr)) ||
            FAILED(ib->lpVtbl->Lock(ib, 0, 0, &p, 0)))
        {
            SafeRelease(vb);
            SafeRelease(ib);
            return false;
        }
        memcpy(p, idx, idxBytes);
        ib->lpVtbl->Unlock(ib);
        return true;
    }

    bool UploadModel(IDirect3DDevice9* dev, Model& m)
    {
        const size_t nv = m.mesh.v.size() / 3;
        bool ok;
        if (nv <= 65535)   // 16-bit indices where they fit
        {
            std::vector<uint16_t> idx16(m.mesh.idx.begin(), m.mesh.idx.end());
            ok = Upload(dev, m.mesh.v, idx16.data(), static_cast<UINT>(idx16.size() * 2), D3DFMT_INDEX16, m.vb, m.ib);
        }
        else
            ok = Upload(dev, m.mesh.v, m.mesh.idx.data(), static_cast<UINT>(m.mesh.idx.size() * 4), D3DFMT_INDEX32,
                        m.vb, m.ib);
        m.nv   = static_cast<UINT>(nv);
        m.ntri = static_cast<UINT>(m.mesh.idx.size() / 3);
        return ok;
    }

    bool UploadTex(IDirect3DDevice9* dev, Tex& t)
    {
        static const D3DFORMAT kFormats[4] = { D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5, D3DFMT_A8R8G8B8 };
        const BlpData& b = t.data;
        if (FAILED(dev->lpVtbl->CreateTexture(dev, b.width, b.height, static_cast<UINT>(b.levels.size()), 0,
                                              kFormats[b.format], D3DPOOL_MANAGED, &t.tex, nullptr)))
            return false;
        for (UINT i = 0; i < b.levels.size(); ++i)
        {
            const UINT w = (std::max)(1u, b.width >> i), h = (std::max)(1u, b.height >> i);
            const UINT rows = b.format == 3 ? h : (std::max)(1u, (h + 3) / 4);
            const UINT rowBytes = b.format == 3 ? w * 4 : (std::max)(1u, (w + 3) / 4) * (b.format == 0 ? 8 : 16);
            D3DLOCKED_RECT lr;
            if (FAILED(t.tex->lpVtbl->LockRect(t.tex, i, &lr, nullptr, 0)))
            {
                SafeRelease(t.tex);
                return false;
            }
            for (UINT r = 0; r < rows; ++r)
                memcpy(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch, &b.levels[i][r * rowBytes], rowBytes);
            t.tex->lpVtbl->UnlockRect(t.tex, i);
        }
        return true;
    }

    // A tile's doodads stand in for the client's own draws once they are on the GPU and every texture they
    // cut leaves with is either there or known to be missing.
    bool DoodadsSettled(const Tile& t)
    {
        if (!t.dOnGpu)
            return false;
        for (const Mesh::Batch& b : t.mesh.dBatches)
        {
            if (b.tex.empty())
                continue;
            auto it = g_texs.find(b.tex);
            if (it == g_texs.end() || (it->second.state != Tex::kReady && it->second.state != Tex::kFailed))
                return false;
        }
        return true;
    }

    void RebuildDoodadCover()
    {
        g_doodadGrid.clear();
        for (const auto& kv : g_tiles)
        {
            if (!DoodadsSettled(kv.second))
                continue;
            const std::vector<float>& p = kv.second.mesh.dPos;
            for (size_t i = 0; i + 2 < p.size(); i += 3)
            {
                std::vector<float>& cell = g_doodadGrid[CellKey(static_cast<long long>(floorf(p[i] * 0.25f)),
                                                                static_cast<long long>(floorf(p[i + 1] * 0.25f)))];
                cell.insert(cell.end(), &p[i], &p[i] + 3);
            }
        }
    }

    // Across the ground from (x, y) to tile (a, b)'s square.
    float Distance(float x, float y, int a, int b)
    {
        const float x1 = CornerX(b), y1 = CornerY(a);
        const float dx = x > x1 ? x - x1 : x < x1 - kTile ? x1 - kTile - x : 0.0f;
        const float dy = y > y1 ? y - y1 : y < y1 - kTile ? y1 - kTile - y : 0.0f;
        return sqrtf(dx * dx + dy * dy);
    }

    // Every corner of a box past the same side of the clip volume: it cannot mark the map.
    bool Outside(const D3DMATRIX& m, const float lo[3], const float hi[3])
    {
        int out[6] = {};
        for (int i = 0; i < 8; ++i)
        {
            const float p[3] = { (i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2] };
            float c[4];
            for (int j = 0; j < 4; ++j)
                c[j] = p[0] * m.m[0][j] + p[1] * m.m[1][j] + p[2] * m.m[2][j] + m.m[3][j];
            out[0] += c[0] > c[3];
            out[1] += c[0] < -c[3];
            out[2] += c[1] > c[3];
            out[3] += c[1] < -c[3];
            out[4] += c[2] < 0.0f;
            out[5] += c[2] > c[3];
        }
        return out[0] == 8 || out[1] == 8 || out[2] == 8 || out[3] == 8 || out[4] == 8 || out[5] == 8;
    }

    // The buildings of the tiles held, each once, and the models they need.
    void RebuildInstances()
    {
        g_insts.clear();
        for (auto& kv : g_models)
            kv.second.used = false;
        std::unordered_set<uint32_t> seen;
        for (const auto& kv : g_tiles)
            for (const Placement& p : kv.second.mesh.wmos)
                if (seen.insert(p.uid).second)
                {
                    Model& m = g_models[p.name];
                    m.used = true;
                    g_insts.push_back({ &p, &m, {}, {} });
                }
        for (auto it = g_models.begin(); it != g_models.end();)
        {
            if (!it->second.used)
            {
                DropGpu(it->second);
                it = g_models.erase(it);
            }
            else
                ++it;
        }
    }

    // The lights of the buildings held, in the world: rebuilt with the cover.
    std::vector<MapLight> g_fileLights;

    // The game objects' lights (2026-10-01): each display id's light, once the loader has read it, and the
    // lights of the game objects in the world, rebuilt four times a second.
    double                g_objWalked = 0.0;
    unsigned              g_objSeen = 0, g_objLit = 0;

    void RebuildObjectLights()
    {
        static ClientObject objs[2048];
        const int n = ClientGameObjects(objs, 2048);
        g_objLights.clear();
        g_objSeen = static_cast<unsigned>(n);
        g_objLit = 0;
        std::vector<int> ask;
        for (int i = 0; i < n; ++i)
        {
            const ClientObject& o = objs[i];
            auto it = g_objectLights.find(o.display);
            if (it == g_objectLights.end())
            {
                g_objectLights[o.display].display = o.display;
                ask.push_back(static_cast<int>(o.display));
                continue;
            }
            const ObjectLight& L = it->second;
            if (!L.word)
                continue;
            ++g_objLit;
            const float c = cosf(o.facing), sn = sinf(o.facing);
            for (int f = 0; f < L.count; ++f)
            {
                MapLight w = {};
                const float x = L.pos[f][0] * o.scale, y = L.pos[f][1] * o.scale;
                w.pos[0] = o.pos[0] + x * c - y * sn;
                w.pos[1] = o.pos[1] + x * sn + y * c;
                w.pos[2] = o.pos[2] + L.pos[f][2] * o.scale;
                w.reach = L.reach * (std::min)((std::max)(o.scale, 0.5f), 2.0f);
                memcpy(w.colour, L.colour, sizeof(w.colour));
                strncpy_s(w.what, L.word, _TRUNCATE);
                g_objLights.push_back(w);
            }
        }
        if (!ask.empty())
        {
            std::lock_guard<std::mutex> lock(g_mx);
            for (int d : ask)
                g_jobs.push_back({ kJobObject, std::string(), d, 0, g_gen });
            g_cv.notify_one();
        }
    }

    void RebuildLights()
    {
        g_fileLights.clear();
        for (const Inst& i : g_insts)
        {
            if (i.m->state != Model::kReady)
                continue;
            for (const WmoLight& L : i.m->lights)
            {
                if (L.set != 0 && L.set != i.p->doodadSet)
                    continue;
                MapLight w = {};
                for (int j = 0; j < 3; ++j)
                    w.pos[j] = L.pos[0] * i.p->rot[0][j] + L.pos[1] * i.p->rot[1][j] + L.pos[2] * i.p->rot[2][j] +
                               i.p->pos[j];
                w.reach = L.reach;
                memcpy(w.colour, L.colour, sizeof(w.colour));
                memcpy(w.what, L.what, sizeof(w.what));
                g_fileLights.push_back(w);
            }
        }
        for (const auto& kv : g_tiles)
            g_fileLights.insert(g_fileLights.end(), kv.second.mesh.dLights.begin(), kv.second.mesh.dLights.end());
    }

    void RebuildCover()
    {
        g_coverGrid.clear();
        for (Inst& i : g_insts)
        {
            memcpy(i.lo, i.p->lo, 12);
            memcpy(i.hi, i.p->hi, 12);
            if (i.m->state != Model::kReady)
                continue;
            for (int c = 0; c < 8; ++c)
            {
                const float q[3] = { (c & 1) ? i.m->hi[0] : i.m->lo[0], (c & 2) ? i.m->hi[1] : i.m->lo[1],
                                     (c & 4) ? i.m->hi[2] : i.m->lo[2] };
                for (int j = 0; j < 3; ++j)
                {
                    const float v = q[0] * i.p->rot[0][j] + q[1] * i.p->rot[1][j] + q[2] * i.p->rot[2][j] + i.p->pos[j];
                    i.lo[j] = (std::min)(i.lo[j], v);
                    i.hi[j] = (std::max)(i.hi[j], v);
                }
            }
        }
        for (const Inst& i : g_insts)
            if (i.m->state == Model::kReady)
                g_coverGrid[CellKey(static_cast<long long>(floorf(i.p->pos[0] * 0.25f)),
                                    static_cast<long long>(floorf(i.p->pos[1] * 0.25f)))].push_back(i.p);
    }
}

void MapTerrainUpdate(IDirect3DDevice9* dev, const float player[3], float reach, float fullReach)
{
    if (reach <= 0.0f)
    {
        if (!g_tiles.empty() || !g_models.empty() || !g_texs.empty())
            DropAll();
        g_map.clear();
        return;
    }
    if (dev != g_dev)
    {
        for (auto& kv : g_tiles)
            DropGpu(kv.second);
        for (auto& kv : g_models)
            DropGpu(kv.second);
        for (auto& kv : g_texs)
            DropGpu(kv.second);
        g_dev = dev;
        g_instDirty = true;
    }
    char map[64] = {};
    if (!ClientMapName(map, sizeof(map)))
        map[0] = 0;
    if (g_map != map)
    {
        if (!g_map.empty() || !g_tiles.empty())
            Log("map terrain: the map is now \"%s\" (was \"%s\")", map, g_map.c_str());
        DropAll();
        g_map = map;
    }
    if (g_map.empty())
        return;

    if (!g_started)
    {
        std::thread(Loader).detach();
        g_started = true;
    }

    // What the loader finished.
    bool coverDirty = false;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        while (!g_done.empty())
        {
            Mesh m = std::move(g_done.front());
            g_done.pop_front();
            if (m.gen != g_gen)
                continue;
            auto it = g_tiles.find(Key(m.a, m.b));
            if (it == g_tiles.end() || !(it->second.pending || (it->second.upgrading && !m.groundOnly)))
                continue;
            (m.found ? g_loadedTotal : g_missingTotal)++;
            g_loadMs += m.ms;
            it->second.mesh = std::move(m);
            it->second.pending = false;
            it->second.upgrading = false;
            g_instDirty = true;
        }
        while (!g_doneWmo.empty())
        {
            Loaded w = std::move(g_doneWmo.front());
            g_doneWmo.pop_front();
            if (w.gen != g_gen)
                continue;
            auto it = g_models.find(w.name);
            if (it == g_models.end() || it->second.state != Model::kAsked)
                continue;
            (w.ok ? g_wmoRead : g_wmoFailed)++;
            g_wmoMs += w.ms;
            if (!w.ok)
                Log("map terrain: could not read the building %s", w.name.c_str());
            it->second.mesh  = std::move(w.mesh);
            it->second.state = w.ok ? Model::kLoaded : Model::kFailed;
        }
        while (!g_doneObj.empty())
        {
            ObjectLight o = std::move(g_doneObj.front());
            g_doneObj.pop_front();
            if (o.word)
                Log("map terrain: game object display %u (%s): %s, %d light%s, reach %.0f, colour (%.2f %.2f %.2f)",
                    o.display, o.model.c_str(), o.word, o.count, o.count == 1 ? "" : "s", o.reach, o.colour[0],
                    o.colour[1], o.colour[2]);
            g_objectLights[o.display] = std::move(o);
        }
        while (!g_doneTex.empty())
        {
            LoadedTex t = std::move(g_doneTex.front());
            g_doneTex.pop_front();
            if (t.gen != g_gen)
                continue;
            auto it = g_texs.find(t.name);
            if (it == g_texs.end() || it->second.state != Tex::kAsked)
                continue;
            (t.ok ? g_texRead : g_texFailed)++;
            if (!t.ok)
            {
                Log("map terrain: could not read the texture %s", t.name.c_str());
                coverDirty = true;   // settled: its leaves are left out, and so are the client's
            }
            it->second.data  = std::move(t.data);
            it->second.state = t.ok ? Tex::kLoaded : Tex::kFailed;
        }
    }

    // Ask for the tiles in reach, nearest first; drop those well out of it. Past fullReach a tile is read
    // for its ground alone (2026-09-30): only the ground casts that far toward the sun ([shadow] horizonDepth),
    // and 40 or 50 tiles with their doodads and buildings would be a lot to hold in a 32-bit client. One held
    // for its ground is read again in full once it comes within fullReach.
    const float x = player[0], y = player[1];
    const int a0 = static_cast<int>(floorf(32.0f - (y + reach) / kTile)), a1 = static_cast<int>(floorf(32.0f - (y - reach) / kTile));
    const int b0 = static_cast<int>(floorf(32.0f - (x + reach) / kTile)), b1 = static_cast<int>(floorf(32.0f - (x - reach) / kTile));
    std::vector<std::pair<float, int>> ask;
    for (int a = (std::max)(a0, 0); a <= (std::min)(a1, 63); ++a)
        for (int b = (std::max)(b0, 0); b <= (std::min)(b1, 63); ++b)
        {
            const float dist = Distance(x, y, a, b);
            if (dist > reach)
                continue;
            auto it = g_tiles.find(Key(a, b));
            if (it == g_tiles.end())
                ask.push_back({ dist, Key(a, b) });
            else if (dist <= fullReach && !it->second.pending && !it->second.upgrading && it->second.mesh.groundOnly)
                ask.push_back({ dist, Key(a, b) });
        }
    if (!ask.empty())
    {
        std::sort(ask.begin(), ask.end());
        std::lock_guard<std::mutex> lock(g_mx);
        for (const auto& q : ask)
        {
            auto it = g_tiles.find(q.second);
            if (it == g_tiles.end())
                g_tiles[q.second];   // pending
            else
                it->second.upgrading = true;
            Job job = { kJobTile, g_map, q.second >> 8, q.second & 0xFF, g_gen };
            job.groundOnly = q.first > fullReach;
            // The nearest first: a tile in full before the ground of one further off.
            g_jobs.push_back(job);
        }
        g_cv.notify_one();
    }
    for (auto it = g_tiles.begin(); it != g_tiles.end();)
    {
        if (Distance(x, y, it->first >> 8, it->first & 0xFF) > reach + 400.0f)
        {
            DropGpu(it->second);
            it = g_tiles.erase(it);   // a pending one's result is thrown away when it comes
            g_instDirty = true;
        }
        else
            ++it;
    }

    // The buildings: each model asked for once, nearest placement first.
    if (g_instDirty)
    {
        RebuildInstances();
        g_instDirty = false;
        coverDirty = true;
        // The leaf textures the tiles held use; the others go.
        for (auto& kv : g_texs)
            kv.second.used = false;
        std::vector<std::string> ask;
        for (const auto& kv : g_tiles)
            for (const Mesh::Batch& b : kv.second.mesh.dBatches)
                if (!b.tex.empty())
                {
                    Tex& t = g_texs[b.tex];
                    t.used = true;
                    if (t.state == Tex::kWant)
                    {
                        t.state = Tex::kAsked;
                        ask.push_back(b.tex);
                    }
                }
        for (auto it = g_texs.begin(); it != g_texs.end();)
        {
            if (!it->second.used)
            {
                DropGpu(it->second);
                it = g_texs.erase(it);
            }
            else
                ++it;
        }
        if (!ask.empty())
        {
            std::lock_guard<std::mutex> lock(g_mx);
            for (const std::string& n : ask)
                g_jobs.push_back({ kJobTexture, n, 0, 0, g_gen });
            g_cv.notify_one();
        }
    }
    {
        std::vector<std::pair<float, const Inst*>> want;
        for (const Inst& i : g_insts)
            if (i.m->state == Model::kWant)
            {
                const float dx = i.p->pos[0] - x, dy = i.p->pos[1] - y;
                want.push_back({ dx * dx + dy * dy, &i });
            }
        if (!want.empty())
        {
            std::sort(want.begin(), want.end(),
                      [](const std::pair<float, const Inst*>& l, const std::pair<float, const Inst*>& r) {
                          return l.first < r.first;
                      });
            std::lock_guard<std::mutex> lock(g_mx);
            for (const auto& w : want)
            {
                if (w.second->m->state != Model::kWant)
                    continue;   // two placements of one model
                w.second->m->state = Model::kAsked;
                g_jobs.push_back({ kJobBuilding, w.second->p->name, 0, 0, g_gen });
            }
            g_cv.notify_one();
        }
    }

    // Up to two terrain tiles and one building to the GPU a frame.
    int uploads = 0;
    for (auto& kv : g_tiles)
    {
        Tile& t = kv.second;
        if (t.pending || !t.mesh.found || t.mesh.idx.empty() || t.vb || uploads >= 2)
            continue;
        if (!Upload(dev, t.mesh.v, t.mesh.idx.data(), static_cast<UINT>(t.mesh.idx.size() * 2), D3DFMT_INDEX16, t.vb, t.ib))
        {
            Log("map terrain: could not make the buffers for tile %d_%d", t.mesh.a, t.mesh.b);
            t.mesh.found = false;   // the client's own draws cast there instead
            continue;
        }
        ++uploads;
    }
    // A tile's doodads, one tile a frame: up to 10 MB.
    for (auto& kv : g_tiles)
    {
        Tile& t = kv.second;
        if (t.pending || t.dOnGpu || (t.mesh.dSolidIdx.empty() && t.mesh.dLeafIdx.empty()))
            continue;
        bool ok = true;
        if (!t.mesh.dSolidIdx.empty())
            ok = Upload(dev, t.mesh.dSolid, t.mesh.dSolidIdx.data(), static_cast<UINT>(t.mesh.dSolidIdx.size() * 4),
                        D3DFMT_INDEX32, t.dSolidVb, t.dSolidIb);
        if (ok && !t.mesh.dLeafIdx.empty())
            ok = Upload(dev, t.mesh.dLeaf, t.mesh.dLeafIdx.data(), static_cast<UINT>(t.mesh.dLeafIdx.size() * 4),
                        D3DFMT_INDEX32, t.dLeafVb, t.dLeafIb, D3DFVF_XYZ | D3DFVF_TEX1);
        if (!ok)
        {
            Log("map terrain: could not make the doodad buffers for tile %d_%d", t.mesh.a, t.mesh.b);
            DropGpu(t);
            t.mesh.dSolidIdx.clear();
            t.mesh.dLeafIdx.clear();
            t.mesh.dPos.clear();   // the client's own draws cast there instead
            continue;
        }
        t.dOnGpu = true;
        ++g_dTiles;
        g_dMs += t.mesh.dMs;
        coverDirty = true;
        break;
    }
    // Up to eight leaf textures a frame: most are 64 to 256 texels, DXT.
    int texUploads = 0;
    for (auto& kv : g_texs)
    {
        Tex& t = kv.second;
        if (t.state != Tex::kLoaded || texUploads >= 8)
            continue;
        if (UploadTex(dev, t))
            t.state = Tex::kReady;
        else
        {
            Log("map terrain: could not make the texture %s (%ux%u, format %d)", kv.first.c_str(), t.data.width,
                t.data.height, t.data.format);
            t.state = Tex::kFailed;
        }
        t.data = BlpData();
        ++texUploads;
        coverDirty = true;
    }
    for (auto& kv : g_models)
    {
        Model& m = kv.second;
        if (m.state != Model::kLoaded)
            continue;
        if (!UploadModel(dev, m))
        {
            Log("map terrain: could not make the buffers for the building %s", kv.first.c_str());
            m.state = Model::kFailed;
        }
        else
        {
            m.state = Model::kReady;
            memcpy(m.lo, m.mesh.lo, 12);
            m.indoor = m.mesh.indoor;
            m.indoorTris = std::move(m.mesh.indoorTris);
            m.lights = m.mesh.lights;
            memcpy(m.hi, m.mesh.hi, 12);
            m.mesh  = WmoMesh();   // on the GPU now; read again after a device change
            coverDirty = true;
        }
        break;
    }
    if (coverDirty)
    {
        RebuildCover();
        RebuildDoodadCover();
        RebuildLights();
        ++g_filesVersion;
    }
    const double now = Now();
    if (now - g_objWalked > 0.25)
    {
        g_objWalked = now;
        RebuildObjectLights();
    }
}

// Nearest first. Until 2026-10-01 they came in the files' order and stopped at max. Stormwind's building
// alone holds up to 1351 (606 of its own, 745 lit doodads), so the candles beside you could be left out.
int MapLightsNear(const float at[3], float radius, MapLight* out, int max)
{
    static std::vector<std::pair<float, const MapLight*>> inReach;
    inReach.clear();
    for (const std::vector<MapLight>* list : { &g_fileLights, &g_objLights })
        for (const MapLight& L : *list)
        {
            const float dx = L.pos[0] - at[0], dy = L.pos[1] - at[1], dz = L.pos[2] - at[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 <= radius * radius)
                inReach.emplace_back(d2, &L);
        }
    const int n = (std::min)(max, static_cast<int>(inReach.size()));
    std::partial_sort(inReach.begin(), inReach.begin() + n, inReach.end(),
                      [](const std::pair<float, const MapLight*>& a, const std::pair<float, const MapLight*>& b)
                      { return a.first < b.first; });
    for (int i = 0; i < n; ++i)
        out[i] = *inReach[i].second;
    return n;
}

unsigned MapLightCount() { return static_cast<unsigned>(g_fileLights.size() + g_objLights.size()); }

void MapObjectsLog(const float at[3], float radius)
{
    static ClientObject objs[2048];
    const int n = ClientGameObjects(objs, 2048);
    int shown = 0;
    for (int i = 0; i < n; ++i)
    {
        const ClientObject& o = objs[i];
        const float dx = o.pos[0] - at[0], dy = o.pos[1] - at[1], dz = o.pos[2] - at[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > radius)
            continue;
        ++shown;
        auto it = g_objectLights.find(o.display);
        const ObjectLight* L = it != g_objectLights.end() ? &it->second : nullptr;
        Log("map terrain:   game object %5.1f yd at (%.1f %.1f %.1f), scale %.2f, display %u: %s, %s", d, o.pos[0],
            o.pos[1], o.pos[2], o.scale, o.display, L && L->done ? (L->model.empty() ? "(no row)" : L->model.c_str())
            : "(not read yet)", L && L->word ? L->word : "no light");
    }
    Log("map terrain: %d of %d game objects within %.0f yards", shown, n, radius);
}

bool MapIndoors(const float p[3])
{
    for (const Inst& i : g_insts)
    {
        if (i.m->state != Model::kReady || i.m->indoor.empty())
            continue;
        if (p[0] < i.lo[0] || p[0] > i.hi[0] || p[1] < i.lo[1] || p[1] > i.hi[1] || p[2] < i.lo[2] || p[2] > i.hi[2])
            continue;
        // Into the building's own space: world = own * rot + pos, and rot is a turn, so own = (world - pos) rot^T.
        const float d[3] = { p[0] - i.p->pos[0], p[1] - i.p->pos[1], p[2] - i.p->pos[2] };
        float q[3];
        for (int j = 0; j < 3; ++j)
            q[j] = d[0] * i.p->rot[j][0] + d[1] * i.p->rot[j][1] + d[2] * i.p->rot[j][2];
        const std::vector<float>& b = i.m->indoor;
        for (size_t k = 0; k + 5 < b.size(); k += 6)
        {
            if (!(q[0] >= b[k] && q[0] <= b[k + 3] && q[1] >= b[k + 1] && q[1] <= b[k + 4] && q[2] >= b[k + 2] &&
                  q[2] <= b[k + 5]))
                continue;
            // A room's box can reach over a street: all of Stormwind is one building, and in the Trade
            // District the sun shadows were off in the open (2026-09-30). The room must also have a ceiling
            // over the point: one of its triangles straight above, more than 1.5 yards up and within 40.
            const size_t g = k / 6;
            if (g >= i.m->indoorTris.size())
                return true;   // no triangles kept: the box alone, as before
            const std::vector<float>& t = i.m->indoorTris[g];
            for (size_t n = 0; n + 8 < t.size(); n += 9)
            {
                const float* a = &t[n];
                const float* c = &t[n + 3];
                const float* e = &t[n + 6];
                const float d0x = c[0] - a[0], d0y = c[1] - a[1], d1x = e[0] - a[0], d1y = e[1] - a[1];
                const float den = d0x * d1y - d1x * d0y;
                if (den > -1e-6f && den < 1e-6f)
                    continue;   // a wall, seen from above
                const float px = q[0] - a[0], py = q[1] - a[1];
                const float u = (px * d1y - d1x * py) / den, v = (d0x * py - px * d0y) / den;
                if (u < 0.0f || v < 0.0f || u + v > 1.0f)
                    continue;
                const float z = a[2] + u * (c[2] - a[2]) + v * (e[2] - a[2]);
                if (z > q[2] + 1.5f && z < q[2] + 40.0f)
                    return true;
            }
        }
    }
    return false;
}

bool MapGroundHeight(float x, float y, float& z)
{
    const int a = static_cast<int>(floorf(32.0f - y / kTile)), b = static_cast<int>(floorf(32.0f - x / kTile));
    auto it = g_tiles.find(Key(a, b));
    if (it == g_tiles.end() || it->second.pending || it->second.mesh.grid.size() != 129 * 129)
        return false;
    const std::vector<float>& g = it->second.mesh.grid;
    const float fx = (CornerX(b) - x) / kUnit, fy = (CornerY(a) - y) / kUnit;
    const int ix = (std::min)(static_cast<int>(fx), 127), iy = (std::min)(static_cast<int>(fy), 127);
    if (ix < 0 || iy < 0)
        return false;
    const float tx = fx - ix, ty = fy - iy;
    const float h00 = g[ix * 129 + iy], h01 = g[ix * 129 + iy + 1], h10 = g[(ix + 1) * 129 + iy],
                h11 = g[(ix + 1) * 129 + iy + 1];
    if (!(h00 == h00 && h01 == h01 && h10 == h10 && h11 == h11))
        return false;
    z = (h00 * (1 - ty) + h01 * ty) * (1 - tx) + (h10 * (1 - ty) + h11 * ty) * tx;
    return true;
}

bool MapWaterHeight(float x, float y, float& z)
{
    const int a = static_cast<int>(floorf(32.0f - y / kTile)), b = static_cast<int>(floorf(32.0f - x / kTile));
    auto it = g_tiles.find(Key(a, b));
    if (it == g_tiles.end() || it->second.pending || it->second.mesh.water.size() != 128 * 128)
        return false;
    const int ix = static_cast<int>((CornerX(b) - x) / kUnit), iy = static_cast<int>((CornerY(a) - y) / kUnit);
    if (ix < 0 || iy < 0 || ix > 127 || iy > 127)
        return false;
    const float w = it->second.mesh.water[ix * 128 + iy];
    if (!(w == w))
        return false;
    z = w;
    return true;
}

bool MapGroundBase(const float at[3], float radius, float& z)
{
    float sum = 0.0f;
    int n = 0;
    for (int i = -3; i <= 3; ++i)
        for (int j = -3; j <= 3; ++j)
        {
            if (i * i + j * j > 10)
                continue;   // a disc, not the square
            float h;
            if (MapGroundHeight(at[0] + i * radius / 3.0f, at[1] + j * radius / 3.0f, h))
            {
                sum += h;
                ++n;
            }
        }
    if (n < 5)
        return false;
    z = sum / n;
    return true;
}

bool MapTerrainCovers(float x, float y)
{
    if (g_tiles.empty())
        return false;
    const int a = static_cast<int>(floorf(32.0f - y / kTile)), b = static_cast<int>(floorf(32.0f - x / kTile));
    auto it = g_tiles.find(Key(a, b));
    return it != g_tiles.end() && it->second.vb != nullptr;
}

bool MapBuildingCovers(const float pos[3])
{
    if (g_coverGrid.empty())
        return false;
    const long long cx = static_cast<long long>(floorf(pos[0] * 0.25f)), cy = static_cast<long long>(floorf(pos[1] * 0.25f));
    for (long long ox = -1; ox <= 1; ++ox)
        for (long long oy = -1; oy <= 1; ++oy)
        {
            auto it = g_coverGrid.find(CellKey(cx + ox, cy + oy));
            if (it == g_coverGrid.end())
                continue;
            for (const Placement* p : it->second)
                if (fabsf(p->pos[0] - pos[0]) < 0.5f && fabsf(p->pos[1] - pos[1]) < 0.5f && fabsf(p->pos[2] - pos[2]) < 0.5f)
                    return true;
        }
    return false;
}

// The doodads the client has drawn, as far as MapDoodadCovers matched them: a quarter-yard key of the place.
std::unordered_set<long long> g_doodadsSeen;

long long SeenKey(const float* p)
{
    return (static_cast<long long>(floorf(p[0] * 4.0f)) << 42) ^ (static_cast<long long>(floorf(p[1] * 4.0f)) << 21) ^
           static_cast<long long>(floorf(p[2] * 4.0f));
}

void MapLogDoodadsNear(const float from[3], float radius)
{
    unsigned shown = 0;
    for (const auto& kv : g_tiles)
    {
        const Mesh& me = kv.second.mesh;
        bool tileShown = false;
        for (size_t i = 0; i + 2 < me.dPos.size() && i / 3 < me.dName.size() && shown < 30; i += 3)
        {
            const float* p = &me.dPos[i];
            const float dx = p[0] - from[0], dy = p[1] - from[1];
            const float d = sqrtf(dx * dx + dy * dy);
            if (d > radius)
                continue;
            if (!tileShown)
            {
                tileShown = true;
                Log("shadow: tile %d_%d, read from the first of: %s", me.a, me.b, me.archives.c_str());
            }
            ++shown;
            Log("shadow:   doodad %s, scale %.2f, at (%.1f %.1f %.1f), %.0f yd from you, %.1f yd up from you; the "
                "client has drawn it: %s", me.dName[i / 3].c_str(), me.dScale[i / 3], p[0], p[1], p[2], d,
                p[2] - from[2], g_doodadsSeen.count(SeenKey(p)) ? "yes" : "not seen");
        }
    }
    if (!shown)
        Log("shadow: no doodads from the files within %.0f yd", radius);
    for (const auto& kv : g_tiles)
    {
        const Mesh& me = kv.second.mesh;
        for (size_t i = 0; i + 2 < me.dAnim.size() && i / 3 < me.dAnimName.size(); i += 3)
        {
            const float* p = &me.dAnim[i];
            const float dx = p[0] - from[0], dy = p[1] - from[1];
            const float d = sqrtf(dx * dx + dy * dy);
            if (d <= radius)
                Log("shadow:   animated doodad %s at (%.1f %.1f %.1f), %.0f yd from you: left to the client's draws",
                    me.dAnimName[i / 3].c_str(), p[0], p[1], p[2], d);
        }
    }
}

bool MapAnimatedDoodadAt(const float pos[3], float tol)
{
    for (const auto& kv : g_tiles)
    {
        const std::vector<float>& a = kv.second.mesh.dAnim;
        for (size_t i = 0; i + 2 < a.size(); i += 3)
        {
            const float dx = a[i] - pos[0], dy = a[i + 1] - pos[1], dz = a[i + 2] - pos[2];
            if (dx * dx + dy * dy + dz * dz < tol * tol)
                return true;
        }
    }
    return false;
}

bool MapDoodadCovers(const float pos[3], float tol)
{
    if (g_doodadGrid.empty())
        return false;
    const long long cx = static_cast<long long>(floorf(pos[0] * 0.25f)), cy = static_cast<long long>(floorf(pos[1] * 0.25f));
    for (long long ox = -1; ox <= 1; ++ox)
        for (long long oy = -1; oy <= 1; ++oy)
        {
            auto it = g_doodadGrid.find(CellKey(cx + ox, cy + oy));
            if (it == g_doodadGrid.end())
                continue;
            const std::vector<float>& p = it->second;
            for (size_t i = 0; i + 2 < p.size(); i += 3)
                if (fabsf(p[i] - pos[0]) < tol && fabsf(p[i + 1] - pos[1]) < tol && fabsf(p[i + 2] - pos[2]) < tol)
                {
                    g_doodadsSeen.insert(SeenKey(&p[i]));
                    return true;
                }
        }
    return false;
}

unsigned MapFilesVersion() { return g_filesVersion; }

bool MapDoodadNearest(const float from[3], float pos[3])
{
    float bestD = 1e30f;
    bool found = false;
    for (const auto& kv : g_doodadGrid)
        for (size_t i = 0; i + 2 < kv.second.size(); i += 3)
        {
            const float* p = &kv.second[i];
            const float dx = p[0] - from[0], dy = p[1] - from[1];
            if (dx * dx + dy * dy < bestD)
            {
                bestD = dx * dx + dy * dy;
                memcpy(pos, p, 12);
                found = true;
            }
        }
    return found;
}

unsigned MapDoodadsDraw(IDirect3DDevice9* dev, const D3DMATRIX& m, const float cam[3], bool leaves, DWORD alphaRef,
                        DWORD alphaFunc)
{
    auto* d = dev->lpVtbl;
    unsigned drawn = 0;
    bool set = false;
    // The sampler states the leaves need, put back afterwards: the cache's own draws after these in the
    // same pass inherit whatever is left.
    static const D3DSAMPLERSTATETYPE kSamp[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER,
                                                 D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER };
    DWORD saved[5] = {};
    for (auto& kv : g_tiles)
    {
        const Tile& t = kv.second;
        if (!t.dOnGpu || (leaves ? !t.dLeafVb : !t.dSolidVb))
            continue;
        const float x1 = CornerX(t.mesh.b), y1 = CornerY(t.mesh.a);
        const float lo[3] = { x1 - kTile - 60.0f, y1 - kTile - 60.0f, t.mesh.dMinZ },
                    hi[3] = { x1 + 60.0f, y1 + 60.0f, t.mesh.dMaxZ };   // a tree reaches past its tile's edge
        if (Outside(m, lo, hi))
            continue;
        if (!set)
        {
            d->SetVertexShader(dev, nullptr);
            d->SetFVF(dev, leaves ? (D3DFVF_XYZ | D3DFVF_TEX1) : D3DFVF_XYZ);
            d->SetTexture(dev, 0, nullptr);
            d->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
            if (leaves)
            {
                for (int i = 0; i < 5; ++i)
                    d->GetSamplerState(dev, 0, kSamp[i], &saved[i]);
                d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
                d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                d->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                d->SetRenderState(dev, D3DRS_ALPHAREF, alphaRef);
                d->SetRenderState(dev, D3DRS_ALPHAFUNC, alphaFunc);
            }
            set = true;
        }
        D3DMATRIX w = {};
        w.m[0][0] = w.m[1][1] = w.m[2][2] = w.m[3][3] = 1.0f;
        w.m[3][0] = x1 - cam[0];
        w.m[3][1] = y1 - cam[1];
        w.m[3][2] = -cam[2];
        d->SetTransform(dev, D3DTS_WORLD, &w);
        if (!leaves)
        {
            d->SetStreamSource(dev, 0, t.dSolidVb, 0, 12);
            d->SetIndices(dev, t.dSolidIb);
            d->DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, static_cast<UINT>(t.mesh.dSolid.size() / 3), 0,
                                    static_cast<UINT>(t.mesh.dSolidIdx.size() / 3));
            ++drawn;
            continue;
        }
        d->SetStreamSource(dev, 0, t.dLeafVb, 0, 20);
        d->SetIndices(dev, t.dLeafIb);
        const UINT nv = static_cast<UINT>(t.mesh.dLeaf.size() / 5);
        for (const Mesh::Batch& b : t.mesh.dBatches)
        {
            IDirect3DTexture9* tex = nullptr;
            if (!b.tex.empty())
            {
                auto it = g_texs.find(b.tex);
                if (it == g_texs.end() || !it->second.tex)
                    continue;   // not loaded yet, or missing: an uncut leaf card would be a solid square
                tex = it->second.tex;
            }
            d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(tex));
            d->SetRenderState(dev, D3DRS_ALPHATESTENABLE, tex ? TRUE : FALSE);
            d->DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, nv, b.start, b.count / 3);
            ++drawn;
        }
    }
    if (set && leaves)
    {
        for (int i = 0; i < 5; ++i)
            d->SetSamplerState(dev, 0, kSamp[i], saved[i]);
        d->SetTexture(dev, 0, nullptr);
    }
    g_dDrawnLast[leaves ? 1 : 0] = drawn;
    return drawn;
}

bool MapBuildingNearest(const float from[3], float pos[3], float rot[3][3], char* name, int size)
{
    const Inst* best = nullptr;
    float bestD = 1e30f;
    for (const Inst& i : g_insts)
    {
        if (i.m->state != Model::kReady)
            continue;
        const float dx = i.p->pos[0] - from[0], dy = i.p->pos[1] - from[1];
        if (dx * dx + dy * dy < bestD)
        {
            bestD = dx * dx + dy * dy;
            best = &i;
        }
    }
    if (!best)
        return false;
    memcpy(pos, best->p->pos, 12);
    memcpy(rot, best->p->rot, 36);
    _snprintf_s(name, size, _TRUNCATE, "%s", best->p->name.c_str());
    return true;
}

unsigned MapTerrainDraw(IDirect3DDevice9* dev, const D3DMATRIX& m, const float cam[3])
{
    auto* d = dev->lpVtbl;
    unsigned drawn = 0;
    bool set = false;
    for (auto& kv : g_tiles)
    {
        const Tile& t = kv.second;
        if (!t.vb || !t.ib)
            continue;
        const float x1 = CornerX(t.mesh.b), y1 = CornerY(t.mesh.a);
        const float lo[3] = { x1 - kTile, y1 - kTile, t.mesh.minZ }, hi[3] = { x1, y1, t.mesh.maxZ };
        if (Outside(m, lo, hi))
            continue;
        if (!set)
        {
            d->SetVertexShader(dev, nullptr);
            d->SetFVF(dev, D3DFVF_XYZ);
            d->SetTexture(dev, 0, nullptr);
            d->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
            set = true;
        }
        D3DMATRIX w = {};
        w.m[0][0] = w.m[1][1] = w.m[2][2] = w.m[3][3] = 1.0f;
        w.m[3][0] = x1 - cam[0];
        w.m[3][1] = y1 - cam[1];
        w.m[3][2] = -cam[2];
        d->SetTransform(dev, D3DTS_WORLD, &w);
        d->SetStreamSource(dev, 0, t.vb, 0, 12);
        d->SetIndices(dev, t.ib);
        d->DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, static_cast<UINT>(t.mesh.v.size() / 3), 0,
                                static_cast<UINT>(t.mesh.idx.size() / 3));
        ++drawn;
    }
    g_drawnLast = drawn;
    return drawn;
}

unsigned MapBuildingsDraw(IDirect3DDevice9* dev, const D3DMATRIX& m, const float cam[3])
{
    auto* d = dev->lpVtbl;
    unsigned drawn = 0;
    bool set = false;
    for (const Inst& i : g_insts)
    {
        const Model& mo = *i.m;
        if (mo.state != Model::kReady || !mo.vb || !mo.ib)
            continue;
        const Placement& p = *i.p;
        if (Outside(m, i.lo, i.hi))
            continue;
        if (!set)
        {
            d->SetVertexShader(dev, nullptr);
            d->SetFVF(dev, D3DFVF_XYZ);
            d->SetTexture(dev, 0, nullptr);
            d->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
            set = true;
        }
        D3DMATRIX w = {};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                w.m[r][c] = p.rot[r][c];
        w.m[3][0] = p.pos[0] - cam[0];
        w.m[3][1] = p.pos[1] - cam[1];
        w.m[3][2] = p.pos[2] - cam[2];
        w.m[3][3] = 1.0f;
        d->SetTransform(dev, D3DTS_WORLD, &w);
        d->SetStreamSource(dev, 0, mo.vb, 0, 12);
        d->SetIndices(dev, mo.ib);
        d->DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, mo.nv, 0, mo.ntri);
        ++drawn;
    }
    g_wmoDrawnLast = drawn;
    return drawn;
}

void MapTerrainRelease()
{
    for (auto& kv : g_tiles)
        DropGpu(kv.second);
    for (auto& kv : g_models)
        DropGpu(kv.second);
    for (auto& kv : g_texs)
        DropGpu(kv.second);
    g_coverGrid.clear();
    g_doodadGrid.clear();
    g_dev = nullptr;
}

const char* MapTerrainInfo()
{
    unsigned ready = 0, pending = 0, empty = 0, ground = 0;
    for (const auto& kv : g_tiles)
    {
        (kv.second.pending ? pending : kv.second.vb ? ready : empty)++;
        ground += !kv.second.pending && kv.second.mesh.groundOnly;
    }
    unsigned mReady = 0, mLoading = 0, mFailed = 0, instReady = 0;
    for (const auto& kv : g_models)
        (kv.second.state == Model::kReady ? mReady : kv.second.state == Model::kFailed ? mFailed : mLoading)++;
    for (const Inst& i : g_insts)
        instReady += i.m->state == Model::kReady;
    unsigned doodads = 0, missing = 0, dTiles = 0, settled = 0, tReady = 0, tFailed = 0, tLoading = 0;
    unsigned long long dTris = 0;
    for (const auto& kv : g_tiles)
    {
        const Mesh& me = kv.second.mesh;
        doodads += me.doodads;
        missing += me.doodadsMissing;
        dTris += (me.dSolidIdx.size() + me.dLeafIdx.size()) / 3;
        dTiles += kv.second.dOnGpu;
        settled += DoodadsSettled(kv.second);
    }
    for (const auto& kv : g_texs)
        (kv.second.state == Tex::kReady ? tReady : kv.second.state == Tex::kFailed ? tFailed : tLoading)++;
    const unsigned done = g_loadedTotal + g_missingTotal, wDone = g_wmoRead + g_wmoFailed;
    _snprintf_s(g_info, sizeof(g_info), _TRUNCATE,
                "map terrain: map \"%s\", %u archives; tiles in reach: %u ready (%u of them the ground alone), %u loading, %u without ground; "
                "%u drawn into the last map; since the start %u read, %u not found, %.0f ms a tile. Buildings: %u "
                "placed (%u ready), %u models (%u ready, %u loading, %u failed), %u drawn into the last map; "
                "%.0f ms a model. Doodads: %u (%u models unreadable), %llu triangles, on the GPU for %u tiles (%u "
                "settled), %.0f ms a tile to build; leaf textures %u ready, %u loading, %u failed; draws into the "
                "last map %u solid, %u leaf. Lights from the buildings (candles, lanterns, fires): %u; game "
                "objects %u, %u of them lit, %u lights, %u display ids read",
                g_map.c_str(), MpqArchiveCount(), ready, ground, pending, empty, g_drawnLast, g_loadedTotal, g_missingTotal,
                done ? g_loadMs / done : 0.0, static_cast<unsigned>(g_insts.size()), instReady,
                static_cast<unsigned>(g_models.size()), mReady, mLoading, mFailed, g_wmoDrawnLast,
                wDone ? g_wmoMs / wDone : 0.0, doodads, missing, dTris, dTiles, settled,
                g_dTiles ? g_dMs / g_dTiles : 0.0, tReady, tLoading, tFailed, g_dDrawnLast[0], g_dDrawnLast[1],
                static_cast<unsigned>(g_fileLights.size()), g_objSeen, g_objLit,
                static_cast<unsigned>(g_objLights.size()), static_cast<unsigned>(g_objectLights.size()));
    return g_info;
}
