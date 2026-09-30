// mapterrain: the ground read from the client's map files, for the shadow map.
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
//   Loading    On a thread of its own, nearest tile first, so no frame waits on a file: about 2 MB of
//              zlib a tile. The meshes stay in memory while their tile is in reach (0.8 MB each), and each
//              goes to the GPU as one vertex and one index buffer in the managed pool, which lives
//              through a device Reset.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "common.h"
#include "mapterrain.h"
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
#include <vector>

namespace
{
    constexpr float kTile  = 1600.0f / 3.0f;     // 533.33 yards
    constexpr float kChunk = kTile / 16.0f;
    constexpr float kUnit  = kChunk / 8.0f;

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    struct Mesh
    {
        int                   a = 0, b = 0;
        unsigned              gen = 0;
        bool                  found = false;   // the file exists and was read
        std::vector<float>    v;               // x y z, x and y relative to the tile's corner
        std::vector<uint16_t> idx;
        float                 minZ = 0.0f, maxZ = 0.0f;
        double                ms = 0.0;        // time to read and build
    };

    struct Tile
    {
        bool                    pending = true;
        Mesh                    mesh;
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9*  ib = nullptr;
    };

    // The corner of tile (a, b): the largest x and y it covers.
    float CornerX(int b) { return (32.0f - b) * kTile; }
    float CornerY(int a) { return (32.0f - a) * kTile; }

    uint32_t U32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; }

    // One tile's file into a mesh. False if the file is not a tile this reader understands.
    bool Build(const std::vector<uint8_t>& d, Mesh& m)
    {
        const float ox = CornerX(m.b), oy = CornerY(m.a);
        m.v.clear();
        m.idx.clear();
        m.minZ = 1e9f;
        m.maxZ = -1e9f;
        unsigned chunks = 0;
        for (size_t o = 0; o + 8 <= d.size();)
        {
            const uint32_t tag = U32(d, o), size = U32(d, o + 4);
            if (o + 8 + static_cast<size_t>(size) > d.size())
                break;
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
        return chunks > 0;
    }

    // --- the loader thread ---------------------------------------------------------------------------

    struct Job { std::string map; int a, b; unsigned gen; };

    std::mutex              g_mx;
    std::condition_variable g_cv;
    std::deque<Job>         g_jobs;
    std::deque<Mesh>        g_done;
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
            Mesh m;
            m.a = job.a;
            m.b = job.b;
            m.gen = job.gen;
            const double t0 = Now();
            if (open)
            {
                char name[160];
                _snprintf_s(name, sizeof(name), _TRUNCATE, "World\\Maps\\%s\\%s_%d_%d.adt", job.map.c_str(),
                            job.map.c_str(), job.a, job.b);
                if (MpqRead(name, file))
                    m.found = Build(file, m);
                if (!m.found)
                {
                    m.v.clear();
                    m.idx.clear();
                }
            }
            m.ms = 1000.0 * (Now() - t0);
            std::lock_guard<std::mutex> lock(g_mx);
            g_done.push_back(std::move(m));
        }
    }

    // --- the render thread's side --------------------------------------------------------------------

    std::unordered_map<int, Tile> g_tiles;
    std::string                   g_map;
    IDirect3DDevice9*             g_dev = nullptr;
    unsigned                      g_drawnLast = 0, g_loadedTotal = 0, g_missingTotal = 0;
    double                        g_loadMs = 0.0;
    char                          g_info[400] = {};

    int Key(int a, int b) { return (a << 8) | (b & 0xFF); }

    void DropGpu(Tile& t)
    {
        SafeRelease(t.vb);
        SafeRelease(t.ib);
    }

    void DropAll()
    {
        for (auto& kv : g_tiles)
            DropGpu(kv.second);
        g_tiles.clear();
        std::lock_guard<std::mutex> lock(g_mx);
        g_jobs.clear();
        ++g_gen;
    }

    bool Upload(IDirect3DDevice9* dev, Tile& t)
    {
        auto* d = dev->lpVtbl;
        const UINT vBytes = static_cast<UINT>(t.mesh.v.size() * sizeof(float));
        const UINT iBytes = static_cast<UINT>(t.mesh.idx.size() * sizeof(uint16_t));
        void* p = nullptr;
        if (FAILED(d->CreateVertexBuffer(dev, vBytes, D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_MANAGED, &t.vb, nullptr)) ||
            FAILED(t.vb->lpVtbl->Lock(t.vb, 0, 0, &p, 0)))
        {
            DropGpu(t);
            return false;
        }
        memcpy(p, t.mesh.v.data(), vBytes);
        t.vb->lpVtbl->Unlock(t.vb);
        if (FAILED(d->CreateIndexBuffer(dev, iBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &t.ib, nullptr)) ||
            FAILED(t.ib->lpVtbl->Lock(t.ib, 0, 0, &p, 0)))
        {
            DropGpu(t);
            return false;
        }
        memcpy(p, t.mesh.idx.data(), iBytes);
        t.ib->lpVtbl->Unlock(t.ib);
        return true;
    }

    // Across the ground from (x, y) to tile (a, b)'s square.
    float Distance(float x, float y, int a, int b)
    {
        const float x1 = CornerX(b), y1 = CornerY(a);
        const float dx = x > x1 ? x - x1 : x < x1 - kTile ? x1 - kTile - x : 0.0f;
        const float dy = y > y1 ? y - y1 : y < y1 - kTile ? y1 - kTile - y : 0.0f;
        return sqrtf(dx * dx + dy * dy);
    }
}

void MapTerrainUpdate(IDirect3DDevice9* dev, const float player[3], float reach)
{
    if (reach <= 0.0f)
    {
        if (!g_tiles.empty())
            DropAll();
        g_map.clear();
        return;
    }
    if (dev != g_dev)
    {
        for (auto& kv : g_tiles)
            DropGpu(kv.second);
        g_dev = dev;
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
            g_jobs.push_back({ g_map, q.second >> 8, q.second & 0xFF, g_gen });
        }
        g_cv.notify_one();
    }
    for (auto it = g_tiles.begin(); it != g_tiles.end();)
    {
        if (Distance(x, y, it->first >> 8, it->first & 0xFF) > reach + 400.0f)
        {
            DropGpu(it->second);
            it = g_tiles.erase(it);   // a pending one's result is thrown away when it comes
        }
        else
            ++it;
    }

    // Up to two tiles to the GPU a frame: each is about 0.8 MB.
    int uploads = 0;
    for (auto& kv : g_tiles)
    {
        Tile& t = kv.second;
        if (t.pending || !t.mesh.found || t.mesh.idx.empty() || t.vb || uploads >= 2)
            continue;
        if (!Upload(dev, t))
        {
            Log("map terrain: could not make the buffers for tile %d_%d", t.mesh.a, t.mesh.b);
            t.mesh.found = false;   // the client's own draws cast there instead
            continue;
        }
        ++uploads;
    }
}

bool MapTerrainCovers(float x, float y)
{
    if (g_tiles.empty())
        return false;
    const int a = static_cast<int>(floorf(32.0f - y / kTile)), b = static_cast<int>(floorf(32.0f - x / kTile));
    auto it = g_tiles.find(Key(a, b));
    return it != g_tiles.end() && it->second.vb != nullptr;
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
        // Off the map entirely: every corner of its box past the same side of the map's clip volume.
        int out[6] = {};
        for (int i = 0; i < 8; ++i)
        {
            const float p[3] = { (i & 1) ? x1 : x1 - kTile, (i & 2) ? y1 : y1 - kTile, (i & 4) ? t.mesh.maxZ : t.mesh.minZ };
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
        if (out[0] == 8 || out[1] == 8 || out[2] == 8 || out[3] == 8 || out[4] == 8 || out[5] == 8)
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

void MapTerrainRelease()
{
    for (auto& kv : g_tiles)
        DropGpu(kv.second);
    g_dev = nullptr;
}

const char* MapTerrainInfo()
{
    unsigned ready = 0, pending = 0, empty = 0;
    for (const auto& kv : g_tiles)
        (kv.second.pending ? pending : kv.second.vb ? ready : empty)++;
    const unsigned done = g_loadedTotal + g_missingTotal;
    _snprintf_s(g_info, sizeof(g_info), _TRUNCATE,
                "map terrain: map \"%s\", %u archives; tiles in reach: %u ready, %u loading, %u without ground; "
                "%u drawn into the last map; since the start %u read, %u not found, %.0f ms a tile",
                g_map.c_str(), MpqArchiveCount(), ready, pending, empty, g_drawnLast, g_loadedTotal, g_missingTotal,
                done ? g_loadMs / done : 0.0);
    return g_info;
}
