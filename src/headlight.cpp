// headlight: the gnome epic mount's lamp throws a cone of light at night (2026-11-01): the lamp on the
// model is there but lights nothing.
//
// Every mounted unit in the object list whose mount's model file (CreatureDisplayInfo.dbc, then CreatureModelData.dbc)
// holds a word of [headlight] models gets a lamp, [headlight] forward yards ahead of the unit's place along its
// facing and [headlight] height up, pointing along the facing, tilted a little down. The model itself carries no
// light (the file has no M2 lights; its emitters are the exhaust at the back), so its place is a setting to tune
// ([headlight] forward, height, tilt).
//
// One full-screen pass, as the lighthouses' (beacon.cpp): each pixel's line of sight runs to the surface the world's
// depth shows.
//   - On the surface: the cone's weight at that point, times how squarely the surface faces the lamp (its normal from
//     the depth's neighbours), so the ground and walls in front of the mount are lit.
//   - In the air: the cone's weight summed in steps along the line of sight, as far as the surface, so dust and mist
//     show the cone; brighter looking into the lamp.
// By night only, by the game clock (NightWeight), [headlight] day of it by day.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "headlight.h"
#include "client.h"
#include "common.h"
#include "config.h"
#include "depth.h"
#include "mapterrain.h"
#include "shadow.h"
#include "sun.h"
#include "shadercache.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr int kMax = 4;   // lamps drawn at once, the nearest

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
float4 gZ    : register(c4);        // the world viewport's MinZ, 1 / (MaxZ - MinZ), the night's weight, the target's width
float4 gP    : register(c5);        // the cone seen in the air, the light on surfaces, its length, tan(half angle)
float4 gCol  : register(c6);        // the light's colour, the target's height
float4 gL[4] : register(c8);        // each lamp, camera-relative; w its strength by its distance (0 none)
float4 gD[4] : register(c12);       // each one's way (xyz, unit)

// The cone's weight at a point: across, a soft edge that widens with the distance; along, a fall-off and an end.
float ConeW(float3 p, float3 l, float3 d)
{
    float3 v = p - l;
    float  t = dot(v, d);
    float  r = max(t, 0.0) * gP.w + 0.15;
    float3 q = v - t * d;
    float  x = dot(q, q) / (r * r);
    return exp(-x * 2.0) * smoothstep(0.0, 1.2, t) * (1.0 - smoothstep(0.6 * gP.z, gP.z, t)) / (1.0 + t * t * 0.02);
}

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float  raw = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    float  d   = min(saturate((raw - gZ.x) * gZ.y), 0.99999);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = ndc.x * gInv0 + ndc.y * gInv1 + d * gInv2 + gInv3;
    float3 P   = wp.xyz / max(wp.w, 1e-6);
    bool   sky = d >= 0.99998;
    float  len = sky ? 1e6 : length(P);
    float3 dir = P / max(length(P), 1e-4);
    // The surface's normal from the world position's change across the pixel.
    float3 N   = normalize(cross(ddx(P), ddy(P)) + float3(0.0, 0.0, 1e-6));
    // Interleaved gradient noise to start the steps at a different place on each pixel.
    float  jit = frac(52.9829189 * frac(dot(uv * float2(gZ.w, gCol.w), float2(0.06711056, 0.00583715))));
    float3 sum = 0.0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float4 L = gL[i];
        [branch] if (L.w > 0.0)
        {
            float3 D = gD[i].xyz;
            float  light = 0.0;
            // On the surface.
            [branch] if (!sky)
            {
                float3 toL = normalize(L.xyz - P);
                float  face = lerp(0.35, 1.0, abs(dot(N, toL)));
                light += gP.y * 3.0 * ConeW(P, L.xyz, D) * face;
            }
            // In the air: along the line of sight where it is near the lamp, as far as the surface.
            float sc = dot(L.xyz, dir);
            float s0 = max(sc - gP.z, 0.0);
            float s1 = min(sc + gP.z, len);
            [branch] if (s1 > s0)
            {
                float stp = (s1 - s0) / 20.0;
                float air = 0.0;
                [loop] for (int k = 0; k < 20; ++k)
                    air += ConeW(dir * (s0 + (k + jit) * stp), L.xyz, D);
                float look = saturate(dot(dir, -D));
                light += gP.x * 0.4 * air * stp * (0.35 + 2.5 * look * look * look);
            }
            sum += gCol.rgb * light * L.w;
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
            Log("headlight: %s failed to compile hr=0x%08X: %s", name, hr,
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
            if (OgBlob* code = Compile(kVsHlsl, "headlight_vs", "vs_3_0"))
            {
                if (FAILED(dev->lpVtbl->CreateVertexShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_vs)))
                    g_vs = nullptr;
                code->lpVtbl->Release(code);
            }
            if (OgBlob* code = Compile(kPsHlsl, "headlight_ps", "ps_3_0"))
            {
                if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_ps)))
                    g_ps = nullptr;
                code->lpVtbl->Release(code);
            }
            if (g_vs && g_ps)
                Log("headlight: shaders compiled");
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

    // Whether a mount display's model file holds one of the words of [headlight] models, by display id, once its
    // file name is known (the loader reads it a little after the first ask).
    bool HasLamp(unsigned display, const HeadlightSettings& s)
    {
        static std::unordered_map<unsigned, bool> known;
        static std::string                        forModels;
        if (forModels != s.models)
        {
            forModels = s.models;
            known.clear();
        }
        const auto it = known.find(display);
        if (it != known.end())
            return it->second;
        const std::string* file = MapCreatureModelName(display);
        if (!file)
            return false;
        std::string name = *file;
        for (char& c : name)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        bool yes = false;
        const std::string list = s.models;
        for (size_t at = 0; at < list.size();)
        {
            size_t end = list.find(',', at);
            if (end == std::string::npos)
                end = list.size();
            std::string word = list.substr(at, end - at);
            while (!word.empty() && isspace(static_cast<unsigned char>(word.front())))
                word.erase(word.begin());
            while (!word.empty() && isspace(static_cast<unsigned char>(word.back())))
                word.pop_back();
            for (char& c : word)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (!word.empty() && name.find(word) != std::string::npos)
                yes = true;
            at = end + 1;
        }
        known[display] = yes;
        Log("headlight: mount display %u wears %s: %s", display, file->c_str(), yes ? "a lamp" : "no lamp");
        return yes;
    }
}

void HeadlightProbe()
{
    g_logNext = true;
}

bool HeadlightDraw(IDirect3DDevice9* dev)
{
    const bool logThis = g_logNext;
    g_logNext = false;
    const HeadlightSettings& s = g_cfg.headlight;
    if (!g_cfg.master || !s.enabled || g_failed || s.strength <= 0.0f)
        return false;
    const float night = s.day + (1.0f - s.day) * Night();
    if (night <= 0.01f)
        return false;
    float cam[3];
    if (!ClientCamera(cam))
        return false;

    // The lamps: every mounted unit with a lamp on its mount, within reach, the nearest kMax.
    static ClientUnit units[512];
    const int count = ClientUnitList(units, 512);
    struct Lamp { float pos[3], dir[3], fade, d2; };
    Lamp lamps[kMax];
    int n = 0;
    for (int i = 0; i < count; ++i)
    {
        const ClientUnit& u = units[i];
        if (!u.mount || u.dead || !u.facingOk || !HasLamp(u.mount, s))
            continue;
        const float dx = u.pos[0] - cam[0], dy = u.pos[1] - cam[1];
        const float dist = sqrtf(dx * dx + dy * dy);
        const float x = (dist - 0.75f * s.reach) / (0.25f * s.reach);
        const float fade = x <= 0.0f ? 1.0f : x >= 1.0f ? 0.0f : 1.0f - x * x * (3.0f - 2.0f * x);
        if (fade <= 0.0f)
            continue;
        Lamp l;
        const float cf = cosf(u.facing), sf = sinf(u.facing);
        l.pos[0] = u.pos[0] + cf * s.forward;
        l.pos[1] = u.pos[1] + sf * s.forward;
        l.pos[2] = u.pos[2] + s.height;
        const float k = 1.0f / sqrtf(1.0f + s.tilt * s.tilt);
        l.dir[0] = cf * k; l.dir[1] = sf * k; l.dir[2] = s.tilt * k;
        l.fade = fade;
        l.d2 = dist * dist;
        if (n < kMax)
            lamps[n++] = l;
        else
        {
            int worst = 0;
            for (int j = 1; j < kMax; ++j)
                if (lamps[j].d2 > lamps[worst].d2) worst = j;
            if (l.d2 < lamps[worst].d2)
                lamps[worst] = l;
        }
        if (logThis)
            Log("headlight: lamp at (%.1f %.1f %.1f), facing %.2f rad (unit at (%.1f %.1f %.1f), mount display %u), "
                "%.0f yards off, strength %.2f", l.pos[0], l.pos[1], l.pos[2], u.facing, u.pos[0], u.pos[1], u.pos[2],
                u.mount, dist, fade);
    }
    if (logThis)
        Log("headlight: %d lamps of %d units, night %.2f", n, count, night);
    if (n == 0)
        return false;

    IDirect3DTexture9* depth = DepthWorldTexture();
    D3DMATRIX view, proj;
    if (!depth || !(ShadowWorldCamera(view, proj) || SunCamera(view, proj)))
        return false;
    if (!Ensure(dev))
    {
        g_failed = true;
        Log("headlight: could not make its shaders or state block: no headlights until the next reset");
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
    pc[18] = night * s.strength;
    pc[19] = static_cast<float>(td.Width);
    pc[20] = s.air;
    pc[21] = s.surface;
    pc[22] = s.length;
    pc[23] = tanf(s.angle * 3.14159265f / 180.0f);
    pc[24] = ((s.color >> 16) & 0xFF) / 255.0f;
    pc[25] = ((s.color >> 8) & 0xFF) / 255.0f;
    pc[26] = (s.color & 0xFF) / 255.0f;
    pc[27] = static_cast<float>(td.Height);
    for (int i = 0; i < n; ++i)
    {
        float* L = pc + 32 + i * 4;
        float* D = pc + 48 + i * 4;
        L[0] = lamps[i].pos[0] - cam[0]; L[1] = lamps[i].pos[1] - cam[1]; L[2] = lamps[i].pos[2] - cam[2];
        L[3] = lamps[i].fade;
        D[0] = lamps[i].dir[0]; D[1] = lamps[i].dir[1]; D[2] = lamps[i].dir[2];
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

void HeadlightReset()
{
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    SafeRelease(g_sb);
    g_tried = false;
    g_failed = false;
}

// The shaders this pass compiles, as it compiles them, for the cache's worker (shadercache.cpp).
void HeadlightShaderList()
{
    ShaderPrecompile("headlight_vs", kVsHlsl, "vs_3_0");
    ShaderPrecompile("headlight_ps", kPsHlsl, "ps_3_0");
}
