// World-space light shafts around the player. See beams.cpp.
#pragma once

#include <d3d9.h>

bool BeamsDraw(IDirect3DDevice9* dev, bool sunSeenThisFrame);   // at the end of the world; true if drawn
void BeamsReset();                                               // before Reset
void BeamsToggle();
void BeamsProbe();                                               // log the next draw (probe key)
