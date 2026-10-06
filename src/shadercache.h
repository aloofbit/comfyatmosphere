#pragma once
// The shaders' cache (2026-10-06): every compile goes through it. A compiled shader is kept for the session and on
// disk (comfyatmos-cache\ beside comfyatmos.log), keyed by its source, name, profile and defines, and a worker compiles
// the passes' shaders from the DLL's start, so the world's first frames find them ready. NOTES, "Shaders at sign-in".
#include <windows.h>
#include "common.h"

// Starts the worker, which compiles or loads every shader the passes list. dir: the folder for the cache.
void ShaderCacheStart(const wchar_t* dir);

// D3DCompile through the cache: the same arguments and result. real is d3dcompiler's own.
HRESULT ShaderCacheCompile(PFN_D3DCompile real, LPCVOID src, SIZE_T size, LPCSTR name, const void* defines,
                           void* include, LPCSTR entry, LPCSTR target, UINT f1, UINT f2, OgBlob** code, OgBlob** errs);

// For a pass's list: one shader as the pass compiles it (entry "main", flags 0); defines as D3DCompile takes them.
void ShaderPrecompile(const char* name, const char* src, const char* profile, const char* const* defines = nullptr);

// Each pass lists its shaders with these (defined in its own file).
void BeaconShaderList();
void BodyMaskShaderList();
void CoverShaderList();
void GradeShaderList();
void GrassShaderList();
void LampGlowShaderList();
void RaysShaderList();
void ShadowShaderList();
void SunShadowsShaderList();
void VolumeShaderList();
void WaterShaderList();
