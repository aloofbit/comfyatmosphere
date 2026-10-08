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
    const wchar_t* kColour  = L"colour";
    const wchar_t* kLighthouse = L"lighthouse";
    const wchar_t* kGrass   = L"grass";
    const wchar_t* kTime    = L"time";
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

    // A word list ("Roc,Bon"): the text up to a comment, without the spaces at its ends.
    void GetS(const wchar_t* sec, const wchar_t* key, char* out, size_t cap, const wchar_t* ini)
    {
        wchar_t buf[128];
        const ConfigSource src = ReadRaw(sec, key, ini, buf, 128);
        if (src != kFromDefault)
        {
            std::string s = Narrow(buf);
            const size_t semi = s.find(';');
            if (semi != std::string::npos)
                s.resize(semi);
            const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
            s = a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
            strncpy_s(out, cap, s.c_str(), _TRUNCATE);
        }
        Note(sec, key, out, src);
    }

    float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

bool ResolveIniPath(HMODULE self, wchar_t* out, size_t count)
{
    GetModuleFileNameW(self, out, static_cast<DWORD>(count));
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash)
        return false;
    wcscpy_s(slash + 1, count - (slash + 1 - out), L"comfyatmos.ini");
    // The files were comfyfog.dll and comfyfog.ini until v0.10.0-alpha (2026-10-06). A client installed by hand may
    // still have only the old ini: it is read, so its values are kept.
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
        return false;
    std::wstring old(out, slash + 1 - out);
    old += L"comfyfog.ini";
    if (GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    wcscpy_s(out, count, old.c_str());
    return true;
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
    s.fog.toward       = Clamp(GetF(kFog, L"toward",       s.fog.toward,       ini), 0.0f, 0.9f);
    s.fog.horizonGlow  = Clamp(GetF(kFog, L"horizonGlow",  s.fog.horizonGlow,  ini), 0.0f, 2.0f);
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
    s.water.rippleDepthMoving = Clamp(GetF(kWater, L"rippleDepthMoving", s.water.rippleDepthMoving, ini), 0.0f, 4.0f);
    s.water.foamDrawn = GetB(kWater, L"foamDrawn", s.water.foamDrawn, ini);
    s.water.foamCell  = Clamp(GetF(kWater, L"foamCell", s.water.foamCell, ini), 0.2f, 4.0f);
    s.water.foamLife  = Clamp(GetF(kWater, L"foamLife", s.water.foamLife, ini), 0.5f, 10.0f);
    s.water.openFoam  = GetB(kWater, L"openFoam", s.water.openFoam, ini);
    s.water.wakeFoam  = Clamp(GetF(kWater, L"wakeFoam", s.water.wakeFoam, ini), 0.0f, 1.0f);
    s.water.shipWake  = Clamp(GetF(kWater, L"shipWake", s.water.shipWake, ini), 0.0f, 20.0f);
    s.water.shipWakeForward = Clamp(GetF(kWater, L"shipWakeForward", s.water.shipWakeForward, ini), 0.0f, 60.0f);
    s.water.shipWakeDepth   = Clamp(GetF(kWater, L"shipWakeDepth", s.water.shipWakeDepth, ini), 0.0f, 6.0f);
    s.water.lakeSwash = Clamp(GetF(kWater, L"lakeSwash", s.water.lakeSwash, ini), 0.0f, 1.0f);
    s.water.lakeFoam  = Clamp(GetF(kWater, L"lakeFoam", s.water.lakeFoam, ini), 0.0f, 1.0f);
    s.water.lakeWaves = Clamp(GetF(kWater, L"lakeWaves", s.water.lakeWaves, ini), 0.0f, 1.0f);
    s.water.rain      = Clamp(GetF(kWater, L"rain", s.water.rain, ini), 0.0f, 2.0f);
    s.water.objectFoam = Clamp(GetF(kWater, L"objectFoam", s.water.objectFoam, ini), 0.0f, 1.0f);
    s.water.objectFoamWidth = Clamp(GetF(kWater, L"objectFoamWidth", s.water.objectFoamWidth, ini), 0.2f, 3.0f);
    s.water.foamEdge  = Clamp(GetF(kWater, L"foamEdge", s.water.foamEdge, ini), 0.0f, 0.9f);
    s.water.rippleSpread = Clamp(GetF(kWater, L"rippleSpread", s.water.rippleSpread, ini), 0.1f, 3.0f);
    s.water.rippleSpreadMoving = Clamp(GetF(kWater, L"rippleSpreadMoving", s.water.rippleSpreadMoving, ini), 0.1f, 3.0f);
    s.water.wetSand     = Clamp(GetF(kWater, L"wetSand",   s.water.wetSand,   ini), 0.0f, 1.0f);
    s.water.surface     = Clamp(GetF(kWater, L"surface",   s.water.surface,   ini), 0.0f, 1.0f);
    s.water.clarity     = Clamp(GetF(kWater, L"clarity",   s.water.clarity,   ini), 0.1f, 10.0f);
    s.water.colour      = Clamp(GetF(kWater, L"colour",    s.water.colour,    ini), 0.0f, 100.0f);
    s.water.reflection  = Clamp(GetF(kWater, L"reflection", s.water.reflection, ini), 0.0f, 1.0f);
    s.water.zone        = GetB(kWater, L"zone",      s.water.zone,      ini);
    s.water.sunset      = Clamp(GetF(kWater, L"sunset",    s.water.sunset,    ini), 0.0f, 3.0f);
    s.water.skyColor    = GetX(kWater, L"skyColor",  s.water.skyColor,  ini) & 0xFFFFFF;
    s.water.skyFromGame = GetB(kWater, L"skyFromGame", s.water.skyFromGame, ini);
    s.water.brightness  = Clamp(GetF(kWater, L"brightness", s.water.brightness, ini), 0.25f, 2.0f);
    s.water.glint       = Clamp(GetF(kWater, L"glint",     s.water.glint,     ini), 0.0f, 3.0f);
    s.water.moonGlint   = Clamp(GetF(kWater, L"moonGlint", s.water.moonGlint, ini), 0.0f, 3.0f);
    s.water.glintSize   = Clamp(GetF(kWater, L"glintSize", s.water.glintSize, ini), 0.25f, 4.0f);
    s.water.waves       = Clamp(GetF(kWater, L"waves",     s.water.waves,     ini), 0.0f, 3.0f);
    s.water.waveHeight  = Clamp(GetF(kWater, L"waveHeight", s.water.waveHeight, ini), 0.0f, 3.0f);
    s.water.waveScale   = Clamp(GetF(kWater, L"waveScale", s.water.waveScale, ini), 0.25f, 4.0f);
    s.water.whitecaps   = Clamp(GetF(kWater, L"whitecaps", s.water.whitecaps, ini), 0.0f, 1.0f);
    s.water.swash       = Clamp(GetF(kWater, L"swash",     s.water.swash,     ini), 0.0f, 1.0f);
    s.water.swashHeight = Clamp(GetF(kWater, L"swashHeight", s.water.swashHeight, ini), 0.0f, 1.0f);
    s.water.swashLength = Clamp(GetF(kWater, L"swashLength", s.water.swashLength, ini), 5.0f, 200.0f);
    s.water.swashSpeed  = Clamp(GetF(kWater, L"swashSpeed", s.water.swashSpeed, ini), 0.1f, 4.0f);
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
    s.water.debugSkip   = GetI(kWater, L"debugSkip", s.water.debugSkip, ini);
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
    s.shadow.keepStill  = GetI(kShadow, L"keepStill", s.shadow.keepStill ? 1 : 0, ini) != 0;
    s.shadow.midEvery   = static_cast<int>(Clamp(static_cast<float>(GetI(kShadow, L"midEvery", s.shadow.midEvery, ini)), 1.0f, 8.0f));
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
    s.shadow.debugSkip  = GetI(kShadow, L"debugSkip", s.shadow.debugSkip, ini);
    s.shadow.minTriangles = GetI(kShadow, L"minTriangles", s.shadow.minTriangles, ini);
    s.shadow.nearMargin = Clamp(GetF(kShadow, L"nearMargin", s.shadow.nearMargin, ini), 0.0f, 200.0f);
    s.shadow.leaves     = GetB(kShadow, L"leaves", s.shadow.leaves, ini);
    s.shadow.terrainLeaves = GetB(kShadow, L"terrainLeaves", s.shadow.terrainLeaves, ini);
    s.shadow.mapTerrain    = GetB(kShadow, L"mapTerrain", s.shadow.mapTerrain, ini);
    s.shadow.terrainLow    = GetB(kShadow, L"terrainLow", s.shadow.terrainLow, ini);
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
    s.sunShadows.hillCarry    = Clamp(GetF(kSunShadows, L"hillCarry",    s.sunShadows.hillCarry,    ini), 0.0f, 500.0f);
    s.sunShadows.water        = Clamp(GetF(kSunShadows, L"water",        s.sunShadows.water,        ini), 0.0f, 1.0f);
    s.sunShadows.sunlight   = Clamp(GetF(kSunShadows, L"sunlight",   s.sunShadows.sunlight,   ini), 0.0f, 0.5f);
    s.sunShadows.softness   = Clamp(GetF(kSunShadows, L"softness",   s.sunShadows.softness,   ini), 0.0f, 8.0f);
    s.sunShadows.fetch4     = GetI(kSunShadows, L"fetch4", s.sunShadows.fetch4 ? 1 : 0, ini) != 0;
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
    s.night.rain     = Clamp(GetF(kNight, L"rain",     s.night.rain,     ini), 0.0f, 0.9f);
    s.night.indoors  = GetB(kNight, L"indoors", s.night.indoors, ini);

    s.colour.enabled = GetB(kColour, L"enabled", s.colour.enabled, ini);
    s.lighthouse.enabled = GetB(kLighthouse, L"enabled", s.lighthouse.enabled, ini);
    s.lighthouse.beacon  = Clamp(GetF(kLighthouse, L"beacon", s.lighthouse.beacon, ini), 0.0f, 3.0f);
    s.lighthouse.beam    = Clamp(GetF(kLighthouse, L"beam", s.lighthouse.beam, ini), 0.0f, 1.0f);
    s.lighthouse.beaconSize = Clamp(GetF(kLighthouse, L"beaconSize", s.lighthouse.beaconSize, ini), 0.2f, 20.0f);
    s.lighthouse.beamWidth  = Clamp(GetF(kLighthouse, L"beamWidth", s.lighthouse.beamWidth, ini), 0.05f, 10.0f);
    s.lighthouse.beamSpread = Clamp(GetF(kLighthouse, L"beamSpread", s.lighthouse.beamSpread, ini), 0.0f, 0.5f);
    s.lighthouse.beamLength = Clamp(GetF(kLighthouse, L"beamLength", s.lighthouse.beamLength, ini), 10.0f, 2000.0f);
    s.lighthouse.reach      = Clamp(GetF(kLighthouse, L"reach", s.lighthouse.reach, ini), 50.0f, 1500.0f);
    s.lighthouse.beamSpeed  = Clamp(GetF(kLighthouse, L"beamSpeed", s.lighthouse.beamSpeed, ini), 1.0f, 120.0f);
    s.lighthouse.surface      = Clamp(GetF(kLighthouse, L"surface", s.lighthouse.surface, ini), 0.0f, 10.0f);
    s.lighthouse.faceStrength = Clamp(GetF(kLighthouse, L"faceStrength", s.lighthouse.faceStrength, ini), 0.0f, 4.0f);
    s.lighthouse.faceTilt     = Clamp(GetF(kLighthouse, L"faceTilt", s.lighthouse.faceTilt, ini), 0.0f, 0.3f);
    s.lighthouse.faceSoft     = Clamp(GetF(kLighthouse, L"faceSoft", s.lighthouse.faceSoft, ini), 0.005f, 0.3f);
    s.lighthouse.waterWidth   = Clamp(GetF(kLighthouse, L"waterWidth", s.lighthouse.waterWidth, ini), 0.25f, 4.0f);
    s.lighthouse.glint        = Clamp(GetF(kLighthouse, L"glint", s.lighthouse.glint, ini), 0.0f, 3.0f);
    s.lighthouse.surfaceReach = Clamp(GetF(kLighthouse, L"surfaceReach", s.lighthouse.surfaceReach, ini), 2.0f, 100.0f);
    s.lighthouse.beamCount  = GetI(kLighthouse, L"beamCount", s.lighthouse.beamCount, ini) >= 2 ? 2 : 1;
    s.lighthouse.beamTilt   = Clamp(GetF(kLighthouse, L"beamTilt", s.lighthouse.beamTilt, ini), -1.0f, 1.0f);
    s.lighthouse.hideGameLight = GetB(kLighthouse, L"hideGameLight", s.lighthouse.hideGameLight, ini);
    s.lighthouse.lampShift[0] = Clamp(GetF(kLighthouse, L"lampShiftX", s.lighthouse.lampShift[0], ini), -10.0f, 10.0f);
    s.lighthouse.lampShift[1] = Clamp(GetF(kLighthouse, L"lampShiftY", s.lighthouse.lampShift[1], ini), -10.0f, 10.0f);
    s.lighthouse.lampRise = Clamp(GetF(kLighthouse, L"lampRise", s.lighthouse.lampRise, ini), -20.0f, 40.0f);
    s.lighthouse.day     = Clamp(GetF(kLighthouse, L"day", s.lighthouse.day, ini), 0.0f, 1.0f);
    s.lighthouse.lampDrop = Clamp(GetF(kLighthouse, L"lampDrop", s.lighthouse.lampDrop, ini), 0.0f, 40.0f);
    s.lighthouse.color   = GetX(kLighthouse, L"color", s.lighthouse.color, ini) & 0xFFFFFF;
    s.colour.day   = Clamp(GetF(kColour, L"day",   s.colour.day,   ini), 0.0f, 200.0f);
    s.colour.night = Clamp(GetF(kColour, L"night", s.colour.night, ini), 0.0f, 200.0f);

    GrassSettings& g = s.grass;
    g.enabled         = GetB(kGrass, L"enabled", g.enabled, ini);
    g.scale           = Clamp(GetF(kGrass, L"scale", g.scale, ini), 0.0f, 5.0f);
    g.directionDeg    = GetF(kGrass, L"directionDeg", g.directionDeg, ini);
    g.speed           = Clamp(GetF(kGrass, L"speed", g.speed, ini), 0.0f, 20.0f);
    g.amplitude       = Clamp(GetF(kGrass, L"amplitude", g.amplitude, ini), 0.0f, 1.0f);
    g.wavelength      = Clamp(GetF(kGrass, L"wavelength", g.wavelength, ini), 0.5f, 200.0f);
    g.crossAmplitude  = Clamp(GetF(kGrass, L"crossAmplitude", g.crossAmplitude, ini), 0.0f, 1.0f);
    g.crossWavelength = Clamp(GetF(kGrass, L"crossWavelength", g.crossWavelength, ini), 0.5f, 200.0f);
    g.crossAngleDeg   = GetF(kGrass, L"crossAngleDeg", g.crossAngleDeg, ini);
    g.lean            = Clamp(GetF(kGrass, L"lean", g.lean, ini), -2.0f, 2.0f);
    g.variance        = Clamp(GetF(kGrass, L"variance", g.variance, ini), 0.0f, 1.0f);
    g.anchor          = Clamp(GetF(kGrass, L"anchor", g.anchor, ini), 0.0f, 0.9f);
    g.worldPhase      = GetB(kGrass, L"worldPhase", g.worldPhase, ini);
    g.parting         = GetB(kGrass, L"parting", g.parting, ini);
    g.radius          = Clamp(GetF(kGrass, L"radius", g.radius, ini), 0.1f, 20.0f);
    g.forceCenter     = Clamp(GetF(kGrass, L"forceCenter", g.forceCenter, ini), 0.0f, 3.0f);
    g.forceEdge       = Clamp(GetF(kGrass, L"forceEdge", g.forceEdge, ini), 0.0f, 3.0f);
    g.centerZ         = Clamp(GetF(kGrass, L"centerZ", g.centerZ, ini), -5.0f, 5.0f);
    g.zRange          = Clamp(GetF(kGrass, L"zRange", g.zRange, ini), 0.1f, 50.0f);
    g.zFade           = Clamp(GetF(kGrass, L"zFade", g.zFade, ini), 0.01f, 50.0f);
    g.models          = GetB(kGrass, L"models", g.models, ini);
    g.rigidHeight     = Clamp(GetF(kGrass, L"rigidHeight", g.rigidHeight, ini), 0.0f, 5.0f);
    GetS(kGrass, L"rigidNames", g.rigidNames, sizeof(g.rigidNames), ini);
    g.fillAddr        = GetX(kGrass, L"fillAddr", g.fillAddr, ini);
    g.stride          = static_cast<UINT>(GetI(kGrass, L"stride", static_cast<int>(g.stride), ini));
    g.minVerts        = static_cast<UINT>(GetI(kGrass, L"minVerts", static_cast<int>(g.minVerts), ini));
    g.maxVerts        = static_cast<UINT>(GetI(kGrass, L"maxVerts", static_cast<int>(g.maxVerts), ini));
    g.primType        = GetI(kGrass, L"primType", g.primType, ini);
    g.debug           = GetI(kGrass, L"debug", g.debug, ini);

    TimeSettings& tm = s.time;
    tm.enabled      = GetB(kTime, L"enabled", tm.enabled, ini);
    tm.hour         = Clamp(GetF(kTime, L"hour", tm.hour, ini), 0.0f, 24.0f);
    tm.step         = Clamp(GetF(kTime, L"step", tm.step, ini), 0.001f, 6.0f);
    tm.dayHour      = Clamp(GetF(kTime, L"dayHour", tm.dayHour, ini), 0.0f, 24.0f);
    tm.nightHour    = Clamp(GetF(kTime, L"nightHour", tm.nightHour, ini), 0.0f, 24.0f);
    tm.addrMinutes  = GetX(kTime, L"addrMinutes",  tm.addrMinutes,  ini);
    tm.addrFraction = GetX(kTime, L"addrFraction", tm.addrFraction, ini);
    tm.addrMinutesF = GetX(kTime, L"addrMinutesF", tm.addrMinutesF, ini);

    s.bench.settle  = Clamp(GetF(kBench, L"settle",  s.bench.settle,  ini), 0.5f, 30.0f);
    s.bench.measure = Clamp(GetF(kBench, L"measure", s.bench.measure, ini), 1.0f, 60.0f);


    s.trace       = GetB(kGeneral, L"trace", s.trace, ini);
    s.logEnabled  = GetB(kGeneral, L"log",         s.logEnabled,  ini);
    s.hook        = GetB(kGeneral, L"hook",        s.hook,        ini);
    s.sliders     = GetB(kGeneral, L"sliders",     s.sliders,     ini);
    s.master      = GetB(kGeneral, L"enabled",     s.master,      ini);
    s.hotkeys     = GetB(kGeneral, L"hotkeys",     s.hotkeys,     ini);
    s.reloadKey   = GetI(kGeneral, L"reloadKey",   s.reloadKey,   ini);
    s.probeKey    = GetI(kGeneral, L"probeKey",    s.probeKey,    ini);
    s.scanKey     = GetI(kGeneral, L"scanKey",     s.scanKey,     ini);
    s.dayNightKey = GetI(kGeneral, L"dayNightKey", s.dayNightKey, ini);
    s.chainWaitMs = GetI(kGeneral, L"chainWaitMs", s.chainWaitMs, ini);
    s.minWorldDraws = GetI(kGeneral, L"minWorldDraws", s.minWorldDraws, ini);

    g_cfg = s;
}
