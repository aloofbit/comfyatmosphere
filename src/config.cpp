#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

Settings g_cfg;

namespace
{
    const wchar_t* kFog     = L"fog";
    const wchar_t* kSun     = L"sun";
    const wchar_t* kClient  = L"client";
    const wchar_t* kSky     = L"sky";
    const wchar_t* kDepth   = L"depth";
    const wchar_t* kShadow  = L"shadow";
    const wchar_t* kVolume  = L"volume";
    const wchar_t* kGeneral = L"general";

    float GetF(const wchar_t* sec, const wchar_t* key, float dflt, const wchar_t* ini)
    {
        wchar_t buf[64] = {};
        wchar_t def[64];
        swprintf(def, 64, L"%.6f", dflt);
        GetPrivateProfileStringW(sec, key, def, buf, 64, ini);
        return static_cast<float>(_wtof(buf));
    }

    int GetI(const wchar_t* sec, const wchar_t* key, int dflt, const wchar_t* ini)
    {
        return static_cast<int>(GetPrivateProfileIntW(sec, key, dflt, ini));
    }

    bool GetB(const wchar_t* sec, const wchar_t* key, bool dflt, const wchar_t* ini)
    {
        return GetPrivateProfileIntW(sec, key, dflt ? 1 : 0, ini) != 0;
    }

    // Reads a value that may be written in hex ("0x5A6470") or decimal.
    DWORD GetX(const wchar_t* sec, const wchar_t* key, DWORD dflt, const wchar_t* ini)
    {
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(sec, key, L"", buf, 64, ini);
        if (!buf[0])
            return dflt;
        return static_cast<DWORD>(wcstoul(buf, nullptr, 0));
    }

    float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

void ResolveIniPath(HMODULE self, wchar_t* out, size_t count)
{
    GetModuleFileNameW(self, out, static_cast<DWORD>(count));
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash)
        wcscpy_s(slash + 1, count - (slash + 1 - out), L"comfyfog.ini");
}

void LoadSettings(const wchar_t* ini)
{
    Settings s;

    s.fog.enabled    = GetB(kFog, L"enabled",    s.fog.enabled,    ini);
    s.fog.thickness  = Clamp(GetF(kFog, L"thickness",  s.fog.thickness,  ini), 0.0f, 100.0f);
    s.fog.haze       = Clamp(GetF(kFog, L"haze",       s.fog.haze,       ini), 0.0f, 0.9f);
    s.fog.reach      = Clamp(GetF(kFog, L"reach",      s.fog.reach,      ini), 0.05f, 2.0f);
    s.fog.desaturate = Clamp(GetF(kFog, L"desaturate", s.fog.desaturate, ini), 0.0f, 1.0f);
    s.fog.darken     = Clamp(GetF(kFog, L"darken",     s.fog.darken,     ini), 0.0f, 1.0f);
    s.fog.tint       = GetX(kFog, L"tint",       s.fog.tint,       ini) & 0xFFFFFF;
    s.fog.tintAmount = Clamp(GetF(kFog, L"tintAmount", s.fog.tintAmount, ini), 0.0f, 1.0f);
    s.fog.shaderReg  = GetI(kFog, L"shaderReg",  s.fog.shaderReg,  ini);

    s.sun.fixed        = GetB(kSun, L"fixed",     s.sun.fixed,     ini);
    s.sun.azimuth      = GetF(kSun, L"azimuth",   s.sun.azimuth,   ini);
    s.sun.elevation    = GetF(kSun, L"elevation", s.sun.elevation, ini);

    s.client.camAddr      = GetX(kClient, L"camAddr",      s.client.camAddr,      ini);
    s.client.objMgrAddr   = GetX(kClient, L"objMgrAddr",   s.client.objMgrAddr,   ini);
    s.client.playerPosOff = GetX(kClient, L"playerPosOff", s.client.playerPosOff, ini);

    s.sky.clouds        = GetB(kSky, L"clouds", s.sky.clouds, ini);
    s.depth.enabled     = GetB(kDepth, L"enabled", s.depth.enabled, ini);
    s.shadow.enabled    = GetB(kShadow, L"enabled", s.shadow.enabled, ini);
    s.shadow.size       = GetI(kShadow, L"size", s.shadow.size, ini);
    s.shadow.range      = Clamp(GetF(kShadow, L"range", s.shadow.range, ini), 5.0f, 1000.0f);
    s.shadow.depth      = Clamp(GetF(kShadow, L"depth", s.shadow.depth, ini), 10.0f, 5000.0f);
    s.shadow.horizon    = GetB(kShadow, L"horizon", s.shadow.horizon, ini);
    s.shadow.copyPerFrame = GetI(kShadow, L"copyPerFrame", s.shadow.copyPerFrame, ini);
    s.shadow.copyMax      = GetI(kShadow, L"copyMax", s.shadow.copyMax, ini);
    if (s.shadow.copyPerFrame < 0)  s.shadow.copyPerFrame = 0;
    if (s.shadow.copyPerFrame > 16) s.shadow.copyPerFrame = 16;
    if (s.shadow.copyMax < 0)       s.shadow.copyMax = 0;
    if (s.shadow.copyMax > 4096)    s.shadow.copyMax = 4096;
    s.shadow.mapEvery   = GetI(kShadow, L"mapEvery", s.shadow.mapEvery, ini);
    if (s.shadow.mapEvery < 1) s.shadow.mapEvery = 1;
    if (s.shadow.mapEvery > 8) s.shadow.mapEvery = 8;
    s.shadow.snap       = GetB(kShadow, L"snap", s.shadow.snap, ini);
    s.shadow.cacheTime  = Clamp(GetF(kShadow, L"cacheTime", s.shadow.cacheTime, ini), 0.0f, 600.0f);
    s.shadow.evictDistance = Clamp(GetF(kShadow, L"evictDistance", s.shadow.evictDistance, ini), 1.0f, 500.0f);
    s.volume.enabled      = GetB(kVolume, L"enabled", s.volume.enabled, ini);
    s.volume.strength     = Clamp(GetF(kVolume, L"strength",     s.volume.strength,     ini), 0.0f, 100.0f);
    s.volume.maxIntensity = Clamp(GetF(kVolume, L"maxIntensity", s.volume.maxIntensity, ini), 0.0f, 20.0f);
    s.volume.density      = Clamp(GetF(kVolume, L"density",      s.volume.density,      ini), 0.0f, 1.0f);
    s.volume.smooth       = Clamp(GetF(kVolume, L"smooth", s.volume.smooth, ini), 0.0f, 0.95f);
    s.volume.steps        = GetI(kVolume, L"steps", s.volume.steps, ini);
    if (s.volume.steps < 8)   s.volume.steps = 8;
    if (s.volume.steps > 128) s.volume.steps = 128;
    s.volume.maxDistance  = Clamp(GetF(kVolume, L"maxDistance",  s.volume.maxDistance,  ini), 1.0f, 1000.0f);
    s.volume.anisotropy   = Clamp(GetF(kVolume, L"anisotropy",   s.volume.anisotropy,   ini), 0.0f, 0.95f);
    s.volume.bias         = Clamp(GetF(kVolume, L"bias",         s.volume.bias,         ini), 0.0f, 20.0f);
    s.volume.color        = GetX(kVolume, L"color", s.volume.color, ini) & 0xFFFFFF;
    s.volume.downscale    = GetI(kVolume, L"downscale", s.volume.downscale, ini);
    s.volume.blur         = GetB(kVolume, L"blur",  s.volume.blur,  ini);
    s.volume.debug        = GetI(kVolume, L"debug", s.volume.debug, ini);
    if (s.volume.downscale < 1) s.volume.downscale = 1;
    if (s.volume.downscale > 8) s.volume.downscale = 8;
    if (s.shadow.size < 256)  s.shadow.size = 256;
    if (s.shadow.size > 4096) s.shadow.size = 4096;


    s.trace       = GetB(kGeneral, L"trace", s.trace, ini);
    s.logEnabled  = GetB(kGeneral, L"log",         s.logEnabled,  ini);
    s.hook        = GetB(kGeneral, L"hook",        s.hook,        ini);
    s.sliders     = GetB(kGeneral, L"sliders",     s.sliders,     ini);
    s.reloadKey   = GetI(kGeneral, L"reloadKey",   s.reloadKey,   ini);
    s.probeKey    = GetI(kGeneral, L"probeKey",    s.probeKey,    ini);
    s.chainWaitMs = GetI(kGeneral, L"chainWaitMs", s.chainWaitMs, ini);
    s.minWorldDraws = GetI(kGeneral, L"minWorldDraws", s.minWorldDraws, ini);

    g_cfg = s;
}
