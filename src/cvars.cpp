// cvars: the in-game controls. The ComfyAtmosphere addon adds tick boxes and sliders to Video > Atmosphere,
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
// The /atmos command (tune.cpp) uses the same two functions. The addon sets the string CVar comfyTune to
// "<number> <command>"; each new value is run once, and the answer is registered as new CVars, since the
// DLL can register a CVar but has no way to write chat: comfyTuneReply<number>_1 and on, one chat line
// each, then comfyTuneReply<number> holding how many. The addon waits for that last one and prints the
// lines. A CVar at its default value is not written to Config.wtf, so the answers are not saved; the
// addon sets comfyTune back to empty once answered, which is its default.
//
// All of this runs on the client's main thread (Present is called from the client's render), the same
// thread Lua runs on, so nothing here races the options panel.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include "cvars.h"
#include "common.h"
#include "config.h"
#include "tune.h"

#include <deque>
#include <string>

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
                kNightStrength, kRaysSoften, kRaysSmooth, kDebugView, kShadowResolution, kShadowSoftness,
                kShadowEvery, kSunShadows, kSunShadowStrength, kFogHeight, kFogFade, kFogDarkness,
                kFogGreyness, kMaster, kFogHaze, kSunlight, kVolumeDensity, kVolumeDistance, kVolumeDirection,
                kShadowsWorld, kShadowsUnits, kShadowsLock, kShadowsTilt, kShadeTint, kSunTint, kFogSunGlow,
                kFogSunBright, kFogReach, kFogNear, kFogFar, kLampGlow, kLampDistance, kNightDarkness,
                kMoonlight, kKnobs };

    const char* const kNames[kKnobs] = {
        "comfyFog", "comfyFogThickness",
        "comfyVolume", "comfyVolumeStrength",
        "comfyClouds",
        "comfyRays", "comfyRaysStrength",
        "comfyVolumeQuality",
        "comfyNightStrength",
        "comfyRaysSoften",
        "comfyRaysSmooth",
        "comfyDebugView",
        "comfyShadowResolution",
        "comfyShadowSoftness",
        "comfyShadowEvery",
        "comfySunShadows",
        "comfySunShadowStrength",
        "comfyFogHeight",
        "comfyFogFade",
        "comfyFogDarkness",
        "comfyFogGreyness",
        "comfyAtmosphere",
        "comfyFogHaze",
        "comfySunlight",
        "comfyVolumeDensity",
        "comfyVolumeDistance",
        "comfyVolumeDirection",
        "comfySunShadowsWorld",
        "comfySunShadowsUnits",
        "comfyShadowLock",
        "comfyShadowTilt",
        "comfyShadeTint",
        "comfySunTint",
        "comfyFogSunGlow",
        "comfyFogSunBright",
        "comfyFogReach",
        "comfyFogNear",
        "comfyFogFar",
        "comfyLampGlow",
        "comfyLampDistance",
        "comfyNightDarkness",
        "comfyMoonlight",
    };

    // The Debug View slider: one number for every effect's debug view, so a view is one move in the
    // options window instead of an ini edit and F11. 0 leaves the ini's own debug values alone. The panel
    // cannot draw a dropdown (it builds a page from a table of tick boxes and sliders), so the names are
    // in the slider's tooltip and in the log.
    struct DebugViewInfo { const char* name; int volume, sunShadows, lamps, rays; };
    const DebugViewInfo kDebugViews[] = {
        { "off",                                              0, 0, 0, 0 },
        { "volumetric light: the glow alone",                 1, 0, 0, 0 },
        { "volumetric light: share of each line in sun",      5, 0, 0, 0 },
        { "volumetric light: the shadow map",                 6, 0, 0, 0 },
        { "volumetric light: the depth it reads",             3, 0, 0, 0 },
        { "sun shadows: the shade alone",                     0, 1, 0, 0 },
        { "lamps: the glow alone",                            0, 0, 1, 0 },
        { "lamps: the distance read",                         0, 0, 2, 0 },
        { "lamps: the light on surfaces alone",               0, 0, 3, 0 },
        { "sun rays: the mask",                               0, 0, 0, 1 },
        { "sun rays: the rays alone",                         0, 0, 0, 2 },
        { "sun rays: the sky kept before the clouds",         0, 0, 0, 3 },
    };
    constexpr int kDebugViewCount = sizeof(kDebugViews) / sizeof(kDebugViews[0]);
    int g_debugViewLogged = -1;

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
    void*    g_tune     = nullptr;  // comfyTune, the /atmos command
    char     g_tuneLast[256] = {};
    std::deque<std::string> g_tuneText;   // names and values given to Register, kept for good

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
        case kNightStrength:  snprintf(out, cap, "%.0f", s.night.strength); break;
        case kRaysSoften:     snprintf(out, cap, "%.0f", s.rays.soften); break;
        case kRaysSmooth:     snprintf(out, cap, "%.0f", s.rays.smooth * 100.0f); break;   // a percentage
        case kDebugView:      snprintf(out, cap, "0"); break;                                // never from the ini
        // 1, 2 or 3: 1024, 2048 or 4096 texels a side.
        case kShadowResolution: snprintf(out, cap, "%d", s.shadow.size <= 1024 ? 1 : s.shadow.size <= 2048 ? 2 : 3);
                              break;
        case kShadowSoftness: snprintf(out, cap, "%.0f", s.sunShadows.softness); break;
        case kShadowEvery:    snprintf(out, cap, "%d", s.shadow.mapEvery); break;
        case kSunShadows:     snprintf(out, cap, "%d", s.sunShadows.enabled ? 1 : 0); break;
        case kSunShadowStrength: snprintf(out, cap, "%.0f", s.sunShadows.strength); break;
        // Yards; the other three in percent.
        case kFogHeight:      snprintf(out, cap, "%.0f", s.fog.height); break;
        case kFogFade:        snprintf(out, cap, "%.0f", s.fog.cover * 100.0f); break;
        case kFogDarkness:    snprintf(out, cap, "%.0f", s.fog.darken * 100.0f); break;
        case kFogGreyness:    snprintf(out, cap, "%.0f", s.fog.desaturate * 100.0f); break;
        case kMaster:         snprintf(out, cap, "%d", s.master ? 1 : 0); break;
        case kFogHaze:        snprintf(out, cap, "%.0f", s.fog.density / 0.0002f); break;   // 100 = 0.02
        case kSunlight:       snprintf(out, cap, "%.0f", s.sunShadows.sunlight * 100.0f); break;   // percent
        // Thousandths: density 0.015 is 15, anisotropy 0.025 is 25. Distance in yards.
        case kVolumeDensity:  snprintf(out, cap, "%.0f", s.volume.density * 1000.0f); break;
        case kVolumeDistance: snprintf(out, cap, "%.0f", s.volume.maxDistance); break;
        case kVolumeDirection: snprintf(out, cap, "%.0f", s.volume.anisotropy * 1000.0f); break;
        case kShadowsWorld:   snprintf(out, cap, "%d", s.sunShadows.world ? 1 : 0); break;
        case kShadowsUnits:   snprintf(out, cap, "%d", s.sunShadows.units ? 1 : 0); break;
        case kShadowsLock:    snprintf(out, cap, "%d", s.sunShadows.lock ? 1 : 0); break;
        case kShadowsTilt:    snprintf(out, cap, "%.0f", s.sunShadows.lockTilt); break;
        // Percentages of the 0..1 values.
        case kShadeTint:      snprintf(out, cap, "%.0f", s.sunShadows.shadeTint * 100.0f); break;
        case kSunTint:        snprintf(out, cap, "%.0f", s.sunShadows.sunTint * 100.0f); break;
        case kFogSunGlow:     snprintf(out, cap, "%.0f", s.fog.sunGlow * 100.0f); break;
        case kFogSunBright:   snprintf(out, cap, "%.0f", s.fog.sunBright * 100.0f); break;
        case kFogReach:       snprintf(out, cap, "%.0f", s.fog.reach * 100.0f); break;       // percent
        case kFogNear:        snprintf(out, cap, "%.0f", s.fog.haze * 100.0f); break;
        case kFogFar:         snprintf(out, cap, "%.0f", s.fog.distance * 100.0f); break;
        case kLampGlow:       snprintf(out, cap, "%.0f", s.lamps.strength); break;
        case kLampDistance:   snprintf(out, cap, "%.0f", s.lamps.fogReach * 100.0f); break;
        case kNightDarkness:  snprintf(out, cap, "%.0f", s.night.darkness * 100.0f); break;
        case kMoonlight:      snprintf(out, cap, "%.0f", s.night.tint * 100.0f); break;
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
        if (c[kNightStrength].seen)  s.night.strength  = Clamp(c[kNightStrength].value, 0.0f, 100.0f);
        if (c[kRaysSoften].seen)     s.rays.soften     = Clamp(c[kRaysSoften].value, 0.0f, 16.0f);
        if (c[kRaysSmooth].seen)     s.rays.smooth     = Clamp(c[kRaysSmooth].value * 0.01f, 0.0f, 0.9f);
        if (c[kShadowResolution].seen)
            s.shadow.size = 512 << static_cast<int>(Clamp(c[kShadowResolution].value, 1.0f, 3.0f) + 0.5f);
        if (c[kShadowSoftness].seen) s.sunShadows.softness = Clamp(c[kShadowSoftness].value, 0.0f, 8.0f);
        if (c[kShadowEvery].seen)    s.shadow.mapEvery = static_cast<int>(Clamp(c[kShadowEvery].value, 1.0f, 8.0f) + 0.5f);
        if (c[kSunShadows].seen)     s.sunShadows.enabled = c[kSunShadows].value != 0.0f;
        if (c[kSunShadowStrength].seen) s.sunShadows.strength = Clamp(c[kSunShadowStrength].value, 0.0f, 100.0f);
        if (c[kFogHeight].seen)      s.fog.height      = Clamp(c[kFogHeight].value, 1.0f, 2000.0f);
        if (c[kFogFade].seen)        s.fog.cover       = Clamp(c[kFogFade].value * 0.01f, 0.0f, 1.0f);
        if (c[kFogDarkness].seen)    s.fog.darken      = Clamp(c[kFogDarkness].value * 0.01f, 0.0f, 1.0f);
        if (c[kFogGreyness].seen)    s.fog.desaturate  = Clamp(c[kFogGreyness].value * 0.01f, 0.0f, 1.0f);
        if (c[kMaster].seen)         s.master          = c[kMaster].value != 0.0f;
        if (c[kFogHaze].seen)        s.fog.density     = Clamp(c[kFogHaze].value, 0.0f, 100.0f) * 0.0002f;
        if (c[kSunlight].seen)       s.sunShadows.sunlight = Clamp(c[kSunlight].value * 0.01f, 0.0f, 0.5f);
        if (c[kVolumeDensity].seen)  s.volume.density  = Clamp(c[kVolumeDensity].value * 0.001f, 0.0f, 0.05f);
        if (c[kVolumeDistance].seen) s.volume.maxDistance = Clamp(c[kVolumeDistance].value, 20.0f, 1000.0f);
        if (c[kVolumeDirection].seen) s.volume.anisotropy = Clamp(c[kVolumeDirection].value * 0.001f, 0.0f, 0.95f);
        if (c[kShadowsWorld].seen)   s.sunShadows.world = c[kShadowsWorld].value != 0.0f;
        if (c[kShadowsUnits].seen)   s.sunShadows.units = c[kShadowsUnits].value != 0.0f;
        if (c[kShadowsLock].seen)    s.sunShadows.lock  = c[kShadowsLock].value != 0.0f;
        if (c[kShadowsTilt].seen)    s.sunShadows.lockTilt = Clamp(c[kShadowsTilt].value, 0.0f, 80.0f);
        if (c[kShadeTint].seen)      s.sunShadows.shadeTint = Clamp(c[kShadeTint].value * 0.01f, 0.0f, 1.0f);
        if (c[kSunTint].seen)        s.sunShadows.sunTint   = Clamp(c[kSunTint].value * 0.01f, 0.0f, 1.0f);
        if (c[kFogSunGlow].seen)     s.fog.sunGlow   = Clamp(c[kFogSunGlow].value * 0.01f, 0.0f, 1.0f);
        if (c[kFogSunBright].seen)   s.fog.sunBright = Clamp(c[kFogSunBright].value * 0.01f, 0.0f, 2.0f);
        if (c[kFogReach].seen)       s.fog.reach     = Clamp(c[kFogReach].value * 0.01f, 0.05f, 2.0f);
        if (c[kFogNear].seen)        s.fog.haze      = Clamp(c[kFogNear].value * 0.01f, 0.0f, 0.9f);
        if (c[kFogFar].seen)         s.fog.distance  = Clamp(c[kFogFar].value * 0.01f, 0.0f, 1.0f);
        if (c[kLampGlow].seen)       s.lamps.strength = Clamp(c[kLampGlow].value, 0.0f, 100.0f);
        if (c[kLampDistance].seen)   s.lamps.fogReach = Clamp(c[kLampDistance].value * 0.01f, 0.5f, 4.0f);
        if (c[kNightDarkness].seen)  s.night.darkness = Clamp(c[kNightDarkness].value * 0.01f, 0.0f, 0.9f);
        if (c[kMoonlight].seen)      s.night.tint     = Clamp(c[kMoonlight].value * 0.01f, 0.0f, 1.0f);
        if (c[kDebugView].seen)
        {
            const int v = static_cast<int>(Clamp(c[kDebugView].value, 0.0f, kDebugViewCount - 1.0f) + 0.5f);
            if (v > 0)
            {
                const DebugViewInfo& d = kDebugViews[v];
                s.volume.debug     = d.volume;
                s.sunShadows.debug = d.sunShadows;
                s.lamps.debug      = d.lamps;
                s.rays.debugView   = d.rays;
            }
            if (v != g_debugViewLogged)
            {
                g_debugViewLogged = v;
                Log("--- debug view %d: %s ---", v, v > 0 ? kDebugViews[v].name : "off (the ini's debug values apply)");
            }
        }
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

        // The Atmosphere Effects box: off, every effect is off, whatever its own box says. The depth
        // buffer and the shadow map go too, since nothing reads them. The fog colour checks it itself.
        if (!s.master)
        {
            s.fog.enabled = s.volume.enabled = s.depth.enabled = s.shadow.enabled = false;
            s.rays.enabled = s.sunShadows.enabled = s.lamps.enabled = false;
        }
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
        g_tune = lookup("comfyTune");
        if (!g_tune)
            g_tune = registerFn("comfyTune", nullptr, 0, "", nullptr, kCategory, 0, nullptr);
        if (!g_tune)
            Log("could not register CVar comfyTune: no /atmos");
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

    // /atmos: a new "<number> <command>" in comfyTune.
    char tune[256];
    DWORD tstr = 0;
    if (g_tune && SafeCopy(reinterpret_cast<uintptr_t>(g_tune) + 0x20, &tstr, 4) && tstr &&
        SafeString(tstr, tune, sizeof(tune)) && strcmp(tune, g_tuneLast) != 0)
    {
        strcpy_s(g_tuneLast, tune);
        char* rest = nullptr;
        const unsigned long seq = strtoul(tune, &rest, 10);
        if (seq && rest && (*rest == ' ' || !*rest))
        {
            bool reloaded = false;
            const std::vector<std::string> lines = TuneRun(*rest ? rest + 1 : "", reloaded);
            if (reloaded)
            {
                CVarsAfterLoad();
                changed = true;
            }
            const auto registerFn = reinterpret_cast<RegisterFn>(kRegister + Slide());
            char name[64];
            for (size_t i = 0; i < lines.size(); ++i)
            {
                snprintf(name, sizeof(name), "comfyTuneReply%lu_%u", seq, static_cast<unsigned>(i + 1));
                g_tuneText.push_back(name);
                const char* n = g_tuneText.back().c_str();
                g_tuneText.push_back(lines[i]);
                registerFn(n, nullptr, 0, g_tuneText.back().c_str(), nullptr, kCategory, 0, nullptr);
            }
            snprintf(name, sizeof(name), "comfyTuneReply%lu", seq);
            g_tuneText.push_back(name);
            const char* n = g_tuneText.back().c_str();
            g_tuneText.push_back(std::to_string(lines.size()));
            registerFn(n, nullptr, 0, g_tuneText.back().c_str(), nullptr, kCategory, 0, nullptr);
        }
    }

    if (changed)
    {
        g_cfg = g_base;
        Apply(g_cfg);
    }
    return changed;
}
