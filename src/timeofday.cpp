// timeofday -- finding where this WoW.exe keeps the time of day. Read-only.
//
// The sky, the sun and the world's lighting all follow the game clock, which the server sets and 1.12
// gives no command to change. To control it we first need to know where the client keeps it, and the
// published 1.12.1 offset lists have already proved wrong for this binary once (see comfygrass's README),
// so it is found by measurement instead.
//
// The test that cannot be fooled easily: the game clock runs at real speed, one game minute per real
// minute. So snapshot every 4-byte value in WoW.exe's writable sections, wait, snapshot again, and keep
// only values that (a) look like a time of day in some encoding and (b) advanced by exactly the minutes
// that really passed. Then keep re-checking the survivors: a clock keeps matching, a coincidence drops out.
//
// Encodings tried, all modulo one day:
//   minutes since midnight, int      (0..1439)
//   seconds since midnight, int      (0..86399)
//   packed date/time, int            (WoW's SMSG time: minute in bits 0-5, hour in bits 6-10, date above)
//   fraction of the day, float       (0..1)
//   hours, float                     (0..24)
//   minutes, float                   (0..1440)
//
// The scan writes nothing.
//
// What it found in this WoW.exe (Ctrl+F12 run against the minimap clock, 16:28 -> 16:29):
//   0x00CE9B60  int    minutes since midnight      988 = 16:28
//   0x00CE9B64  float  fraction of the day         0.6865, continuous (carries the seconds)
//   0x00CE8574  float  minutes since midnight      988.0
// The first two sit side by side -- the client's game-time pair. (A second pair at 0x00CE9D00/04 ran at
// the same rate but 78 minutes behind; not understood yet, and not written.)
//
// Control (TimeApply) writes a chosen time into all three, twice a frame: after the frame at Present, and
// again at BeginScene, just before the sky is drawn. Whether that sticks depends on whether the client
// keeps this value and advances it, or rebuilds it every frame from the server clock -- so it also checks,
// before each write, whether the client has overwritten the last one, and logs what it finds.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include "common.h"
#include "config.h"
#include "timeofday.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    enum Kind { kMinInt, kSecInt, kPacked, kFrac, kHours, kMinFloat, kKinds };
    const char* kKindName[kKinds] = { "minutes (int)", "seconds (int)", "packed date/time", "day fraction (float)",
                                      "hours (float)", "minutes (float)" };

    struct Region { uintptr_t base; size_t size; };
    struct Candidate { uintptr_t addr; Kind kind; uint32_t last; };

    std::vector<Region>    g_regions;
    std::vector<uint32_t>  g_snap;          // the first snapshot, all regions back to back
    std::vector<Candidate> g_cands;
    int    g_stage   = 0;                   // 0 idle, 1 waiting for the first compare, 2+ narrowing rounds
    double g_lastT   = 0.0;
    constexpr double kFirstWait  = 120.0;   // seconds before the first compare
    constexpr double kRoundWait  = 60.0;    // then one round a minute
    constexpr int    kRounds     = 4;

    bool SafeRead(uintptr_t src, void* dst, size_t n)
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

    // The writable sections of WoW.exe, from its PE header (the camera and player globals live there).
    void FindRegions()
    {
        g_regions.clear();
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE))
                continue;
            const size_t size = sec[i].Misc.VirtualSize & ~static_cast<size_t>(3);
            g_regions.push_back({ base + sec[i].VirtualAddress, size });
            char name[9] = {};
            memcpy(name, sec[i].Name, 8);
            Log("time scan: section %-8s at 0x%08X, %u KB", name, static_cast<unsigned>(base + sec[i].VirtualAddress),
                static_cast<unsigned>(size / 1024));
        }
    }

    bool Snapshot(std::vector<uint32_t>& out)
    {
        size_t total = 0;
        for (const Region& r : g_regions)
            total += r.size / 4;
        out.resize(total);
        size_t at = 0;
        for (const Region& r : g_regions)
        {
            if (!SafeRead(r.base, out.data() + at, r.size))
                return false;
            at += r.size / 4;
        }
        return true;
    }

    inline float AsFloat(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
    inline bool  Finite(float f) { return f == f && f > -1e30f && f < 1e30f; }

    // Distance travelled from a to b, modulo `period` (the clock may have wrapped past midnight).
    inline double Advance(double a, double b, double period)
    {
        double d = fmod(b - a, period);
        return d < 0 ? d + period : d;
    }

    // Does (v0 -> v1) look like `kind` advancing by `minutes`? Also yields the decoded minute of day.
    bool Matches(Kind kind, uint32_t v0, uint32_t v1, double minutes, double* dayMinute)
    {
        if (v0 == v1)
            return false;
        switch (kind)
        {
        case kMinInt:
            if (v0 >= 1440 || v1 >= 1440) return false;
            *dayMinute = v1;
            return fabs(Advance(v0, v1, 1440.0) - minutes) <= 1.01;
        case kSecInt:
            if (v0 >= 86400 || v1 >= 86400) return false;
            *dayMinute = v1 / 60.0;
            return fabs(Advance(v0, v1, 86400.0) - minutes * 60.0) <= 61.0;
        case kPacked:
        {
            // Only values that also carry date bits count here; a bare minute count is kMinInt.
            if ((v0 >> 11) == 0) return false;
            if ((v1 >> 11) - (v0 >> 11) > 8) return false;   // the date may tick over at midnight, no more
            const uint32_t m0 = v0 & 63, h0 = (v0 >> 6) & 31, m1 = v1 & 63, h1 = (v1 >> 6) & 31;
            if (m0 > 59 || m1 > 59 || h0 > 23 || h1 > 23) return false;
            *dayMinute = h1 * 60.0 + m1;
            return fabs(Advance(h0 * 60.0 + m0, h1 * 60.0 + m1, 1440.0) - minutes) <= 1.01;
        }
        case kFrac:
        {
            const float a = AsFloat(v0), b = AsFloat(v1);
            if (!Finite(a) || !Finite(b) || a < 0.0f || a >= 1.0f || b < 0.0f || b >= 1.0f) return false;
            *dayMinute = b * 1440.0;
            return fabs(Advance(a, b, 1.0) - minutes / 1440.0) <= 1.5 / 1440.0;
        }
        case kHours:
        {
            const float a = AsFloat(v0), b = AsFloat(v1);
            if (!Finite(a) || !Finite(b) || a < 0.0f || a >= 24.0f || b < 0.0f || b >= 24.0f) return false;
            *dayMinute = b * 60.0;
            return fabs(Advance(a, b, 24.0) - minutes / 60.0) <= 1.5 / 60.0;
        }
        case kMinFloat:
        {
            const float a = AsFloat(v0), b = AsFloat(v1);
            if (!Finite(a) || !Finite(b) || a < 0.0f || a >= 1440.0f || b < 0.0f || b >= 1440.0f) return false;
            *dayMinute = b;
            return fabs(Advance(a, b, 1440.0) - minutes) <= 1.5;
        }
        default:
            return false;
        }
    }

    void Report(const char* heading)
    {
        int perKind[kKinds] = {};
        for (const Candidate& c : g_cands)
            perKind[c.kind]++;
        Log("time scan: %s -- %u candidates (minutes %d, seconds %d, packed %d, fraction %d, hours %d, minute-float %d)",
            heading, static_cast<unsigned>(g_cands.size()), perKind[kMinInt], perKind[kSecInt], perKind[kPacked],
            perKind[kFrac], perKind[kHours], perKind[kMinFloat]);
        if (g_cands.size() > 40)
            return;
        for (const Candidate& c : g_cands)
        {
            double dm = 0.0;
            uint32_t now = 0;
            SafeRead(c.addr, &now, 4);
            switch (c.kind)
            {
            case kMinInt:   dm = now; break;
            case kSecInt:   dm = now / 60.0; break;
            case kPacked:   dm = ((now >> 6) & 31) * 60.0 + (now & 63); break;
            case kFrac:     dm = AsFloat(now) * 1440.0; break;
            case kHours:    dm = AsFloat(now) * 60.0; break;
            case kMinFloat: dm = AsFloat(now); break;
            default: break;
            }
            const int h = static_cast<int>(dm / 60.0) % 24, m = static_cast<int>(fmod(dm, 60.0));
            Log("time scan:   0x%08X  %-22s raw 0x%08X  reads %02d:%02d", static_cast<unsigned>(c.addr),
                kKindName[c.kind], now, h, m);
        }
    }
}

void TimeScanStart()
{
    FindRegions();
    if (g_regions.empty() || !Snapshot(g_snap))
    {
        Log("time scan: could not read WoW.exe's writable sections");
        g_stage = 0;
        return;
    }
    g_cands.clear();
    g_stage = 1;
    g_lastT = Now();
    Log("time scan: snapshot of %u values taken. Keep playing (stay logged in); the first comparison runs in "
        "%.0f seconds, then one a minute for %d more.", static_cast<unsigned>(g_snap.size()), kFirstWait, kRounds);
}

void TimeScanTick()
{
    if (!g_stage)
        return;
    const double now = Now();
    const double elapsed = now - g_lastT;

    if (g_stage == 1)
    {
        if (elapsed < kFirstWait)
            return;
        std::vector<uint32_t> snap;
        if (!Snapshot(snap) || snap.size() != g_snap.size())
        {
            Log("time scan: second snapshot failed, scan abandoned");
            g_stage = 0;
            return;
        }
        const double minutes = elapsed / 60.0;
        size_t at = 0;
        for (const Region& r : g_regions)
        {
            for (size_t i = 0; i < r.size / 4; ++i, ++at)
            {
                const uint32_t v0 = g_snap[at], v1 = snap[at];
                if (v0 == v1)
                    continue;
                for (int k = 0; k < kKinds; ++k)
                {
                    double dm;
                    if (Matches(static_cast<Kind>(k), v0, v1, minutes, &dm))
                        g_cands.push_back({ r.base + i * 4, static_cast<Kind>(k), v1 });
                }
            }
        }
        g_snap.clear();
        g_snap.shrink_to_fit();
        g_lastT = now;
        g_stage = 2;
        char heading[64];
        _snprintf_s(heading, sizeof(heading), _TRUNCATE, "after %.1f minutes", minutes);
        Report(heading);
        if (g_cands.empty())
        {
            Log("time scan: nothing matched. The time may live outside WoW.exe's own sections (on the heap), "
                "or the realm's clock does not run at real speed.");
            g_stage = 0;
        }
        return;
    }

    if (elapsed < kRoundWait)
        return;
    const double minutes = elapsed / 60.0;
    std::vector<Candidate> keep;
    for (Candidate c : g_cands)
    {
        uint32_t v = 0;
        double dm;
        if (SafeRead(c.addr, &v, 4) && Matches(c.kind, c.last, v, minutes, &dm))
        {
            c.last = v;
            keep.push_back(c);
        }
    }
    g_cands.swap(keep);
    g_lastT = now;
    const int round = g_stage - 1;
    char heading[64];
    _snprintf_s(heading, sizeof(heading), _TRUNCATE, "round %d of %d", round, kRounds);
    Report(heading);
    if (g_cands.empty())
    {
        Log("time scan: every candidate dropped out");
        g_stage = 0;
        return;
    }
    if (++g_stage > kRounds + 1)
    {
        Log("time scan: done. Compare the times above with the minimap clock (hover it).");
        g_stage = 0;
    }
}

// ---------------------------------------------------------------------------------------------------
// control

namespace
{
    float g_hour        = -1.0f;   // the time being shown, hours; <0 until first applied
    bool  g_checked     = false;   // addresses validated once per (re)load
    bool  g_usable      = false;
    bool  g_wrote       = false;
    uint32_t g_lastMin  = 0;       // what was written last, to notice the client overwriting it
    float g_lastFrac    = 0.0f;
    int   g_fights      = 0;       // writes found overwritten
    int   g_holds       = 0;       // writes found intact
    int   g_reports     = 0;
    double g_reportT    = 0.0;
    DWORD g_checkedAddr[3] = {};   // the addresses that passed Validate

    bool Writable(uintptr_t a)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!a || !VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return false;
        const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (mbi.Protect & rw) && !(mbi.Protect & PAGE_GUARD);
    }

    // The three still hold the time, in the encodings we expect -- a different WoW.exe would have moved them.
    bool Validate()
    {
        const TimeSettings& t = g_cfg.time;
        const uintptr_t slide = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) - 0x00400000;
        const uintptr_t aMin = t.addrMinutes + slide, aFrac = t.addrFraction + slide, aMinF = t.addrMinutesF + slide;
        if (!Writable(aMin) || !Writable(aFrac) || (t.addrMinutesF && !Writable(aMinF)))
        {
            Log("time: refusing to write -- an address is not writable memory (different WoW.exe?)");
            return false;
        }
        uint32_t m = 0, fr = 0, mf = 0;
        SafeRead(aMin, &m, 4);
        SafeRead(aFrac, &fr, 4);
        if (t.addrMinutesF) SafeRead(aMinF, &mf, 4);
        const float frac = AsFloat(fr), minF = AsFloat(mf);
        const bool ok = m < 1440 && Finite(frac) && frac >= 0.0f && frac < 1.0f &&
                        fabsf(frac * 1440.0f - static_cast<float>(m)) < 2.0f &&
                        (!t.addrMinutesF || (Finite(minF) && fabsf(minF - static_cast<float>(m)) < 2.0f));
        if (!ok)
        {
            Log("time: refusing to write -- the addresses do not hold a consistent time (minutes %u, fraction %.4f, "
                "minute-float %.2f). Re-run the Ctrl+F12 search for this WoW.exe.", m, frac, minF);
            return false;
        }
        Log("time: addresses check out (client time %02u:%02u); writing %02d:%02d from now on",
            m / 60, m % 60, static_cast<int>(g_hour), static_cast<int>(fmodf(g_hour * 60.0f, 60.0f)));
        return true;
    }
}

// A reload re-reads the chosen hour, but does not re-check addresses that already passed. Mid-session
// they hold a mix of the client's time and ours -- it rewrites some of them every frame -- which the
// consistency test rightly rejects, and did: time control went dead after the first F11. The test is
// there to catch a different WoW.exe, which a reload cannot bring; changed addresses still re-check.
void TimeReload()
{
    const TimeSettings& t = g_cfg.time;
    if (!g_usable || g_checkedAddr[0] != t.addrMinutes || g_checkedAddr[1] != t.addrFraction ||
        g_checkedAddr[2] != t.addrMinutesF)
        g_checked = false;
    g_hour  = -1.0f;
    g_wrote = false;
}

void TimeStep(float hours)
{
    if (!g_cfg.time.enabled)
        return;
    if (g_hour < 0.0f)
        g_hour = g_cfg.time.hour;
    g_hour = fmodf(g_hour + hours + 24.0f, 24.0f);
    Log("--- time: %02d:%02d ---", static_cast<int>(g_hour), static_cast<int>(fmodf(g_hour * 60.0f, 60.0f)));
}

void TimeApply(const char* where)
{
    const TimeSettings& t = g_cfg.time;
    if (!t.enabled)
        return;
    if (g_hour < 0.0f)
        g_hour = fmodf(t.hour + 24.0f, 24.0f);
    if (!g_checked)
    {
        g_checked = true;
        g_usable  = Validate();
        g_checkedAddr[0] = t.addrMinutes; g_checkedAddr[1] = t.addrFraction; g_checkedAddr[2] = t.addrMinutesF;
    }
    if (!g_usable)
        return;

    const uintptr_t slide = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) - 0x00400000;
    auto* pMin  = reinterpret_cast<volatile uint32_t*>(t.addrMinutes + slide);
    auto* pFrac = reinterpret_cast<volatile float*>(t.addrFraction + slide);
    auto* pMinF = t.addrMinutesF ? reinterpret_cast<volatile float*>(t.addrMinutesF + slide) : nullptr;

    // Did the client put its own time back since our last write?
    if (g_wrote)
    {
        const bool fought = *pMin != g_lastMin || fabsf(*pFrac - g_lastFrac) > 1e-6f;
        (fought ? g_fights : g_holds)++;
        const double now = Now();
        if (g_reports < 12 && now - g_reportT > 5.0)
        {
            g_reportT = now;
            ++g_reports;
            Log("time: at %s the client had %s our time (%d overwritten, %d intact so far)%s", where,
                fought ? "OVERWRITTEN" : "kept", g_fights, g_holds,
                fought ? " -- it rebuilds the time itself; writing just before the sky is what can still work" : "");
        }
    }

    const float    minutes = g_hour * 60.0f;
    const uint32_t m       = static_cast<uint32_t>(minutes) % 1440;
    const float    frac    = minutes / 1440.0f;
    *pMin  = m;
    *pFrac = frac;
    if (pMinF)
        *pMinF = static_cast<float>(m);
    g_lastMin  = m;
    g_lastFrac = frac;
    g_wrote    = true;
}
