// sun: where the sun is and where the camera looks, for the shadow map and the volumetric light.
//
// The sun comes from the sky. The visible sun is the first draw of each frame, a quad under a view whose
// translation is the sun's camera-space position (comfyfog.cpp, NoteSkySun, hands it to SunSetView). It
// is turned into a world direction against the frame's camera, which is mirrored here from SetTransform.
// When the sun is not drawn (off screen, indoors), the last world direction carries on. [sun] fixed = 1
// replaces it with a fixed direction instead.
//
// This code was part of the screen-space sun rays (rays.cpp) until the rays were removed.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "common.h"
#include "config.h"
#include "sun.h"

#include <cmath>
#include <cstring>

namespace
{
    D3DMATRIX g_view = {}, g_proj = {};
    bool      g_haveView = false, g_haveProj = false;

    // Direction TO the sun, world space, unit length.
    float g_sunDir[3]    = { 0.0f, 0.0f, 1.0f };
    bool  g_haveSun      = false;
    float g_sunView[3]   = { 0.0f, 0.0f, 1.0f };
    bool  g_sunViewFresh = false;
    float g_loggedDir[3] = { 0.0f, 0.0f, 0.0f };
    int   g_sunLogs      = 0;

    // Camera space -> world space. The camera matrix is a pure rotation here (this client folds the
    // camera position into every world matrix), so its inverse is its transpose: for D3D's row vectors,
    // world[i] = sum_j view_dir[j] * V[i][j].
    void SunViewToWorld()
    {
        if (!g_haveView)
            return;
        float d[3];
        for (int i = 0; i < 3; ++i)
            d[i] = g_sunView[0] * g_view.m[i][0] + g_sunView[1] * g_view.m[i][1] + g_sunView[2] * g_view.m[i][2];
        const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(len > 1e-4f))
            return;
        for (int i = 0; i < 3; ++i)
            d[i] /= len;

        // A big jump has to hold for a few frames before it is believed. The sprite match now and then
        // catches another sky quad for a single frame (logged: 75.8 -> 25.8 -> 75.8 degrees within one
        // second), and every sun-driven pass (shadow map, volume) flashed with it. Real jumps (the time
        // stepped with Ctrl+PageUp) still land, a sixth of a second late; small drift follows at once.
        static int   pending = 0;
        static float pendDir[3] = {};
        if (g_haveSun && d[0] * g_sunDir[0] + d[1] * g_sunDir[1] + d[2] * g_sunDir[2] < 0.9962f)   // > 5 degrees
        {
            const bool same = d[0] * pendDir[0] + d[1] * pendDir[1] + d[2] * pendDir[2] > 0.9962f;
            pending = same ? pending + 1 : 1;
            memcpy(pendDir, d, sizeof(d));
            if (pending < 10)
                return;
        }
        pending = 0;
        memcpy(g_sunDir, d, sizeof(d));
        g_haveSun = true;

        // Logged whenever it moves more than ~2 degrees, as azimuth/elevation (Z up) with the local time,
        // so the log shows whether it follows the time of day.
        const float dot = d[0] * g_loggedDir[0] + d[1] * g_loggedDir[1] + d[2] * g_loggedDir[2];
        if (dot < 0.99939f && g_sunLogs < 200)
        {
            ++g_sunLogs;
            memcpy(g_loggedDir, d, sizeof(d));
            const float az = atan2f(d[1], d[0]) * 57.29578f;
            const float el = asinf(d[2] < -1.0f ? -1.0f : (d[2] > 1.0f ? 1.0f : d[2])) * 57.29578f;
            SYSTEMTIME t;
            GetLocalTime(&t);
            Log("sun from the sky: azimuth %.1f, elevation %.1f (dir %.3f %.3f %.3f) at %02d:%02d:%02d",
                az, el, d[0], d[1], d[2], t.wHour, t.wMinute, t.wSecond);
        }
    }
}

bool SunDirection(float dir[3])
{
    // [sun] fixed = 1 pins the sun for the shadow map and the volumetric light, whatever the game's clock
    // says.
    const SunSettings& s = g_cfg.sun;
    if (s.fixed)
    {
        const float az = s.azimuth * 0.01745329f, el = s.elevation * 0.01745329f;
        dir[0] = cosf(el) * cosf(az); dir[1] = cosf(el) * sinf(az); dir[2] = sinf(el);
        return true;
    }
    if (g_sunViewFresh)
    {
        g_sunViewFresh = false;
        SunViewToWorld();
    }
    if (!g_haveSun)
        return false;

    // The sprite is measured afresh every frame and the measurement is noisy: standing still, with the
    // camera still and the time pinned, the direction wandered in the fifth decimal. Everything here is
    // built around it, so that wander turned the shadow map a little each frame. The direction is taken
    // only once it has moved 0.05 degrees, which a minute of sun passes easily.
    static float stable[3] = { 0.0f, 0.0f, 0.0f };
    static bool  have = false;
    const float dot = stable[0] * g_sunDir[0] + stable[1] * g_sunDir[1] + stable[2] * g_sunDir[2];
    if (!have || dot < 0.9999996f)
    {
        memcpy(stable, g_sunDir, sizeof(stable));
        have = true;
    }
    memcpy(dir, stable, sizeof(stable));
    return true;
}

bool SunCamera(D3DMATRIX& view, D3DMATRIX& proj)
{
    if (!g_haveView || !g_haveProj)
        return false;
    view = g_view;
    proj = g_proj;
    return true;
}

void SunSetView(const float v[3])
{
    const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len > 1e-4f))
        return;
    for (int i = 0; i < 3; ++i)
        g_sunView[i] = v[i] / len;
    g_sunViewFresh = true;
}

void SunSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
{
    if (!m)
        return;

    // Only the world camera matters. The UI sets its own transforms after the world, so an identity view
    // or an orthographic projection is ignored rather than allowed to replace the camera. So is any view
    // with a translation: the world camera never has one in this client, and the sky's sun sprite is
    // drawn under a special view whose translation IS the sun's position. Taking that for the camera
    // would turn every later sun direction wrong.
    if (state == D3DTS_VIEW)
    {
        bool identity = true;
        for (int r = 0; r < 4 && identity; ++r)
            for (int c = 0; c < 4; ++c)
                if (fabsf(m->m[r][c] - (r == c ? 1.0f : 0.0f)) > 1e-5f) { identity = false; break; }
        const bool translated = fabsf(m->m[3][0]) > 1e-3f || fabsf(m->m[3][1]) > 1e-3f || fabsf(m->m[3][2]) > 1e-3f;
        if (!identity && !translated)
        {
            g_view     = *m;
            g_haveView = true;
        }
    }
    else if (state == D3DTS_PROJECTION)
    {
        const bool perspective = fabsf(m->m[2][3] - 1.0f) < 1e-3f && fabsf(m->m[3][3]) < 1e-3f;
        if (perspective)
        {
            g_proj     = *m;
            g_haveProj = true;
        }
    }
}
