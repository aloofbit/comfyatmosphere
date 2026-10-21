// Shared between the parts of comfyatmos.dll.
#pragma once

#include <windows.h>

void   Log(const char* fmt, ...);
double Now();

// Minimal ID3DBlob. d3dcommon.h's definition is awkward under CINTERFACE, and only the two accessors are
// needed to get bytecode or disassembly text out.
struct OgBlob;
struct OgBlobVtbl
{
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(OgBlob*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(OgBlob*);
    ULONG   (STDMETHODCALLTYPE* Release)(OgBlob*);
    LPVOID  (STDMETHODCALLTYPE* GetBufferPointer)(OgBlob*);
    SIZE_T  (STDMETHODCALLTYPE* GetBufferSize)(OgBlob*);
};
struct OgBlob { const OgBlobVtbl* lpVtbl; };

// d3dcompiler_47 ships with Windows, so shaders are compiled from HLSL at run time: no D3DX, no
// hand-assembled bytecode. Null if the DLL or the export is missing.
using PFN_D3DCompile     = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR,
                                            LPCSTR, UINT, UINT, OgBlob**, OgBlob**);
using PFN_D3DDisassemble = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, OgBlob**);

FARPROC CompilerProc(const char* name);

// The FVF of the vertices bound for a draw (2026-10-21): GetFVF, or with a vertex declaration bound, the FVF it is
// the same as. DXVK 2.7 answered GetFVF so for a declaration; DXVK 3 and Windows' own D3D9 answer 0, and the water,
// the rain mist and the particles were not found by their FVF (on DXVK 3 the game's own water showed, not ours).
struct IDirect3DDevice9;
void DrawFvf(IDirect3DDevice9* dev, unsigned long* out);
