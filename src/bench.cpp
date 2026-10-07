// bench: what each feature costs, measured in the running client.
//
// Alt + the probe key (Alt+F12) runs three steps, one after the other: sun rays and volumetric light; sun
// rays; nothing. Each step runs for [bench] settle seconds, so shaders compile, then is
// measured for [bench] measure seconds. Then the settings go back as they were, and one table goes to
// comfyatmos.log.
//
// The light goes first, so it is measured with the shadow cache the player built while playing. Measured
// after steps without it, the cache had aged out (cacheTime) and the light step saw a third of the casters
// that normal play holds, and so a third of the cost.
//
// Three numbers per step:
//   - The frame rate and the slowest 1% of frames, from the time between two Presents. That is what a
//     player sees, but a frame cap (vsync, a limiter) hides every cost under it.
//   - Our GPU time: timestamp queries around our own passes (the shadow map and the light passes at the end
//     of the world, and the rays before the UI). A cap does not hide these. The results are read four
//     frames late, so the GPU is never waited for.
//   - Our CPU time around the same passes, plus the recording of the client's draws (shadow.cpp), which
//     happens during the world and is in no pass.
// For the light, one more line splits both: the shadow map and the light passes on the GPU; recording, the
// cache upkeep and the replay on the CPU; and how many casters the map drew.
//
// Stand still for the whole run and do not move the mouse. Face the sun: the rays do not draw when the sun
// is behind you, and the table says so. F11, a control moved in Video > Atmosphere, or a device reset stops
// the run.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "bench.h"
#include "common.h"
#include "config.h"
#include "cvars.h"
#include "shadow.h"

#include <algorithm>
#include <intrin.h>
#include <string>
#include <vector>

namespace
{
    // The hooks' timers (2026-10-06) read the CPU's time stamp counter: a few nanoseconds a read, where
    // QueryPerformanceCounter costs tens, and a hook runs thousands of times a frame. Its rate is found
    // against Now() over each run.
    double g_tickSecs = 0.0;                    // seconds a tick, once known
    unsigned long long g_calTick = 0;
    double g_calNow = 0.0;

    void CalibrateStart()
    {
        g_calTick = __rdtsc();
        g_calNow = Now();
    }

    void CalibrateEnd()
    {
        const unsigned long long t = __rdtsc();
        const double secs = Now() - g_calNow;
        if (t > g_calTick && secs > 0.5)
            g_tickSecs = secs / static_cast<double>(t - g_calTick);
    }

    double TickSecs()
    {
        return g_tickSecs > 0.0 ? g_tickSecs : 1.0 / 2.5e9;   // a guess until a run has measured it
    }

    struct Step
    {
        const char* name;
        bool rays, light, fog;
    };

    const Step kSteps[] = {
        { "rays + light + fog", true,  true,  true  },
        { "rays + fog alone",   true,  false, true  },
        { "rays",               true,  false, false },
        { "nothing",            false, false, false },
    };
    constexpr int kLightStep = 0;
    constexpr int kFogStep   = 1;
    constexpr int kRaysStep  = 2;
    constexpr int kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

    struct Result
    {
        double fps = 0.0, msAvg = 0.0, msSlow = 0.0;
        double gpuMs[kBenchSections] = {};
        double cpuMs[kBenchSections] = {};
        unsigned frames = 0, gpuFrames = 0;
        unsigned ran[kBenchSections] = {};      // frames each section ran in
        double recordMs = 0.0, cacheMs = 0.0, replayMs = 0.0;   // shadow.cpp's CPU split, a frame
        double drawn = 0.0, entries = 0.0;      // casters drawn a replay, entries held
        double mergeMs = 0.0;                   // the part of cacheMs that is Merge
        double stillShare = 0.0;                // of the entries seen again, the share that had not moved
    };

    // A frame's queries: two timestamps per section and the counter's frequency. Four frames of them, so
    // a result is read back four frames after it was issued, when the GPU has long finished it.
    constexpr int kRing = 4;
    struct FrameQueries
    {
        IDirect3DQuery9* begin[kBenchSections] = {};
        IDirect3DQuery9* end[kBenchSections]   = {};
        IDirect3DQuery9* freq = nullptr;
        bool issued[kBenchSections] = {};
        bool used = false;                      // issued this round, so there is something to read
        bool measured = false;                  // issued inside a step's measure window
        bool flMeasured = false;                // issued while the frame log ran
    };

    FrameQueries g_q[kRing];
    int          g_slot = 0;
    bool         g_queriesFailed = false;

    bool     g_running = false;
    int      g_step = 0;
    double   g_stepStart = 0.0;
    Settings g_saved;                           // the settings before the run, put back after it
    Result   g_results[kStepCount];
    std::vector<double> g_frameTimes;           // this step's measured frames, seconds
    double   g_cpuStart[kBenchSections] = {};
    double   g_cpuSum[kBenchSections] = {};
    double   g_gpuSum[kBenchSections] = {};
    unsigned g_gpuFrames = 0;
    unsigned g_ran[kBenchSections] = {};
    double   g_sRecord = 0.0, g_sCache = 0.0, g_sReplay = 0.0;
    unsigned g_sDrawn = 0, g_sEntries = 0, g_sFrames = 0, g_sReplays = 0;
    double   g_sMerge = 0.0;
    unsigned g_sStill = 0, g_sRefreshed = 0;
    char     g_setup[256] = {};
    unsigned long long g_benchParts[kCpuParts] = {};   // this step's hook ticks, measured frames only

    // The frame log's share of the same timers (2026-10-06), so the frame-rate test reports each pass.
    bool     g_flOn = false;
    double   g_flGpuSum[kBenchSections] = {};
    unsigned g_flGpuRan[kBenchSections] = {};
    unsigned g_flGpuFrames = 0;
    double   g_flCpuSum[kBenchSections] = {};
    unsigned long long g_flParts[kCpuParts] = {};
    unsigned long long g_flProf[kProfCount] = {};
    // In the order of BenchProf. Indented: a part of the one above it.
    const char* const kProfNames[kProfCount] = {
        "refresh objects", "merge: place", "  of which objects' parts", "  of which the files' check",
        "merge: claims", "merge: match", "animate objects", "evict", "map terrain update", "replay prep",
        "state save", "the seven passes", "  of which the files' draws", "state restore",
    };

    unsigned long long g_sectionTicks = 0;      // every pass's CPU ticks, a running sum (BenchSectionTicks)
    unsigned long long g_secTick[kBenchSections] = {};
    bool     g_frameBegun = false;              // the GPU frame's first timestamp is issued

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    void ReleaseQueries()
    {
        for (FrameQueries& f : g_q)
        {
            for (int s = 0; s < kBenchSections; ++s)
            {
                SafeRelease(f.begin[s]);
                SafeRelease(f.end[s]);
            }
            SafeRelease(f.freq);
            f = FrameQueries();
        }
        g_slot = 0;
        g_frameBegun = false;
    }

    bool MakeQueries(IDirect3DDevice9* dev)
    {
        if (g_q[0].freq)
            return true;
        auto* d = dev->lpVtbl;
        for (FrameQueries& f : g_q)
        {
            bool ok = SUCCEEDED(d->CreateQuery(dev, D3DQUERYTYPE_TIMESTAMPFREQ, &f.freq));
            for (int s = 0; s < kBenchSections && ok; ++s)
                ok = SUCCEEDED(d->CreateQuery(dev, D3DQUERYTYPE_TIMESTAMP, &f.begin[s])) &&
                     SUCCEEDED(d->CreateQuery(dev, D3DQUERYTYPE_TIMESTAMP, &f.end[s]));
            if (!ok)
            {
                ReleaseQueries();
                return false;
            }
        }
        return true;
    }

    bool InMeasure(double now)
    {
        return now - g_stepStart >= g_cfg.bench.settle;
    }

    // Reads the frame in this slot, if it was issued, and adds it to the step when it was measured.
    void Collect(FrameQueries& f)
    {
        if (!f.used)
            return;
        f.used = false;
        UINT64 freq = 0;
        if (f.freq->lpVtbl->GetData(f.freq, &freq, sizeof(freq), 0) != S_OK || !freq)
            return;
        double ms[kBenchSections] = {};
        for (int s = 0; s < kBenchSections; ++s)
        {
            if (!f.issued[s])
                continue;
            UINT64 a = 0, b = 0;
            if (f.begin[s]->lpVtbl->GetData(f.begin[s], &a, sizeof(a), 0) != S_OK ||
                f.end[s]->lpVtbl->GetData(f.end[s], &b, sizeof(b), 0) != S_OK || b < a)
                return;                         // not ready after four frames: drop the frame
            ms[s] = 1000.0 * static_cast<double>(b - a) / static_cast<double>(freq);
        }
        if (f.flMeasured)
        {
            for (int s = 0; s < kBenchSections; ++s)
            {
                g_flGpuSum[s] += ms[s];
                g_flGpuRan[s] += f.issued[s] ? 1 : 0;
            }
            ++g_flGpuFrames;
        }
        if (!f.measured)
            return;
        for (int s = 0; s < kBenchSections; ++s)
            g_gpuSum[s] += ms[s];
        ++g_gpuFrames;
    }

    void ApplyStep(int i)
    {
        const Step& st = kSteps[i];
        g_cfg = g_saved;
        g_cfg.volume.debug  = 0;
        g_cfg.rays.debugView = 0;

        g_cfg.rays.enabled = st.rays;
        if (st.rays && g_cfg.rays.strength <= 0.0f)
            g_cfg.rays.strength = 35.0f;

        // The light needs the depth buffer and the shadow map, and they cost nothing without it. The fog
        // alone needs the depth buffer only.
        g_cfg.volume.enabled = st.light;
        g_cfg.depth.enabled  = st.light || st.fog;
        g_cfg.shadow.enabled = st.light;
        if (st.light && g_cfg.volume.strength <= 0.0f)
            g_cfg.volume.strength = 30.0f;
        g_cfg.fog.enabled = st.fog;
        if (st.fog && g_cfg.fog.density <= 0.0f)
            g_cfg.fog.density = 0.0025f;

        g_step = i;
        g_stepStart = Now();
        g_frameTimes.clear();
        for (int s = 0; s < kBenchSections; ++s)
            g_cpuSum[s] = g_gpuSum[s] = 0.0, g_ran[s] = 0;
        g_gpuFrames = 0;
        g_sRecord = g_sCache = g_sReplay = 0.0;
        g_sDrawn = g_sEntries = g_sFrames = g_sReplays = 0;
        g_sMerge = 0.0;
        g_sStill = g_sRefreshed = 0;
        for (unsigned long long& t : g_benchParts)
            t = 0;
    }

    void FinishStep()
    {
        Result& r = g_results[g_step];
        r.frames = static_cast<unsigned>(g_frameTimes.size());
        if (r.frames)
        {
            double sum = 0.0;
            for (double t : g_frameTimes)
                sum += t;
            std::vector<double> sorted = g_frameTimes;
            std::sort(sorted.begin(), sorted.end());
            const size_t at = static_cast<size_t>(0.99 * (sorted.size() - 1));
            r.msAvg  = 1000.0 * sum / r.frames;
            r.fps    = r.frames / sum;
            r.msSlow = 1000.0 * sorted[at];
        }
        r.gpuFrames = g_gpuFrames;
        for (int s = 0; s < kBenchSections; ++s)
        {
            r.gpuMs[s] = g_gpuFrames ? g_gpuSum[s] / g_gpuFrames : 0.0;
            r.cpuMs[s] = r.frames ? 1000.0 * g_cpuSum[s] / r.frames : 0.0;
            r.ran[s]   = g_ran[s];
        }
        if (r.frames)
        {
            r.recordMs = 1000.0 * g_sRecord / r.frames;
            r.cacheMs  = 1000.0 * g_sCache / r.frames;
            r.replayMs = 1000.0 * g_sReplay / r.frames;
        }
        r.drawn   = g_sReplays ? static_cast<double>(g_sDrawn) / g_sReplays : 0.0;
        r.mergeMs = r.frames ? 1000.0 * g_sMerge / r.frames : 0.0;
        r.stillShare = g_sRefreshed ? static_cast<double>(g_sStill) / g_sRefreshed : 0.0;
        r.entries = g_sFrames ? static_cast<double>(g_sEntries) / g_sFrames : 0.0;
    }

    void Report()
    {
        Log("bench: %s", g_setup);
        Log("bench: %-20s %7s %9s %11s %12s %12s", "step", "fps", "ms/frame", "slowest 1%", "our GPU ms",
            "our CPU ms");
        double lo = 1e9, hi = 0.0;
        for (int i = 0; i < kStepCount; ++i)
        {
            const Result& r = g_results[i];
            double gpu = 0.0, cpu = r.recordMs;
            for (int s = 0; s < kBenchTop; ++s)
                gpu += r.gpuMs[s], cpu += r.cpuMs[s];
            char gpuText[32];
            if (g_queriesFailed)
                _snprintf_s(gpuText, sizeof(gpuText), _TRUNCATE, "%12s", "n/a");
            else
                _snprintf_s(gpuText, sizeof(gpuText), _TRUNCATE, "%12.2f", gpu);
            Log("bench: %-20s %7.1f %9.2f %11.2f %s %12.2f", kSteps[i].name, r.fps, r.msAvg, r.msSlow, gpuText, cpu);
            if (r.fps < lo) lo = r.fps;
            if (r.fps > hi) hi = r.fps;
        }

        // What the parts cost on their own, from the light step, which has them all.
        const Result& all = g_results[kLightStep];
        Log("bench: the light, a frame: GPU %.2f ms for the shadow map and %.2f ms for the light passes; CPU "
            "%.2f ms recording the client's draws, %.2f ms keeping the cache, %.2f ms replaying it, %.2f ms "
            "issuing the light passes", all.gpuMs[kBenchShadow], all.gpuMs[kBenchVolume], all.recordMs,
            all.cacheMs, all.replayMs, all.cpuMs[kBenchVolume]);
        Log("bench: the shadow map drew %.0f casters each time, from a cache of %.0f entries",
            all.drawn, all.entries);
        Log("bench: keeping the cache: %.2f ms merging the frame's draws in, %.2f ms evicting; %.0f%% of the "
            "entries seen again had not moved and were not copied", all.mergeMs, all.cacheMs - all.mergeMs,
            100.0 * all.stillShare);
        const Result& fog = g_results[kFogStep];
        Log("bench: the fog alone, a frame: GPU %.2f ms, CPU %.2f ms (no shadow map)", fog.gpuMs[kBenchVolume],
            fog.cpuMs[kBenchVolume]);
        Log("bench: the rays, a frame: GPU %.2f ms, CPU %.2f ms", all.gpuMs[kBenchRays], all.cpuMs[kBenchRays]);
        Log("bench: the fog around lamps, a frame: GPU %.2f ms, CPU %.2f ms, drawn in %u of %u frames",
            all.gpuMs[kBenchLamps], all.cpuMs[kBenchLamps], all.ran[kBenchLamps], all.frames);
        Log("bench: the sun shadows, a frame: GPU %.2f ms, CPU %.2f ms, drawn in %u of %u frames",
            all.gpuMs[kBenchSunShadows], all.cpuMs[kBenchSunShadows], all.ran[kBenchSunShadows], all.frames);
        Log("bench: the shadow maps on the GPU, a frame: far %.2f, far leaves %.2f, terrain %.2f, middle %.2f, "
            "near %.2f, near leaves %.2f, units %.2f ms", all.gpuMs[kBenchMapFar], all.gpuMs[kBenchMapFarLeaf],
            all.gpuMs[kBenchMapTerrain], all.gpuMs[kBenchMapMid], all.gpuMs[kBenchMapNear],
            all.gpuMs[kBenchMapNearLeaf], all.gpuMs[kBenchMapUnits]);
        Log("bench: on the GPU, a frame: the body mask and depth %.2f ms, the lighthouses %.2f ms, the saturation "
            "%.2f ms; the GPU's whole frame %.2f ms", all.gpuMs[kBenchMask], all.gpuMs[kBenchBeacon],
            all.gpuMs[kBenchGrade], all.gpuMs[kBenchFrame]);
        if (all.frames)
        {
            const double k = 1000.0 * TickSecs() / all.frames;
            Log("bench: our CPU in the hooks on the client's draws, a frame: %.2f ms (the water %.2f ms of it)",
                g_benchParts[kCpuHooks] * k, g_benchParts[kCpuWater] * k);
        }

        const Result& rays = g_results[kRaysStep];
        if (rays.frames && rays.ran[kBenchRays] < rays.frames / 2)
            Log("bench: the rays drew in only %u of %u frames, so their cost is too low. Face the sun and "
                "run it again.", rays.ran[kBenchRays], rays.frames);
        if (all.frames && all.ran[kBenchVolume] < all.frames / 2)
            Log("bench: the light drew in only %u of %u frames. It does not draw at night or indoors, or "
                "when Alt+F11 turned it off.", all.ran[kBenchVolume], all.frames);
        if (hi > 0.0 && (hi - lo) / hi < 0.02)
            Log("bench: the frame rate did not change between the steps, so it is capped (vsync or a frame "
                "limiter). Compare the GPU and CPU columns, or turn the cap off and run it again.");
        Log("bench: done. The settings are back as they were.");

        // The results in the chat (2026-10-06, the owner): the frame rate with every effect and with none, what the
        // effects take a frame, and each part's GPU time. The table and the rest stay in the log.
        const Result& none = g_results[kStepCount - 1];
        char line[256];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "Benchmark done. %.0f fps with every effect, %.0f fps with none: the effects take %.1f ms a "
                    "frame. Slowest 1%%: %.1f ms.", all.fps, none.fps, all.msAvg - none.msAvg, all.msSlow);
        CVarsNotice(line);
        if (!g_queriesFailed)
        {
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "On the GPU, a frame: shadow map %.1f ms, light and fog %.1f ms, sun shadows %.1f ms, "
                        "rays %.1f ms, lamps %.1f ms.", all.gpuMs[kBenchShadow], all.gpuMs[kBenchVolume],
                        all.gpuMs[kBenchSunShadows], all.gpuMs[kBenchRays], all.gpuMs[kBenchLamps]);
            CVarsNotice(line);
        }
        if (rays.frames && rays.ran[kBenchRays] < rays.frames / 2)
            CVarsNotice("The rays did not draw for most of the run. Face the sun and run it again.");
        if (all.frames && all.ran[kBenchVolume] < all.frames / 2)
            CVarsNotice("The light did not draw for most of the run. It does not draw at night or indoors.");
        if (hi > 0.0 && (hi - lo) / hi < 0.02)
            CVarsNotice("The frame rate is capped (vsync or a frame limiter), so the fps above do not show the cost. "
                        "Turn the cap off and run it again.");
        CVarsNotice("The full table is in Logs\\comfyatmos.log.");
    }

    void Finish()
    {
        CalibrateEnd();
        Report();
        g_cfg = g_saved;
        g_running = false;
        ShadowTiming(false);
    }
}

void BenchStart(IDirect3DDevice9* dev)
{
    if (g_running)
    {
        BenchCancel("Alt+F12 again", true);
        return;
    }
    if (!g_queriesFailed && !MakeQueries(dev))
    {
        g_queriesFailed = true;
        Log("bench: this d3d9.dll has no timestamp queries, so the GPU column stays empty");
    }

    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC bd = {};
    if (SUCCEEDED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
    {
        bb->lpVtbl->GetDesc(bb, &bd);
        bb->lpVtbl->Release(bb);
    }
    const Settings& s = g_cfg;
    _snprintf_s(g_setup, sizeof(g_setup), _TRUNCATE,
                "%ux%u; light: steps %d, downscale %d, smooth %.2f, shadow map %d every %d frames; "
                "rays: downscale %d, passes %d",
                bd.Width, bd.Height, s.volume.steps, s.volume.downscale, s.volume.smooth, s.shadow.size,
                s.shadow.mapEvery, s.rays.downscale, s.rays.passes);

    g_saved = g_cfg;
    g_running = true;
    CalibrateStart();
    ShadowTiming(true);
    for (Result& r : g_results)
        r = Result();
    const double each = g_cfg.bench.settle + g_cfg.bench.measure;
    Log("bench: %d steps of %.0f s (%.0f s to settle, then %.0f s measured), %.0f s in all. Stand still, do "
        "not move the mouse, and face the sun.", kStepCount, each, g_cfg.bench.settle, g_cfg.bench.measure,
        each * kStepCount);
    char line[160];
    snprintf(line, sizeof(line), "Running benchmark: %.0f seconds. Stand still, face the sun and do not move the mouse.",
             each * kStepCount);
    CVarsNotice(line);
    ApplyStep(0);
}

bool BenchFrame(IDirect3DDevice9* dev, double frameSeconds)
{
    if (!g_running && !g_flOn)
        return false;

    // Close this frame's queries, then read the oldest frame before its slot is used again. The GPU's frame
    // ends here, at Present. The frame log uses the same queries (2026-10-06).
    if (g_frameBegun)
    {
        BenchSectionEnd(dev, kBenchFrame, true);
        g_frameBegun = false;
    }
    FrameQueries& cur = g_q[g_slot];
    const double now = Now();
    if (cur.freq && cur.used)
    {
        cur.freq->lpVtbl->Issue(cur.freq, D3DISSUE_END);
        cur.measured = g_running && InMeasure(now);
        cur.flMeasured = g_flOn;
    }
    g_slot = (g_slot + 1) % kRing;
    FrameQueries& next = g_q[g_slot];
    if (next.freq)
        Collect(next);
    for (int s = 0; s < kBenchSections; ++s)
        next.issued[s] = false;
    next.used = next.measured = next.flMeasured = false;
    if (!g_running)
        return false;

    if (InMeasure(now) && frameSeconds > 0.0)
        g_frameTimes.push_back(frameSeconds);
    {
        double rec = 0.0, cache = 0.0, replay = 0.0;
        unsigned drawn = 0, entries = 0, frames = 0, replays = 0;
        ShadowTakeTimes(rec, cache, replay, drawn, entries, frames, replays);
        double merge = 0.0;
        unsigned still = 0, refreshed = 0;
        ShadowTakeCacheSplit(merge, still, refreshed);
        if (InMeasure(now))
        {
            g_sRecord += rec; g_sCache += cache; g_sReplay += replay;
            g_sDrawn += drawn; g_sEntries += entries; g_sFrames += frames; g_sReplays += replays;
            g_sMerge += merge; g_sStill += still; g_sRefreshed += refreshed;
        }
    }

    if (now - g_stepStart < g_cfg.bench.settle + g_cfg.bench.measure)
        return false;
    FinishStep();
    Log("bench: step %d of %d, %s: %.1f fps", g_step + 1, kStepCount, kSteps[g_step].name,
        g_results[g_step].fps);
    if (g_step + 1 < kStepCount)
        ApplyStep(g_step + 1);
    else
        Finish();
    return true;
}

void BenchCancel(const char* why, bool restore)
{
    if (!g_running)
        return;
    if (restore)
        g_cfg = g_saved;
    g_running = false;
    ShadowTiming(false);
    Log("bench: stopped (%s)%s", why, restore ? ". The settings are back as they were." : "");
    char line[160];
    snprintf(line, sizeof(line), "Benchmark stopped: %s.", why);
    CVarsNotice(line);
}

bool BenchRunning()
{
    return g_running;
}

bool BenchTiming()
{
    return g_running || g_flOn;
}

void BenchProfAdd(BenchProf p, unsigned long long ticks)
{
    if (g_flOn)
        g_flProf[p] += ticks;
}

void BenchCpuAddTicks(BenchCpu part, unsigned long long ticks)
{
    if (g_running && InMeasure(Now()))
        g_benchParts[part] += ticks;
    if (g_flOn)
        g_flParts[part] += ticks;
}

unsigned long long BenchSectionTicks()
{
    return g_sectionTicks;
}

void BenchFrameBegin(IDirect3DDevice9* dev)
{
    if (!BenchTiming() || g_frameBegun)
        return;
    g_frameBegun = true;
    BenchSectionBegin(dev, kBenchFrame);
}

void BenchSectionBegin(IDirect3DDevice9* dev, BenchSection s)
{
    if (!BenchTiming())
        return;
    // The frame log needs the queries too, and it starts from a chat command, without the device.
    if (!g_queriesFailed && !g_q[0].freq && !MakeQueries(dev))
    {
        g_queriesFailed = true;
        Log("bench: this d3d9.dll has no timestamp queries, so the GPU column stays empty");
    }
    g_cpuStart[s] = Now();
    g_secTick[s] = __rdtsc();
    FrameQueries& f = g_q[g_slot];
    if (f.begin[s])
        f.begin[s]->lpVtbl->Issue(f.begin[s], D3DISSUE_END);
}

void BenchSectionEnd(IDirect3DDevice9* dev, BenchSection s, bool drew)
{
    if (!BenchTiming())
        return;
    (void)dev;
    const double now = Now();
    // Only our passes go into the running sum the hooks take out: a part of one, or the frame, would count twice.
    if (s < kBenchTop)
        g_sectionTicks += __rdtsc() - g_secTick[s];
    if (g_running && InMeasure(now))
    {
        g_cpuSum[s] += now - g_cpuStart[s];
        g_ran[s] += drew ? 1 : 0;
    }
    if (g_flOn)
        g_flCpuSum[s] += now - g_cpuStart[s];
    FrameQueries& f = g_q[g_slot];
    if (f.end[s])
    {
        f.end[s]->lpVtbl->Issue(f.end[s], D3DISSUE_END);
        f.issued[s] = true;
        f.used = true;
    }
}

namespace
{
    struct FrameRow
    {
        double at, ms, record, cache, replay;
        unsigned added, evicted, copies, entries;
    };
    double                g_flStart = 0.0, g_flLength = 0.0;
    std::vector<FrameRow> g_flRows;
    unsigned              g_flCopies = 0;

    void FrameLogReport()
    {
        const size_t n = g_flRows.size();
        if (n < 2)
        {
            Log("framelog: done: too few frames (%u)", static_cast<unsigned>(n));
            return;
        }
        double sum = 0.0, sumOurs = 0.0, sumRec = 0.0, sumCache = 0.0, sumRep = 0.0;
        unsigned added = 0, copies = 0;
        for (const FrameRow& r : g_flRows)
        {
            sum += r.ms;
            sumRec += r.record; sumCache += r.cache; sumRep += r.replay;
            sumOurs += r.record + r.cache + r.replay;
            added += r.added; copies += r.copies;
        }
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [](size_t a, size_t b) { return g_flRows[a].ms > g_flRows[b].ms; });
        const size_t onePct = (std::max)(static_cast<size_t>(1), n / 100);
        double low = 0.0;
        for (size_t i = 0; i < onePct; ++i) low += g_flRows[order[i]].ms;
        low /= onePct;
        const size_t fivePct = (std::max)(static_cast<size_t>(1), n / 20);
        double oursSlow = 0.0;
        for (size_t i = 0; i < fivePct; ++i)
        {
            const FrameRow& r = g_flRows[order[i]];
            oursSlow += r.record + r.cache + r.replay;
        }
        oursSlow /= fivePct;
        const double oursRest = n > fivePct ? (sumOurs - oursSlow * fivePct) / (n - fivePct) : 0.0;
        Log("framelog: %u frames over %.1f s: %.2f ms a frame (%.1f fps), slowest 1%% %.2f ms, worst %.2f ms",
            static_cast<unsigned>(n), g_flRows.back().at, sum / n, 1000.0 * n / sum, low, g_flRows[order[0]].ms);
        Log("framelog: our CPU a frame %.2f ms (recording %.2f, cache %.2f, replay %.2f); %u new cache entries, "
            "%u copies of streamed geometry", sumOurs / n, sumRec / n, sumCache / n, sumRep / n, added, copies);
        Log("framelog: our CPU in the slowest 5%% of frames %.2f ms, in the rest %.2f ms", oursSlow, oursRest);
        // The whole of our time (2026-10-06). The lines above count the shadow cache alone. Our CPU in all is the
        // hooks on the client's draws (the recording among them) and our passes (the cache and the replay among
        // them). The GPU figures are averages over the frames whose timestamps came back.
        {
            const double k = 1000.0 * TickSecs() / n;
            double passes = 0.0;
            for (int s = 0; s < kBenchTop; ++s)
                passes += g_flCpuSum[s];
            passes *= 1000.0 / n;
            const double hooks = g_flParts[kCpuHooks] * k;
            Log("framelog: our CPU in all a frame %.2f ms: %.2f ms in the hooks on the client's draws (the water "
                "%.2f ms of it), %.2f ms in our passes (the shadow maps %.2f ms)", hooks + passes, hooks,
                g_flParts[kCpuWater] * k, passes, 1000.0 * g_flCpuSum[kBenchShadow] / n);
            // Each pass's CPU (2026-10-07, perf-1): the shadow maps were 2 of 6 ms in our passes.
            const double c = 1000.0 / n;
            Log("framelog: our passes' CPU a frame: shadow maps %.2f, sun shadows %.2f, light and fog %.2f, lamp fog "
                "%.2f, rays %.2f, lighthouses %.2f, body mask and depth %.2f, saturation %.2f ms",
                g_flCpuSum[kBenchShadow] * c, g_flCpuSum[kBenchSunShadows] * c, g_flCpuSum[kBenchVolume] * c,
                g_flCpuSum[kBenchLamps] * c, g_flCpuSum[kBenchRays] * c, g_flCpuSum[kBenchBeacon] * c,
                g_flCpuSum[kBenchMask] * c, g_flCpuSum[kBenchGrade] * c);
            std::string split;
            for (int p = 0; p < kProfCount; ++p)
            {
                char part[96];
                _snprintf_s(part, sizeof(part), _TRUNCATE, "%s%s %.3f", p ? "; " : "", kProfNames[p], g_flProf[p] * k);
                split += part;
            }
            Log("framelog: shadow CPU split, ms a frame: %s", split.c_str());
            Log("framelog: the water's CPU a frame: %.2f ms reading the wet cells, %.2f ms issuing our draws over the "
                "chunks, %.2f ms the rest", g_flParts[kCpuWaterCells] * k, g_flParts[kCpuWaterDraws] * k,
                (g_flParts[kCpuWater] - g_flParts[kCpuWaterCells] - g_flParts[kCpuWaterDraws]) * k);
            Log("framelog: the grass's CPU a frame: %.2f ms, its draws included", g_flParts[kCpuGrass] * k);
            if (g_flGpuFrames)
            {
                const double gf = 1.0 / g_flGpuFrames;
                double gpu = 0.0;
                for (int s = 0; s < kBenchTop; ++s)
                    gpu += g_flGpuSum[s];
                Log("framelog: our GPU a frame %.2f ms: shadow maps %.2f, sun shadows %.2f, light and fog %.2f, lamp "
                    "fog %.2f, rays %.2f, lighthouses %.2f, body mask and depth %.2f, saturation %.2f ms; the GPU's "
                    "whole frame %.2f ms (%u frames timed)", gpu * gf, g_flGpuSum[kBenchShadow] * gf,
                    g_flGpuSum[kBenchSunShadows] * gf, g_flGpuSum[kBenchVolume] * gf, g_flGpuSum[kBenchLamps] * gf,
                    g_flGpuSum[kBenchRays] * gf, g_flGpuSum[kBenchBeacon] * gf, g_flGpuSum[kBenchMask] * gf,
                    g_flGpuSum[kBenchGrade] * gf, g_flGpuSum[kBenchFrame] * gf, g_flGpuFrames);
                Log("framelog: the shadow maps on the GPU a frame: far %.2f, far leaves %.2f, terrain %.2f, middle "
                    "%.2f, near %.2f, near leaves %.2f, units %.2f ms (drawn in %u, %u, %u, %u, %u, %u and %u of the "
                    "frames)", g_flGpuSum[kBenchMapFar] * gf, g_flGpuSum[kBenchMapFarLeaf] * gf,
                    g_flGpuSum[kBenchMapTerrain] * gf, g_flGpuSum[kBenchMapMid] * gf, g_flGpuSum[kBenchMapNear] * gf,
                    g_flGpuSum[kBenchMapNearLeaf] * gf, g_flGpuSum[kBenchMapUnits] * gf, g_flGpuRan[kBenchMapFar],
                    g_flGpuRan[kBenchMapFarLeaf], g_flGpuRan[kBenchMapTerrain], g_flGpuRan[kBenchMapMid],
                    g_flGpuRan[kBenchMapNear], g_flGpuRan[kBenchMapNearLeaf], g_flGpuRan[kBenchMapUnits]);
            }
            if (g_flGpuFrames && g_flGpuRan[kBenchWaterSpan])
                Log("framelog: the water on the GPU a frame: %.2f ms for its copies and the wet sand, %.2f ms from its "
                    "first chunk to the world's end, the game's draws in that span included (drawn in %u of the frames)",
                    g_flGpuSum[kBenchWaterPrep] / g_flGpuFrames, g_flGpuSum[kBenchWaterSpan] / g_flGpuFrames,
                    g_flGpuRan[kBenchWaterSpan]);
            if (!g_flGpuFrames)
                Log("framelog: no GPU times: no timestamp queries came back");
        }
        Log("framelog: the slowest frames:");
        for (size_t i = 0; i < (std::min)(n, static_cast<size_t>(10)); ++i)
        {
            const FrameRow& r = g_flRows[order[i]];
            Log("framelog:   %6.2f ms at %5.2f s; ours %.2f (recording %.2f, cache %.2f, replay %.2f); %u added, "
                "%u evicted, %u copies, %u entries", r.ms, r.at, r.record + r.cache + r.replay, r.record, r.cache,
                r.replay, r.added, r.evicted, r.copies, r.entries);
        }
        Log("framelog: done");
    }
}

void FrameLogStart(double seconds)
{
    if (g_running)
    {
        Log("framelog: not started: the benchmark is running");
        return;
    }
    g_flRows.clear();
    g_flRows.reserve(4096);
    g_flStart = Now();
    g_flLength = seconds;
    g_flOn = true;
    CalibrateStart();
    for (int s = 0; s < kBenchSections; ++s)
        g_flGpuSum[s] = g_flCpuSum[s] = 0.0, g_flGpuRan[s] = 0;
    g_flGpuFrames = 0;
    for (unsigned long long& t : g_flParts)
        t = 0;
    for (unsigned long long& t : g_flProf)
        t = 0;
    ShadowTiming(true);
    double r, c, p;
    unsigned d, e, f, rp, failed;
    ShadowTakeTimes(r, c, p, d, e, f, rp);   // start from nothing
    g_flCopies = ShadowCopies(failed);
    Log("framelog: started, %.1f s", seconds);
}

void FrameLogFrame(double frameSeconds)
{
    if (!g_flOn || frameSeconds <= 0.0)
        return;
    FrameRow row = {};
    row.at = Now() - g_flStart;
    row.ms = 1000.0 * frameSeconds;
    unsigned d, e, f, rp;
    ShadowTakeTimes(row.record, row.cache, row.replay, d, e, f, rp);
    row.record *= 1000.0; row.cache *= 1000.0; row.replay *= 1000.0;
    int outcome = 0;
    unsigned drawn = 0, counts[5] = {};
    const char* info = nullptr;
    ShadowLastReplay(outcome, drawn, row.entries, counts, info);
    row.added = counts[1];
    row.evicted = counts[2] + counts[3] + counts[4];
    unsigned failed = 0;
    const unsigned copies = ShadowCopies(failed);
    row.copies = copies > g_flCopies ? copies - g_flCopies : 0;
    g_flCopies = copies;
    g_flRows.push_back(row);
    if (row.at >= g_flLength)
    {
        g_flOn = false;
        CalibrateEnd();
        ShadowTiming(false);
        FrameLogReport();
        CVarsNotice("Frame log done. The results are in Logs\\comfyatmos.log.");
    }
}

bool FrameLogRunning()
{
    return g_flOn;
}

void BenchReset()
{
    BenchCancel("device reset", true);
    ReleaseQueries();
}
