// Sun shafts: a post-process pass run from the Present hook. See rays.cpp.
#pragma once

#include <d3d9.h>

bool RaysBeforeUI(IDirect3DDevice9* dev);    // at the world -> UI boundary; true if the pass ran
void RaysPresent(IDirect3DDevice9* dev);     // before the client's frame is shown; ends the frame
void RaysReset();                            // before Reset: drop every D3DPOOL_DEFAULT object
void RaysReload();                           // after the ini is reloaded
void RaysToggle();
void RaysProbe();                            // log this frame's brightest pixel (probe key)
void RaysSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m);
void RaysSetSunView(const float v[3]);       // the sky sun's position in camera space, this frame
bool RaysSunDirection(float dir[3]);         // world direction TO the sun; false until one is known
bool RaysCamera(D3DMATRIX& view, D3DMATRIX& proj);  // the world camera (rotation-only view), if seen
