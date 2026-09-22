// shadow: a depth map of the world as the sun sees it, kept by a cache of casters.
//
// Volumetric light asks, for any point in the air: does the sun reach it? That needs the scene's depth
// from the sun's side, and the client never renders one. So its opaque world draws are recorded as they
// happen and replayed from the sun into a depth texture over a square around the player.
//
// The client only draws what is in the camera's view, so a map made from one frame's draws loses a tree
// the moment it leaves the screen, and the air it shaded flashes bright. Hence a cache: every recorded
// draw becomes an entry, kept across frames in ABSOLUTE world coordinates, and the map is drawn from the
// whole cache every frame.
//
//   Recording   Between the end of the sky and the end of the world: every draw that writes depth with
//               blending off: terrain, buildings, trees, characters. Each record keeps its buffers,
//               shader, declaration, texture, alpha test and world matrix, holding a reference to each.
//               Shader draws also keep a snapshot of all 256 vertex-shader constants (bones, c2..c5 and
//               the rest), from a mirror kept current by every client upload. Dynamic vertex buffers are
//               skipped: the client re-fills them mid-frame (the grass arena is one, see comfygrass).
//   Absolute    The client draws camera-relative. Fixed-function entries keep their world matrix with the
//               camera's position added back. Shader entries keep A = M * inverse(camera view-proj) * T(cam)
//               (their own transform with the camera taken out), which is affine whether the client
//               folds the world matrix into c2..c5 or into the bones (both checked by the probe).
//   Identity    What is drawn (buffers, shader, index range), and among the instances of that, the nearest
//               within matchRadius yards. A first version keyed on position to a quarter of a yard, and
//               trees, whose reference point is a swaying root bone, drifted across those boundaries
//               and were re-added every few frames (472 new entries a frame, a cache of 2500 duplicates).
//               Matching by nearness keeps a swaying tree one entry, and lets a walking character's entry
//               move with it instead of leaving a trail. Each entry matches at most once a frame.
//   Eviction    An entry is gone if it was NOT drawn this frame although it sits in view and near: a
//               character that walked off, a mesh that switched level of detail. Out of view it stays,
//               for up to cacheTime seconds: the tree over your head keeps shading you after you look away
//               from it. There is deliberately no "too far from the player" rule: a model's reference
//               point is its first bone, which for some models sits far from the geometry (measured:
//               trees inside the map reading 85-95 yards away). So a step across that limit dropped a
//               nearby tree from the map and the air it shaded lit up. Far entries cost a draw that the
//               map clips; the cap bounds them.
//   Replay      Into a 2048x2048 INTZ depth texture (readable, like depth.cpp's), colour writes off, no
//               culling (leaves are two-sided), alpha test kept so foliage casts leaf-shaped shadows.
//               Each entry is put back relative to the current camera: fixed-function under the sun's view
//               and projection, shader draws with c2..c5 = A * T(-camera) * sunViewProj.
//
// Two things the client does that this has to allow for, both measured: it draws the world into depth
// slice 0.0..0.94 of the buffer, and it draws the sky and the far horizon with cameras of their own. So the
// world's depth slice and camera are taken by vote over the frame's depth-writing draws.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "common.h"
#include "config.h"
#include "rays.h"
#include "shadow.h"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    const D3DFORMAT kINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    const D3DFORMAT kNULL = static_cast<D3DFORMAT>(MAKEFOURCC('N', 'U', 'L', 'L'));

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    // ---------------------------------------------------------------------------------------------
    // matrices (D3D9: row vectors, v' = v * M)

    void Mul(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX& out)
    {
        D3DMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        out = r;
    }

    // General 4x4 inverse, in double: the camera's view-projection has a near plane of 0.1 yards.
    bool Invert(const D3DMATRIX& src, D3DMATRIX& out)
    {
        double a[4][8];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j)
                a[i][j] = j < 4 ? src.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c)
        {
            int p = c;
            for (int r = c + 1; r < 4; ++r)
                if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
            if (fabs(a[p][c]) < 1e-12)
                return false;
            if (p != c)
                for (int j = 0; j < 8; ++j) { const double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
            const double inv = 1.0 / a[c][c];
            for (int j = 0; j < 8; ++j) a[c][j] *= inv;
            for (int r = 0; r < 4; ++r)
                if (r != c && a[r][c] != 0.0)
                {
                    const double f = a[r][c];
                    for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
                }
        }
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out.m[i][j] = static_cast<float>(a[i][j + 4]);
        return true;
    }

    void Translation(float x, float y, float z, D3DMATRIX& out)
    {
        out = {};
        out.m[0][0] = out.m[1][1] = out.m[2][2] = out.m[3][3] = 1.0f;
        out.m[3][0] = x; out.m[3][1] = y; out.m[3][2] = z;
    }

    // Left-handed look-at and orthographic projection, D3DX's conventions.
    void LookAtLH(const float eye[3], const float at[3], const float up[3], D3DMATRIX& out)
    {
        float z[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
        float zl = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
        for (float& v : z) v /= zl;
        float x[3] = { up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0] };
        float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        for (float& v : x) v /= xl;
        const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
        out = {};
        out.m[0][0] = x[0]; out.m[0][1] = y[0]; out.m[0][2] = z[0];
        out.m[1][0] = x[1]; out.m[1][1] = y[1]; out.m[1][2] = z[1];
        out.m[2][0] = x[2]; out.m[2][1] = y[2]; out.m[2][2] = z[2];
        out.m[3][0] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
        out.m[3][1] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
        out.m[3][2] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
        out.m[3][3] = 1.0f;
    }

    void OrthoLH(float w, float h, float zn, float zf, D3DMATRIX& out)
    {
        out = {};
        out.m[0][0] = 2.0f / w;
        out.m[1][1] = 2.0f / h;
        out.m[2][2] = 1.0f / (zf - zn);
        out.m[3][2] = zn / (zn - zf);
        out.m[3][3] = 1.0f;
    }

    // c2..c5 as the shader reads them (each register one column) <-> the matrix.
    void FromRegisters(const float* c, D3DMATRIX& m)
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                m.m[row][col] = c[col * 4 + row];
    }
    void ToRegisters(const D3DMATRIX& m, float* c)
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                c[col * 4 + row] = m.m[row][col];
    }

    // ---------------------------------------------------------------------------------------------
    // one frame's recording

    struct Rec
    {
        bool                         indexed;
        D3DPRIMITIVETYPE             prim;
        INT                          baseVertex;
        UINT                         minIndex, numVertices, startIndex, primCount;
        IDirect3DVertexShader9*      vs;
        IDirect3DVertexDeclaration9* decl;
        DWORD                        fvf;
        IDirect3DVertexBuffer9*      vb[2];
        UINT                         vbOffset[2], vbStride[2];
        IDirect3DIndexBuffer9*       ib;
        IDirect3DBaseTexture9*       tex0;
        DWORD                        alphaTest, alphaRef, alphaFunc;
        D3DMATRIX                    world;       // fixed-function: camera-relative, as drawn
        size_t                       consts;      // shader draws: offset of the snapshot in g_constPool
        uint64_t                     seq;         // writes seen when this draw was recorded
        float                        minZ, maxZ;  // the viewport's depth slice, to tell world draws apart
        bool                         hasProj;     // fixed-function: the projection it was drawn with
        float                        proj00, proj22, proj32;
    };

    std::vector<Rec>   g_frame;
    std::vector<float> g_constPool;
    float              g_mirror[256 * 4];   // the vertex-shader constants as the client last set them
    bool               g_mirrorValid = false;
    bool               g_recording   = false;
    constexpr size_t   kMaxFrame     = 6000;

    // Why world draws were not recorded, this frame.
    UINT  g_rejZ = 0, g_rejZW = 0, g_rejBlend = 0, g_rejNoVB = 0, g_rejDynamic = 0, g_seen = 0;

    void ReleaseRec(Rec& r)
    {
        SafeRelease(r.vs);
        SafeRelease(r.decl);
        SafeRelease(r.vb[0]);
        SafeRelease(r.vb[1]);
        SafeRelease(r.ib);
        SafeRelease(r.tex0);
    }

    void ReleaseFrame()
    {
        for (Rec& r : g_frame)
            ReleaseRec(r);
        g_frame.clear();
        g_constPool.clear();
        g_rejZ = g_rejZW = g_rejBlend = g_rejNoVB = g_rejDynamic = g_seen = 0;
    }

    // ---------------------------------------------------------------------------------------------
    // the world's depth slice and camera, by vote

    struct Slice   { float minZ, maxZ; UINT votes; };
    struct CamVote { D3DMATRIX view, proj; UINT votes; float minZ, maxZ; };
    Slice     g_slices[8];
    int       g_sliceCount = 0;
    CamVote   g_cams[8];
    int       g_camCount = 0;
    float     g_worldMinZ = 0.0f, g_worldMaxZ = 1.0f;
    D3DMATRIX g_worldView = {}, g_worldProj = {};
    bool      g_haveWorldCam = false;

    void VoteSlice(float minZ, float maxZ)
    {
        for (int i = 0; i < g_sliceCount; ++i)
            if (g_slices[i].minZ == minZ && g_slices[i].maxZ == maxZ) { ++g_slices[i].votes; return; }
        if (g_sliceCount < 8)
            g_slices[g_sliceCount++] = { minZ, maxZ, 1 };
    }

    void VoteCamera(const D3DMATRIX& view, const D3DMATRIX& proj, float minZ, float maxZ)
    {
        for (int i = 0; i < g_camCount; ++i)
            if (g_cams[i].proj.m[2][2] == proj.m[2][2] && g_cams[i].proj.m[3][2] == proj.m[3][2] &&
                g_cams[i].proj.m[0][0] == proj.m[0][0])
            {
                ++g_cams[i].votes;
                g_cams[i].view = view;
                return;
            }
        if (g_camCount < 8)
            g_cams[g_camCount++] = { view, proj, 1, minZ, maxZ };
    }

    bool SameCamera(const D3DMATRIX& a, const D3DMATRIX& b)
    {
        return a.m[0][0] == b.m[0][0] && a.m[2][2] == b.m[2][2] && a.m[3][2] == b.m[3][2];
    }

    void SettleVotes(bool log)
    {
        if (g_sliceCount)
        {
            const Slice* best = &g_slices[0];
            for (int i = 1; i < g_sliceCount; ++i)
                if (g_slices[i].votes > best->votes) best = &g_slices[i];
            g_worldMinZ = best->minZ;
            g_worldMaxZ = best->maxZ;
            if (log)
                for (int i = 0; i < g_sliceCount; ++i)
                    Log("shadow: depth slice %.4f..%.4f used by %u draws%s", g_slices[i].minZ, g_slices[i].maxZ,
                        g_slices[i].votes, &g_slices[i] == best ? "  <-- the world" : "");
        }
        if (g_camCount)
        {
            // Only cameras used inside the world's own depth slice can be the world's: the far horizon
            // draws with a camera of its own (near 467, far 2112 here) and its own slice. Letting it win
            // a frame's vote re-expressed every cached caster through the wrong camera, which filled the
            // shadow map with misplaced casters and switched the volumetric light off for that frame.
            const CamVote* best = nullptr;
            for (int i = 0; i < g_camCount; ++i)
            {
                if (g_cams[i].minZ != g_worldMinZ || g_cams[i].maxZ != g_worldMaxZ)
                    continue;
                if (!best || g_cams[i].votes > best->votes)
                    best = &g_cams[i];
            }
            // And a frame where the world draws thin out must not flip it either: the camera in use stays
            // unless another takes twice its votes.
            if (best && g_haveWorldCam)
                for (int i = 0; i < g_camCount; ++i)
                    if (SameCamera(g_cams[i].proj, g_worldProj) && g_cams[i].votes * 2 >= best->votes)
                    {
                        best = &g_cams[i];
                        break;
                    }
            if (best)
            {
            g_worldView = best->view;
            g_worldProj = best->proj;
            g_haveWorldCam = true;
            }
            if (log)
                for (int i = 0; i < g_camCount; ++i)
                {
                    const float n = -g_cams[i].proj.m[3][2] / g_cams[i].proj.m[2][2];
                    Log("shadow: camera near %.3f / far %.1f used by %u draws in slice %.4f..%.4f%s", n,
                        g_cams[i].proj.m[2][2] * n / (g_cams[i].proj.m[2][2] - 1.0f), g_cams[i].votes,
                        g_cams[i].minZ, g_cams[i].maxZ, &g_cams[i] == best ? "  <-- the world" : "");
                }
        }
        g_sliceCount = 0;
        g_camCount   = 0;
    }

    // ---------------------------------------------------------------------------------------------
    // the cache

    // What is drawn, without where: instances of the same model share it.
    struct Key
    {
        const void* vb; const void* ib; const void* vs;
        INT  baseVertex;
        UINT minIndex, numVertices, startIndex, primCount;
        bool operator==(const Key& o) const
        {
            return vb == o.vb && ib == o.ib && vs == o.vs && baseVertex == o.baseVertex && minIndex == o.minIndex &&
                   numVertices == o.numVertices && startIndex == o.startIndex && primCount == o.primCount;
        }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const
        {
            size_t h = reinterpret_cast<size_t>(k.vb) * 0x9E3779B1u;
            auto mix = [&h](size_t v) { h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2); };
            mix(reinterpret_cast<size_t>(k.ib)); mix(reinterpret_cast<size_t>(k.vs));
            mix(static_cast<size_t>(k.baseVertex)); mix(k.startIndex); mix(k.primCount);
            return h;
        }
    };

    struct Entry
    {
        Rec                rec;            // references owned by the entry
        D3DMATRIX          absolute;       // fixed-function: world, absolute; shader: A, bones -> absolute
        std::vector<float> consts;         // shader draws: the constants snapshot
        bool               mobile;         // matched by the moving rule: it goes when it stops being drawn
        uint64_t           seq;            // writes seen when its geometry was last recorded
        float              pos[3];         // absolute position of its reference point
        double             lastSeen;       // == now once matched this frame
    };

    // Each key holds the instances of that model, wherever they stand.
    std::unordered_map<Key, std::vector<Entry>, KeyHash> g_cache;
    size_t           g_entries = 0;
    constexpr size_t kMaxCache = 5000;
    constexpr float  kMatchRadius = 3.0f;   // yards a recorded draw may be from an instance and still be it
    // Anything that moves faster than kMatchRadius a frame looked like a new object every frame: a bird
    // flying by at 3.5 yards a frame left a new caster in the map each time, a trail of birds that all
    // shaded the air until they aged out. So when nothing is near enough, an instance of the same model
    // that the client did NOT draw this frame, within this much, is taken to be it, moved.
    constexpr float  kMoveRadius = 60.0f;

    // Refreshed / added / evicted this frame, for the probe.
    UINT g_nRefreshed = 0, g_nAdded = 0, g_nEvictView = 0, g_nEvictAge = 0, g_nEvictCap = 0;

    void ClearCache()
    {
        for (auto& kv : g_cache)
            for (Entry& e : kv.second)
                ReleaseRec(e.rec);
        g_cache.clear();
        g_entries = 0;
    }

    // Buffers the client writes to. An entry is a pointer into a buffer plus an index range, not a copy
    // of the geometry, and the client re-fills some of its buffers as visibility changes: measured, 62 of
    // the cached entries had their vertices overwritten during one 180-frame trace, nearly all of them
    // while the client was NOT drawing them. Replaying those drew whatever had taken their place, which
    // is where the spikes fanning out of a building came from, and turning around filled the map with
    // them. Entries from such a buffer are kept only for as long as the client keeps drawing them.
    // The client also re-fills a buffer LATER IN THE SAME FRAME, after the draw we recorded from it and
    // before the replay at the end of the world: the abbey's geometry was redrawn every frame, so it was
    // never stale, yet a triangle of whatever came next covered it in the map. So each write is numbered,
    // each record keeps the number it saw, and an entry whose buffer has been written since is gone.
    // Whole buffers are too blunt: these arenas hold many objects, and a write to one of them would
    // drop every other object in the same buffer, which took the abbey out of the map altogether. So each
    // write keeps its byte range, and an entry is gone only once a later write lands on its own vertices.
    struct Write { UINT start, end; uint64_t seq; };
    struct BufferWrites { Write ring[16]; int next = 0; uint64_t last = 0; };
    std::unordered_map<const void*, BufferWrites> g_written;
    uint64_t g_writeSeq = 0;
    unsigned g_nEvictWritten = 0;

    bool WrittenOver(const void* b, UINT start, UINT bytes, uint64_t seq)
    {
        if (!b)
            return false;
        auto it = g_written.find(b);
        if (it == g_written.end() || it->second.last <= seq)
            return false;
        const UINT end = start + bytes;
        for (const Write& w : it->second.ring)
            if (w.seq > seq && w.start < end && start < w.end)
                return true;
        return false;
    }

    // An entry's own bytes: its vertices, and, when it is indexed, the whole index buffer, whose element
    // size is not kept here.
    bool OverwrittenSince(const Rec& r, uint64_t seq)
    {
        const UINT first = static_cast<UINT>(r.baseVertex + r.minIndex);
        const UINT start = r.vbOffset[0] + first * r.vbStride[0];
        if (WrittenOver(r.vb[0], start, r.numVertices * r.vbStride[0], seq))
            return true;
        if (r.vb[1] && WrittenOver(r.vb[1], r.vbOffset[1] + first * r.vbStride[1],
                                   r.numVertices * r.vbStride[1], seq))
            return true;
        return WrittenOver(r.ib, 0, 0xFFFFFFFFu, seq);
    }

    // Records dropped this frame because they were not drawn with the world's camera and depth slice.
    unsigned g_nOffWorld = 0;
    bool     g_vbSample = false;

    // Does the geometry a cached entry points at still hold what it held when it was recorded? The client
    // re-fills its buffers as visibility changes, and an entry is a pointer plus an index range, not a copy.
    // Trace only: reading a write-only buffer back is slow, so this samples a few entries per frame.
    struct Sampled { uint32_t hash; double when; };
    std::unordered_map<const void*, Sampled> g_vbHash;
    unsigned g_vbChecked = 0, g_vbChanged = 0, g_vbCursor = 0, g_vbTotalChanged = 0;
    char     g_vbInfo[260] = {};

    uint32_t HashBytes(const uint8_t* p, size_t n)
    {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
        return h;
    }

    // The absolute transform derived for the frame's first M2 record, against the camera's own rotation:
    // if an entry's frame of reference is the camera, keeping it across frames cannot work.
    char g_frameInfo[360] = {};
    char g_frameInfo2[240] = {};

    // Refreshed entries whose replay inputs changed since last frame, for the volume trace: how many,
    // and the biggest change.
    unsigned g_nChanged = 0;
    float    g_maxDiff  = 0.0f;
    char     g_diffInfo[400] = {};

    // The first few entries added this frame, for the volume trace.
    char g_newInfo[640] = {};
    int  g_newInfoLen   = 0;
    int  g_newInfoCount = 0;

    // What the world filter drops, kept apart from the new-entry list so neither crowds the other out.
    char g_dropInfo[400] = {};
    int  g_dropInfoLen   = 0;
    int  g_dropInfoCount = 0;

    // And what the client overwrote under us, again on its own.
    char g_overInfo[400] = {};
    int  g_overInfoLen   = 0;
    int  g_overInfoCount = 0;

    // A few new entries' positions relative to the player, for the probe: are they where trees stand?
    int   g_samplesLeft = 0;
    float g_logPlayer[3] = {};

    // The frame's records into the cache. camVPInv takes the frame's camera out of shader draws; cam is
    // the camera's absolute position, to put fixed-function draws back into absolute coordinates.
    void Merge(const D3DMATRIX& camVPInv, const float cam[3], double now)
    {
        D3DMATRIX toAbs;
        Translation(cam[0], cam[1], cam[2], toAbs);
        D3DMATRIX camOut;
        Mul(camVPInv, toAbs, camOut);   // camera clip -> absolute world
        g_newInfo[0] = 0; g_newInfoLen = 0; g_newInfoCount = 0;
        g_dropInfo[0] = 0; g_dropInfoLen = 0; g_dropInfoCount = 0;
        g_overInfo[0] = 0; g_overInfoLen = 0; g_overInfoCount = 0;
        g_nChanged = 0; g_maxDiff = 0.0f; g_diffInfo[0] = 0;
        g_nOffWorld = 0;
        g_frameInfo[0] = 0;

        for (Rec& r : g_frame)
        {
            // The client draws the sky and the far horizon with cameras and depth slices of their own.
            // Taken out of the camera with the WORLD camera, such a draw lands in the wrong place at the
            // wrong scale: in the shadow map, a huge caster near the sun that covered a third of the map
            // on some frames and made the volumetric light blink off. Only world draws are kept.
            // A fixed-function draw carries its own world matrix, and the client renders camera-relative
            // from one camera position, so which projection drew it does not matter: the far horizon
            // converts as well as the world does, and a ridge between you and the sun can shade you. A
            // shader draw is different: its transform is folded into c2..c5 with the camera that drew it,
            // and taking the wrong camera out of that is what put a huge caster in the map.
            const bool offSlice = r.minZ != g_worldMinZ || r.maxZ != g_worldMaxZ;
            const bool offCam   = r.hasProj && g_haveWorldCam &&
                                  (r.proj00 != g_worldProj.m[0][0] || r.proj22 != g_worldProj.m[2][2] ||
                                   r.proj32 != g_worldProj.m[3][2]);
            const bool keep     = g_cfg.shadow.horizon && !r.vs;
            if ((offSlice || offCam) && !keep)
            {
                if (g_dropInfoCount < 5)   // the trace: what the world filter throws away
                {
                    ++g_dropInfoCount;
                    g_dropInfoLen += _snprintf_s(g_dropInfo + g_dropInfoLen, sizeof(g_dropInfo) - g_dropInfoLen,
                        _TRUNCATE, " [%s %uv %up slice %.4f..%.4f%s proj %.3f/%.4f/%.2f]",
                        r.vs ? "M2" : "ff", r.numVertices, r.primCount, r.minZ, r.maxZ,
                        offSlice ? " OFF-SLICE" : " off-camera", r.proj00, r.proj22, r.proj32);
                }
                ReleaseRec(r);
                r = Rec{};
                ++g_nOffWorld;
                continue;
            }

            Entry e;
            float pos[3];
            if (r.vs)
            {
                const float* c = &g_constPool[r.consts];
                D3DMATRIX m;
                FromRegisters(&c[2 * 4], m);
                Mul(m, camOut, e.absolute);
                if (!g_frameInfo[0])
                {
                    const D3DMATRIX& a = e.absolute;
                    _snprintf_s(g_frameInfo, sizeof(g_frameInfo), _TRUNCATE,
                        "A rows (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) T (%.1f %.1f %.1f); "
                        "view rows (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f)",
                        a.m[0][0], a.m[0][1], a.m[0][2], a.m[1][0], a.m[1][1], a.m[1][2],
                        a.m[2][0], a.m[2][1], a.m[2][2], a.m[3][0], a.m[3][1], a.m[3][2],
                        g_worldView.m[0][0], g_worldView.m[0][1], g_worldView.m[0][2],
                        g_worldView.m[1][0], g_worldView.m[1][1], g_worldView.m[1][2],
                        g_worldView.m[2][0], g_worldView.m[2][1], g_worldView.m[2][2]);
                }
                // Key point: the first bone's origin (c31..c33 .w), through A. For models whose c2..c5
                // carry the world matrix instead, that is the model's own origin. Either way it is stable.
                const float b[4] = { c[31 * 4 + 3], c[32 * 4 + 3], c[33 * 4 + 3], 1.0f };
                for (int j = 0; j < 3; ++j)
                    pos[j] = b[0] * e.absolute.m[0][j] + b[1] * e.absolute.m[1][j] + b[2] * e.absolute.m[2][j] +
                             e.absolute.m[3][j];
            }
            else
            {
                e.absolute = r.world;
                e.absolute.m[3][0] += cam[0];
                e.absolute.m[3][1] += cam[1];
                e.absolute.m[3][2] += cam[2];
                pos[0] = e.absolute.m[3][0]; pos[1] = e.absolute.m[3][1]; pos[2] = e.absolute.m[3][2];
            }

            const Key k = { r.vb[0], r.ib, r.vs, r.baseVertex, r.minIndex, r.numVertices, r.startIndex, r.primCount };
            std::vector<Entry>& list = g_cache[k];
            Entry* best = nullptr;
            float  bestD2 = kMatchRadius * kMatchRadius;
            for (Entry& cand : list)
            {
                if (cand.lastSeen == now)
                    continue;                      // already matched this frame: another instance
                const float dx = cand.pos[0] - pos[0], dy = cand.pos[1] - pos[1], dz = cand.pos[2] - pos[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 <= bestD2) { bestD2 = d2; best = &cand; }
            }
            bool moved = false;
            if (!best)
            {
                float moveD2 = kMoveRadius * kMoveRadius;
                for (Entry& cand : list)
                {
                    if (cand.lastSeen == now)
                        continue;
                    const float dx = cand.pos[0] - pos[0], dy = cand.pos[1] - pos[1], dz = cand.pos[2] - pos[2];
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 <= moveD2) { moveD2 = d2; best = &cand; moved = true; }
                }
            }
            if (best)
            {
                // Seen again: fresh position, matrices, constants and alpha state; same objects referenced.
                {
                    float dm = 0.0f;
                    for (int i = 0; i < 4; ++i)
                        for (int j = 0; j < 4; ++j)
                            dm = (std::max)(dm, fabsf(best->absolute.m[i][j] - e.absolute.m[i][j]));
                    float dc = 0.0f;
                    int   dcReg = -1;
                    if (r.vs && best->consts.size() == 256 * 4)
                    {
                        const float* nc = &g_constPool[r.consts];
                        for (int i = 0; i < 256 * 4; ++i)
                        {
                            const float dd = fabsf(best->consts[i] - nc[i]);
                            if (dd > dc) { dc = dd; dcReg = i; }
                        }
                    }
                    const float worst = (std::max)(dm, dc);
                    if (worst > 1e-3f)
                    {
                        ++g_nChanged;
                        if (worst > g_maxDiff)
                        {
                            g_maxDiff = worst;
                            _snprintf_s(g_diffInfo, sizeof(g_diffInfo), _TRUNCATE,
                                "%s %uv at (%.0f %.0f %.0f) from camera: matrix %.3g, constant c%d.%c %.3g (%.4g -> %.4g)",
                                r.vs ? "M2" : "ff", r.numVertices, pos[0] - cam[0], pos[1] - cam[1], pos[2] - cam[2],
                                dm, dcReg >= 0 ? dcReg / 4 : -1, "xyzw"[dcReg >= 0 ? dcReg % 4 : 0], dc,
                                dcReg >= 0 ? best->consts[dcReg] : 0.0f, dcReg >= 0 ? g_constPool[r.consts + dcReg] : 0.0f);
                        }
                    }
                }
                best->absolute = e.absolute;
                memcpy(best->pos, pos, sizeof(pos));
                if (r.vs)
                    best->consts.assign(&g_constPool[r.consts], &g_constPool[r.consts] + 256 * 4);
                best->rec.alphaTest = r.alphaTest; best->rec.alphaRef = r.alphaRef; best->rec.alphaFunc = r.alphaFunc;
                best->seq = r.seq;
                best->mobile = best->mobile || moved;
                best->lastSeen = now;
                ReleaseRec(r);
                ++g_nRefreshed;
            }
            else
            {
                e.rec = r;                         // the entry takes over the references
                if (r.vs)
                    e.consts.assign(&g_constPool[r.consts], &g_constPool[r.consts] + 256 * 4);
                memcpy(e.pos, pos, sizeof(pos));
                e.mobile = false;
                e.seq = r.seq;
                e.lastSeen = now;
                list.push_back(std::move(e));
                ++g_entries;
                ++g_nAdded;
                if (g_newInfoCount < 5)
                {
                    ++g_newInfoCount;
                    const D3DMATRIX& a = list.back().absolute;
                    const float sx = sqrtf(a.m[0][0] * a.m[0][0] + a.m[0][1] * a.m[0][1] + a.m[0][2] * a.m[0][2]);
                    const float sz = sqrtf(a.m[2][0] * a.m[2][0] + a.m[2][1] * a.m[2][1] + a.m[2][2] * a.m[2][2]);
                    g_newInfoLen += _snprintf_s(g_newInfo + g_newInfoLen, sizeof(g_newInfo) - g_newInfoLen, _TRUNCATE,
                        " [%s %uv %up at (%.0f %.0f %.0f) vb %p base %d min %u start %u scale %.3g/%.3g]",
                        r.vs ? "M2" : "ff", r.numVertices, r.primCount, pos[0] - cam[0], pos[1] - cam[1],
                        pos[2] - cam[2], r.vb[0], r.baseVertex, r.minIndex, r.startIndex, sx, sz);
                }
                if (g_samplesLeft > 0 && r.vs)
                {
                    --g_samplesLeft;
                    Log("shadow:   new M2 entry at %.1f %.1f %.1f from the player (%u verts)", pos[0] - g_logPlayer[0],
                        pos[1] - g_logPlayer[1], pos[2] - g_logPlayer[2], r.numVertices);
                }
            }
            r = Rec{};                              // references now belong to the cache
        }
        g_frame.clear();
        g_constPool.clear();
    }

    // Gone if unseen this frame while in view and near; else aged out, or beyond the map's reach.
    void Evict(const D3DMATRIX& camVP, const float cam[3], double now)
    {
        const ShadowSettings& s = g_cfg.shadow;
        for (auto kv = g_cache.begin(); kv != g_cache.end(); )
        {
            std::vector<Entry>& list = kv->second;
            for (size_t i = 0; i < list.size(); )
            {
                Entry& e = list[i];
                bool gone = false;
                if (OverwrittenSince(e.rec, e.seq))
                {
                    gone = true;
                    ++g_nEvictWritten;
                    if (g_overInfoCount < 5)   // the trace: what the client overwrites under us
                    {
                        ++g_overInfoCount;
                        g_overInfoLen += _snprintf_s(g_overInfo + g_overInfoLen, sizeof(g_overInfo) - g_overInfoLen,
                            _TRUNCATE, " [%s %uv %up vb %p stride %u %s]", e.rec.vs ? "M2" : "ff",
                            e.rec.numVertices, e.rec.primCount, e.rec.vb[0], e.rec.vbStride[0],
                            e.lastSeen == now ? "drawn this frame" : "not drawn");
                    }
                }
                else if (e.lastSeen < now)
                {
                    const float rel[3] = { e.pos[0] - cam[0], e.pos[1] - cam[1], e.pos[2] - cam[2] };
                    float c[4];
                    for (int j = 0; j < 4; ++j)
                        c[j] = rel[0] * camVP.m[0][j] + rel[1] * camVP.m[1][j] + rel[2] * camVP.m[2][j] + camVP.m[3][j];
                    const float dist2 = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2];
                    const bool inView = c[3] > 0.0f && fabsf(c[0]) < 0.9f * c[3] && fabsf(c[1]) < 0.9f * c[3];
                    if (OverwrittenSince(e.rec, e.seq))
                    {
                        gone = true; ++g_nEvictWritten;
                    }
                    else if (inView && dist2 < s.evictDistance * s.evictDistance)
                    {
                        gone = true; ++g_nEvictView;
                    }
                    else if (e.mobile || now - e.lastSeen > s.cacheTime)
                    {
                        // Something that moves is gone the moment it stops being drawn: its shade belongs
                        // where it is now, not where it was.
                        gone = true; ++g_nEvictAge;
                    }
                }
                if (gone)
                {
                    ReleaseRec(e.rec);
                    list[i] = std::move(list.back());
                    list.pop_back();
                    --g_entries;
                }
                else
                    ++i;
            }
            kv = list.empty() ? g_cache.erase(kv) : std::next(kv);
        }
        // Over the cap: the longest unseen go first.
        while (g_entries > kMaxCache)
        {
            std::vector<Entry>* oldList = nullptr;
            size_t oldIdx = 0;
            double oldT = now + 1.0;
            for (auto& kv : g_cache)
                for (size_t i = 0; i < kv.second.size(); ++i)
                    if (kv.second[i].lastSeen < oldT) { oldT = kv.second[i].lastSeen; oldList = &kv.second; oldIdx = i; }
            if (!oldList)
                break;
            ReleaseRec((*oldList)[oldIdx].rec);
            (*oldList)[oldIdx] = std::move(oldList->back());
            oldList->pop_back();
            --g_entries;
            ++g_nEvictCap;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // resources

    IDirect3DTexture9*    g_depthTex  = nullptr;
    IDirect3DSurface9*    g_depthSurf = nullptr;
    IDirect3DSurface9*    g_colour    = nullptr;   // a render target has to be bound; nothing is written to it
    UINT                  g_size      = 0;
    IDirect3DStateBlock9* g_sb        = nullptr;
    bool                  g_failed    = false;
    bool                  g_valid     = false;
    bool                  g_logNext   = false;
    D3DMATRIX             g_shadowVP  = {};        // camera-relative world -> shadow clip, for the reader

    void ReleaseResources()
    {
        SafeRelease(g_depthSurf);
        SafeRelease(g_depthTex);
        SafeRelease(g_colour);
        SafeRelease(g_sb);
        g_size  = 0;
        g_valid = false;
    }

    bool EnsureResources(IDirect3DDevice9* dev, UINT size)
    {
        if (g_size == size && g_depthSurf && g_colour && g_sb)
            return true;
        ReleaseResources();
        auto* d = dev->lpVtbl;
        HRESULT hr = d->CreateTexture(dev, size, size, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT,
                                      &g_depthTex, nullptr);
        if (SUCCEEDED(hr))
            hr = g_depthTex->lpVtbl->GetSurfaceLevel(g_depthTex, 0, &g_depthSurf);
        if (FAILED(hr))
        {
            Log("shadow: could not create the %ux%u INTZ depth map (hr=0x%08X)", size, size, hr);
            ReleaseResources();
            return false;
        }
        // The NULL format is a render target that takes no memory; fall back to a cheap real one.
        const char* colourKind = "NULL";
        if (FAILED(d->CreateRenderTarget(dev, size, size, kNULL, D3DMULTISAMPLE_NONE, 0, FALSE, &g_colour, nullptr)))
        {
            colourKind = "R5G6B5";
            if (FAILED(d->CreateRenderTarget(dev, size, size, D3DFMT_R5G6B5, D3DMULTISAMPLE_NONE, 0, FALSE, &g_colour, nullptr)))
            {
                Log("shadow: could not create a %ux%u colour target", size, size);
                ReleaseResources();
                return false;
            }
        }
        if (FAILED(d->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)) || !g_sb)
        {
            Log("shadow: could not create a state block");
            ReleaseResources();
            return false;
        }
        g_size = size;
        Log("shadow: %ux%u INTZ depth map ready (colour target %s)", size, size, colourKind);
        return true;
    }

    // Render states the replay changes; re-set afterwards through the vtable so other hooks' mirrors stay
    // true, then the state block restores the device exactly (the same pattern as rays.cpp and volume.cpp).
    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
        D3DRS_ALPHABLENDENABLE, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_LIGHTING, D3DRS_STENCILENABLE,
        D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
        // Geometry and depth states the replay must not inherit (see the replay).
        D3DRS_VERTEXBLEND, D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_DEPTHBIAS,
        D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_FILLMODE, D3DRS_CLIPPING,
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);
    constexpr int kFirstGeometry = 14;   // index of D3DRS_VERTEXBLEND above

    // What the client left in those states at this frame's replay, for the volume trace.
    char g_inherited[200] = {};
}

void ShadowSetPhase(bool recording)
{
    g_recording = recording && g_cfg.shadow.enabled && !g_failed;
}

void ShadowNoteBufferWrite(const void* buffer, UINT offset, UINT size)
{
    if (!buffer || (g_written.size() >= 8192 && !g_written.count(buffer)))
        return;
    BufferWrites& b = g_written[buffer];
    b.last = ++g_writeSeq;
    b.ring[b.next] = { offset, size ? offset + size : 0xFFFFFFFFu, b.last };   // size 0 is the whole buffer
    b.next = (b.next + 1) % 16;
}

void RecordConstants(UINT reg, const float* data, UINT count)
{
    // The mirror follows every client upload, recording or not, so a snapshot is right whenever taken.
    if (!g_mirrorValid || !data || reg >= 256)
        return;
    if (reg + count > 256)
        count = 256 - reg;
    memcpy(&g_mirror[reg * 4], data, count * 4 * sizeof(float));
}

void RecordDraw(IDirect3DDevice9* dev, bool indexed, D3DPRIMITIVETYPE prim, INT baseVertex, UINT minIndex,
                UINT numVertices, UINT startIndex, UINT primCount)
{
    if (!g_recording)
        return;
    auto* d = dev->lpVtbl;
    if (!g_mirrorValid)
    {
        d->GetVertexShaderConstantF(dev, 0, g_mirror, 256);
        g_mirrorValid = true;
    }

    // Opaque, depth-writing world geometry only.
    DWORD zen = 0, zw = 0, blend = 1;
    d->GetRenderState(dev, D3DRS_ZENABLE, &zen);
    d->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
    d->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
    ++g_seen;
    if (!zen)   { ++g_rejZ;     return; }
    if (!zw)    { ++g_rejZW;    return; }
    if (blend)  { ++g_rejBlend; return; }
    if (g_frame.size() >= kMaxFrame)
        return;

    Rec r = {};
    d->GetStreamSource(dev, 0, &r.vb[0], &r.vbOffset[0], &r.vbStride[0]);
    if (!r.vb[0])
    {
        ++g_rejNoVB;
        return;
    }
    D3DVERTEXBUFFER_DESC vd = {};
    r.vb[0]->lpVtbl->GetDesc(r.vb[0], &vd);
    if (vd.Usage & D3DUSAGE_DYNAMIC)
    {
        SafeRelease(r.vb[0]);
        ++g_rejDynamic;
        return;
    }

    r.seq = g_writeSeq;
    r.indexed = indexed; r.prim = prim; r.baseVertex = baseVertex; r.minIndex = minIndex;
    r.numVertices = numVertices; r.startIndex = startIndex; r.primCount = primCount;
    d->GetStreamSource(dev, 1, &r.vb[1], &r.vbOffset[1], &r.vbStride[1]);
    d->GetVertexShader(dev, &r.vs);
    d->GetVertexDeclaration(dev, &r.decl);
    d->GetFVF(dev, &r.fvf);
    if (indexed)
        d->GetIndices(dev, &r.ib);
    d->GetTexture(dev, 0, &r.tex0);
    d->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &r.alphaTest);
    d->GetRenderState(dev, D3DRS_ALPHAREF, &r.alphaRef);
    d->GetRenderState(dev, D3DRS_ALPHAFUNC, &r.alphaFunc);
    d->GetTransform(dev, D3DTS_WORLD, &r.world);
    if (r.vs)
    {
        r.consts = g_constPool.size();
        g_constPool.insert(g_constPool.end(), g_mirror, g_mirror + 256 * 4);
    }
    D3DVIEWPORT9 vp = {};
    r.minZ = 0.0f; r.maxZ = 1.0f;
    if (SUCCEEDED(d->GetViewport(dev, &vp)))
    {
        VoteSlice(vp.MinZ, vp.MaxZ);
        r.minZ = vp.MinZ; r.maxZ = vp.MaxZ;
    }
    if (!r.vs)
    {
        D3DMATRIX cv, cp;
        d->GetTransform(dev, D3DTS_VIEW, &cv);
        d->GetTransform(dev, D3DTS_PROJECTION, &cp);
        VoteCamera(cv, cp, r.minZ, r.maxZ);
        r.hasProj = true;
        r.proj00 = cp.m[0][0]; r.proj22 = cp.m[2][2]; r.proj32 = cp.m[3][2];
    }

    g_frame.push_back(r);
}

namespace
{
    // 0 drawn, 1 disabled or failed, 2 no sun or camera, 3 camera did not invert, 4 empty cache or no
    // resources.
    int      g_replayOutcome = -1;
    unsigned g_replayDrawn   = 0;
}

void ShadowLastReplay(int& outcome, unsigned& drawn, unsigned& entries, unsigned counts[5], const char*& newInfo)
{
    outcome = g_replayOutcome;
    drawn   = g_replayDrawn;
    entries = static_cast<unsigned>(g_entries);
    counts[0] = g_nRefreshed; counts[1] = g_nAdded; counts[2] = g_nEvictView; counts[3] = g_nEvictAge;
    counts[4] = g_nEvictCap + g_nEvictWritten;
    newInfo = g_newInfo;
}

const char* ShadowInherited()
{
    return g_inherited;
}

void ShadowWorldCameraPlanes(float& nearZ, float& farZ)
{
    const float a = g_worldProj.m[2][2];
    nearZ = a != 0.0f ? -g_worldProj.m[3][2] / a : 0.0f;
    farZ  = (a - 1.0f) != 0.0f ? a * nearZ / (a - 1.0f) : 0.0f;
}

const char* ShadowFrameInfo()
{
    return g_frameInfo;
}

const char* ShadowMapCentre()
{
    return g_frameInfo2;
}

const char* ShadowBufferCheck(unsigned& checked, unsigned& changed)
{
    g_vbSample = true;
    checked = g_vbChecked;
    changed = g_vbTotalChanged;   // running total over the trace
    return g_vbInfo;
}

const char* ShadowChanges(unsigned& changed)
{
    changed = g_nChanged;
    return g_diffInfo;
}

unsigned ShadowOffWorld()
{
    return g_nOffWorld;
}

const char* ShadowDropped()
{
    return g_dropInfo;
}

const char* ShadowOverwritten(unsigned& count)
{
    count = g_nEvictWritten;
    return g_overInfo;
}

void ShadowWorldEnded(IDirect3DDevice9* dev)
{
    g_replayOutcome = 1;
    g_replayDrawn   = 0;
    const bool logThis = g_logNext;
    g_logNext = false;
    g_recording = false;
    SettleVotes(logThis);

    const ShadowSettings& s = g_cfg.shadow;
    if (!s.enabled || g_failed)
    {
        ReleaseFrame();
        if (!s.enabled && !g_cache.empty())
            ClearCache();
        return;
    }

    float sunDir[3], cam[3], pl[3];
    D3DMATRIX view, proj;
    const bool worldCam = g_haveWorldCam;
    if (worldCam) { view = g_worldView; proj = g_worldProj; }
    if (!RaysSunDirection(sunDir) || !(worldCam || RaysCamera(view, proj)) || !ClientCamera(cam))
    {
        if (logThis) Log("shadow: nothing replayed (no sun, camera matrices or camera position)");
        g_replayOutcome = 2;
        ReleaseFrame();
        return;
    }
    const bool havePlayer = ClientPlayer(pl);
    if (!havePlayer) { pl[0] = cam[0]; pl[1] = cam[1]; pl[2] = cam[2]; }

    // The sun comes from the sprite the client draws, measured afresh each frame, and that measurement
    // is noisy: standing still, with the camera not moving and the time pinned, it wandered in the fifth
    // decimal. The map is built around it, so the whole map turned a little every frame and everything in
    // it shifted: that is the shimmer, and it also defeated the texel snapping below, whose grid is the
    // light's own axes. The direction is only taken when it has really moved, a quarter of a degree.
    {
        static float stable[3] = { 0.0f, 0.0f, 0.0f };
        static bool  have = false;
        const float dot = stable[0] * sunDir[0] + stable[1] * sunDir[1] + stable[2] * sunDir[2];
        if (!have || dot < 0.9999996f)      // cos(0.05 degrees): above the wobble, below a minute of sun
        {
            stable[0] = sunDir[0]; stable[1] = sunDir[1]; stable[2] = sunDir[2];
            have = true;
        }
        sunDir[0] = stable[0]; sunDir[1] = stable[1]; sunDir[2] = stable[2];
    }

    D3DMATRIX camVP, camVPInv;
    Mul(view, proj, camVP);
    if (!Invert(camVP, camVPInv))
    {
        g_replayOutcome = 3;
        ReleaseFrame();
        return;
    }

    const double now = Now();
    const double t0  = now;
    const UINT recorded = static_cast<UINT>(g_frame.size());
    g_nRefreshed = g_nAdded = g_nEvictView = g_nEvictAge = g_nEvictCap = g_nEvictWritten = 0;
    if (logThis)
        Log("shadow: this frame's world draws: %u seen, recorded %u; rejected: depth test off %u, depth writes "
            "off %u, blended %u, no vertex buffer %u, dynamic %u", g_seen, recorded, g_rejZ, g_rejZW, g_rejBlend,
            g_rejNoVB, g_rejDynamic);
    g_rejZ = g_rejZW = g_rejBlend = g_rejNoVB = g_rejDynamic = g_seen = 0;
    if (logThis)
    {
        g_samplesLeft = 6;
        memcpy(g_logPlayer, pl, sizeof(g_logPlayer));
    }
    Merge(camVPInv, cam, now);
    g_samplesLeft = 0;
    Evict(camVP, cam, now);

    if (g_cache.empty() || !EnsureResources(dev, static_cast<UINT>(s.size)))
    {
        g_replayOutcome = 4;
        if (!g_cache.empty())
            g_failed = true;
        return;
    }

    // The sun camera, camera-relative: centred on the player, looking down the sun, `range` yards either
    // side and `depth` yards toward the sun and away from it.
    //
    // Its centre is snapped to whole texels of its own grid first. A 2048 map over 180 yards is a texel
    // every 0.088 yards, and a map centred exactly on the player slides by a fraction of a texel with
    // every step: each shadow edge then re-samples differently from frame to frame, which is the swimming
    // the light picked up from the smallest camera move. Whole-texel steps leave the edges where they are.
    {
        const float texel = s.snap ? (s.range * 2.0f) / (s.size > 0 ? s.size : 1) : 0.0f;
        const float up0[3] = { 0.0f, 0.0f, 1.0f };
        float x[3] = { up0[1] * sunDir[2] - up0[2] * sunDir[1], up0[2] * sunDir[0] - up0[0] * sunDir[2],
                       up0[0] * sunDir[1] - up0[1] * sunDir[0] };
        const float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (xl > 1e-4f && texel > 1e-6f)
        {
            for (float& v : x) v /= xl;
            const float y[3] = { sunDir[1] * x[2] - sunDir[2] * x[1], sunDir[2] * x[0] - sunDir[0] * x[2],
                                 sunDir[0] * x[1] - sunDir[1] * x[0] };
            // Snapped in absolute coordinates, so the grid stands still in the world, not with the camera.
            const float dx = pl[0] * x[0] + pl[1] * x[1] + pl[2] * x[2];
            const float dy = pl[0] * y[0] + pl[1] * y[1] + pl[2] * y[2];
            const float sx = floorf(dx / texel + 0.5f) * texel - dx;
            const float sy = floorf(dy / texel + 0.5f) * texel - dy;
            for (int i = 0; i < 3; ++i)
                pl[i] += sx * x[i] + sy * y[i];
        }
    }
    const float centre[3] = { pl[0] - cam[0], pl[1] - cam[1], pl[2] - cam[2] };
    _snprintf_s(g_frameInfo2, sizeof(g_frameInfo2), _TRUNCATE,
                "map centre abs (%.4f %.4f %.4f), camera (%.4f %.4f %.4f), sun (%.6f %.6f %.6f)",
                pl[0], pl[1], pl[2], cam[0], cam[1], cam[2], sunDir[0], sunDir[1], sunDir[2]);
    const float eye[3] = { centre[0] + sunDir[0] * s.depth, centre[1] + sunDir[1] * s.depth,
                           centre[2] + sunDir[2] * s.depth };
    const float up[3]  = { 0.0f, 0.0f, 1.0f };
    const float upX[3] = { 1.0f, 0.0f, 0.0f };
    D3DMATRIX sunView, sunProj, sunVP, fromAbs, fromAbsToSun;
    LookAtLH(eye, centre, fabsf(sunDir[2]) > 0.99f ? upX : up, sunView);
    OrthoLH(s.range * 2.0f, s.range * 2.0f, 1.0f, s.depth * 2.0f, sunProj);
    Mul(sunView, sunProj, sunVP);
    Translation(-cam[0], -cam[1], -cam[2], fromAbs);
    Mul(fromAbs, sunVP, fromAbsToSun);   // absolute world -> sun clip, for the shader entries
    g_shadowVP = sunVP;

    auto* d = dev->lpVtbl;

    // --- save ---------------------------------------------------------------------------------------
    IDirect3DSurface9* oldRT = nullptr;
    IDirect3DSurface9* oldDS = nullptr;
    d->GetRenderTarget(dev, 0, &oldRT);
    d->GetDepthStencilSurface(dev, &oldDS);
    g_sb->lpVtbl->Capture(g_sb);
    DWORD saved[kTouchedCount];
    for (int i = 0; i < kTouchedCount; ++i)
        d->GetRenderState(dev, kTouched[i], &saved[i]);
    D3DMATRIX oldWorld, oldView, oldProj;
    d->GetTransform(dev, D3DTS_WORLD, &oldWorld);
    d->GetTransform(dev, D3DTS_VIEW, &oldView);
    d->GetTransform(dev, D3DTS_PROJECTION, &oldProj);
    IDirect3DBaseTexture9*       oldTex0 = nullptr;
    IDirect3DVertexShader9*      oldVS   = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD                        oldFVF  = 0;
    IDirect3DVertexBuffer9*      oldVB   = nullptr;
    UINT                         oldOff = 0, oldStride = 0;
    d->GetTexture(dev, 0, &oldTex0);
    d->GetVertexShader(dev, &oldVS);
    d->GetVertexDeclaration(dev, &oldDecl);
    d->GetFVF(dev, &oldFVF);
    d->GetStreamSource(dev, 0, &oldVB, &oldOff, &oldStride);

    // --- replay the cache ---------------------------------------------------------------------------
    d->SetRenderTarget(dev, 0, g_colour);
    d->SetDepthStencilSurface(dev, g_depthSurf);
    d->Clear(dev, 0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    d->SetRenderState(dev, D3DRS_ZENABLE,           D3DZB_TRUE);
    d->SetRenderState(dev, D3DRS_ZWRITEENABLE,      TRUE);
    d->SetRenderState(dev, D3DRS_ZFUNC,             D3DCMP_LESSEQUAL);
    d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,  FALSE);
    d->SetRenderState(dev, D3DRS_CULLMODE,          D3DCULL_NONE);
    d->SetRenderState(dev, D3DRS_FOGENABLE,         FALSE);
    d->SetRenderState(dev, D3DRS_LIGHTING,          FALSE);
    d->SetRenderState(dev, D3DRS_STENCILENABLE,     FALSE);
    d->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(dev, D3DRS_COLORWRITEENABLE,  0);
    d->SetRenderState(dev, D3DRS_SRGBWRITEENABLE,   FALSE);
    d->SetTransform(dev, D3DTS_VIEW, &sunView);
    d->SetTransform(dev, D3DTS_PROJECTION, &sunProj);
    d->SetPixelShader(dev, nullptr);
    // Alpha for the test comes straight from the texture: foliage keeps its leaf shapes.
    d->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    d->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(dev, 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    d->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(dev, 1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
    d->SetTextureStageState(dev, 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);

    // The replay draws the cached objects with whatever the client left in every state it does not set,
    // and the client leaves different values on different frames. Vertex blending left on, for one,
    // draws each fixed-function object with another world matrix, and the same cache then fills the
    // whole map on one frame: the volumetric light blinked off with nothing in the cache changed.
    DWORD tci = 0, ttf = 0;
    d->GetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, &tci);
    d->GetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf);
    {
        const DWORD* g = &saved[kFirstGeometry];
        float bias = 0.0f, slope = 0.0f;
        memcpy(&bias, &g[3], 4);
        memcpy(&slope, &g[4], 4);
        _snprintf_s(g_inherited, sizeof(g_inherited), _TRUNCATE,
                    "vblend %u, indexed vblend %u, clip planes 0x%X, depth bias %g, slope bias %g, fill %u, "
                    "clipping %u, tci %u, ttf %u", g[0], g[1], g[2], bias, slope, g[5], g[6], tci, ttf);
    }
    d->SetRenderState(dev, D3DRS_VERTEXBLEND,              D3DVBF_DISABLE);
    d->SetRenderState(dev, D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    d->SetRenderState(dev, D3DRS_CLIPPLANEENABLE,          0);
    d->SetRenderState(dev, D3DRS_DEPTHBIAS,                0);
    d->SetRenderState(dev, D3DRS_SLOPESCALEDEPTHBIAS,      0);
    d->SetRenderState(dev, D3DRS_FILLMODE,                 D3DFILL_SOLID);
    d->SetRenderState(dev, D3DRS_CLIPPING,                 TRUE);
    d->SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX,         0);
    d->SetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetStreamSourceFreq(dev, 0, 1);
    d->SetStreamSourceFreq(dev, 1, 1);

    UINT drawn = 0, drawnVS = 0, unseen = 0;
    for (auto& kv : g_cache)
    for (const Entry& e : kv.second)
    {
        const Rec&   r = e.rec;
        if (e.lastSeen < now)
            ++unseen;
        if (r.vs)
        {
            D3DMATRIX m;
            Mul(e.absolute, fromAbsToSun, m);
            float c[16];
            ToRegisters(m, c);
            d->SetVertexShaderConstantF(dev, 0, e.consts.data(), 256);
            d->SetVertexShaderConstantF(dev, 2, c, 4);
            d->SetVertexShader(dev, r.vs);
            ++drawnVS;
        }
        else
        {
            D3DMATRIX w = e.absolute;
            w.m[3][0] -= cam[0]; w.m[3][1] -= cam[1]; w.m[3][2] -= cam[2];
            d->SetVertexShader(dev, nullptr);
            d->SetTransform(dev, D3DTS_WORLD, &w);
        }
        if (r.decl)
            d->SetVertexDeclaration(dev, r.decl);
        else
            d->SetFVF(dev, r.fvf);
        d->SetStreamSource(dev, 0, r.vb[0], r.vbOffset[0], r.vbStride[0]);
        if (r.vb[1])
            d->SetStreamSource(dev, 1, r.vb[1], r.vbOffset[1], r.vbStride[1]);
        if (r.indexed)
            d->SetIndices(dev, r.ib);
        d->SetTexture(dev, 0, r.tex0);
        d->SetRenderState(dev, D3DRS_ALPHATESTENABLE, r.alphaTest);
        d->SetRenderState(dev, D3DRS_ALPHAREF,        r.alphaRef);
        d->SetRenderState(dev, D3DRS_ALPHAFUNC,       r.alphaFunc);
        if (r.indexed)
            d->DrawIndexedPrimitive(dev, r.prim, r.baseVertex, r.minIndex, r.numVertices, r.startIndex, r.primCount);
        else
            d->DrawPrimitive(dev, r.prim, r.baseVertex, r.primCount);
        if (r.vb[1])
            d->SetStreamSource(dev, 1, nullptr, 0, 0);
        ++drawn;
    }

    // --- restore ------------------------------------------------------------------------------------
    for (int i = 0; i < kTouchedCount; ++i)
        d->SetRenderState(dev, kTouched[i], saved[i]);
    d->SetTransform(dev, D3DTS_WORLD, &oldWorld);
    d->SetTransform(dev, D3DTS_VIEW, &oldView);
    d->SetTransform(dev, D3DTS_PROJECTION, &oldProj);
    d->SetTexture(dev, 0, oldTex0);
    d->SetVertexShader(dev, oldVS);
    d->SetFVF(dev, oldFVF);
    if (oldDecl)
        d->SetVertexDeclaration(dev, oldDecl);
    d->SetStreamSource(dev, 0, oldVB, oldOff, oldStride);
    d->SetRenderTarget(dev, 0, oldRT);
    d->SetDepthStencilSurface(dev, oldDS);
    g_sb->lpVtbl->Apply(g_sb);

    SafeRelease(oldRT);
    SafeRelease(oldDS);
    SafeRelease(oldTex0);
    SafeRelease(oldVS);
    SafeRelease(oldDecl);
    SafeRelease(oldVB);

    if (g_vbSample)
    {
        g_vbSample = false;
        g_vbChecked = g_vbChanged = 0;
        g_vbInfo[0] = 0;
        // A different slice of the cache each frame, so a trace sweeps the whole of it.
        unsigned seen = 0, taken = 0;
        for (auto& kv : g_cache)
        {
            for (const Entry& e : kv.second)
            {
                if (taken >= 40)
                    break;
                if (seen++ < g_vbCursor || !e.rec.vb[0] || !e.rec.vbStride[0])
                    continue;
                ++taken;
                // From the middle of the entry's own range: a refill that keeps the first vertices still
                // moves what is behind them.
                const UINT mid = e.rec.numVertices / 2;
                const UINT at = e.rec.vbOffset[0] + (e.rec.baseVertex + e.rec.minIndex + mid) * e.rec.vbStride[0];
                const UINT n  = e.rec.vbStride[0] * (e.rec.numVertices - mid < 8 ? e.rec.numVertices - mid : 8);
                if (!n)
                    continue;
                void* p = nullptr;
                if (FAILED(e.rec.vb[0]->lpVtbl->Lock(e.rec.vb[0], at, n, &p, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !p)
                    continue;
                const uint32_t h = HashBytes(static_cast<const uint8_t*>(p), n);
                e.rec.vb[0]->lpVtbl->Unlock(e.rec.vb[0]);
                ++g_vbChecked;
                auto it = g_vbHash.find(&e);
                if (it != g_vbHash.end() && it->second.hash != h)
                {
                    ++g_vbChanged;
                    ++g_vbTotalChanged;
                    if (!g_vbInfo[0])
                        _snprintf_s(g_vbInfo, sizeof(g_vbInfo), _TRUNCATE,
                            "%s %uv %up vb %p changed %s this frame", e.rec.vs ? "M2" : "ff", e.rec.numVertices,
                            e.rec.primCount, e.rec.vb[0], e.lastSeen == now ? "(redrawn)" : "(NOT redrawn)");
                }
                g_vbHash[&e] = { h, now };
            }
            if (taken >= 40)
                break;
        }
        g_vbCursor = taken ? g_vbCursor + taken : 0;   // wrap once the sweep runs off the end
    }

    g_valid = true;
    g_replayOutcome = 0;
    g_replayDrawn   = drawn;
    if (logThis)
        Log("shadow: cache %u entries (%u not drawn this frame, kept from earlier): %u refreshed, %u new; evicted "
            "%u in view but gone, %u aged out, %u over the cap. Replayed %u (%u through M2 "
            "shaders) in %.2f ms CPU; sun (%.2f %.2f %.2f), centred on the %s",
            static_cast<unsigned>(g_entries), unseen, g_nRefreshed, g_nAdded, g_nEvictView, g_nEvictAge,
            g_nEvictCap, drawn, drawnVS, 1000.0 * (Now() - t0), sunDir[0], sunDir[1], sunDir[2],
            havePlayer ? "player" : "camera");
}

void ShadowFrameEnd()
{
    g_recording = false;
    ReleaseFrame();
    g_sliceCount = 0;
    g_camCount   = 0;
}

bool ShadowWorldCamera(D3DMATRIX& view, D3DMATRIX& proj)
{
    if (!g_haveWorldCam)
        return false;
    view = g_worldView;
    proj = g_worldProj;
    return true;
}

void ShadowWorldDepthRange(float& minZ, float& maxZ)
{
    minZ = g_worldMinZ;
    maxZ = g_worldMaxZ;
}

IDirect3DTexture9* ShadowTexture()
{
    return (g_cfg.shadow.enabled && g_valid) ? g_depthTex : nullptr;
}

bool ShadowMatrix(D3DMATRIX& m)
{
    if (!g_valid)
        return false;
    m = g_shadowVP;
    return true;
}

void ShadowReset()
{
    ReleaseFrame();
    g_written.clear();     // and every buffer that survives it is a new object at a new address
    ClearCache();          // the client's buffers are rebuilt across a Reset: nothing cached stays valid
    ReleaseResources();
    g_mirrorValid = false;
    g_failed = false;
}

void ShadowProbe()
{
    g_logNext = true;
}
