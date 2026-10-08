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
// Notices (2026-09-30), for a line in chat the DLL starts on its own, such as the benchmark's start and end:
// the same way, comfyNotice1, comfyNotice2 and on, one line each. The addon checks for the next one twice a
// second, and at load skips those already there, which a /reload would otherwise print again.
//
// Stats (2026-09-30): /atmos stats shows a few lines of figures on screen. comfyStats is registered with a
// default of kStatsLen spaces, and the DLL writes its text into that string in place, padded with spaces to
// the same length, so the client's buffer is never outgrown and never replaced. Only while its length is
// still kStatsLen: had anything set the CVar, the string would be another, and it is left alone.
//
// The time of day (2026-10-07): the debug panel's slider and the key bindings send "<number> <verb> [value]" in
// comfyTimeSet, a new number each time, so the same verb twice runs twice. The verbs: set <hour>, step <steps>
// (in [time] step hours), daynight, save. The first value read is left alone: it is what Config.wtf kept from the
// last session. comfyTimeShown carries "<hour> <state>" back (TimeState), written in place as comfyStats is, and
// only while it still holds kTimeLen characters. CVarsTime runs every frame, not every 0.2 s as the controls do: a
// held key steps the time every 0.03 s.
//
// All of this runs on the client's main thread (Present is called from the client's render), the same
// thread Lua runs on, so nothing here races the options panel.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include "cvars.h"
#include "common.h"
#include "config.h"
#include "timeofday.h"
#include "tune.h"

#include <deque>
#include <string>

#include <cmath>
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

    enum Knob { kVolume, kVolumeStrength, kClouds, kRays, kRaysStrength, kVolumeQuality,
                kNightStrength, kRaysSoften, kRaysSmooth, kDebugView, kShadowResolution, kShadowSoftness,
                kShadowEvery, kSunShadows, kSunShadowStrength, kMaster, kSunlight, kVolumeDensity,
                kVolumeDistance, kVolumeDirection, kShadowsWorld, kShadowsUnits, kShadowsLock, kShadowsTilt,
                kShadeTint, kSunTint, kLampGlow, kLampDistance, kNightDarkness, kMoonlight, kFog, kFogDensity,
                kFogHeight, kFogBrightness, kFogSun, kFogPatches, kFogWind, kFogWindDir, kFogReach,
                kFogSky, kFogLow, kFogWater, kFogMorning, kFogLamps, kShadowsNight, kShadowsUnitStrength, kSunGlide, kLamps, kTorchLight, kLanternLight, kIndoorLamps, kLampsDay,
                kShadowsBody, kShadowNear, kTreeShade, kWaveHeight, kWaveSize, kWater, kWaterColour, kWaterClarity, kWaterReflect, kWaterBend, kWaterCover, kWaterWake, kWaterFoam, kWaterSwash, kWaterGlint, kWaterMoonGlint, kWaterGlintSize, kWaterEdge, kWaterEdgeWidth, kWaterBright, kWaterRippleDepth, kWaterSwashHeight, kWaterSwashLength, kWaterSwashSpeed, kDaySaturation, kNightSaturation, kColour, kWaterRippleMoving, kWaterSpread, kWaterSpreadMoving, kFoamDrawn, kFoamSize, kFoamReach, kOpenFoam, kOpenFoamAmount, kFoamEdge, kWakeFoam, kObjectFoam, kObjectFoamWidth, kLakeSwash, kLakeFoam, kLakeWaves, kRainOnWater, kLighthouses, kLighthouseBeam, kLhBeacon, kLhLength, kLhWidth, kLhSpread, kLhSpeed, kLhTwo, kLhGlint, kLhFace, kLhTilt, kLhSoft, kLhWaterWidth, kRainDarkness, kShipWake, kShipForward, kShipDepth, kShadowWater, kWetSand, kLhReach,
                kGrass, kGrassWind, kGrassSpeed, kGrassLean, kGrassWave, kGrassWindDir, kGrassParting, kGrassRadius,
                kHotkeys,
                kKnobs };

    const char* const kNames[kKnobs] = {
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
        "comfyAtmosphere",
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
        "comfyLampGlow",
        "comfyLampDistance",
        "comfyNightDarkness",
        "comfyMoonlight",
        // New names for the new fog (2026-09-30): Config.wtf may still hold the old fog's comfyFog values.
        "comfyMist",
        "comfyMistDensity",
        "comfyMistHeight",
        "comfyMistBrightness",
        "comfyMistSun",
        "comfyMistPatches",
        "comfyMistWind",
        "comfyMistWindDir",
        "comfyMistReach",
        "comfyMistSky",
        "comfyMistLow",
        "comfyMistWater",
        "comfyMistMorning",
        "comfyMistLamps",
        "comfySunShadowsNight",
        "comfySunShadowsUnitStrength",
        "comfySunGlide",
        "comfyLamps",
        "comfyTorchLight",
        "comfyLanternLight",
        "comfyIndoorLamps",
        "comfyLampsDay",
        "comfySunShadowsBody",
        "comfyShadowNear",
        "comfyTreeShade",
        "comfyWaveHeight",
        "comfyWaveSize",
        "comfyWater",
        "comfyWaterColour",
        "comfyWaterClarity",
        "comfyWaterReflect",
        "comfyWaterBend",
        "comfyWaterCover",
        "comfyWaterWake",
        "comfyWaterFoam",
        "comfyWaterSwash",
        "comfyWaterGlint",
        "comfyWaterMoonGlint",
        "comfyWaterGlintSize",
        "comfyWaterEdge",
        "comfyWaterEdgeWidth",
        "comfyWaterBright",
        "comfyWaterRippleDepth",
        "comfyWaterSwashHeight",
        "comfyWaterSwashLength",
        "comfyWaterSwashSpeed",
        "comfyDaySaturation",
        "comfyNightSaturation",
        "comfyColor",
        "comfyWaterRippleMoving",
        "comfyWaterRippleSpread",
        "comfyWaterRippleSpreadMoving",
        "comfyWaterFoamDrawn",
        "comfyWaterFoamSize",
        "comfyWaterFoamReach",
        "comfyWaterOpenFoam",
        "comfyWaterOpenFoamAmount",
        "comfyWaterFoamEdge",
        "comfyWaterWakeFoam",
        "comfyWaterObjectFoam",
        "comfyWaterObjectFoamWidth",
        "comfyWaterLakeSwash",
        "comfyWaterLakeFoam",
        "comfyWaterLakeWaves",
        "comfyWaterRain",
        "comfyLighthouses",
        "comfyLighthouseBeam",
        "comfyLighthouseBeacon",
        "comfyLighthouseLength",
        "comfyLighthouseWidth",
        "comfyLighthouseSpread",
        "comfyLighthouseSpeed",
        "comfyLighthouseTwoBeams",
        "comfyLighthouseGlint",
        "comfyLighthouseFace",
        "comfyLighthouseFaceTilt",
        "comfyLighthouseFaceSoft",
        "comfyLighthouseWaterWidth",
        "comfyRainDarkness",
        "comfyWaterShipWake",
        "comfyWaterShipForward",
        "comfyWaterShipDepth",
        "comfyShadowOnWater",
        "comfyWaterWetSand",
        "comfyLighthouseDistance",
        // The grass (2026-10-06, from comfygrass).
        "comfyGrass",
        "comfyGrassWind",
        "comfyGrassSpeed",
        "comfyGrassLean",
        "comfyGrassWaveLength",
        "comfyGrassWindDir",
        "comfyGrassParting",
        "comfyGrassPartingRadius",
        // The debug panel's Developer keys box (2026-10-07): [general] hotkeys.
        "comfyHotkeys",
    };

    // The Debug View slider: one number for every effect's debug view, so a view is one move in the
    // options window instead of an ini edit and F11. 0 leaves the ini's own debug values alone. The panel
    // cannot draw a dropdown (it builds a page from a table of tick boxes and sliders), so the names are
    // in the slider's tooltip and in the log.
    struct DebugViewInfo { const char* name; int volume, sunShadows, lamps, rays, fog, water = 0, grass = 0; };
    const DebugViewInfo kDebugViews[] = {
        { "off",                                              0, 0, 0, 0, 0 },
        { "volumetric light: the glow alone",                 1, 0, 0, 0, 0 },
        { "volumetric light: share of each line in sun",      5, 0, 0, 0, 0 },
        { "volumetric light: the shadow map",                 6, 0, 0, 0, 0 },
        { "volumetric light: the depth it reads",             3, 0, 0, 0, 0 },
        { "sun shadows: the shade alone",                     0, 1, 0, 0, 0 },
        { "lamps: the glow alone",                            0, 0, 1, 0, 0 },
        { "lamps: the distance read",                         0, 0, 2, 0, 0 },
        { "lamps: the light on surfaces alone",               0, 0, 3, 0, 0 },
        { "sun rays: the mask",                               0, 0, 0, 1, 0 },
        { "sun rays: the rays alone",                         0, 0, 0, 2, 0 },
        { "sun rays: the sky kept before the clouds",         0, 0, 0, 3, 0 },
        { "fog: how much gets through (white = clear)",       0, 0, 0, 0, 1 },
        { "fog: the sky's light on it alone",                 0, 0, 0, 0, 2 },
        { "fog: where mist collects (low ground, water)",     7, 0, 0, 0, 0 },
        { "sun shadows: the bodies it finds",                 0, 3, 0, 0, 0 },
        { "water: as drawn, over what lies under it: magenta the far slice, orange untextured draws, cyan terrain past 80 yards", 0, 0, 0, 0, 0, 1 },
        { "water: the foam alone",                            0, 0, 0, 0, 0, 2 },
        { "water: the wet sand alone",                        0, 0, 0, 0, 0, 3 },
        { "water: what lies under it (red: the far terrain, blue: only sky behind)", 0, 0, 0, 0, 0, 4 },
        { "water: ripples (red, green) and the wake (blue)",  0, 0, 0, 0, 0, 5 },
        { "water: the game's own water drawn (red), other liquid (magenta), ours off", 0, 0, 0, 0, 0, 6 },
        { "water: the height over the water, in contours every 0.1 yards (green above, red below, blue the dry cells)", 0, 0, 0, 0, 0, 7 },
        { "water: the slope the swash uses (dark to bright up to 0.3, a line every 0.05)", 0, 0, 0, 0, 0, 8 },
        { "sun shadows: where the shade comes from (red solid, green leaves, blue hills)", 0, 4, 0, 0, 0 },
        { "sun shadows: shade taken off behind a hill (red solid, green leaves, blue where the check runs)", 0, 5, 0, 0, 0 },
        { "sun shadows: the hill check's depths (red the solid caster before the hill, green behind it, blue the point behind the hill; 15 yards full at terrainBias 1.5)", 0, 6, 0, 0, 0 },
        { "water: the drawn foam (white) over its age (red), the open water's foam (blue)", 0, 0, 0, 0, 0, 9 },
        { "sun shadows: the water it finds (blue: the water's depth over a bed, grey: none, so the shade falls on what the depth shows)", 0, 7, 0, 0, 0 },
        { "water: the depth under it (0 to 256 yards, blue to red, a line at 1, 2, 4, 8 ... 256; hatched: the map's depth)", 0, 0, 0, 0, 0, 10 },
        { "grass: the bend (black still, white the tips; blue where it comes from the texture)", 0, 0, 0, 0, 0, 0, 1 },
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
    std::deque<std::string> g_notices;    // lines for chat, not yet registered
    constexpr size_t kStatsLen = 1000;   // 600 until 2026-10-07: the indoor pair made it longer; the addon sets as many
    void*    g_stats = nullptr;             // comfyStats
    constexpr size_t kTimeLen = 16;
    void*    g_timeSet   = nullptr;         // comfyTimeSet
    void*    g_timeShown = nullptr;         // comfyTimeShown
    char     g_timeSetLast[64] = {};
    bool     g_timeSetSeen = false;
    char     g_timeShownLast[kTimeLen + 1] = {};
    unsigned long g_noticeSeq = 0;

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
        case kShadowNear:     snprintf(out, cap, "%.0f", s.shadow.nearRange); break;   // yards
        case kSunShadows:     snprintf(out, cap, "%d", s.sunShadows.enabled ? 1 : 0); break;
        case kSunShadowStrength: snprintf(out, cap, "%.0f", s.sunShadows.strength); break;
        case kMaster:         snprintf(out, cap, "%d", s.master ? 1 : 0); break;
        case kSunlight:       snprintf(out, cap, "%.0f", s.sunShadows.sunlight * 100.0f); break;   // percent
        case kTreeShade:      snprintf(out, cap, "%.0f", s.sunShadows.leafShade * 100.0f); break;  // percent
        case kWaveHeight:     snprintf(out, cap, "%.0f", s.water.waveHeight * 10.0f); break;       // tenths of a yard
        case kWaveSize:       snprintf(out, cap, "%.0f", s.water.waveScale * 100.0f); break;       // percent
        case kWater:          snprintf(out, cap, "%d", s.water.enabled ? 1 : 0); break;
        case kWaterColour:    snprintf(out, cap, "%.0f", s.water.colour); break;
        case kWaterClarity:   snprintf(out, cap, "%.0f", s.water.clarity * 100.0f); break;     // percent
        case kWaterReflect:   snprintf(out, cap, "%.0f", s.water.reflection * 100.0f); break;  // percent
        case kWaterBend:      snprintf(out, cap, "%.0f", s.water.refraction * 200.0f); break;  // 100 = half a yard
        case kWaterCover:     snprintf(out, cap, "%.0f", s.water.cover * 100.0f); break;       // percent
        case kWaterWake:      snprintf(out, cap, "%.0f", s.water.wake * 100.0f); break;        // percent
        case kWaterFoam:      snprintf(out, cap, "%.0f", s.water.foam * 100.0f); break;        // percent
        case kWaterSwash:     snprintf(out, cap, "%d", s.water.swash > 0.0f ? 1 : 0); break;    // on or off
        case kWaterGlint:     snprintf(out, cap, "%.0f", s.water.glint * 100.0f); break;       // percent
        case kWaterMoonGlint: snprintf(out, cap, "%.0f", s.water.moonGlint * 100.0f); break;   // percent
        case kWaterGlintSize: snprintf(out, cap, "%.0f", s.water.glintSize * 100.0f); break;   // percent
        case kWaterEdge:      snprintf(out, cap, "%.0f", s.water.edgeLine * 100.0f); break;    // percent
        case kWaterEdgeWidth: snprintf(out, cap, "%.0f", s.water.edgeWidth * 10.0f); break;    // tenths of a yard
        case kWaterBright:    snprintf(out, cap, "%.0f", s.water.brightness * 100.0f); break;  // percent
        case kWaterRippleDepth: snprintf(out, cap, "%.0f", s.water.rippleDepth * 100.0f); break; // percent
        case kWaterRippleMoving: snprintf(out, cap, "%.0f", s.water.rippleDepthMoving * 100.0f); break;
        case kWaterSpread:    snprintf(out, cap, "%.0f", s.water.rippleSpread * 100.0f); break;        // percent
        case kWaterSpreadMoving: snprintf(out, cap, "%.0f", s.water.rippleSpreadMoving * 100.0f); break;
        case kFoamDrawn:      snprintf(out, cap, "%d", s.water.foamDrawn ? 1 : 0); break;
        case kFoamSize:       snprintf(out, cap, "%.0f", s.water.foamCell * 10.0f); break;      // tenths of a yard
        case kFoamReach:      snprintf(out, cap, "%.0f", s.water.foamLife * 10.0f); break;      // tenths of a yard
        case kOpenFoam:       snprintf(out, cap, "%d", s.water.openFoam ? 1 : 0); break;
        case kOpenFoamAmount: snprintf(out, cap, "%.0f", s.water.whitecaps * 100.0f); break;    // percent
        case kFoamEdge:       snprintf(out, cap, "%.0f", s.water.foamEdge * 100.0f); break;     // percent
        case kWakeFoam:       snprintf(out, cap, "%.0f", s.water.wakeFoam * 100.0f); break;     // percent
        case kObjectFoam:     snprintf(out, cap, "%.0f", s.water.objectFoam * 100.0f); break;   // percent
        case kObjectFoamWidth: snprintf(out, cap, "%.0f", s.water.objectFoamWidth * 10.0f); break; // tenths of a yard
        case kLakeSwash:      snprintf(out, cap, "%.0f", s.water.lakeSwash * 100.0f); break;    // percent
        case kLakeFoam:       snprintf(out, cap, "%.0f", s.water.lakeFoam * 100.0f); break;     // percent
        case kLakeWaves:      snprintf(out, cap, "%.0f", s.water.lakeWaves * 100.0f); break;    // percent
        case kRainOnWater:    snprintf(out, cap, "%.0f", s.water.rain * 100.0f); break;         // percent
        case kLighthouses:    snprintf(out, cap, "%d", s.lighthouse.enabled ? 1 : 0); break;
        case kLighthouseBeam: snprintf(out, cap, "%.0f", s.lighthouse.beam * 100.0f); break;    // percent
        case kLhBeacon:       snprintf(out, cap, "%.0f", s.lighthouse.beacon * 100.0f); break;  // percent
        case kLhLength:       snprintf(out, cap, "%.0f", s.lighthouse.beamLength); break;       // yards
        case kLhWidth:        snprintf(out, cap, "%.0f", s.lighthouse.beamWidth * 10.0f); break; // tenths of a yard
        case kLhSpread:       snprintf(out, cap, "%.0f", s.lighthouse.beamSpread * 1000.0f); break; // thousandths
        case kLhSpeed:        snprintf(out, cap, "%.0f", s.lighthouse.beamSpeed); break;        // seconds a turn
        case kLhTwo:          snprintf(out, cap, "%d", s.lighthouse.beamCount >= 2 ? 1 : 0); break;
        case kLhGlint:        snprintf(out, cap, "%.0f", s.lighthouse.glint * 100.0f); break;        // percent
        case kLhFace:         snprintf(out, cap, "%.0f", s.lighthouse.faceStrength * 100.0f); break; // percent
        case kLhTilt:         snprintf(out, cap, "%.0f", s.lighthouse.faceTilt * 1000.0f); break;    // thousandths
        case kLhSoft:         snprintf(out, cap, "%.0f", s.lighthouse.faceSoft * 1000.0f); break;    // thousandths
        case kLhWaterWidth:   snprintf(out, cap, "%.0f", s.lighthouse.waterWidth * 100.0f); break;   // percent
        case kRainDarkness:   snprintf(out, cap, "%.0f", s.night.rain * 100.0f); break;   // percent
        case kShipWake:       snprintf(out, cap, "%.0f", s.water.shipWake); break;
        case kShipForward:    snprintf(out, cap, "%.0f", s.water.shipWakeForward); break;
        case kShipDepth:      snprintf(out, cap, "%.0f", s.water.shipWakeDepth * 100.0f); break;   // percent
        case kShadowWater:    snprintf(out, cap, "%.0f", s.sunShadows.water * 100.0f); break;   // percent
        case kWetSand:        snprintf(out, cap, "%.0f", s.water.wetSand * 100.0f); break;     // percent
        case kLhReach:        snprintf(out, cap, "%.0f", s.lighthouse.reach); break;          // yards
        case kGrass:          snprintf(out, cap, "%d", s.grass.enabled ? 1 : 0); break;
        case kGrassWind:      snprintf(out, cap, "%.0f", s.grass.scale * 100.0f); break;         // percent
        case kGrassSpeed:     snprintf(out, cap, "%.0f", s.grass.speed * 10.0f); break;          // tenths of a yard a second
        case kGrassLean:      snprintf(out, cap, "%.0f", s.grass.lean * 100.0f); break;          // percent
        case kGrassWave:      snprintf(out, cap, "%.0f", s.grass.wavelength); break;             // yards
        case kGrassWindDir:   snprintf(out, cap, "%.0f", fmodf(fmodf(s.grass.directionDeg, 360.0f) + 360.0f, 360.0f)); break;
        case kGrassParting:   snprintf(out, cap, "%.0f", s.grass.forceCenter * 100.0f); break;   // percent
        case kGrassRadius:    snprintf(out, cap, "%.0f", s.grass.radius * 10.0f); break;         // tenths of a yard
        case kHotkeys:        snprintf(out, cap, "%d", s.hotkeys ? 1 : 0); break;
        case kWaterSwashHeight: snprintf(out, cap, "%.0f", s.water.swashHeight * 100.0f); break; // hundredths of a yard
        case kWaterSwashLength: snprintf(out, cap, "%.0f", s.water.swashLength); break;          // yards
        case kWaterSwashSpeed:  snprintf(out, cap, "%.0f", s.water.swashSpeed * 100.0f); break;  // percent
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
        case kLampGlow:       snprintf(out, cap, "%.0f", s.lamps.strength); break;
        case kLampDistance:   snprintf(out, cap, "%.0f", s.lamps.fogReach * 100.0f); break;
        case kNightDarkness:  snprintf(out, cap, "%.0f", s.night.darkness * 100.0f); break;
        case kMoonlight:      snprintf(out, cap, "%.0f", s.night.tint * 100.0f); break;
        case kDaySaturation:  snprintf(out, cap, "%.0f", s.colour.day); break;      // percent already
        case kNightSaturation: snprintf(out, cap, "%.0f", s.colour.night); break;
        case kColour:         snprintf(out, cap, "%d", s.colour.enabled ? 1 : 0); break;
        case kFog:            snprintf(out, cap, "%d", s.fog.enabled ? 1 : 0); break;
        // Ten-thousandths a yard (40 is 0.004), yards, and percentages.
        case kFogDensity:     snprintf(out, cap, "%.0f", s.fog.density * 10000.0f); break;
        case kFogHeight:      snprintf(out, cap, "%.0f", s.fog.height); break;
        case kFogBrightness:  snprintf(out, cap, "%.0f", s.fog.brightness * 100.0f); break;
        case kFogSun:         snprintf(out, cap, "%.0f", s.fog.sunLight * 10.0f); break;
        case kFogLow:         snprintf(out, cap, "%.0f", s.fog.lowGround * 100.0f); break;   // percent
        case kFogWater:       snprintf(out, cap, "%.0f", s.fog.water * 100.0f); break;
        case kFogMorning:     snprintf(out, cap, "%.0f", s.fog.morning * 100.0f); break;
        case kFogLamps:       snprintf(out, cap, "%.0f", s.fog.lampMist * 100.0f); break;
        case kShadowsNight:   snprintf(out, cap, "%.0f", s.sunShadows.night); break;
        case kShadowsUnitStrength: snprintf(out, cap, "%.0f", s.sunShadows.unitStrength); break;
        case kSunGlide:       snprintf(out, cap, "%.0f", s.sun.glide); break;            // seconds
        case kLamps:          snprintf(out, cap, "%d", s.lamps.enabled ? 1 : 0); break;
        case kTorchLight:     snprintf(out, cap, "%.0f", s.lamps.torchLight * 100.0f); break;     // percent
        case kLanternLight:   snprintf(out, cap, "%.0f", s.lamps.lanternLight * 100.0f); break;
        case kIndoorLamps:    snprintf(out, cap, "%.0f", s.lamps.indoors * 100.0f); break;
        case kLampsDay:       snprintf(out, cap, "%.0f", s.lamps.day); break;               // percent already
        case kShadowsBody:    snprintf(out, cap, "%.0f", s.sunShadows.bodyShade); break;    // percent already
        case kFogReach:       snprintf(out, cap, "%.0f", s.fog.reach); break;         // yards
        case kFogSky:         snprintf(out, cap, "%.0f", s.fog.skyDistance); break;
        case kFogPatches:     snprintf(out, cap, "%.0f", s.fog.patchiness * 100.0f); break;
        case kFogWind:        snprintf(out, cap, "%.0f", s.fog.windSpeed * 10.0f); break;   // tenths of a yard a second
        case kFogWindDir:     snprintf(out, cap, "%.0f", fmodf(fmodf(s.fog.windDeg, 360.0f) + 360.0f, 360.0f)); break;
        }
    }

    // The controls laid over a copy of the ini settings. Only a control that has been read counts.
    void Apply(Settings& s)
    {
        const Slot* c = g_slots;
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
        if (c[kShadowNear].seen)     s.shadow.nearRange = Clamp(c[kShadowNear].value, 16.0f, 128.0f);
        if (c[kSunShadows].seen)     s.sunShadows.enabled = c[kSunShadows].value != 0.0f;
        if (c[kSunShadowStrength].seen) s.sunShadows.strength = Clamp(c[kSunShadowStrength].value, 0.0f, 100.0f);
        if (c[kMaster].seen)         s.master          = c[kMaster].value != 0.0f;
        if (c[kSunlight].seen)       s.sunShadows.sunlight = Clamp(c[kSunlight].value * 0.01f, 0.0f, 0.5f);
        if (c[kTreeShade].seen)      s.sunShadows.leafShade = Clamp(c[kTreeShade].value * 0.01f, 0.0f, 1.0f);
        if (c[kWaveHeight].seen)     s.water.waveHeight = Clamp(c[kWaveHeight].value * 0.1f, 0.0f, 3.0f);
        if (c[kWaveSize].seen)       s.water.waveScale = Clamp(c[kWaveSize].value * 0.01f, 0.25f, 4.0f);
        if (c[kWater].seen)          s.water.enabled   = c[kWater].value != 0.0f;
        if (c[kWaterColour].seen)    s.water.colour    = Clamp(c[kWaterColour].value, 0.0f, 100.0f);
        if (c[kWaterClarity].seen)   s.water.clarity   = Clamp(c[kWaterClarity].value * 0.01f, 0.1f, 10.0f);
        if (c[kWaterReflect].seen)   s.water.reflection = Clamp(c[kWaterReflect].value * 0.01f, 0.0f, 1.0f);
        if (c[kWaterBend].seen)      s.water.refraction = Clamp(c[kWaterBend].value * 0.005f, 0.0f, 0.5f);
        if (c[kWaterCover].seen)     s.water.cover     = Clamp(c[kWaterCover].value * 0.01f, 0.0f, 1.0f);
        if (c[kWaterWake].seen)      s.water.wake      = Clamp(c[kWaterWake].value * 0.01f, 0.0f, 1.0f);
        if (c[kWaterFoam].seen)      s.water.foam      = Clamp(c[kWaterFoam].value * 0.01f, 0.0f, 1.0f);
        if (c[kWaterSwash].seen)     s.water.swash     = c[kWaterSwash].value > 0.0f ? 1.0f : 0.0f;
        if (c[kWaterGlint].seen)     s.water.glint     = Clamp(c[kWaterGlint].value * 0.01f, 0.0f, 3.0f);
        if (c[kWaterMoonGlint].seen) s.water.moonGlint = Clamp(c[kWaterMoonGlint].value * 0.01f, 0.0f, 3.0f);
        if (c[kWaterGlintSize].seen) s.water.glintSize = Clamp(c[kWaterGlintSize].value * 0.01f, 0.25f, 4.0f);
        if (c[kWaterEdge].seen)      s.water.edgeLine  = Clamp(c[kWaterEdge].value * 0.01f, 0.0f, 1.0f);
        if (c[kWaterEdgeWidth].seen) s.water.edgeWidth = Clamp(c[kWaterEdgeWidth].value * 0.1f, 0.05f, 3.0f);
        if (c[kWaterBright].seen)    s.water.brightness = Clamp(c[kWaterBright].value * 0.01f, 0.25f, 2.0f);
        if (c[kWaterRippleDepth].seen) s.water.rippleDepth = Clamp(c[kWaterRippleDepth].value * 0.01f, 0.0f, 4.0f);
        if (c[kWaterRippleMoving].seen) s.water.rippleDepthMoving = Clamp(c[kWaterRippleMoving].value * 0.01f, 0.0f, 4.0f);
        if (c[kWaterSpread].seen)    s.water.rippleSpread = Clamp(c[kWaterSpread].value * 0.01f, 0.1f, 3.0f);
        if (c[kWaterSpreadMoving].seen) s.water.rippleSpreadMoving = Clamp(c[kWaterSpreadMoving].value * 0.01f, 0.1f, 3.0f);
        if (c[kFoamDrawn].seen)      s.water.foamDrawn = c[kFoamDrawn].value != 0.0f;
        if (c[kFoamSize].seen)       s.water.foamCell  = Clamp(c[kFoamSize].value * 0.1f, 0.2f, 4.0f);
        if (c[kFoamReach].seen)      s.water.foamLife  = Clamp(c[kFoamReach].value * 0.1f, 0.5f, 10.0f);
        if (c[kOpenFoam].seen)       s.water.openFoam  = c[kOpenFoam].value != 0.0f;
        if (c[kOpenFoamAmount].seen) s.water.whitecaps = Clamp(c[kOpenFoamAmount].value * 0.01f, 0.0f, 1.0f);
        if (c[kFoamEdge].seen)       s.water.foamEdge  = Clamp(c[kFoamEdge].value * 0.01f, 0.0f, 0.9f);
        if (c[kWakeFoam].seen)       s.water.wakeFoam  = Clamp(c[kWakeFoam].value * 0.01f, 0.0f, 1.0f);
        if (c[kObjectFoam].seen)     s.water.objectFoam = Clamp(c[kObjectFoam].value * 0.01f, 0.0f, 1.0f);
        if (c[kObjectFoamWidth].seen) s.water.objectFoamWidth = Clamp(c[kObjectFoamWidth].value * 0.1f, 0.2f, 3.0f);
        if (c[kLakeSwash].seen)      s.water.lakeSwash = Clamp(c[kLakeSwash].value * 0.01f, 0.0f, 1.0f);
        if (c[kLakeFoam].seen)       s.water.lakeFoam  = Clamp(c[kLakeFoam].value * 0.01f, 0.0f, 1.0f);
        if (c[kLakeWaves].seen)      s.water.lakeWaves = Clamp(c[kLakeWaves].value * 0.01f, 0.0f, 1.0f);
        if (c[kRainOnWater].seen)    s.water.rain      = Clamp(c[kRainOnWater].value * 0.01f, 0.0f, 2.0f);
        if (c[kLighthouses].seen)    s.lighthouse.enabled = c[kLighthouses].value != 0.0f;
        if (c[kLighthouseBeam].seen) s.lighthouse.beam = Clamp(c[kLighthouseBeam].value * 0.01f, 0.0f, 1.0f);
        if (c[kLhBeacon].seen)       s.lighthouse.beacon = Clamp(c[kLhBeacon].value * 0.01f, 0.0f, 3.0f);
        if (c[kLhLength].seen)       s.lighthouse.beamLength = Clamp(c[kLhLength].value, 10.0f, 2000.0f);
        if (c[kLhWidth].seen)        s.lighthouse.beamWidth = Clamp(c[kLhWidth].value * 0.1f, 0.05f, 10.0f);
        if (c[kLhSpread].seen)       s.lighthouse.beamSpread = Clamp(c[kLhSpread].value * 0.001f, 0.0f, 0.5f);
        if (c[kLhSpeed].seen)        s.lighthouse.beamSpeed = Clamp(c[kLhSpeed].value, 1.0f, 120.0f);
        if (c[kLhTwo].seen)          s.lighthouse.beamCount = c[kLhTwo].value != 0.0f ? 2 : 1;
        if (c[kLhGlint].seen)        s.lighthouse.glint = Clamp(c[kLhGlint].value * 0.01f, 0.0f, 3.0f);
        if (c[kLhFace].seen)         s.lighthouse.faceStrength = Clamp(c[kLhFace].value * 0.01f, 0.0f, 4.0f);
        if (c[kLhTilt].seen)         s.lighthouse.faceTilt = Clamp(c[kLhTilt].value * 0.001f, 0.0f, 0.3f);
        if (c[kLhSoft].seen)         s.lighthouse.faceSoft = Clamp(c[kLhSoft].value * 0.001f, 0.005f, 0.3f);
        if (c[kLhWaterWidth].seen)   s.lighthouse.waterWidth = Clamp(c[kLhWaterWidth].value * 0.01f, 0.25f, 4.0f);
        if (c[kRainDarkness].seen)   s.night.rain = Clamp(c[kRainDarkness].value * 0.01f, 0.0f, 0.9f);
        if (c[kShipWake].seen)       s.water.shipWake = Clamp(c[kShipWake].value, 0.0f, 20.0f);
        if (c[kShipForward].seen)    s.water.shipWakeForward = Clamp(c[kShipForward].value, 0.0f, 60.0f);
        if (c[kShipDepth].seen)      s.water.shipWakeDepth = Clamp(c[kShipDepth].value * 0.01f, 0.0f, 6.0f);
        if (c[kShadowWater].seen)    s.sunShadows.water = Clamp(c[kShadowWater].value * 0.01f, 0.0f, 1.0f);
        if (c[kWetSand].seen)        s.water.wetSand   = Clamp(c[kWetSand].value * 0.01f, 0.0f, 1.0f);
        if (c[kLhReach].seen)        s.lighthouse.reach = Clamp(c[kLhReach].value, 50.0f, 1500.0f);
        if (c[kGrass].seen)          s.grass.enabled = c[kGrass].value != 0.0f;
        if (c[kGrassWind].seen)      s.grass.scale = Clamp(c[kGrassWind].value * 0.01f, 0.0f, 5.0f);
        if (c[kGrassSpeed].seen)     s.grass.speed = Clamp(c[kGrassSpeed].value * 0.1f, 0.0f, 20.0f);
        if (c[kGrassLean].seen)      s.grass.lean = Clamp(c[kGrassLean].value * 0.01f, 0.0f, 2.0f);
        if (c[kGrassWave].seen)      s.grass.wavelength = Clamp(c[kGrassWave].value, 0.5f, 200.0f);
        if (c[kGrassWindDir].seen)   s.grass.directionDeg = Clamp(c[kGrassWindDir].value, 0.0f, 360.0f);
        if (c[kGrassParting].seen)   s.grass.forceCenter = Clamp(c[kGrassParting].value * 0.01f, 0.0f, 3.0f);
        if (c[kGrassRadius].seen)    s.grass.radius = Clamp(c[kGrassRadius].value * 0.1f, 0.1f, 20.0f);
        if (c[kHotkeys].seen)        s.hotkeys = c[kHotkeys].value != 0.0f;
        if (c[kWaterSwashHeight].seen) s.water.swashHeight = Clamp(c[kWaterSwashHeight].value * 0.01f, 0.0f, 1.0f);
        if (c[kWaterSwashLength].seen) s.water.swashLength = Clamp(c[kWaterSwashLength].value, 5.0f, 200.0f);
        if (c[kWaterSwashSpeed].seen)  s.water.swashSpeed  = Clamp(c[kWaterSwashSpeed].value * 0.01f, 0.1f, 4.0f);
        if (c[kVolumeDensity].seen)  s.volume.density  = Clamp(c[kVolumeDensity].value * 0.001f, 0.0f, 0.05f);
        if (c[kVolumeDistance].seen) s.volume.maxDistance = Clamp(c[kVolumeDistance].value, 20.0f, 1000.0f);
        if (c[kVolumeDirection].seen) s.volume.anisotropy = Clamp(c[kVolumeDirection].value * 0.001f, 0.0f, 0.95f);
        if (c[kShadowsWorld].seen)   s.sunShadows.world = c[kShadowsWorld].value != 0.0f;
        if (c[kShadowsUnits].seen)   s.sunShadows.units = c[kShadowsUnits].value != 0.0f;
        if (c[kShadowsLock].seen)    s.sunShadows.lock  = c[kShadowsLock].value != 0.0f;
        if (c[kShadowsTilt].seen)    s.sunShadows.lockTilt = Clamp(c[kShadowsTilt].value, 0.0f, 80.0f);
        if (c[kShadeTint].seen)      s.sunShadows.shadeTint = Clamp(c[kShadeTint].value * 0.01f, 0.0f, 1.0f);
        if (c[kSunTint].seen)        s.sunShadows.sunTint   = Clamp(c[kSunTint].value * 0.01f, 0.0f, 1.0f);
        if (c[kLampGlow].seen)       s.lamps.strength = Clamp(c[kLampGlow].value, 0.0f, 100.0f);
        if (c[kLampDistance].seen)   s.lamps.fogReach = Clamp(c[kLampDistance].value * 0.01f, 0.5f, 4.0f);
        if (c[kNightDarkness].seen)  s.night.darkness = Clamp(c[kNightDarkness].value * 0.01f, 0.0f, 0.9f);
        if (c[kMoonlight].seen)      s.night.tint     = Clamp(c[kMoonlight].value * 0.01f, 0.0f, 1.0f);
        if (c[kDaySaturation].seen)  s.colour.day     = Clamp(c[kDaySaturation].value, 0.0f, 200.0f);
        if (c[kNightSaturation].seen) s.colour.night  = Clamp(c[kNightSaturation].value, 0.0f, 200.0f);
        if (c[kColour].seen)         s.colour.enabled = c[kColour].value != 0.0f;
        if (c[kFog].seen)            s.fog.enabled    = c[kFog].value != 0.0f;
        if (c[kFogDensity].seen)     s.fog.density    = Clamp(c[kFogDensity].value * 0.0001f, 0.0f, 0.1f);
        if (c[kFogHeight].seen)      s.fog.height     = Clamp(c[kFogHeight].value, 1.0f, 2000.0f);
        if (c[kFogBrightness].seen)  s.fog.brightness = Clamp(c[kFogBrightness].value * 0.01f, 0.0f, 4.0f);
        if (c[kFogSun].seen)         s.fog.sunLight   = Clamp(c[kFogSun].value * 0.1f, 0.0f, 50.0f);
        if (c[kFogLow].seen)         s.fog.lowGround  = Clamp(c[kFogLow].value * 0.01f, 0.0f, 10.0f);
        if (c[kFogWater].seen)       s.fog.water      = Clamp(c[kFogWater].value * 0.01f, 0.0f, 10.0f);
        if (c[kFogMorning].seen)     s.fog.morning    = Clamp(c[kFogMorning].value * 0.01f, 0.0f, 10.0f);
        if (c[kFogLamps].seen)       s.fog.lampMist   = Clamp(c[kFogLamps].value * 0.01f, 0.0f, 10.0f);
        if (c[kShadowsNight].seen)   s.sunShadows.night = Clamp(c[kShadowsNight].value, 0.0f, 100.0f);
        if (c[kShadowsUnitStrength].seen)
            s.sunShadows.unitStrength = Clamp(c[kShadowsUnitStrength].value, 0.0f, 100.0f);
        if (c[kSunGlide].seen)       s.sun.glide = Clamp(c[kSunGlide].value, 0.0f, 60.0f);
        if (c[kLamps].seen)          s.lamps.enabled = c[kLamps].value != 0.0f;
        if (c[kTorchLight].seen)     s.lamps.torchLight = Clamp(c[kTorchLight].value * 0.01f, 0.0f, 2.0f);
        if (c[kLanternLight].seen)   s.lamps.lanternLight = Clamp(c[kLanternLight].value * 0.01f, 0.0f, 2.0f);
        if (c[kIndoorLamps].seen)    s.lamps.indoors = Clamp(c[kIndoorLamps].value * 0.01f, 0.0f, 1.0f);
        if (c[kLampsDay].seen)       s.lamps.day = Clamp(c[kLampsDay].value, 0.0f, 100.0f);
        if (c[kShadowsBody].seen)    s.sunShadows.bodyShade = Clamp(c[kShadowsBody].value, 0.0f, 100.0f);
        if (c[kFogReach].seen)       s.fog.reach      = Clamp(c[kFogReach].value, 20.0f, 5000.0f);
        if (c[kFogSky].seen)         s.fog.skyDistance = Clamp(c[kFogSky].value, 0.0f, 5000.0f);
        if (c[kFogPatches].seen)     s.fog.patchiness = Clamp(c[kFogPatches].value * 0.01f, 0.0f, 1.0f);
        if (c[kFogWind].seen)        s.fog.windSpeed  = Clamp(c[kFogWind].value * 0.1f, 0.0f, 50.0f);
        if (c[kFogWindDir].seen)     s.fog.windDeg    = Clamp(c[kFogWindDir].value, 0.0f, 360.0f);
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
                s.fog.debug        = d.fog;
                s.water.debug      = d.water;
                s.grass.debug      = d.grass;
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
        // The fog reads the depth buffer, and draws without the light since 2026-10-02.
        if (c[kFog].seen && c[kFog].value != 0.0f)
            s.depth.enabled = true;

        // Last: the quality level replaces the values it covers, whichever of the ini and the controls
        // set it.
        ApplyVolumeQuality(s);

        // The Atmosphere Effects box: off, every effect is off, whatever its own box says. The depth
        // buffer and the shadow map go too, since nothing reads them.
        if (!s.master)
        {
            s.volume.enabled = s.depth.enabled = s.shadow.enabled = s.fog.enabled = false;
            s.rays.enabled = s.sunShadows.enabled = s.lamps.enabled = s.colour.enabled = false;
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
        g_stats = lookup("comfyStats");
        if (!g_stats)
        {
            g_tuneText.push_back(std::string(kStatsLen, ' '));
            g_stats = registerFn("comfyStats", nullptr, 0, g_tuneText.back().c_str(), nullptr, kCategory, 0, nullptr);
        }
        g_timeSet = lookup("comfyTimeSet");
        if (!g_timeSet)
            g_timeSet = registerFn("comfyTimeSet", nullptr, 0, "", nullptr, kCategory, 0, nullptr);
        g_timeShown = lookup("comfyTimeShown");
        if (!g_timeShown)
        {
            g_tuneText.push_back(std::string(kTimeLen, ' '));
            g_timeShown = registerFn("comfyTimeShown", nullptr, 0, g_tuneText.back().c_str(), nullptr, kCategory, 0,
                                     nullptr);
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

    // Notices waiting for chat.
    if (!g_notices.empty())
    {
        const auto registerFn = reinterpret_cast<RegisterFn>(kRegister + Slide());
        char name[64];
        while (!g_notices.empty())
        {
            snprintf(name, sizeof(name), "comfyNotice%lu", ++g_noticeSeq);
            g_tuneText.push_back(name);
            const char* n = g_tuneText.back().c_str();
            g_tuneText.push_back(g_notices.front());
            g_notices.pop_front();
            registerFn(n, nullptr, 0, g_tuneText.back().c_str(), nullptr, kCategory, 0, nullptr);
        }
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
                TimeAfterTune();
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

void CVarsNotice(const char* text)
{
    if (g_gaveUp || g_notices.size() >= 16)
        return;
    g_notices.push_back(text);
}

void CVarsControlsText(std::string& out)
{
    if (!g_ready)
    {
        out = g_gaveUp ? "none (wrong client build, or [general] sliders = 0)" : "not registered yet";
        return;
    }
    out.clear();
    for (int k = 0; k < kKnobs; ++k)
        if (g_slots[k].seen)
            out += std::string(out.empty() ? "" : " ") + kNames[k] + "=" + g_slots[k].last;
}

namespace
{
    bool SafeWrite(uintptr_t dst, const void* src, size_t n)
    {
        __try
        {
            memcpy(reinterpret_cast<void*>(dst), src, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

void CVarsTime()
{
    if (!g_ready)
        return;

    char cmd[64];
    DWORD str = 0;
    if (g_timeSet && SafeCopy(reinterpret_cast<uintptr_t>(g_timeSet) + 0x20, &str, 4) && str &&
        SafeString(str, cmd, sizeof(cmd)) && (!g_timeSetSeen || strcmp(cmd, g_timeSetLast) != 0))
    {
        const bool first = !g_timeSetSeen;
        g_timeSetSeen = true;
        strcpy_s(g_timeSetLast, cmd);
        // The number only makes each command new; it is skipped.
        char verb[16] = {};
        float value = 0.0f;
        const int n = first ? 0 : sscanf_s(cmd, "%*s %15s %f", verb, static_cast<unsigned>(sizeof(verb)), &value);
        if (n == 2 && strcmp(verb, "set") == 0)
            TimeSet(value);
        else if (n == 2 && strcmp(verb, "step") == 0)
            TimeStep(value * g_cfg.time.step);
        else if (n >= 1 && strcmp(verb, "daynight") == 0)
            TimeToggleDayNight();
        else if (n >= 1 && strcmp(verb, "save") == 0)
            TimeSaveHour();
        else if (!first && cmd[0])
            Log("comfyTimeSet: \"%s\" is not a command", cmd);
    }

    char shown[kTimeLen + 1];
    snprintf(shown, sizeof(shown), "%.4f %d", TimeCurrentHour(), TimeState());
    const size_t len = strlen(shown);
    memset(shown + len, ' ', kTimeLen - len);
    shown[kTimeLen] = 0;
    if (strcmp(shown, g_timeShownLast) == 0)
        return;
    char probe[kTimeLen + 2];
    str = 0;
    if (!g_timeShown || !SafeCopy(reinterpret_cast<uintptr_t>(g_timeShown) + 0x20, &str, 4) || !str ||
        !SafeString(str, probe, sizeof(probe)) || strlen(probe) != kTimeLen)
        return;
    if (SafeWrite(str, shown, kTimeLen))
        strcpy_s(g_timeShownLast, shown);
}

void CVarsStats(const std::string& text)
{
    if (!g_ready || !g_stats)
        return;
    DWORD str = 0;
    char probe[kStatsLen + 2];
    if (!SafeCopy(reinterpret_cast<uintptr_t>(g_stats) + 0x20, &str, 4) || !str ||
        !SafeString(str, probe, sizeof(probe)) || strlen(probe) != kStatsLen)
        return;
    std::string padded = text.substr(0, kStatsLen);
    padded.resize(kStatsLen, ' ');
    SafeWrite(str, padded.data(), kStatsLen);
}
