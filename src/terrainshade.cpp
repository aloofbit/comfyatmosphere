// terrainshade: the terrain's baked shadow, weakened while the sun shadows draw.
//
// Each terrain chunk carries a 64 x 64 map, baked offline for one sun direction, with the shade of the
// hills, trees and buildings around it. The client's terrain pixel shader reads it as the alpha of the
// chunk's blend map (the same texture whose x, y and z mix the ground layers), and draws a shaded texel
// at 70% brightness and with its specular gone:
//
//     colour = lerp(layer0, layer1, map.x) ... lerp(.., layerN, map.y / map.z)
//     out    = colour x (map.w x 0.3 + 0.7) x diffuse + colour.a x map.w x specular
//
// (disassembled from the shaders the client binds, 2026-09-29: one for each number of ground layers).
// The shade points one way at every hour, so with the sun shadows on it disagreed with them. The CVar
// mapShadows, and the console command setShadow, flip the client's "terrain shadows" flag and nothing in
// this client reads it.
//
// So each new pixel shader the client binds is disassembled once, and one that matches that pattern is
// swapped for a copy with map.w replaced by lerp(1, map.w, keep): keep = 1 is the client's own shader,
// 0 no baked shade. keep is in c31, which no client pixel shader uses (the highest is c9). A shader that
// uses the 0.3 / 0.7 of the shade but does not match the pattern is logged and left alone.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "common.h"
#include "terrainshade.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    struct Seen { IDirect3DPixelShader9* ps; int layers; };   // layers 0 = not a terrain shader
    Seen g_seen[256];
    int  g_seenCount = 0;
    IDirect3DPixelShader9* g_copy[5] = {};                   // by the number of ground layers, 1..4
    bool g_copyFailed[5] = {};

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    std::string Disassemble(IDirect3DPixelShader9* ps)
    {
        auto dis = reinterpret_cast<PFN_D3DDisassemble>(CompilerProc("D3DDisassemble"));
        UINT size = 0;
        if (!dis || FAILED(ps->lpVtbl->GetFunction(ps, nullptr, &size)) || size == 0 || size > 65536)
            return {};
        std::string code(size, '\0');
        if (FAILED(ps->lpVtbl->GetFunction(ps, &code[0], &size)))
            return {};
        OgBlob* text = nullptr;
        if (FAILED(dis(code.data(), size, 0, nullptr, &text)) || !text)
            return {};
        std::string out(static_cast<const char*>(text->lpVtbl->GetBufferPointer(text)),
                        text->lpVtbl->GetBufferSize(text));
        text->lpVtbl->Release(text);
        return out;
    }

    int Count(const std::string& s, const char* what)
    {
        int n = 0;
        for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1))
            ++n;
        return n;
    }

    // The number of ground layers if this is the client's terrain shader, else 0.
    int TerrainLayers(const std::string& t)
    {
        if (t.find("ps_2_0") == std::string::npos || t.find("def c0, 0.300000012, 0.699999988, 1, 0") == std::string::npos)
            return 0;
        const int samplers = Count(t, "dcl_2d s");
        const int lerps    = Count(t, "lrp ");
        const int layers   = samplers - 1;
        const bool tail = t.find("mad r2.w, r1.w, c0.x, c0.y") != std::string::npos &&
                          t.find("mul r0.w, r1.w, r0.w") != std::string::npos &&
                          t.find("mul r0.xyz, r0, r2.w") != std::string::npos &&
                          t.find("mul r1.xyz, r0.w, v1") != std::string::npos &&
                          t.find("mad r0.xyz, r0, v0, r1") != std::string::npos &&
                          Count(t, "texld ") == samplers;
        if (layers >= 1 && layers <= 4 && lerps == layers - 1 && tail)
            return layers;
        return -1;   // uses the shade's numbers, but not in the known pattern
    }

    IDirect3DPixelShader9* Copy(IDirect3DDevice9* dev, int layers)
    {
        if (g_copy[layers] || g_copyFailed[layers])
            return g_copy[layers];
        static const char* kMix[4] = { "", "c = lerp(c, l1, m.x);", "c = lerp(c, l2, m.y);", "c = lerp(c, l3, m.z);" };
        std::string src =
            "struct In { float3 diffuse : COLOR0; float3 specular : COLOR1;";
        for (int i = 0; i <= layers; ++i)
            src += " float2 t" + std::to_string(i) + " : TEXCOORD" + std::to_string(i) + ";";
        src += " };\n";
        for (int i = 0; i <= layers; ++i)
            src += "sampler2D s" + std::to_string(i) + " : register(s" + std::to_string(i) + ");\n";
        src += "float4 gK : register(c31);\n"
               "float4 main(In i) : COLOR\n{\n";
        for (int i = 0; i < layers; ++i)
            src += "    float4 l" + std::to_string(i) + " = tex2D(s" + std::to_string(i) + ", i.t" +
                   std::to_string(i) + ");\n";
        src += "    float4 m = tex2D(s" + std::to_string(layers) + ", i.t" + std::to_string(layers) + ");\n"
               "    float4 c = l0;\n";
        for (int i = 1; i < layers; ++i)
            src += std::string("    ") + kMix[i] + "\n";
        src += "    float lit = lerp(1.0, m.w, gK.x);\n"
               "    float3 rgb = c.rgb * (lit * 0.3 + 0.7) * i.diffuse + (lit * c.w) * i.specular;\n"
               "    return float4(rgb, 1.0);\n}\n";

        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        if (!compile || FAILED(compile(src.c_str(), src.size(), "terrainshade", nullptr, nullptr, "main", "ps_2_0",
                                       0, 0, &code, &errs)) || !code)
        {
            Log("terrainshade: the copy for %d layers failed to compile: %s", layers,
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no compiler)");
            g_copyFailed[layers] = true;
        }
        else if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)),
                                                       &g_copy[layers])))
        {
            g_copy[layers] = nullptr;
            g_copyFailed[layers] = true;
            Log("terrainshade: the copy for %d layers could not be created", layers);
        }
        else
        {
            Log("terrainshade: copy for %d ground layers ready", layers);
        }
        if (code) code->lpVtbl->Release(code);
        if (errs) errs->lpVtbl->Release(errs);
        return g_copy[layers];
    }

    int Layers(IDirect3DPixelShader9* ps)
    {
        for (int i = 0; i < g_seenCount; ++i)
            if (g_seen[i].ps == ps)
                return g_seen[i].layers;
        const std::string t = Disassemble(ps);
        const int layers = t.empty() ? 0 : TerrainLayers(t);
        if (layers > 0)
            Log("terrainshade: pixel shader %p is the terrain's, %d ground layers", ps, layers);
        else if (layers < 0)
            Log("terrainshade: pixel shader %p uses the terrain shade's numbers in an unknown way; left alone:\n%s",
                ps, t.c_str());
        if (g_seenCount < 256)
            g_seen[g_seenCount++] = { ps, layers > 0 ? layers : 0 };
        return layers > 0 ? layers : 0;
    }
}

IDirect3DPixelShader9* TerrainShadeSwap(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, float keep)
{
    if (!ps || keep >= 0.999f)
        return ps;
    const int layers = Layers(ps);
    if (layers <= 0)
        return ps;
    IDirect3DPixelShader9* copy = Copy(dev, layers);
    if (!copy)
        return ps;
    const float k[4] = { keep < 0.0f ? 0.0f : keep, 0.0f, 0.0f, 0.0f };
    dev->lpVtbl->SetPixelShaderConstantF(dev, 31, k, 1);
    return copy;
}

bool TerrainShadeIsTerrain(IDirect3DPixelShader9* ps)
{
    if (!ps)
        return false;
    for (int i = 1; i < 5; ++i)
        if (g_copy[i] == ps)
            return true;
    for (int i = 0; i < g_seenCount; ++i)
        if (g_seen[i].ps == ps)
            return g_seen[i].layers > 0;
    return false;
}

void TerrainShadeReset()
{
    for (int i = 0; i < 5; ++i)
    {
        SafeRelease(g_copy[i]);
        g_copyFailed[i] = false;
    }
    g_seenCount = 0;
}
