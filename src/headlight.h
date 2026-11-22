// Headlights: a cone of light from a mount's lamp, at night. A pass of its own before the UI. See headlight.cpp.
#pragma once

#include <d3d9.h>

bool HeadlightDraw(IDirect3DDevice9* dev);   // after the lighthouses, before the UI; true if it drew
void HeadlightProbe();                       // log the next draw's lights
void HeadlightReset();                       // before Reset: drop every D3DPOOL_DEFAULT object
