// depth: a depth buffer the shaders can read.
//
// Volumetric light needs to know, for every pixel, how far the view ray travels through the fog before
// it hits something: the scene's depth. D3D9 depth buffers cannot be sampled, but the driver-level
// INTZ format is a depth-stencil (24-bit depth, 8-bit stencil) that is ALSO a texture, and DXVK supports
// it. So the client's depth buffer is swapped for an INTZ one of the same size: the client draws into it
// exactly as before, and afterwards the pass can read it.
//
// The swap: at BeginScene, and whenever the client binds a depth surface, a client D24S8-style surface is
// replaced by an INTZ texture's surface of the same size (made once per client surface, kept in a small
// map). Every INTZ texture is released before Reset.
//
// A multisampled client surface (anti-aliasing on, gxMultisample above 1) cannot become a texture: a
// texture cannot be multisampled, and the depth buffer must match the render target's sample count. It is
// swapped for a multisampled INTZ SURFACE instead, which the client draws into, and when the world ends
// StretchRect resolves that into a plain INTZ texture of the same size. DXVK takes a StretchRect between
// two surfaces of the same format as a Vulkan depth resolve. It refuses a depth StretchRect inside a scene,
// so the scene is ended for it and begun again. RESZ, the resolve drivers offer for this, was tried first:
// DXVK 2.7.1 turns it on only for an AMD GPU (d3d9_device.cpp, D3DRS_POINTSIZE).
//
// NOTES.md deferred this as the risky part of the work; it is behind [depth] enabled so it can be
// switched off with F11 if it ever misbehaves.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "common.h"
#include "config.h"
#include "depth.h"

namespace
{
    const D3DFORMAT kINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

    struct Swap
    {
        IDirect3DSurface9* client = nullptr;   // compared only; not referenced
        // Its size and sample count, so a new surface that reuses a freed address is caught.
        UINT               w = 0, h = 0;
        D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE;
        IDirect3DTexture9* tex    = nullptr;   // what the light reads
        IDirect3DSurface9* surf   = nullptr;   // tex's level 0
        IDirect3DSurface9* msaa   = nullptr;   // for a multisampled client surface: what the client draws into,
                                               // resolved into surf when the world ends. Null otherwise
        DWORD              used   = 0;         // GetTickCount when the client last bound it
        IDirect3DSurface9* Bound() const { return msaa ? msaa : surf; }
    };

    constexpr int kMaxSwaps = 8;
    Swap  g_swaps[kMaxSwaps];
    int   g_swapCount = 0;
    bool  g_checked   = false;
    bool  g_supported = false;
    bool  g_logNext   = false;
    IDirect3DTexture9* g_worldTex = nullptr;   // not referenced separately: owned by g_swaps
    bool  g_resolveFailed = false;              // StretchRect refused once: not tried again until Reset

    // A client surface that could not be swapped, so it is not tried (and logged) again at every
    // BeginScene and every bind.
    IDirect3DSurface9* g_refused = nullptr;
    UINT               g_refusedW = 0, g_refusedH = 0;

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    // Frees the stand-ins the client has not bound for 3 seconds. The client makes new depth surfaces when
    // the window changes size, often with no Reset, and the mod holds no reference to the client's own, so
    // it cannot see one freed. Measured on 2026-09-25: three size changes without a Reset, and each left its
    // stand-ins behind (at 4x and 2560x1440, some 75 MB of video memory each), until kMaxSwaps ran out and
    // new surfaces went without one. The client binds a live depth surface every frame. A stand-in still
    // bound when freed is safe: the device holds its own reference.
    void PruneSwaps()
    {
        static DWORD lastPrune = 0;
        const DWORD now = GetTickCount();
        if (now - lastPrune < 1000)
            return;
        lastPrune = now;
        for (int i = 0; i < g_swapCount;)
        {
            Swap& s = g_swaps[i];
            if (now - s.used > 3000)
            {
                Log("depth: freed the stand-in for client depth %p (%ux%u), not bound for 3 seconds", s.client, s.w, s.h);
                if (g_worldTex == s.tex)
                    g_worldTex = nullptr;
                SafeRelease(s.msaa);
                SafeRelease(s.surf);
                SafeRelease(s.tex);
                s = g_swaps[--g_swapCount];
                g_swaps[g_swapCount] = Swap();
            }
            else
            {
                ++i;
            }
        }
    }

    bool CheckSupport(IDirect3DDevice9* dev)
    {
        if (g_checked)
            return g_supported;
        g_checked = true;

        IDirect3D9* d3d = nullptr;
        D3DDISPLAYMODE mode = {};
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        if (FAILED(dev->lpVtbl->GetDirect3D(dev, &d3d)) || !d3d)
            return false;
        dev->lpVtbl->GetCreationParameters(dev, &cp);
        dev->lpVtbl->GetDisplayMode(dev, 0, &mode);
        const HRESULT hr = d3d->lpVtbl->CheckDeviceFormat(d3d, cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                          D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, kINTZ);
        d3d->lpVtbl->Release(d3d);
        g_supported = SUCCEEDED(hr);
        Log("depth: INTZ %s (hr=0x%08X)", g_supported ? "supported" : "NOT supported, depth stays unreadable", hr);
        return g_supported;
    }

    bool IsOurs(IDirect3DSurface9* s)
    {
        for (int i = 0; i < g_swapCount; ++i)
            if (g_swaps[i].Bound() == s)
                return true;
        return false;
    }

    Swap* Find(IDirect3DSurface9* client)
    {
        for (int i = 0; i < g_swapCount; ++i)
            if (g_swaps[i].client == client)
                return &g_swaps[i];
        return nullptr;
    }

    // The INTZ stand-in for a client surface, made on first sight. Null when it cannot or should not be.
    Swap* ForClient(IDirect3DDevice9* dev, IDirect3DSurface9* client)
    {
        if (!client || IsOurs(client))
            return nullptr;
        D3DSURFACE_DESC desc = {};
        if (FAILED(client->lpVtbl->GetDesc(client, &desc)))
            return nullptr;
        if (Swap* s = Find(client))
        {
            if (s->w == desc.Width && s->h == desc.Height && s->ms == desc.MultiSampleType)
            {
                s->used = GetTickCount();
                return s;
            }
            // Same address, different surface: the old one was freed. Rebuild in place.
            SafeRelease(s->msaa);
            SafeRelease(s->surf);
            SafeRelease(s->tex);
            *s = g_swaps[--g_swapCount];
            g_swaps[g_swapCount] = Swap();
        }
        if (client == g_refused && desc.Width == g_refusedW && desc.Height == g_refusedH)
            return nullptr;
        if (g_swapCount >= kMaxSwaps || !CheckSupport(dev))
            return nullptr;

        const bool multisampled = desc.MultiSampleType != D3DMULTISAMPLE_NONE;
        Swap s;
        s.client = client;
        s.w = desc.Width; s.h = desc.Height; s.ms = desc.MultiSampleType;
        s.used = GetTickCount();
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, desc.Width, desc.Height, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ,
                                                D3DPOOL_DEFAULT, &s.tex, nullptr);
        if (SUCCEEDED(hr))
            hr = s.tex->lpVtbl->GetSurfaceLevel(s.tex, 0, &s.surf);
        if (SUCCEEDED(hr) && multisampled)
            hr = dev->lpVtbl->CreateDepthStencilSurface(dev, desc.Width, desc.Height, kINTZ, desc.MultiSampleType,
                                                        desc.MultiSampleQuality, FALSE, &s.msaa, nullptr);
        if (FAILED(hr))
        {
            Log("depth: could not create an INTZ %ux%u stand-in for %p (multisample %d, hr=0x%08X): depth stays unreadable",
                desc.Width, desc.Height, client, static_cast<int>(desc.MultiSampleType), hr);
            SafeRelease(s.msaa);
            SafeRelease(s.surf);
            SafeRelease(s.tex);
            g_refused = client; g_refusedW = desc.Width; g_refusedH = desc.Height;
            return nullptr;
        }
        g_swaps[g_swapCount] = s;
        if (multisampled)
            Log("depth: client depth %p (%ux%u, format %d, multisample %d) now drawn into multisampled INTZ %p, "
                "resolved into INTZ texture %p", client, desc.Width, desc.Height, static_cast<int>(desc.Format),
                static_cast<int>(desc.MultiSampleType), s.msaa, s.tex);
        else
            Log("depth: client depth %p (%ux%u, format %d) now drawn into INTZ texture %p",
                client, desc.Width, desc.Height, static_cast<int>(desc.Format), s.tex);
        return &g_swaps[g_swapCount++];
    }
}

IDirect3DSurface9* DepthSubstitute(IDirect3DDevice9* dev, IDirect3DSurface9* clientSurface)
{
    if (!g_cfg.depth.enabled)
        return clientSurface;
    Swap* s = ForClient(dev, clientSurface);
    return s ? s->Bound() : clientSurface;
}

void DepthBeginScene(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* cur = nullptr;
    if (FAILED(dev->lpVtbl->GetDepthStencilSurface(dev, &cur)) || !cur)
        return;
    if (!g_cfg.depth.enabled)
    {
        // Switched off mid-session: hand the client its own surface back. It may never rebind it itself.
        for (int i = 0; i < g_swapCount; ++i)
            if (g_swaps[i].Bound() == cur)
                dev->lpVtbl->SetDepthStencilSurface(dev, g_swaps[i].client);
        cur->lpVtbl->Release(cur);
        return;
    }
    if (!IsOurs(cur))
    {
        // Goes through our SetDepthStencilSurface hook, which passes our own surfaces straight on.
        if (Swap* s = ForClient(dev, cur))
            dev->lpVtbl->SetDepthStencilSurface(dev, s->Bound());
    }
    else
    {
        for (int i = 0; i < g_swapCount; ++i)   // still bound from the last frame: the client did not rebind it
            if (g_swaps[i].Bound() == cur)
                g_swaps[i].used = GetTickCount();
    }
    cur->lpVtbl->Release(cur);
    PruneSwaps();
}

void DepthWorldEnded(IDirect3DDevice9* dev, bool resolve)
{
    g_worldTex = nullptr;
    IDirect3DSurface9* cur = nullptr;
    if (FAILED(dev->lpVtbl->GetDepthStencilSurface(dev, &cur)) || !cur)
    {
        if (g_logNext) { g_logNext = false; Log("depth: no depth surface bound when the world ended"); }
        return;
    }
    const Swap* s = nullptr;
    for (int i = 0; i < g_swapCount; ++i)
        if (g_swaps[i].Bound() == cur)
            s = &g_swaps[i];
    if (s && !s->msaa)
    {
        g_worldTex = s->tex;
    }
    else if (s && resolve && !g_resolveFailed)
    {
        auto* d = dev->lpVtbl;
        const bool inScene = SUCCEEDED(d->EndScene(dev));
        const HRESULT hr = d->StretchRect(dev, s->msaa, nullptr, s->surf, nullptr, D3DTEXF_NONE);
        if (inScene)
            d->BeginScene(dev);
        if (SUCCEEDED(hr))
        {
            g_worldTex = s->tex;
        }
        else
        {
            g_resolveFailed = true;
            Log("depth: resolving the multisampled depth failed (hr=0x%08X): depth stays unreadable", hr);
        }
    }
    if (g_logNext)
    {
        g_logNext = false;
        D3DSURFACE_DESC d = {};
        cur->lpVtbl->GetDesc(cur, &d);
        Log("depth: world ended with depth %p (%ux%u, format 0x%08X, multisample %d): %s", cur, d.Width, d.Height,
            static_cast<unsigned>(d.Format), static_cast<int>(d.MultiSampleType),
            !s ? "NOT ours, unreadable" : g_worldTex ? (s->msaa ? "ours, resolved, readable" : "our INTZ, readable") :
            "ours, not resolved this frame");
    }
    cur->lpVtbl->Release(cur);
}

IDirect3DTexture9* DepthWorldTexture()
{
    return g_cfg.depth.enabled ? g_worldTex : nullptr;
}

void DepthReset(IDirect3DDevice9* dev)
{
    // Unbind first, so the device does not keep ours alive through the Reset. Null for a device the client
    // has let go of (CheckDevice in comfyfog.cpp): nothing is called on it, ours are only released.
    IDirect3DSurface9* cur = nullptr;
    if (dev && SUCCEEDED(dev->lpVtbl->GetDepthStencilSurface(dev, &cur)) && cur)
    {
        if (IsOurs(cur))
            dev->lpVtbl->SetDepthStencilSurface(dev, nullptr);
        cur->lpVtbl->Release(cur);
    }
    for (int i = 0; i < g_swapCount; ++i)
    {
        SafeRelease(g_swaps[i].msaa);
        SafeRelease(g_swaps[i].surf);
        SafeRelease(g_swaps[i].tex);
        g_swaps[i].client = nullptr;
    }
    g_swapCount = 0;
    g_worldTex  = nullptr;
    g_resolveFailed = false;
    g_refused = nullptr;
    g_refusedW = g_refusedH = 0;
}

void DepthProbe()
{
    g_logNext = true;
}
