// cvars: the in-game controls. The ComfyAtmosphere addon adds tick boxes and sliders to Video > Shaders,
// and each one is a CVar. The client's options panel calls SetCVar as a slider moves, and this file reads
// the CVars back a few times a second and lays their values over the ini.
//
// The DLL registers the CVars itself, with the ini values as their defaults. That does two things:
//   - the panel's Defaults button (GetCVarDefault) puts the ini values back;
//   - the addon can tell the DLL is loaded: GetCVar on a CVar that does not exist raises a Lua error,
//     so the addon adds its controls only when these exist.
//
// Two client functions, found by disassembling the Lua RegisterCVar at 0x00488B00 in this WoW.exe:
//
//   0x0063DEC0  CVar* __fastcall Lookup(const char* name)
//               Returns 0 for an unknown name, and also for an entry that is not registered yet (flag
//               0x80000000 clear at +0x1C), which is what a Config.wtf line for a later CVar leaves.
//   0x0063DB90  CVar* __fastcall Register(name, help, flags, default, callback, category, arg5, cbArg)
//               ret 0x18. The Lua RegisterCVar calls Register(name, 0, 0, default, 0, 9, 0, 0) only
//               when Lookup misses, and that exact call is repeated here.
//
// The value string is at +0x20 (the Lua GetCVar reads it there). A CVar is never freed while the client
// runs, so the pointer Register returns is kept. The string it points to is not: SetCVar can replace it,
// so +0x20 is read again on every poll.
//
// All of this runs on the client's main thread (Present is called from the client's render), the same
// thread Lua runs on, so nothing here races the options panel.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include "cvars.h"
#include "common.h"
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr DWORD kLookup    = 0x0063DEC0;
    constexpr DWORD kRegister  = 0x0063DB90;
    constexpr DWORD kHashMask  = 0x00C4EDB8;   // -1 until the client's CVar table exists
    constexpr DWORD kCategory  = 9;            // what the Lua RegisterCVar passes

    // The client's Lua state: 0x007040D0 is "mov eax, [0x00CEEF74]; ret", which FrameScript uses to get
    // it. It stays 0 until the client's Lua is up.
    //
    // A registration as soon as the CVar table existed was too early. It fell on the client's first
    // frame, before the login screen had loaded, and the client drew nothing after it: a white window.
    // The next start registered after the login screen had drawn, and ran normally. So the DLL now waits
    // until Lua has been up for kLuaSettle seconds, which puts it where an addon's RegisterCVar would be.
    constexpr DWORD  kLuaStateGetter = 0x007040D0;
    constexpr DWORD  kLuaState       = 0x00CEEF74;
    constexpr double kLuaSettle      = 1.0;
    const int kLuaStateGetterHead[]  = { 0xA1, -1, -1, -1, -1, 0xC3 };

    // The first bytes of each function. -1 marks the four bytes of the absolute address 0x00C4EDB8,
    // which are skipped so a relocated image still matches.
    const int kLookupHead[]   = { 0x83, 0x3D, -1, -1, -1, -1, 0xFF, 0x53, 0x56, 0x57, 0x8B, 0xF9 };
    const int kRegisterHead[] = { 0x55, 0x8B, 0xEC, 0x51, 0x83, 0x3D, -1, -1, -1, -1, 0xFF, 0x53 };

    using LookupFn   = void* (__fastcall*)(const char* name);
    using RegisterFn = void* (__fastcall*)(const char* name, const char* help, DWORD flags,
                                           const char* dflt, void* callback, DWORD category,
                                           DWORD arg5, void* cbArg);

    enum Knob { kFog, kFogThickness, kVolume, kVolumeStrength, kClouds, kRays, kRaysStrength, kVolumeQuality,
                kKnobs };

    const char* const kNames[kKnobs] = {
        "comfyFog", "comfyFogThickness",
        "comfyVolume", "comfyVolumeStrength",
        "comfyClouds",
        "comfyRays", "comfyRaysStrength",
        "comfyVolumeQuality",
    };

    struct Slot
    {
        void* cvar    = nullptr;
        bool  seen    = false;     // a value was read at least once
        float value   = 0.0f;
        char  last[32] = {};
    };

    Slot     g_slots[kKnobs];
    Settings g_base;                // the settings as the ini gave them, before the controls
    bool     g_haveBase = false;
    bool     g_ready    = false;    // the CVars are registered
    bool     g_gaveUp   = false;    // wrong client build, or [general] sliders = 0
    bool     g_checked  = false;    // the function heads matched this WoW.exe
    double   g_luaSince = 0.0;      // when the Lua state was first seen, 0 while it is not there
    double   g_nextPoll = 0.0;

    intptr_t Slide()
    {
        static const intptr_t slide = reinterpret_cast<intptr_t>(GetModuleHandleW(nullptr)) - 0x00400000;
        return slide;
    }

    bool SafeCopy(uintptr_t src, void* dst, size_t n)
    {
        __try
        {
            memcpy(dst, reinterpret_cast<const void*>(src), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Copies a C string one byte at a time, so a short string near the end of a page does not fault.
    bool SafeString(uintptr_t src, char* out, size_t cap)
    {
        __try
        {
            const char* s = reinterpret_cast<const char*>(src);
            size_t i = 0;
            for (; i + 1 < cap && s[i]; ++i)
                out[i] = s[i];
            out[i] = 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out[0] = 0;
            return false;
        }
    }

    bool HeadMatches(DWORD addr, const int* head, size_t n)
    {
        unsigned char b[16] = {};
        if (n > sizeof(b) || !SafeCopy(addr + Slide(), b, n))
            return false;
        for (size_t i = 0; i < n; ++i)
            if (head[i] >= 0 && b[i] != head[i])
                return false;
        return true;
    }

    float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // The ini value each control starts from, as the string Register takes for a default.
    void DefaultFor(int k, const Settings& s, char* out, size_t cap)
    {
        switch (k)
        {
        case kFog:            snprintf(out, cap, "%d", s.fog.enabled ? 1 : 0); break;
        case kFogThickness:   snprintf(out, cap, "%.0f", s.fog.thickness); break;
        // The light draws nothing without the depth buffer and the shadow map, so the ini counts as
        // "on" only when all three are.
        case kVolume:         snprintf(out, cap, "%d",
                                       (s.volume.enabled && s.depth.enabled && s.shadow.enabled) ? 1 : 0);
                              break;
        case kVolumeStrength: snprintf(out, cap, "%.0f", s.volume.strength); break;
        case kClouds:         snprintf(out, cap, "%d", s.sky.clouds ? 1 : 0); break;
        case kRays:           snprintf(out, cap, "%d", s.rays.enabled ? 1 : 0); break;
        case kRaysStrength:   snprintf(out, cap, "%.0f", s.rays.strength); break;
        case kVolumeQuality:  snprintf(out, cap, "%d", s.volume.quality); break;
        }
    }

    // The controls laid over a copy of the ini settings. Only a control that has been read counts.
    void Apply(Settings& s)
    {
        const Slot* c = g_slots;
        if (c[kFog].seen)            s.fog.enabled     = c[kFog].value != 0.0f;
        if (c[kFogThickness].seen)   s.fog.thickness   = Clamp(c[kFogThickness].value, 0.0f, 100.0f);
        if (c[kVolumeStrength].seen) s.volume.strength = Clamp(c[kVolumeStrength].value, 0.0f, 100.0f);
        if (c[kClouds].seen)         s.sky.clouds      = c[kClouds].value != 0.0f;
        if (c[kRays].seen)           s.rays.enabled    = c[kRays].value != 0.0f;
        if (c[kRaysStrength].seen)   s.rays.strength   = Clamp(c[kRaysStrength].value, 0.0f, 100.0f);
        if (c[kVolumeQuality].seen)  s.volume.quality  = static_cast<int>(Clamp(c[kVolumeQuality].value, 1.0f, 3.0f) + 0.5f);

        // One tick box for the light, so it turns on what the light needs. Off, the depth buffer and the
        // shadow map go back to what the ini says.
        if (c[kVolume].seen)
        {
            const bool on = c[kVolume].value != 0.0f;
            s.volume.enabled = on;
            if (on)
                s.depth.enabled = s.shadow.enabled = true;
        }

        // Last: the quality level replaces the values it covers, whichever of the ini and the controls
        // set it.
        ApplyVolumeQuality(s);
    }

    // False when this is not the WoW.exe the addresses were found in, or [general] sliders = 0.
    bool Check()
    {
        if (!g_cfg.sliders)
        {
            Log("[general] sliders = 0: the in-game controls are not registered");
            return false;
        }
        if (!HeadMatches(kLookup, kLookupHead, sizeof(kLookupHead) / sizeof(kLookupHead[0])) ||
            !HeadMatches(kRegister, kRegisterHead, sizeof(kRegisterHead) / sizeof(kRegisterHead[0])) ||
            !HeadMatches(kLuaStateGetter, kLuaStateGetterHead,
                         sizeof(kLuaStateGetterHead) / sizeof(kLuaStateGetterHead[0])))
        {
            Log("the CVar functions are not where this WoW.exe keeps them: no in-game controls");
            return false;
        }
        return true;
    }

    // True once the client's Lua has been up for kLuaSettle seconds.
    bool LuaSettled(double now)
    {
        DWORD state = 0;
        if (!SafeCopy(kLuaState + Slide(), &state, 4) || !state)
        {
            g_luaSince = 0.0;
            return false;
        }
        if (g_luaSince == 0.0)
            g_luaSince = now;
        return now - g_luaSince >= kLuaSettle;
    }

    void Register()
    {
        const auto lookup   = reinterpret_cast<LookupFn>(kLookup + Slide());
        const auto registerFn = reinterpret_cast<RegisterFn>(kRegister + Slide());
        for (int k = 0; k < kKnobs; ++k)
        {
            void* cv = lookup(kNames[k]);
            if (!cv)
            {
                char dflt[32];
                DefaultFor(k, g_base, dflt, sizeof(dflt));
                cv = registerFn(kNames[k], nullptr, 0, dflt, nullptr, kCategory, 0, nullptr);
            }
            g_slots[k].cvar = cv;
            if (!cv)
                Log("could not register CVar %s", kNames[k]);
        }
        Log("in-game controls: %d CVars registered, %.1f s after the client's Lua came up", kKnobs,
            Now() - g_luaSince);
    }
}

void CVarsAfterLoad()
{
    g_base = g_cfg;
    g_haveBase = true;
    Apply(g_cfg);
}

bool CVarsPoll()
{
    if (g_gaveUp || !g_haveBase)
        return false;

    const double now = Now();
    if (now < g_nextPoll)
        return false;
    g_nextPoll = now + 0.2;

    if (!g_ready)
    {
        if (!g_checked)
        {
            if (!Check())
            {
                g_gaveUp = true;
                return false;
            }
            g_checked = true;
        }
        if (!LuaSettled(now))
            return false;
        DWORD mask = 0xFFFFFFFF;
        if (!SafeCopy(kHashMask + Slide(), &mask, 4) || mask == 0xFFFFFFFF)
            return false;
        Register();
        g_ready = true;
    }

    bool changed = false;
    for (int k = 0; k < kKnobs; ++k)
    {
        Slot& s = g_slots[k];
        DWORD str = 0;
        char  buf[32];
        if (!s.cvar || !SafeCopy(reinterpret_cast<uintptr_t>(s.cvar) + 0x20, &str, 4) || !str ||
            !SafeString(str, buf, sizeof(buf)))
            continue;
        if (s.seen && strcmp(buf, s.last) == 0)
            continue;
        strcpy_s(s.last, buf);
        s.value = static_cast<float>(atof(buf));
        s.seen = true;
        changed = true;
        Log("--- control: %s = %s ---", kNames[k], buf);
    }

    if (changed)
    {
        g_cfg = g_base;
        Apply(g_cfg);
    }
    return changed;
}
