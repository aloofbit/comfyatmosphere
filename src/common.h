// Shared between the fog and the rays halves of comfyfog.dll.
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
