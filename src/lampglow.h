// The fog glowing around lamps, lanterns and torches. See lampglow.cpp.
#pragma once

#include <d3d9.h>

bool LampGlowActive();                     // should the lights be tracked and the glow drawn this frame?
bool LampGlowDraw(IDirect3DDevice9* dev);  // before the UI, after the volumetric light; true if drawn
void LampGlowReset();                      // before Reset
void LampGlowProbe();                      // log the next draw

// The fog the world was drawn with, as sent to the device (comfyfog.cpp): false before the client set one.
bool WorldFog(float& start, float& end);
