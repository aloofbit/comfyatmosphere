#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

Settings g_cfg;

namespace
{
    std::vector<ConfigKey>             g_keys;
    std::map<std::string, std::string> g_overrides;
    wchar_t                            g_ini[MAX_PATH] = {};

    std::string Narrow(const wchar_t* w)
    {
        std::string s;
        for (; *w; ++w)
            s += static_cast<char>(*w < 128 ? *w : '?');
        return s;
    }

    // The raw text for a key: from /atmos, else the ini, else none (the code's default is used).
    ConfigSource ReadRaw(const wchar_t* sec, const wchar_t* key, const wchar_t* ini, wchar_t* buf, int cap)
    {
        const auto it = g_overrides.find(Narrow(sec) + "." + Narrow(key));
        if (it != g_overrides.end())
        {
            MultiByteToWideChar(CP_ACP, 0, it->second.c_str(), -1, buf, cap);
            return kFromTune;
        }
        buf[0] = 0;
        GetPrivateProfileStringW(sec, key, L"", buf, cap, ini);
        return buf[0] ? kFromIni : kFromDefault;
    }

    void Note(const wchar_t* sec, const wchar_t* key, const char* value, ConfigSource source)
    {
        g_keys.push_back({ Narrow(sec), Narrow(key), value, source });
    }
}

const std::vector<ConfigKey>& ConfigKeys() { return g_keys; }
const wchar_t* ConfigIniPath() { return g_ini; }
std::map<std::string, std::string>& ConfigOverrides() { return g_overrides; }

namespace
{
    const wchar_t* kFog     = L"fog";
    const wchar_t* kSun     = L"sun";
    const wchar_t* kClient  = L"client";
    const wchar_t* kSky     = L"sky";
    const wchar_t* kWater   = L"water";
    const wchar_t* kDepth   = L"depth";
    const wchar_t* kShadow  = L"shadow";
    const wchar_t* kVolume  = L"volume";
    const wchar_t* kLamps   = L"lamps";
    const wchar_t* kSunShadows = L"sunshadows";
    const wchar_t* kRays    = L"rays";
    const wchar_t* kNight   = L"night";
    const wchar_t* kBench   = L"bench";
    const wchar_t* kGeneral = L"general";

    // Each reader notes the key and the value it used (ConfigKeys). The ini's text runs on to any comment,
    // so a number is read from its start.
    float GetF(const wchar_t* sec, const wchar_t* key, float dflt, const wchar_t* ini)
    {
        wchar_t buf[64];
        const ConfigSource src = ReadRaw(sec, key, ini, buf, 64);
        const float v = src != kFromDefault ? static_cast<float>(_wtof(buf)) : dflt;
        char text[32];
        snprintf(text, sizeof(text), "%g", v);
        Note(sec, key, text, src);
        return v;
    }

    int GetI(const wchar_t* sec, const wchar_t* key, int dflt, const wchar_t* ini)
    {
        wchar_t buf[64];
        const ConfigSource src = ReadRaw(sec, key, ini, buf, 64);
        const int v = src != kFromDefault ? _wtoi(buf) : dflt;
        char text[32];
        snprintf(text, sizeof(text), "%d", v);
        Note(sec, key, text, src);
        return v;
    }

    bool GetB(const wchar_t* sec, const wchar_t* key, bool dflt, const wchar_t* ini)
    {
        wchar_t buf[64];
        const ConfigSource src = ReadRaw(sec, key, ini, buf, 64);
        const bool v = src != kFromDefault ? _wtoi(buf) != 0 : dflt;
        Note(sec, key, v ? "1" : "0", src);
        return v;
    }

    // Reads a value that may be written in hex ("0x5A6470") or decimal.
    DWORD GetX(const wchar_t* sec, const wchar_t* key, DWORD dflt, const wchar_t* ini)
    {
        wchar_t buf[64];
        const ConfigSource src = ReadRaw(sec, key, ini, buf, 64);
        const DWORD v = src != kFromDefault ? static_cast<DWORD>(wcstoul(buf, nullptr, 0)) : dflt;
        char text[32];
        snprintf(text, sizeof(text), "0x%06lX", static_cast<unsigned long>(v));
        Note(sec, key, text, src);
        return v;
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

void ApplyVolumeQuality(Settings& s)
{
    //                           low  medium
    static const int steps[2]    = {   32,   48 };
    static const int down[2]     = {    3,    2 };
    if (s.volume.quality >= 3)
        return;
    const int q = s.volume.quality <= 1 ? 0 : 1;
    s.volume.steps     = steps[q];
    s.volume.downscale = down[q];
}

void LoadSettings(const wchar_t* ini)
{
    Settings s;
    g_keys.clear();
    if (ini != g_ini)
        wcsncpy_s(g_ini, ini, _TRUNCATE);

    s.fog.enabled      = GetB(kFog, L"enabled", s.fog.enabled, ini);
    s.fog.density      = Clamp(GetF(kFog, L"density",      s.fog.density,      ini), 0.0f, 0.1f);
    s.fog.height       = Clamp(GetF(kFog, L"height",       s.fog.height,       ini), 1.0f, 2000.0f);
    s.fog.groundRadius = Clamp(GetF(kFog, L"groundRadius", s.fog.groundRadius, ini), 20.0f, 500.0f);
    s.fog.reach        = Clamp(GetF(kFog, L"reach",        s.fog.reach,        ini), 20.0f, 5000.0f);
    s.fog.skyDistance  = Clamp(GetF(kFog, L"skyDistance",  s.fog.skyDistance,  ini), 0.0f, 5000.0f);
    s.fog.brightness   = Clamp(GetF(kFog, L"brightness",   s.fog.brightness,   ini), 0.0f, 4.0f);
    s.fog.sunLight     = Clamp(GetF(kFog, L"sunLight",     s.fog.sunLight,     ini), 0.0f, 50.0f);
    s.fog.patchiness   = Clamp(GetF(kFog, L"patchiness",   s.fog.patchiness,   ini), 0.0f, 1.0f);
    s.fog.scale        = Clamp(GetF(kFog, L"scale",        s.fog.scale,        ini), 5.0f, 1000.0f);
    s.fog.flatten      = Clamp(GetF(kFog, L"flatten",      s.fog.flatten,      ini), 0.25f, 8.0f);
    s.fog.windDeg      = GetF(kFog, L"windDeg", s.fog.windDeg, ini);
    s.fog.windSpeed    = Clamp(GetF(kFog, L"windSpeed",    s.fog.windSpeed,    ini), 0.0f, 50.0f);
    s.fog.follow       = Clamp(GetF(kFog, L"follow",       s.fog.follow,       ini), 0.0f, 1.0f);
    s.fog.smoothRadius = Clamp(GetF(kFog, L"smoothRadius", s.fog.smoothRadius, ini), 8.0f, 400.0f);
    s.fog.lowGround    = Clamp(GetF(kFog, L"lowGround",    s.fog.lowGround,    ini), 0.0f, 10.0f);
    s.fog.lowDepth     = Clamp(GetF(kFog, L"lowDepth",     s.fog.lowDepth,     ini), 1.0f, 500.0f);
    s.fog.water        = Clamp(GetF(kFog, L"water",        s.fog.water,        ini), 0.0f, 10.0f);
    s.fog.morning      = Clamp(GetF(kFog, L"morning",      s.fog.morning,      ini), 0.0f, 10.0f);
    s.fog.lampMist     = Clamp(GetF(kFog, L"lampMist",     s.fog.lampMist,     ini), 0.0f, 10.0f);
    s.fog.debug        = GetI(kFog, L"debug", s.fog.debug, ini);

    s.sun.fixed        = GetB(kSun, L"fixed",     s.sun.fixed,     ini);
    s.sun.azimuth      = GetF(kSun, L"azimuth",   s.sun.azimuth,   ini);
    s.sun.elevation    = GetF(kSun, L"elevation", s.sun.elevation, ini);
    s.sun.glide        = Clamp(GetF(kSun, L"glide", s.sun.glide, ini), 0.0f, 60.0f);

    s.client.camAddr      = GetX(kClient, L"camAddr",      s.client.camAddr,      ini);
    s.client.objMgrAddr   = GetX(kClient, L"objMgrAddr",   s.client.objMgrAddr,   ini);
    s.client.playerPosOff = GetX(kClient, L"playerPosOff", s.client.playerPosOff, ini);
    s.client.clockAddr    = GetX(kClient, L"clockAddr",    s.client.clockAddr,    ini);
    s.client.mapNameAddr  = GetX(kClient, L"mapNameAddr",  s.client.mapNameAddr,  ini);

    s.sky.clouds        = GetB(kSky, L"clouds", s.sky.clouds, ini);
    s.water.enabled     = GetB(kWater, L"enabled", s.water.enabled, ini);
    s.water.foam        = Clamp(GetF(kWater, L"foam",      s.water.foam,      ini), 0.0f, 1.0f);
    s.water.foamWidth   = Clamp(GetF(kWater, L"foamWidth", s.water.foamWidth, ini), 0.1f, 10.0f);
    s.water.foamReach   = Clamp(GetF(kWater, L"foamReach", s.water.foamReach, ini), 0.5f, 50.0f);
    s.water.ripples     = Clamp(GetF(kWater, L"ripples",   s.water.ripples,   ini), 0.0f, 1.0f);
    s.water.rippleDepth = Clamp(GetF(kWater, L"rippleDepth", s.water.rippleDepth, ini), 0.0f, 4.0f);
    s.water.wetSand     = Clamp(GetF(kWater, L"wetSand",   s.water.wetSand,   ini), 0.0f, 1.0f);
    s.water.surface     = Clamp(GetF(kWater, L"surface",   s.water.surface,   ini), 0.0f, 1.0f);
    s.water.clarity     = Clamp(GetF(kWater, L"clarity",   s.water.clarity,   ini), 0.1f, 10.0f);
    s.water.colour      = Clamp(GetF(kWater, L"colour",    s.water.colour,    ini), 0.0f, 100.0f);
    s.water.reflection  = Clamp(GetF(kWater, L"reflection", s.water.reflection, ini), 0.0f, 1.0f);
    s.water.skyColor    = GetX(kWater, L"skyColor",  s.water.skyColor,  ini) & 0xFFFFFF;
    s.water.brightness  = Clamp(GetF(kWater, L"brightness", s.water.brightness, ini), 0.25f, 2.0f);
    s.water.glint       = Clamp(GetF(kWater, L"glint",     s.water.glint,     ini), 0.0f, 3.0f);
    s.water.moonGlint   = Clamp(GetF(kWater, L"moonGlint", s.water.moonGlint, ini), 0.0f, 3.0f);
    s.water.glintSize   = Clamp(GetF(kWater, L"glintSize", s.water.glintSize, ini), 0.25f, 4.0f);
    s.water.waves       = Clamp(GetF(kWater, L"waves",     s.water.waves,     ini), 0.0f, 3.0f);
    s.water.waveHeight  = Clamp(GetF(kWater, L"waveHeight", s.water.waveHeight, ini), 0.0f, 3.0f);
    s.water.waveScale   = Clamp(GetF(kWater, L"waveScale", s.water.waveScale, ini), 0.25f, 4.0f);
    s.water.whitecaps   = Clamp(GetF(kWater, L"whitecaps", s.water.whitecaps, ini), 0.0f, 1.0f);
    s.water.swash       = Clamp(GetF(kWater, L"swash",     s.water.swash,     ini), 0.0f, 1.0f);
    s.water.swashRun    = Clamp(GetF(kWater, L"swashRun",  s.water.swashRun,  ini), 0.0f, 5.0f);
    s.water.swashHeight = Clamp(GetF(kWater, L"swashHeight", s.water.swashHeight, ini), 0.0f, 1.0f);
    s.water.wake        = Clamp(GetF(kWater, L"wake",      s.water.wake,      ini), 0.0f, 1.0f);
    s.water.cover       = Clamp(GetF(kWater, L"cover",     s.water.cover,     ini), 0.0f, 1.0f);
    s.water.refraction  = Clamp(GetF(kWater, L"refraction", s.water.refraction, ini), 0.0f, 2.0f);
    s.water.gameWake    = GetB(kWater, L"gameWake", s.water.gameWake, ini);
    s.water.edgeLine    = Clamp(GetF(kWater, L"edgeLine", s.water.edgeLine, ini), 0.0f, 1.0f);
    s.water.edgeWidth   = Clamp(GetF(kWater, L"edgeWidth", s.water.edgeWidth, ini), 0.05f, 3.0f);
    s.water.shoreFoam   = Clamp(GetF(kWater, L"shoreFoam", s.water.shoreFoam, ini), 0.0f, 1.0f);
    s.water.shoreFoamSize = Clamp(GetF(kWater, L"shoreFoamSize", s.water.shoreFoamSize, ini), 0.5f, 50.0f);
    s.water.foamScale   = Clamp(GetF(kWater, L"foamScale", s.water.foamScale, ini), 0.05f, 10.0f);
    s.water.foamSpeed   = Clamp(GetF(kWater, L"foamSpeed", s.water.foamSpeed, ini), 0.0f, 10.0f);
    s.water.foamColor   = GetX(kWater, L"foamColor", s.water.foamColor, ini) & 0xFFFFFF;
    s.water.fadeEnd     = Clamp(GetF(kWater, L"fadeEnd",   s.water.fadeEnd,   ini), 10.0f, 2000.0f);
    s.water.debug       = GetI(kWater, L"debug", s.water.debug, ini);
    s.depth.enabled     = GetB(kDepth, L"enabled", s.depth.enabled, ini);
    s.depth.seeThrough  = GetB(kDepth, L"seeThrough", s.depth.seeThrough, ini);
    s.depth.seeThroughNear = Clamp(GetF(kDepth, L"seeThroughNear", s.depth.seeThroughNear, ini), 0.0f, 50.0f);
    s.depth.seeThroughNearMounted = Clamp(GetF(kDepth, L"seeThroughNearMounted", s.depth.seeThroughNearMounted, ini),
                                          0.0f, 50.0f);
    s.depth.waterDepth  = GetB(kDepth, L"waterDepth", s.depth.waterDepth, ini);
    s.shadow.enabled    = GetB(kShadow, L"enabled", s.shadow.enabled, ini);
    s.shadow.size       = GetI(kShadow, L"size", s.shadow.size, ini);
    s.shadow.range      = Clamp(GetF(kShadow, L"range", s.shadow.range, ini), 5.0f, 1000.0f);
    s.shadow.nearRange  = Clamp(GetF(kShadow, L"nearRange", s.shadow.nearRange, ini), 0.0f, 200.0f);
    s.shadow.midRange   = Clamp(GetF(kShadow, L"midRange", s.shadow.midRange, ini), 0.0f, 250.0f);
    s.shadow.depth      = Clamp(GetF(kShadow, L"depth", s.shadow.depth, ini), 10.0f, 5000.0f);
    s.shadow.horizonDepth = Clamp(GetF(kShadow, L"horizonDepth", s.shadow.horizonDepth, ini), 0.0f, 5000.0f);
    s.shadow.horizon    = GetB(kShadow, L"horizon", s.shadow.horizon, ini);
    s.shadow.copyPerFrame = GetI(kShadow, L"copyPerFrame", s.shadow.copyPerFrame, ini);
    s.shadow.copyMax      = GetI(kShadow, L"copyMax", s.shadow.copyMax, ini);
    if (s.shadow.copyPerFrame < 0)  s.shadow.copyPerFrame = 0;
    if (s.shadow.copyPerFrame > 16) s.shadow.copyPerFrame = 16;
    if (s.shadow.copyMax < 0)       s.shadow.copyMax = 0;
    if (s.shadow.copyMax > 4096)    s.shadow.copyMax = 4096;
    s.shadow.mapEvery   = GetI(kShadow, L"mapEvery", s.shadow.mapEvery, ini);
    s.shadow.farEvery   = GetI(kShadow, L"farEvery", s.shadow.farEvery, ini);
    s.shadow.minTriangles = GetI(kShadow, L"minTriangles", s.shadow.minTriangles, ini);
    s.shadow.nearMargin = Clamp(GetF(kShadow, L"nearMargin", s.shadow.nearMargin, ini), 0.0f, 200.0f);
    s.shadow.leaves     = GetB(kShadow, L"leaves", s.shadow.leaves, ini);
    s.shadow.terrainLeaves = GetB(kShadow, L"terrainLeaves", s.shadow.terrainLeaves, ini);
    s.shadow.mapTerrain    = GetB(kShadow, L"mapTerrain", s.shadow.mapTerrain, ini);
    s.shadow.leafAlpha     = static_cast<int>(Clamp(GetF(kShadow, L"leafAlpha", static_cast<float>(s.shadow.leafAlpha), ini), 1.0f, 255.0f));
    if (s.shadow.minTriangles < 0) s.shadow.minTriangles = 0;
    if (s.shadow.farEvery < 1) s.shadow.farEvery = 1;
    if (s.shadow.farEvery > 8) s.shadow.farEvery = 8;
    if (s.shadow.mapEvery < 1) s.shadow.mapEvery = 1;
    if (s.shadow.mapEvery > 8) s.shadow.mapEvery = 8;
    s.shadow.snap       = GetB(kShadow, L"snap", s.shadow.snap, ini);
    s.shadow.nearSnap   = GetB(kShadow, L"nearSnap", s.shadow.nearSnap, ini);
    s.shadow.sunStep    = Clamp(GetF(kShadow, L"sunStep", s.shadow.sunStep, ini), 0.0f, 5.0f);
    s.shadow.keepMargin = Clamp(GetF(kShadow, L"keepMargin", s.shadow.keepMargin, ini), 0.0f, 1000.0f);
    s.shadow.cacheTime  = Clamp(GetF(kShadow, L"cacheTime", s.shadow.cacheTime, ini), 0.0f, 600.0f);
    s.shadow.staleTime  = Clamp(GetF(kShadow, L"staleTime", s.shadow.staleTime, ini), 0.0f, 600.0f);
    s.shadow.evictDistance = Clamp(GetF(kShadow, L"evictDistance", s.shadow.evictDistance, ini), 1.0f, 500.0f);
    s.shadow.stillRadius   = Clamp(GetF(kShadow, L"stillRadius", s.shadow.stillRadius, ini), 0.0f, 3.0f);
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
    s.volume.leafShade    = Clamp(GetF(kVolume, L"leafShade",    s.volume.leafShade,    ini), 0.0f, 1.0f);
    s.volume.color        = GetX(kVolume, L"color", s.volume.color, ini) & 0xFFFFFF;
    s.volume.downscale    = GetI(kVolume, L"downscale", s.volume.downscale, ini);
    s.volume.blur         = GetB(kVolume, L"blur",  s.volume.blur,  ini);
    s.volume.debug        = GetI(kVolume, L"debug", s.volume.debug, ini);
    s.volume.quality      = GetI(kVolume, L"quality", s.volume.quality, ini);
    s.volume.occlusion    = GetB(kVolume, L"occlusion", s.volume.occlusion, ini);
    if (s.volume.quality < 1) s.volume.quality = 1;
    if (s.volume.quality > 3) s.volume.quality = 3;
    if (s.volume.downscale < 1) s.volume.downscale = 1;
    if (s.volume.downscale > 8) s.volume.downscale = 8;
    if (s.shadow.size < 256)  s.shadow.size = 256;
    if (s.shadow.size > 4096) s.shadow.size = 4096;

    s.sunShadows.enabled    = GetB(kSunShadows, L"enabled", s.sunShadows.enabled, ini);
    s.sunShadows.world      = GetB(kSunShadows, L"world", s.sunShadows.world, ini);
    s.sunShadows.units      = GetB(kSunShadows, L"units", s.sunShadows.units, ini);
    s.sunShadows.lock       = GetB(kSunShadows, L"lock", s.sunShadows.lock, ini);
    s.sunShadows.lockTilt   = Clamp(GetF(kSunShadows, L"lockTilt", s.sunShadows.lockTilt, ini), 0.0f, 80.0f);
    s.sunShadows.lodBias    = Clamp(GetF(kSunShadows, L"lodBias", s.sunShadows.lodBias, ini), 0.0f, 20.0f);
    s.sunShadows.lodStart   = Clamp(GetF(kSunShadows, L"lodStart", s.sunShadows.lodStart, ini), 0.0f, 1000.0f);
    s.sunShadows.indoor     = Clamp(GetF(kSunShadows, L"indoor", s.sunShadows.indoor, ini), 0.0f, 1.0f);
    s.sunShadows.shadeColor = GetX(kSunShadows, L"shadeColor", s.sunShadows.shadeColor, ini) & 0xFFFFFF;
    s.sunShadows.shadeTint  = Clamp(GetF(kSunShadows, L"shadeTint", s.sunShadows.shadeTint, ini), 0.0f, 1.0f);
    s.sunShadows.sunColor   = GetX(kSunShadows, L"sunColor", s.sunShadows.sunColor, ini) & 0xFFFFFF;
    s.sunShadows.sunTint    = Clamp(GetF(kSunShadows, L"sunTint", s.sunShadows.sunTint, ini), 0.0f, 1.0f);
    s.sunShadows.strength   = Clamp(GetF(kSunShadows, L"strength",   s.sunShadows.strength,   ini), 0.0f, 100.0f);
    s.sunShadows.night      = Clamp(GetF(kSunShadows, L"night",      s.sunShadows.night,      ini), 0.0f, 100.0f);
    s.sunShadows.riseFrom   = Clamp(GetF(kSunShadows, L"riseFrom",   s.sunShadows.riseFrom,   ini), 0.0f, 60.0f);
    s.sunShadows.riseTo     = Clamp(GetF(kSunShadows, L"riseTo",     s.sunShadows.riseTo,     ini), 10.0f, 89.0f);
    s.sunShadows.unitStrength = Clamp(GetF(kSunShadows, L"unitStrength", s.sunShadows.unitStrength, ini), 0.0f, 100.0f);
    s.sunShadows.unitGap    = Clamp(GetF(kSunShadows, L"unitGap",    s.sunShadows.unitGap,    ini), 0.0f, 5.0f);
    s.sunShadows.unitDrop   = Clamp(GetF(kSunShadows, L"unitDrop",   s.sunShadows.unitDrop,   ini), 0.0f, 100.0f);
    s.sunShadows.bodyShade  = Clamp(GetF(kSunShadows, L"bodyShade",  s.sunShadows.bodyShade,  ini), 0.0f, 100.0f);
    s.sunShadows.bias       = Clamp(GetF(kSunShadows, L"bias",       s.sunShadows.bias,       ini), 0.0f, 20.0f);
    s.sunShadows.normalBias = Clamp(GetF(kSunShadows, L"normalBias", s.sunShadows.normalBias, ini), 0.0f, 20.0f);
    s.sunShadows.slope      = Clamp(GetF(kSunShadows, L"slope",      s.sunShadows.slope,      ini), 0.0f, 1.0f);
    s.sunShadows.minGap     = Clamp(GetF(kSunShadows, L"minGap",     s.sunShadows.minGap,     ini), 0.0f, 20.0f);
    s.sunShadows.sunOffset  = Clamp(GetF(kSunShadows, L"sunOffset",  s.sunShadows.sunOffset,  ini), 0.0f, 2.0f);
    s.sunShadows.baked      = Clamp(GetF(kSunShadows, L"baked",      s.sunShadows.baked,      ini), 0.0f, 1.0f);
    s.sunShadows.leafShade  = Clamp(GetF(kSunShadows, L"leafShade",  s.sunShadows.leafShade,  ini), 0.0f, 1.0f);
    s.sunShadows.terrainShade = Clamp(GetF(kSunShadows, L"terrainShade", s.sunShadows.terrainShade, ini), 0.0f, 1.0f);
    s.sunShadows.terrainBias  = Clamp(GetF(kSunShadows, L"terrainBias",  s.sunShadows.terrainBias,  ini), 0.0f, 20.0f);
    s.sunShadows.sunlight   = Clamp(GetF(kSunShadows, L"sunlight",   s.sunShadows.sunlight,   ini), 0.0f, 0.5f);
    s.sunShadows.softness   = Clamp(GetF(kSunShadows, L"softness",   s.sunShadows.softness,   ini), 0.0f, 8.0f);
    s.sunShadows.debug      = GetI(kSunShadows, L"debug", s.sunShadows.debug, ini);

    s.lamps.enabled      = GetB(kLamps, L"enabled", s.lamps.enabled, ini);
    s.lamps.strength     = Clamp(GetF(kLamps, L"strength",     s.lamps.strength,     ini), 0.0f, 100.0f);
    s.lamps.maxIntensity = Clamp(GetF(kLamps, L"maxIntensity", s.lamps.maxIntensity, ini), 0.0f, 50.0f);
    s.lamps.surface      = Clamp(GetF(kLamps, L"surface",      s.lamps.surface,      ini), 0.0f, 20.0f);
    s.lamps.torchLight   = Clamp(GetF(kLamps, L"torchLight",   s.lamps.torchLight,   ini), 0.0f, 2.0f);
    s.lamps.lanternLight = Clamp(GetF(kLamps, L"lanternLight", s.lamps.lanternLight, ini), 0.0f, 2.0f);
    s.lamps.indoors      = Clamp(GetF(kLamps, L"indoors",      s.lamps.indoors,      ini), 0.0f, 1.0f);
    s.lamps.density      = Clamp(GetF(kLamps, L"density",      s.lamps.density,      ini), 0.0f, 1.0f);
    s.lamps.day          = Clamp(GetF(kLamps, L"day",          s.lamps.day,          ini), 0.0f, 100.0f);
    s.lamps.maxDistance  = Clamp(GetF(kLamps, L"maxDistance",  s.lamps.maxDistance,  ini), 5.0f, 1000.0f);
    s.lamps.maxLights    = GetI(kLamps, L"maxLights", s.lamps.maxLights, ini);
    if (s.lamps.maxLights < 1)  s.lamps.maxLights = 1;
    if (s.lamps.maxLights > 32) s.lamps.maxLights = 32;
    s.lamps.keep         = Clamp(GetF(kLamps, L"keep",         s.lamps.keep,         ini), 0.0f, 30.0f);
    s.lamps.softness     = Clamp(GetF(kLamps, L"softness",     s.lamps.softness,     ini), 0.05f, 5.0f);
    s.lamps.through      = Clamp(GetF(kLamps, L"through",      s.lamps.through,      ini), 0.0f, 10.0f);
    s.lamps.sprites      = GetB(kLamps, L"sprites", s.lamps.sprites, ini);
    s.lamps.files        = GetB(kLamps, L"files", s.lamps.files, ini);
    s.lamps.fogReach     = Clamp(GetF(kLamps, L"fogReach", s.lamps.fogReach, ini), 0.5f, 4.0f);
    s.lamps.spriteReach  = Clamp(GetF(kLamps, L"spriteReach",  s.lamps.spriteReach,  ini), 1.0f, 60.0f);
    s.lamps.spriteGain   = Clamp(GetF(kLamps, L"spriteGain",   s.lamps.spriteGain,   ini), 0.0f, 10.0f);
    s.lamps.debug        = GetI(kLamps, L"debug", s.lamps.debug, ini);

    s.rays.enabled      = GetB(kRays, L"enabled",      s.rays.enabled,      ini);
    s.rays.strength     = Clamp(GetF(kRays, L"strength",     s.rays.strength,     ini), 0.0f, 100.0f);
    s.rays.maxExposure  = Clamp(GetF(kRays, L"maxExposure",  s.rays.maxExposure,  ini), 0.0f, 20.0f);
    s.rays.threshold    = Clamp(GetF(kRays, L"threshold",    s.rays.threshold,    ini), 0.0f, 0.99f);
    s.rays.relThreshold = Clamp(GetF(kRays, L"relThreshold", s.rays.relThreshold, ini), 0.0f, 0.99f);
    s.rays.radius       = Clamp(GetF(kRays, L"radius",       s.rays.radius,       ini), 0.05f, 10.0f);
    s.rays.falloff      = Clamp(GetF(kRays, L"falloff",      s.rays.falloff,      ini), 0.25f, 8.0f);
    s.rays.length       = Clamp(GetF(kRays, L"length",       s.rays.length,       ini), 0.0f, 1.0f);
    s.rays.maxLength    = Clamp(GetF(kRays, L"maxLength",    s.rays.maxLength,    ini), 0.05f, 3.0f);
    s.rays.maxAngle     = Clamp(GetF(kRays, L"maxAngle",     s.rays.maxAngle,     ini), 45.0f, 180.0f);
    s.rays.viewFalloff  = Clamp(GetF(kRays, L"viewFalloff",  s.rays.viewFalloff,  ini), 0.1f, 8.0f);
    s.rays.parallel     = Clamp(GetF(kRays, L"parallel",     s.rays.parallel,     ini), 0.0f, 1.0f);
    s.rays.adaptTime    = Clamp(GetF(kRays, L"adaptTime",    s.rays.adaptTime,    ini), 0.0f, 10.0f);
    s.rays.coverTime    = Clamp(GetF(kRays, L"coverTime",    s.rays.coverTime,    ini), 0.0f, 10.0f);
    s.rays.soften       = Clamp(GetF(kRays, L"soften",       s.rays.soften,       ini), 0.0f, 32.0f);
    s.rays.smooth       = Clamp(GetF(kRays, L"smooth",       s.rays.smooth,       ini), 0.0f, 0.95f);
    s.rays.decay        = Clamp(GetF(kRays, L"decay",        s.rays.decay,        ini), 0.5f, 1.0f);
    s.rays.color        = GetX(kRays, L"color", s.rays.color, ini) & 0xFFFFFF;
    s.rays.passes       = GetI(kRays, L"passes",    s.rays.passes,    ini);
    s.rays.downscale    = GetI(kRays, L"downscale", s.rays.downscale, ini);
    s.rays.debugView    = GetI(kRays, L"debugView", s.rays.debugView, ini);
    s.rays.placement    = GetI(kRays, L"placement", s.rays.placement, ini);
    s.rays.skyOnly      = GetB(kRays, L"skyOnly",   s.rays.skyOnly,   ini);
    s.rays.mask         = static_cast<int>(Clamp(GetF(kRays, L"mask", static_cast<float>(s.rays.mask), ini), 0.0f, 1.0f));
    s.rays.secondMoon   = GetB(kRays, L"secondMoon", s.rays.secondMoon, ini);
    s.rays.occlusion       = GetB(kRays, L"occlusion", s.rays.occlusion, ini);
    s.rays.occlusionRadius = Clamp(GetF(kRays, L"occlusionRadius", s.rays.occlusionRadius, ini), 0.01f, 0.5f);
    s.rays.occlusionFull   = Clamp(GetF(kRays, L"occlusionFull",   s.rays.occlusionFull,   ini), 0.05f, 1.0f);
    if (s.rays.passes < 1)    s.rays.passes = 1;
    if (s.rays.passes > 3)    s.rays.passes = 3;
    if (s.rays.downscale < 1) s.rays.downscale = 1;
    if (s.rays.downscale > 8) s.rays.downscale = 8;

    s.night.strength = Clamp(GetF(kNight, L"strength", s.night.strength, ini), 0.0f, 100.0f);
    s.night.dusk     = Clamp(GetF(kNight, L"dusk",     s.night.dusk,     ini), 0.0f, 24.0f);
    s.night.dawn     = Clamp(GetF(kNight, L"dawn",     s.night.dawn,     ini), 0.0f, 24.0f);
    s.night.fade     = Clamp(GetF(kNight, L"fade",     s.night.fade,     ini), 0.0f, 6.0f);
    s.night.darkness = Clamp(GetF(kNight, L"darkness", s.night.darkness, ini), 0.0f, 0.9f);
    s.night.tint     = Clamp(GetF(kNight, L"tint",     s.night.tint,     ini), 0.0f, 1.0f);
    s.night.moonColor = GetX(kNight, L"moonColor", s.night.moonColor, ini) & 0xFFFFFF;
    s.night.sky      = Clamp(GetF(kNight, L"sky",      s.night.sky,      ini), 0.0f, 1.0f);
    s.night.indoors  = GetB(kNight, L"indoors", s.night.indoors, ini);

    s.bench.settle  = Clamp(GetF(kBench, L"settle",  s.bench.settle,  ini), 0.5f, 30.0f);
    s.bench.measure = Clamp(GetF(kBench, L"measure", s.bench.measure, ini), 1.0f, 60.0f);


    s.trace       = GetB(kGeneral, L"trace", s.trace, ini);
    s.logEnabled  = GetB(kGeneral, L"log",         s.logEnabled,  ini);
    s.hook        = GetB(kGeneral, L"hook",        s.hook,        ini);
    s.sliders     = GetB(kGeneral, L"sliders",     s.sliders,     ini);
    s.master      = GetB(kGeneral, L"enabled",     s.master,      ini);
    s.reloadKey   = GetI(kGeneral, L"reloadKey",   s.reloadKey,   ini);
    s.probeKey    = GetI(kGeneral, L"probeKey",    s.probeKey,    ini);
    s.chainWaitMs = GetI(kGeneral, L"chainWaitMs", s.chainWaitMs, ini);
    s.minWorldDraws = GetI(kGeneral, L"minWorldDraws", s.minWorldDraws, ini);

    g_cfg = s;
}
