// The benchmark: each feature in turn, and what it costs. See bench.cpp.
#pragma once

#include <d3d9.h>

// The sections before kBenchTop are our passes, one after another, and their sum is our GPU time. The rest
// are parts of one of them or span them (2026-10-06): the seven shadow maps inside kBenchShadow, and the
// GPU's whole frame from the first BeginScene to Present.
enum BenchSection
{
    kBenchShadow, kBenchVolume, kBenchRays, kBenchLamps, kBenchSunShadows, kBenchMask, kBenchBeacon, kBenchGrade,
    kBenchTop,
    kBenchMapFar = kBenchTop, kBenchMapFarLeaf, kBenchMapNear, kBenchMapNearLeaf, kBenchMapMid, kBenchMapUnits,
    kBenchMapTerrain,
    kBenchWaterPrep,      // the water's copies and the wet sand, at its first chunk
    kBenchWaterSpan,      // from the water's first chunk to the world's end: its draws, the game's among them
    kBenchFrame,
    kBenchSections
};

// Our CPU time outside our passes (2026-10-06), timed only while the bench or the frame log runs: our work in
// the hooks on the client's draws, without the draws themselves, and the water's part of that. In time stamp
// counter ticks (__rdtsc), which cost a few nanoseconds to read; the hooks run thousands of times a frame.
// The water's own split (2026-10-06): reading the game's indices for the wet cells, and our draws over each chunk.
enum BenchCpu { kCpuHooks, kCpuWater, kCpuWaterCells, kCpuWaterDraws, kCpuParts };
bool BenchTiming();                                       // the bench or the frame log is running
void BenchCpuAddTicks(BenchCpu part, unsigned long long ticks);
unsigned long long BenchSectionTicks();                   // ticks spent in our passes so far (a running sum)
void BenchFrameBegin(IDirect3DDevice9* dev);              // the first BeginScene of a frame

void BenchStart(IDirect3DDevice9* dev);                    // Alt + the probe key
bool BenchFrame(IDirect3DDevice9* dev, double frameSeconds);   // at Present; true when it changed g_cfg
void BenchCancel(const char* why, bool restore);          // restore = false: g_cfg was rebuilt by the caller
bool BenchRunning();
void BenchSectionBegin(IDirect3DDevice9* dev, BenchSection s);   // around our own passes
void BenchSectionEnd(IDirect3DDevice9* dev, BenchSection s, bool drew);   // drew: the pass drew something
void BenchReset();                                        // before Reset: the queries go

// The frame log (/atmos framelog, 2026-10-02): every frame's time and our CPU share of it for a number of
// seconds, then the slowest frames and what was done in each. For the camera-turn test (wow-test-tool): the
// benchmark stands still, and the frame rate drops when the camera turns.
void FrameLogStart(double seconds);
void FrameLogFrame(double frameSeconds);                  // at Present
bool FrameLogRunning();
