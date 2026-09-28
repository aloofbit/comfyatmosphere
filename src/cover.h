// How much of the sun is in view on screen, for the sun rays and the volumetric light. See cover.cpp.
#pragma once

#include <d3d9.h>

// One eased value for each user. The rays use one per sun, the light its own.
enum CoverSlot { kCoverRaysSun, kCoverRaysMoon, kCoverVolume, kCoverSlots };

struct CoverInput
{
    float px, py;                 // the sun on screen, texture space (y down)
    bool  test;                   // false: no test, the sun counts as in view (off screen, debug views)
    float aspect;                 // back buffer width / height
    IDirect3DTexture9* depth;     // the world's depth (depth.cpp), or null: then by brightness
    IDirect3DTexture9* sky;       // the sky before the clouds (rays.cpp), or null
    IDirect3DTexture9* scene;     // the finished frame, for the brightness test; null = no brightness test
};

// Measures on the GPU and eases the result into the slot's 1x1 texture, which it returns (r = 0..1) for a
// pixel shader to multiply by. Nothing is read back. Changes the render target, the textures and sampler
// states of stages 0 and 1, the pixel shader and its constants c0..c9, the vertex shader (null), the FVF
// alpha blending (left off) and colour writes (left all on): the caller sets what it needs afterwards.
// Null if it cannot run.
IDirect3DTexture9* CoverMeasure(IDirect3DDevice9* dev, CoverSlot slot, const CoverInput& in, float easeTime);
void CoverProbe();                // log the next measure of each slot, with the value read back
void CoverReset();                // before Reset: drop every D3DPOOL_DEFAULT object
