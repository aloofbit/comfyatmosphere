// The terrain's baked shadow, weakened while the sun shadows draw. See terrainshade.cpp.
#pragma once

#include <d3d9.h>

// The shader to bind in place of ps: a copy of the client's terrain shader that keeps `keep` (0..1) of
// the baked shadow, or ps itself when ps is not a terrain shader or keep is 1.
IDirect3DPixelShader9* TerrainShadeSwap(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, float keep);
void TerrainShadeReset();   // before Reset, and for a new device
