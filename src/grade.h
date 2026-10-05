// Colour saturation by day and by night: a pass at the world -> UI boundary. See grade.cpp.
#pragma once

#include <d3d9.h>

bool GradeBeforeUI(IDirect3DDevice9* dev);   // after the rays, before the UI; true if the pass drew
void GradeReset();                           // before Reset: drop every D3DPOOL_DEFAULT object
float GradeSaturationNow();                  // the saturation in use now, 1 = the game's own
