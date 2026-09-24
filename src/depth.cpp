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
// map). Multisampled surfaces are left alone: a texture cannot be multisampled, and the render target
// would no longer match. Every INTZ texture is released before Reset.
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
        UINT               w = 0, h = 0;       // its size, so a new surface reusing a freed address is caught
        IDirect3DTexture9* tex    = nullptr;
        IDirect3DSurface9* surf   = nullptr;
    };

    constexpr int kMaxSwaps = 8;
    Swap  g_swaps[kMaxSwaps];
    int   g_swapCount = 0;
    bool  g_checked   = false;
    bool  g_supported = false;
    bool  g_logNext   = false;
    IDirect3DTexture9* g_worldTex = nullptr;   // not referenced separately: owned by g_swaps

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
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
            if (g_swaps[i].surf == s)
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
            if (s->w == desc.Width && s->h == desc.Height)
                return s;
            // Same address, different surface: the old one was freed. Rebuild in place.
            SafeRelease(s->surf);
            SafeRelease(s->tex);
            *s = g_swaps[--g_swapCount];
            g_swaps[g_swapCount] = Swap();
        }
        if (g_swapCount >= kMaxSwaps || !CheckSupport(dev))
            return nullptr;
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
        {
            Log("depth: client depth surface %p is multisampled (%d): left alone, depth stays unreadable",
                client, static_cast<int>(desc.MultiSampleType));
            return nullptr;
        }

        Swap s;
        s.client = client;
        s.w = desc.Width; s.h = desc.Height;
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, desc.Width, desc.Height, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ,
                                                D3DPOOL_DEFAULT, &s.tex, nullptr);
        if (SUCCEEDED(hr))
            hr = s.tex->lpVtbl->GetSurfaceLevel(s.tex, 0, &s.surf);
        if (FAILED(hr))
        {
            Log("depth: could not create an INTZ %ux%u stand-in for %p (hr=0x%08X)", desc.Width, desc.Height, client, hr);
            SafeRelease(s.surf);
            SafeRelease(s.tex);
            return nullptr;
        }
        g_swaps[g_swapCount] = s;
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
    return s ? s->surf : clientSurface;
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
            if (g_swaps[i].surf == cur)
                dev->lpVtbl->SetDepthStencilSurface(dev, g_swaps[i].client);
        cur->lpVtbl->Release(cur);
        return;
    }
    if (!IsOurs(cur))
    {
        // Goes through our SetDepthStencilSurface hook, which passes our own surfaces straight on.
        if (Swap* s = ForClient(dev, cur))
            dev->lpVtbl->SetDepthStencilSurface(dev, s->surf);
    }
    cur->lpVtbl->Release(cur);
}

void DepthWorldEnded(IDirect3DDevice9* dev)
{
    g_worldTex = nullptr;
    IDirect3DSurface9* cur = nullptr;
    if (FAILED(dev->lpVtbl->GetDepthStencilSurface(dev, &cur)) || !cur)
    {
        if (g_logNext) { g_logNext = false; Log("depth: no depth surface bound when the world ended"); }
        return;
    }
    for (int i = 0; i < g_swapCount; ++i)
        if (g_swaps[i].surf == cur)
            g_worldTex = g_swaps[i].tex;
    if (g_logNext)
    {
        g_logNext = false;
        D3DSURFACE_DESC d = {};
        cur->lpVtbl->GetDesc(cur, &d);
        Log("depth: world ended with depth %p (%ux%u, format 0x%08X): %s", cur, d.Width, d.Height,
            static_cast<unsigned>(d.Format), g_worldTex ? "our INTZ, readable" : "NOT ours, unreadable");
    }
    cur->lpVtbl->Release(cur);
}

IDirect3DTexture9* DepthWorldTexture()
{
    return g_cfg.depth.enabled ? g_worldTex : nullptr;
}

void DepthReset(IDirect3DDevice9* dev)
{
    // Unbind first, so the device does not keep ours alive through the Reset.
    IDirect3DSurface9* cur = nullptr;
    if (SUCCEEDED(dev->lpVtbl->GetDepthStencilSurface(dev, &cur)) && cur)
    {
        if (IsOurs(cur))
            dev->lpVtbl->SetDepthStencilSurface(dev, nullptr);
        cur->lpVtbl->Release(cur);
    }
    for (int i = 0; i < g_swapCount; ++i)
    {
        SafeRelease(g_swaps[i].surf);
        SafeRelease(g_swaps[i].tex);
        g_swaps[i].client = nullptr;
    }
    g_swapCount = 0;
    g_worldTex  = nullptr;
}

void DepthProbe()
{
    g_logNext = true;
}
