// Sun rays: a post-process pass at the world -> UI boundary. See rays.cpp.
#pragma once

#include <d3d9.h>

bool RaysBeforeUI(IDirect3DDevice9* dev);    // at the world -> UI boundary; true if rays were drawn
void RaysBeforeClouds(IDirect3DDevice9* dev); // the sky is drawn and the clouds are next: keep the sky
bool RaysPresent(IDirect3DDevice9* dev);     // before the client's frame is shown; ends the frame
void RaysReset();                            // before Reset: drop every D3DPOOL_DEFAULT object
void RaysReload();                           // after the ini is reloaded
void RaysToggle();
void RaysProbe();                            // log this frame's brightest pixel (probe key)
