#pragma once
// The command stream (perf branch, 2026-10-21): the game's thread records its D3D9 calls into a queue and a thread
// of our own replays them on DXVK, as Northlight's CommandStream does (github.com/lepola/northlight, MIT). See the
// notes at the top of stream.cpp.

#define CINTERFACE
#include <d3d9.h>

// At attach, with the probe device, before comfyatmos hooks the same slots: the stream's wrappers go innermost.
// Reads [stream] enabled from the ini; with 0 nothing is patched.
void StreamInstall(IDirect3DDevice9* probe);

// True once the stream records the game's calls (after the first Present of the game's device).
bool StreamActive();

// Waits until the replay thread has run everything recorded so far. For a read of the GPU's results.
void StreamDrain();

// Once a frame, from Present: the frame's numbers go to the log every few seconds when [stream] stats is 1.
void StreamFrameStats();
