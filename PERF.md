# Performance: where it stands

The current state of comfyatmos's frame rate, and the plan. The measurements and the reasoning behind each line are
in `NOTES.md`, in the sections named at the end. Update this file when a finding changes; put the long form in
`NOTES.md`.

Last updated 2026-10-23, after the second night on `perf-2`, measured in motion.

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

## Next

1. **Towns, CPU: the units' draws.** About 1,000 unit parts held at Goldshire, drawn into the units' map, the near and
   middle maps. Ideas: one draw per unit per map where its parts share a buffer (merge index ranges), or the parts'
   bones uploaded once per unit.
2. **Towns and forests, GPU: the near leaves' map** (2 ms at Goldshire). Held in place it is kept on most frames in
   the open, not in towns: the client's batched doodads and objects drawn from changing buffers come and go. Their
   shade could come from the files (the objects' models are known), or they could go to a small map of their own.
   A held map at the same texel size would need a 4096 map over a wider box: check the VRAM first.
3. **The water** (7.8 ms at Auberdine while moving): the baked-texture rewrite below.
4. **The light's march** at a fixed cost (1.2 to 3 ms).

## Ideas for the rewrite

The aim of each is a cost that stays the same when the camera moves.

- **Shadow casters from the map files.** Done before this plan: terrain, buildings, their outdoor doodads and the
  tiles' doodads come from the files (mapterrain.cpp), and the client's draws of them are refused. The census of
  2026-10-23 found the town cache to be units, not buildings.
- **Volumetric light at a fixed cost.** March into a low-resolution grid (for example 160 x 90 cells across the screen,
  64 slices deep) instead of once a pixel, then read the grid for each pixel. The cost depends on the grid, not on
  the screen size or on what is in view. Or march at half or quarter resolution and widen with a depth-aware
  upsample.
- **Fewer shadow maps in towns.** Find which of the seven maps the light's march needs, and what a town's buildings
  add to each. A town's buildings do not move: their depth can be drawn once per area, not once per rebuild.
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
