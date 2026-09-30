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
//   Holes      A 4 x 4 mask over the 8 x 8 cells, where a cave mouth or a building's cellar goes into the
//              ground: bit (row / 2) * 4 + (column / 2). A cell in a hole is left out.
//   Buildings  (added 2026-09-30) Each tile places its WMOs in MODF, 64 bytes each: the name's index
//              (through MWID into MWMO), a unique id, a position, a rotation in degrees, and a box. A
//              building crossing tiles is placed by each of them under the same id, and drawn once. The
//              placement is the core's vmap maths (tools/vmap_extractor, src/game/vmap/ModelInstance.cpp):
//              world = (17066.67 - p.z, 17066.67 - p.x, p.y), turned by Rz(r.y) Ry(r.x) Rx(r.z) and then a
//              half turn about the vertical. mapwmo.cpp reads the building; the client's own draw of it
//              (fixed-function, its place the placement's) is then left out, except its alpha-keyed parts.
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
#include "mapwmo.h"
#include "mpq.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
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
        std::vector<float>     v;               // x y z, x and y relative to the tile's corner
        std::vector<uint16_t>  idx;
        float                  minZ = 0.0f, maxZ = 0.0f;
        std::vector<Placement> wmos;
        double                 ms = 0.0;        // time to read and build
    };

    struct Tile
    {
        bool                    pending = true;
        Mesh                    mesh;
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9*  ib = nullptr;
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
            if (!p.name.empty())
                out.push_back(std::move(p));
        }
    }

    // One tile's file into a mesh. False if the file is not a tile this reader understands.
    bool Build(const std::vector<uint8_t>& d, Mesh& m)
    {
        const float ox = CornerX(m.b), oy = CornerY(m.a);
        m.v.clear();
        m.idx.clear();
        m.wmos.clear();
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

    struct Job { bool building; std::string name; int a, b; unsigned gen; };   // name: the map, or the WMO
    struct Loaded { std::string name; unsigned gen; bool ok; WmoMesh mesh; double ms; };

    std::mutex              g_mx;
    std::condition_variable g_cv;
    std::deque<Job>         g_jobs;
    std::deque<Mesh>        g_done;
    std::deque<Loaded>      g_doneWmo;
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
            if (job.building)
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
            if (open)
            {
                char name[160];
                _snprintf_s(name, sizeof(name), _TRUNCATE, "World\\Maps\\%s\\%s_%d_%d.adt", job.name.c_str(),
                            job.name.c_str(), job.a, job.b);
                if (MpqRead(name, file))
                    m.found = Build(file, m);
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
    char                          g_info[600] = {};

    int Key(int a, int b) { return (a << 8) | (b & 0xFF); }
    long long CellKey(long long cx, long long cy) { return (cx << 32) ^ (cy & 0xFFFFFFFFll); }

    void DropGpu(Tile& t)
    {
        SafeRelease(t.vb);
        SafeRelease(t.ib);
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
        g_insts.clear();
        g_coverGrid.clear();
        g_instDirty = true;
        std::lock_guard<std::mutex> lock(g_mx);
        g_jobs.clear();
        ++g_gen;
    }

    template <typename VB, typename IB>
    bool Upload(IDirect3DDevice9* dev, const std::vector<float>& v, const void* idx, UINT idxBytes, D3DFORMAT fmt,
                VB*& vb, IB*& ib)
    {
        auto* d = dev->lpVtbl;
        const UINT vBytes = static_cast<UINT>(v.size() * sizeof(float));
        void* p = nullptr;
        if (FAILED(d->CreateVertexBuffer(dev, vBytes, D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_MANAGED, &vb, nullptr)) ||
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

void MapTerrainUpdate(IDirect3DDevice9* dev, const float player[3], float reach)
{
    if (reach <= 0.0f)
    {
        if (!g_tiles.empty() || !g_models.empty())
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
            if (it == g_tiles.end() || !it->second.pending)
                continue;
            (m.found ? g_loadedTotal : g_missingTotal)++;
            g_loadMs += m.ms;
            it->second.mesh = std::move(m);
            it->second.pending = false;
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
    }

    // Ask for the tiles in reach, nearest first; drop those well out of it.
    const float x = player[0], y = player[1];
    const int a0 = static_cast<int>(floorf(32.0f - (y + reach) / kTile)), a1 = static_cast<int>(floorf(32.0f - (y - reach) / kTile));
    const int b0 = static_cast<int>(floorf(32.0f - (x + reach) / kTile)), b1 = static_cast<int>(floorf(32.0f - (x - reach) / kTile));
    std::vector<std::pair<float, int>> ask;
    for (int a = (std::max)(a0, 0); a <= (std::min)(a1, 63); ++a)
        for (int b = (std::max)(b0, 0); b <= (std::min)(b1, 63); ++b)
        {
            const float dist = Distance(x, y, a, b);
            if (dist <= reach && !g_tiles.count(Key(a, b)))
                ask.push_back({ dist, Key(a, b) });
        }
    if (!ask.empty())
    {
        std::sort(ask.begin(), ask.end());
        std::lock_guard<std::mutex> lock(g_mx);
        for (const auto& q : ask)
        {
            g_tiles[q.second];   // pending
            g_jobs.push_back({ false, g_map, q.second >> 8, q.second & 0xFF, g_gen });
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
                g_jobs.push_back({ true, w.second->p->name, 0, 0, g_gen });
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
            memcpy(m.hi, m.mesh.hi, 12);
            m.mesh  = WmoMesh();   // on the GPU now; read again after a device change
            coverDirty = true;
        }
        break;
    }
    if (coverDirty)
        RebuildCover();
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
    g_coverGrid.clear();
    g_dev = nullptr;
}

const char* MapTerrainInfo()
{
    unsigned ready = 0, pending = 0, empty = 0;
    for (const auto& kv : g_tiles)
        (kv.second.pending ? pending : kv.second.vb ? ready : empty)++;
    unsigned mReady = 0, mLoading = 0, mFailed = 0, instReady = 0;
    for (const auto& kv : g_models)
        (kv.second.state == Model::kReady ? mReady : kv.second.state == Model::kFailed ? mFailed : mLoading)++;
    for (const Inst& i : g_insts)
        instReady += i.m->state == Model::kReady;
    const unsigned done = g_loadedTotal + g_missingTotal, wDone = g_wmoRead + g_wmoFailed;
    _snprintf_s(g_info, sizeof(g_info), _TRUNCATE,
                "map terrain: map \"%s\", %u archives; tiles in reach: %u ready, %u loading, %u without ground; "
                "%u drawn into the last map; since the start %u read, %u not found, %.0f ms a tile. Buildings: %u "
                "placed (%u ready), %u models (%u ready, %u loading, %u failed), %u drawn into the last map; "
                "%.0f ms a model",
                g_map.c_str(), MpqArchiveCount(), ready, pending, empty, g_drawnLast, g_loadedTotal, g_missingTotal,
                done ? g_loadMs / done : 0.0, static_cast<unsigned>(g_insts.size()), instReady,
                static_cast<unsigned>(g_models.size()), mReady, mLoading, mFailed, g_wmoDrawnLast,
                wDone ? g_wmoMs / wDone : 0.0);
    return g_info;
}
