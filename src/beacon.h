// Lighthouses: a beacon in the lamp room and a turning beam, at night. A pass of its own before the UI. See
// beacon.cpp.
#pragma once

#include <d3d9.h>

bool BeaconDraw(IDirect3DDevice9* dev);   // after the lamps, before the UI; true if it drew
void BeaconProbe();                       // log the next draw's lighthouses
// During a probe: a client draw is logged when it stands within 25 yards of a lighthouse's lamp, to find the
// game's own light model there.
// ffPlace: a fixed-function draw's world matrix's place, camera-relative; null for a draw through a vertex shader.
void BeaconProbeDraw(IDirect3DDevice9* dev, const char* kind, unsigned prims, unsigned verts, const float* ffPlace);
void BeaconReset();                       // before Reset: drop every D3DPOOL_DEFAULT object
// The lighthouses' lamps found last frame, in the world, for the lamps' pass to light the surfaces round them.
int BeaconLamps(float (*out)[3], int max);
// How strong the lighthouse's glitter on the water is now: [lighthouse] glint x beacon x its day-and-night share.
float BeaconGlint();
// The nearest lighthouse's beam's way across the ground now (x, y, unit), as the pass draws it; true with a second
// beam opposite. False when there is none.
bool BeaconBeamWay(float way[2], bool& two);
// Whether a client draw is the game's own lighthouse light, to be left out ([lighthouse] hideGameLight): an
// alpha-blended model draw writing no depth, within 5 yards of a LIGHTHOUSEEFFECT's origin.
bool BeaconSkipsDraw(IDirect3DDevice9* dev);
