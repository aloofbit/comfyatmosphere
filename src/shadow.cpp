// shadow -- a depth map of the world as the sun sees it, kept by a cache of casters.
//
// Volumetric light asks, for any point in the air: does the sun reach it? That needs the scene's depth
// from the sun's side, and the client never renders one. So its opaque world draws are recorded as they
// happen and replayed from the sun into a depth texture over a square around the player.
//
// The client only draws what is in the camera's view, so a map made from one frame's draws loses a tree
// the moment it leaves the screen -- and the air it shaded flashes bright. Hence a cache: every recorded
// draw becomes an entry, kept across frames in ABSOLUTE world coordinates, and the map is drawn from the
// whole cache every frame.
//
//   Recording   Between the end of the sky and the end of the world: every draw that writes depth with
//               blending off -- terrain, buildings, trees, characters. Each record keeps its buffers,
//               shader, declaration, texture, alpha test and world matrix, holding a reference to each.
//               Shader draws also keep a snapshot of all 256 vertex-shader constants (bones, c2..c5 and
//               the rest), from a mirror kept current by every client upload. Dynamic vertex buffers are
//               skipped: the client re-fills them mid-frame (the grass arena is one, see comfygrass).
//   Absolute    The client draws camera-relative. Fixed-function entries keep their world matrix with the
//               camera's position added back. Shader entries keep A = M * inverse(camera view-proj) * T(cam)
//               -- their own transform with the camera taken out -- which is affine whether the client
//               folds the world matrix into c2..c5 or into the bones (both checked by the probe).
//   Identity    What is drawn (buffers, shader, index range), and among the instances of that, the nearest
//               within matchRadius yards. A first version keyed on position to a quarter of a yard, and
//               trees -- whose reference point is a swaying root bone -- drifted across those boundaries
//               and were re-added every few frames (472 new entries a frame, a cache of 2500 duplicates).
//               Matching by nearness keeps a swaying tree one entry, and lets a walking character's entry
//               move with it instead of leaving a trail. Each entry matches at most once a frame.
//   Eviction    An entry is gone if it was NOT drawn this frame although it sits in view and near -- a
//               character that walked off, a mesh that switched level of detail. Out of view it stays,
//               for up to cacheTime seconds: the tree over your head keeps shading you after you look away
//               from it. There is deliberately no "too far from the player" rule: a model's reference
//               point is its first bone, which for some models sits far from the geometry -- measured,
//               trees inside the map reading 85-95 yards away -- so a step across that limit dropped a
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
    struct CamVote { D3DMATRIX view, proj; UINT votes; };
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

    void VoteCamera(const D3DMATRIX& view, const D3DMATRIX& proj)
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
            g_cams[g_camCount++] = { view, proj, 1 };
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
            const CamVote* best = &g_cams[0];
            for (int i = 1; i < g_camCount; ++i)
                if (g_cams[i].votes > best->votes) best = &g_cams[i];
            g_worldView = best->view;
            g_worldProj = best->proj;
            g_haveWorldCam = true;
            if (log)
                for (int i = 0; i < g_camCount; ++i)
                {
                    const float n = -g_cams[i].proj.m[3][2] / g_cams[i].proj.m[2][2];
                    Log("shadow: camera near %.3f / far %.1f used by %u draws%s", n,
                        g_cams[i].proj.m[2][2] * n / (g_cams[i].proj.m[2][2] - 1.0f), g_cams[i].votes,
                        &g_cams[i] == best ? "  <-- the world" : "");
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
        float              pos[3];         // absolute position of its reference point
        double             lastSeen;       // == now once matched this frame
    };

    // Each key holds the instances of that model, wherever they stand.
    std::unordered_map<Key, std::vector<Entry>, KeyHash> g_cache;
    size_t           g_entries = 0;
    constexpr size_t kMaxCache = 5000;
    constexpr float  kMatchRadius = 3.0f;   // yards a recorded draw may be from an instance and still be it

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

        for (Rec& r : g_frame)
        {
            Entry e;
            float pos[3];
            if (r.vs)
            {
                const float* c = &g_constPool[r.consts];
                D3DMATRIX m;
                FromRegisters(&c[2 * 4], m);
                Mul(m, camOut, e.absolute);
                // Key point: the first bone's origin (c31..c33 .w), through A. For models whose c2..c5
                // carry the world matrix instead, that is the model's own origin -- either way stable.
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
            if (best)
            {
                // Seen again: fresh position, matrices, constants and alpha state; same objects referenced.
                best->absolute = e.absolute;
                memcpy(best->pos, pos, sizeof(pos));
                if (r.vs)
                    best->consts.assign(&g_constPool[r.consts], &g_constPool[r.consts] + 256 * 4);
                best->rec.alphaTest = r.alphaTest; best->rec.alphaRef = r.alphaRef; best->rec.alphaFunc = r.alphaFunc;
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
                e.lastSeen = now;
                list.push_back(std::move(e));
                ++g_entries;
                ++g_nAdded;
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
                if (e.lastSeen < now)
                {
                    const float rel[3] = { e.pos[0] - cam[0], e.pos[1] - cam[1], e.pos[2] - cam[2] };
                    float c[4];
                    for (int j = 0; j < 4; ++j)
                        c[j] = rel[0] * camVP.m[0][j] + rel[1] * camVP.m[1][j] + rel[2] * camVP.m[2][j] + camVP.m[3][j];
                    const float dist2 = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2];
                    const bool inView = c[3] > 0.0f && fabsf(c[0]) < 0.9f * c[3] && fabsf(c[1]) < 0.9f * c[3];
                    if (inView && dist2 < s.evictDistance * s.evictDistance)
                    {
                        gone = true; ++g_nEvictView;
                    }
                    else if (now - e.lastSeen > s.cacheTime)
                    {
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
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);
}

void ShadowSetPhase(bool recording)
{
    g_recording = recording && g_cfg.shadow.enabled && !g_failed;
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
    else
    {
        D3DMATRIX cv, cp;
        d->GetTransform(dev, D3DTS_VIEW, &cv);
        d->GetTransform(dev, D3DTS_PROJECTION, &cp);
        VoteCamera(cv, cp);
    }
    D3DVIEWPORT9 vp = {};
    if (SUCCEEDED(d->GetViewport(dev, &vp)))
        VoteSlice(vp.MinZ, vp.MaxZ);

    g_frame.push_back(r);
}

void ShadowWorldEnded(IDirect3DDevice9* dev)
{
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
        ReleaseFrame();
        return;
    }
    const bool havePlayer = ClientPlayer(pl);
    if (!havePlayer) { pl[0] = cam[0]; pl[1] = cam[1]; pl[2] = cam[2]; }

    D3DMATRIX camVP, camVPInv;
    Mul(view, proj, camVP);
    if (!Invert(camVP, camVPInv))
    {
        ReleaseFrame();
        return;
    }

    const double now = Now();
    const double t0  = now;
    const UINT recorded = static_cast<UINT>(g_frame.size());
    g_nRefreshed = g_nAdded = g_nEvictView = g_nEvictAge = g_nEvictCap = 0;
    if (logThis)
        Log("shadow: this frame's world draws: %u seen, recorded %u; rejected -- depth test off %u, depth writes "
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
        if (!g_cache.empty())
            g_failed = true;
        return;
    }

    // The sun camera, camera-relative: centred on the player, looking down the sun, `range` yards either
    // side and `depth` yards toward the sun and away from it.
    const float centre[3] = { pl[0] - cam[0], pl[1] - cam[1], pl[2] - cam[2] };
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

    g_valid = true;
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
    ClearCache();          // the client's buffers are rebuilt across a Reset: nothing cached stays valid
    ReleaseResources();
    g_mirrorValid = false;
    g_failed = false;
}

void ShadowProbe()
{
    g_logNext = true;
}
