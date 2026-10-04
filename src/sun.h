// Where the sun is and where the camera looks, for the shadow map and the volumetric light. See sun.cpp.
#pragma once

#include <d3d9.h>

#include <string>

void SunSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m);
void SunFrameStart();                        // a new frame: forget the last frame's sky quads
void SunSetView(const float v[3]);           // a sun-shaped sky quad in camera space; call once per quad
bool SunDirection(float dir[3]);             // world direction TO the sun; false until one is known
bool ShadowSunDirection(float dir[3]);       // the same for the shadows: [sunshadows] lock sets its tilt
bool SunSecondDirection(float dir[3]);       // at night, the moon SunDirection does not follow; false by day
bool SunCamera(D3DMATRIX& view, D3DMATRIX& proj);  // the world camera (rotation-only view), if seen
void SunStatsText(std::string& out);         // the on-screen stats (/atmos stats): the sun three ways, as name=value;
float NightWeight(float hour);               // 0 by day, 1 at night, by [night] dusk, dawn and fade
float NightScale();                          // what the rays and the light are multiplied by now: 1 by day,
                                             // [night] strength / 100 at night; 1 without the game clock
float NightScale(float percent);             // the same for another night share, in percent
