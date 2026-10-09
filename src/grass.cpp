// grass: the grass in the wind. From comfygrass (2026-10-06), which was a DLL of its own until then; its
// README and src/README.md hold the reverse engineering, and NOTES.md, "The grass in the wind", the merge.
//
// The 1.12 client has no grass shader. Its five vertex shaders are patter, rain, sand, snowpoint and Model2, and
// the grass (the "detail doodads", WorldClient/DetailDoodad.cpp) is geometry the CPU builds and draws through
// the fixed-function pipeline. So a draw that is grass gets a vs_2_0 of ours, which sways each blade on the GPU
// from the client's own vertex buffer. Nothing is read back: that buffer is WRITEONLY and maps to uncached
// memory, which reads at about 20 MB/s and sank three CPU designs in comfygrass. D3D9 allows a vertex shader
// with the fixed-function pixel pipeline, so the shader only does the vertex work: the transform, the
// fixed-function lighting, the fog and the texture coordinates.
//
// A draw is grass when it has no vertex shader, its world matrix has no rotation (grass is batched per map
// chunk, so its world is a pure translation; trees and bushes share the vertex format but are rotated), its
// stride is 36 (POSITION f3, NORMAL f3, COLOR, TEXCOORD0 f2), it is a triangle list, and it has 64 vertices or
// more (about 65 small doodads share the format).
//
// The wind shader writes the vertex shader constants c0 to c21. The client uploads a constant only when its
// own copy changes, so the client's values are written back after each grass draw: rain (rain.bls) once drew
// with the grass's matrices and did not show.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "grass.h"
#include "bodymask.h"
#include "client.h"
#include "common.h"
#include "config.h"
#include "shadercache.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace
{
    constexpr float kTwoPi = 6.2831853f;
    constexpr UINT  kWindConsts = 22;

    GrassCalls g_calls = {};
    bool       g_attached = false;
    bool       g_otherDll = false;   // comfygrass.dll is loaded too: its grass, not ours

    // The client's values in the constants the wind shader writes, from its SetVertexShaderConstantF.
    float g_vsConst[kWindConsts][4] = {};
    bool  g_vsConstSeeded = false;   // read from the device once, before the first grass draw writes them

    IDirect3DVertexShader9* g_windVS = nullptr;
    bool                    g_windVSTried = false;

    // This frame's and last frame's draws, for the probe.
    unsigned g_draws = 0, g_verts = 0, g_drawsLast = 0, g_vertsLast = 0;

    bool SafeCopy(uintptr_t src, void* dst, size_t n)
    {
        __try
        {
            memcpy(dst, reinterpret_cast<const void*>(src), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // the wind and the player, worked out at the frame's first grass draw

    struct WindFrame
    {
        float d1[2], d2[2];
        float k1, k2, amp1, amp2, lean, phase1, phase2;
        float variance, anchor, scale;
    };
    WindFrame g_wf;
    bool      g_frameReady = false;

    void BuildWindFrame()
    {
        const GrassSettings& w = g_cfg.grass;
        const float t  = (GetTickCount() % 1000000u) * 0.001f;
        const float a1 = w.directionDeg * (kTwoPi / 360.0f);
        const float a2 = (w.directionDeg + w.crossAngleDeg) * (kTwoPi / 360.0f);
        g_wf.d1[0] = cosf(a1); g_wf.d1[1] = sinf(a1);
        g_wf.d2[0] = cosf(a2); g_wf.d2[1] = sinf(a2);
        g_wf.k1     = kTwoPi / (w.wavelength      > 0.1f ? w.wavelength      : 0.1f);
        g_wf.k2     = kTwoPi / (w.crossWavelength > 0.1f ? w.crossWavelength : 0.1f);
        g_wf.amp1   = w.amplitude;
        g_wf.amp2   = w.crossAmplitude;
        g_wf.lean   = w.lean;
        g_wf.phase1 = -fmodf(g_wf.k1 * w.speed * t, kTwoPi);
        g_wf.phase2 = -fmodf(g_wf.k2 * w.speed * 1.37f * t, kTwoPi);
        g_wf.variance = w.variance;
        g_wf.anchor   = w.anchor;
        g_wf.scale    = w.scale;
    }

    // The parting needs the player in the grass's own space. The client renders camera-relative: its view
    // matrix has no translation, and each draw's world matrix is the chunk's origin minus the camera. So the
    // value needed is the player minus the camera (client.cpp reads both, at the addresses comfygrass found).
    float g_anchorRel[3] = {};
    bool  g_anchorValid = false;

    void UpdateAnchor()
    {
        g_anchorValid = false;
        float cam[3], pl[3];
        bool onShip = false;
        if (!g_cfg.grass.parting || !ClientCamera(cam) || !ClientPlayer(pl, &onShip) || onShip)
            return;
        float d2 = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            g_anchorRel[i] = pl[i] - cam[i];
            d2 += g_anchorRel[i] * g_anchorRel[i];
        }
        g_anchorValid = d2 < 80.0f * 80.0f;
    }

    // ---------------------------------------------------------------------------------------------
    // the fallback bend: the texture's v, for a vertex without a height (see "detail models")
    //
    // v runs from the tip to the base, but only across a slice of the atlas, which differs per texture. So the
    // span is measured once per texture from a small sample. This read happens a few dozen times a session.

    struct Layout { int posOffset = -1, texOffset = -1; };
    std::map<IDirect3DVertexDeclaration9*, Layout> g_layouts;
    struct VSpan { float lo; float inv; };
    std::map<void*, VSpan> g_vspan;

    Layout LayoutFor(IDirect3DVertexDeclaration9* decl)
    {
        if (!decl)
            return Layout();
        auto it = g_layouts.find(decl);
        if (it != g_layouts.end())
            return it->second;
        Layout L;
        D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH + 1] = {};
        UINT n = 0;
        if (SUCCEEDED(decl->lpVtbl->GetDeclaration(decl, elems, &n)))
            for (UINT i = 0; i < n; ++i)
            {
                const D3DVERTEXELEMENT9& e = elems[i];
                if (e.Stream != 0 || e.Type == D3DDECLTYPE_UNUSED)
                    continue;
                if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_FLOAT3)
                    L.posOffset = e.Offset;
                if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 0 &&
                    (e.Type == D3DDECLTYPE_FLOAT2 || e.Type == D3DDECLTYPE_FLOAT3 || e.Type == D3DDECLTYPE_FLOAT4))
                    L.texOffset = e.Offset;
            }
        return g_layouts.emplace(decl, L).first->second;
    }

    VSpan SpanFor(IDirect3DDevice9* dev, const GrassDrawArgs& d, IDirect3DVertexBuffer9* vb, UINT vbOffset, UINT stride)
    {
        IDirect3DBaseTexture9* tex = nullptr;
        dev->lpVtbl->GetTexture(dev, 0, &tex);
        if (tex)
            tex->lpVtbl->Release(tex);   // the device holds it; only its address is the key
        auto it = g_vspan.find(tex);
        if (it != g_vspan.end())
            return it->second;

        IDirect3DVertexDeclaration9* decl = nullptr;
        dev->lpVtbl->GetVertexDeclaration(dev, &decl);
        const Layout L = LayoutFor(decl);
        if (decl)
            decl->lpVtbl->Release(decl);

        float lo = 3.4e38f, hi = -3.4e38f;
        const UINT n = d.nv < 256 ? d.nv : 256;
        if (L.texOffset >= 0 && n)
        {
            const UINT first = d.indexed ? static_cast<UINT>(d.bvi) + d.mvi : d.sv;
            void* p = nullptr;
            if (SUCCEEDED(vb->lpVtbl->Lock(vb, vbOffset + first * stride, n * stride, &p,
                                           D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) && p)
            {
                std::vector<uint8_t> buf(static_cast<size_t>(n) * stride);
                memcpy(buf.data(), p, buf.size());
                vb->lpVtbl->Unlock(vb);
                for (UINT k = 0; k < n; ++k)
                {
                    const float v = reinterpret_cast<const float*>(buf.data() + k * stride + L.texOffset)[1];
                    lo = v < lo ? v : lo;
                    hi = v > hi ? v : hi;
                }
            }
        }
        VSpan sp{ 0.0f, 1.0f };
        if (hi > lo && hi - lo > 1e-5f)
            sp = { lo, 1.0f / (hi - lo) };
        Log("grass: the texture v of %p runs %.4f .. %.4f", tex, lo, hi);
        return g_vspan.emplace(tex, sp).first->second;
    }

    // ---------------------------------------------------------------------------------------------
    // detail models: a height for every vertex, and rigid models
    //
    // The shader needs how high up its blade each vertex is: the root stays planted and the tip moves most. The
    // draw does not say, and the texture's v is a poor measure of it (BadGra01's top vertices have v 0.52 in a
    // span of 0.02 to 0.73). A ground layer's models are also rocks, pebbles, shells and bones, drawn in the same
    // batches as the grass: BadRoc01 and BadGra02 share 8des_detaildoodads01.blp.
    //
    // The client knows both while it fills a batch, so its fill loop at 0x006B2600 is patched in two places:
    //
    //   006B2690  mov edx, [esi+4]            ; esi = the instance, [esi+4] = its model index
    //   006B2693  mov eax, [0x00CAFFFC]       ; the table of CDetailDoodadData, one per model
    //   006B2698  mov eax, [eax+edx*4]        ; CDetailDoodadData: +0 file name, +4 model handle
    //   006B269E  call 0x00710E20             ; model handle -> M2 header
    //   ...
    //   006B26CD  mov ebx, [ebp+ebx*4-0x24]   ; pick the lit or the shadowed colour  <- patch 1
    //   006B26D1  mov [ebp-0x18], ebx         ; the colour for every vertex of this instance
    //   006B26D4  jmp 0x006B26E0              ; into the per-vertex loop
    //   ...                                   ; ecx = the model vertex, edi = the next output vertex
    //   006B2743  mov edx, [ebp-0x18]         ;                                      <- patch 2
    //   006B2746  mov [edi-0xC], edx          ; write the colour
    //   006B2749  mov eax, [ecx+0x20]         ; the uv follows
    //
    // Patch 1 runs once per instance. eax holds the M2 header: +0x44 vertex count, +0x48 vertices (0x30 bytes
    // each, z at +8), +0x50 the first view (+0 index count, +4 the u16 indices). It finds the model's top and
    // decides whether the model is rigid, kept per model.
    //
    // Patch 2 runs once per vertex. It divides the model vertex's z by the model's top, 0 at the ground and 1 at
    // the top, and stores it in 6 bits: the lowest 2 bits of red, of green and of blue. A rigid model stores 0.
    // The alpha's lowest bit is cleared as the sign that the colour carries a height. The client writes only
    // 0xFFFFFFFF and 0xFFC0C0C0 (in shade), so the shader puts the exact colour back. The fixed-function path,
    // which draws the grass with the effect off, sees each channel off by at most 3/255.
    //
    // A model is rigid if its top is lower than rigidHeight, or its file name holds a word of rigidNames. Over
    // the 447 models in GroundEffectDoodad.dbc every rock is lower than 0.28 yards, every model lower than 0.3
    // is a rock, a pebble, a shell, a low tuft or a small mushroom, and grass starts at 0.31. The bones in
    // Deadwind Pass and the Eastern Plaguelands reach 0.93 yards, so the name catches those.
    //
    // Before the patch, the whole loop (0xE7 bytes from 0x006B2690) is compared with the bytes expected. Another
    // WoW.exe fails the check: nothing is patched, and the bend falls back to the texture's v.

    constexpr DWORD   kFillBlockBack = 0x3D;         // the check starts at 0x006B2690
    constexpr uint8_t kFillBlock[]   = {
        0x8B, 0x56, 0x04, 0xA1, 0xFC, 0xFF, 0xCA, 0x00, 0x8B, 0x04, 0x90, 0x8B, 0x48, 0x04, 0xE8, 0x7D,
        0xE7, 0x05, 0x00, 0x8B, 0x50, 0x50, 0x8B, 0x0A, 0x89, 0x4D, 0xF8, 0x8A, 0x0E, 0x33, 0xDB, 0xF6,
        0xC1, 0x01, 0x89, 0x45, 0xEC, 0x89, 0x55, 0xF0, 0x74, 0x05, 0xBB, 0x01, 0x00, 0x00, 0x00, 0x33,
        0xC9, 0x39, 0x4D, 0xF8, 0x89, 0x4D, 0xFC, 0x0F, 0x86, 0x9A, 0x00, 0x00, 0x00, 0x8B, 0x5C, 0x9D,
        0xDC, 0x89, 0x5D, 0xE8, 0xEB, 0x0A, 0x8B, 0x55, 0xF0, 0x8B, 0x45, 0xEC, 0x8D, 0x64, 0x24, 0x00,
        0x8B, 0x52, 0x04, 0x0F, 0xB7, 0x0C, 0x4A, 0x8B, 0x50, 0x48, 0x8D, 0x0C, 0x49, 0xC1, 0xE1, 0x04,
        0x03, 0xCA, 0x8D, 0x46, 0x20, 0x8D, 0x57, 0x0C, 0xD9, 0x01, 0x83, 0xC7, 0x24, 0xD8, 0x4E, 0x18,
        0xD9, 0x46, 0x14, 0xD8, 0x49, 0x04, 0xDE, 0xE9, 0xD8, 0x4E, 0x1C, 0xD8, 0x46, 0x08, 0xD9, 0x5F,
        0xDC, 0xD9, 0x01, 0xD8, 0x4E, 0x14, 0xD9, 0x46, 0x18, 0xD8, 0x49, 0x04, 0xDE, 0xC1, 0xD8, 0x4E,
        0x1C, 0xD8, 0x46, 0x0C, 0xD9, 0x5F, 0xE0, 0xD9, 0x41, 0x08, 0xD8, 0x4E, 0x1C, 0xD8, 0x46, 0x10,
        0xD9, 0x5F, 0xE4, 0x8B, 0x18, 0x89, 0x1A, 0x8B, 0x58, 0x04, 0x89, 0x5A, 0x04, 0x8B, 0x40, 0x08,
        0x89, 0x42, 0x08, 0x8B, 0x55, 0xE8, 0x89, 0x57, 0xF4, 0x8B, 0x41, 0x20, 0x89, 0x47, 0xF8, 0x8B,
        0x49, 0x24, 0x8B, 0x45, 0xF8, 0x89, 0x4F, 0xFC, 0x8B, 0x4D, 0xFC, 0x41, 0x3B, 0xC8, 0x89, 0x4D,
        0xFC, 0x0F, 0x82, 0x6F, 0xFF, 0xFF, 0xFF, 0x8B, 0x45, 0xF4, 0x83, 0xC6, 0x2C, 0x48, 0x89, 0x45,
        0xF4, 0x0F, 0x85, 0x19, 0xFF, 0xFF, 0xFF,
    };
    constexpr DWORD kInstanceLen    = 7;     // patch 1 replaces two instructions, 7 bytes
    constexpr DWORD kInstanceResume = 0x13;  // 0x006B26E0, where the jmp at 0x006B26D4 goes
    constexpr DWORD kVertexAt       = 0x76;  // patch 2, 0x006B2743
    constexpr DWORD kVertexLen      = 6;     // two instructions, 6 bytes
    constexpr int   kHeightSteps    = 63;    // 6 bits

    DWORD g_instanceResume = 0;    // the addresses the stubs return to
    DWORD g_vertexResume   = 0;
    DWORD g_modelTable     = 0;    // 0x00CAFFFC, read out of the checked code
    bool  g_fillTried      = false;
    bool  g_fillPatched    = false;

    // Written by patch 1 for the instance, read by patch 2 for each of its vertices.
    DWORD g_encode      = 0;       // 1: store the height in the colour
    float g_heightScale = 0.0f;    // kHeightSteps / the model's top, or 0 for a rigid model
    DWORD g_heightTmp   = 0;

    // Kept per model. The key holds both the M2 header and the CDetailDoodadData, so a model freed and
    // replaced at the same address is not taken for the old one.
    struct ModelEntry { const void* model; const void* data; float scale; };
    ModelEntry g_modelCache[64];
    int        g_modelCount = 0;
    float      g_cachedRigidHeight = -1.0f;   // the settings the cache was made with
    char       g_cachedRigidNames[128] = {};

    const void* DetailData(const uint8_t* inst)
    {
        const uint8_t* const* table =
            *reinterpret_cast<const uint8_t* const* const*>(static_cast<uintptr_t>(g_modelTable));
        return table[*reinterpret_cast<const DWORD*>(inst + 4)];
    }

    // The model's top and file name. Guarded, with no C++ objects, so that __try is allowed.
    bool ReadDetailModel(const uint8_t* model, const void* data, float* topOut, char* nameOut, size_t nameCount)
    {
        __try
        {
            const DWORD     nVerts = *reinterpret_cast<const DWORD*>(model + 0x44);
            const uint8_t*  verts  = *reinterpret_cast<const uint8_t* const*>(model + 0x48);
            const uint8_t*  view   = *reinterpret_cast<const uint8_t* const*>(model + 0x50);
            const DWORD     nIdx   = *reinterpret_cast<const DWORD*>(view);
            const uint16_t* idx    = *reinterpret_cast<const uint16_t* const*>(view + 4);
            float top = -3.4e38f;
            for (DWORD k = 0; k < nIdx; ++k)
            {
                const DWORD i = idx[k];
                if (i >= nVerts)
                    continue;
                const float z = *reinterpret_cast<const float*>(verts + i * 0x30 + 8);
                if (z > top)
                    top = z;
            }
            const char* name = *reinterpret_cast<const char* const*>(data);
            *topOut = top;
            strncpy_s(nameOut, nameCount, name ? name : "", _TRUNCATE);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Case-insensitive: is one of the comma-separated words in list part of name?
    bool NameMatches(const char* name, const char* list)
    {
        char lower[128];
        size_t n = 0;
        for (; name[n] && n + 1 < sizeof(lower); ++n)
            lower[n] = static_cast<char>(tolower(static_cast<unsigned char>(name[n])));
        lower[n] = 0;
        const char* p = list;
        while (*p)
        {
            while (*p == ',' || *p == ' ' || *p == '\t')
                ++p;
            char   word[32];
            size_t w = 0;
            while (*p && *p != ',' && *p != ' ' && *p != '\t')
            {
                if (w + 1 < sizeof(word))
                    word[w++] = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
                ++p;
            }
            word[w] = 0;
            if (w && strstr(lower, word))
                return true;
        }
        return false;
    }

    // Patch 1, once per instance while the client fills a batch: g_encode and g_heightScale for its vertices.
    void __cdecl InstanceSetup(const uint8_t* model, const uint8_t* inst)
    {
        g_encode = 0;
        const GrassSettings& g = g_cfg.grass;
        if (!g.models)
            return;

        const void* data = DetailData(inst);
        for (int k = 0; k < g_modelCount; ++k)
            if (g_modelCache[k].model == model && g_modelCache[k].data == data)
            {
                g_heightScale = g_modelCache[k].scale;
                g_encode      = 1;
                return;
            }

        float top = 0.0f;
        char  name[64];
        if (!ReadDetailModel(model, data, &top, name, sizeof(name)))
            return;
        const bool  low   = top < g.rigidHeight;
        const bool  named = NameMatches(name, g.rigidNames);
        const bool  flat  = top < 0.01f;   // nothing to divide by
        const float scale = (low || named || flat) ? 0.0f : kHeightSteps / top;
        Log("grass: detail model %s: top %.2f yards, %s", name, top,
            scale == 0.0f ? (named ? "rigid (name)" : "rigid (low)") : "moves");

        if (g_modelCount == static_cast<int>(sizeof(g_modelCache) / sizeof(g_modelCache[0])))
            g_modelCount = 0;   // more models than slots: start again, do not grow
        g_modelCache[g_modelCount++] = { model, data, scale };
        g_heightScale = scale;
        g_encode      = 1;
    }

    // Patch 1. On entry: eax = the M2 header, esi = the instance, ebx = 0 or 1 (in shade).
    __declspec(naked) void InstanceStub()
    {
        __asm
        {
            pushad
            push esi
            push eax
            call InstanceSetup
            add  esp, 8
            popad
            mov  ebx, [ebp + ebx * 4 - 0x24]
            mov  [ebp - 0x18], ebx
            jmp  dword ptr [g_instanceResume]
        }
    }

    // Patch 2. On entry: ecx = the model vertex, edi = the output vertex + 0x24. eax, ebx and edx are free: the
    // client loads all three again before it reads them. The x87 stack is empty, and fld/fistp leave it so.
    __declspec(naked) void VertexStub()
    {
        __asm
        {
            mov  edx, [ebp - 0x18]
            cmp  dword ptr [g_encode], 0
            je   store

            fld   dword ptr [ecx + 8]
            fmul  dword ptr [g_heightScale]
            fistp dword ptr [g_heightTmp]
            mov   eax, [g_heightTmp]
            test  eax, eax
            jge   notBelow
            xor   eax, eax            // below the ground, or out of range
        notBelow:
            cmp   eax, 63
            jle   inRange
            mov   eax, 63
        inRange:
            and  edx, 0xFEFCFCFC      // alpha 0xFE marks a height; clear 2 bits of r, g and b
            mov  ebx, eax
            and  ebx, 3
            shl  ebx, 16
            or   edx, ebx             // red:   bits 0-1 of the height
            mov  ebx, eax
            shr  ebx, 2
            and  ebx, 3
            shl  ebx, 8
            or   edx, ebx             // green: bits 2-3
            shr  eax, 4
            or   edx, eax             // blue:  bits 4-5
        store:
            mov  [edi - 0x0C], edx
            jmp  dword ptr [g_vertexResume]
        }
    }

    bool WriteCode(uintptr_t at, const uint8_t* code, DWORD len)
    {
        DWORD prot = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(at), len, PAGE_EXECUTE_READWRITE, &prot))
            return false;
        memcpy(reinterpret_cast<void*>(at), code, len);
        VirtualProtect(reinterpret_cast<void*>(at), len, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), len);
        return true;
    }

    bool WriteJump(uintptr_t at, const void* to, DWORD len)
    {
        uint8_t code[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (at + 5));
        memcpy(code + 1, &rel, sizeof(rel));
        return WriteCode(at, code, len);
    }

    // At Present, on the render thread, so the fill loop cannot run while the bytes change.
    void InstallFillPatch()
    {
        g_fillTried = true;
        const DWORD fillAddr = g_cfg.grass.fillAddr;
        const intptr_t  slide = reinterpret_cast<intptr_t>(GetModuleHandleW(nullptr)) - 0x00400000;
        const uintptr_t site  = static_cast<uintptr_t>(static_cast<intptr_t>(fillAddr) + slide);
        uint8_t have[sizeof(kFillBlock)] = {};
        if (!fillAddr || !SafeCopy(site - kFillBlockBack, have, sizeof(have)) ||
            memcmp(have, kFillBlock, sizeof(have)) != 0)
        {
            Log("grass: the code at 0x%08X is not the expected fill loop. Nothing is patched: rocks move, and the "
                "bend comes from the texture.", fillAddr);
            return;
        }
        memcpy(&g_modelTable, have + 4, sizeof(g_modelTable));
        g_instanceResume = static_cast<DWORD>(site + kInstanceResume);
        g_vertexResume   = static_cast<DWORD>(site + kVertexAt + kVertexLen);

        // Patch 2 first: until patch 1 is in, g_encode stays 0 and patch 2 writes the colour unchanged.
        if (!WriteJump(site + kVertexAt, &VertexStub, kVertexLen) || !WriteJump(site, &InstanceStub, kInstanceLen))
        {
            Log("grass: VirtualProtect failed (%lu), the fill loop is not patched", GetLastError());
            return;
        }
        g_fillPatched = true;
        Log("grass: fill loop patched at 0x%08X and 0x%08X (model table 0x%08X)", static_cast<DWORD>(site),
            static_cast<DWORD>(site + kVertexAt), g_modelTable);
    }

    // ---------------------------------------------------------------------------------------------
    // the wind shader

    const char* kWindHlsl = R"HLSL(
struct VsIn  { float3 pos : POSITION; float3 nrm : NORMAL; float4 col : COLOR0; float2 uv : TEXCOORD0; };
struct VsOut { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; float fog : FOG; };

float4 c0 : register(c0);   // rows of the transposed world-view-projection
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 gW1 : register(c4);  // first wave:  dir.xy, amplitude, wavenumber
float4 gW2 : register(c5);  // second wave: dir.xy, amplitude, wavenumber
float4 gPh : register(c6);  // phase1, phase2, lean, variance
float4 gSh : register(c7);  // anchor, 1/(1-anchor), scale, unused
float4 gWT : register(c8);  // world translation (the chunk's origin), with worldPhase
float4 gFg : register(c9);  // fogStart, fogEnd, 1/(end-start), fogEnable
float4 gAm : register(c10); // global ambient rgb: D3DRS_AMBIENT plus every enabled light's Ambient
float4 gLD : register(c11); // light direction xyz, enabled
float4 gLC : register(c12); // light diffuse colour rgb
float4 gVS : register(c13); // v at the blade's tip, 1/(base-tip)
float4 gPC : register(c14); // parting's centre in chunk-local space (xyz), parting on (w)
float4 gPR : register(c15); // 1/radius, force at the centre, force at the edge, 1/zFade
float4 gPM : register(c16); // radius, zRange, unused, unused
float4 gMD : register(c17); // material diffuse
float4 gMA : register(c18); // material ambient
float4 gME : register(c19); // material emissive
float4 gMS : register(c20); // ambient/diffuse/emissive from the vertex colour (1) or the material (0), lighting on
float4 gDb : register(c21); // x: 1 = the bend as a colour (Debug View 30)

VsOut main(VsIn i)
{
    VsOut o;
    float3 p = i.pos;

    // A vertex with alpha 0xFE carries its height up the model in the lowest 2 bits of r, g and b (see "detail
    // models"): 0 at the ground, 63 at the top, 0 on a rigid model. The client writes only 0xFF and 0xC0 in
    // those channels, so the exact colour comes back: clear the bits, then 0xFC can only have been 0xFF.
    float4 c255   = floor(i.col * 255.0 + 0.5);
    float  marked = step(c255.a, 254.5);
    float3 bits   = c255.rgb - 4.0 * floor(c255.rgb * 0.25);
    float  height = dot(bits, float3(1.0, 4.0, 16.0)) * (1.0 / 63.0);
    float3 plain  = c255.rgb - bits;
    plain += 3.0 * step(251.5, plain);
    float4 col    = lerp(i.col, float4(plain * (1.0 / 255.0), 1.0), marked);

    // The bend weight. The square gives a stiff base and a loose tip. Without a height: v, normalised over the
    // span measured for the texture, with the lowest share held still, as the quads are sunk into the ground.
    float base = saturate((i.uv.y - gVS.x) * gVS.y);
    float tip  = 1.0 - base;
    float wuv  = saturate((tip - gSh.x) * gSh.y);
    float w    = lerp(wuv * wuv, height * height, marked);

    // The phase from the chunk-local position, which does not move with the camera, plus gWT with worldPhase.
    float2 wxy    = p.xy + gWT.xy;
    float  jit    = frac(wxy.x * 0.737 + wxy.y * 1.311);
    float  varMul = (1.0 - gPh.w * 0.5) + gPh.w * jit;
    float  phj    = jit * 1.7;

    float s1 = sin(dot(wxy, gW1.xy) * gW1.w + gPh.x + phj);
    float s2 = sin(dot(wxy, gW2.xy) * gW2.w + gPh.y + phj);

    float2 off = gW1.xy * (gW1.z * (s1 + gPh.z)) + gW2.xy * (gW2.z * s2);
    off *= w * varMul * gSh.z;

    // The parting: blades lean away from the player. gPC is in this vertex's own chunk-local space, so this is
    // one subtraction. The weight that hinges the wind hinges this too. The z term stops grass on a ledge above
    // or below the player from reacting.
    float3 dp    = i.pos - gPC.xyz;
    float  dd    = dot(dp.xy, dp.xy) + 1e-4;
    float  dinv  = rsqrt(dd);
    float  dist  = dd * dinv;
    float  force = lerp(gPR.y, gPR.z, saturate(dist * gPR.x));
    force *= step(dist, gPM.x) * saturate((gPM.y - abs(dp.z)) * gPR.w) * w * gPC.w;
    off += dp.xy * (dinv * force);

    p.xy += off;

    float4 wp = float4(p, 1.0);
    o.pos = float4(dot(wp, c0), dot(wp, c1), dot(wp, c2), dot(wp, c3));

    // The fixed-function lighting, with the material sources the client set:
    //     emissive + ambientMat * globalAmbient + diffuseMat * lightDiffuse * N.L
    float3 N    = normalize(i.nrm);
    float  ndl  = saturate(dot(N, -gLD.xyz)) * gLD.w;
    float4 ambM = lerp(gMA, col, gMS.x);
    float4 difM = lerp(gMD, col, gMS.y);
    float4 emiM = lerp(gME, col, gMS.z);
    float3 lit  = saturate(emiM.rgb + ambM.rgb * gAm.rgb + difM.rgb * gLC.rgb * ndl);
    o.col = lerp(col, float4(lit, difM.a), gMS.w);
    // Debug View 30: the bend, black where still and white at the tips; blue where it comes from the texture.
    o.col = lerp(o.col, float4(w, w, lerp(1.0, w, marked), 1.0), gDb.x);

    o.uv  = i.uv;
    o.fog = lerp(1.0, saturate((gFg.y - o.pos.w) * gFg.z), gFg.w);
    return o;
}
)HLSL";

    bool EnsureWindShader(IDirect3DDevice9* dev)
    {
        if (g_windVS)
            return true;
        if (g_windVSTried)
            return false;
        g_windVSTried = true;
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        if (!compile || FAILED(compile(kWindHlsl, strlen(kWindHlsl), "grass", nullptr, nullptr, "main", "vs_2_0", 0, 0,
                                       &code, &errs)) || !code)
            Log("grass: the wind shader failed to compile: %s",
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no compiler)");
        else if (FAILED(dev->lpVtbl->CreateVertexShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)),
                                                        &g_windVS)) || !g_windVS)
        {
            Log("grass: the wind shader could not be created");
            g_windVS = nullptr;
        }
        if (code) code->lpVtbl->Release(code);
        if (errs) errs->lpVtbl->Release(errs);
        return g_windVS != nullptr;
    }

    void MatMul(D3DMATRIX& out, const D3DMATRIX& a, const D3DMATRIX& b)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] +
                              a.m[r][2] * b.m[2][c] + a.m[r][3] * b.m[3][c];
    }

    bool IsGrass(const GrassDrawArgs& d)
    {
        const GrassSettings& g = g_cfg.grass;
        if (g.primType && static_cast<int>(d.prim) != g.primType)
            return false;
        if ((g.minVerts && d.nv < g.minVerts) || (g.maxVerts && d.nv > g.maxVerts))
            return false;
        if (d.indexed && d.bvi + static_cast<INT>(d.mvi) < 0)
            return false;
        // Grass is batched per map chunk, so its world matrix is a pure translation. Without this test the wind
        // bent the trees too.
        const D3DMATRIX& w = *d.world;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                if (fabsf(w.m[r][c] - (r == c ? 1.0f : 0.0f)) > 1e-3f)
                    return false;
        return true;
    }

    float RsFloat(IDirect3DDevice9* dev, D3DRENDERSTATETYPE st, float dflt)
    {
        DWORD v = 0;
        if (FAILED(dev->lpVtbl->GetRenderState(dev, st, &v)))
            return dflt;
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
}

void GrassAttach(const GrassCalls& calls)
{
    g_calls = calls;
    g_attached = calls.drawPrim && calls.drawIdxPrim && calls.setVS && calls.setVSConstF;
    // The old DLL draws its own grass from its own hooks; two would bend each blade twice.
    g_otherDll = GetModuleHandleA("comfygrass.dll") != nullptr;
    if (g_otherDll)
        Log("grass: comfygrass.dll is loaded too, so its grass is drawn and ours stays off. Take its line out of "
            "dlls.txt: comfyatmos.dll draws the grass now.");
}

bool GrassDraw(IDirect3DDevice9* dev, const GrassDrawArgs& d, HRESULT& hr)
{
    const GrassSettings& g = g_cfg.grass;
    if (!g_attached || g_otherDll || !g.enabled || !g_cfg.master || !IsGrass(d))
        return false;

    IDirect3DVertexBuffer9* vb = nullptr;
    UINT vbOffset = 0, stride = 0;
    if (FAILED(dev->lpVtbl->GetStreamSource(dev, 0, &vb, &vbOffset, &stride)) || !vb)
        return false;
    vb->lpVtbl->Release(vb);   // the device holds it for the draw
    if (g.stride && stride != g.stride)
        return false;
    if (!EnsureWindShader(dev))
        return false;

    if (!g_frameReady)
    {
        g_frameReady = true;
        BuildWindFrame();
        UpdateAnchor();
    }

    D3DMATRIX wv, wvp;
    MatMul(wv, *d.world, *d.view);
    MatMul(wvp, wv, *d.proj);

    // The transpose, so each register holds a column and dot(v, c[n]) is D3D9's row-vector multiply.
    float c[kWindConsts][4] = {};
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            c[r][k] = wvp.m[k][r];
    c[4][0] = g_wf.d1[0];   c[4][1] = g_wf.d1[1];   c[4][2] = g_wf.amp1; c[4][3] = g_wf.k1;
    c[5][0] = g_wf.d2[0];   c[5][1] = g_wf.d2[1];   c[5][2] = g_wf.amp2; c[5][3] = g_wf.k2;
    c[6][0] = g_wf.phase1;  c[6][1] = g_wf.phase2;  c[6][2] = g_wf.lean; c[6][3] = g_wf.variance;
    c[7][0] = g_wf.anchor;  c[7][1] = 1.0f / (1.0f - g_wf.anchor);  c[7][2] = g_wf.scale;

    // With worldPhase the phase adds the draw's world offset, which is camera-relative: the pattern runs on
    // across chunks, but slides as the camera moves.
    if (g.worldPhase)
    {
        c[8][0] = d.world->m[3][0];
        c[8][1] = d.world->m[3][1];
        c[8][2] = d.world->m[3][2];
    }

    // The fog and the lighting, read from the device: the client may have set them before the hooks went in,
    // and a stale default made the grass dark in comfygrass. D3D9's own defaults stand in if a read fails.
    auto rs = [dev](D3DRENDERSTATETYPE st, DWORD def) {
        DWORD v = def;
        return SUCCEEDED(dev->lpVtbl->GetRenderState(dev, st, &v)) ? v : def;
    };
    const float fogStart = RsFloat(dev, D3DRS_FOGSTART, 0.0f), fogEnd = RsFloat(dev, D3DRS_FOGEND, 1.0f);
    const float span = fogEnd - fogStart > 0.001f ? fogEnd - fogStart : 1.0f;
    c[9][0] = fogStart; c[9][1] = fogEnd; c[9][2] = 1.0f / span;
    c[9][3] = rs(D3DRS_FOGENABLE, FALSE) ? 1.0f : 0.0f;

    const DWORD lighting    = rs(D3DRS_LIGHTING, TRUE);
    const DWORD colorVertex = rs(D3DRS_COLORVERTEX, TRUE);
    const DWORD ambSrc      = rs(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    const DWORD difSrc      = rs(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    const DWORD emiSrc      = rs(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    const DWORD amb         = rs(D3DRS_AMBIENT, 0);
    // The grass vertex has a diffuse colour and no specular, so only COLOR1 reads the vertex; D3D falls back to
    // the material for COLOR2, and for every source when COLORVERTEX is off.
    auto fromVertex = [colorVertex](DWORD src) { return (colorVertex && src == D3DMCS_COLOR1) ? 1.0f : 0.0f; };

    D3DMATERIAL9 mat = {};
    dev->lpVtbl->GetMaterial(dev, &mat);
    const D3DCOLORVALUE* mats[3] = { &mat.Diffuse, &mat.Ambient, &mat.Emissive };
    for (int m = 0; m < 3; ++m)
    {
        c[17 + m][0] = mats[m]->r; c[17 + m][1] = mats[m]->g;
        c[17 + m][2] = mats[m]->b; c[17 + m][3] = mats[m]->a;
    }
    c[20][0] = fromVertex(ambSrc);
    c[20][1] = fromVertex(difSrc);
    c[20][2] = fromVertex(emiSrc);
    c[20][3] = lighting ? 1.0f : 0.0f;
    c[10][0] = ((amb >> 16) & 0xFF) / 255.0f;
    c[10][1] = ((amb >>  8) & 0xFF) / 255.0f;
    c[10][2] = ((amb      ) & 0xFF) / 255.0f;

    // Every enabled light adds its Ambient to the global ambient; the first directional one gives the diffuse.
    for (DWORD li = 0; li < 8; ++li)
    {
        BOOL on = FALSE;
        if (FAILED(dev->lpVtbl->GetLightEnable(dev, li, &on)) || !on)
            continue;
        D3DLIGHT9 L = {};
        if (FAILED(dev->lpVtbl->GetLight(dev, li, &L)))
            continue;
        c[10][0] += L.Ambient.r; c[10][1] += L.Ambient.g; c[10][2] += L.Ambient.b;
        if (L.Type != D3DLIGHT_DIRECTIONAL || c[11][3] != 0.0f)
            continue;
        float dx = L.Direction.x, dy = L.Direction.y, dz = L.Direction.z;
        const float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len > 1e-4f) { dx /= len; dy /= len; dz /= len; }
        c[11][0] = dx; c[11][1] = dy; c[11][2] = dz; c[11][3] = 1.0f;
        c[12][0] = L.Diffuse.r; c[12][1] = L.Diffuse.g; c[12][2] = L.Diffuse.b;
    }

    static bool litLogged = false;
    if (!litLogged)
    {
        litLogged = true;
        Log("grass: lighting LIGHTING=%u COLORVERTEX=%u src amb/dif/emi=%u/%u/%u ambient=0x%08X globalAmb=(%.2f %.2f "
            "%.2f) sun=(%.2f %.2f %.2f) on=%.0f mat dif=(%.2f %.2f %.2f) amb=(%.2f %.2f %.2f) emi=(%.2f %.2f %.2f)",
            lighting, colorVertex, ambSrc, difSrc, emiSrc, amb, c[10][0], c[10][1], c[10][2], c[12][0], c[12][1],
            c[12][2], c[11][3], mat.Diffuse.r, mat.Diffuse.g, mat.Diffuse.b, mat.Ambient.r, mat.Ambient.g,
            mat.Ambient.b, mat.Emissive.r, mat.Emissive.g, mat.Emissive.b);
    }

    // The texture's span, needed only when the fill loop is not patched.
    VSpan uv{ 0.0f, 1.0f };
    if (!g_fillPatched || !g.models)
        uv = SpanFor(dev, d, vb, vbOffset, stride);
    c[13][0] = uv.lo;
    c[13][1] = uv.inv;

    // The parting. The anchor is camera-relative and the vertices chunk-local, so the draw's world offset is
    // folded in here: (p + world) - anchor == p - (anchor - world).
    c[14][0] = g_anchorRel[0] - d.world->m[3][0];
    c[14][1] = g_anchorRel[1] - d.world->m[3][1];
    c[14][2] = g_anchorRel[2] - d.world->m[3][2] + g.centerZ;
    c[14][3] = g.parting && g_anchorValid ? 1.0f : 0.0f;
    c[15][0] = 1.0f / g.radius; c[15][1] = g.forceCenter;
    c[15][2] = g.forceEdge;     c[15][3] = 1.0f / g.zFade;
    c[16][0] = g.radius;        c[16][1] = g.zRange;
    c[21][0] = g.debug ? 1.0f : 0.0f;

    if (!g_vsConstSeeded)
    {
        g_vsConstSeeded = true;
        if (FAILED(dev->lpVtbl->GetVertexShaderConstantF(dev, 0, &g_vsConst[0][0], kWindConsts)))
            Log("grass: the vertex shader constants could not be read; the client's later writes restore them");
    }
    if (FAILED(g_calls.setVSConstF(dev, 0, &c[0][0], kWindConsts)) || FAILED(g_calls.setVS(dev, g_windVS)))
    {
        g_calls.setVSConstF(dev, 0, &g_vsConst[0][0], kWindConsts);
        return false;
    }
    BodyMarkGrass(dev);   // the sun shadows shade it as the ground round it (2026-10-08)
    hr = d.indexed ? g_calls.drawIdxPrim(dev, d.prim, d.bvi, d.mvi, d.nv, d.si, d.pc)
                   : g_calls.drawPrim(dev, d.prim, d.sv, d.pc);
    // The fixed-function pipeline back, and the client's constants.
    g_calls.setVS(dev, nullptr);
    g_calls.setVSConstF(dev, 0, &g_vsConst[0][0], kWindConsts);
    ++g_draws;
    g_verts += d.nv;
    return true;
}

void GrassConstants(UINT reg, const float* data, UINT count)
{
    if (!data)
        return;
    for (UINT r = reg; r < reg + count && r < kWindConsts; ++r)
        memcpy(g_vsConst[r], data + (r - reg) * 4, sizeof(g_vsConst[r]));
}

void GrassFrameEnd()
{
    g_drawsLast = g_draws;
    g_vertsLast = g_verts;
    g_draws = g_verts = 0;
    g_frameReady = false;
    const GrassSettings& g = g_cfg.grass;
    if (!g_attached || g_otherDll || !g.enabled || !g_cfg.master)
        return;
    if (g.models && !g_fillTried)
        InstallFillPatch();
    // New rigid rules apply to the models the client fills from now on.
    if (g.rigidHeight != g_cachedRigidHeight || strcmp(g.rigidNames, g_cachedRigidNames) != 0)
    {
        g_modelCount = 0;
        g_cachedRigidHeight = g.rigidHeight;
        strcpy_s(g_cachedRigidNames, g.rigidNames);
    }
}

void GrassReset()
{
    if (g_windVS)
        g_windVS->lpVtbl->Release(g_windVS);
    g_windVS = nullptr;
    g_windVSTried = false;
    g_layouts.clear();
    g_vspan.clear();
    g_vsConstSeeded = false;   // a Reset puts the constants back to their defaults
}

// The hot reload (2026-10-09): the client's own bytes back, from the block checked before the patch. At Present, as
// the patch was put in.
void GrassDetach()
{
    if (!g_fillPatched)
        return;
    const uintptr_t site = g_instanceResume - kInstanceResume;
    if (WriteCode(site, kFillBlock + kFillBlockBack, kInstanceLen) &&
        WriteCode(site + kVertexAt, kFillBlock + kFillBlockBack + kVertexAt, kVertexLen))
    {
        g_fillPatched = false;
        Log("grass: the fill loop's patch is taken out");
    }
    else
        Log("grass: VirtualProtect failed (%lu), the fill loop's patch stays", GetLastError());
}

void GrassProbe()
{
    const GrassSettings& g = g_cfg.grass;
    Log("grass: %s; last frame %u draws, %u vertices; the fill loop %s; the wind shader %s; the parting %s",
        g_otherDll ? "off, comfygrass.dll draws it" : !g.enabled || !g_cfg.master ? "off" : "on", g_drawsLast,
        g_vertsLast, g_fillPatched ? "patched (each vertex has its height)" : g_fillTried ? "NOT patched (the bend "
        "comes from the texture)" : "not patched yet", g_windVS ? "ready" : g_windVSTried ? "FAILED" : "not made yet",
        g_anchorValid ? "on the player" : "off (no player, on a ship, or [grass] parting 0)");
}

void GrassShaderList()
{
    ShaderPrecompile("grass", kWindHlsl, "vs_2_0");
}
