// bodymask: which pixels on screen show a player or a creature, for the sun shadows. See bodymask.cpp.
#pragma once

#include <d3d9.h>

void BodyMarkDraw(IDirect3DDevice9* dev);         // before each client draw in the world: the stencil mark
void BodyMarkWorldEnded(IDirect3DDevice9* dev);   // the world is drawn: build the mask, put the states back
void BodyMarkFrameEnd(IDirect3DDevice9* dev);     // at Present: a world that never ended gets its states back
IDirect3DTexture9* BodyMaskTexture();             // this frame's mask (r = 1 on a body), or null
// Foliage and the ground so far this world (b = 1 where an alpha-tested model is in front, reeds and leaves; r = 1
// where the terrain is), built now into the mask's texture, for the water's foam round objects; null when there is
// no mark this frame. The world's end builds the mask again over it.
IDirect3DTexture9* BodyMaskLeavesNow(IDirect3DDevice9* dev);
// The mark is in the stencil now (bit 0x80 on a body drawn so far this world), for a pass that skips bodies.
bool BodyMarkLive(DWORD& bit);
void BodyMaskReset();                             // before Reset: drop every D3DPOOL_DEFAULT object
void BodyMaskProbe();                             // log the next three frames' model draws near the player
