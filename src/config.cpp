#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

Settings g_cfg;

namespace
{
    const wchar_t* kFog     = L"fog";
    const wchar_t* kRays    = L"rays";
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

    s.rays.enabled     = GetB(kRays, L"enabled",     s.rays.enabled,     ini);
    s.rays.strength    = Clamp(GetF(kRays, L"strength",    s.rays.strength,    ini), 0.0f, 100.0f);
    s.rays.maxExposure = Clamp(GetF(kRays, L"maxExposure", s.rays.maxExposure, ini), 0.0f, 20.0f);
    s.rays.threshold   = Clamp(GetF(kRays, L"threshold",   s.rays.threshold,   ini), 0.0f, 0.99f);
    s.rays.relThreshold = Clamp(GetF(kRays, L"relThreshold", s.rays.relThreshold, ini), 0.0f, 0.99f);
    s.rays.radius      = Clamp(GetF(kRays, L"radius",      s.rays.radius,      ini), 0.05f, 10.0f);
    s.rays.falloff     = Clamp(GetF(kRays, L"falloff",     s.rays.falloff,     ini), 0.25f, 8.0f);
    s.rays.length      = Clamp(GetF(kRays, L"length",      s.rays.length,      ini), 0.0f, 1.0f);
    s.rays.maxLength   = Clamp(GetF(kRays, L"maxLength",   s.rays.maxLength,   ini), 0.05f, 3.0f);
    s.rays.maxAngle    = Clamp(GetF(kRays, L"maxAngle",    s.rays.maxAngle,    ini), 45.0f, 180.0f);
    s.rays.viewFalloff = Clamp(GetF(kRays, L"viewFalloff", s.rays.viewFalloff, ini), 0.1f, 8.0f);
    s.rays.parallel    = Clamp(GetF(kRays, L"parallel",    s.rays.parallel,    ini), 0.0f, 1.0f);
    s.rays.adaptTime   = Clamp(GetF(kRays, L"adaptTime",   s.rays.adaptTime,   ini), 0.0f, 10.0f);
    s.rays.decay       = Clamp(GetF(kRays, L"decay",       s.rays.decay,       ini), 0.5f, 1.0f);
    s.rays.color       = GetX(kRays, L"color", s.rays.color, ini) & 0xFFFFFF;
    s.rays.passes      = GetI(kRays, L"passes",      s.rays.passes,      ini);
    s.rays.downscale   = GetI(kRays, L"downscale",   s.rays.downscale,   ini);
    s.rays.sunMode     = GetI(kRays, L"sunMode",     s.rays.sunMode,     ini);
    s.rays.sunX        = GetF(kRays, L"sunX",        s.rays.sunX,        ini);
    s.rays.sunY        = GetF(kRays, L"sunY",        s.rays.sunY,        ini);
    s.rays.azimuth     = GetF(kRays, L"azimuth",     s.rays.azimuth,     ini);
    s.rays.elevation   = GetF(kRays, L"elevation",   s.rays.elevation,   ini);
    s.rays.debugView   = GetI(kRays, L"debugView",   s.rays.debugView,   ini);
    s.rays.placement     = GetI(kRays, L"placement",     s.rays.placement,     ini);
    s.rays.minWorldDraws = GetI(kRays, L"minWorldDraws", s.rays.minWorldDraws, ini);
    if (s.rays.passes < 1)    s.rays.passes = 1;
    if (s.rays.passes > 3)    s.rays.passes = 3;
    if (s.rays.downscale < 1) s.rays.downscale = 1;
    if (s.rays.downscale > 8) s.rays.downscale = 8;

    s.client.camAddr      = GetX(kClient, L"camAddr",      s.client.camAddr,      ini);
    s.client.objMgrAddr   = GetX(kClient, L"objMgrAddr",   s.client.objMgrAddr,   ini);
    s.client.playerPosOff = GetX(kClient, L"playerPosOff", s.client.playerPosOff, ini);

    s.sky.clouds        = GetB(kSky, L"clouds", s.sky.clouds, ini);
    s.depth.enabled     = GetB(kDepth, L"enabled", s.depth.enabled, ini);
    s.depth.viewRange   = Clamp(GetF(kDepth, L"viewRange", s.depth.viewRange, ini), 1.0f, 5000.0f);
    s.shadow.enabled    = GetB(kShadow, L"enabled", s.shadow.enabled, ini);
    s.shadow.size       = GetI(kShadow, L"size", s.shadow.size, ini);
    s.shadow.range      = Clamp(GetF(kShadow, L"range", s.shadow.range, ini), 5.0f, 1000.0f);
    s.shadow.depth      = Clamp(GetF(kShadow, L"depth", s.shadow.depth, ini), 10.0f, 5000.0f);
    s.shadow.cacheTime  = Clamp(GetF(kShadow, L"cacheTime", s.shadow.cacheTime, ini), 0.0f, 600.0f);
    s.shadow.evictDistance = Clamp(GetF(kShadow, L"evictDistance", s.shadow.evictDistance, ini), 1.0f, 500.0f);
    s.volume.enabled      = GetB(kVolume, L"enabled", s.volume.enabled, ini);
    s.volume.strength     = Clamp(GetF(kVolume, L"strength",     s.volume.strength,     ini), 0.0f, 100.0f);
    s.volume.maxIntensity = Clamp(GetF(kVolume, L"maxIntensity", s.volume.maxIntensity, ini), 0.0f, 20.0f);
    s.volume.density      = Clamp(GetF(kVolume, L"density",      s.volume.density,      ini), 0.0f, 1.0f);
    s.volume.maxDistance  = Clamp(GetF(kVolume, L"maxDistance",  s.volume.maxDistance,  ini), 1.0f, 1000.0f);
    s.volume.anisotropy   = Clamp(GetF(kVolume, L"anisotropy",   s.volume.anisotropy,   ini), 0.0f, 0.95f);
    s.volume.bias         = Clamp(GetF(kVolume, L"bias",         s.volume.bias,         ini), 0.0f, 20.0f);
    s.volume.downscale    = GetI(kVolume, L"downscale", s.volume.downscale, ini);
    s.volume.blur         = GetB(kVolume, L"blur",  s.volume.blur,  ini);
    s.volume.debug        = GetI(kVolume, L"debug", s.volume.debug, ini);
    if (s.volume.downscale < 1) s.volume.downscale = 1;
    if (s.volume.downscale > 8) s.volume.downscale = 8;
    if (s.shadow.size < 256)  s.shadow.size = 256;
    if (s.shadow.size > 4096) s.shadow.size = 4096;


    s.logEnabled  = GetB(kGeneral, L"log",         s.logEnabled,  ini);
    s.hook        = GetB(kGeneral, L"hook",        s.hook,        ini);
    s.reloadKey   = GetI(kGeneral, L"reloadKey",   s.reloadKey,   ini);
    s.probeKey    = GetI(kGeneral, L"probeKey",    s.probeKey,    ini);
    s.chainWaitMs = GetI(kGeneral, L"chainWaitMs", s.chainWaitMs, ini);

    g_cfg = s;
}
