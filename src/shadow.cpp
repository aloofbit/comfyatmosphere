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
//               trees drifted across those boundaries and were re-added every few frames (472 new entries
//               a frame, a cache of 2500 duplicates): a model's reference point is worked out through the
//               camera, which moves during the frame, so it shifts a little from frame to frame. (Classic
//               WoW trees do not sway; an older version of this comment said they did.)
//               Matching by nearness keeps a tree one entry, and lets a walking character's entry
//               move with it instead of leaving a trail. Each entry matches at most once a frame.
//   Eviction    An entry is gone if it was NOT drawn this frame although it sits in view and near: a
//               character that walked off, a mesh that switched level of detail. Out of view it stays
//               while it can still cast into the map: until its reference point is more than [shadow]
//               range + keepMargin yards from the player, across the ground. Until 2026-09-29 it stayed for
//               cacheTime seconds instead, and a tree beside or behind you left the map 8 seconds after you
//               last looked at it, with its shadow on the ground in front of you; turning round brought it
//               back, so shadows jumped in and out. The margin is wide because a model's reference point is
//               its first bone, which for some models sits far from the geometry (measured: trees inside the
//               map reading 85-95 yards away), and a low sun throws long shadows. cacheTime is still there
//               as an extra limit, 0 (off) by default. Something that moves is gone the moment it is not
//               drawn. The cap bounds the rest.
//   Replay      Into a 2048x2048 INTZ depth texture (readable, like depth.cpp's), colour writes off, no
//               culling (leaves are two-sided), alpha test kept so foliage casts leaf-shaped shadows.
//               Each entry is put back relative to the current camera: fixed-function under the sun's view
//               and projection, shader draws with c2..c5 = A * T(-camera) * sunViewProj.
//
// Leaf maps (added 2026-09-29): the far and the near map each come as two, the solid things (terrain,
// buildings, trunks, characters, doodads) and the leaves (the alpha-tested draws: leaves, bushes, the
// grass-like doodads). Leaves let part of the sun through ([sunshadows] leafShade): under a forest canopy
// the ground was in full shade, and a character's shadow on it changed nothing, so characters looked
// afloat. With leaves at part shade, anything solid under them shades the rest. Nothing is decided by
// guesswork: alpha test is a property of the draw. Four ways of telling characters from doodads were
// tried first the same day (many bones, near the player, bones moving, a unit standing there), and each
// let something through; a model that changed sides flickered as you passed it.
//
// The near map (added 2026-09-29, for the sun shadows on the world): the same cache replayed a second time
// into a second map of the same size that covers only [shadow] nearRange yards either side of the player,
// with the same depth toward the sun, so a tall tree further off still shades the near ground. At the
// default 64 on a 4096 map a texel is 0.03 yards against the far map's 0.12, which was too coarse for a trunk, a post or
// a character. Models whose reference point is well outside the small box are not drawn into it. The
// volumetric light reads the far map only.
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
#include "sun.h"
#include "shadow.h"
#include "terrainshade.h"
#include "mapterrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <string>
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
        UINT                         nregs;       // ...and how many registers it holds
        UINT                         nregsOwn;    // the registers this draw's own uploads reached (see RecordDraw)
        uint64_t                     seq;         // writes seen when this draw was recorded
        float                        minZ, maxZ;  // the viewport's depth slice, to tell world draws apart
        bool                         hasProj;     // fixed-function: the projection it was drawn with
        bool                         terrain;     // drawn with the terrain's pixel shader
        float                        proj00, proj22, proj32;
    };

    std::vector<Rec>   g_frame;
    std::vector<float> g_constPool;
    float              g_mirror[256 * 4];   // the vertex-shader constants as the client last set them
    bool               g_mirrorValid = false;
    bool               g_recording   = false;
    // Whether this frame redraws the map. Only such a frame records the client's draws in full and brings
    // the cache up to date; the others only vote on the world camera, which the light needs every frame.
    // Decided at the end of the frame before, so the recording knows from the first draw.
    bool               g_fullFrame   = true;
    // The fog alone (2026-10-02): the frame only votes on the world camera, and no map is drawn.
    bool               g_votesOnly   = false;
    unsigned           g_mapTick     = 0;
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
            // The world's slice starts at 0; the sky and the far scenery come after it (0.94..1 here).
            // By votes alone, looking level toward the sun, the far terrain drawn in the sky's slice
            // outnumbered the world's draws: every world pixel was then rebuilt at the near plane, and the
            // sun shadows turned the whole view to shade (2026-09-29). So the slice from 0 is the world's
            // when it has a few draws; the most votes only when none starts there.
            // Even one draw will do: tilting the camera up to the sky left fewer than 8 world draws at
            // times, the sky's slice won again, and the view went dark. With no slice from 0 at all this
            // frame, the world's slice stays what it was; the most votes decide only before any is known.
            static bool known = false;
            const Slice* best = nullptr;
            for (int i = 0; i < g_sliceCount; ++i)
                if (g_slices[i].minZ == 0.0f && g_slices[i].votes >= 1)
                {
                    best = &g_slices[i];
                    break;
                }
            if (!best && !known)
            {
                best = &g_slices[0];
                for (int i = 1; i < g_sliceCount; ++i)
                    if (g_slices[i].votes > best->votes) best = &g_slices[i];
            }
            if (best)
            {
                g_worldMinZ = best->minZ;
                g_worldMaxZ = best->maxZ;
                known = known || best->minZ == 0.0f;
            }
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
        double             claimed = -1.0; // == now when a draw this frame stands within kMatchRadius of it
        bool               unit = false;   // seen where a unit stands: a character's, never leaves (sticky)
        unsigned           drawnFor = 0;   // map redraws it was drawn on (see Evict: kBrief)
        bool               drifts = false; // has moved past stillRadius once: the still rule no longer holds it
        float              posEnd[3];      // trace only: pos with camAddr as read at the end of the world
        float              spread = 0.0f;  // yards from pos to its farthest bone: a batch of models (see Spread)
    };

    // Each key holds the instances of that model, wherever they stand.
    std::unordered_map<Key, std::vector<Entry>, KeyHash> g_cache;
    // Which entries the last redraw drew, for the probe's list of those near you: bit 1 the far map, 2 the near.
    std::unordered_map<const Entry*, unsigned char> g_replayed;
    size_t           g_entries = 0;
    constexpr size_t kMaxCache = 5000;
    constexpr float  kMatchRadius = 3.0f;   // yards a recorded draw may be from an instance and still be it
    // Anything that moves faster than kMatchRadius a frame looked like a new object every frame: a bird
    // flying by at 3.5 yards a frame left a new caster in the map each time, a trail of birds that all
    // shaded the air until they aged out. So when nothing is near enough, an instance of the same model
    // that the client did NOT draw this frame, within this much, is taken to be it, moved; but only one
    // the client drew the frame before, whose stored place is still in view. Without those two tests a
    // forest of the same tree took every tree that left the screen as one that had moved to the tree that
    // came in: the old tree's shadow went from where it stood, the entry was marked moving and left the
    // map as soon as it was not drawn, and shadows hopped from tree to tree as the camera turned (525 of
    // 3,833 entries marked moving, 2026-09-29). Something that moves was drawn a frame ago, near where
    // it is now, so in view; a tree that just left the screen was not in view.
    //
    // 15 yards, and never an instance that has stood still for kSettled draws unless it is a unit's
    // (2026-09-30). It was 60, with no such test. In Stormwind a model is drawn with 3,664 vertices one
    // frame and 4,122 the next, which is a key of its own, so as you walked its draw found no entry within
    // kMatchRadius, and the move rule took another copy of it 51 yards off that had just switched the other
    // way. Up to 10 of 11 copies a frame were swapped like this; each was marked moving and left the map
    // as soon as it was not drawn, and their shadows blinked while you walked and settled when you stopped.
    // A draw that finds nothing now becomes an entry of its own, beside the other key's.
    constexpr float  kMoveRadius = 15.0f;
    constexpr unsigned kSettled = 20;

    // Refreshed / added / evicted this frame, for the probe.
    UINT g_nRefreshed = 0, g_nAdded = 0, g_nEvictView = 0, g_nEvictAge = 0, g_nEvictCap = 0;

    // The models known to have an alpha-tested part (a tree, a bush), by vertex buffer and shader: once
    // seen, for good, until the cache is cleared. Built afresh each replay it lost trees as you played:
    // a tree's leaf entries left the cache before its trunk, or a leaf batch was drawn once without the
    // alpha test, and the trunk went back to the solid map (2026-09-29).
    std::unordered_set<unsigned long long> g_leafModels;

    unsigned long long ModelKey(const void* vb, const void* vs)
    {
        return (static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vb)) << 32) ^
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vs));
    }

    // The models the cache has at a unit, by vertex buffer and shader, rebuilt at each replay: the body
    // mark (bodymask.cpp) asks it about each draw as the client makes it (2026-10-01). A model's place is
    // known only once the frame is merged, so a unit that has just come into view is marked a frame late.
    std::unordered_set<unsigned long long> g_unitModels;

    void ClearCache()
    {
        g_leafModels.clear();
        g_unitModels.clear();
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
    // frames: in how many different frames the client wrote the buffer. A static buffer is written once,
    // when it is loaded; a buffer written in two frames or more is one the client streams through.
    struct BufferWrites { Write ring[16]; int next = 0; uint64_t last = 0; unsigned frames = 0; unsigned lastFrame = 0; };
    std::unordered_map<const void*, BufferWrites> g_written;
    uint64_t g_writeSeq = 0;
    unsigned g_frameId  = 1;                 // counted at Present, for BufferWrites::frames
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

    // ---------------------------------------------------------------------------------------------
    // copies of the geometry the client streams
    //
    // Terrain and buildings come through a buffer the client re-fills as it draws, so what a cached entry
    // points at is someone else's geometry by the end of the frame: the mountain between you and the sun
    // held no shade, and flickered as the odd frame survived. The vertices themselves do not change, so
    // the first time a chunk is seen it is copied into a buffer of our own and that copy is used for good.
    // The copy reads the client's memory back, which is slow, so only a few are taken each frame.
    struct ArenaKey
    {
        UINT  verts, prims;
        int   x, y, z;                     // its place in the world, to a tenth of a yard
        bool operator==(const ArenaKey& o) const
        {
            return verts == o.verts && prims == o.prims && x == o.x && y == o.y && z == o.z;
        }
    };
    struct ArenaKeyHash
    {
        size_t operator()(const ArenaKey& k) const
        {
            size_t h = k.verts * 2654435761u;
            auto mix = [&h](size_t v) { h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2); };
            mix(k.prims); mix(static_cast<size_t>(k.x)); mix(static_cast<size_t>(k.y));
            mix(static_cast<size_t>(k.z));
            return h;
        }
    };
    struct ArenaCopy
    {
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9*  ib = nullptr;
        UINT stride = 0, firstVertex = 0;
        double lastUsed = 0.0;       // when a draw last used it, for eviction
    };
    std::unordered_map<ArenaKey, ArenaCopy, ArenaKeyHash> g_copies;
    unsigned g_copiesTaken = 0;      // this frame
    unsigned g_copiesMoved = 0;      // copies that followed a moving object, since the last probe
    double   g_copyNow     = 0.0;    // the time at this frame's recording
    double   g_copySwept   = 0.0;    // when the full store was last swept

    // With the store full, frees the copies no draw has used for cacheTime: a caster out of view that long
    // has left the cache as well. Until 2026-09-25 a copy lived until the next device reset, so after
    // copyMax distinct chunks nothing new was copied: logged in at Stormwind, the store held 2048 and the
    // city's buildings and ground were missing from the map. A cache entry holds its own references, so
    // an entry still drawing a freed copy keeps its buffers.
    void SweepCopies()
    {
        if (g_copyNow - g_copySwept < 1.0)
            return;
        g_copySwept = g_copyNow;
        // 8 seconds unless cacheTime sets a limit: with cacheTime 0 (no limit on the cache itself, since
        // 2026-09-29) the store still frees what no draw has used for a while.
        const double stale = g_cfg.shadow.cacheTime > 1.0f ? g_cfg.shadow.cacheTime : 8.0;
        for (auto it = g_copies.begin(); it != g_copies.end();)
        {
            if (g_copyNow - it->second.lastUsed > stale)
            {
                SafeRelease(it->second.vb);
                SafeRelease(it->second.ib);
                it = g_copies.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    unsigned g_copyFailed  = 0;
    unsigned g_copyRefusedStream2 = 0;   // arena draws not copied: they use a second vertex stream
    unsigned g_copyRefusedOther   = 0;   // ...no vertex buffer, no stride or no vertices
    // camAddr when the frame's recording began. camAddr moves during the frame: a trace while walking
    // measured 0.27 yards on average between the start of the frame and the end of the world, in 173 of
    // 180 frames. Model (M2) matrices are relative to the start value, fixed-function draws to the end
    // value (see Merge).
    float    g_camAtBegin[3] = {};
    bool     g_haveCamAtBegin = false;
    bool     g_copying     = false;  // our own writes must not count as the client re-filling a buffer
    uint64_t g_frameSeqStart = 0;    // writes seen when this frame began

    // The vertices this draw uses and its indices, into buffers of our own. The indices are taken as they
    // are and the vertices from minIndex on, so the draw is re-issued with baseVertex making up the
    // difference. Both locks are read-only on the client's side; ours are ours, so the write watch is
    // told to ignore them.
    UINT VertsForPrims(D3DPRIMITIVETYPE prim, UINT primCount)
    {
        switch (prim)
        {
        case D3DPT_POINTLIST:     return primCount;
        case D3DPT_LINELIST:      return primCount * 2;
        case D3DPT_LINESTRIP:     return primCount + 1;
        case D3DPT_TRIANGLELIST:  return primCount * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN:   return primCount + 2;
        default:                  return 0;
        }
    }

    bool TakeCopy(IDirect3DDevice9* dev, const Rec& r, ArenaCopy& out)
    {
        if (r.vb[1])                                     // one stream only: a second would need its own copy
        {
            ++g_copyRefusedStream2;
            return false;
        }
        const UINT first = static_cast<UINT>(r.baseVertex + r.minIndex);
        const UINT bytes = r.numVertices * r.vbStride[0];
        if (!r.vb[0] || !r.vbStride[0] || !bytes)
        {
            ++g_copyRefusedOther;
            return false;
        }

        auto* d = dev->lpVtbl;
        g_copying = true;
        bool ok = false;
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9*  ib = nullptr;
        if (SUCCEEDED(d->CreateVertexBuffer(dev, bytes, 0, 0, D3DPOOL_MANAGED, &vb, nullptr)) && vb)
        {
            void* src = nullptr;
            void* dst = nullptr;
            if (SUCCEEDED(r.vb[0]->lpVtbl->Lock(r.vb[0], r.vbOffset[0] + first * r.vbStride[0], bytes, &src,
                                                D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) && src)
            {
                if (SUCCEEDED(vb->lpVtbl->Lock(vb, 0, bytes, &dst, 0)) && dst)
                {
                    memcpy(dst, src, bytes);
                    vb->lpVtbl->Unlock(vb);
                    ok = true;
                }
                r.vb[0]->lpVtbl->Unlock(r.vb[0]);
            }
        }

        if (ok && r.indexed && r.ib)
        {
            ok = false;
            D3DINDEXBUFFER_DESC id = {};
            r.ib->lpVtbl->GetDesc(r.ib, &id);
            const UINT idxSize = id.Format == D3DFMT_INDEX32 ? 4u : 2u;
            const UINT idxCount = VertsForPrims(r.prim, r.primCount);
            const UINT idxBytes = idxCount * idxSize;
            if (idxBytes && SUCCEEDED(d->CreateIndexBuffer(dev, idxBytes, 0, id.Format, D3DPOOL_MANAGED,
                                                           &ib, nullptr)) && ib)
            {
                void* src = nullptr;
                void* dst = nullptr;
                if (SUCCEEDED(r.ib->lpVtbl->Lock(r.ib, r.startIndex * idxSize, idxBytes, &src,
                                                 D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) && src)
                {
                    if (SUCCEEDED(ib->lpVtbl->Lock(ib, 0, idxBytes, &dst, 0)) && dst)
                    {
                        memcpy(dst, src, idxBytes);
                        ib->lpVtbl->Unlock(ib);
                        ok = true;
                    }
                    r.ib->lpVtbl->Unlock(r.ib);
                }
            }
        }
        g_copying = false;

        if (!ok)
        {
            SafeRelease(vb);
            SafeRelease(ib);
            ++g_copyFailed;
            return false;
        }
        out.vb = vb;
        out.ib = ib;
        out.stride = r.vbStride[0];
        out.firstVertex = first;
        return true;
    }

    // Records dropped this frame because they were not drawn with the world's camera and depth slice.
    unsigned g_nOffWorld = 0;
    double   g_replaySeconds = 0.0;          // what this frame's replay cost
    unsigned g_replaySkipped = 0;            // entries outside the map, not drawn

    // For the benchmark only (ShadowTiming): CPU time in the three parts, summed over frames until taken.
    bool     g_timing       = false;
    double   g_tRecord      = 0.0;           // RecordDraw, over the whole world pass
    double   g_tCache       = 0.0;           // Merge and Evict
    double   g_tMerge       = 0.0;           // ...of which Merge
    unsigned g_tStill       = 0;             // models seen again that had not moved
    unsigned g_tRefreshed   = 0;             // entries seen again
    double   g_tReplay      = 0.0;           // drawing the cache into the map
    unsigned g_tDrawn       = 0;             // casters drawn into the map
    unsigned g_tEntries     = 0;             // entries in the cache
    unsigned g_tFrames      = 0;             // frames the cache was kept
    unsigned g_tReplays     = 0;             // frames the map was drawn ([shadow] mapEvery)

    // The absolute transform derived for the frame's first M2 record, against the camera's own rotation:
    // if an entry's frame of reference is the camera, keeping it across frames cannot work.
    char g_frameInfo[360] = {};
    char g_frameInfo2[800] = {};

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

    // What was added and dropped within kNearTrace yards of you, and why (2026-09-30): the planter's shadow
    // blinked as you walked, and the lists above are taken up by what is far off.
    constexpr float kNearTrace = 40.0f;
    char g_nearInfo[1200] = {};
    int  g_nearInfoLen   = 0;
    int  g_nearInfoCount = 0;

    // The figures of the last second, for the on-screen stats (/atmos stats, ShadowStatsText).
    struct SecStats
    {
        unsigned frames = 0, added = 0, evView = 0, evAge = 0, evWritten = 0, evCap = 0;
        unsigned nearAdded = 0, nearGone = 0, nearRefused = 0, mostDrawn = 0;
        unsigned seen = 0, recorded = 0, rejZW = 0, rejBlend = 0, rejDynamic = 0, recFrames = 0;
        std::map<std::string, unsigned> nearWhy;
    };
    SecStats g_sec;

    void NoteNear(const char* why, const Rec& r, const float pos[3], const float player[3], bool have, unsigned drawnFor)
    {
        if (!have)
            return;
        const float dx = pos[0] - player[0], dy = pos[1] - player[1], dz = pos[2] - player[2];
        if (dx * dx + dy * dy + dz * dz > kNearTrace * kNearTrace)
            return;
        if (strcmp(why, "new") == 0)
            ++g_sec.nearAdded;
        else if (strncmp(why, "refused", 7) == 0)
            ++g_sec.nearRefused;
        else
        {
            ++g_sec.nearGone;
            ++g_sec.nearWhy[why];
        }
        if (!g_cfg.trace || g_nearInfoCount >= 12)
            return;
        ++g_nearInfoCount;
        g_nearInfoLen += _snprintf_s(g_nearInfo + g_nearInfoLen, sizeof(g_nearInfo) - g_nearInfoLen, _TRUNCATE,
            " [%s: %s %uv %up base %d min %u start %u, drawn %u, %.0f yd at (%.1f %.1f %.1f)]", why, r.vs ? "M2" : "ff",
            r.numVertices, r.primCount, r.baseVertex, r.minIndex, r.startIndex, drawnFor,
            sqrtf(dx * dx + dy * dy + dz * dz), pos[0], pos[1], pos[2]);
    }

    // And what the client overwrote under us, again on its own.
    char g_overInfo[400] = {};
    int  g_overInfoLen   = 0;
    int  g_overInfoCount = 0;

    // A few new entries' positions relative to the player, for the probe: are they where trees stand?
    int   g_samplesLeft = 0;
    float g_logPlayer[3] = {};

    // The frame's records into the cache. camVPInv takes the frame's camera out of shader draws; cam is
    // the camera's absolute position, to put fixed-function draws back into absolute coordinates.
    // Trace only: how far a still object's stored position drifts from one frame to the next, with the
    // camera read when the frame began ("start") and at the end of the world ("end"), for models (M2,
    // shader) and fixed-function draws apart. The right camera gives about zero drift.
    double   g_prevMergeNow = 0.0;
    double   g_driftSum[2][2] = {};          // [M2, ff][start, end]
    unsigned g_driftN[2] = {};
    // Trace only: large models (over 4000 vertices, buildings): matched in place and how far they
    // shifted, matched only by the move rule and how far they jumped, and added new, this frame.
    unsigned g_bigNear = 0, g_bigMoved = 0, g_bigNew = 0;
    float    g_bigNearShift = 0.0f, g_bigMoveJump = 0.0f;

    // cam: camAddr at the end of the world, which fixed-function draws are relative to (a fixed-function
    // object's stored position drifted 0.0000 yards a frame with it, by trace). camModels: the camera the
    // model (M2) matrices are taken back to. It is the same value now; camAddr as the frame began was tried
    // for models on 2026-09-24 and did not stop a distant tower jumping, because the jump came from the
    // projection (see IsProjection below).
    // A D3D perspective projection and nothing else: no rotation, no translation, w = z.
    bool IsProjection(const D3DMATRIX& m)
    {
        const float eps = 1e-3f;
        return fabsf(m.m[0][1]) < eps && fabsf(m.m[0][2]) < eps && fabsf(m.m[0][3]) < eps &&
               fabsf(m.m[1][0]) < eps && fabsf(m.m[1][2]) < eps && fabsf(m.m[1][3]) < eps &&
               fabsf(m.m[2][0]) < eps && fabsf(m.m[2][1]) < eps && fabsf(m.m[2][3] - 1.0f) < eps &&
               fabsf(m.m[3][0]) < eps && fabsf(m.m[3][1]) < eps && fabsf(m.m[3][3]) < eps &&
               fabsf(m.m[0][0]) > eps && fabsf(m.m[1][1]) > eps;
    }
    unsigned g_nProjOnly = 0;                // this frame: model records with a projection alone in c2..c5
    unsigned g_nStill    = 0;                // this frame: models seen again that had not moved (not copied)

    int g_unitsSeen = 0;                    // for the probe: units and players known

    // What the files draw (mapterrain.cpp) is not kept here (2026-09-30): a terrain chunk in a loaded tile,
    // a loaded building's opaque groups, a doodad from a loaded tile. The cache then holds what the files do
    // not have: characters, creatures, the server's objects, cut-out parts of buildings, and anything past
    // the tiles loaded. Not a model where a unit stands: a character standing on a doodad's place is still
    // a character. The units are taken once a full frame, into 2-yard cells.
    unsigned g_nFilesRefused = 0, g_nFilesEvicted = 0;   // this frame
    // After a probe, the first few building draws the files cover are logged with the turn the client draws
    // them with and the turn the files give (2026-09-30): Stormwind's shade came from the wrong district, and
    // the cover test matches the place only.
    int g_refusedLogs = 0;
    std::unordered_map<long long, std::vector<int>> g_filesUnitCells;
    float g_filesUnits[512][3];

    void TakeUnits()
    {
        g_filesUnitCells.clear();
        const int n = ClientUnits(g_filesUnits, 512);
        for (int i = 0; i < n; ++i)
            g_filesUnitCells[(static_cast<long long>(floorf(g_filesUnits[i][0] * 0.5f)) << 32) ^
                             (static_cast<long long>(floorf(g_filesUnits[i][1] * 0.5f)) & 0xFFFFFFFFll)].push_back(i);
    }

    bool UnitAt(const float pos[3])
    {
        const long long cx = static_cast<long long>(floorf(pos[0] * 0.5f));
        const long long cy = static_cast<long long>(floorf(pos[1] * 0.5f));
        for (long long ox = -1; ox <= 1; ++ox)
            for (long long oy = -1; oy <= 1; ++oy)
            {
                auto it = g_filesUnitCells.find(((cx + ox) << 32) ^ ((cy + oy) & 0xFFFFFFFFll));
                if (it == g_filesUnitCells.end())
                    continue;
                for (int i : it->second)
                {
                    const float dx = pos[0] - g_filesUnits[i][0], dy = pos[1] - g_filesUnits[i][1],
                                dz = pos[2] - g_filesUnits[i][2];
                    if (dx * dx + dy * dy < 0.25f && dz > -4.0f && dz < 4.0f)
                        return true;
                }
            }
        return false;
    }

    // A model's place in the cache is its first bone's origin. Which point is the doodad's own place
    // depends on where the client put the placement (2026-09-30, measured in Elwynn):
    //   - in c2..c5 (a bush): the absolute transform's origin is the placement, and so is the first bone's;
    //   - in the bones (the canopy trees; c2..c5 then holds the projection alone): the transform's origin is
    //     the camera, and the first bone stood 2 to 25 yards from the placement, a pose of its own. A bone
    //     with no pose of its own maps the model's origin to the placement exactly, so every bone is asked,
    //     to a tenth of a yard: a character's hand beside a bush is not a bush. Only the registers the draw
    //     uploaded itself (nregsOwn): past them the snapshot holds the bones of models drawn before it.
    // With the first bone alone, 283 draws were refused a frame and 4,765 models kept.
    // How far a shader draw's bones reach from its reference point (2026-09-30). In Stormwind the client draws
    // planters in batches, up to 10 copies of a 458-vertex model in one draw with a bone each, and an entry's
    // place is its first bone: the first planter of the batch, which as you moved could be 40 yards off. The
    // map left the batch out by that place, and with it the planter beside you; which planter came first
    // changed as you walked and turned, so its shadow came and went, and was gone once you stood still.
    // Only the registers the draw uploaded itself (nregs here), as in FromFiles.
    float Spread(const float* consts, UINT nregs, const D3DMATRIX& absolute, const float pos[3])
    {
        float most = 0.0f;
        for (UINT k = 0; consts && 34 + 3 * k <= nregs && k < 64; ++k)
        {
            const float b[3] = { consts[(31 + 3 * k) * 4 + 3], consts[(32 + 3 * k) * 4 + 3], consts[(33 + 3 * k) * 4 + 3] };
            float d2 = 0.0f;
            for (int j = 0; j < 3; ++j)
            {
                const float w = b[0] * absolute.m[0][j] + b[1] * absolute.m[1][j] + b[2] * absolute.m[2][j] +
                                absolute.m[3][j];
                d2 += (w - pos[j]) * (w - pos[j]);
            }
            most = (std::max)(most, d2);
        }
        most = sqrtf(most);
        return std::isfinite(most) ? (std::min)(most, 300.0f) : 0.0f;
    }

    bool FromFiles(const Rec& r, const float pos[3], const D3DMATRIX& absolute, const float* consts, UINT nregs)
    {
        if (!g_cfg.shadow.mapTerrain)
            return false;
        if (r.terrain)   // its place is its corner, the largest x and y: a yard inside finds the tile
            return MapTerrainCovers(pos[0] - 1.0f, pos[1] - 1.0f);
        // The far horizon: the client draws the distant ground again, coarse, in the sky's depth slice, and
        // [shadow] horizon keeps those draws, which are not drawn with the terrain's shader. Where a tile from
        // the files covers the ground, the coarse copy floated over every dip once you came close, and cast a
        // soft blob with nothing above it (2026-09-30, Gavin's Naze, gone after a restart).
        if (!r.vs && (r.minZ != g_worldMinZ || r.maxZ != g_worldMaxZ))
            return MapTerrainCovers(pos[0] - 1.0f, pos[1] - 1.0f) || MapTerrainCovers(pos[0], pos[1]);
        if (!r.vs)       // a building's group is drawn with the placement's matrix
            return !r.alphaTest && MapBuildingCovers(pos);
        if (UnitAt(pos))
            return false;
        const float origin[3] = { absolute.m[3][0], absolute.m[3][1], absolute.m[3][2] };
        if (MapDoodadCovers(pos) || MapDoodadCovers(origin))
            return !UnitAt(origin);
        for (UINT k = 0; consts && 34 + 3 * k <= nregs && k < 64; ++k)
        {
            const float b[3] = { consts[(31 + 3 * k) * 4 + 3], consts[(32 + 3 * k) * 4 + 3], consts[(33 + 3 * k) * 4 + 3] };
            float w[3];
            for (int j = 0; j < 3; ++j)
                w[j] = b[0] * absolute.m[0][j] + b[1] * absolute.m[1][j] + b[2] * absolute.m[2][j] + absolute.m[3][j];
            if (MapDoodadCovers(w, 0.1f))
                return true;
        }
        return false;
    }
    constexpr float kPlayerModels = 3.0f;   // yards from the player (1 yard above the feet): always placed again


    void Merge(const D3DMATRIX& camVP, const D3DMATRIX& camVPInv, const float cam[3], const float camModels[3],
               double now)
    {
        float camEnd[3] = { cam[0], cam[1], cam[2] };
        if (g_cfg.trace)
            ClientCamera(camEnd);
        const float delta[3] = { camEnd[0] - cam[0], camEnd[1] - cam[1], camEnd[2] - cam[2] };
        g_driftSum[0][0] = g_driftSum[0][1] = g_driftSum[1][0] = g_driftSum[1][1] = 0.0;
        g_driftN[0] = g_driftN[1] = 0;
        g_bigNear = g_bigMoved = g_bigNew = 0;
        g_nProjOnly = 0;
        g_nStill = 0;
        g_bigNearShift = g_bigMoveJump = 0.0f;
        D3DMATRIX toAbs;
        Translation(camModels[0], camModels[1], camModels[2], toAbs);
        D3DMATRIX camOut;
        Mul(camVPInv, toAbs, camOut);   // camera clip -> absolute world
        g_newInfo[0] = 0; g_newInfoLen = 0; g_newInfoCount = 0;
        g_dropInfo[0] = 0; g_dropInfoLen = 0; g_dropInfoCount = 0;
        g_overInfo[0] = 0; g_overInfoLen = 0; g_overInfoCount = 0;
        g_nearInfo[0] = 0; g_nearInfoLen = 0; g_nearInfoCount = 0;
        g_nChanged = 0; g_maxDiff = 0.0f; g_diffInfo[0] = 0;
        g_nOffWorld = 0;
        g_frameInfo[0] = 0;
        g_nFilesRefused = 0;
        TakeUnits();
        // The player's own models (the character, a mount, a pet) are always placed again: see below.
        float player[3] = {};
        const bool havePlayer = ClientPlayer(player);
        player[2] += 1.0f;

        // Three passes. First every draw is placed. Then each claims the nearest instance of its model
        // within kMatchRadius. Only then are they matched, and the moving rule may not take an instance
        // another draw this frame has claimed. In one pass, a copy of a tree that came into view before the
        // tree already stored was matched took that tree's entry as "moved" (2026-09-29).
        struct Placed { bool ok; D3DMATRIX absolute; float pos[3]; };
        std::vector<Placed> placed(g_frame.size());
        for (size_t fi = 0; fi < g_frame.size(); ++fi)
        {
            Rec& r = g_frame[fi];
            placed[fi].ok = false;
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
                if (g_cfg.trace && g_dropInfoCount < 5)   // the trace: what the world filter throws away
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
            Placed& pl = placed[fi];
            if (r.vs)
            {
                const float* c = &g_constPool[r.consts];
                D3DMATRIX m;
                FromRegisters(&c[2 * 4], m);
                // Some models upload only the projection in c2..c5 and carry world and view in the bones.
                // Dividing the world camera's projection out of that left whatever the two projections
                // differ by, in the depth direction: a tower 200 yards away had one row of A change by
                // 0.008 from frame to frame with the view unchanged, 1.6 yards at its distance, and it
                // jumped in the shadow map as you walked. For those, A is the inverse of the view (a pure
                // rotation here) and the camera's position, with no projection in it at all.
                if (IsProjection(m) && g_haveWorldCam)
                {
                    D3DMATRIX& a = e.absolute;
                    a = {};
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            a.m[i][j] = g_worldView.m[j][i];
                    a.m[3][0] = camModels[0]; a.m[3][1] = camModels[1]; a.m[3][2] = camModels[2];
                    a.m[3][3] = 1.0f;
                    ++g_nProjOnly;
                }
                else
                {
                    Mul(m, camOut, e.absolute);
                }
                if (g_cfg.trace && !g_frameInfo[0])
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
            if (FromFiles(r, pos, e.absolute, r.vs ? &g_constPool[r.consts] : nullptr, (std::min)(r.nregs, r.nregsOwn)))
            {
                NoteNear("refused, the files place it", r, pos, player, havePlayer, 0);
                if (!r.vs && !r.terrain && g_refusedLogs > 0)
                {
                    --g_refusedLogs;
                    float bp[3], br[3][3];
                    char bn[160];
                    const D3DMATRIX& a = e.absolute;
                    if (MapBuildingNearest(pos, bp, br, bn, sizeof(bn)))
                        Log("shadow: the client draws a building at (%.2f %.2f %.2f), rows (%.3f %.3f %.3f) (%.3f %.3f "
                            "%.3f) (%.3f %.3f %.3f); the files: %s at (%.2f %.2f %.2f), rows (%.3f %.3f %.3f) (%.3f "
                            "%.3f %.3f) (%.3f %.3f %.3f)", pos[0], pos[1], pos[2], a.m[0][0], a.m[0][1], a.m[0][2],
                            a.m[1][0], a.m[1][1], a.m[1][2], a.m[2][0], a.m[2][1], a.m[2][2], bn, bp[0], bp[1], bp[2],
                            br[0][0], br[0][1], br[0][2], br[1][0], br[1][1], br[1][2], br[2][0], br[2][1], br[2][2]);
                }
                ReleaseRec(r);
                r = Rec{};
                ++g_nFilesRefused;
                continue;
            }
            pl.ok = true;
            pl.absolute = e.absolute;
            memcpy(pl.pos, pos, sizeof(pos));
        }

        // Claims: the nearest unclaimed instance within kMatchRadius, for each draw.
        for (size_t fi = 0; fi < g_frame.size(); ++fi)
        {
            if (!placed[fi].ok)
                continue;
            const Rec& r = g_frame[fi];
            const Key k = { r.vb[0], r.ib, r.vs, r.baseVertex, r.minIndex, r.numVertices, r.startIndex, r.primCount };
            auto it = g_cache.find(k);
            if (it == g_cache.end())
                continue;
            Entry* nearest = nullptr;
            float  nd2 = kMatchRadius * kMatchRadius;
            for (Entry& cand : it->second)
            {
                if (cand.claimed == now)
                    continue;
                const float dx = cand.pos[0] - placed[fi].pos[0], dy = cand.pos[1] - placed[fi].pos[1],
                            dz = cand.pos[2] - placed[fi].pos[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 <= nd2) { nd2 = d2; nearest = &cand; }
            }
            if (nearest)
                nearest->claimed = now;
        }

        for (size_t fi = 0; fi < g_frame.size(); ++fi)
        {
            if (!placed[fi].ok)
                continue;
            Rec& r = g_frame[fi];
            Entry e;
            e.absolute = placed[fi].absolute;
            float pos[3] = { placed[fi].pos[0], placed[fi].pos[1], placed[fi].pos[2] };

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
                    if (cand.lastSeen == now || cand.lastSeen != g_prevMergeNow || cand.claimed == now)
                        continue;                  // drawn this frame already, not drawn the frame before, or
                                                   // claimed by a draw standing where it is
                    if (!cand.mobile && !cand.drifts && !cand.unit && cand.drawnFor >= kSettled)
                        continue;                  // it has stood still: something else, not it moved
                    const float dx = cand.pos[0] - pos[0], dy = cand.pos[1] - pos[1], dz = cand.pos[2] - pos[2];
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 > moveD2)
                        continue;
                    const float rel[3] = { cand.pos[0] - cam[0], cand.pos[1] - cam[1], cand.pos[2] - cam[2] };
                    float c[4];
                    for (int j = 0; j < 4; ++j)
                        c[j] = rel[0] * camVP.m[0][j] + rel[1] * camVP.m[1][j] + rel[2] * camVP.m[2][j] + camVP.m[3][j];
                    const bool inView = c[3] > 0.0f && fabsf(c[0]) < c[3] && fabsf(c[1]) < c[3];
                    if (inView) { moveD2 = d2; best = &cand; moved = true; }
                }
            }
            if (best)
            {
                // Seen again: fresh position, matrices, constants and alpha state; same objects referenced.
                // What changed is only for the trace. Compared for every shader draw every frame, 1024
                // constants each, it cost the light CPU time with the trace off.
                if (g_cfg.trace)
                {
                    float dm = 0.0f;
                    for (int i = 0; i < 4; ++i)
                        for (int j = 0; j < 4; ++j)
                            dm = (std::max)(dm, fabsf(best->absolute.m[i][j] - e.absolute.m[i][j]));
                    float dc = 0.0f;
                    int   dcReg = -1;
                    if (r.vs && !best->consts.empty())
                    {
                        const float* nc = &g_constPool[r.consts];
                        const int n = static_cast<int>((std::min)(best->consts.size(), static_cast<size_t>(r.nregs) * 4));
                        for (int i = 0; i < n; ++i)
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
                if (g_cfg.trace && r.vs && r.numVertices > 4000)
                {
                    const float dx = best->pos[0] - pos[0], dy = best->pos[1] - pos[1], dz = best->pos[2] - pos[2];
                    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (moved) { ++g_bigMoved; g_bigMoveJump = (std::max)(g_bigMoveJump, d); }
                    else       { ++g_bigNear;  g_bigNearShift = (std::max)(g_bigNearShift, d); }
                }
                if (g_cfg.trace && best->lastSeen == g_prevMergeNow && !moved)
                {
                    const float pe[3] = { pos[0] + delta[0], pos[1] + delta[1], pos[2] + delta[2] };
                    float db = 0.0f, de = 0.0f;
                    for (int j = 0; j < 3; ++j)
                    {
                        db += (pos[j] - best->pos[j]) * (pos[j] - best->pos[j]);
                        de += (pe[j] - best->posEnd[j]) * (pe[j] - best->posEnd[j]);
                    }
                    db = sqrtf(db); de = sqrtf(de);
                    if (db < 2.0f && de < 2.0f)
                    {
                        const int kind = r.vs ? 0 : 1;
                        g_driftSum[kind][0] += db;
                        g_driftSum[kind][1] += de;
                        ++g_driftN[kind];
                    }
                }
                // A model that has not moved keeps the matrix and constants it has: they were recorded
                // together and still describe it, and copying them again (up to 3.5 KB) for every still
                // model every frame was a good part of the cache's CPU time. For many models the bones
                // are relative to the camera, so their constants change whenever the camera moves; that is
                // not the model moving. A model that does animate in place (a windmill) keeps the pose it
                // was first seen in, in the map.
                //
                // "Not moved" is within [shadow] stillRadius of the stored position. It was 0.02 yards, and
                // walking put every tree past it: the position comes through the camera, which moves by
                // one frame's walk during the frame. Each redraw then placed the tree again, up to a
                // texel off, and its leaves came out as a new pattern in the map while in game they stayed.
                //
                // Not for a model within kPlayerModels yards of the player. The player moves about 0.12 yards
                // a frame at a run, so the still rule held the character's shadow, in place and in pose,
                // for two frames and then moved it 0.3 yards at once: ten texels of the near map, which
                // flickered over the character's own body (2026-09-29).
                const float sx = best->pos[0] - pos[0], sy = best->pos[1] - pos[1], sz = best->pos[2] - pos[2];
                const float sr = g_cfg.shadow.stillRadius;
                const float px = pos[0] - player[0], py = pos[1] - player[1], pz = pos[2] - player[2];
                const bool  playerModel = havePlayer && px * px + py * py + pz * pz < kPlayerModels * kPlayerModels;
                // Nor for a model where a unit stands (2026-09-30): an NPC animating in place kept the pose it
                // was first seen in, and its shadow did not follow the animation. With the trees and doodads
                // from the files, the models left here are mostly characters and creatures.
                // Nor for a model that has once moved past stillRadius (2026-09-30): a ship's sails, drawn as
                // models, moved under 0.3 yards a frame, were held for a frame or two and then jumped, while its
                // hull (a building) followed every frame. What stands still never gets that far.
                const bool within = sx * sx + sy * sy + sz * sz < sr * sr;
                if (!within)
                    best->drifts = true;   // fixed-function too: a ship's hull (see Evict)
                // Nor for an animated doodad the files leave to the client's draws (2026-09-30): a gryphon at a
                // flight master stands still and moves its wings.
                const bool still = r.vs && !moved && !playerModel && within && !best->drifts &&
                                   !best->consts.empty() && !UnitAt(pos) && !UnitAt(&e.absolute.m[3][0]) &&
                                   !MapAnimatedDoodadAt(pos, 2.0f) && !MapAnimatedDoodadAt(&e.absolute.m[3][0], 2.0f);
                if (still)
                {
                    ++g_nStill;
                }
                else
                {
                    for (int j = 0; j < 3; ++j)
                        best->posEnd[j] = pos[j] + delta[j];
                    best->absolute = e.absolute;
                    memcpy(best->pos, pos, sizeof(pos));
                    if (r.vs)
                    {
                        best->consts.assign(&g_constPool[r.consts], &g_constPool[r.consts] + r.nregs * 4);
                        best->spread = Spread(&g_constPool[r.consts], (std::min)(r.nregs, r.nregsOwn), e.absolute, pos);
                    }
                }
                best->rec.alphaTest = r.alphaTest; best->rec.alphaRef = r.alphaRef; best->rec.alphaFunc = r.alphaFunc;
                if (r.nregsOwn > best->rec.nregsOwn)
                    best->rec.nregsOwn = r.nregsOwn;
                best->seq = r.seq;
                best->mobile = best->mobile || moved;
                best->unit = best->unit || (r.vs && UnitAt(pos));
                best->lastSeen = now;
                ++best->drawnFor;
                ReleaseRec(r);
                ++g_nRefreshed;
            }
            else
            {
                e.rec = r;                         // the entry takes over the references
                if (r.vs)
                {
                    e.consts.assign(&g_constPool[r.consts], &g_constPool[r.consts] + r.nregs * 4);
                    e.spread = Spread(&g_constPool[r.consts], (std::min)(r.nregs, r.nregsOwn), e.absolute, pos);
                }
                memcpy(e.pos, pos, sizeof(pos));
                for (int j = 0; j < 3; ++j)
                    e.posEnd[j] = pos[j] + delta[j];
                if (g_cfg.trace && r.vs && r.numVertices > 4000)
                    ++g_bigNew;
                e.mobile = false;
                e.unit = r.vs && UnitAt(pos);
                e.seq = r.seq;
                e.lastSeen = now;
                e.drawnFor = 1;
                NoteNear("new", r, pos, player, havePlayer, 1);
                list.push_back(std::move(e));
                ++g_entries;
                ++g_nAdded;
                if (g_cfg.trace && g_newInfoCount < 5)
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
        g_prevMergeNow = now;
    }

    // Gone if unseen this frame while in view and near; else aged out, or beyond the map's reach.
    void Evict(const D3DMATRIX& camVP, const float cam[3], const float player[3], double now)
    {
        const ShadowSettings& s = g_cfg.shadow;
        const float keep = s.range + s.keepMargin;
        g_nFilesEvicted = 0;
        // Entries are checked against the files only when what the files cover has changed: new ones are
        // refused at Merge, so an entry kept is one the files did not cover when it came in.
        static unsigned filesSeen = ~0u;
        const bool filesChanged = MapFilesVersion() != filesSeen;
        filesSeen = MapFilesVersion();
        // Terrain and buildings (fixed-function) are kept out to the map's depth toward the sun: the far
        // horizon's mountains are drawn only while you look at them, and at range + keepMargin they left
        // the map as soon as you looked down, so a ridge's shade came and went as the camera tilted, and
        // the view darkened and lightened with it (2026-09-29). They are a few hundred draws.
        const float keepFixed = (std::max)(keep, s.depth);

        // One version of each terrain chunk, the most detailed seen. The client draws a chunk with a
        // coarser mesh further off (145 vertices and 256 triangles near, 41 and 64 past about 250 yards),
        // and each version was an entry of its own that nothing removed: tilting the camera toward a
        // mountain added version after version, and its shadow showed several ridges (2026-09-29). A
        // chunk's place is its corner, to a yard. Of equal versions, the one seen last stays.
        struct Best { UINT prims; double seen; };
        std::unordered_map<long long, Best> best;
        auto placeKey = [](const Entry& e) {
            return (static_cast<long long>(floorf(e.pos[0])) << 42) ^ (static_cast<long long>(floorf(e.pos[1])) << 21) ^
                   static_cast<long long>(floorf(e.pos[2]));
        };
        for (const auto& kv : g_cache)
            for (const Entry& e : kv.second)
                if (e.rec.terrain)
                {
                    Best& b = best[placeKey(e)];
                    if (e.rec.primCount > b.prims || (e.rec.primCount == b.prims && e.lastSeen > b.seen))
                        b = { e.rec.primCount, e.lastSeen };
                }
        for (auto kv = g_cache.begin(); kv != g_cache.end(); )
        {
            std::vector<Entry>& list = kv->second;
            for (size_t i = 0; i < list.size(); )
            {
                Entry& e = list[i];
                bool gone = false;
                const char* why = "";
                if (OverwrittenSince(e.rec, e.seq))
                {
                    gone = true;
                    why = "overwritten";
                    ++g_nEvictWritten;
                    if (g_cfg.trace && g_overInfoCount < 5)   // the trace: what the client overwrites under us
                    {
                        ++g_overInfoCount;
                        g_overInfoLen += _snprintf_s(g_overInfo + g_overInfoLen, sizeof(g_overInfo) - g_overInfoLen,
                            _TRUNCATE, " [%s %uv %up vb %p stride %u %s]", e.rec.vs ? "M2" : "ff",
                            e.rec.numVertices, e.rec.primCount, e.rec.vb[0], e.rec.vbStride[0],
                            e.lastSeen == now ? "drawn this frame" : "not drawn");
                    }
                }
                else if (filesChanged &&
                         FromFiles(e.rec, e.pos, e.absolute, e.consts.empty() ? nullptr : e.consts.data(),
                                   (std::min)(static_cast<UINT>(e.consts.size() / 4), e.rec.nregsOwn)))
                {
                    gone = true; ++g_nFilesEvicted;   // kept before its tile came in
                    why = "the files have it";
                }
                else if (e.rec.terrain && [&] {
                             const Best& b = best[placeKey(e)];
                             return e.rec.primCount < b.prims || (e.rec.primCount == b.prims && e.lastSeen < b.seen);
                         }())
                {
                    gone = true; ++g_nEvictView;   // a finer or later version of the same chunk is held
                    why = "finer chunk";
                }
                else if (e.lastSeen < now)
                {
                    const float rel[3] = { e.pos[0] - cam[0], e.pos[1] - cam[1], e.pos[2] - cam[2] };
                    float c[4];
                    for (int j = 0; j < 4; ++j)
                        c[j] = rel[0] * camVP.m[0][j] + rel[1] * camVP.m[1][j] + rel[2] * camVP.m[2][j] + camVP.m[3][j];
                    const float dist2 = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2];
                    const bool inView = c[3] > 0.0f && fabsf(c[0]) < 0.9f * c[3] && fabsf(c[1]) < 0.9f * c[3];
                    // With the world from the files, a fixed-function draw left in the cache is something the
                    // files do not place, which moves: a ship (2026-09-30). In view and not drawn, it has gone,
                    // at any distance: the ship at Auberdine left a band of shade along its path, 1,793 entries
                    // 50 to 150 yards off, until staleTime.
                    // Only one that has shown it moves (2026-09-30): it has moved, or it was seen on fewer than
                    // kSettled redraws (a ship's pieces, matched anew each frame). The stone frame and wooden
                    // boxes of a planter in Stormwind are fixed-function too, and the files do not place them;
                    // as you walked the client drew them from other places in its buffers, a key of their own,
                    // and the entry they stood in was in view and not drawn, so it was dropped, and their
                    // shadows blinked.
                    const bool movingFixed = s.mapTerrain && !e.rec.vs && !e.rec.terrain &&
                                             (e.mobile || e.drifts || (e.drawnFor < kSettled && now - e.lastSeen > 0.5));
                    if (inView && (movingFixed || dist2 < s.evictDistance * s.evictDistance))
                    {
                        gone = true; ++g_nEvictView;
                        why = movingFixed ? "in view, not drawn, moving fixed" : "in view, not drawn, near";
                    }
                    else
                    {
                        // Something that moves is gone the moment it stops being drawn: its shade belongs
                        // where it is now, not where it was. A unit's model, once no unit stands at its place
                        // (2026-09-30: a wolf that walked a little each frame was matched as the same entry,
                        // never marked as moving, and left its shade behind when it went out of view). Anything
                        // else, once it is out of the map's reach across the ground, or (if set) unseen longer
                        // than cacheTime.
                        const float dx = e.pos[0] - player[0], dy = e.pos[1] - player[1];
                        const float reach = e.rec.vs ? keep : keepFixed;
                        // A model drawn on fewer than kBrief redraws, once it stops being drawn (2026-09-30): a
                        // bird flew further between frames than a draw is matched over, each place became an
                        // entry of its own, drawn once or twice, and left a trail of shade. It is a unit, but its
                        // model flies high over the unit's place, so the unit rule did not take it.
                        constexpr unsigned kBrief = 20;
                        // With the world from the files, fixed-function draws too (2026-09-30): a ship's.
                        // Half a second after it was last drawn, not at once (2026-09-30). In Stormwind the client
                        // draws some models from a buffer it streams through, from another place in it as you walk
                        // (490 vertices from index 0, then 960, then 2208; and batches of 5 to 10 copies of a
                        // 458-vertex model): each place is a key of its own, back within a few frames. Dropped the
                        // first frame it was not drawn, each was gone when the map was redrawn, and the planters'
                        // shadows blinked while you walked. A bird's places are caught by the move rule, which
                        // marks them moving, and they still go at once.
                        constexpr double kBriefGrace = 0.5;
                        const bool brief = e.drawnFor < kBrief && now - e.lastSeen > kBriefGrace &&
                                           (e.rec.vs || (s.mapTerrain && !e.rec.terrain));
                        // Anything that has moved since it was first seen, once it stops being drawn, at any
                        // distance and in view or not (2026-09-30): a ship's pieces that it left out of view
                        // stayed as a dark outline of the ship until staleTime.
                        if (e.mobile || e.drifts || (e.unit && !UnitAt(e.pos)) || brief ||
                            dx * dx + dy * dy > (reach + e.spread) * (reach + e.spread) ||
                            (s.cacheTime > 0.0f && now - e.lastSeen > s.cacheTime) ||
                            (s.mapTerrain && s.staleTime > 0.0f && now - e.lastSeen > s.staleTime))
                        {
                            gone = true; ++g_nEvictAge;
                            why = e.mobile ? "moving" : e.drifts ? "has moved" : (e.unit && !UnitAt(e.pos)) ? "unit gone" :
                                  brief ? "brief" : dx * dx + dy * dy > (reach + e.spread) * (reach + e.spread) ? "out of reach" :
                                  "unseen too long";
                        }
                    }
                }
                if (gone)
                {
                    NoteNear(why, e.rec, e.pos, player, true, e.drawnFor);
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
        // Over the cap: the farthest from the player go first, across the ground. It was the longest
        // unseen until 2026-09-29, which could be a tree right beside you that you had not looked at, and
        // its shadow went. Finding them one at a time meant a pass over the whole cache for each one, and
        // with dozens going a frame that pass was a stutter of its own. One pass takes the cut-off, a
        // second drops everything farther than it. Something drawn this frame never goes.
        if (g_entries > kMaxCache)
        {
            auto far2 = [&](const Entry& e) {
                const float dx = e.pos[0] - player[0], dy = e.pos[1] - player[1];
                return e.lastSeen == now ? -1.0f : dx * dx + dy * dy;
            };
            std::vector<float> dist;
            dist.reserve(g_entries);
            for (auto& kv : g_cache)
                for (const Entry& e : kv.second)
                    dist.push_back(far2(e));
            const size_t over = g_entries - kMaxCache;
            std::nth_element(dist.begin(), dist.begin() + (dist.size() - over), dist.end());
            const float cutoff = dist[dist.size() - over];
            for (auto kv = g_cache.begin(); kv != g_cache.end() && g_entries > kMaxCache; )
            {
                std::vector<Entry>& list = kv->second;
                for (size_t i = 0; i < list.size() && g_entries > kMaxCache; )
                {
                    if (far2(list[i]) >= cutoff && list[i].lastSeen != now)
                    {
                        ReleaseRec(list[i].rec);
                        list[i] = std::move(list.back());
                        list.pop_back();
                        --g_entries;
                        ++g_nEvictCap;
                    }
                    else
                        ++i;
                }
                kv = list.empty() ? g_cache.erase(kv) : std::next(kv);
            }
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
    D3DMATRIX             g_mapAbsToSun = {};      // absolute world -> shadow clip, as the map was last drawn
    IDirect3DTexture9*    g_nearTex   = nullptr;   // the near map: [shadow] nearRange either side
    IDirect3DTexture9*    g_midTex    = nullptr;   // the middle map: [shadow] midRange, solid only
    IDirect3DSurface9*    g_midSurf   = nullptr;
    bool                  g_midValid  = false;
    D3DMATRIX             g_midVP     = {};        // camera-relative world -> middle map clip, for the reader
    D3DMATRIX             g_midAbsToSun = {};      // absolute world -> middle map clip, as it was last drawn
    IDirect3DSurface9*    g_nearSurf  = nullptr;
    bool                  g_nearValid = false;
    IDirect3DTexture9*    g_nearLeafTex  = nullptr;   // the near map's leaves (alpha-tested draws)
    IDirect3DSurface9*    g_nearLeafSurf = nullptr;
    IDirect3DTexture9*    g_farLeafTex   = nullptr;   // the far map's leaves
    IDirect3DSurface9*    g_farLeafSurf  = nullptr;
    bool                  g_nearLeafValid = false, g_farLeafValid = false;
    // The units' map (2026-09-30): players and creatures alone, under the near map's camera, so the sun
    // shadows can darken their shade more than the world's. Half the near map's size until 2026-10-01, with a
    // colour target of its own; now its size, sharing the maps' colour target (g_unitColour holds a reference
    // to g_colour).
    IDirect3DTexture9*    g_unitTex    = nullptr;
    IDirect3DSurface9*    g_unitSurf   = nullptr;
    IDirect3DSurface9*    g_unitColour = nullptr;
    bool                  g_unitValid  = false;
    D3DMATRIX             g_nearVP    = {};        // camera-relative world -> near map clip, for the reader
    D3DMATRIX             g_nearAbsToSun = {};     // absolute world -> near map clip, as it was last drawn

    // The alpha test with the UVs where the shader puts them (2026-10-01). The replay turns the pixel shader
    // off, and stage 0 then cuts an alpha-tested model by texture coordinate 0. A druid's bear and travel form
    // are drawn by a vertex shader that writes a constant to oT0 and the model's UVs to oT2, so the whole
    // model was cut by one texel and cast no shadow at some places (a player's report from Darnassus, with
    // the fix). For such a shader the replay binds a pixel shader that samples texture 0 by the coordinate
    // the UVs went to; the alpha test still applies to what it returns.
    std::unordered_map<IDirect3DVertexShader9*, int> g_uvOut;   // each holds a reference: no reused address
    IDirect3DPixelShader9* g_uvPs[8] = {};
    bool                   g_uvPsFailed[8] = {};

    // From the disassembly: the register of the first TEXCOORD input, then the first instruction that
    // writes an oT register from it. 0 when that is oT0, or when the shader has another shape.
    int ParseUvOutput(const std::string& text)
    {
        int in = -1;
        for (size_t at = text.find("dcl_texcoord"); at != std::string::npos; at = text.find("dcl_texcoord", at + 1))
        {
            size_t q = at + 12;
            if (q < text.size() && text[q] == '0')
                ++q;
            if (q >= text.size() || text[q] != ' ')
                continue;   // dcl_texcoord1 and on
            q = text.find_first_not_of(' ', q);
            if (q + 1 < text.size() && text[q] == 'v' && isdigit(static_cast<unsigned char>(text[q + 1])))
            {
                in = atoi(text.c_str() + q + 1);
                break;
            }
        }
        if (in < 0)
            return 0;
        const std::string reg = "v" + std::to_string(in);
        size_t at = 0;
        while (at < text.size())
        {
            size_t end = text.find('\n', at);
            if (end == std::string::npos)
                end = text.size();
            const std::string line = text.substr(at, end - at);
            at = end + 1;
            const size_t op = line.find_first_not_of(' ');
            if (op == std::string::npos || !isalpha(static_cast<unsigned char>(line[op])) ||
                line.compare(op, 3, "dcl") == 0 || line.compare(op, 3, "def") == 0)
                continue;
            const size_t dst = line.find_first_not_of(' ', line.find(' ', op));
            if (dst == std::string::npos || line.compare(dst, 2, "oT") != 0 || dst + 2 >= line.size() ||
                !isdigit(static_cast<unsigned char>(line[dst + 2])))
                continue;
            const int k = line[dst + 2] - '0';
            for (size_t v = line.find(reg, line.find(',', dst)); v != std::string::npos; v = line.find(reg, v + 1))
            {
                const size_t after = v + reg.size();
                const bool whole = (v == 0 || !isalnum(static_cast<unsigned char>(line[v - 1]))) &&
                                   (after >= line.size() || !isdigit(static_cast<unsigned char>(line[after])));
                if (whole)
                    return k;
            }
        }
        return 0;
    }

    int UvOutput(IDirect3DVertexShader9* vs)
    {
        auto it = g_uvOut.find(vs);
        if (it != g_uvOut.end())
            return it->second;
        int out = 0;
        UINT size = 0;
        auto dis = reinterpret_cast<PFN_D3DDisassemble>(CompilerProc("D3DDisassemble"));
        if (dis && SUCCEEDED(vs->lpVtbl->GetFunction(vs, nullptr, &size)) && size)
        {
            std::vector<char> code(size);
            OgBlob* text = nullptr;
            if (SUCCEEDED(vs->lpVtbl->GetFunction(vs, code.data(), &size)) &&
                SUCCEEDED(dis(code.data(), size, 0, nullptr, &text)) && text)
            {
                out = ParseUvOutput(std::string(static_cast<const char*>(text->lpVtbl->GetBufferPointer(text)),
                                                text->lpVtbl->GetBufferSize(text)));
                text->lpVtbl->Release(text);
            }
        }
        vs->lpVtbl->AddRef(vs);
        g_uvOut[vs] = out;
        if (out > 0)
            Log("shadow: vertex shader %p puts the UVs in TEXCOORD%d: its alpha test samples there", vs, out);
        return out;
    }

    IDirect3DPixelShader9* UvShader(IDirect3DDevice9* dev, int k)
    {
        if (k <= 0 || k > 7 || g_uvPsFailed[k])
            return nullptr;
        if (g_uvPs[k])
            return g_uvPs[k];
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        char src[200];
        snprintf(src, sizeof(src), "sampler2D t : register(s0);\n"
                                   "float4 main(float2 uv : TEXCOORD%d) : COLOR { return tex2D(t, uv); }\n", k);
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        if (compile && SUCCEEDED(compile(src, strlen(src), "shadow_uv", nullptr, nullptr, "main", "ps_2_0", 0, 0, &code,
                                         &errs)) && code)
            dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)),
                                           &g_uvPs[k]);
        if (code) code->lpVtbl->Release(code);
        if (errs) errs->lpVtbl->Release(errs);
        if (!g_uvPs[k])
        {
            g_uvPsFailed[k] = true;
            Log("shadow: could not make the TEXCOORD%d alpha mask; those models keep the plain alpha test", k);
        }
        else
            Log("shadow: TEXCOORD%d alpha mask ready", k);
        return g_uvPs[k];
    }

    void ReleaseResources()
    {
        for (auto& kv : g_uvOut)
            kv.first->lpVtbl->Release(kv.first);
        g_uvOut.clear();
        for (int k = 0; k < 8; ++k)
        {
            SafeRelease(g_uvPs[k]);
            g_uvPsFailed[k] = false;
        }
        SafeRelease(g_nearSurf);
        SafeRelease(g_nearTex);
        g_nearValid = false;
        SafeRelease(g_midSurf);
        SafeRelease(g_midTex);
        g_midValid = false;
        SafeRelease(g_nearLeafSurf);
        SafeRelease(g_nearLeafTex);
        SafeRelease(g_farLeafSurf);
        SafeRelease(g_farLeafTex);
        g_nearLeafValid = g_farLeafValid = false;
        SafeRelease(g_unitSurf);
        SafeRelease(g_unitTex);
        SafeRelease(g_unitColour);
        g_unitValid = false;
        SafeRelease(g_depthSurf);
        SafeRelease(g_depthTex);
        SafeRelease(g_colour);
        SafeRelease(g_sb);
        g_size  = 0;
        g_valid = false;
    }

    bool EnsureResources(IDirect3DDevice9* dev, UINT size)
    {
        const bool wantNear = g_cfg.shadow.nearRange > 0.0f;
        const bool wantLeaves = g_cfg.shadow.leaves;
        const bool wantMid = g_cfg.shadow.midRange > 0.0f;
        if (g_size == size && g_depthSurf && g_colour && g_sb && wantNear == (g_nearSurf != nullptr) &&
            wantLeaves == (g_farLeafSurf != nullptr) && (wantLeaves && wantNear) == (g_nearLeafSurf != nullptr) &&
            wantNear == (g_unitSurf != nullptr) && wantMid == (g_midSurf != nullptr))
            return true;
        ReleaseResources();
        auto* d = dev->lpVtbl;
        HRESULT hr = d->CreateTexture(dev, size, size, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT,
                                      &g_depthTex, nullptr);
        if (SUCCEEDED(hr))
            hr = g_depthTex->lpVtbl->GetSurfaceLevel(g_depthTex, 0, &g_depthSurf);
        // The near map is the same size, so the one colour target serves both.
        if (SUCCEEDED(hr) && wantNear)
        {
            hr = d->CreateTexture(dev, size, size, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT, &g_nearTex, nullptr);
            if (SUCCEEDED(hr))
                hr = g_nearTex->lpVtbl->GetSurfaceLevel(g_nearTex, 0, &g_nearSurf);
            if (SUCCEEDED(hr) && wantLeaves)
            {
                hr = d->CreateTexture(dev, size, size, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT, &g_nearLeafTex, nullptr);
                if (SUCCEEDED(hr))
                    hr = g_nearLeafTex->lpVtbl->GetSurfaceLevel(g_nearLeafTex, 0, &g_nearLeafSurf);
            }
        }
        // The middle map: solid only. Leaves at that distance still come from the far map's leaf map.
        if (SUCCEEDED(hr) && wantMid)
        {
            hr = d->CreateTexture(dev, size, size, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT, &g_midTex, nullptr);
            if (SUCCEEDED(hr))
                hr = g_midTex->lpVtbl->GetSurfaceLevel(g_midTex, 0, &g_midSurf);
        }
        // The units' map at the near map's size since 2026-10-01 (half until then): a character's shade on
        // itself now comes from it, and half the size stepped about on the body.
        if (SUCCEEDED(hr) && wantNear)
        {
            hr = d->CreateTexture(dev, size, size, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT, &g_unitTex, nullptr);
            if (SUCCEEDED(hr))
                hr = g_unitTex->lpVtbl->GetSurfaceLevel(g_unitTex, 0, &g_unitSurf);
        }
        if (SUCCEEDED(hr) && wantLeaves)
        {
            hr = d->CreateTexture(dev, size, size, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT, &g_farLeafTex, nullptr);
            if (SUCCEEDED(hr))
                hr = g_farLeafTex->lpVtbl->GetSurfaceLevel(g_farLeafTex, 0, &g_farLeafSurf);
        }
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
        if (g_unitSurf)
        {
            g_unitColour = g_colour;   // the same size: one colour target serves every map
            g_unitColour->lpVtbl->AddRef(g_unitColour);
        }
        g_size = size;
        Log("shadow: %ux%u INTZ depth map%s ready (colour target %s)%s", size, size,
            g_nearLeafSurf ? "s, far and near, each solid and leaves," : g_nearSurf ? "s, far and near," : "",
            colourKind, g_unitSurf ? ", and one of the units" : "");
        if (g_midSurf)
            Log("shadow: and a middle map, solid only, %ux%u", size, size);
        return true;
    }

    // Render states the replay changes; re-set afterwards through the vtable so other hooks' mirrors stay
    // true, then the state block restores the device exactly (the same pattern as volume.cpp).
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

    // Texture stage states the replay changes, re-set by hand the same way. With only the state block
    // restoring them, and the pass run at the copy of the world (anti-aliasing on), the chat background and
    // the XP bar drew as opaque white on the replay frames: 11 of 30 captures. Re-set by hand: 0 of 40.
    struct StageState { DWORD stage; D3DTEXTURESTAGESTATETYPE type; };
    const StageState kStageTouched[] = {
        { 0, D3DTSS_COLOROP }, { 0, D3DTSS_COLORARG1 }, { 0, D3DTSS_ALPHAOP }, { 0, D3DTSS_ALPHAARG1 },
        { 0, D3DTSS_TEXCOORDINDEX }, { 0, D3DTSS_TEXTURETRANSFORMFLAGS },
        { 1, D3DTSS_COLOROP }, { 1, D3DTSS_ALPHAOP },
    };
    constexpr int kStageTouchedCount = sizeof(kStageTouched) / sizeof(kStageTouched[0]);

    // What the client left in those states at this frame's replay, for the volume trace.
    char g_inherited[200] = {};
}

void ShadowSetPhase(bool recording, bool votesOnly)
{
    g_votesOnly = !recording && votesOnly;
    g_recording = ((recording && g_cfg.shadow.enabled) || g_votesOnly) && !g_failed;
    if (recording && !g_haveCamAtBegin)
        g_haveCamAtBegin = ClientCamera(g_camAtBegin);
    if (recording)
    {
        g_frameSeqStart = g_writeSeq;
        g_copiesTaken = 0;
        g_copyNow = Now();
    }
}

void ShadowNoteBufferWrite(const void* buffer, UINT offset, UINT size)
{
    if (g_copying)
        return;                      // filling a copy of our own
    if (!buffer || (g_written.size() >= 8192 && !g_written.count(buffer)))
        return;
    BufferWrites& b = g_written[buffer];
    b.last = ++g_writeSeq;
    if (b.lastFrame != g_frameId)
    {
        b.lastFrame = g_frameId;
        ++b.frames;
    }
    b.ring[b.next] = { offset, size ? offset + size : 0xFFFFFFFFu, b.last };   // size 0 is the whole buffer
    b.next = (b.next + 1) % 16;
}

UINT g_maxConstReg = 0;   // the highest register the client has ever set: the replay need go no further
UINT g_maxSinceDraw = 0;  // the highest register uploaded since the last recorded model draw
UINT g_lastOwn = 34;      // what the last model draw's own uploads reached

void RecordConstants(UINT reg, const float* data, UINT count)
{
    // The mirror follows every client upload, recording or not, so a snapshot is right whenever taken.
    if (!g_mirrorValid || !data || reg >= 256)
        return;
    if (reg + count > 256)
        count = 256 - reg;
    memcpy(&g_mirror[reg * 4], data, count * 4 * sizeof(float));
    if (reg + count > g_maxConstReg)
        g_maxConstReg = reg + count;
    if (reg + count > g_maxSinceDraw)
        g_maxSinceDraw = reg + count;
}

void RecordDraw(IDirect3DDevice9* dev, bool indexed, D3DPRIMITIVETYPE prim, INT baseVertex, UINT minIndex,
                UINT numVertices, UINT startIndex, UINT primCount)
{
    if (!g_recording)
        return;
    struct Timer
    {
        double t0 = g_timing ? Now() : 0.0;
        ~Timer() { if (g_timing) g_tRecord += Now() - t0; }
    } timer;
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
        // The probe (2026-09-30): which draws these are. Looked at when planters in Stormwind lost their
        // shadow after you stopped; that turned out to be the batches (see Spread), not these.
        static unsigned logged = 0;
        if (!g_logNext)
            logged = 0;
        else if (logged < 40)
        {
            ++logged;
            IDirect3DVertexShader9* vs = nullptr;
            d->GetVertexShader(dev, &vs);
            D3DMATRIX w = {};
            d->GetTransform(dev, D3DTS_WORLD, &w);
            float cam[3] = {};
            ClientCamera(cam);
            Log("shadow:   dynamic buffer draw turned away: %s %uv %up, stride %u, buffer %p offset %u, base %d start %u; "
                "world at (%.1f %.1f %.1f); c31..33.w (%.1f %.1f %.1f)", vs ? "M2" : "ff", numVertices, primCount,
                r.vbStride[0], r.vb[0], r.vbOffset[0], baseVertex, startIndex, w.m[3][0] + cam[0], w.m[3][1] + cam[1],
                w.m[3][2] + cam[2], g_mirror[31 * 4 + 3] + cam[0], g_mirror[32 * 4 + 3] + cam[1],
                g_mirror[33 * 4 + 3] + cam[2]);
            SafeRelease(vs);
        }
        SafeRelease(r.vb[0]);
        ++g_rejDynamic;
        return;
    }
    if (!g_fullFrame || g_votesOnly)
    {
        // A frame that does not redraw the map: the same votes as below, from the same draws, and nothing
        // else. Recording every draw and merging it into the cache on every frame cost 1.4 ms of CPU a
        // frame in Stormwind (1391 entries), for a cache that only the replay frames read.
        SafeRelease(r.vb[0]);
        IDirect3DVertexShader9* vs = nullptr;
        d->GetVertexShader(dev, &vs);
        D3DVIEWPORT9 vp = {};
        float minZ = 0.0f, maxZ = 1.0f;
        if (SUCCEEDED(d->GetViewport(dev, &vp)))
        {
            VoteSlice(vp.MinZ, vp.MaxZ);
            minZ = vp.MinZ; maxZ = vp.MaxZ;
        }
        if (!vs)
        {
            D3DMATRIX cv, cp;
            d->GetTransform(dev, D3DTS_VIEW, &cv);
            d->GetTransform(dev, D3DTS_PROJECTION, &cp);
            VoteCamera(cv, cp, minZ, maxZ);
        }
        SafeRelease(vs);
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
    {
        IDirect3DPixelShader9* ps = nullptr;
        d->GetPixelShader(dev, &ps);
        r.terrain = TerrainShadeIsTerrain(ps);
        SafeRelease(ps);
    }
    d->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &r.alphaTest);
    d->GetRenderState(dev, D3DRS_ALPHAREF, &r.alphaRef);
    d->GetRenderState(dev, D3DRS_ALPHAFUNC, &r.alphaFunc);
    d->GetTransform(dev, D3DTS_WORLD, &r.world);
    if (r.vs)
    {
        // Only as far as the highest register the client has set (157 to 220 measured), not all 256:
        // this copy is made for every model draw, every frame. At least c0..c33, which Merge reads.
        r.nregs  = g_maxConstReg < 34 ? 34 : (g_maxConstReg > 256 ? 256 : g_maxConstReg);
        // What the replay uploads (2026-09-29): a model's bones are uploaded just before it is drawn, so
        // the uploads since the last model draw reach as far as this one uses. With none (the same model
        // drawn again), the last one's reach. Uploading up to the highest register ever set (157 to 220)
        // for every model was 12.4 MB a replay; this is 4.8 MB.
        r.nregsOwn = g_maxSinceDraw >= 34 ? g_maxSinceDraw : g_lastOwn;
        if (r.nregsOwn > r.nregs)
            r.nregsOwn = r.nregs;
        g_lastOwn = r.nregsOwn;
        g_maxSinceDraw = 0;
        r.consts = g_constPool.size();
        g_constPool.insert(g_constPool.end(), g_mirror, g_mirror + r.nregs * 4);
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

    // Arena geometry: a buffer the client streams through, so by the end of the world pass, or a few
    // frames on, what it holds will be something else. A copy of our own stands in, taken once per chunk
    // and kept: the same terrain arrives at the same place with the same counts every frame, which is what
    // identifies it, since its buffer and offsets are different each time.
    //
    // A buffer counts as an arena when the client wrote it earlier in this frame, or in two frames or
    // more. The first rule alone missed terrain the client wrote into the arena in an earlier frame and
    // drew from it now: that was cached as a plain pointer, dropped as soon as the client refilled the
    // buffer, and back the next frame. A trace while walking showed 8 to 12 such chunks dropped a frame
    // and up to 168 at once, all from one buffer, and the shade of the ground blinked: the jitter while
    // walking.
    {
        auto w = g_written.find(r.vb[0]);
        const bool arena = w != g_written.end() && (w->second.last > g_frameSeqStart || w->second.frames >= 2);
        if (arena && !r.vs && g_cfg.shadow.copyMax > 0)
        {
            float cam[3] = { 0.0f, 0.0f, 0.0f };
            ClientCamera(cam);
            const ArenaKey key = {
                r.numVertices, r.primCount,
                static_cast<int>((r.world.m[3][0] + cam[0]) * 10.0f),
                static_cast<int>((r.world.m[3][1] + cam[1]) * 10.0f),
                static_cast<int>((r.world.m[3][2] + cam[2]) * 10.0f),
            };
            auto it = g_copies.find(key);
            // A moving object (2026-09-30): the ship at Auberdine is a building drawn through the arena, at a
            // new place each frame. Keyed by place, every piece of it wanted a new copy each frame: past
            // copyPerFrame the rest had no shade that frame (a flicker), and each new copy was a new buffer,
            // so the cache saw a new object each frame and kept the old places until staleTime (a band of
            // shade along its path). Its copy is the one with the same counts that was used in the last
            // quarter of a second and not yet this frame, within 3 yards; it moves to the new place.
            if (it == g_copies.end())
            {
                auto nearest = g_copies.end();
                long long nearestD2 = 30LL * 30LL + 1;
                for (auto c = g_copies.begin(); c != g_copies.end(); ++c)
                {
                    if (c->first.verts != key.verts || c->first.prims != key.prims ||
                        !(c->second.lastUsed < g_copyNow && g_copyNow - c->second.lastUsed < 0.25))
                        continue;
                    const long long dx = c->first.x - key.x, dy = c->first.y - key.y, dz = c->first.z - key.z;
                    const long long d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < nearestD2) { nearestD2 = d2; nearest = c; }
                }
                if (nearest != g_copies.end())
                {
                    const ArenaCopy moved = nearest->second;
                    g_copies.erase(nearest);
                    it = g_copies.emplace(key, moved).first;
                    ++g_copiesMoved;
                }
            }
            if (it == g_copies.end() && g_copiesTaken < static_cast<unsigned>(g_cfg.shadow.copyPerFrame) &&
                g_copies.size() >= static_cast<size_t>(g_cfg.shadow.copyMax))
                SweepCopies();
            if (it == g_copies.end() &&
                g_copiesTaken < static_cast<unsigned>(g_cfg.shadow.copyPerFrame) &&
                g_copies.size() < static_cast<size_t>(g_cfg.shadow.copyMax))
            {
                ArenaCopy copy;
                if (TakeCopy(dev, r, copy))
                {
                    it = g_copies.emplace(key, copy).first;
                    ++g_copiesTaken;
                }
            }
            if (it != g_copies.end())
            {
                it->second.lastUsed = g_copyNow;
                // Point the record at the copy: the vertices start at 0 in it, so baseVertex carries the
                // difference and the indices are used as they were recorded.
                SafeRelease(r.vb[0]);
                SafeRelease(r.ib);
                r.vb[0] = it->second.vb;
                r.vb[0]->lpVtbl->AddRef(r.vb[0]);
                r.vbOffset[0] = 0;
                r.vbStride[0] = it->second.stride;
                r.baseVertex  = -static_cast<INT>(r.minIndex);
                r.startIndex  = 0;
                if (it->second.ib)
                {
                    r.ib = it->second.ib;
                    r.ib->lpVtbl->AddRef(r.ib);
                }
            }
            else
            {
                ReleaseRec(r);     // no copy yet: caching a pointer into the arena is worse than nothing
                return;
            }
        }
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

unsigned ShadowCopies(unsigned& failed)
{
    failed = g_copyFailed;
    return static_cast<unsigned>(g_copies.size());
}

void ShadowVotesEnded()
{
    const bool logThis = g_logNext;
    g_logNext = false;
    g_recording = false;
    g_votesOnly = false;
    SettleVotes(logThis);
}

void ShadowNoReplay()
{
    g_replaySeconds = 0.0;
    g_replayDrawn   = 0;
    g_replaySkipped = 0;
}

void ShadowTiming(bool on)
{
    g_timing = on;
    g_tRecord = g_tCache = g_tReplay = 0.0;
    g_tDrawn = g_tEntries = g_tFrames = g_tReplays = 0;
    g_tMerge = 0.0;
    g_tStill = g_tRefreshed = 0;
}

void ShadowTakeCacheSplit(double& merge, unsigned& still, unsigned& refreshed)
{
    merge = g_tMerge; still = g_tStill; refreshed = g_tRefreshed;
    g_tMerge = 0.0;
    g_tStill = g_tRefreshed = 0;
}

void ShadowTakeTimes(double& record, double& cache, double& replay, unsigned& drawn, unsigned& entries,
                     unsigned& frames, unsigned& replays)
{
    record = g_tRecord; cache = g_tCache; replay = g_tReplay;
    drawn = g_tDrawn; entries = g_tEntries; frames = g_tFrames; replays = g_tReplays;
    g_tRecord = g_tCache = g_tReplay = 0.0;
    g_tDrawn = g_tEntries = g_tFrames = g_tReplays = 0;
}

double ShadowReplaySeconds(unsigned& drawn, unsigned& skipped)
{
    drawn   = g_replayDrawn;
    skipped = g_replaySkipped;
    return g_replaySeconds;
}

void ShadowStatsText(std::string& out)
{
    const SecStats& s = g_sec;
    char line[512];
    unsigned models = 0, fixed = 0, moving = 0;
    for (const auto& kv : g_cache)
        for (const Entry& e : kv.second)
        {
            (e.rec.vs ? models : fixed)++;
            moving += (e.mobile || e.drifts) ? 1u : 0u;
        }
    const unsigned rf = s.recFrames ? s.recFrames : 1;
    std::string why;
    for (const auto& kv : s.nearWhy)
    {
        snprintf(line, sizeof(line), "%s%s %u", why.empty() ? "" : ", ", kv.first.c_str(), kv.second);
        why += line;
    }
    snprintf(line, sizeof(line),
             "held=%zu;models=%u;fixed=%u;moving=%u;drawn=%u;seen=%u;rec=%u;zw=%u;blend=%u;dyn=%u;added=%u;"
             "dropped=%u;dview=%u;dage=%u;dover=%u;dcap=%u;nadd=%u;ndrop=%u;nref=%u;nwhy=%s;",
             g_entries, models, fixed, moving, s.mostDrawn, s.seen / rf, s.recorded / rf, s.rejZW / rf,
             s.rejBlend / rf, s.rejDynamic / rf, s.added, s.evView + s.evAge + s.evWritten + s.evCap, s.evView,
             s.evAge, s.evWritten, s.evCap, s.nearAdded, s.nearGone, s.nearRefused, why.c_str());
    out += line;
    g_sec = SecStats();
}

const char* ShadowNearChanges()
{
    return g_nearInfo;
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
    if (!ShadowSunDirection(sunDir) || !(worldCam || SunCamera(view, proj)) || !ClientCamera(cam))
    {
        if (logThis) Log("shadow: nothing replayed (no sun, camera matrices or camera position)");
        g_replayOutcome = 2;
        ReleaseFrame();
        return;
    }
    const bool havePlayer = ClientPlayer(pl);
    if (!havePlayer) { pl[0] = cam[0]; pl[1] = cam[1]; pl[2] = cam[2]; }

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
    // The last frame's figures, into the second's (/atmos stats).
    ++g_sec.frames;
    g_sec.added += g_nAdded; g_sec.evView += g_nEvictView; g_sec.evAge += g_nEvictAge;
    g_sec.evWritten += g_nEvictWritten; g_sec.evCap += g_nEvictCap;
    g_nRefreshed = g_nAdded = g_nEvictView = g_nEvictAge = g_nEvictCap = g_nEvictWritten = 0;
    if (logThis)
        Log("shadow: this frame's world draws: %u seen, recorded %u; rejected: depth test off %u, depth writes "
            "off %u, blended %u, no vertex buffer %u, dynamic %u", g_seen, recorded, g_rejZ, g_rejZW, g_rejBlend,
            g_rejNoVB, g_rejDynamic);
    g_sec.seen += g_seen; g_sec.recorded += recorded; g_sec.rejZW += g_rejZW; g_sec.rejBlend += g_rejBlend;
    g_sec.rejDynamic += g_rejDynamic; ++g_sec.recFrames;
    g_rejZ = g_rejZW = g_rejBlend = g_rejNoVB = g_rejDynamic = g_seen = 0;
    if (logThis)
    {
        g_samplesLeft = 6;
        memcpy(g_logPlayer, pl, sizeof(g_logPlayer));
    }
    double tCache = t0;   // where the replay's own timing starts
    if (g_fullFrame)
    {
        Merge(camVP, camVPInv, cam, cam, now);
        const double tMerged = Now();
        g_samplesLeft = 0;
        Evict(camVP, cam, pl, now);
        tCache = Now();
        if (logThis)
            Log("shadow: time: matching %.2f ms, eviction %.2f ms", 1000.0 * (tMerged - t0),
                1000.0 * (tCache - tMerged));
        if (g_timing)
        {
            g_tCache   += tCache - t0;
            g_tMerge   += tMerged - t0;
            g_tStill   += g_nStill;
            g_tRefreshed += g_nRefreshed;
            g_tEntries += static_cast<unsigned>(g_entries);
            ++g_tFrames;
        }
    }

    // The ground from the map files (mapterrain.cpp): the tiles the far map can reach. Its box runs
    // ShadowMapDepth() yards toward the sun, so a ridge that far off can still shade you.
    const float mapDepth = ShadowMapDepth();
    MapTerrainUpdate(dev, pl, s.mapTerrain ? (std::max)(s.range, mapDepth) + 60.0f : 0.0f,
                     (std::max)(s.range, s.depth) + 60.0f);   // past this, the ground alone

    // Nothing to draw only if the cache is empty and the files are off: with them on, the cache holds only
    // what moves, and can be empty on a hill with nobody about.
    const bool nothing = g_cache.empty() && !s.mapTerrain;
    if (nothing || !EnsureResources(dev, static_cast<UINT>(s.size)))
    {
        g_replayOutcome = 4;
        if (!nothing)
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
    if (g_cfg.trace)   // the trace's line about this frame; formatted every frame, it was wasted work
    {
        const float mx = cam[0] - g_camAtBegin[0], my = cam[1] - g_camAtBegin[1], mz = cam[2] - g_camAtBegin[2];
        _snprintf_s(g_frameInfo2, sizeof(g_frameInfo2), _TRUNCATE,
                    "map centre abs (%.4f %.4f %.4f), camera (%.4f %.4f %.4f), sun (%.6f %.6f %.6f); camera moved "
                    "%.4f yards since the frame began%s; copies refused: second stream %u, other %u; drift a frame: "
                    "M2 start %.4f end %.4f (%u), ff start %.4f end %.4f (%u); big models: %u in place (largest "
                    "shift %.2f), %u by the move rule (largest jump %.2f), %u new; model records with a projection "
                    "alone %u",
                    pl[0], pl[1], pl[2], cam[0], cam[1], cam[2], sunDir[0], sunDir[1], sunDir[2],
                    g_haveCamAtBegin ? sqrtf(mx * mx + my * my + mz * mz) : -1.0f,
                    g_haveCamAtBegin ? "" : " (not read at the start)", g_copyRefusedStream2, g_copyRefusedOther,
                    g_driftN[0] ? g_driftSum[0][0] / g_driftN[0] : 0.0, g_driftN[0] ? g_driftSum[0][1] / g_driftN[0] : 0.0,
                    g_driftN[0], g_driftN[1] ? g_driftSum[1][0] / g_driftN[1] : 0.0,
                    g_driftN[1] ? g_driftSum[1][1] / g_driftN[1] : 0.0, g_driftN[1], g_bigNear, g_bigNearShift,
                    g_bigMoved, g_bigMoveJump, g_bigNew, g_nProjOnly);
    }
    const float eye[3] = { centre[0] + sunDir[0] * mapDepth, centre[1] + sunDir[1] * mapDepth,
                           centre[2] + sunDir[2] * mapDepth };
    const float up[3]  = { 0.0f, 0.0f, 1.0f };
    const float upX[3] = { 1.0f, 0.0f, 0.0f };
    D3DMATRIX sunView, sunProj, sunVP, fromAbs, fromAbsToSun;
    LookAtLH(eye, centre, fabsf(sunDir[2]) > 0.99f ? upX : up, sunView);
    OrthoLH(s.range * 2.0f, s.range * 2.0f, 1.0f, mapDepth * 2.0f, sunProj);
    Mul(sunView, sunProj, sunVP);
    Translation(-cam[0], -cam[1], -cam[2], fromAbs);
    Mul(fromAbs, sunVP, fromAbsToSun);   // absolute world -> sun clip, for the shader entries
    // Buildings and models from the files are culled to `depth` along the sun, as before horizonDepth: only
    // the ground reaches further. The same box, cut to `depth` either side of the centre.
    const float cutNear = (std::max)(mapDepth - s.depth, 1.0f), cutFar = mapDepth + s.depth;
    D3DMATRIX cutProj, cutAbsToSun;
    OrthoLH(s.range * 2.0f, s.range * 2.0f, cutNear, cutFar, cutProj);
    {
        D3DMATRIX v;
        Mul(sunView, cutProj, v);
        Mul(fromAbs, v, cutAbsToSun);
    }

    // The map need not be redrawn every frame. What it holds is then a frame or two old, which the light's
    // own smoothing covers. The cache is brought up to date on the same frames only (g_fullFrame).
    //
    // On a frame without a replay, the reader must see the map as it was DRAWN: the replay's matrix,
    // kept in absolute coordinates, brought to this frame's camera. This frame's own sunVP is centred on
    // where the player is now, while the map holds the shade around where the player was at the replay:
    // read through it, the shade was shifted by the distance walked since then and snapped back at the
    // next replay, which was jitter while walking, none standing still, and worse at mapEvery 3 than 2.
    // A first try at this (2026-09-24) flashed every few frames. Tried again the same day, after a
    // distant model's transform was fixed (IsProjection), it neither flashed nor jittered.
    if (!g_fullFrame && g_valid)
    {
        D3DMATRIX toAbs;
        Translation(cam[0], cam[1], cam[2], toAbs);
        Mul(toAbs, g_mapAbsToSun, g_shadowVP);
        if (g_nearValid)
            Mul(toAbs, g_nearAbsToSun, g_nearVP);
        if (g_midValid)
            Mul(toAbs, g_midAbsToSun, g_midVP);
        g_replayOutcome = 0;
        g_replaySeconds = Now() - t0;
        return;
    }
    // The far map on every farEvery-th replay only (2026-09-29): it covers 250 yards and its shade barely
    // changes from one frame to the next, while the near map, where the player and everything close
    // stand, is redrawn each time. It cost 5 ms of a 10.8 ms replay. In between it is read, like the map
    // on a frame without a replay, through the matrix it was drawn with, brought to this camera.
    static unsigned farTick = 0;
    const unsigned farEvery = s.farEvery > 1 ? static_cast<unsigned>(s.farEvery) : 1u;
    const bool drawFar = !g_valid || farEvery <= 1 || (farTick++ % farEvery) == 0;
    if (drawFar)
    {
        g_shadowVP    = sunVP;
        g_mapAbsToSun = fromAbsToSun;
    }
    else
    {
        D3DMATRIX toAbs;
        Translation(cam[0], cam[1], cam[2], toAbs);
        Mul(toAbs, g_mapAbsToSun, g_shadowVP);
    }

    // The near and middle maps: the same sun camera, narrower. Each is held on whole texels of its own grid
    // ([shadow] nearSnap, 2026-10-02). They share the far map's centre, which snap holds on the far map's
    // texels only (7.8 of the near map's), so the near map slid under the world with every step: each
    // caster's outline fell on other texels, and shadow edges hopped as you walked and stood still when you
    // only turned. Where the world's origin lands in the map's clip space is kept on a whole texel, worked
    // out in doubles (cam is thousands of yards out).
    auto narrowMap = [&](float range, D3DMATRIX& proj, D3DMATRIX& vp, D3DMATRIX& absToSun, D3DMATRIX& cutToSun)
    {
        OrthoLH(range * 2.0f, range * 2.0f, 1.0f, mapDepth * 2.0f, proj);
        D3DMATRIX p, v;
        OrthoLH(range * 2.0f, range * 2.0f, cutNear, cutFar, p);
        if (s.nearSnap)
        {
            const double texelClip = 2.0 / (s.size > 0 ? s.size : 1);
            for (int a = 0; a < 2; ++a)
            {
                const double viewAt = -(static_cast<double>(cam[0]) * sunView.m[0][a] +
                                        static_cast<double>(cam[1]) * sunView.m[1][a] +
                                        static_cast<double>(cam[2]) * sunView.m[2][a]) + sunView.m[3][a];
                const double clipAt = viewAt * proj.m[a][a] + proj.m[3][a];
                const double shift  = floor(clipAt / texelClip + 0.5) * texelClip - clipAt;
                proj.m[3][a] += static_cast<float>(shift);
                p.m[3][a]    += static_cast<float>(shift);
            }
        }
        Mul(sunView, proj, vp);
        Mul(fromAbs, vp, absToSun);
        Mul(sunView, p, v);
        Mul(fromAbs, v, cutToSun);
    };
    const bool doNear = s.nearRange > 0.0f && g_nearSurf;
    D3DMATRIX nearProj, nearVP, nearAbsToSun, nearCutAbsToSun;
    if (doNear)
        narrowMap(s.nearRange, nearProj, nearVP, nearAbsToSun, nearCutAbsToSun);
    // The middle map ([shadow] midRange, 2026-10-02): past the near map, a shadow cast from under a yard
    // away (a merlon on the wall behind it, an eave) was lost in the far map's slack, up to 1.4 yards.
    const bool doMid = s.midRange > 0.0f && g_midSurf;
    D3DMATRIX midProj, midVP, midAbsToSun, midCutAbsToSun;
    if (doMid)
        narrowMap(s.midRange, midProj, midVP, midAbsToSun, midCutAbsToSun);

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
    DWORD savedStage[kStageTouchedCount];
    for (int i = 0; i < kStageTouchedCount; ++i)
        d->GetTextureStageState(dev, kStageTouched[i].stage, kStageTouched[i].type, &savedStage[i]);
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
    if (g_cfg.trace)
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

    UINT drawn = 0, drawnVS = 0, unseen = 0, skipped = 0, nearDrawn = 0;
    // Four passes: far solid, far leaves, near solid, near leaves. What goes to the leaves: any model
    // with an alpha-tested part, whole (a tree with its trunk, a bush), and any other alpha-tested draw;
    // but never a model standing where a unit or a player does, whose hair or cloak may be alpha tested
    // too. A model's parts share its vertex buffer, and so do all its copies. By draw alone, a trunk was
    // solid and threw a dark bar through the canopy's part shade (2026-09-29).
    const bool doLeaves = s.leaves && g_farLeafSurf && (!doNear || g_nearLeafSurf);

    static std::unordered_map<long long, std::vector<int>> unitCells;   // 2-yard cells: the units in each
    static float units[512][3];
    unitCells.clear();
    if (doLeaves)
    {
        for (const auto& kv : g_cache)
            for (const Entry& e : kv.second)
                if (e.rec.vs && e.rec.alphaTest)
                    g_leafModels.insert(ModelKey(e.rec.vb[0], e.rec.vs));
        const int n = ClientUnits(units, 512);
        for (int i = 0; i < n; ++i)
            unitCells[(static_cast<long long>(floorf(units[i][0] * 0.5f)) << 32) ^
                      (static_cast<long long>(floorf(units[i][1] * 0.5f)) & 0xFFFFFFFFll)].push_back(i);
        g_unitsSeen = n;
    }
    // A model seen where a unit stands stays that unit's (2026-09-30). A Northshire peasant carrying lumber
    // lost his shade for one frame at the same point of his walk each time round: the lumber (or his hair)
    // is alpha tested, so his model is a leafy one, kept out of the leaves only while its reference point
    // (the first bone) is within half a yard of the unit. The carry animation moves that bone further for
    // a frame, and he cast as leaves, at part shade. The entry follows him from frame to frame, so it keeps
    // the answer.
    auto atUnit = [&](const Entry& e) {
        // A unit within half a yard across the ground and 4 up or down, found through this cell and those
        // around it: a character's reference point is its root bone, at the unit's feet.
        if (unitCells.empty())
            return false;
        const long long cx = static_cast<long long>(floorf(e.pos[0] * 0.5f));
        const long long cy = static_cast<long long>(floorf(e.pos[1] * 0.5f));
        for (long long ox = -1; ox <= 1; ++ox)
            for (long long oy = -1; oy <= 1; ++oy)
            {
                auto it = unitCells.find(((cx + ox) << 32) ^ ((cy + oy) & 0xFFFFFFFFll));
                if (it == unitCells.end())
                    continue;
                for (int i : it->second)
                {
                    const float dx = e.pos[0] - units[i][0], dy = e.pos[1] - units[i][1], dz = e.pos[2] - units[i][2];
                    if (dx * dx + dy * dy < 0.25f && dz > -4.0f && dz < 4.0f)
                        return true;
                }
            }
        return false;
    };
    if (doLeaves)
        for (auto& kv : g_cache)
            for (Entry& e : kv.second)
                if (e.rec.vs && !e.unit && atUnit(e))
                    e.unit = true;
    // A unit's parts share its vertex buffer and shader, so one part at a unit makes them all units
    // (2026-10-01). A player's legs, 631 vertices, had their first bone 0.54 yards across the ground from the
    // player, past the half yard atUnit takes; as the idle animation moved it across that line they went
    // to the near map and back, and the shadow's legs came out lighter than its body now and then.
    g_unitModels.clear();
    for (const auto& kv : g_cache)
        for (const Entry& e : kv.second)
            if (e.rec.vs && e.unit)
                g_unitModels.insert(ModelKey(e.rec.vb[0], e.rec.vs));
    for (auto& kv : g_cache)
        for (Entry& e : kv.second)
            if (e.rec.vs && !e.unit && g_unitModels.count(ModelKey(e.rec.vb[0], e.rec.vs)))
                e.unit = true;
    auto isLeaf = [&](const Entry& e) {
        // Terrain casts as leaves do ([shadow] terrainLeaves): hills and mountains let part of the sun
        // through, as the owner wanted, where buildings stop it all (2026-09-29).
        if (e.rec.terrain)
            return s.terrainLeaves;
        if (e.rec.vs && e.unit)
            return false;
        return e.rec.alphaTest != 0 || (e.rec.vs && g_leafModels.count(ModelKey(e.rec.vb[0], e.rec.vs)) != 0);
    };
    UINT leafDrawn = 0;
    unsigned farTiles = 0, nearTiles = 0, farWmos = 0, nearWmos = 0, farDoodads = 0, nearDoodads = 0;
    unsigned midTiles = 0, midWmos = 0, midDoodads = 0;
    UINT midDrawn = 0;
    // The fifth pass draws the units alone into their own map, under the near map's camera.
    const bool doUnits = doNear && g_unitSurf && g_unitColour && g_cfg.sunShadows.units &&
                         g_cfg.sunShadows.unitStrength > 0.0f;
    UINT unitDrawn = 0;
    // Passes: far solid, far leaves, near solid, near leaves, middle (solid), units. The middle map is the
    // sun shadows' alone, as the near one is; it holds the units too, since the units' map is the near one's.
    double passTime[6] = {};
    unsigned long long bytesNow[6] = {};
    for (int pass = 0; pass < 6; ++pass)
    {
    const bool unitPass = pass == 5;
    const bool midPass  = pass == 4;
    const bool nearPass = pass >= 2 && !midPass;   // the near map's camera: near solid, near leaves, units
    const bool leafPass = pass == 1 || pass == 3;
    if ((pass < 2 && !drawFar) || (nearPass && !doNear) || (midPass && !doMid) || (leafPass && !doLeaves) ||
        (unitPass && !doUnits))
        continue;
    const double passStart = Now();
    const float mapRange = nearPass ? s.nearRange : midPass ? s.midRange : s.range;
    const D3DMATRIX& passAbsToSun = nearPass ? nearAbsToSun : midPass ? midAbsToSun : fromAbsToSun;
    const D3DMATRIX& passCut      = nearPass ? nearCutAbsToSun : midPass ? midCutAbsToSun : cutAbsToSun;
    if (unitPass)
        d->SetRenderTarget(dev, 0, g_unitColour);   // the last pass: the restore puts the client's back
    d->SetDepthStencilSurface(dev, unitPass ? g_unitSurf : nearPass ? (leafPass ? g_nearLeafSurf : g_nearSurf)
                                            : midPass ? g_midSurf : (leafPass ? g_farLeafSurf : g_depthSurf));
    d->Clear(dev, 0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    d->SetTransform(dev, D3DTS_PROJECTION, nearPass ? &nearProj : midPass ? &midProj : &sunProj);

    // Everything in the cache used to be replayed every frame, and the GPU clipped whatever fell outside
    // the map: 2000 to 5000 draws a frame, 6 to 9 ms of CPU, which the game feels. An entry whose
    // reference point is well outside the box the map covers cannot mark it, so it is not drawn. The
    // margin is generous because a model's reference point is its first bone, which for some models sits
    // far from the geometry (trees measured at 85 to 95 yards away).
    // The near map's margin is 16 yards, not 40 (2026-09-29): 40 around a 32-yard map was a box more than
    // twice its size, and 2,000 draws; 16 still takes in a big tree's crown beside it.
    const float sideReach = mapRange + (nearPass || midPass ? s.nearMargin : 40.0f);   // `range` either side
    const float alongReach = s.depth + 40.0f;    // and `depth` toward the sun and away from it
    // [sunshadows] world and units (2026-09-30): the near maps are the sun shadows' alone, so the world is
    // left out of them when its shadows are off; units are left out of every map when theirs are.
    const bool worldHere = (!(nearPass || midPass) || g_cfg.sunShadows.world) && !unitPass;
    const bool unitsHere = g_cfg.sunShadows.units;
    // The ground from the files goes where the client's terrain would: the leaf map with terrainLeaves.
    if (worldHere && s.mapTerrain && (doLeaves ? leafPass == s.terrainLeaves : !leafPass))
    {
        const unsigned n = MapTerrainDraw(dev, passAbsToSun, cam);
        if (nearPass) nearTiles = n; else if (midPass) midTiles = n; else farTiles = n;
    }
    // The buildings from the files: solid.
    if (worldHere && s.mapTerrain && !leafPass)
    {
        const unsigned n = MapBuildingsDraw(dev, passCut, cam);
        if (nearPass) nearWmos = n; else if (midPass) midWmos = n; else farWmos = n;
    }
    // The doodads from the files: the solid models here, the trees and bushes with the leaves (or here as
    // well, without leaf maps).
    if (worldHere && s.mapTerrain)
    {
        unsigned n = 0;
        if (!leafPass)
            n += MapDoodadsDraw(dev, passCut, cam, false, static_cast<DWORD>(s.leafAlpha), D3DCMP_GREATEREQUAL);
        if (leafPass || !doLeaves)
            n += MapDoodadsDraw(dev, passCut, cam, true, static_cast<DWORD>(s.leafAlpha), D3DCMP_GREATEREQUAL);
        if (nearPass) nearDoodads += n; else if (midPass) midDoodads += n; else farDoodads += n;
    }
    IDirect3DPixelShader9* passPs = nullptr;   // the UV alpha mask bound, if any (UvOutput)
    for (auto& kv : g_cache)
    for (const Entry& e : kv.second)
    {
        const Rec&   r = e.rec;
        if (e.lastSeen < now && pass < 2)
            ++unseen;
        // With leaf maps, the leaves go there and everything else to the solid map.
        if (doLeaves && isLeaf(e) != leafPass)
            continue;
        // With the units' map drawn, the units stay out of the near solid map (2026-10-01): a character's
        // shadow then comes from one map, with one outline. In both, its shadow was drawn twice with two
        // outlines, a lighter one and a darker one, and the near map's slack kept the darker one off the feet.
        if (e.unit ? (!unitsHere || (nearPass && !leafPass && !unitPass && doUnits)) : !worldHere)
            continue;
        if (unitPass && !(r.vs && e.unit))
            continue;
        // Only the models are culled. Terrain and buildings are fixed-function, there are a couple of
        // hundred of them rather than thousands, and one chunk covers so much ground that the point we
        // hold for it can sit well outside the map while its geometry crosses the middle: culling those
        // took the shade out from under a mountain 150 yards away.
        if (r.vs)
        {
            const float dx = e.pos[0] - pl[0], dy = e.pos[1] - pl[1], dz = e.pos[2] - pl[2];
            const float along = dx * sunDir[0] + dy * sunDir[1] + dz * sunDir[2];
            const float sx = dx - along * sunDir[0], sy = dy - along * sunDir[1], sz = dz - along * sunDir[2];
            const float side2 = sx * sx + sy * sy + sz * sz;
            const float sr = sideReach + e.spread, ar = alongReach + e.spread;   // a batch: any of its models
            if (side2 > sr * sr || along > ar || along < -ar)
            {
                if (pass < 2)
                    ++skipped;
                continue;
            }
            // Small models far off stay out of the far map ([shadow] minTriangles): a flower or a stone
            // 60 yards away is a few texels, and each costs a draw (about 1 microsecond) all the same.
            if (pass < 2 && r.primCount < static_cast<UINT>(s.minTriangles) &&
                dx * dx + dy * dy > 60.0f * 60.0f)
            {
                ++skipped;
                continue;
            }
        }
        if (r.vs)
        {
            D3DMATRIX m;
            Mul(e.absolute, passAbsToSun, m);
            float c[16];
            ToRegisters(m, c);
            const UINT regs = (std::min)(static_cast<UINT>(e.consts.size() / 4), (std::max)(e.rec.nregsOwn, 34u));
            d->SetVertexShaderConstantF(dev, 0, e.consts.data(), regs);
            bytesNow[pass] += static_cast<unsigned long long>(regs) * 16;
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
        IDirect3DPixelShader9* ps = r.vs && r.alphaTest ? UvShader(dev, UvOutput(r.vs)) : nullptr;
        if (ps != passPs)
        {
            d->SetPixelShader(dev, ps);
            passPs = ps;
        }
        d->SetRenderState(dev, D3DRS_ALPHATESTENABLE, r.alphaTest);
        d->SetRenderState(dev, D3DRS_ALPHAREF,        r.alphaRef);
        d->SetRenderState(dev, D3DRS_ALPHAFUNC,       r.alphaFunc);
        if (r.indexed)
            d->DrawIndexedPrimitive(dev, r.prim, r.baseVertex, r.minIndex, r.numVertices, r.startIndex, r.primCount);
        else
            d->DrawPrimitive(dev, r.prim, r.baseVertex, r.primCount);
        if (logThis)
            g_replayed[&e] |= (nearPass || midPass) ? 2 : 1;
        if (r.vb[1])
            d->SetStreamSource(dev, 1, nullptr, 0, 0);
        if (unitPass)
            ++unitDrawn;
        else if (leafPass)
            ++leafDrawn;
        else if (midPass)
            ++midDrawn;
        else if (nearPass)
            ++nearDrawn;
        else
            ++drawn;
    }
    if (passPs)
        d->SetPixelShader(dev, nullptr);   // the next pass draws the files' terrain and buildings first
    passTime[pass] = Now() - passStart;
    }   // the maps
    if (logThis)
    {
        Log("shadow: %s; far map %u tiles, %u buildings and %u doodad draws, near map %u, %u and %u; the "
            "game's draws the files cover: %u refused this frame, %u kept from before evicted; leaves cut at "
            "alpha %d ([shadow] leafAlpha)", MapTerrainInfo(), farTiles, farWmos, farDoodads, nearTiles, nearWmos,
            nearDoodads, g_nFilesRefused, g_nFilesEvicted, s.leafAlpha);
        if (doMid)
            Log("shadow: middle map, %.0f yards either side: %u tiles, %u buildings and %u doodad draws, %u of the "
                "game's draws, in %.2f ms", s.midRange, midTiles, midWmos, midDoodads, midDrawn, 1000.0 * passTime[4]);
        // The models the cache keeps: those at units (characters, creatures) and the others (the server's
        // objects, animated doodads, the furniture inside buildings, anything past the tiles loaded).
        {
            unsigned models = 0, atUnits = 0, alpha = 0;
            for (const auto& kv : g_cache)
                for (const Entry& e : kv.second)
                    if (e.rec.vs)
                    {
                        ++models;
                        if (e.unit) ++atUnits;
                        else if (e.rec.alphaTest) ++alpha;
                    }
            Log("shadow: the cache keeps %u models: %u at units, %u others (%u alpha tested)", models, atUnits,
                models - atUnits, alpha);
        }
        MapLogDoodadsNear(pl, 40.0f);
        // The cache's entries within 60 yards the client has not drawn for over 2 seconds: kept shade
        // with nothing to show for it, the first thing to look at when a shadow has no caster.
        {
            unsigned shown = 0, total = 0;
            for (const auto& kv : g_cache)
                for (const Entry& e : kv.second)
                {
                    const float dx = e.pos[0] - pl[0], dy = e.pos[1] - pl[1];
                    const float d = sqrtf(dx * dx + dy * dy);
                    if (d > 150.0f || now - e.lastSeen < 1.0)
                        continue;
                    ++total;
                    if (shown++ < 15)
                        Log("shadow:   kept undrawn %.1f s: %s, %u triangles%s%s%s%s, drawn on %u redraws, at (%.1f %.1f "
                            "%.1f), %.0f yd from you",
                            now - e.lastSeen, e.rec.vs ? "model" : e.rec.terrain ? "terrain" : "fixed-function",
                            e.rec.primCount, e.rec.alphaTest ? ", alpha tested" : "", e.unit ? ", at a unit" : "",
                            e.mobile ? ", moving" : "", e.drifts ? ", has moved" : "", e.drawnFor, e.pos[0],
                            e.pos[1], e.pos[2], d);
                }
            Log("shadow: %u cache entries within 150 yd not drawn for over 1 s", total);
        }
        // Every entry within 25 yards (2026-09-30), for a shadow that is missing while you stand still: where
        // its place is held, when the game last drew it, and whether this redraw put it in either map.
        {
            unsigned listed = 0;
            for (const auto& kv : g_cache)
                for (const Entry& e : kv.second)
                {
                    const float dx = e.pos[0] - pl[0], dy = e.pos[1] - pl[1], dz = e.pos[2] - pl[2];
                    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (d - e.spread > 25.0f || listed >= 60)
                        continue;
                    ++listed;
                    auto f = g_replayed.find(&e);
                    const unsigned char bits = f == g_replayed.end() ? 0 : f->second;
                    Log("shadow:   near you: %s %uv %up start %u at (%.1f %.1f %.1f), %.1f yd (its bones %.1f yd about); the game drew it %.1f s ago, "
                        "on %u redraws%s%s%s%s; this redraw: far map %s, near map %s", e.rec.vs ? "M2" : "ff",
                        e.rec.numVertices, e.rec.primCount, e.rec.startIndex, e.pos[0], e.pos[1], e.pos[2], d,
                        e.spread, now - e.lastSeen, e.drawnFor, e.unit ? ", at a unit" : "", e.mobile ? ", moving" : "",
                        e.drifts ? ", has moved" : "", e.rec.alphaTest ? ", alpha tested" : "",
                        (bits & 1) ? "yes" : "no", (bits & 2) ? "yes" : "no");
                }
            Log("shadow: %u cache entries within 25 yd", listed);
        }
        g_replayed.clear();
        // The check on the doodads' places: the nearest one from the files against the nearest model draw.
        float dp[3];
        if (MapDoodadNearest(pl, dp))
        {
            const Entry* hit = nullptr;
            float nd = 1e30f;
            for (const auto& kv : g_cache)
                for (const Entry& e : kv.second)
                    if (e.rec.vs)
                    {
                        const float dx = e.pos[0] - dp[0], dy = e.pos[1] - dp[1], dz = e.pos[2] - dp[2];
                        if (dx * dx + dy * dy + dz * dz < nd) { nd = dx * dx + dy * dy + dz * dz; hit = &e; }
                    }
            if (hit)
                Log("shadow: doodad from the files at (%.2f %.2f %.2f); the game's nearest model draw at (%.2f "
                    "%.2f %.2f), %.2f yd off, %u triangles%s", dp[0], dp[1], dp[2], hit->pos[0], hit->pos[1],
                    hit->pos[2], sqrtf(nd), hit->rec.primCount, hit->rec.alphaTest ? ", alpha tested" : "");
        }
        // The check on the placement maths: the nearest building from the files against the client's own
        // draw nearest to it (fixed-function, not terrain), their places and turns side by side.
        float bp[3], br[3][3];
        char bn[160];
        if (MapBuildingNearest(pl, bp, br, bn, sizeof(bn)))
        {
            const Entry* hit = nullptr;
            float nd = 1e30f;
            for (const auto& kv : g_cache)
                for (const Entry& e : kv.second)
                    if (!e.rec.vs && !e.rec.terrain)
                    {
                        const float dx = e.pos[0] - bp[0], dy = e.pos[1] - bp[1], dz = e.pos[2] - bp[2];
                        if (dx * dx + dy * dy + dz * dz < nd) { nd = dx * dx + dy * dy + dz * dz; hit = &e; }
                    }
            Log("shadow: building %s from the files at (%.2f %.2f %.2f), rows (%.3f %.3f %.3f) (%.3f %.3f %.3f) "
                "(%.3f %.3f %.3f)", bn, bp[0], bp[1], bp[2], br[0][0], br[0][1], br[0][2], br[1][0], br[1][1],
                br[1][2], br[2][0], br[2][1], br[2][2]);
            if (hit)
            {
                const D3DMATRIX& a = hit->absolute;
                Log("shadow: the game's nearest building draw at (%.2f %.2f %.2f), %.2f yd off, rows (%.3f %.3f "
                    "%.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f), %u triangles", hit->pos[0], hit->pos[1],
                    hit->pos[2], sqrtf(nd), a.m[0][0], a.m[0][1], a.m[0][2], a.m[1][0], a.m[1][1], a.m[1][2],
                    a.m[2][0], a.m[2][1], a.m[2][2], hit->rec.primCount);
            }
        }
    }
    if (logThis)
        Log("shadow: time: far map %.2f + %.2f ms (solid + leaves)%s, near map %.2f + %.2f ms, units %.2f ms; "
            "%u leaf draws, %u unit draws; model constants uploaded %.1f MB (far) + %.1f MB (near)",
            1000.0 * passTime[0], 1000.0 * passTime[1], drawFar ? "" : " (not redrawn this time)",
            1000.0 * passTime[2], 1000.0 * passTime[3], 1000.0 * passTime[5], leafDrawn, unitDrawn,
            (bytesNow[0] + bytesNow[1]) / 1048576.0,
            (bytesNow[2] + bytesNow[3] + bytesNow[4] + bytesNow[5]) / 1048576.0);
    if (drawFar)
        g_farLeafValid = doLeaves;
    g_nearLeafValid = doLeaves && doNear;
    g_unitValid = doUnits;
    if (doNear)
    {
        g_nearVP       = nearVP;
        g_nearAbsToSun = nearAbsToSun;
        g_nearValid    = true;
    }
    else
    {
        g_nearValid = false;
    }
    if (doMid)
    {
        g_midVP       = midVP;
        g_midAbsToSun = midAbsToSun;
        g_midValid    = true;
    }
    else
    {
        g_midValid = false;
    }

    // --- restore ------------------------------------------------------------------------------------
    for (int i = 0; i < kTouchedCount; ++i)
        d->SetRenderState(dev, kTouched[i], saved[i]);
    for (int i = 0; i < kStageTouchedCount; ++i)
        d->SetTextureStageState(dev, kStageTouched[i].stage, kStageTouched[i].type, savedStage[i]);
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
    g_replayOutcome = 0;
    g_replayDrawn   = drawn;
    g_sec.mostDrawn = (std::max)(g_sec.mostDrawn, static_cast<unsigned>(drawn));
    if (g_timing)
    {
        g_tReplay += Now() - tCache;
        g_tDrawn  += drawn;
        ++g_tReplays;
    }
    if (logThis)
        Log("shadow: cache %u entries (%u not drawn this frame, kept from earlier): %u refreshed, %u new; evicted "
            "%u in view but gone, %u aged out, %u over the cap. Replayed %u (%u through M2 "
            "shaders), and %u into the near map, in %.2f ms CPU; sun (%.2f %.2f %.2f), centred on the %s",
            static_cast<unsigned>(g_entries), unseen, g_nRefreshed, g_nAdded, g_nEvictView, g_nEvictAge,
            g_nEvictCap, drawn, drawnVS, nearDrawn, 1000.0 * (Now() - t0), sunDir[0], sunDir[1], sunDir[2],
            havePlayer ? "player" : "camera");
    if (logThis)
    {
        Log("shadow: copies of streamed geometry: %u held, %u taken this frame, %u moved with a moving object since "
            "the last probe", static_cast<unsigned>(g_copies.size()), g_copiesTaken, g_copiesMoved);
        g_copiesMoved = 0;
    }
    // What the cache holds, for F12: by kind, leafy (alpha tested), moving, and by distance from the
    // player across the ground.
    if (logThis)
    {
        unsigned ff = 0, m2 = 0, leafy = 0, moving = 0, band[4] = {};
        unsigned long long tris = 0;
        for (const auto& kv : g_cache)
            for (const Entry& e : kv.second)
            {
                (e.rec.vs ? m2 : ff)++;
                leafy  += e.rec.alphaTest ? 1u : 0u;
                moving += e.mobile ? 1u : 0u;
                tris   += e.rec.primCount;
                const float dx = e.pos[0] - pl[0], dy = e.pos[1] - pl[1];
                const float dd = sqrtf(dx * dx + dy * dy);
                band[dd < 50.0f ? 0 : dd < 150.0f ? 1 : dd < 250.0f ? 2 : 3]++;
            }
        // Terrain entries that share a place (to a yard): more than one at a place is the same ground held
        // twice, as a level-of-detail version or a copy.
        {
            std::unordered_map<long long, unsigned> at;
            unsigned terrainN = 0;
            for (const auto& kv : g_cache)
                for (const Entry& e : kv.second)
                    if (e.rec.terrain)
                    {
                        ++terrainN;
                        const long long k = (static_cast<long long>(floorf(e.pos[0])) << 42) ^
                                            (static_cast<long long>(floorf(e.pos[1])) << 21) ^
                                            static_cast<long long>(floorf(e.pos[2]));
                        ++at[k];
                    }
            unsigned one = 0, two = 0, more = 0;
            for (const auto& kv : at)
                (kv.second == 1 ? one : kv.second == 2 ? two : more)++;
            Log("shadow: %u terrain entries at %u places: %u held once, %u twice, %u three times or more",
                terrainN, static_cast<unsigned>(at.size()), one, two, more);
        }
        Log("shadow: the cache holds %u fixed-function draws (terrain, buildings) and %u model draws (trees, "
            "doodads, characters); %u alpha tested (leaves, bushes), %u moving; %llu triangles. From you: %u "
            "within 50 yd, %u 50-150, %u 150-250, %u past 250; %d units and players known", ff, m2, leafy, moving,
            tris, band[0], band[1], band[2], band[3], g_unitsSeen);
    }
    g_replaySeconds = Now() - t0;
    g_replaySkipped = skipped;
}

void ShadowFrameEnd()
{
    const int every = g_cfg.shadow.mapEvery;
    g_fullFrame = every <= 1 || !g_valid || (++g_mapTick % static_cast<unsigned>(every)) == 0;
    ++g_frameId;
    g_haveCamAtBegin = false;
    g_recording = false;
    g_votesOnly = false;
    ReleaseFrame();
    g_sliceCount = 0;
    g_camCount   = 0;
}

float ShadowMapDepth()
{
    const ShadowSettings& s = g_cfg.shadow;
    return s.mapTerrain ? (std::max)(s.depth, s.horizonDepth) : s.depth;
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

IDirect3DTexture9* ShadowNearLeaves()
{
    return (g_cfg.shadow.enabled && g_valid && g_nearValid && g_nearLeafValid) ? g_nearLeafTex : nullptr;
}

IDirect3DTexture9* ShadowNearUnits()
{
    return (g_cfg.shadow.enabled && g_valid && g_nearValid && g_unitValid) ? g_unitTex : nullptr;
}

bool ShadowIsUnitDraw(IDirect3DDevice9* dev)
{
    if (g_unitModels.empty())
        return false;
    IDirect3DVertexShader9* vs = nullptr;
    dev->lpVtbl->GetVertexShader(dev, &vs);
    if (!vs)
        return false;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    dev->lpVtbl->GetStreamSource(dev, 0, &vb, &offset, &stride);
    const bool unit = vb && g_unitModels.count(ModelKey(vb, vs)) != 0;
    SafeRelease(vb);
    SafeRelease(vs);
    return unit;
}

IDirect3DTexture9* ShadowFarLeaves()
{
    return (g_cfg.shadow.enabled && g_valid && g_farLeafValid) ? g_farLeafTex : nullptr;
}

bool ShadowNear(IDirect3DTexture9*& tex, D3DMATRIX& camRelToShadowClip, float& range)
{
    if (!g_cfg.shadow.enabled || !g_valid || !g_nearValid || !g_nearTex)
        return false;
    tex = g_nearTex;
    camRelToShadowClip = g_nearVP;
    range = g_cfg.shadow.nearRange;
    return true;
}

bool ShadowMid(IDirect3DTexture9*& tex, D3DMATRIX& camRelToShadowClip, float& range)
{
    if (!g_cfg.shadow.enabled || !g_valid || !g_midValid || !g_midTex)
        return false;
    tex = g_midTex;
    camRelToShadowClip = g_midVP;
    range = g_cfg.shadow.midRange;
    return true;
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
    for (auto& kv : g_copies)   // D3DPOOL_MANAGED survives a reset, but the geometry may not be wanted again
    {
        SafeRelease(kv.second.vb);
        SafeRelease(kv.second.ib);
    }
    g_copies.clear();
    ClearCache();          // the client's buffers are rebuilt across a Reset: nothing cached stays valid
    ReleaseResources();
    g_mirrorValid = false;
    g_failed = false;
}

void ShadowProbe()
{
    g_logNext = true;
    g_refusedLogs = 6;
    g_fullFrame = true;   // called after ShadowFrameEnd: the probed frame records in full and redraws the map
}
