# ComfyFogAndRays design notes

**Status: working in game, all in one DLL (`comfyfog.dll`, sources in `src/`).** Fog, screen-space sun
rays, volumetric light (depth + sun shadow map + ray-march), and cloud removal, with controls in the game's
options through the ComfyAtmosphere addon. The sun rays (`rays.cpp`) were removed on 2026-09-23 as a step the
volumetric light had replaced, and put back on 2026-09-24 because players asked for them. They are cheap and
need no depth, so they are on by default and the light is not. Shafts need a large bright source that
trees and ridges cut into, and the bright sky around the sun is that source: at `relThreshold` 0.75 only
the sun and its halo cast, and a ridge in front of the sun gave one smooth fan (`debugView = 1` showed
it). 0.35 lets the sky cast. Lit clouds then cast too, and glow around the sun. `[rays] skyOnly = 1`
casts from the image kept just before the cloud draw (a probe shows the order: sun sprite, sky dome
added, cloud layer alpha-blended, then the world), per pixel the darker of that and the finished frame.
It was first judged broken, because only the sun disk cast. The kept image is also free of the client's
Full-Screen Glow, which brightens everything near the sun in the finished frame, so its sky is dimmer and
needs a lower `relThreshold`. `debugView = 3` shows the kept image. The defaults since 2026-09-24,
chosen in game: `skyOnly = 1` and `relThreshold = 0.20`. At 0.35 the kept sky only just cast (about 10%
strength in the mask); at 0.20 it casts about three times as strongly. 0.20 is also the absolute floor
(`threshold`), so a lower value changes nothing. The sun direction and the camera they
tracked stay in `sun.cpp`, which all three passes share. Time-of-day control, first
built here, is now its own DLL: comfytime (https://github.com/aloofbit/comfytime). Everything is tuned from
`comfyfog.ini` and reloads with F11. The sections after *What was found* are the original feasibility
write-up, kept for the reasoning. Where they disagree with *What was found*, *What was found* is what
measurement showed.

| Piece | File | Keys |
| --- | --- | --- |
| Fog: one `thickness` dial, haze floor + gentler climb | `comfyfog.cpp` | Shift+F11 toggle |
| Sun direction and world camera, for the shadow map and the light | `sun.cpp` | none |
| Sun rays: radial blur of the bright sky toward the sun | `rays.cpp` | Ctrl+F11 toggle |
| Volumetric light: fog lit by the sun, shaded by the shadow map | `volume.cpp` (+ `depth.cpp`, `shadow.cpp`) | Alt+F11 toggle |
| Clouds off: `[sky] clouds = 0` | `comfyfog.cpp` | none |
| In-game controls: CVars for the addon's sliders, and the quality levels | `cvars.cpp`, `config.cpp` | none |
| Diagnostics | all | F12: one-frame probe, then a 180-frame trace of the light and the map |
| Lamp probe: where local lights can be found (street lamps, lanterns, torches) | `lamps.cpp` | F12, with the frame probe: a 60-frame report |
| Lamps: the fog glows around lamps and torches, and lampposts light the surfaces near them | `lamps.cpp` (tracker), `lampglow.cpp` | none; `[lamps]` in the ini |
| Sun shadows on the world, from the light's shadow map and its near map | `sunshadows.cpp` (+ `shadow.cpp`) | none; `[sunshadows]` in the ini |
| Benchmark: each feature in turn, frame rate and our own GPU and CPU time | `bench.cpp` | Alt+F12 |

## What was found (measured in this `WoW.exe`)

**Fog.** Linear (`FOGVERTEXMODE` 3), set again many times a frame (zone colour ↔ black for additive
passes). M2s (trees, characters) fog in their vertex shaders from `c30 = (-1/(end-start), end/(end-start))`.
All 19 shaders that fog read nothing else. comfyfog waits for comfygrass to patch first and chains on top,
so grass sees the rewritten fog.

**World → UI boundary.** The first switch from a perspective to an orthographic projection each frame,
after some world has been drawn (the client also switches at draw 0 with nothing drawn). `ZENABLE` is never
set, so it is no marker. With **Full Screen Glow** on (the default), the world is drawn into an off-screen
render target. After the switch, the client blurs it and draws world + glow onto the back buffer in one
unblended, pixel-shaded draw. Rays drawn at the switch were painted over. They then ran before the first
back-buffer draw with no pixel shader (the first UI draw, glow on or off), until they were removed. The
world end is still found here, for depth, shadows and the light.

**The sun is not the directional light.** The world light is a fixed direction (azimuth 45°, elevation
28°, orange) that never follows the time. The visible sun is the **first draw of the frame**: a unit quad
(4 vertices, strip, identity world, no depth writes) under a special view matrix whose *translation* is
the sun's camera-space position. The world camera never has a translation (this client folds the camera
into every world matrix), so views with one are excluded from the camera mirror.

**The sky** is the first three draws: sun quad, untextured additive dome (sky colour), textured
alpha-blended strip (~177 vertices, the clouds). All leave depth writes off; the first depth-writing draw
is terrain. Clouds are skipped by that rule. An identity-world test never matched.

**Game time** (now in comfytime; found by the Ctrl+F12 search against the minimap clock): `0x00CE9B60` int
minutes, `0x00CE9B64` float day fraction (continuous), `0x00CE8574` float minutes. In the world the client
writes them every frame between `BeginScene` and `Present`, so comfytime writes the chosen time at `Present`
and again at `BeginScene`, before the sky reads it. A second pair at `0x00CE9D00/04` runs at the same rate, 78 minutes behind. It is
not understood and not written.

**CVars, for the in-game controls.** Found by disassembling the Lua `RegisterCVar` (`0x00488B00`) and
`GetCVar` (`0x00488BA0`), which the Lua function table at `0x0083DEE0` names.
`0x0063DEC0` is `CVar* __fastcall Lookup(name)`. It returns 0 for an entry whose flags at `+0x1C` do not
have `0x80000000` set, which is an entry not registered yet. `0x0063DB90` is `CVar* __fastcall Register(name,
help, flags, default, callback, category, arg5, cbArg)` and ends `ret 0x18`. The Lua `RegisterCVar` calls it
as `(name, 0, 0, default, 0, 9, 0, 0)` only when `Lookup` misses. `arg5 = 0` is what sets `0x80000000`.
The value string is at `+0x20`. The Lua `RegisterCVar` refuses a 31st addon CVar (a count at `0x00B4E3C8`);
a DLL registration does not count toward it. `GetCVar` on a missing name raises a Lua error, not `nil`.
The client saves these CVars to `Config.wtf` itself, and only the values that differ from the default
(measured: `comfyFogThickness 85` was written, `comfyFog` at its default was not), so the addon keeps no
copy of its own.
**When to register matters.** Registered on the first frame after the CVar table existed, one start went
white: the registration fell before the login screen had drawn, and the client drew nothing after it. The
next start registered after the login screen and ran normally. The DLL now waits until the client's Lua
state (`[0x00CEEF74]`, read by `0x007040D0`) has been non-zero for one second, which is where an addon's
`RegisterCVar` would run. The cause of the white window is not known.
The Turtle options window (`Interface\FrameXML\OptionsFrame.lua` in `patch-9.mpq`) calls `SetCVar` on
every slider move and `GetCVarDefault` for its Defaults button.

**Why screen rays alone were not enough.** They are rebuilt from the image each frame, so they swing as the
camera moves. A `parallel` blend was simulated and swings *more* (75° vs 58° over a ±40° pan), because the fan
toward the sun is already the correct perspective of parallel shafts. What steadied them: a brightness
reference eased over time (`adaptTime`), intensity by view angle (`viewFalloff`), a sharper falloff around the
sun (`falloff`), and brightness relative to the frame's peak (`relThreshold`). Under a foggy canopy nothing
reaches a fixed threshold (the fog colour itself sat at 0.33). The world shafts (`beams.cpp`, since removed;
volumetric light replaced them) answered "stays put while I look around": world-grid placement, depth-tested
at the end of the world pass, screen-blended so they vanish against bright sky, and faded out in the open by a
canopy estimate from the top half of the frame.

Goal: fog control and sun shafts (crepuscular rays) for the 1.12 client, in the same shape as
[`comfygrass`](../comfygrass): a DLL loaded by VanillaFixes from `dlls.txt`, hooking DXVK's
`IDirect3DDevice9` vtable, with everything tunable from an ini and reloadable in game.

Read [`comfygrass/src/README.md`](../comfygrass/src/README.md) before starting. It has the attach
mechanism, the reversed addresses and how each was verified, and four architectures that did not work.

## The shadow map, and what made the light flicker

The volumetric light blinked off on single frames and jittered as you walked. Five separate causes, all
found by tracing 180 frames at a time (F12, below) rather than by looking:

**The far horizon is not the world.** The client draws it with a camera of its own (near 467, far 2112
here) into its own depth slice, 0.9550..0.9600. Taken out of the camera with the WORLD camera, those
draws land at the wrong scale, and one frame in a handful the map filled with a caster near the sun and
the light went out. Fixed-function draws carry their own world matrix and convert correctly whatever
projection drew them, so they are kept (`[shadow] horizon`): a ridge between you and the sun now shades
you, which is what `depth` 700 is for. Shader draws fold the camera into c2..c5 and are left out.

**The world camera is voted on, and the vote could flip.** On a frame where the world's own draws thinned
out, the horizon camera could win it, and every cached caster was then re-expressed through the wrong
camera: hundreds of entries re-added in the wrong places, and no light that frame. The vote now only
counts draws inside the world's depth slice, and the camera in use is kept unless another takes twice its
votes.

**A cached entry is a pointer, not a copy.** It holds the buffer, the index range and the transform, and
the client re-fills its buffers, including later in the same frame: measured, 62 entries overwritten in
one trace, 75 a frame beside the abbey. Replaying them drew whatever had taken their place, which is the
spikes that fanned out of a building. Locks are hooked on DXVK's shared vertex- and index-buffer vtables,
every write is numbered with the byte range it covers, and an entry goes as soon as a later write lands
on its own vertices. Geometry the client streams through an arena (that building) is therefore gone by
the end of the world pass and casts nothing. Drawing it into the map as the client draws it was tried:
it works, but it costs a save and restore of device state per draw, up to 75 a frame, and it showed
artefacts from geometry recorded mid-frame. Not worth it for the shade of one building.

**Anything that moves faster than three yards a frame looked like a new object.** A bird flying past left
a new caster every frame, a trail of birds shading the air until they aged out. An instance of the same
model that the client did not draw this frame, within 60 yards, is now taken to be it, moved, and an
entry matched that way is dropped the moment the client stops drawing it.

**The sun wobbled.** It comes from the sprite the client draws, measured afresh each frame, and standing
still with the time pinned it wandered in the fifth decimal. The map is built around it, so the whole map
turned a little every frame and everything in it shifted. The direction is taken only when it has really
moved, 0.05 degrees. `[shadow] snap` holds the map on whole texels of its own grid as well; it is off by
default, because it steps visibly as you walk and the sun fix removed most of what it was for.

**The client streams its terrain and buildings through a buffer it re-fills.** A cached entry is a
pointer into that buffer, so it holds someone else's geometry by the end of the frame: the abbey cast
nothing, and a mountain 150 yards away flickered as the odd frame survived. The vertices themselves do
not change, so a chunk is copied once into a buffer of our own, keyed by its size and where it stands
rather than by its address, which is different every frame. Reading the client's memory back is slow, so
`copyPerFrame` (2) are taken a frame and `copyMax` (768) are kept; the shade fills in over a few seconds
and then stays. Measured at the abbey: entries overwritten under us fell from about 100 a frame to 11.

**What the map cannot see is not drawn into it, but only for the models.** Replaying the whole cache cost
6 to 9 ms of CPU a frame, which the game felt. A model's own point sits on its geometry, so it culls
honestly; one terrain chunk covers so much ground that its point can sit outside the map while its
geometry crosses the middle, and culling those took the shade out from under that mountain. Terrain and
buildings are a couple of hundred entries, so they are always drawn.

**The map is only for the light.** With volumetric light off, the map was still being built: Alt+F11
looked like it saved nothing because the expensive half was still running.

The rest of what the cost report shows, measured in Elwynn with the settings the ini ships with: the
light costs little against the client's own 17 to 21 ms a frame, `mapEvery` 2 halves the replay for no
visible difference, and `cacheTime` is worth more than it looks: 30 seconds of history held 5000 entries
where 10 seconds holds 600, because a tree is several batches and every level of detail is its own.

What the light still does not do: the bias grows with how steeply a line of sight runs into the map,
without which a low sun flickered badly around itself.

## Volumetric Light Quality (2026-09-24)

Players reported low frame rates with the light on, so the light got a quality control: a three-position
slider in Video > Shaders (`comfyVolumeQuality`, `[volume] quality`). High uses the ini's values as they
are, so hand tuning keeps working. Medium and Low replace four values (`ApplyVolumeQuality`, applied
after the controls are laid over the ini):

| | Low | Medium | High |
| --- | --- | --- | --- |
| `[volume] steps` | 32 | 48 | ini (64) |
| `[volume] downscale` | 3 | 2 | ini (2) |

It also set `[shadow] size` (1024 at Low and Medium) and `[shadow] mapEvery` (4 and 3) until 2026-09-29,
when those got their own controls, Shadow Resolution and Shadow Redraw.

A slider, not a dropdown: the options window labels a slider's ends Low and High when it has no
`numberLabels` (OptionsFrame.lua in patch-9.mpq), and a dropdown needs menu code of its own. Not yet
measured with the benchmark.

## Jitter in the light while walking (2026-09-24)

The light was steady standing still and jittered while walking. The shadow map view (`[volume] debug =
6`) showed where: a mountain slid smoothly with the player, a tower 200 yards away jumped. Traces (F12 with
`[general] trace = 1`) found three causes, fixed in this order:

**Streamed terrain blinked.** An arena chunk was only recognised as streamed when the client had written
its buffer earlier in the same frame. Terrain written into the arena in an earlier frame and drawn from it
later was cached as a plain pointer, dropped as soon as the client refilled the buffer, and back the next
frame: 8 to 12 entries a frame, up to 168 at once, all from one buffer. A buffer written in two frames or
more now counts as streamed, so it gets a copy. That made copies the bottleneck, so `copyPerFrame` went
from 2 to 16 and `copyMax` from 768 to 2048.

**The tower's transform carried the projection's error.** Some models upload only the projection in c2..c5
and carry world and view in their bones. Their absolute transform was found by dividing the world
camera's projection out, and the two projections differ in their depth range: one row of A changed by
0.008 between frames with the view unchanged, 1.6 yards at the tower's distance. When c2..c5 is a pure
projection (`IsProjection`), A is now the inverse of the view plus the camera position. A large model's
stored position then shifted 0.000 yards a frame while the camera moved 0.28 (it was up to 0.33).

**Frames without a replay read the map through the wrong matrix.** With `mapEvery` above 1, those frames
used a matrix centred on the player's position now, while the map held the shade around the position at
the last replay, so the shade shifted by the distance walked and snapped back. They now use the replay's
matrix, kept in absolute coordinates, brought to the current camera. A first try at this flashed; after
the tower fix it neither flashed nor jittered.

On the way, the camera position was measured: `camAddr` moves by one frame's walk (0.27 yards on average)
between the start of the frame and the end of the world. Fixed-function draws are relative to the value at
the end of the world (0.0000 yards of drift with it). Using the start value was tried and was not the
cause. `smooth` went from 0.6 to 0.85, which calms the noise standing still.

## Smoothing and upsampling the light (2026-09-24)

Taken as ideas, not code, from coa-vfog (a volumetric fog DLL for a 3.3.5a client). Not yet checked in game.

**The noise did not average out.** The march starts each pixel's steps at an offset from interleaved-gradient
noise. The offset was the same every frame, so `[volume] smooth` blended the same noise pattern into
itself: it hid the frame-to-frame change and removed none of the noise. The offset now turns by the golden
ratio each frame (`frac(noise + frame * 0.618)`), so each kept frame samples the ray at other places.

**The kept frame smeared on a turn.** It was blended in at the same screen position, so it was faded out
as the camera turned. It is now reprojected. The march writes each pixel's distance to green. The temporal
pass rebuilds the point from its direction and distance, shifts it by how far the camera moved (the client
draws camera-relative, so the last frame's space is this one moved by that vector, read from `camAddr`),
and projects it through the last frame's view-projection. What it finds there is clamped to the range of
this frame's 3x3 neighbourhood, and dropped where the stored distance does not match the point's (that
point was hidden last frame). A history older than one frame is not used.

**The glow bled across edges.** The march runs at half resolution and was stretched over the world, so a
tree trunk in front of bright air took a rim of that air's glow. The blur and the composite now weight each
tap by how near its distance is to the centre's. The composite takes the four low-resolution texels
around each full-resolution pixel, with bilinear weights divided by `1e-3 + |distance difference| /
distance`.

Not taken: extinction in the march (`(1 - e^-t) / t` per step), which would stop a thick `density` from
blowing out toward the sun. It changes how the tuned defaults look, so it waits for a session in game.

## Anti-aliasing (2026-09-25)

**With multisampling on, there was no volumetric light.** `gxMultisample` 4 gives the client a multisampled
depth buffer. A texture cannot be multisampled, so the INTZ swap skipped it and the light had no depth. The
log said so on every BeginScene and every bind: 67,918 lines, 6 MB, in one session.

**RESZ does not work on NVIDIA under DXVK.** RESZ is the driver resolve for this case: an INTZ texture on
sampler 0, then `D3DRS_POINTSIZE` set to `0x7FA05000`. DXVK 2.7.1 implements it, but only when the GPU
reports as AMD (`d3d9_device.cpp`, `D3DRS_POINTSIZE`; `d3d9_adapter.cpp` gives the same answer to
`CheckDeviceFormat`). Measured on the RTX 2080: `hr=0x8876086A`.

**StretchRect does it on any GPU.** The client's multisampled surface is swapped for a multisampled INTZ
SURFACE (`CreateDepthStencilSurface`, same sample count), and when the world ends `StretchRect` resolves it
into a plain INTZ texture. DXVK does a Vulkan depth resolve only when the two D3D formats are the same.
Between D24S8 and INTZ it takes a framebuffer blit, which binds the depth view as a colour attachment. It
refuses a depth StretchRect inside a scene, so the scene is ended for it and begun again. It resolves only
when the light is on.

**With depth fixed, the light still did not show.** A probe found the reason. Full Screen Glow cannot draw
a multisampled world into its texture, so with anti-aliasing on the client draws the world into the back
buffer and copies it out with `StretchRect` (draw 526 of 1202 in the probe). The world end was found later,
at the switch to 2D, so the light went into the back buffer after the copy. The glow composite then drew
over the whole back buffer, unblended, and the light was lost. `debug 1`, which should show only the
glow, looked the same as the normal view. A `StretchRect` from the world's render target now ends the
world, so the light is drawn before the copy. Without anti-aliasing the client makes no such copy.

**Drawn at the copy, the shadow pass made the UI flash.** On the frames that rebuild the shadow map
(`mapEvery` 3), the chat background and the XP bar drew as opaque white: the look of stage 0 left on
`SELECTARG1` from the texture, which is what the replay sets. It was measured by screen captures of the chat
area: 11 of 30 with the light on, 0 of 30 with it off. The replay restored its texture stage states only
through the state block. They are now also re-set by hand, as its render states already were: 0 of 40.
Why the state block alone was not enough at this point in the frame is not known.

## The copy store filled and stayed full (2026-09-25)

**In Stormwind the shadow map held trees and nothing else.** `debug 6` showed it, and the cost line said
`2048 copied chunks (0 failed)`, which is `copyMax`. A copy was freed only on a device reset, so after 2048
distinct terrain and building chunks nothing new was copied, and a chunk with no copy is not cached at all.
Trees are M2s drawn through shaders and need no copy, so they stayed. Moving around one city was enough.

Each copy now records when a draw last used it. When the store is full, it frees the copies unused for
`cacheTime` (at most one sweep a second). Tested with `copyMax` 100 at the harbour: the store fell from 197 to
79, and the new area's buildings came into the map. A cache entry holds its own references, so an entry still
drawing a freed copy keeps its buffers.

Standing still, the harbour needed 125 copies. The cache is a separate limit: a caster out of view for
`cacheTime` (8 s) leaves the map, so a building behind you stops shading you after 8 seconds.

## The cache is kept only on the frames that redraw the map (2026-09-25)

With the city in the map, the benchmark in Stormwind put the light at 2.0 ms of CPU a frame: 0.72 recording
the client's draws, 0.68 keeping the cache, 0.51 replaying it. The first two ran on every frame, but at
`mapEvery` 3 only one frame in three reads the cache.

Now only the frames that redraw the map record the draws in full and merge them. The other frames still
vote on the world camera from the same draws, because the light needs this frame's camera. Which kind a
frame is gets decided at the end of the frame before (`g_fullFrame`). A probe forces a full frame.

Measured in Stormwind, the same way: 1.05 ms of CPU (0.33 recording, 0.19 cache, 0.43 replay), with 1091
casters against 1255. The light's cost in frame time went from 2.48 ms to 1.27 ms.

## The light through leaves re-formed as you walked (2026-09-25)

Alpha-tested leaves leave a sponge of gaps in the shadow map, and the light comes through them. Standing
still the sponge held; walking, it came out as a new pattern at every step, and the light jittered. Two
causes, found with `debug 6` and screenshots a few steps apart.

**Every tree was placed again at each redraw.** A model seen again keeps its stored matrix and constants
when it has not moved, and "not moved" was 0.02 yards. Its position is worked out through the camera,
which moves by one frame's walk during the frame, so walking put every tree past that and each redraw
placed it again, up to a texel off. It is now `[shadow] stillRadius`, 0.3 yards. `snap` did not help on its
own: it holds the grid still, and the tree moved against it. With both fixes in, snap off looked better than
snap on, so it stays off. Turtle's trees do not sway.

**The client's Full Screen Glow bloomed the light.** The light was drawn with the world, before the glow,
and the glow blooms a small copy of the screen. The sponge is too fine for it, so the bloom re-formed at
every step. Turning the glow off stopped it. The light is now drawn where the rays are, before the first UI
draw and after the glow: the depth resolve and the shadow map stay at the end of the world, and the light
waits (`g_volumePending`). It draws before the rays, which streak what is bright on screen.

## Depth stand-ins were kept after the window changed size (2026-09-25)

The client makes new depth surfaces when the window changes size, often with no Reset. The mod holds no
reference to the client's surfaces, so it never saw the old ones freed, and their stand-ins stayed until the
next Reset: at 4x and 2560x1440, some 75 MB of video memory each. After 8 (`kMaxSwaps`), new surfaces got no
stand-in and the light went without depth. A stand-in the client has not bound for 3 seconds is now freed
(`PruneSwaps`, at most once a second). Tested with several resizes: each left `depth: freed the stand-in`
lines for the old size a few seconds later. A surface the client binds rarely (one at the loading screen)
gets its stand-in made again when it is next bound.

## A resolution change makes a new device (2026-09-25)

Unchecking Maximized in the video options stopped WoW with `ERROR #124 (0x8510007c) Memory Invalid Block`,
`SMem3: Pointer does not refer to a valid allocated block of memory`, the stack all in `WoW.exe`. The same
stack came up on 2026-09-24 with an older build. Without comfyfog in `dlls.txt` it did not happen. With it,
switching 1080p to 1440p drew the font texture over the whole screen, and the log had no `device reset`.

The client lets its device go and makes a new one for a resolution change or a Maximized toggle. The hooks
sit in DXVK's shared vtable, so they carried on for the new device, while everything made on the old one
stayed in use: shadow cache buffers, depth stand-ins, the light's targets, the state blocks. `CheckDevice`
now compares the device each hook is given with the last one seen, and on a change releases all of it, as a
Reset does. It is called from BeginScene, SetDepthStencilSurface, SetRenderTarget, StretchRect,
SetTransform, Present and Reset. Tested with six switches: six `device changed` lines, a normal picture, the
light back each time, no crash.

comfygrass keeps its wind vertex shader (`g_windVS`) across the same change.

## Night and the two moons (2026-09-28)

At night the rays and the light were as strong as by day. The sun's height cannot tell night from day,
because at night the client draws Azeroth's two moons with the same sprite as the sun, high in the sky
(logged: 75 to 83 degrees up at 01:00). So `[night]` reads the game clock at `0x00CE9B64` (the day fraction
comfytime found), and **Night Strength** scales both effects by it (`NightScale`, `sun.cpp`).

The moons were not found at first. At night the sky starts with a dozen model draws (stars, sky models),
and the two moon quads come at draws 12 and 13, where the sprite match only looked at the first 8. The light
kept the last day direction all night. The match now covers the whole sky phase, up to the first depth
write, and so does cloud hiding, which had a 16-draw limit with the clouds at draw 15 at night.

The two moons stand at the same height and about 92 degrees apart in azimuth (45.3 degrees each at 00:40).
Following "the highest" swapped between them every few seconds. The light now follows the first quad above
the horizon, which is the larger moon (a quad of 1.8 against 1.0), and the other moon casts rays of its own
(`[rays] secondMoon`). Late in the night the client stops drawing the larger moon, and the light moves to
the other one.

## Rays and light through mountains (2026-09-28)

The rays came over mountains with the sun behind them. `[volume] debug = 5` showed the light shaded right;
it was the rays: the mask keeps only sky, but the blur carries the sky above a ridge toward the sun and over
the ridge. `cover.cpp` now measures how much of the sun's disc is in view on screen, at nine points, and the
rays fade by it. A first version compared brightness with the sky kept before the world, and failed at fog
thickness 100: a distant mountain takes the fog colour, and near the sun that is nearly as bright as the
sky. It now reads depth when there is a readable one: the sky keeps the clear value 1, the world and the far
horizon sit below 0.96. Linear, a sun 80% behind a ridge still showed through it, so the share is squared.

The light had the same fault at sunset for another reason: the shadow map reaches only `[shadow] depth`,
so terrain on the far horizon cannot shade you, and a sun setting behind it kept lighting the fog. The
light takes the same test, with its own eased value (`[volume] occlusion`). Neither test runs with the sun
off screen; a sun just past the edge behind a ridge still lights the fog and casts rays.

## Local lights: the lamp probe (2026-09-28)

The question was whether street lamps and lanterns can emit rays and light. The effect is the smaller
part. The positions of the lamps are the hard part. Three sources were found or proposed:

**The client's model shaders carry two point lights.** From the disassembly of shader `1229F540`: the
positions are `c21` and `c22`, in the same space as the skinned vertex (the space `c2..c5` takes to clip),
the colours are `c17` and `c18`, and the attenuation is `1 / (c25 + c26 d + c27 d^2)`, `.x` for the first
light and `.y` for the second. The client lights a model near a torch through these. A position goes back
to the world through `c2..c5` and the inverse of the world camera. That works for models that fold their
world matrix into `c2..c5` and for those that carry it in the bones.

**Fixed-function lights** (`SetLight`) were never checked. comfygrass reads only the directional one.

**Glow sprites** are additive draws (`DESTBLEND ONE`), which the client fogs to black. Spell effects and
particles are too, so these need a filter.

`lamps.cpp` watches all three for 60 frames after F12 and writes one report to `comfyfog.log`, lines
starting `lamps:`. It gives each source's places in world coordinates, nearest first, with the screen
position of each, and the number of frames it was seen in. A lamp is seen in all of them. Stand still
during the window. The first 40 new places of each source are also logged raw, and the `SetLight` calls
of the first frame. A shader light's raw line gives the distance from the light to the model it lit, as a
check on the transform. The probe costs nothing outside its window.

**The road into Duskwood, near the Elwynn border (probe 1, 2026-09-28).** Camera at (-10906, -418), not
in Darkshire town, although it was taken to be. 750 world draws a frame.
- Shader lights: none. No world draw used a shader that reads `c21`. The light-capable shaders were bound
  only before the device reset, on the character select screen (fog `0x808080`). In the world the models
  are lit by `c10..c16` (the sun and the ambient, as spherical harmonics) and nothing else, so the
  lamps do not light the models near them.
- Fixed lights: 540 `SetLight` calls, all the directional light (type 3). No point or spot light was
  enabled at any draw.
- Glows: about 12 additive draws a frame, at 10 places. The lamppost beside the player drew a
  four-vertex M2 sprite, 1.6 yards from centre to corner, 64x64 DXT5 with a grey texture (0.48),
  source blend `SRCALPHA`, fogged black, in 60 of 60 frames. A screenshot put the lantern within about a
  yard of where the probe placed it. The same texture 102 yards away is the next lamppost. The others
  were a glow on the character, small particles and sky sprites 250 yards up.


**Darkshire town, outdoors (2026-09-28).** Camera at (-10553, -1204), 1290 world draws a frame.
- Client point lights: 5 in view, in 60 of 60 frames. All have the colour (1.40 0.87 0.40) and the
  attenuation (0, 0.7, 0.03), so they reach 16.7 yards at 5%. The shader route (`c21` through `c2..c5`)
  and the fixed-function route (`SetLight`, camera-relative, plus the camera) gave the same absolute
  positions to 0.1 yard, which proves both transforms. The client sets them with `SetLight` on indices
  1 to 3 (about 70 calls a frame), so that hook alone finds every light. Two more fixed lights sat 40
  and 75 yards above the street, with flame particles at the same places.
- Each of the 5 lights sits on a 64-vertex additive M2 with a flame texture (64x64 DXT1, nearly black
  on average): torches or braziers.
- The lampposts have no point light. Each is found only by its glow sprite: the four-vertex M2 with the
  grey 64x64 texture. `c28 + c29` at that draw is (0.95 0.60 0.22 0.91), the lamp's orange. Three were
  in view.

**The Darkshire inn (2026-09-28).** Camera at (-10525, -1162). No point lights at all: interiors are lit
by the client's baked vertex colours. The candles are fixed-function particles, each a group of three
textures in one place (orange 64x128, yellow 32x32, blue 128x128).

So there are two sources to build on. The client's point lights give torches and braziers with their
exact position, colour and reach. The glow sprites, filtered to four-vertex additive M2 draws with a
warm `c28 + c29`, give the lampposts.

## Lamps: the glow and the light (2026-09-28)

`lamps.cpp` keeps a list of lights every frame; `lampglow.cpp` draws two full-screen passes from it,
just after the volumetric light. It reads the light's depth, world camera and depth range, so it draws
only while `[volume]` draws, but it does not follow the sun and draws at night too. By day it is turned
down to `[lamps] day` by the game clock.

**The lights.** Client point lights from `SetLight` (torches and braziers, and those NPCs carry), and
lamppost sprites (four-vertex additive M2 draws with a warm `c28 + c29`, which stay where they were first
seen). They are held in world coordinates. A light is dropped only after `[lamps] keep` seconds **on
screen** without being seen. The client draws a lamp's sprite only while the lamp is on screen, so a
lamppost beside the camera faded out while the fog around it was still in view. At some places in
Darkshire the client sends no point light in most frames, and the lampposts are all there is.

A row of client point lights sits about 120 yards above Darkshire, at the same world positions frame
after frame. What they belong to is not known. Nearest-first keeps them behind the street lamps.

**The glow in the air.** The integral of 1 / d^2 along the line of sight has a closed form, so there is no
march and no noise (the formula is in `lampglow.cpp`). The scene's depth ends the line, and it runs on
`[lamps] through` yards past it, since the client's light point sits inside the torch head or the bowl.

**The light on surfaces.** Lampposts only: the client already lights with its own lights. The facing
of a surface comes from `ddx`/`ddy` of the rebuilt position. The falloff is the torch's, 1 / (0.7 d +
0.03 d^2), and the light scales the colour already there, out = scene x (1 + light). The client adds a
torch's light to the dim light a model already has, so `[lamps] surface` is about 1 / the night's light:
7 at 100, 2.8 at the default strength of 40. `spriteGain` 1.5 puts a lamp's colour near a torch's.

**The trap: no `break` in an unrolled light loop.** The first version stopped the loop after the last
light (`if (i >= count) break;` in an `[unroll]` loop). d3dcompiler_47 compiled it so that light 0,
the nearest, never added anything. In game that looked like "the lamp goes dim at some angles": it went
dim whenever it was the nearest light. The loop now always runs over all 16 slots, and an empty slot has
a reach too short to pass the test. A plain `[loop]` indexes the arrays by comparing against every
element (32 compares a light), which is why it is unrolled at all.

The debug views found it: `[lamps] debug` 1 is the glow alone, 2 the distance read (white = 50 yards),
3 the light on surfaces alone. F12 also draws the pass into a target of its own and logs, for each
light, the glow and the distance at its pixel and beside it (lines `lampglow:   read back`).

`maxIntensity` 4 could not be seen; the default is 10 at strength 40. The cost has not been measured:
the benchmark has a line for it (`bench: the fog around lamps`).

## Sun shadows on the world (2026-09-29)

`sunshadows.cpp` lays the volumetric light's shadow map on the world, just before the light: the point
each pixel shows is rebuilt from the depth, looked up in the map with nine taps, and the scene is scaled
by 1 - strength x shaded. It draws only while `[volume]` draws, since the map is built for the light, and
follows the sun's height and `[night] strength` as the light does.

**The near map.** The far map is 2048 texels over 500 yards, a quarter of a yard a texel, and too coarse
for a trunk, a post or a character. `shadow.cpp` now replays the same cache a second time into a map of
the same size over `[shadow] nearRange` yards either side of the player (32: 0.03 yards a texel), with the
full depth toward the sun. Models whose reference point is well outside the small box are not drawn into
it. The shadows blend from the near map into the far one over 80% to 90% of the near map's half-width.
The volumetric light still reads the far map only. `bias` and `normalBias` are in texels of each map.

**Surfaces facing away from the sun.** At first they got no shade, on the idea that the client's lighting
had darkened them already. It does not for models: it lights trees and characters almost evenly all
round, and backlit trunks stayed bright in the canopy's shade. Then they were tested against the map, and
the back of a walking character flickered: the test there grazes the model's own body. Now a surface
facing away gets `[sunshadows] backShade` (0.8) with no test, one facing the sun gets the map's answer,
and the facing blends the two. backShade is under 1 so that terrain facing away, which the client does
darken, is not darkened twice.

**The player's shadow.** The cache keeps a model's stored place and pose while it stays within
`[shadow] stillRadius` (0.3 yards), so that trees do not shimmer as the camera drifts. The player runs
about 0.12 yards a frame, so the character's own shadow held for two frames and then jumped 0.3 yards,
ten texels of the near map, and flickered over the character. Models within 3 yards of the player are
now always placed again. With that, `[shadow] mapEvery` 3 looks the same as 1 in game.

**Stepped edges, and the resolution control.** Each of the nine taps tested one texel, so the shade had
ten levels and its edge moved in whole texels: beyond the near map, shadows had the outline of the far
map's quarter-yard texels. Each tap now tests the four texels around its point and blends the answers by
where the point falls between them, so the edge moves smoothly inside a texel. The shader reads the near
map first and the far map only where the near map does not cover all of the shade. `[shadow] size` got
its own control (Shadow Resolution: 1024, 2048, 4096), and Volumetric Light Quality stopped setting it;
`[sunshadows] softness` got one too (Shadow Softness), and so did `[shadow] mapEvery` (Shadow Redraw, 1..8).

**A page of its own.** With fifteen controls, the addon moved them from Video > Shaders to a page of its
own, Video > Atmosphere (2026-09-29). In `GameOptions` an entry with `options` is a page and one without
is a heading; the list shows 18 rows and the client uses 15. The panel draws the list once as it loads,
before `VARIABLES_LOADED`, so the addon calls `OptionsFrame_UpdateCategories` after it inserts the page.

The cost of the near map and of the pass has not been measured: the benchmark has a line for the pass
(`bench: the sun shadows`), and the replay's share is in the shadow map's line.

## Shadows on models, and /atmos (2026-09-29)

The sun shadows are drawn from the depth buffer only, so a surface's facing is rebuilt from the depth.
Four faults followed one another on a character's face and shoulder, each from the fix before it:

- **2 x 2 blocks.** The facing came from ddx and ddy of the rebuilt point, which the GPU works out once
  for each 2 x 2 block of pixels. That was harmless while the facing only chose the back shade; once it
  set each shadow tap's depth (the slope that stopped stripes on sloped ground), models were shaded in
  2 x 2 blocks. The facing now comes from the four neighbouring pixels, nearer in depth on each axis.
- **Triangles.** The depth gives each triangle's flat facing, and the client lights a model with smooth
  normals. The back shade switched on sharply (facing x 4), so a face was shaded triangle by triangle.
  The facing is now taken from points `[sunshadows] normalSmooth` yards apart (0.1), which spans several
  triangles, and the back shade fades in over a wider range (smoothstep from -0.1 to 0.4).
- **A copy of the nose.** A pixel near the edge of the nose took a wide point from the cheek or the
  background behind it, and a copy of the nose's outline showed 0.1 yards away. A wide point now counts
  only if it lies near the plane the one-pixel neighbours give.
- **Speckle.** A hard cut-off for that test flipped from pixel to pixel on a face. It is now a blend:
  all of the wide point within about 12 degrees of the plane, none past 30.

Also the normal offset grows up to 4 times where the sun grazes the surface, against self-shading on
the far side of a shoulder, where one texel of the map covers most surface.

**/atmos.** Each of these took a build and a client restart to try one value. `/atmos` (tune.cpp) sets
any ini value from the chat, at once. The DLL cannot write chat, so it answers by registering new CVars
(`comfyTuneReply<n>`), which the addon prints; a CVar at its default is not saved to Config.wtf. Every
key LoadSettings reads is noted with the value it used (`ConfigKeys`), so every setting is reachable and
a misspelt key is refused. Values the Atmosphere page's controls set are refused too, since the control
is laid over the ini and the value would not show.

## The map alone, and the round shadow (2026-09-29)

With `/atmos` the owner tried the lookup settings one at a time against the debug view of the map's
answer alone (`[sunshadows] debug` 3, since removed). The map alone was clean; every patch on the
character came from the facing rebuilt from the depth. The facing term, `backShade`, `normalSmooth`
and the face blend were removed. The values kept: `bias` 3 texels and `sunOffset` 0.06 yards toward the
sun, against the body shading itself in bands; `normalBias` and `slope` 0 (the facing is rebuilt for
them only when either is above 0). The back of a character is shaded by its front, as the sun sees it.

`[sunshadows] minGap` (yards, on both maps alike) ignores a blocker nearer than that along the sun: an
approximation of "no shadow on itself", which would need an object id for each pixel. Tried at 0.25 to
1 against trees at odd sun angles; the owner preferred 0, and it stays 0 until that is looked at again.

The game's round shadow under each unit is the CVar `shadowLOD` ("Unit shadow LOD", default 1): 0 turns
it off. `showShadow`, next to it in the strings, is a console command registered with `waterRipples`
and `showLowDetail`, and does not. The addon sets `shadowLOD` to 0 while Volumetric Light and the new
Sun Shadows box are on, and back to the default at logout, so Config.wtf never keeps the 0.

## Our own fog (2026-09-29)

Far away looked lit up, and it took several steps to see why.

- **The gaps were sky.** Past the view distance (`farclip`) the client draws nothing, and between the
  trees the sky showed, bright. Darkening the fog colour made far trees darker and left the sky as it
  was. The sky match (`[fog] skyMatch`, `skyBand`) faded the sky near the horizon into the fog colour.
  Scaling the sky by the fog colour's change first gave two tones, since the game's sky at the horizon
  is not quite its fog colour.
- **The fog colour was part of the dial.** `darken`, `desaturate` and `tint` were scaled by the
  thickness and did nothing with Atmospheric Fog off. They now apply as set, fog on or off.
- **Shadows were drawn over the fog.** A shaded tree at the fog wall came out darker than the fog and
  the sky it should fade into. The shade now weakens with the fog at each pixel's depth.
- **The game's fog is a straight ramp**, the same in the terrain and in the tree shaders, so a stronger
  fog still ended in a wall and fogged models stood out as flat white shapes. `[fog] mode 1` draws our
  own fog over the picture from the depth, with the game's fog moved past anything drawn (`Remap`, which
  also rewrites the tree shaders' c30). It runs only while the volumetric light does, since it needs the
  depth; otherwise the game's fog is used as before.
- **Height fog alone thinned out on mountains**, which showed every flat face, where the game's fog makes
  far mountains one flat layer. Our fog is now the game's distance fog (its start and end as the dial
  moves them) on an S curve, plus height fog (`density`, `height`) on top. The sky gets no distance fog,
  so a ridge keeps its line against the sky.
- **A second ridge.** Far mountains are drawn with the sky, in the sky's depth slice (past the world's
  0..0.94, in front of the sky itself). Every pass took them for sky. Pixels in that slice but in front
  of `[fog] skyDepth` now get full fog, as the game gives them. `[fog] debug 2` shows them in red.
- **The view distance shrank** once both parts were on: the height fog had been tuned alone, scaled by
  the dial, at 0.012 a yard. It is now its own control, Ground Haze (`density`, 100 = 0.02 a yard,
  default 0.005), not scaled by the dial; Fog Thickness moves only the distance fog.

The Atmosphere page gained Ground Haze, Fog Height, Fog Edge Fade (`cover`), Fog Darkness and Fog
Greyness, and a master box at the top, Atmosphere Effects (`[general] enabled`), that turns every effect
off at once (and with it the depth buffer and the shadow map).

## The terrain's baked shadow, and Sunlight (2026-09-29)

**Sunlight.** Northshire, a forest, was dark all over under the sun shadows. `[sunshadows] sunlight`
(the Sunlight control, 0..50%) brightens what the sun reaches. A shader's output stops at 1, so the pass
is drawn at half and blended as 2 x modulate (`DESTCOLOR`, `SRCCOLOR`): an output of 0.5 changes nothing.

**The baked shadow.** Each terrain chunk carries a 64 x 64 shadow baked offline for one sun direction,
with the shade of the trees and buildings around it, so it disagreed with the sun shadows. The CVar
`mapShadows` and the console command `setShadow` flip bit 0x40 of the render flags at `0x00C7B2A4`
("Terrain shadows enabled/disabled"), and nothing in this client reads that bit (every read of the word
was checked). F12 now logs each texture stage's size, format and combine, and writes each pixel shader's
bytecode to `comfyfog_ps_<address>.bin`. The terrain draws: ps_2_0, one to four DXT5 ground layers and a
64 x 64 A4R4G4B4 blend map on the last stage, whose x, y, z mix the layers and whose w is the shade:

    colour = lerp(layer0, layer1, map.x) ... ; out = colour x (map.w x 0.3 + 0.7) x diffuse + colour.a x map.w x specular

`terrainshade.cpp` hooks `SetPixelShader`, disassembles each new shader once, and swaps one that matches
that pattern (all four layer counts matched) for a copy with `map.w` replaced by `lerp(1, map.w, keep)`,
`keep` in c31 (no client pixel shader uses past c9). `keep` is `[sunshadows] baked` (0 by default) while
the sun shadows draw, back to 1 as they fade at dusk and whenever they are off.

## Shadows jumping in and out of the map (2026-09-29)

With the sun shadows on the ground, casters leaving and rejoining the map showed as shadows jumping.
`/atmos probe` (the same as F12, from chat) now also logs what the cache holds: by kind, alpha tested,
moving, triangles, and by distance from the player. Four causes, in the order found:

- **Kept by time.** A caster out of view stayed `cacheTime` (8) seconds, so a tree beside or behind you
  left the map 8 seconds after you looked away, with its shadow in front of you. Now a caster that stays
  put is kept while its reference point is within `[shadow] range + keepMargin` (100) yards of the player
  across the ground; `cacheTime` is an extra limit, 0 (off) by default.
- **Identical trees taken for one that moved.** With no instance of the same model within 3 yards, the
  cache took one up to 60 yards off that was not drawn this frame to be it, moved (a rule for birds).
  In a forest of one tree model, a tree leaving the screen was "moved" onto the one coming in: its shadow
  left where it stood, and the entry, marked moving, left the map once not drawn (525 of 3,833 entries
  marked moving). Now an instance counts as moved only if it was drawn the frame before and its stored
  place is still in view, and matching runs in three passes (place every draw, let each claim its own
  instance within 3 yards, then match), so a draw cannot take an instance another draw stands on.
  Moving entries fell to 7 to 16, the characters and creatures about.
- **The cap dropped the longest unseen**, which could be a tree right beside you. It drops the farthest
  first now, and nothing drawn this frame.
- **The sun stepped.** The map took the sky's sun direction only once it had moved 0.05 degrees, so
  about every 30 seconds a low sun's long shadows moved some ten pixels at once. It now glides toward the
  measurement with a 10 second time constant, once a frame for every pass; a change past 5 degrees lands
  at once. The sky's own sun does not step: in a session it moved more than 0.2 degrees in a frame once,
  24.5 degrees as the world loaded.

The cost stood at 5,000 entries (the cap), 2.6 million triangles and about 9.5 ms of CPU a replay.

**Making the replay cheaper (2026-09-29).** `/atmos probe` now times each part. One replay was 10.8 ms:
matching 1.2, eviction 0.4, far map 5.1 (4,484 draws), near map 2.4 (2,003 draws), and 12.4 MB of model
constants uploaded. Each draw costs about a microsecond whatever its size, so the count matters most.

- Each model uploads only as far as its own uploads reached before it was drawn (`nregsOwn`), not to the
  highest register ever set (157 to 220): 12.4 MB fell to 4.0 MB, but the far map only to 4.0 ms.
- The far map is redrawn on every second replay only (`[shadow] farEvery` 2), read in between through
  the matrix it was drawn with. The near map, where the player and everything close stand, each time.
- The near map's cull margin is 16 yards, not 40: 2,022 draws fell to 1,246, 2.4 ms to about 1.7.
- Models under `[shadow] minTriangles` (200) stay out of the far map past 60 yards: at 100 about 1,400
  of 4,457 draws went and the far map took 3.0 to 3.7 ms; neither 100 nor 200 could be told apart.

A replay now averages about 4.8 ms (matching and eviction 1.5, near 1.7, far 3.3 every other time), from
10.8. Terrain and buildings are never culled: a chunk's reference point says too little about where
its ground lies. The probe's own total reads high, since the probe frame also writes the log.

## To do: character shadows under the canopy (noted 2026-09-29)

In Elwynn Forest the tree cover casts shade over nearly all the ground, so a player or an NPC standing
in it casts no shadow of its own: the ground is already in shade, and shade on shade changes nothing.
Characters then look like they float. The owner's idea: let character shadows add a little on top, so
they show even inside the canopy's shade.

A way to do it: characters are the cache's moving entries and the models near the player (`mobile`,
`kPlayerModels`). Draw those alone into a small map of their own (or a second channel of the near map),
and in the sun shadows darken by that map in addition to the main one, at its own strength, so a
character's shadow deepens the canopy's shade instead of disappearing into it. The game's round shadow
(`shadowLOD`) is the fallback that shows today how much is missing.

## Sun rays jumping behind leaves (2026-09-29)

Tilting the camera slowly down from the sun past trees 100 yards away, the sun rays jumped by 50 pixels
or more. It took a while to find because two effects are called "rays": the volumetric light was
suspected first, and a light grid and a four-texel shadow read were built for it and then removed, since
the volumetric light was not the cause. **Ctrl+F11 (rays off) and Alt+F11 (light off) tell the two apart
in one test; do that first next time.** Turning the sun-cover test off did not change it either.

The mask view (`[rays] debugView = 1`) showed the cause: the only bright pixels were small gaps of sky
between the leaves at the top of the screen, a pixel or two wide at that distance. Any camera move
opened or closed one (the trees do not sway), and the radial blur turned each into a whole streak.

- `[rays] soften` (8): the mask is blurred, 9-tap Gaussian across and down, before the rays are drawn,
  so a one-pixel gap adds a faint streak and a real opening still gives a strong beam. This is the fix.
- `[rays] smooth` (0.7): part of the last frame's mask is kept, moved by the sun's shift on screen
  (the mask holds only far things, which move with the sun as the camera turns). Not used after a fast
  turn, a pause, or with the sun behind the camera.

Both are sliders in Video > Atmosphere (Sun Rays Softness, Sun Rays Smoothing; smoothing in percent). The
panel also has a **Debug View** slider: one number per debug view of every effect, listed in its tooltip
and named in the log as `--- debug view N ---`; 0 leaves the ini's own debug values in charge. The list is
`kDebugViews` in `cvars.cpp`, and the addon's tooltip must list it in the same order. The panel builds a
page from a table of tick boxes and sliders, so a dropdown is not possible there. The addon now adds a
control only if its CVar exists, so an older DLL cannot break the page.

## The framing that matters

**comfygrass is a vertex-shader substitution mod. This is a post-process mod.** comfygrass never allocates
a surface, never binds a pixel shader, and hooks `Reset` only to drop a shader. All of that is new here, and
it is where the crashes will be.

About 60% of the scaffolding transfers. The rest is render-target work comfygrass never had to do.

## Two mods, not one

They are bundled in the ask, but they are far apart in difficulty. Ship them separately.

| | Effort | Risk |
| --- | --- | --- |
| Fog control | an evening | ~none |
| Sun shafts | a weekend or two | moderate |
| Height/volumetric fog | +1 week | high: needs depth, see *Deferred* |

## What transfers from comfygrass

Exact anchors, because these took time to get right:

| Piece | Where in `comfygrass.cpp` | Note |
| --- | --- | --- |
| `HookSlot`: vtable patch | `HookSlot` | Patch **in place**, never a copy. A copied vtable drops the RTTI word DXVK keeps behind `vtable[0]`, and the client silently gives up before `CreateDevice`: black window, no error. |
| Probe-device attach | `AttachToDxvk` | Every DXVK `IDirect3DDevice9` shares one class vtable, so a throwaway device names it and the client's real device is caught whatever the order. Also pins `d3d9.dll`. ~250–430 ms, once, background thread. |
| Runtime HLSL compile | `EnsureWindShader` | `d3dcompiler_47` via `GetProcAddress`. Add `ps_2_0` next to the existing `vs_2_0`. |
| Mirrored `SetTransform` | `hkSetTransform` | world / view / proj. Needed here to project the sun to screen space. |
| Mirrored `SetRenderState` (`g_rs`) | `hkSetRenderState` | Already carries every fog state. This *is* the fog mod. |
| **Directional light read** | the light loop in `DrawWithWindShader` | Walks `GetLightEnable`/`GetLight` for the first enabled directional light. **This is the sun.** The part expected to be hard is already done. |
| F9 frame capture | `ProbeDraw` | Per-draw prim type, stride, world/view/proj and fog states. This is the tool for finding the world→UI boundary (below). |
| F10 ini reload + toggle | `PollKeys` | Matters more here than for grass: a rays effect is all tunables (decay, density, weight, exposure, threshold, sample count). |

Also inherited, as a rule: **comfygrass only acted on draws it could positively identify, and did nothing
when unsure.** A post-process touches every frame unconditionally, so it needs a deliberate equivalent:
bail out and pass through cleanly on any failed surface creation or lost device. The failure to avoid is a
black screen where a missing effect belonged.

## Part 1: fog

Intercept in the existing `hkSetRenderState` and substitute: `D3DRS_FOGSTART`, `D3DRS_FOGEND`,
`D3DRS_FOGCOLOR`, `D3DRS_FOGDENSITY`, `D3DRS_FOGTABLEMODE`. Drive from an ini; optionally per zone or
per time of day.

Side effect: the grass shader reads fog out of that same mirrored state map (in `DrawWithWindShader`), so
if both DLLs are loaded, the grass follows the new fog.

comfygrass has a known gap: its shader fogs on `pos.w`, so **range fog would differ**. If this mod ever
enables `D3DRS_RANGEFOGENABLE`, that gap becomes visible on grass.

## Part 2: sun shafts

Classic post-process radial blur toward the sun (GPU Gems 3, "Volumetric Light Scattering as a
Post-Process"). Four steps:

1. Capture scene colour to a quarter-res render target.
2. Mask it: bright pixels near the sun survive, everything else goes black.
3. 2–3 radial blur passes accumulating toward the sun's screen position.
4. Additive blend back over the scene.

Cost at quarter res is well under a millisecond. Performance is not the problem here; correctness is.

**Sun screen position.** Take the directional light from comfygrass's light loop, negate it, and project
it through the mirrored view-proj. Fade the effect out as the sun leaves the frustum or goes behind the
camera. Not fading is the classic artifact.

The client renders **camera-relative** (`view[3] = 0,0,0,1`; the camera position is folded into each world
matrix). For a *direction* that does not matter, which is why the sun is easy and the grass wind phase was
not.

### The three things that will take the time

**1. The occlusion mask: use luminance, not depth.**
Proper shafts want depth to know what blocks the sun. In D3D9 that means forcing the depth-stencil to
`INTZ`: hooking `CreateDevice`'s `AutoDepthStencilFormat` and substituting a texture-backed surface.
DXVK supports the INTZ hack. **Do not start here.** Threshold the colour buffer instead: sky near the sun
is bright, trees are dark, and the silhouette falls out for free. It is a period-correct technique. It
fails only on snow and water glare, and it looks right in the forest case that motivates the mod.

**2. The world→UI boundary: the hard part.**
Composite at `Present` and the UI is already drawn, so the rays smear the action bars. The pass has to go
in after world geometry and before UI. The marker is the switch to an orthographic projection, or
`ZENABLE` going off, on the first 2D draw. `SetTransform` is already mirrored, and F9 already dumps
per-draw matrices, so the instrument for finding that boundary *in this binary* exists. Expect most of the
debugging to land here.

**3. `Reset` and state leakage.**
Every `D3DPOOL_DEFAULT` surface must be released and recreated around alt-tab and resolution change.
`hkReset` already exists in comfygrass but only drops a shader. Here it carries load, and it is the
classic device-lost crash.

A post-process pass changes a dozen render states, textures, sampler states, stream sources and shaders
that the client's fixed-function pipeline needs back exactly. **Do not restore them by hand.** Wrap the
pass in an `IDirect3DStateBlock9` capture/apply: one call each way, built into D3D9.

## Deferred / rejected

- **INTZ depth readback.** Adds roughly a week and a risk of breakage for a better occlusion mask.
  Revisit only once the luminance version works and its limitation matters.
- **Height fog via depth.** Needs depth *and* the inverse view-projection to reconstruct world position.
  Same gate as above.
- **Per-vertex height fog in the terrain draws** (the comfygrass technique applied to terrain). Terrain
  uses many vertex formats, and each would need its fixed-function path reimplemented. comfygrass's
  approximate lighting was the cost of doing this for *one* format. Not worth it.

## Build order

1. **Fog state override.** Ships alone, proves the ini/reload loop, near-zero risk.
2. **RT scaffolding + state block + `Reset` handling**, validated with a plain full-screen tint.
   This is where the crashes live, so isolate it before any effect work.
3. **Luminance mask + radial blur, composited at `Present`.** Accept the UI smear so the effect is
   visible and tunable.
4. **Move the composite before the UI.** F9 captures find the ortho transition.

## Open questions for the keyboard

- Where is the world→UI transition in this `WoW.exe`? (F9 capture: look for the proj matrix going ortho.)
  Is it stable across zones, and with the map or character sheet open?
- Does DXVK's `StretchRect` from the backbuffer behave at quarter res here, or is a full-res intermediate
  needed?
- Does the client's directional light stay sane indoors and at night, or does it need gating? (The sun is
  not the directional light, see above. At night the rays and the light follow the moons, and `[night]`
  gates them by the game clock.)
- Do fog state overrides fight anything? Does the client set fog per draw, or per zone?

## Build

Same as comfygrass. **32-bit only**: the 1.12 client is x86.

```
cmake -B build -A Win32
cmake --build build --config Release
```
