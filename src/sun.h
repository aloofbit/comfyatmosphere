// Where the sun is and where the camera looks, for the shadow map and the volumetric light. See sun.cpp.
#pragma once

#include <d3d9.h>

void SunSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m);
void SunSetView(const float v[3]);           // the sky sun's position in camera space, this frame
bool SunDirection(float dir[3]);             // world direction TO the sun; false until one is known
bool SunCamera(D3DMATRIX& view, D3DMATRIX& proj);  // the world camera (rotation-only view), if seen
