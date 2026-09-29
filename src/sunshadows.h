// Sun shadows on the world, from the volumetric light's shadow map. See sunshadows.cpp.
#pragma once

#include <d3d9.h>

bool SunShadowsDraw(IDirect3DDevice9* dev);   // before the volumetric light; true if drawn
void SunShadowsReset();                        // before Reset, and for a new device
void SunShadowsProbe();                        // log the next draw
