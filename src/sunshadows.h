// Sun shadows on the world, from the volumetric light's shadow map. See sunshadows.cpp.
#pragma once

#include <d3d9.h>

bool SunShadowsDraw(IDirect3DDevice9* dev);   // before the volumetric light; true if drawn
void SunShadowsReset();                        // before Reset, and for a new device
void SunShadowsProbe();                        // log the next draw

// The sky near the horizon darkened (or tinted) by as much as [fog] shapes the fog's colour, so fogged
// ground and trees meet a sky of the same colour. Before the sun shadows; true if drawn.
bool SkyMatchDraw(IDirect3DDevice9* dev);

// In comfyfog.cpp: the fog colour the world last set (not black), as the client gave it and as [fog]
// darken, desaturate and tint shape it.
bool WorldFogColor(DWORD& client, DWORD& shaped);
bool WorldFog(float& start, float& end);   // the world's fog distances, as [fog] remaps them
bool OwnFogActive();                       // our own fog draws this frame, and the game's is off

// Our own fog, over the picture (see sunshadows.cpp): colour as 0xRRGGBB, the height fog's density per
// yard, and the distance fog's start and end in yards (the game's, as the dial moves them; 0 = none).
bool FogDraw(IDirect3DDevice9* dev, DWORD colour, float density, float distStart, float distEnd);
