// bodymask: which pixels on screen show a player or a creature, for the sun shadows (2026-10-01).
//
// The sun shadows treat a body and the ground beside it differently: a body takes its own shade from the
// units' map with the slack against a surface shading itself, the ground with almost none, so a shadow
// reaches the feet; and Character Backside Shadow scales the shade on a body alone. The depth maps cannot
// tell the two apart, and an upright cylinder about each unit took the cobbles at a character's feet for
// its body. So the client's own draws mark them.
//
// The mark: one bit (0x80) of the stencil that comes with the world's depth buffer (depth.cpp's INTZ, 24-bit
// depth and 8-bit stencil). While the world is drawn, every depth-writing draw writes that bit where it
// passes the depth test: set for a model the shadow cache knows stands at a unit (ShadowIsUnitDraw), cleared
// for anything else. So a tree drawn after a character and in front of it clears the mark where it hides
// the character. Blended draws leave it alone, as they leave the depth. The client's stencil states are
// kept as they were and put back when the world ends; if the client has the stencil on itself, nothing is
// marked that frame.
//
// The mask: when the world ends, with its render target and depth buffer still bound, one full-screen quad
// passes the stencil test where the bit is set and writes 1 into a target of the screen's size. A
// multisampled depth buffer needs a multisampled target, resolved into a texture; the edge of a body then
// comes out between 0 and 1. The sun shadows read the texture beside the depth.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "bodymask.h"
#include "client.h"
#include "common.h"
#include "config.h"
#include "shadow.h"
#include "volume.h"

#include <cmath>
#include <cstring>

namespace
{
    const char* kOneHlsl = "float4 main() : COLOR { return float4(1.0, 1.0, 1.0, 1.0); }\n";

    constexpr DWORD kBit = 0x80;

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    // The client's stencil states as the world began, put back when it ends.
    const D3DRENDERSTATETYPE kStates[] = {
        D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
        D3DRS_STENCILPASS, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_TWOSIDEDSTENCILMODE,
    };
    constexpr int kStateCount = sizeof(kStates) / sizeof(kStates[0]);
    DWORD g_saved[kStateCount] = {};

    bool  g_started   = false;   // this world: the mark is on
    bool  g_skipFrame = false;   // this world: the client had the stencil on, nothing is marked
    bool  g_anyUnit   = false;   // this world: at least one draw was marked
    DWORD g_ref = 0, g_writeMask = 0;   // what is set now, so a state is set only when it changes
    unsigned g_clientStencilFrames = 0;
    // The probe (2026-10-02): for a few frames, every depth-writing model draw near the player, marked or not.
    int      g_probeFrames = 0, g_probeLines = 0, g_probeFrame = 0;
    unsigned g_probeMarked = 0, g_probeUnmarked = 0;

    // The mask.
    IDirect3DSurface9*     g_msSurf  = nullptr;   // multisampled target, when the depth buffer is
    IDirect3DTexture9*     g_tex     = nullptr;   // the texture the shadows read
    IDirect3DSurface9*     g_texSurf = nullptr;
    UINT                   g_w = 0, g_h = 0;
    D3DMULTISAMPLE_TYPE    g_ms = D3DMULTISAMPLE_NONE;
    DWORD                  g_msq = 0;
    IDirect3DPixelShader9* g_ps = nullptr;
    IDirect3DStateBlock9*  g_sb = nullptr;
    bool                   g_failed = false;      // could not be made: give up until Reset
    bool                   g_valid  = false;      // this frame's mask was built

    bool Wanted()
    {
        const SunShadowSettings& ss = g_cfg.sunShadows;
        return g_cfg.shadow.enabled && ss.enabled && ss.units && !g_failed && VolumeLightActive();
    }

    void Release()
    {
        SafeRelease(g_msSurf);
        SafeRelease(g_texSurf);
        SafeRelease(g_tex);
        SafeRelease(g_ps);
        SafeRelease(g_sb);
        g_w = g_h = 0;
        g_valid = false;
    }

    bool Ensure(IDirect3DDevice9* dev, const D3DSURFACE_DESC& ds)
    {
        auto* d = dev->lpVtbl;
        if (!g_ps)
        {
            auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
            OgBlob* code = nullptr;
            OgBlob* errs = nullptr;
            if (compile && SUCCEEDED(compile(kOneHlsl, strlen(kOneHlsl), "bodymask", nullptr, nullptr, "main", "ps_2_0",
                                             0, 0, &code, &errs)) && code)
                d->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_ps);
            if (code) code->lpVtbl->Release(code);
            if (errs) errs->lpVtbl->Release(errs);
            if (!g_ps)
            {
                Log("bodymask: could not make the pixel shader; no body mask until Reset");
                g_failed = true;
                return false;
            }
        }
        if (!g_sb && (FAILED(d->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)) || !g_sb))
        {
            Log("bodymask: could not make a state block; no body mask until Reset");
            g_failed = true;
            return false;
        }
        if (g_tex && g_w == ds.Width && g_h == ds.Height && g_ms == ds.MultiSampleType && g_msq == ds.MultiSampleQuality)
            return true;
        SafeRelease(g_msSurf);
        SafeRelease(g_texSurf);
        SafeRelease(g_tex);
        D3DFORMAT fmt = D3DFMT_R5G6B5;
        HRESULT hr = d->CreateTexture(dev, ds.Width, ds.Height, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &g_tex,
                                      nullptr);
        if (FAILED(hr))
        {
            fmt = D3DFMT_A8R8G8B8;
            hr = d->CreateTexture(dev, ds.Width, ds.Height, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &g_tex,
                                  nullptr);
        }
        if (SUCCEEDED(hr))
            hr = g_tex->lpVtbl->GetSurfaceLevel(g_tex, 0, &g_texSurf);
        if (SUCCEEDED(hr) && ds.MultiSampleType != D3DMULTISAMPLE_NONE)
            hr = d->CreateRenderTarget(dev, ds.Width, ds.Height, fmt, ds.MultiSampleType, ds.MultiSampleQuality, FALSE,
                                       &g_msSurf, nullptr);
        if (FAILED(hr))
        {
            Log("bodymask: could not make a %ux%u mask (multisample %d, hr=0x%08X); no body mask until Reset",
                ds.Width, ds.Height, static_cast<int>(ds.MultiSampleType), hr);
            Release();
            g_failed = true;
            return false;
        }
        g_w = ds.Width; g_h = ds.Height; g_ms = ds.MultiSampleType; g_msq = ds.MultiSampleQuality;
        Log("bodymask: %ux%u mask ready (%s, multisample %d)", g_w, g_h, fmt == D3DFMT_R5G6B5 ? "R5G6B5" : "A8R8G8B8",
            static_cast<int>(g_ms));
        return true;
    }

    struct QuadVertex { float x, y, z, rhw; };
}

void BodyMarkDraw(IDirect3DDevice9* dev)
{
    auto* d = dev->lpVtbl;
    if (!g_started)
    {
        if (g_skipFrame || !Wanted())
            return;
        DWORD zw = 0;
        d->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
        if (!zw)
            return;   // the sky: the mark begins with the first depth-writing draw
        for (int i = 0; i < kStateCount; ++i)
            d->GetRenderState(dev, kStates[i], &g_saved[i]);
        if (g_saved[0])
        {
            if (g_clientStencilFrames++ == 0)
                Log("bodymask: the client has the stencil on in the world; no body mask in such a frame");
            g_skipFrame = true;
            return;
        }
        d->Clear(dev, 0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f, 0);
        d->SetRenderState(dev, D3DRS_STENCILENABLE,       TRUE);
        d->SetRenderState(dev, D3DRS_STENCILFUNC,         D3DCMP_ALWAYS);
        d->SetRenderState(dev, D3DRS_STENCILPASS,         D3DSTENCILOP_REPLACE);
        d->SetRenderState(dev, D3DRS_STENCILFAIL,         D3DSTENCILOP_KEEP);
        d->SetRenderState(dev, D3DRS_STENCILZFAIL,        D3DSTENCILOP_KEEP);
        d->SetRenderState(dev, D3DRS_TWOSIDEDSTENCILMODE, FALSE);
        d->SetRenderState(dev, D3DRS_STENCILMASK,         0xFFFFFFFF);
        d->SetRenderState(dev, D3DRS_STENCILREF,          0);
        d->SetRenderState(dev, D3DRS_STENCILWRITEMASK,    0);
        g_ref = 0;
        g_writeMask = 0;
        g_started = true;
    }
    // The client may have turned it off since (it never has been seen to): on again.
    DWORD on = 0;
    d->GetRenderState(dev, D3DRS_STENCILENABLE, &on);
    if (!on)
        d->SetRenderState(dev, D3DRS_STENCILENABLE, TRUE);
    DWORD zw = 0;
    d->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
    const DWORD writeMask = zw ? kBit : 0;
    const DWORD ref = zw && ShadowIsUnitDraw(dev) ? kBit : 0;
    if (ref)
        g_anyUnit = true;
    if (g_probeFrames > 0 && zw)
    {
        IDirect3DVertexShader9* vs = nullptr;
        d->GetVertexShader(dev, &vs);
        float p[3], pl[3];
        if (vs && ShadowDrawPosition(dev, p) && ClientPlayer(pl))
        {
            const float dx = p[0] - pl[0], dy = p[1] - pl[1], dz = p[2] - pl[2];
            const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < 6.0f)
            {
                ++(ref ? g_probeMarked : g_probeUnmarked);
                IDirect3DVertexBuffer9* vb = nullptr;
                UINT off = 0, stride = 0;
                d->GetStreamSource(dev, 0, &vb, &off, &stride);
                DWORD cw = 0, blend = 0;
                d->GetRenderState(dev, D3DRS_COLORWRITEENABLE, &cw);
                d->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
                if (g_probeLines++ < 80)
                    Log("bodymask: frame %d: model draw %s, root bone %.2f yd from you, vs %p vb %p offset %u, colour "
                        "writes 0x%X, blend %u", g_probeFrame, ref ? "MARKED" : "not marked", dist, vs, vb, off, cw, blend);
                if (vb) vb->lpVtbl->Release(vb);
            }
        }
        if (vs) vs->lpVtbl->Release(vs);
    }
    if (writeMask != g_writeMask)
    {
        d->SetRenderState(dev, D3DRS_STENCILWRITEMASK, writeMask);
        g_writeMask = writeMask;
    }
    if (ref != g_ref)
    {
        d->SetRenderState(dev, D3DRS_STENCILREF, ref);
        g_ref = ref;
    }
}

void BodyMaskProbe()
{
    g_probeFrames = 3;
    g_probeFrame = 0;
}

void BodyMarkWorldEnded(IDirect3DDevice9* dev)
{
    auto* d = dev->lpVtbl;
    g_valid = false;
    if (g_probeFrames > 0)
    {
        Log("bodymask: frame %d ended: %s; near you %u model draws marked, %u not", g_probeFrame,
            g_skipFrame ? "skipped (the client's stencil)" : g_started ? (g_anyUnit ? "mask built" : "no unit drawn, no mask")
                        : "the mark never began", g_probeMarked, g_probeUnmarked);
        --g_probeFrames;
        ++g_probeFrame;
        g_probeLines = 0;
        g_probeMarked = g_probeUnmarked = 0;
    }
    const bool started = g_started, any = g_anyUnit;
    g_started = false;
    g_skipFrame = false;
    g_anyUnit = false;
    if (!started)
        return;
    IDirect3DSurface9* ds = nullptr;
    IDirect3DSurface9* rt = nullptr;
    d->GetDepthStencilSurface(dev, &ds);
    d->GetRenderTarget(dev, 0, &rt);
    D3DSURFACE_DESC dd = {};
    if (any && ds && rt && SUCCEEDED(ds->lpVtbl->GetDesc(ds, &dd)) && Ensure(dev, dd))
    {
        g_sb->lpVtbl->Capture(g_sb);
        IDirect3DSurface9* target = g_msSurf ? g_msSurf : g_texSurf;
        d->SetRenderTarget(dev, 0, target);
        d->SetDepthStencilSurface(dev, ds);
        const D3DVIEWPORT9 vp = { 0, 0, g_w, g_h, 0.0f, 1.0f };
        d->SetViewport(dev, &vp);
        d->Clear(dev, 0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        d->SetRenderState(dev, D3DRS_ZENABLE,             D3DZB_FALSE);
        d->SetRenderState(dev, D3DRS_ZWRITEENABLE,        FALSE);
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,    FALSE);
        d->SetRenderState(dev, D3DRS_ALPHATESTENABLE,     FALSE);
        d->SetRenderState(dev, D3DRS_CULLMODE,            D3DCULL_NONE);
        d->SetRenderState(dev, D3DRS_FOGENABLE,           FALSE);
        d->SetRenderState(dev, D3DRS_SCISSORTESTENABLE,   FALSE);
        d->SetRenderState(dev, D3DRS_SRGBWRITEENABLE,     FALSE);
        d->SetRenderState(dev, D3DRS_COLORWRITEENABLE,    0xF);
        d->SetRenderState(dev, D3DRS_STENCILENABLE,       TRUE);
        d->SetRenderState(dev, D3DRS_STENCILFUNC,         D3DCMP_EQUAL);
        d->SetRenderState(dev, D3DRS_STENCILREF,          kBit);
        d->SetRenderState(dev, D3DRS_STENCILMASK,         kBit);
        d->SetRenderState(dev, D3DRS_STENCILWRITEMASK,    0);
        d->SetRenderState(dev, D3DRS_STENCILPASS,         D3DSTENCILOP_KEEP);
        d->SetRenderState(dev, D3DRS_STENCILFAIL,         D3DSTENCILOP_KEEP);
        d->SetRenderState(dev, D3DRS_STENCILZFAIL,        D3DSTENCILOP_KEEP);
        d->SetRenderState(dev, D3DRS_TWOSIDEDSTENCILMODE, FALSE);
        d->SetVertexShader(dev, nullptr);
        d->SetPixelShader(dev, g_ps);
        d->SetFVF(dev, D3DFVF_XYZRHW);
        const float w = static_cast<float>(g_w) - 0.5f, h = static_cast<float>(g_h) - 0.5f;
        const QuadVertex q[4] = { { -0.5f, -0.5f, 0, 1 }, { w, -0.5f, 0, 1 }, { -0.5f, h, 0, 1 }, { w, h, 0, 1 } };
        d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
        if (g_msSurf)
            d->StretchRect(dev, g_msSurf, nullptr, g_texSurf, nullptr, D3DTEXF_NONE);
        d->SetRenderTarget(dev, 0, rt);
        d->SetDepthStencilSurface(dev, ds);
        g_sb->lpVtbl->Apply(g_sb);
        g_valid = true;
    }
    SafeRelease(ds);
    SafeRelease(rt);
    for (int i = 0; i < kStateCount; ++i)
        d->SetRenderState(dev, kStates[i], g_saved[i]);
}

void BodyMarkFrameEnd(IDirect3DDevice9* dev)
{
    if (g_started)
        for (int i = 0; i < kStateCount; ++i)
            dev->lpVtbl->SetRenderState(dev, kStates[i], g_saved[i]);
    g_started = false;
    g_skipFrame = false;
    g_anyUnit = false;
}

IDirect3DTexture9* BodyMaskTexture()
{
    return g_valid ? g_tex : nullptr;
}

void BodyMaskReset()
{
    Release();
    g_failed = false;
    g_started = false;
    g_skipFrame = false;
    g_anyUnit = false;
}
