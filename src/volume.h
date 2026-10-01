// Volumetric light: the fog glowing where the sun reaches it. See volume.cpp.
#pragma once

#include <d3d9.h>

#include <string>

bool VolumeDraw(IDirect3DDevice9* dev);   // at the end of the world, after the shadow map; true if drawn
void VolumeReset();                       // before Reset
void VolumeToggle();
void VolumeProbe();                       // log the next draw
bool VolumeActive();                      // is the light actually drawing? the shadow map is for it
void VolumeFrameEnd();                    // at Present: count the frame, log the draw/skip summary

// The fog, for the lamps (lampglow.cpp), as this frame's fog pass found it: its density per yard at the
// ground, dawn included (0 when the fog is off), and how many times that it holds at a camera-relative point.
float FogDensityNow();
float FogThicknessAt(const float rel[3]);
void VolumeStatsText(std::string& out);   // the on-screen stats (/atmos stats): the fog's figures, as name=value;
