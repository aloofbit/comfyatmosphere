// sun: where the sun is and where the camera looks, for the shadow map and the volumetric light.
//
// The sun comes from the sky. The visible sun is a quad drawn in the sky, under a view whose translation is
// the sun's camera-space position (comfyfog.cpp, NoteSkySun, hands each such quad to SunSetView). By day it
// is the first draw of the frame. At night the sky draws two, the two moons, and the first one above the
// horizon is taken (PickQuad). It is turned into a world direction against the frame's camera, which is
// mirrored here from SetTransform.
// When the sun is not drawn (off screen, indoors), the last world direction carries on. [sun] fixed = 1
// replaces it with a fixed direction instead.
//
// At night the rays and the light follow a moon as if it were the sun. NightScale turns them down by
// the game clock, since the moon's height cannot tell night from day.
//
// This code was part of the screen-space sun rays (rays.cpp) until the rays were removed.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "common.h"
#include "config.h"
#include "sun.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    D3DMATRIX g_view = {}, g_proj = {};
    bool      g_haveView = false, g_haveProj = false;

    // Direction TO the sun, world space, unit length.
    float g_sunDir[3]    = { 0.0f, 0.0f, 1.0f };
    unsigned g_sunFrame  = 0;       // counts frames, so the glide below advances once a frame
    bool  g_haveSun      = false;
    // The sun-shaped quads of this frame's sky, in camera space: one by day, two at night.
    constexpr int kMaxQuads = 4;
    float g_quads[kMaxQuads][3] = {};
    int   g_nQuads       = 0;
    int   g_loggedQuads  = -1;
    int   g_quadLogs     = 0;
    float g_sunView[3]   = { 0.0f, 0.0f, 1.0f };
    bool  g_sunViewFresh = false;
    float g_secondDir[3] = { 0.0f, 0.0f, 1.0f };    // the other moon, world space, for the rays
    bool  g_haveSecond   = false;
    float g_loggedDir[3] = { 0.0f, 0.0f, 0.0f };
    int   g_sunLogs      = 0;

    // Camera space -> world space. The camera matrix is a pure rotation here (this client folds the
    // camera position into every world matrix), so its inverse is its transpose: for D3D's row vectors,
    // world[i] = sum_j view_dir[j] * V[i][j].
    bool ViewToWorld(const float v[3], float d[3])
    {
        for (int i = 0; i < 3; ++i)
            d[i] = v[0] * g_view.m[i][0] + v[1] * g_view.m[i][1] + v[2] * g_view.m[i][2];
        const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(len > 1e-4f))
            return false;
        for (int i = 0; i < 3; ++i)
            d[i] /= len;
        return true;
    }

    // Which of this frame's quads the light follows. By day there is one, the sun. At night there are
    // two: Azeroth's two moons. Both were logged at the same height (45.3 degrees at 00:40) and 92 degrees
    // apart in azimuth, so "the highest" swapped between them every few seconds and the light jumped from
    // one moon to the other. So the rule has to hold on to one: the first quad in draw order that is above
    // the horizon (the first is the larger moon: a quad of 1.8 against 1.0), or the highest when none is.
    // Logged each time the count changes, with every quad's place, so the log shows which was taken.
    void PickQuad()
    {
        const int n = g_nQuads;
        g_nQuads = 0;
        if (!g_haveView || n == 0)
            return;
        // By day (the game clock), two quads are the moons, not the sun (2026-09-30). The sky still draws
        // them by day; logged at 10:00: two quads at 31.8 degrees, 95 apart, on some frames and the sun alone
        // at 56.6 on others, by where the camera looked. The light followed the moon on those frames, and
        // the rays came from it and from "the second moon" too, so they jumped as the camera tilted. Such a
        // frame is left out: the sun carries on from its own quad.
        float hour = 0.0f;
        if (n >= 2 && ClientHour(hour) && NightWeight(hour) < 0.5f)
        {
            g_haveSecond = false;
            if (n != g_loggedQuads && g_quadLogs < 100)
            {
                ++g_quadLogs;
                g_loggedQuads = n;
                Log("sun: %d sun-shaped quads in the sky by day: the moons, left out", n);
            }
            return;
        }
        int pick = -1, highest = -1;
        float highZ = -2.0f, el[kMaxQuads] = {}, az[kMaxQuads] = {}, dirs[kMaxQuads][3] = {};
        bool ok[kMaxQuads] = {};
        for (int q = 0; q < n; ++q)
        {
            float* d = dirs[q];
            if (!ViewToWorld(g_quads[q], d))
                continue;
            ok[q] = true;
            el[q] = asinf(d[2] < -1.0f ? -1.0f : (d[2] > 1.0f ? 1.0f : d[2])) * 57.29578f;
            az[q] = atan2f(d[1], d[0]) * 57.29578f;
            if (pick < 0 && d[2] > 0.0f)
                pick = q;
            if (d[2] > highZ)
            {
                highZ = d[2];
                highest = q;
            }
        }
        if (pick < 0)
            pick = highest;
        if (pick < 0)
            return;
        memcpy(g_sunView, g_quads[pick], sizeof(g_sunView));

        // The other moon casts rays too ([rays] secondMoon): the first other quad above the horizon.
        g_haveSecond = false;
        for (int q = 0; q < n && !g_haveSecond; ++q)
            if (q != pick && ok[q] && dirs[q][2] > 0.0f)
            {
                memcpy(g_secondDir, dirs[q], sizeof(g_secondDir));
                g_haveSecond = true;
            }

        if (n != g_loggedQuads && g_quadLogs < 100)
        {
            ++g_quadLogs;
            g_loggedQuads = n;
            char line[200];
            int at = snprintf(line, sizeof(line), "sun: %d sun-shaped quad%s in the sky (azimuth/elevation):", n,
                              n == 1 ? "" : "s");
            for (int q = 0; q < n && at > 0 && at < static_cast<int>(sizeof(line)); ++q)
                at += snprintf(line + at, sizeof(line) - at, "%s %.1f/%.1f", q ? "," : "", az[q], el[q]);
            Log("%s; following number %d", line, pick + 1);
        }
    }

    void SunViewToWorld()
    {
        if (!g_haveView)
            return;
        float d[3];
        if (!ViewToWorld(g_sunView, d))
            return;

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

        // Logged whenever it moves more than ~2 degrees, as azimuth/elevation (Z up) with the local time
        // and the game's, so the log shows whether it follows the time of day.
        const float dot = d[0] * g_loggedDir[0] + d[1] * g_loggedDir[1] + d[2] * g_loggedDir[2];
        if (dot < 0.99939f && g_sunLogs < 200)
        {
            ++g_sunLogs;
            memcpy(g_loggedDir, d, sizeof(d));
            const float az = atan2f(d[1], d[0]) * 57.29578f;
            const float el = asinf(d[2] < -1.0f ? -1.0f : (d[2] > 1.0f ? 1.0f : d[2])) * 57.29578f;
            SYSTEMTIME t;
            GetLocalTime(&t);
            float hour = -1.0f;
            ClientHour(hour);
            const int gm = hour >= 0.0f ? static_cast<int>(hour * 60.0f) : -1;
            Log("sun from the sky: azimuth %.1f, elevation %.1f (dir %.3f %.3f %.3f) at %02d:%02d:%02d, "
                "game time %02d:%02d", az, el, d[0], d[1], d[2], t.wHour, t.wMinute, t.wSecond,
                gm >= 0 ? gm / 60 : -1, gm >= 0 ? gm % 60 : -1);
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
        PickQuad();
        SunViewToWorld();
    }
    if (!g_haveSun)
        return false;

    // The sprite is measured afresh every frame and the measurement is noisy: standing still, with the
    // camera still and the time pinned, the direction wandered in the fifth decimal. Everything here is
    // built around it, so that wander turned the shadow map a little each frame. Until 2026-09-29 the
    // direction was taken only once it had moved 0.05 degrees: the map then held still and jumped, and
    // about every 30 seconds the long shadows of a low sun moved some ten pixels at once. Now it glides
    // toward the measurement with a time constant of 10 seconds: the noise averages out, the real sun is
    // followed a hair each frame, and a change of more than 5 degrees (the time set by hand) lands at once.
    // The 10 seconds are [sun] glide since 2026-09-30 (the Sun Smoothing control); 0 follows at once.
    static float  stable[3] = { 0.0f, 0.0f, 0.0f };
    static bool   have = false;
    static double last = 0.0;
    static unsigned frame = ~0u;
    // Once a frame: the shadow map, the sun shadows, the light and the rays all ask, and must agree.
    if (have && frame == g_sunFrame)
    {
        memcpy(dir, stable, sizeof(stable));
        return true;
    }
    frame = g_sunFrame;
    static float  raw[3] = { 0.0f, 0.0f, 0.0f };
    static int    stepLogs = 0;
    const double  now = Now();
    // Log the measurement's own jumps, to see whether the sky moves its sun in steps.
    const float rawDot = raw[0] * g_sunDir[0] + raw[1] * g_sunDir[1] + raw[2] * g_sunDir[2];
    if (have && rawDot < 0.9999939f && stepLogs < 60)   // > 0.2 degrees in one frame
    {
        ++stepLogs;
        Log("sun: the sky's sun moved %.2f degrees in one frame, at %.1f s", acosf(rawDot > 1.0f ? 1.0f : rawDot) * 57.29578f,
            now);
    }
    memcpy(raw, g_sunDir, sizeof(raw));
    const float dot = stable[0] * g_sunDir[0] + stable[1] * g_sunDir[1] + stable[2] * g_sunDir[2];
    if (!have || dot < 0.9962f)
    {
        memcpy(stable, g_sunDir, sizeof(stable));
        have = true;
    }
    else
    {
        const double dt = now - last;
        const double glide = g_cfg.sun.glide;
        const float  k  = glide <= 0.0 ? 1.0f :
                          static_cast<float>(1.0 - exp(-(dt > 0.0 && dt < 1.0 ? dt : 0.0) / glide));
        float len = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            stable[i] += (g_sunDir[i] - stable[i]) * k;
            len += stable[i] * stable[i];
        }
        len = sqrtf(len);
        if (len > 1e-6f)
            for (int i = 0; i < 3; ++i)
                stable[i] /= len;
    }
    last = now;
    memcpy(dir, stable, sizeof(stable));
    return true;
}

// How dark it is by the game clock: 0 by day, 1 at night, a straight ramp over [night] fade hours from
// dusk and from dawn.
float NightWeight(float hour)
{
    const NightSettings& n = g_cfg.night;
    const auto since = [](float from, float to) { const float t = fmodf(to - from, 24.0f); return t < 0.0f ? t + 24.0f : t; };
    const float span = since(n.dusk, n.dawn);                    // hours from dusk to dawn
    float fade = n.fade;
    if (fade > span)         fade = span;
    if (fade > 24.0f - span) fade = 24.0f - span;
    const float t = since(n.dusk, hour);
    if (t < span)
        return fade > 0.0f && t < fade ? t / fade : 1.0f;
    const float u = t - span;                                    // hours since dawn
    return fade > 0.0f && u < fade ? 1.0f - u / fade : 0.0f;
}

float NightScale()
{
    return NightScale(g_cfg.night.strength);
}

float NightScale(float percent)
{
    const float night = percent * 0.01f;
    float hour = 0.0f;
    if (night >= 1.0f || !ClientHour(hour))
        return 1.0f;
    return 1.0f + (night - 1.0f) * NightWeight(hour);
}

// The shadows' sun: the sun, or with [sunshadows] lock the sun's azimuth at lockTilt degrees from straight
// down. With the sun overhead, whose azimuth says nothing, the client's 45 degrees.
// Below [sunshadows] riseFrom degrees the shadows' light climbs back toward riseTo as the real light sinks
// (2026-09-30), on a smoothstep: at the horizon and under it, the shadows are short, as at noon.
static bool ShadowTarget(float dir[3])
{
    if (!SunDirection(dir))
        return false;
    const SunShadowSettings& ss = g_cfg.sunShadows;
    const float h = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
    const float ax = h > 1e-4f ? dir[0] / h : 0.70710678f, ay = h > 1e-4f ? dir[1] / h : 0.70710678f;
    if (!ss.lock)
    {
        const float el = asinf(dir[2] < -1.0f ? -1.0f : dir[2] > 1.0f ? 1.0f : dir[2]) * 57.29578f;
        if (ss.riseFrom <= 0.0f || el >= ss.riseFrom)
            return true;
        float t = (ss.riseFrom - el) / ss.riseFrom;
        t = t > 1.0f ? 1.0f : t;
        t = t * t * (3.0f - 2.0f * t);
        const float lifted = el > 0.0f ? el : 0.0f;
        const float up = (lifted + ((std::max)(ss.riseTo, ss.riseFrom) - lifted) * t) * 0.01745329f;
        dir[0] = cosf(up) * ax;
        dir[1] = cosf(up) * ay;
        dir[2] = sinf(up);
        return true;
    }
    const float tilt = ss.lockTilt * 0.01745329f;
    dir[0] = sinf(tilt) * ax;
    dir[1] = sinf(tilt) * ay;
    dir[2] = cosf(tilt);
    return true;
}

// Turned toward ShadowTarget at most kTurnRate degrees a second, once a frame (2026-09-30). A moon can
// first show well above the horizon, where the shadows' light stood at riseTo, and the shadows turned at
// once: the sun's own glide in SunDirection lets a change of more than 5 degrees through at once, for the
// time set by hand. Here such a change sweeps over. A slow change passes as it is, so [sun] glide alone
// sets how far the shadows lag the sun. This was an ease of 1.5 seconds first, and with Sun Smoothing at 2
// the shadows still took about 6 seconds to settle.
bool ShadowSunDirection(float dir[3])
{
    static float    cur[3] = { 0.0f, 0.0f, 1.0f };
    static bool     have = false;
    static unsigned frame = ~0u;
    static double   last = 0.0;
    if (have && frame == g_sunFrame)
    {
        memcpy(dir, cur, sizeof(cur));
        return true;
    }
    float target[3];
    if (!ShadowTarget(target))
        return false;
    frame = g_sunFrame;
    const double now = Now();
    if (!have)
        memcpy(cur, target, sizeof(cur));
    else
    {
        constexpr float kTurnRate = 30.0f;   // degrees a second
        const double dt   = now - last;
        const float  step = kTurnRate * 0.01745329f * static_cast<float>(dt > 0.0 && dt < 1.0 ? dt : 0.0);
        float d = cur[0] * target[0] + cur[1] * target[1] + cur[2] * target[2];
        d = d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d;
        const float angle = acosf(d);
        if (angle <= step || angle < 1e-4f || sinf(angle) < 1e-4f)
            memcpy(cur, target, sizeof(cur));
        else
        {
            // Along the great circle from cur to target, by step.
            const float a = sinf(angle - step) / sinf(angle), b = sinf(step) / sinf(angle);
            for (int i = 0; i < 3; ++i)
                cur[i] = cur[i] * a + target[i] * b;
        }
    }
    have = true;
    last = now;
    memcpy(dir, cur, sizeof(cur));
    return true;
}

bool SunSecondDirection(float dir[3])
{
    if (g_cfg.sun.fixed)
        return false;
    if (g_sunViewFresh)                // this frame's quads are sorted by SunDirection
    {
        float first[3];
        SunDirection(first);
    }
    if (!g_haveSecond)
        return false;
    memcpy(dir, g_secondDir, sizeof(g_secondDir));
    return true;
}

void SunFrameStart()
{
    ++g_sunFrame;
    g_nQuads = 0;
    g_sunViewFresh = false;
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
    g_sunViewFresh = true;
    if (g_nQuads < kMaxQuads)
    {
        for (int i = 0; i < 3; ++i)
            g_quads[g_nQuads][i] = v[i] / len;
        ++g_nQuads;
    }
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
