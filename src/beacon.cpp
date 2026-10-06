// beacon: lighthouses at night (2026-10-05, the owner): a beacon in the lamp room and two beams turning round.
//
// The game's own lighthouse light does not work with the HD models players use, so the lighthouses stood dark.
// Each building the map files place with LIGHTHOUSE in its file name, within kReach yards, gets a lamp: in the
// middle of the building's highest group's box, a little under its top (MapLighthouses, mapterrain.cpp).
//
// One full-screen pass, as the lamps' glow (lampglow.cpp): each pixel's line of sight runs to the surface the
// world's depth shows, and the light along it is added.
//   - The beacon: a glow round the line of sight to the lamp, at least kMinAngle wide however far it is, so it
//     shows from 500 yards. Hidden where something stands nearer than the lamp on that line.
//   - The beams: one, or two opposite ([lighthouse] beamCount), sweeping round once each beamSpeed seconds, each a cone beamLength yards long that
//     widens and fades along it. For each, the nearest point between the line of sight and the beam's axis; the
//     light falls off with the distance between the two, and counts only when that point lies before the surface
//     the pixel shows, so hills and walls hide the beam. Looking along a beam, the line of sight crosses more of
//     it, and the beam brightens; pointed at you, the beacon flares.
// At night only, by the game clock (NightWeight). The game's fog dims them far less than it dims other lights:
// a lighthouse is there to be seen through it.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "beacon.h"
#include "client.h"
#include "common.h"
#include "config.h"
#include "depth.h"
#include "mapterrain.h"
#include "shadow.h"
#include "sun.h"
#include "sunshadows.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace
{
    constexpr int   kMax = 4;            // lighthouses drawn at once, the nearest
    constexpr float kReach = 1500.0f;    // yards: lighthouses further off are left out

    const char* kVsHlsl = R"HLSL(
float4 gHalf : register(c0);   // D3D9 half-pixel offset, in clip units
struct O { float4 pos : POSITION; float2 uv : TEXCOORD0; };
O main(float3 pos : POSITION, float2 uv : TEXCOORD0)
{
    O o;
    o.pos = float4(pos.xy + gHalf.xy, 0.0, 1.0);
    o.uv  = uv;
    return o;
}
)HLSL";

    const char* kPsHlsl = R"HLSL(
sampler2D sDepth : register(s0);    // the scene's depth (INTZ)
float4 gInv0 : register(c0);        // rows of inverse(camera view-projection): clip -> camera-relative world
float4 gInv1 : register(c1);
float4 gInv2 : register(c2);
float4 gInv3 : register(c3);
float4 gZ    : register(c4);        // the world viewport's MinZ, 1 / (MaxZ - MinZ), the night's weight, the beam's
                                    // half width at the lamp
float4 gB    : register(c5);        // the beacon's radius (yards), the beam's length, its widening a yard, the fog's reach
float4 gCol  : register(c6);        // the light's colour, the beams' strength
float4 gL[4] : register(c8);        // each lighthouse's lamp, camera-relative; w 1 in use
float4 gD[4] : register(c12);       // each one's first beam: its way (xyz, unit); w 1 with a second beam opposite

// The light of one beam along this line of sight.
float Beam(float3 p, float3 bd, float3 dir, float len)
{
    float b   = dot(dir, bd);
    float du  = -dot(dir, p);
    float dv  = -dot(bd, p);
    float den = max(1.0 - b * b, 1e-4);
    float s   = clamp((dv - b * du) / den, 0.0, gB.y);   // along the beam
    float3 q  = p + s * bd;
    float t   = max(dot(q, dir), 0.0);                    // along the line of sight
    float h   = length(t * dir - q);
    float w   = gZ.w + s * gB.z;
    float chord = 1.0 / max(sqrt(den), 0.12);             // looking along it, the line of sight crosses more
    return exp(-h * h / (w * w)) * exp(-s / (gB.y * 0.45)) * chord * smoothstep(-3.0, 0.0, len - t) *
           exp(-t / gB.w);
}

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float  raw = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    float  d   = min(saturate((raw - gZ.x) * gZ.y), 0.99999);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    float3 P   = wp.xyz / max(wp.w, 1e-6);
    float  len = d >= 0.99998 ? 1e6 : length(P);          // the sky: nothing in the way
    float3 dir = P / max(length(P), 1e-4);
    float3 sum = 0.0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float4 L = gL[i];
        [branch] if (L.w > 0.5)
        {
            float3 p    = L.xyz;
            float  dist = length(p);
            float3 toL  = p / max(dist, 1e-3);
            float3 b1   = gD[i].xyz;
            float3 b2   = float3(-b1.xy, b1.z);
            sum += gCol.rgb * gCol.w * (Beam(p, b1, dir, len) + gD[i].w * Beam(p, b2, dir, len));
            // The beacon: at least about a fifth of a degree wide, hidden behind what stands nearer.
            float cosA  = dot(dir, toL);
            float ang   = sqrt(max(2.0 * (1.0 - cosA), 0.0));
            float rad   = max(gB.x / max(dist, 1.0), 0.0035);
            float seen  = smoothstep(dist - 6.0, dist - 2.0, len);
            float flare = max(pow(saturate(dot(b1, -toL)), 60.0), gD[i].w * pow(saturate(dot(b2, -toL)), 60.0));
            float glow  = exp(-(ang * ang) / (rad * rad)) + 0.3 * exp(-ang / (rad * 5.0));
            sum += gCol.rgb * glow * seen * exp(-dist / gB.w) * (1.2 + 5.0 * flare);
        }
    }
    sum *= gZ.z;
    sum = (sum >= 0.0 && sum < 16.0) ? sum : 0.0;
    return float4(sum, 0.0);
}
)HLSL";

    IDirect3DVertexShader9* g_vs = nullptr;
    IDirect3DPixelShader9*  g_ps = nullptr;
    IDirect3DStateBlock9*   g_sb = nullptr;
    bool                    g_tried = false;
    bool                    g_failed = false;
    bool                    g_logNext = false;
    float                   g_lamps[kMax][3] = {};   // the last lamps found, for the probe's draws
    int                     g_lampCount = 0;
    float                   g_effects[kMax][3] = {}; // the game's own lighthouse lights (LIGHTHOUSEEFFECT), their origins
    int                     g_effectCount = 0;
    // The vertex buffers the game's own light was drawn from (BeaconSkipsDraw): a draw from one is left out even in
    // a frame its place cannot be read, which left it flashing on the screen now and then (the owner).
    IDirect3DVertexBuffer9* g_lightVbs[8] = {};   // compared only, never used
    int                     g_lightVbCount = 0;

    // The game's own lighthouse lights (2026-10-05): an animated doodad named LIGHTHOUSEEFFECT, at the tower's axis.
    // Each gives a lamp, lampRise over its origin: Stormwind's harbour lighthouse is one group 69 x 56 yards across,
    // and its box said nothing of where the tower stands. The nearest first.
    int Effects(const float cam[3], float (*out)[3], int max)
    {
        static float pts[64][3];
        static std::string names[64];
        const int n = MapAnimatedDoodads(cam, kReach, pts, 64, names);
        int k = 0;
        float d2s[kMax];
        for (int i = 0; i < n; ++i)
        {
            std::string up = names[i];
            for (char& c : up)
                c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if (up.find("LIGHTHOUSEEFFECT") == std::string::npos)
                continue;
            const float dx = pts[i][0] - cam[0], dy = pts[i][1] - cam[1], d2 = dx * dx + dy * dy;
            int at = k < max ? k : max;
            while (at > 0 && d2s[at - 1] > d2)
                --at;
            if (at >= max)
                continue;
            for (int j = (k < max ? k : max - 1); j > at; --j)
            {
                memcpy(out[j], out[j - 1], sizeof(out[j]));
                d2s[j] = d2s[j - 1];
            }
            memcpy(out[at], pts[i], sizeof(out[at]));
            d2s[at] = d2;
            if (k < max)
                ++k;
        }
        return k;
    }

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    OgBlob* Compile(const char* src, const char* name, const char* profile)
    {
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        if (!compile)
            return nullptr;
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        const HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", profile, 0, 0, &code, &errs);
        if (FAILED(hr) || !code)
        {
            Log("beacon: %s failed to compile hr=0x%08X: %s", name, hr,
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no message)");
            if (code) code->lpVtbl->Release(code);
            code = nullptr;
        }
        if (errs) errs->lpVtbl->Release(errs);
        return code;
    }

    bool Ensure(IDirect3DDevice9* dev)
    {
        if (!g_tried)
        {
            g_tried = true;
            if (OgBlob* code = Compile(kVsHlsl, "beacon_vs", "vs_3_0"))
            {
                if (FAILED(dev->lpVtbl->CreateVertexShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_vs)))
                    g_vs = nullptr;
                code->lpVtbl->Release(code);
            }
            if (OgBlob* code = Compile(kPsHlsl, "beacon_ps", "ps_3_0"))
            {
                if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_ps)))
                    g_ps = nullptr;
                code->lpVtbl->Release(code);
            }
            if (g_vs && g_ps)
                Log("beacon: shaders compiled");
        }
        if (!g_vs || !g_ps)
            return false;
        if (!g_sb && (FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)) || !g_sb))
            return false;
        return true;
    }

    void Mul(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX& out)
    {
        D3DMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        out = r;
    }

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

    struct ClipVertex { float x, y, z, u, v; };

    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
        D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE,
        D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);

    // The night's weight now, eased over a second so a clock jump does not switch the lights on in one frame.
    float Night()
    {
        static float  now = 0.0f;
        static double last = 0.0;
        float hour = 0.0f;
        const float target = ClientHour(hour) ? NightWeight(hour) : 0.0f;
        const double t = Now();
        if (last <= 0.0 || t - last > 2.0)
            now = target;
        else
            now += (target - now) * static_cast<float>(1.0 - exp(-(t - last) / 0.5));
        last = t;
        return now;
    }
}

void BeaconProbe()
{
    g_logNext = true;
}

void BeaconProbeDraw(IDirect3DDevice9* dev, const char* kind, unsigned prims, unsigned verts, const float* ffPlace)
{
    if (!g_lampCount && !g_effectCount)
        return;
    // A shader draw by its bones; a fixed-function one by its world matrix's place, camera-relative, plus the camera.
    float p[3], cam[3];
    if (ffPlace)
    {
        if (!ClientCamera(cam))
            return;
        for (int j = 0; j < 3; ++j)
            p[j] = ffPlace[j] + cam[j];
    }
    else if (!ShadowDrawPosition(dev, p))
        return;
    // With the game's own lights known, only the draws within 8 yards of one of their origins: those are its own.
    const bool byEffect = g_effectCount > 0;
    const int count = byEffect ? g_effectCount : g_lampCount;
    for (int i = 0; i < count; ++i)
    {
        const float* at = byEffect ? g_effects[i] : g_lamps[i];
        const float reach = byEffect ? 8.0f : 25.0f;
        const float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
        if (dx * dx + dy * dy + dz * dz > reach * reach)
            continue;
        auto* d = dev->lpVtbl;
        IDirect3DBaseTexture9* tex = nullptr;
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0;
        DWORD blend = 0, src = 0, dst = 0, zw = 0;
        d->GetTexture(dev, 0, &tex);
        d->GetVertexShader(dev, &vs);
        d->GetStreamSource(dev, 0, &vb, &off, &stride);
        d->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
        d->GetRenderState(dev, D3DRS_SRCBLEND, &src);
        d->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
        d->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
        UINT w = 0, h = 0;
        if (tex && tex->lpVtbl->GetType(tex) == D3DRTYPE_TEXTURE)
        {
            D3DSURFACE_DESC td = {};
            if (SUCCEEDED(reinterpret_cast<IDirect3DTexture9*>(tex)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(tex), 0, &td)))
                w = td.Width, h = td.Height;
        }
        D3DVERTEXBUFFER_DESC vd = {};
        if (vb) vb->lpVtbl->GetDesc(vb, &vd);
        Log("beacon: draw near %s: %s, %u prims, %u verts, at (%.1f %.1f %.1f), %.1f yards from it; vs %p, vb %p "
            "(%u bytes, stride %u), tex %p %ux%u, blend %lu src %lu dst %lu zw %lu", byEffect ? "the game's light" : "the lamp",
            kind, prims, verts, p[0], p[1], p[2],
            sqrtf(dx * dx + dy * dy + dz * dz), vs, vb, vd.Size, stride, tex, w, h, blend, src, dst, zw);
        if (tex) tex->lpVtbl->Release(tex);
        if (vs) vs->lpVtbl->Release(vs);
        if (vb) vb->lpVtbl->Release(vb);
        return;
    }
}

bool BeaconDraw(IDirect3DDevice9* dev)
{
    const bool logThis = g_logNext;
    g_logNext = false;
    const LighthouseSettings& s = g_cfg.lighthouse;
    if (!g_cfg.master || !s.enabled || g_failed)
        return false;
    // The lamps first, by day too: the probe's draws are measured against them (BeaconProbeDraw).
    float cam[3];
    if (!ClientCamera(cam))
        return false;
    float found[kMax][3], roof[kMax][2];
    // The game's own lights first, each a lamp lampRise over its origin.
    g_effectCount = Effects(cam, g_effects, kMax);
    if (!g_effectCount)
        g_lightVbCount = 0;   // away from them: a freed buffer's address may come back as another model's
    int n = 0;
    for (int i = 0; i < g_effectCount; ++i)
    {
        found[n][0] = g_effects[i][0] + s.lampShift[0]; found[n][1] = g_effects[i][1] + s.lampShift[1];
        found[n][2] = g_effects[i][2] + s.lampRise;
        roof[n][0] = roof[n][1] = 0.0f;
        ++n;
    }
    // Then a lighthouse building with none of those within 40 yards: the box's guess.
    float byBox[kMax][3], boxRoof[kMax][2];
    const int nb = MapLighthouses(cam, kReach, byBox, kMax, boxRoof);
    for (int b = 0; b < nb && n < kMax; ++b)
    {
        bool beside = false;
        for (int i = 0; i < g_effectCount; ++i)
        {
            const float dx = byBox[b][0] - g_effects[i][0], dy = byBox[b][1] - g_effects[i][1];
            beside = beside || dx * dx + dy * dy < 40.0f * 40.0f;
        }
        if (beside)
            continue;
        memcpy(found[n], byBox[b], sizeof(found[n]));
        memcpy(roof[n], boxRoof[b], sizeof(roof[n]));
        // A tower in one group from its foot to its tip: the lamp is lampDrop under the tip.
        if (roof[n][1] - roof[n][0] >= 30.0f)
            found[n][2] = roof[n][1] - s.lampDrop;
        ++n;
    }
    memcpy(g_lamps, found, sizeof(found[0]) * n);
    g_lampCount = n;
    // By day too, at [lighthouse] day of the night's strength (2026-10-05, the owner: 1, as bright by day).
    const float night = s.day + (1.0f - s.day) * Night();
    if (night <= 0.01f && !logThis)
        return false;
    IDirect3DTexture9* depth = DepthWorldTexture();
    D3DMATRIX view, proj;
    if (!depth || !(ShadowWorldCamera(view, proj) || SunCamera(view, proj)))
        return false;
    if (logThis)
    {
        Log("beacon: %d lighthouses within %.0f yards, night %.2f", n, kReach, night);
        for (int i = 0; i < n; ++i)
        {
            Log("beacon:   lamp at (%.1f %.1f %.1f), %.0f yards off; the highest group from %.1f to %.1f up", found[i][0],
                found[i][1], found[i][2],
                sqrtf((found[i][0] - cam[0]) * (found[i][0] - cam[0]) + (found[i][1] - cam[1]) * (found[i][1] - cam[1])),
                roof[i][0], roof[i][1]);
            MapLogDoodadsNear(found[i], 20.0f);   // the game's own light model, to leave out
        }
        // You, in the nearest lighthouse's own space: stand by a lamp and probe to place it by hand.
        float pl[3], own[3];
        char name[128];
        if (ClientPlayer(pl) && MapLighthouseOwn(pl, own, name, sizeof(name)))
        {
            Log("beacon: you stand at (%.2f %.2f %.2f) in %s's own space", own[0], own[1], own[2], name);
            MapLighthouseGroupsLog(pl);
        }

    }
    if (n == 0 || night <= 0.01f)
        return false;
    if (!Ensure(dev))
    {
        g_failed = true;
        Log("beacon: could not make its shaders or state block: no lighthouses until the next reset");
        return false;
    }
    D3DMATRIX vp, inv;
    Mul(view, proj, vp);
    if (!Invert(vp, inv))
        return false;
    auto* d = dev->lpVtbl;
    IDirect3DSurface9* target = nullptr;
    d->GetRenderTarget(dev, 0, &target);
    if (!target)
        return false;
    D3DSURFACE_DESC td = {};
    target->lpVtbl->GetDesc(target, &td);
    target->lpVtbl->Release(target);

    // --- save ---------------------------------------------------------------------------------------
    IDirect3DSurface9* oldDS = nullptr;
    d->GetDepthStencilSurface(dev, &oldDS);
    g_sb->lpVtbl->Capture(g_sb);
    DWORD saved[kTouchedCount];
    for (int i = 0; i < kTouchedCount; ++i)
        d->GetRenderState(dev, kTouched[i], &saved[i]);
    IDirect3DBaseTexture9*       oldTex0 = nullptr;
    IDirect3DVertexShader9*      oldVS   = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD                        oldFVF  = 0;
    d->GetTexture(dev, 0, &oldTex0);
    d->GetVertexShader(dev, &oldVS);
    d->GetVertexDeclaration(dev, &oldDecl);
    d->GetFVF(dev, &oldFVF);

    // --- draw ---------------------------------------------------------------------------------------
    d->SetDepthStencilSurface(dev, nullptr);   // the scene's depth is read, so it cannot be bound
    d->SetRenderState(dev, D3DRS_ZENABLE,           D3DZB_FALSE);
    d->SetRenderState(dev, D3DRS_ZWRITEENABLE,      FALSE);
    d->SetRenderState(dev, D3DRS_ALPHATESTENABLE,   FALSE);
    d->SetRenderState(dev, D3DRS_CULLMODE,          D3DCULL_NONE);
    d->SetRenderState(dev, D3DRS_FOGENABLE,         FALSE);
    d->SetRenderState(dev, D3DRS_STENCILENABLE,     FALSE);
    d->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(dev, D3DRS_SRGBWRITEENABLE,   FALSE);
    d->SetRenderState(dev, D3DRS_COLORWRITEENABLE,  D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                    D3DCOLORWRITEENABLE_BLUE);
    d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,  TRUE);
    d->SetRenderState(dev, D3DRS_BLENDOP,           D3DBLENDOP_ADD);
    d->SetRenderState(dev, D3DRS_SRCBLEND,          D3DBLEND_ONE);
    d->SetRenderState(dev, D3DRS_DESTBLEND,         D3DBLEND_ONE);
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(depth));
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
    d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, 0);
    d->SetVertexShader(dev, g_vs);
    d->SetPixelShader(dev, g_ps);

    float pc[16 * 4] = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc[r * 4 + c] = inv.m[r][c];
    float minZ = 0.0f, maxZ = 1.0f;
    ShadowWorldDepthRange(minZ, maxZ);
    pc[16] = minZ;
    pc[17] = (maxZ - minZ) > 1e-6f ? 1.0f / (maxZ - minZ) : 1.0f;
    pc[18] = night;
    pc[19] = s.beamWidth;
    float fs = 0.0f, fe = 0.0f;
    const float fogReach = WorldFog(fs, fe) && fe > 1.0f ? (std::max)(fe * 1.5f, 300.0f) : 1500.0f;
    pc[20] = s.beaconSize;               // the beacon's radius, yards
    pc[21] = s.beamLength;
    pc[22] = s.beamSpread;               // yards it widens a yard out
    pc[23] = fogReach;
    pc[24] = ((s.color >> 16) & 0xFF) / 255.0f * s.beacon;
    pc[25] = ((s.color >> 8) & 0xFF) / 255.0f * s.beacon;
    pc[26] = (s.color & 0xFF) / 255.0f * s.beacon;
    pc[27] = s.beam;
    // Each lighthouse: its lamp, camera-relative, and its first beam's way: round once a beamSpeed, each lighthouse
    // a turn of its own by its place, tilted a little down.
    const double t = fmod(Now(), 3600.0);
    for (int i = 0; i < kMax; ++i)
    {
        float* L = pc + 32 + i * 4;
        float* D = pc + 48 + i * 4;
        if (i < n)
        {
            L[0] = found[i][0] - cam[0]; L[1] = found[i][1] - cam[1]; L[2] = found[i][2] - cam[2]; L[3] = 1.0f;
            const float phase = fmodf(found[i][0] * 0.0131f + found[i][1] * 0.0173f, 6.2831853f);
            const float a = static_cast<float>(t * 6.2831853 / s.beamSpeed) + phase;
            const float tilt = s.beamTilt;
            const float l = sqrtf(1.0f + tilt * tilt);
            D[0] = cosf(a) / l; D[1] = sinf(a) / l; D[2] = tilt / l; D[3] = s.beamCount >= 2 ? 1.0f : 0.0f;
        }
    }
    // c0 to c6, then c8 to c15: the compiler keeps a constant of its own (def) in c7, which must not be set.
    d->SetPixelShaderConstantF(dev, 0, pc, 7);
    d->SetPixelShaderConstantF(dev, 8, pc + 32, 8);

    const float half[4] = { -1.0f / td.Width, 1.0f / td.Height, 0.0f, 0.0f };
    d->SetVertexShaderConstantF(dev, 0, half, 1);
    d->SetFVF(dev, D3DFVF_XYZ | D3DFVF_TEX1);
    const ClipVertex q[4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
    };
    d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(ClipVertex));

    // --- restore ------------------------------------------------------------------------------------
    for (int i = 0; i < kTouchedCount; ++i)
        d->SetRenderState(dev, kTouched[i], saved[i]);
    d->SetTexture(dev, 0, oldTex0);
    d->SetVertexShader(dev, oldVS);
    d->SetFVF(dev, oldFVF);
    if (oldDecl)
        d->SetVertexDeclaration(dev, oldDecl);
    d->SetDepthStencilSurface(dev, oldDS);
    g_sb->lpVtbl->Apply(g_sb);
    SafeRelease(oldDS);
    SafeRelease(oldTex0);
    SafeRelease(oldVS);
    SafeRelease(oldDecl);
    return true;
}

bool BeaconSkipsDraw(IDirect3DDevice9* dev)
{
    // The game's own light (2026-10-05, the owner): one model draw of 8 triangles, 16 vertices, a 256 x 256 texture,
    // blended, no depth written, 2.8 yards from the LIGHTHOUSEEFFECT doodad's origin in Stormwind's harbour.
    const LighthouseSettings& s = g_cfg.lighthouse;
    if (!g_effectCount || !g_cfg.master || !s.enabled || !s.hideGameLight)
        return false;
    auto* d = dev->lpVtbl;
    DWORD blend = 0;
    d->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
    if (!blend)
        return false;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    d->GetStreamSource(dev, 0, &vb, &off, &stride);
    if (vb)
        vb->lpVtbl->Release(vb);   // only compared
    for (int i = 0; i < g_lightVbCount; ++i)
        if (vb && g_lightVbs[i] == vb)
            return true;
    float p[3];
    if (!ShadowDrawPosition(dev, p))
        return false;
    for (int i = 0; i < g_effectCount; ++i)
    {
        const float dx = p[0] - g_effects[i][0], dy = p[1] - g_effects[i][1], dz = p[2] - g_effects[i][2];
        if (dx * dx + dy * dy + dz * dz < 6.0f * 6.0f)
        {
            // Remembered, if it is a buffer of its own: a small one, as the light's is (768 bytes).
            D3DVERTEXBUFFER_DESC vd = {};
            if (vb && SUCCEEDED(vb->lpVtbl->GetDesc(vb, &vd)) && vd.Size <= 64 * 1024 && g_lightVbCount < 8)
                g_lightVbs[g_lightVbCount++] = vb;
            return true;
        }
    }
    return false;
}

void BeaconReset()
{
    g_lightVbCount = 0;
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    SafeRelease(g_sb);
    g_tried = false;
    g_failed = false;
}
