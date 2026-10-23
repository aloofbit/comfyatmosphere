# Performance: where it stands

The current state of comfyatmos's frame rate, and the plan. The measurements and the reasoning behind each line are
in `NOTES.md`, in the sections named at the end. Update this file when a finding changes; put the long form in
`NOTES.md`.

Last updated 2026-10-23: the plan for baked static shadows, step 1 and part of step 5 built, then stopped.

## Where it stopped (2026-10-23, afternoon)

Built, not measured:

- **Step 1.** The far map, its leaves, the hills, the near map and its leaves are baked (shadow.cpp, "the baked maps").
  Each map's grid is fixed in the world and its texture wraps. Only scrolled strips, changed rectangles (the files'
  changes come from `MapTakeChanges`, the cache's from a diff of the entries by content) and, after a sun step, a band
  a frame into a spare map are drawn. The middle map and the units' map are still drawn each time. The shaders read
  each baked map through its own matrix with `D3DTADDRESS_WRAP`.
- **Part of step 5.** The cache takes only units' and ships' draws (`CacheTakes`). A unit's entry stays out of view
  while its unit stands there, and goes when it is in view and not drawn.
- The settings this made meaningless are gone: keepStill, keepPoseEvery, farUnitReach, leafHold, leafHoldFar,
  farLeafSize, farEvery, minTriangles, snap, nearSnap.

Found:

- Five shadow tests (Ashenvale edge strips, Elwynn trough and stump, far terrain cache, Stormwind wall foot) gave the
  same shade as the morning's build: the shots differ only in clouds, grass, water and one camera place.
- While turning, the old cache added and dropped hundreds of entries a second (`tests/perf-spin.json`, the
  "shadow: churn" log lines): at Goldshire about 320 units' parts a second, dropped as "brief" each time they left the
  view; at Stormwind's harbour and wall 50 to 100 a second of the client's other draws.

**The owner, in play: overall worse.** No measurement was taken of this build. The test client went back to 0.17.0.
Not filled (step 4): the shade of what the files do not draw, now that the cache does not take it (buildings' cut-out
parts, furniture inside buildings, models the files do not place).

## The owner's findings (2026-10-21, in play)

- Standing still, the frame rate is fair.
- **Moving and turning the camera is the problem.** The perf build was faster standing still and slower moving than
  0.17.0.
- Out in the world (Westfall) the frame rate is good.
- **In towns the frame rate drops.** The volumetric light is the largest cost there.
- **Out of towns, near water, the water is the largest cost.**
- Multithreading the D3D9 calls is not the answer for now.

## The decision

The code has reached its limit for tuning. The effects look right, but they are built in layers that no one can
trace back to a cause from the code alone. The next step is to rewrite the costly effects from the ground up, with
the current look as the target.

## Rules for the next round

- **Measure in motion.** A steady circle: autorun (Num Lock) and one turn key held. Report the moving figure first,
  then the standing one. A gain measured standing still is not a result.
- A cache that keeps last frame's work pays only while nothing moves. Do not count on one for the frame rate in
  motion.
- Compare builds in alternating rounds (A, B, A, B). The laptop's GPU is held by a power limit and drifts by more
  than most changes.
- Measure at 1920 x 1080 with MSAA 2, the owner's size, with `gxVSync 0`. At 1152 x 864 the CPU sets the frame
  time; at 1920 x 1080 the GPU does.
- Every figure names its conditions: place, still or moving, resolution, settings, sun.

## What the measurements show

**At 1920 x 1080 with the effects on, every test place is bound by the GPU.** With the effects off, it is bound by
the CPU. Work moved off the game's thread cannot raise the frame rate while the GPU is the limit.

GPU time a frame at the harbour, standing (2026-10-21): light and fog 3.1 ms (2.1 at 32 steps), sun shadows 1.8,
shadow maps 1.1, sun rays 1.0, water 3.8 to 4.6. At Auberdine the water is 5.9 ms.

**The Volumetric Light box turns on more than the light.** The seven shadow maps, the sun shadows, the bodies' mask
and the lamps' glow all follow it (`VolumeLightActive`). In a town, "the volumetric light" is mostly the shadow maps
of many buildings and models, and the march through them. At Goldshire the shadow maps were 7.5 ms of a 15.3 ms GPU
frame, 6 ms of it the leaves maps.

**Darkshire, turning (2026-10-21, 1920 x 1080, the owner's game, 10 s frame log).** 130 fps with the effects
off, 40 to 60 with them on. Frame 16.6 ms, slowest 1% 39 ms. Our CPU 7.1 ms a frame, of which the shadow cache
4.5 (`animate objects` 2.7, placing entries 0.9, evict 0.3); in the ten slowest frames the cache alone took 20 to
31 ms. 2,581 new cache entries in 10 s. Our GPU 11.0 ms: shadow maps 2.9, lamp fog 2.2, sun shadows 2.1, body mask
2.0, light and fog 1.8. **The cost of the volumetric light in a town is keeping the shadow maps' casters current
while the camera turns**, not drawing the maps and not the light's march through them. The casters come from what
the client draws on screen, so a turn changes them.

**The water:** the water part is 3 to 4 ms at 1920 x 1080. Through a one-colour shader the draws alone are still 1.1
to 1.4 ms: the game's water chunks are drawn three times each. The shader is 3,940 instruction slots and 178
texture reads. The drawn foam is 1.8 ms at Auberdine (the foam round objects 0.85 of it). The glint, reflection,
ripples, wake, swash and edge line cost close to nothing each.

**Turning costs frames without the mod too** (2026-10-02, Wetlands: 120 to 107 fps without the mod, 110 to 95 with
it). In Stormwind every turning run read about 60 fps with the effects on and off. This can be a limit of the client
while the mouse turns the camera. Measure it again at 1920 x 1080 before a rewrite counts on it.

**The game's thread, 1152 x 864** (comfyprof): about 96% busy everywhere. Effects on: WoW 41%, comfyatmos 24 to 31%,
DXVK's D3D9 front end 15 to 25%, UnitXP_SP3 3.5%, nampower 2 to 3.5%.

## What was tried on 2026-10-21

| Change | Result | State |
| --- | --- | --- |
| Command stream (D3D9 calls recorded and replayed on a second thread, as Northlight does) | +8% harbour, 0 Elwynn, -3.5% Goldshire at 1152 x 864 standing; no help at 1920 (GPU bound). Not measured in motion | In the perf build, `[stream] enabled = 0` |
| `[shadow] keepPoseEvery 3` (animated models' poses redraw a leaves map on every third chance) | Goldshire 68 to 97 fps standing. Moving: lower than 0.17.0 (owner) | Default 3 in the perf build |
| Light steps 64 to 32 | Light and fog 3.1 to 2.1 ms standing. Not checked in motion | Default 32 in the perf build; the test client's ini still says 64 |
| Wet sand in one draw, early leave far from the water | Small | In the perf build |
| DXVK 3.1.1 | 4 to 6% faster CPU bound. With MSAA, our depth stand-in is refused: the depth effects stop | Not used |
| DXVK 2.7.1 gplasync, `enableAsync` | Not measured | In the test client |
| `DrawFvf` (DXVK 3 gives FVF 0 for a declaration) | Fixes the missing water on DXVK 3 | In the perf build; keep |

## What was tried on 2026-10-23 (in motion)

Measured with `tests/perf-move-only.json` (running in a circle) and `tools/perf/AB-Move.ps1`, 1920 x 1080, MSAA 2,
alternating rounds. NOTES.md, "Performance in motion: units, the pose search and the leaves' maps", has the detail.

| Change | Result, moving | State |
| --- | --- | --- |
| Units past 30 yards out of the far maps (`farUnitReach`), unit parts under 50 triangles out of the far and middle maps (`unitMinTriangles`) | Goldshire +11%, Elwynn +9% | Default |
| Units past 60 yards out of the middle map (`midUnitReach`) | +1 to 2%; units 60 to 100 yards off lose their shade | Off |
| One full pose search a frame (AnimateObjects) | Darkshire +9%; its 20 to 31 ms frames gone | Always |
| Replay sorted, constants not uploaded twice, unit questions every fourth frame | Eviction 1.06 to 0.57 ms, preparation 0.84 to 0.65 ms of CPU | Always |
| Leaves' maps held in place (`leafHold` 6, `leafHoldFar` 40), with the keep test by content | +2 to 3% on Elwynn's ridge and at Auberdine, 0 in towns; leaf shade a little softer | Off |
| Far leaves' map at 2048 (`farLeafSize`) | Its GPU time halves; no frame-rate change while the CPU sets it | Off |
| Near leaves' map at 2048 | Leaf shade on the ground goes soft | Not kept |

**Where it stands, moving** (each place 30 s after the arrival: earlier runs measured while the files' doodads were
still loading). Effects off: Goldshire 88 fps, Darkshire 120, Elwynn 145, Auberdine 177. Effects on: 0.17.0 40, 55, 76
and 58; perf-2 now 47.5, 62, 88 and 62 (+18%, +14%, +17%, +6%). At Goldshire our CPU is about 10 ms a frame and the GPU's frame 19 ms (4 ms with the effects
off): both sides must come down for a large gain.

## The plan: static shadows baked, dynamic shadows for what moves (2026-10-23, the owner)

The shadow system was built before the map files were read. It still draws every caster again each frame, from
what the client draws. The files now give the place and the model of everything that stands still, so its shade
can be drawn once and kept. Only players, creatures and moving objects need a shadow drawn each frame.

What supports it:

- The terrain, the buildings and the outdoor doodads already come from the files. The files' own draws cost 0.33 ms
  of CPU a frame at Goldshire. The cost is the client's draws that are matched and drawn again each frame (units,
  batched doodads, object parts) and the leaves drawn again at full size.
- The sun's azimuth is 45 degrees all day. Only its height changes, about 10 degrees an hour of game time: one
  degree in about 6 minutes.

GPU time a frame at Goldshire, moving, 1920 x 1080 (perf-2, 2026-10-23): shadow maps 7.2 ms (near leaves 2.5, far
leaves 2.1), sun shadows 2.1, light and fog 1.7, lamp fog 0.9; the GPU's whole frame 18.9 ms, 4.1 with the effects
off.

### Steps, in this order

1. **Static maps, drawn once and kept.**
   - The world from the files goes into maps fixed in the world, at the near, middle and far levels.
   - One texture a level holds the solid, leaf and terrain depths in its channels. A level is drawn rarely, so the
     cost of blending does not matter, and the shaders read the three depths in one fetch.
   - A level scrolls as you walk: only the strip that comes into range is drawn. Running moves about 4 texels a
     frame on the near level.
   - **The sun: background rebakes (the owner's choice).** On a sun step the levels are drawn again in the
     background, a part each frame, and swapped when done, with a short crossfade. Steps of about 0.25 degrees,
     one every 90 seconds or so. A jump of the clock (login, a teleport, a change of time) rebuilds at once.
2. **One dynamic map, drawn each frame:** players, creatures and moving objects (ships, doors that open,
   elevators), near you only. A ship may also need the far level.
3. **The shaders.** The sun shadows read the static level and the dynamic map. The light's march reads only the
   static far level: a unit's shadow in the fog does not show.
4. **Fill the gaps in the files**, so the bake can replace the cache:
   - **Server objects that stand still are baked (the owner's choice):** mailboxes, signs, closed doors, housing
     furniture. Their place comes from the object table, their model from the files. An object that moves goes to
     the dynamic map.
   - Animated doodads (mill wheels, flags): baked in their rest pose, or dynamic.
   - Furniture inside buildings, which comes from the client's draws now.
5. **Remove the cache's work for static casters:** matching client draws to the files, the keep test, eviction.
   The cache keeps units and moving objects only.

Not in this plan: the full-screen passes (sun shadows and light and fog, about 2 ms each, per pixel) get cheaper only
through fewer reads. The water (8.3 ms at Auberdine) is a separate job.

Later, decided by look after step 1: bake the volumetric light into a 3D grid of where the sun reaches, built from
the static maps and scrolled the same way. The risk is leaf shafts finer than the grid.

**Measuring:** build first, check the look with screenshots while building, and measure in motion once when a step
works. Ask the owner before any run longer than a few minutes.

### Left from the night of 2026-10-23

- **The units' draws** (CPU, towns): about 1,000 unit parts at Goldshire. With one dynamic map they are drawn once,
  not into three maps. One draw per unit where its parts share a buffer, or the bones uploaded once per unit, may
  still help.
- **The water** (7.8 ms at Auberdine while moving): the baked-texture rewrite below.

## Ideas for the rewrite

The aim of each is a cost that stays the same when the camera moves.

- **Shadow casters from the map files.** Done before this plan: terrain, buildings, their outdoor doodads and the
  tiles' doodads come from the files (mapterrain.cpp), and the client's draws of them are refused. The census of
  2026-10-23 found the town cache to be units, not buildings.
- **Volumetric light at a fixed cost.** March into a low-resolution grid (for example 160 x 90 cells across the screen,
  64 slices deep) instead of once a pixel, then read the grid for each pixel. The cost depends on the grid, not on
  the screen size or on what is in view. Or march at half or quarter resolution and widen with a depth-aware
  upsample.
- **Fewer shadow maps in towns.** Taken up by the plan above: what stands still is drawn once.
- **Water from baked data.** The shore distance, the foam field and the bed's slope do not change while the player
  stays in an area. Bake them into textures once per area, and keep the per-pixel shader to a few reads.
- **GPU work across frames.** D3D9 gives one graphics queue, so the GPU cannot run two passes at once. What can
  spread is the work: update a part of the shadow maps or the grid each frame, with a cost that does not jump when
  the camera turns.

## Where the long form is

`NOTES.md` sections:

- "Frame drops while turning, and wow-test-tool (2026-10-02)"
- "Performance: the timers, the shadow pipeline and the water (2026-10-06)"
- "Performance: the volumetric light (2026-10-07, perf-1)"
- "Performance: the water at 2560x1440 (2026-10-11)"
- "Performance: the command stream, the game thread and the GPU at 1920 x 1080 (2026-10-21, the perf branch)"
- "Performance in motion: units, the pose search and the leaves' maps (2026-10-23, perf-2)"

Also `changelog/2026-10-06-performance-tests.md` (the performance tests and their page). Tools: `tools/prof`
(comfyprof, `/atmos prof`), `tools/perf` (Fresh-Client, Run-Perf, AB-Build, AB-Move, AB-Dxvk, AB-Stream, Diff-Shots,
shader.py), `/atmos framelog`.
