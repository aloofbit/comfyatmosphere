// Volumetric light: the fog glowing where the sun reaches it. See volume.cpp.
#pragma once

#include <d3d9.h>

void VolumeDraw(IDirect3DDevice9* dev);   // at the end of the world, after the shadow map is drawn
void VolumeReset();                       // before Reset
void VolumeToggle();
void VolumeProbe();                       // log the next draw
bool VolumeActive();                      // is the light actually drawing? the shadow map is for it
void VolumeFrameEnd();                    // at Present: count the frame, log the draw/skip summary
