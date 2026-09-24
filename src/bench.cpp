// bench: what each feature costs, measured in the running client.
//
// Alt + the probe key (Alt+F12) runs four steps, one after the other: fog, sun rays and volumetric light;
// fog and sun rays; fog; nothing. Each step runs for [bench] settle seconds, so shaders compile, then is
// measured for [bench] measure seconds. Then the settings go back as they were, and one table goes to
// comfyfog.log.
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
// The fog costs nothing to draw: it is the client's own fog with other numbers. Any difference between
// "nothing" and "fog" is noise, which shows how far apart two steps must be to mean something.
//
// Stand still for the whole run and do not move the mouse. Face the sun: the rays do not draw when the sun
// is behind you, and the table says so. F11, a control moved in Video > Shaders, or a device reset stops
// the run.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "bench.h"
#include "common.h"
#include "config.h"
#include "shadow.h"

#include <algorithm>
#include <vector>

namespace
{
    struct Step
    {
        const char* name;
        bool fog, rays, light;
    };

    const Step kSteps[] = {
        { "fog + rays + light",  true,  true,  true  },
        { "fog + rays",          true,  true,  false },
        { "fog",                 true,  false, false },
        { "nothing",             false, false, false },
    };
    constexpr int kLightStep = 0;
    constexpr int kRaysStep  = 1;
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

        g_cfg.fog.enabled = st.fog;
        if (st.fog && g_cfg.fog.thickness <= 0.0f)
            g_cfg.fog.thickness = 60.0f;

        g_cfg.rays.enabled = st.rays;
        if (st.rays && g_cfg.rays.strength <= 0.0f)
            g_cfg.rays.strength = 35.0f;

        // The light needs the depth buffer and the shadow map, and they cost nothing without it.
        g_cfg.volume.enabled = st.light;
        g_cfg.depth.enabled  = st.light;
        g_cfg.shadow.enabled = st.light;
        if (st.light && g_cfg.volume.strength <= 0.0f)
            g_cfg.volume.strength = 30.0f;

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
            for (int s = 0; s < kBenchSections; ++s)
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
        Log("bench: the rays, a frame: GPU %.2f ms, CPU %.2f ms", all.gpuMs[kBenchRays], all.cpuMs[kBenchRays]);

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
    }

    void Finish()
    {
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
                "rays: downscale %d, passes %d; fog thickness %.0f",
                bd.Width, bd.Height, s.volume.steps, s.volume.downscale, s.volume.smooth, s.shadow.size,
                s.shadow.mapEvery, s.rays.downscale, s.rays.passes, s.fog.thickness);

    g_saved = g_cfg;
    g_running = true;
    ShadowTiming(true);
    for (Result& r : g_results)
        r = Result();
    const double each = g_cfg.bench.settle + g_cfg.bench.measure;
    Log("bench: %d steps of %.0f s (%.0f s to settle, then %.0f s measured), %.0f s in all. Stand still, do "
        "not move the mouse, and face the sun.", kStepCount, each, g_cfg.bench.settle, g_cfg.bench.measure,
        each * kStepCount);
    ApplyStep(0);
}

bool BenchFrame(IDirect3DDevice9* dev, double frameSeconds)
{
    if (!g_running)
        return false;

    // Close this frame's queries, then read the oldest frame before its slot is used again.
    FrameQueries& cur = g_q[g_slot];
    const double now = Now();
    if (cur.freq && cur.used)
    {
        cur.freq->lpVtbl->Issue(cur.freq, D3DISSUE_END);
        cur.measured = InMeasure(now);
    }
    g_slot = (g_slot + 1) % kRing;
    FrameQueries& next = g_q[g_slot];
    if (next.freq)
        Collect(next);
    for (int s = 0; s < kBenchSections; ++s)
        next.issued[s] = false;
    next.used = next.measured = false;
    (void)dev;

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
}

bool BenchRunning()
{
    return g_running;
}

void BenchSectionBegin(IDirect3DDevice9* dev, BenchSection s)
{
    if (!g_running)
        return;
    g_cpuStart[s] = Now();
    FrameQueries& f = g_q[g_slot];
    if (f.begin[s])
        f.begin[s]->lpVtbl->Issue(f.begin[s], D3DISSUE_END);
    (void)dev;
}

void BenchSectionEnd(IDirect3DDevice9* dev, BenchSection s, bool drew)
{
    if (!g_running)
        return;
    const double now = Now();
    if (InMeasure(now))
    {
        g_cpuSum[s] += now - g_cpuStart[s];
        g_ran[s] += drew ? 1 : 0;
    }
    FrameQueries& f = g_q[g_slot];
    if (f.end[s])
    {
        f.end[s]->lpVtbl->Issue(f.end[s], D3DISSUE_END);
        f.issued[s] = true;
        f.used = true;
    }
    (void)dev;
}

void BenchReset()
{
    BenchCancel("device reset", true);
    ReleaseQueries();
}
