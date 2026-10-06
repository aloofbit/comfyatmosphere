// Sun shadows on the world, from the volumetric light's shadow map. See sunshadows.cpp.
#pragma once

#include <d3d9.h>

bool SunShadowsDraw(IDirect3DDevice9* dev);   // before the volumetric light; true if drawn
void SunShadowsReset();                        // before Reset, and for a new device
void SunShadowsProbe();                        // log the next draw
float SunShadowsShare();                       // 0..1: how much the sun shadows drew last frame (the sun's
                                               // height and [night] strength), 0 when they did not

// In comfyatmos.cpp: the game's fog as the world last set it. The colour is the last one that was not black.
bool WorldFogColor(DWORD& color);
bool WorldFog(float& start, float& end);   // the world's fog start and end, in yards
