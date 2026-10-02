// bodymask: which pixels on screen show a player or a creature, for the sun shadows. See bodymask.cpp.
#pragma once

#include <d3d9.h>

void BodyMarkDraw(IDirect3DDevice9* dev);         // before each client draw in the world: the stencil mark
void BodyMarkWorldEnded(IDirect3DDevice9* dev);   // the world is drawn: build the mask, put the states back
void BodyMarkFrameEnd(IDirect3DDevice9* dev);     // at Present: a world that never ended gets its states back
IDirect3DTexture9* BodyMaskTexture();             // this frame's mask (r = 1 on a body), or null
void BodyMaskReset();                             // before Reset: drop every D3DPOOL_DEFAULT object
