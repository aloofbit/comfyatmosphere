#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

Settings g_cfg;

namespace
{
    const wchar_t* kFog     = L"fog";
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

    s.logEnabled  = GetB(kGeneral, L"log",         s.logEnabled,  ini);
    s.hook        = GetB(kGeneral, L"hook",        s.hook,        ini);
    s.reloadKey   = GetI(kGeneral, L"reloadKey",   s.reloadKey,   ini);
    s.probeKey    = GetI(kGeneral, L"probeKey",    s.probeKey,    ini);
    s.chainWaitMs = GetI(kGeneral, L"chainWaitMs", s.chainWaitMs, ini);

    g_cfg = s;
}
