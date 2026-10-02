// The benchmark: each feature in turn, and what it costs. See bench.cpp.
#pragma once

#include <d3d9.h>

enum BenchSection { kBenchShadow, kBenchVolume, kBenchRays, kBenchLamps, kBenchSunShadows, kBenchSections };

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
