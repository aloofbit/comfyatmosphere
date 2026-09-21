// shadow -- a depth map of the world as the sun sees it.
//
// Volumetric light asks, for any point in the air: does the sun reach it? That needs the scene's depth
// from the sun's side, and the client never renders one. So the frame's opaque world draws are recorded
// as they happen and replayed, once the world has finished, into a depth texture under a camera that
// looks down the sun direction over a square around the player.
//
//   Recording   Between the end of the sky and the end of the world: every draw that writes depth with
//               blending off -- terrain, buildings, trees, characters. Sky, clouds, water, particles and
//               the UI all fail that test. Each record keeps its buffers, shader, declaration, texture,
//               alpha test and world matrix, holding a reference to each until replayed. Shader-constant
//               uploads are recorded too, in order, so the replay sees each draw's constants exactly as
//               it did. Dynamic vertex buffers are skipped: the client re-fills them mid-frame (the grass
//               arena is one, see comfygrass), so by replay time they hold other geometry.
//   Replay      Into a 2048x2048 INTZ depth texture (readable, like depth.cpp's), colour writes off, no
//               culling (leaves are two-sided), alpha test kept so foliage casts leaf-shaped shadows.
//               Fixed-function draws get the sun's view and projection. Shader draws -- all 26 of the
//               client's M2 shaders place vertices with dp4 oPos, c2..c5 -- get c2..c5 rewritten as
//                   M' = M * inverse(cameraViewProj) * sunViewProj
//               which is right whether the client folded the world matrix into c2..c5 or not.
//   Space       Everything is camera-relative, as the client draws it. The sun camera is centred on the
//               player (player - camera), looks along -sun, and covers `range` yards either way.

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
#include <vector>

namespace
{
    const D3DFORMAT kINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    const D3DFORMAT kNULL = static_cast<D3DFORMAT>(MAKEFOURCC('N', 'U', 'L', 'L'));

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }
    template <typename T> T* AddRef(T* p)
    {
        if (p) p->lpVtbl->AddRef(p);
        return p;
    }

    // ---------------------------------------------------------------------------------------------
    // the recording

    struct DrawRec
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
        D3DMATRIX                    world;
    };

    // One stream of events, in the order the client issued them.
    struct Event
    {
        bool   isDraw;
        UINT   reg, count;     // constants: first register, how many
        size_t at;             // constants: offset into g_constPool; draws: index into g_draws
    };

    std::vector<Event>   g_events;
    std::vector<DrawRec> g_draws;
    std::vector<float>   g_constPool;
    float                g_constStart[256 * 4];   // the constants as they stood when recording began
    bool                 g_recording = false;
    bool                 g_haveStart = false;
    UINT                 g_skipped   = 0;         // dynamic buffers, UP draws, over the cap
    // Why world draws were not recorded, this frame: depth test off, depth writes off, blended, no buffer.
    UINT                 g_rejZ = 0, g_rejZW = 0, g_rejBlend = 0, g_rejNoVB = 0, g_seen = 0;
    DWORD                g_clipPlanes = 0;        // the client's user clip planes, at the first recorded draw

    // The camera the world's depth is drawn with, taken from the device at a recorded fixed-function
    // draw (terrain, buildings) -- which uses exactly these. The "last perspective projection set" that
    // rays.cpp keeps can be the sky's instead (near 0.1 / far 500 against the world's 0.222 / 611), and
    // rebuilding distances from depth with the wrong planes squashed the whole scene to within 2 yards.
    D3DMATRIX            g_worldView = {}, g_worldProj = {};
    bool                 g_haveWorldCam = false;   // persists across frames: the last one settled

    // Which camera, by vote as well: the client draws the far horizon with a camera of its own (measured:
    // near 467 / far 2112 against the world's 0.1 / 500), so "the last terrain draw's camera" flipped to
    // the horizon's at some camera angles and everything read as 467+ yards away. The camera used by the
    // most terrain and building draws is the world's.
    struct CamVote { D3DMATRIX view, proj; UINT votes; };
    CamVote              g_cams[8];
    int                  g_camCount = 0;

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
    // The viewport's depth range the world is drawn in. The client squeezes depth into slices of the
    // buffer (MinZ..MaxZ): measured, the world in 0.0..0.94, with other things (the sky, perhaps the far
    // horizon) in another slice. The buffer holds MinZ + z * (MaxZ - MinZ), so reading it as 0..1 put
    // every surface within two yards of the camera -- and taking the range from whichever draw came last
    // flipped the whole screen to "far" whenever that draw was from the other slice, which depended on
    // the camera angle. So every depth-writing draw votes, and the most common slice wins.
    struct Slice { float minZ, maxZ; UINT votes; };
    Slice                g_slices[8];
    int                  g_sliceCount = 0;
    float                g_worldMinZ = 0.0f, g_worldMaxZ = 1.0f;

    void VoteSlice(float minZ, float maxZ)
    {
        for (int i = 0; i < g_sliceCount; ++i)
            if (g_slices[i].minZ == minZ && g_slices[i].maxZ == maxZ) { ++g_slices[i].votes; return; }
        if (g_sliceCount < 8)
            g_slices[g_sliceCount++] = { minZ, maxZ, 1 };
    }
    constexpr size_t     kMaxDraws   = 6000;

    void ReleaseRecording()
    {
        for (DrawRec& d : g_draws)
        {
            SafeRelease(d.vs);
            SafeRelease(d.decl);
            SafeRelease(d.vb[0]);
            SafeRelease(d.vb[1]);
            SafeRelease(d.ib);
            SafeRelease(d.tex0);
        }
        g_draws.clear();
        g_events.clear();
        g_constPool.clear();
        g_haveStart = false;
        g_skipped   = 0;
        g_rejZ = g_rejZW = g_rejBlend = g_rejNoVB = g_seen = 0;
        g_sliceCount = 0;
        g_camCount = 0;
    }

    // ---------------------------------------------------------------------------------------------
    // resources

    IDirect3DTexture9*    g_depthTex  = nullptr;
    IDirect3DSurface9*    g_depthSurf = nullptr;
    IDirect3DSurface9*    g_colour    = nullptr;   // a render target has to be bound; nothing is written to it
    UINT                  g_size      = 0;
    IDirect3DStateBlock9* g_sb        = nullptr;
    bool                  g_failed    = false;
    bool                  g_valid     = false;     // the map holds this frame's (or a recent) replay
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

    // General 4x4 inverse, in double: the camera's view-projection has a near plane of ~0.2 yards.
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

    // Render states the replay changes; re-set afterwards through the vtable so other hooks' mirrors stay
    // true, then the state block restores the device exactly (the same pattern as rays.cpp and beams.cpp).
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
    if (!g_recording || !data || reg >= 256)
        return;
    if (reg + count > 256)
        count = 256 - reg;
    Event e = { false, reg, count, g_constPool.size() };
    g_constPool.insert(g_constPool.end(), data, data + count * 4);
    g_events.push_back(e);
}

void RecordDraw(IDirect3DDevice9* dev, bool indexed, D3DPRIMITIVETYPE prim, INT baseVertex, UINT minIndex,
                UINT numVertices, UINT startIndex, UINT primCount)
{
    if (!g_recording)
        return;
    auto* d = dev->lpVtbl;

    // Opaque, depth-writing world geometry only.
    DWORD zen = 0, zw = 0, blend = 1;
    d->GetRenderState(dev, D3DRS_ZENABLE, &zen);
    d->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
    d->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
    ++g_seen;
    if (!zen)   { ++g_rejZ;     return; }
    if (!zw)    { ++g_rejZW;    return; }
    if (blend)  { ++g_rejBlend; return; }
    if (g_draws.size() >= kMaxDraws)
    {
        ++g_skipped;
        return;
    }

    DrawRec r = {};
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
        ++g_skipped;
        return;
    }

    if (!g_haveStart)
    {
        d->GetVertexShaderConstantF(dev, 0, g_constStart, 256);
        d->GetRenderState(dev, D3DRS_CLIPPLANEENABLE, &g_clipPlanes);
        g_haveStart = true;
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
    if (!r.vs)
    {
        D3DMATRIX cv, cp;
        d->GetTransform(dev, D3DTS_VIEW, &cv);
        d->GetTransform(dev, D3DTS_PROJECTION, &cp);
        VoteCamera(cv, cp);
    }

    D3DVIEWPORT9 vp = {};
    if (SUCCEEDED(d->GetViewport(dev, &vp)))
        VoteSlice(vp.MinZ, vp.MaxZ);

    Event e = { true, 0, 0, g_draws.size() };
    g_draws.push_back(r);
    g_events.push_back(e);
}

void ShadowWorldEnded(IDirect3DDevice9* dev)
{
    const bool logThis = g_logNext;
    g_logNext = false;
    g_recording = false;

    if (g_sliceCount)
    {
        const Slice* best = &g_slices[0];
        for (int i = 1; i < g_sliceCount; ++i)
            if (g_slices[i].votes > best->votes)
                best = &g_slices[i];
        g_worldMinZ = best->minZ;
        g_worldMaxZ = best->maxZ;
        if (logThis)
            for (int i = 0; i < g_sliceCount; ++i)
                Log("shadow: depth slice %.4f..%.4f used by %u draws%s", g_slices[i].minZ, g_slices[i].maxZ,
                    g_slices[i].votes, &g_slices[i] == best ? "  <-- the world" : "");
    }
    g_sliceCount = 0;
    if (g_camCount)
    {
        const CamVote* best = &g_cams[0];
        for (int i = 1; i < g_camCount; ++i)
            if (g_cams[i].votes > best->votes)
                best = &g_cams[i];
        g_worldView = best->view;
        g_worldProj = best->proj;
        g_haveWorldCam = true;
        if (logThis)
            for (int i = 0; i < g_camCount; ++i)
            {
                const float n = -g_cams[i].proj.m[3][2] / g_cams[i].proj.m[2][2];
                Log("shadow: camera near %.3f / far %.1f used by %u draws%s", n,
                    g_cams[i].proj.m[2][2] * n / (g_cams[i].proj.m[2][2] - 1.0f), g_cams[i].votes,
                    &g_cams[i] == best ? "  <-- the world" : "");
            }
    }
    g_camCount = 0;

    const ShadowSettings& s = g_cfg.shadow;
    if (!s.enabled || g_failed || g_draws.empty())
    {
        if (logThis)
            Log("shadow: nothing replayed (%s)", !s.enabled ? "off" : g_failed ? "failed earlier" : "no draws recorded");
        ReleaseRecording();
        return;
    }

    float sunDir[3], cam[3], pl[3];
    D3DMATRIX view, proj;
    const bool worldCam = g_haveWorldCam;
    if (worldCam) { view = g_worldView; proj = g_worldProj; }
    if (!RaysSunDirection(sunDir) || !(worldCam || RaysCamera(view, proj)) || !ClientCamera(cam))
    {
        if (logThis) Log("shadow: nothing replayed (no sun, camera matrices or camera position)");
        ReleaseRecording();
        return;
    }
    const bool havePlayer = ClientPlayer(pl);
    const float centre[3] = { havePlayer ? pl[0] - cam[0] : 0.0f, havePlayer ? pl[1] - cam[1] : 0.0f,
                              havePlayer ? pl[2] - cam[2] : 0.0f };

    if (!EnsureResources(dev, static_cast<UINT>(s.size)))
    {
        g_failed = true;
        ReleaseRecording();
        return;
    }

    // The sun camera: centred on the player, looking down the sun, `range` yards either side and
    // `depth` yards toward the sun and away from it.
    const float eye[3] = { centre[0] + sunDir[0] * s.depth, centre[1] + sunDir[1] * s.depth,
                           centre[2] + sunDir[2] * s.depth };
    const float up[3]  = { 0.0f, 0.0f, fabsf(sunDir[2]) > 0.99f ? 0.0f : 1.0f };
    const float upX[3] = { 1.0f, 0.0f, 0.0f };
    D3DMATRIX sunView, sunProj, sunVP, camVP, camVPInv, toSun;
    LookAtLH(eye, centre, fabsf(sunDir[2]) > 0.99f ? upX : up, sunView);
    OrthoLH(s.range * 2.0f, s.range * 2.0f, 1.0f, s.depth * 2.0f, sunProj);
    Mul(sunView, sunProj, sunVP);
    Mul(view, proj, camVP);
    if (!Invert(camVP, camVPInv))
    {
        ReleaseRecording();
        return;
    }
    Mul(camVPInv, sunVP, toSun);   // camera clip -> sun clip, for the shader draws
    g_shadowVP = sunVP;

    const double t0 = Now();
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

    // --- replay -------------------------------------------------------------------------------------
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

    // The constants as they stood when recording began; the stream of uploads then rebuilds each
    // draw's constants exactly, with c2..c5 rewritten per shader draw.
    static float consts[256 * 4];
    memcpy(consts, g_constStart, sizeof(consts));
    d->SetVertexShaderConstantF(dev, 0, consts, 256);

    if (logThis)
    {
        Log("shadow: world camera from %s: viewport depth range %.4f..%.4f, proj z terms m22 %.6f m32 %.6f "
            "(near %.3f, far %.1f)",
            worldCam ? "a recorded terrain/building draw" : "rays.cpp's last perspective (fallback)",
            g_worldMinZ, g_worldMaxZ,
            proj.m[2][2], proj.m[3][2], -proj.m[3][2] / proj.m[2][2],
            proj.m[2][2] * (-proj.m[3][2] / proj.m[2][2]) / (proj.m[2][2] - 1.0f));
        // The first M2's c2..c5 times inverse(world camera) must be affine -- a move and a rotation,
        // last column (0 0 0 1) -- if that camera is the one the shaders project with.
        float cc[256 * 4];
        memcpy(cc, g_constStart, sizeof(cc));
        for (const Event& e : g_events)
        {
            if (!e.isDraw)
            {
                memcpy(&cc[e.reg * 4], &g_constPool[e.at], e.count * 4 * sizeof(float));
                continue;
            }
            if (!g_draws[e.at].vs)
                continue;
            D3DMATRIX m, x;
            FromRegisters(&cc[2 * 4], m);
            Mul(m, camVPInv, x);
            Log("shadow: first M2's c2..c5 x inverse(world camera): last column (%.4f %.4f %.4f %.4f) -- %s",
                x.m[0][3], x.m[1][3], x.m[2][3], x.m[3][3],
                (fabsf(x.m[0][3]) + fabsf(x.m[1][3]) + fabsf(x.m[2][3]) + fabsf(x.m[3][3] - 1.0f)) < 0.01f
                    ? "affine: the shaders use this camera" : "NOT affine: the camera does not match the shaders");
            break;
        }
        Log("shadow: this frame's world draws: %u seen, recorded %u; rejected -- depth test off %u, depth writes "
            "off %u, blended %u, no vertex buffer %u, dynamic/over cap %u; client clip planes 0x%X",
            g_seen, static_cast<unsigned>(g_draws.size()), g_rejZ, g_rejZW, g_rejBlend, g_rejNoVB, g_skipped,
            g_clipPlanes);
        // Where each draw's origin lands in the map: fixed-function via its world matrix, shader draws via
        // their own c2..c5 taken back through the camera. uv outside 0..1 or depth outside 0..1 = off the map.
        float c[256 * 4];
        memcpy(c, g_constStart, sizeof(c));
        UINT n = 0;
        for (const Event& e : g_events)
        {
            if (!e.isDraw)
            {
                memcpy(&c[e.reg * 4], &g_constPool[e.at], e.count * 4 * sizeof(float));
                continue;
            }
            if (++n > 40)
                break;
            const DrawRec& r = g_draws[e.at];
            D3DMATRIX toClip;
            if (r.vs)
            {
                D3DMATRIX m;
                FromRegisters(&c[2 * 4], m);
                Mul(m, toSun, toClip);
            }
            else
                Mul(r.world, sunVP, toClip);
            const float* o = toClip.m[3];            // the origin, (0,0,0,1) * M
            const float w = fabsf(o[3]) > 1e-6f ? o[3] : 1e-6f;
            Log("  shadow draw %2u: %s prim=%d prims=%u verts=%u world=(%.1f %.1f %.1f) -> map uv (%.2f, %.2f) depth %.3f",
                n, r.vs ? "M2 " : "FF ", static_cast<int>(r.prim), r.primCount, r.numVertices,
                r.world.m[3][0], r.world.m[3][1], r.world.m[3][2],
                o[0] / w * 0.5f + 0.5f, -o[1] / w * 0.5f + 0.5f, o[2] / w);
        }
    }

    UINT drawn = 0, drawnVS = 0;
    for (const Event& e : g_events)
    {
        if (!e.isDraw)
        {
            memcpy(&consts[e.reg * 4], &g_constPool[e.at], e.count * 4 * sizeof(float));
            d->SetVertexShaderConstantF(dev, e.reg, &g_constPool[e.at], e.count);
            continue;
        }
        const DrawRec& r = g_draws[e.at];
        if (r.vs)
        {
            D3DMATRIX m, m2;
            FromRegisters(&consts[2 * 4], m);
            Mul(m, toSun, m2);
            float c[16];
            ToRegisters(m2, c);
            d->SetVertexShaderConstantF(dev, 2, c, 4);
            d->SetVertexShader(dev, r.vs);
            ++drawnVS;
        }
        else
        {
            d->SetVertexShader(dev, nullptr);
            d->SetTransform(dev, D3DTS_WORLD, &r.world);
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
        Log("shadow: replayed %u draws (%u through M2 shaders, %u skipped) in %.2f ms CPU, %u constant uploads; "
            "sun (%.2f %.2f %.2f), centred on the %s", drawn, drawnVS, g_skipped, 1000.0 * (Now() - t0),
            static_cast<unsigned>(g_events.size() - g_draws.size()), sunDir[0], sunDir[1], sunDir[2],
            havePlayer ? "player" : "camera");
    ReleaseRecording();
}

void ShadowFrameEnd()
{
    g_recording = false;
    ReleaseRecording();
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
    ReleaseRecording();
    ReleaseResources();
    g_failed = false;
}

void ShadowProbe()
{
    g_logNext = true;
}
