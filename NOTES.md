# ComfyFogAndRays — design notes

**Status: working in game, all in one DLL (`comfyfog.dll`, sources in `src/`).** Fog, screen-space sun
rays, world-space light shafts, cloud removal and time-of-day control. Everything is tuned from
`comfyfog.ini` and reloads with F11. The sections below *What was found* are the original feasibility
write-up, kept for the reasoning; where they disagree with *What was found*, the latter is what
measurement showed.

| Piece | File | Keys |
| --- | --- | --- |
| Fog: one `thickness` dial, haze floor + gentler climb | `comfyfog.cpp` | Shift+F11 toggle |
| Screen rays: radial blur toward the sun, before the UI | `rays.cpp` | Ctrl+F11 toggle |
| World shafts: beams on a world grid around the player | `beams.cpp` | Alt+F11 toggle |
| Clouds off: `[sky] clouds = 0` | `comfyfog.cpp` | — |
| Time of day: `[time] hour`, stepped in game | `timeofday.cpp` | Ctrl+PageUp/PageDown |
| Diagnostics | all | F12 one-frame probe, Ctrl+F12 clock search |

## What was found (measured in this `WoW.exe`)

**Fog.** Linear (`FOGVERTEXMODE` 3), re-set many times a frame (zone colour ↔ black for additive
passes). M2s (trees, characters) fog in their vertex shaders from `c30 = (-1/(end-start), end/(end-start))`
— read nothing else in all 19 shaders that use it. comfyfog waits for comfygrass to patch first and
chains on top, so grass sees the rewritten fog.

**World → UI boundary.** The first switch from a perspective to an orthographic projection each frame,
after some world has been drawn (the client also flips at draw 0 with nothing drawn). `ZENABLE` is never
set, so it is no marker. With **Full Screen Glow** on (the default) the world is drawn into an off-screen
render target; after the switch the client blurs it and draws world + glow onto the back buffer in one
unblended, pixel-shaded draw. Rays drawn at the switch were painted over — they now run just before the
first back-buffer draw with no pixel shader (the first UI draw, glow on or off).

**The sun is not the directional light.** The world light is a fixed direction (azimuth 45°, elevation
28°, orange) that never follows the time. The visible sun is the **first draw of the frame**: a unit quad
(4 vertices, strip, identity world, no depth writes) under a special view matrix whose *translation* is
the sun's camera-space position. The world camera never has a translation (this client folds the camera
into every world matrix), so views with one are excluded from the camera mirror.

**The sky** is the first three draws: sun quad, untextured additive dome (sky colour), textured
alpha-blended strip (~177 vertices, the clouds). All leave depth writes off; the first depth-writing draw
is terrain. Clouds are skipped by that rule — an identity-world test never matched.

**Game time** (Ctrl+F12 search against the minimap clock): `0x00CE9B60` int minutes, `0x00CE9B64`
float day fraction (continuous), `0x00CE8574` float minutes. In the world the client rewrites them every
frame between `BeginScene` and `Present`, so the chosen time is written at `BeginScene`, just before the
sky reads it. A second pair at `0x00CE9D00/04` runs at the same rate 78 minutes behind — not understood,
not written.

**Why screen rays alone were not enough.** They are rebuilt from the image each frame, so they swing as
the camera moves; a `parallel` blend was simulated and swings *more* (75° vs 58° over a ±40° pan), because
the fan toward the sun already is the correct perspective of parallel shafts. What steadied them: a
brightness reference eased over time (`adaptTime`), intensity by view angle (`viewFalloff`), a sharper
falloff around the sun (`falloff`), and brightness relative to the frame's peak (`relThreshold`) — under a
foggy canopy nothing reaches a fixed threshold (the fog colour itself sat at 0.33). The world shafts
(`beams.cpp`) are the answer to "stays put while I look around": world-grid placement, depth-tested at the
end of the world pass, screen-blended so they vanish against bright sky, faded out in the open by a canopy
estimate from the top half of the frame.

Goal: fog control and sun shafts (crepuscular rays) for the 1.12 client, in the same shape as
[`comfygrass`](../comfygrass) — a DLL loaded by VanillaFixes from `dlls.txt`, hooking DXVK's
`IDirect3DDevice9` vtable, with everything tunable from an ini and reloadable in game.

Read [`comfygrass/src/README.md`](../comfygrass/src/README.md) before starting. It has the attach
mechanism, the reversed addresses and how each was verified, and four architectures that did not work.

## The framing that matters

**comfygrass is a vertex-shader substitution mod. This is a post-process mod.** Different category of
work. comfygrass never allocates a surface, never binds a pixel shader, and hooks `Reset` only to drop
a shader. All of that is new here, and it is where the crashes will be.

About 60% of the scaffolding transfers. The rest is render-target work comfygrass never had to do.

## Two mods, not one

They are bundled in the ask but they are nowhere near the same difficulty. Ship them separately.

| | Effort | Risk |
| --- | --- | --- |
| Fog control | an evening | ~none |
| Sun shafts | a weekend or two | moderate |
| Height/volumetric fog | +1 week | high — needs depth, see *Deferred* |

## What transfers from comfygrass

Precise anchors, because these took real time to get right the first time:

| Piece | Where | Note |
| --- | --- | --- |
| `HookSlot` — vtable patch | `comfygrass.cpp:91` | Patch **in place**, never a copy. A copied vtable drops the RTTI word DXVK keeps behind `vtable[0]` and the client silently gives up before `CreateDevice` — black window, no error. |
| Probe-device attach | `comfygrass.cpp:1375` | Every DXVK `IDirect3DDevice9` shares one class vtable, so a throwaway device names it and the client's real device is caught whatever the order. Also pins `d3d9.dll`. ~250–430 ms, once, background thread. |
| Runtime HLSL compile | `comfygrass.cpp:904` | `d3dcompiler_47` via `GetProcAddress`. Add `ps_2_0` next to the existing `vs_2_0`. |
| Mirrored `SetTransform` | `comfygrass.cpp:1244` | world / view / proj. Needed here to project the sun to screen space. |
| Mirrored `SetRenderState` (`g_rs`) | `comfygrass.cpp:1263` | Already carries every fog state. This *is* the fog mod. |
| **Directional light read** | `comfygrass.cpp:1049` | Walks `GetLightEnable`/`GetLight` for the first enabled directional light. **This is the sun.** The part I expected to be hard is already done. |
| F9 frame capture | `ProbeDraw`, dumps at `comfygrass.cpp:349–380` | Per-draw prim type, stride, world/view/proj and fog states. This is the tool for finding the world→UI boundary (below). |
| F10 ini reload + toggle | — | Matters more here than it did for grass: a rays effect is all tunables (decay, density, weight, exposure, threshold, sample count). |

Also inherited, and worth restating as a rule: **comfygrass only ever acted on draws it could positively
identify, and did nothing when unsure.** A post-process touches every frame unconditionally, so it needs
a deliberate equivalent — bail out and pass through cleanly on any failed surface creation or lost
device. The failure mode to avoid is a black screen where a missing effect belonged.

## Part 1 — fog

Intercept in the existing `hkSetRenderState` and substitute: `D3DRS_FOGSTART`, `D3DRS_FOGEND`,
`D3DRS_FOGCOLOR`, `D3DRS_FOGDENSITY`, `D3DRS_FOGTABLEMODE`. Drive from an ini; optionally per-zone or
per-time-of-day.

Nice side effect: the grass shader reads fog out of that same mirrored state map
(`comfygrass.cpp:1024`), so if both DLLs are loaded the grass follows the new fog for free.

Note comfygrass's known gap — its shader fogs on `pos.w`, so **range fog would differ**. If this mod
ever enables `D3DRS_RANGEFOGENABLE`, that gap becomes visible on grass.

## Part 2 — sun shafts

Classic post-process radial blur toward the sun (GPU Gems 3, "Volumetric Light Scattering as a
Post-Process"). Four steps:

1. Capture scene colour to a quarter-res render target.
2. Mask it — bright pixels near the sun survive, everything else goes black.
3. 2–3 radial blur passes accumulating toward the sun's screen position.
4. Additive blend back over the scene.

Cost at quarter res is well under a millisecond. Perf is not the problem here; correctness is.

**Sun screen position.** Take the directional light from `comfygrass.cpp:1049`, negate it, project
through the mirrored view-proj. Fade the effect out as the sun leaves the frustum or goes behind the
camera — not fading is the classic artifact.

Remember the client renders **camera-relative** (`view[3] = 0,0,0,1`; the camera position is folded into
each world matrix). For a *direction* that does not matter, which is why the sun is easy and the grass
wind phase was not. Do not re-derive this the hard way.

### The three things that will eat the time

**1. The occlusion mask — use luminance, not depth.**
Proper shafts want depth to know what blocks the sun. In D3D9 that means forcing the depth-stencil to
`INTZ` — hooking `CreateDevice`'s `AutoDepthStencilFormat` and substituting a texture-backed surface.
DXVK does support the INTZ hack. **Do not start here.** Threshold the colour buffer instead: sky near
the sun is bright, trees are dark, the silhouette falls out for free. Period-correct technique, fails
only on snow and water glare, and looks right in exactly the forest case that motivates the mod.

**2. The world→UI boundary — the actual hard part.**
Composite at `Present` and the UI is already drawn, so the rays smear the action bars. The pass has to
go in after world geometry and before UI. The marker is the switch to an orthographic projection, or
`ZENABLE` going off, on the first 2D draw. `SetTransform` is already mirrored and F9 already dumps
per-draw matrices — so the instrument for finding that boundary *in this specific binary* exists.
Expect most of the debugging to land here.

**3. `Reset` and state leakage.**
Every `D3DPOOL_DEFAULT` surface must be released and recreated around alt-tab and resolution change.
`hkReset` already exists (`comfygrass.cpp:1234`) but currently only drops a shader; here it becomes
load-bearing and is the classic device-lost crash.

A post-process pass stomps a dozen render states, textures, sampler states, stream sources and shaders
that the client's fixed-function pipeline needs back exactly. **Do not hand-restore** — wrap the pass in
an `IDirect3DStateBlock9` capture/apply. One call each way, built into D3D9.

## Deferred / rejected

- **INTZ depth readback.** Adds roughly a week and real breakage risk for a better occlusion mask.
  Revisit only once the luminance version is working and the limitation is actually bothering you.
- **Height fog via depth.** Needs depth *and* inverse view-projection to reconstruct world position.
  Same gate as above.
- **Per-vertex height fog in the terrain draws** (the comfygrass technique applied to terrain). Terrain
  uses many vertex formats and each would need its fixed-function path reimplemented. comfygrass's
  "lighting is approximate" gap was the cost of doing this for *one* format. Not worth it.

## Build order

1. **Fog state override.** Ships alone, proves the ini/reload loop, near-zero risk.
2. **RT scaffolding + state block + `Reset` handling**, validated with a trivial full-screen tint.
   Boring, but it is where the crashes live — isolate it before any effect work.
3. **Luminance mask + radial blur, composited at `Present`.** Accept the UI smear so the effect is
   visible and tunable.
4. **Move the composite before the UI.** F9 captures to find the ortho transition.

## Open questions for the keyboard

- Where exactly is the world→UI transition in this `WoW.exe`? (F9 capture, look for the proj matrix
  going ortho.) Is it stable across zones, and with the map or character sheet open?
- Does DXVK's `StretchRect` from the backbuffer behave at quarter res here, or is a full-res
  intermediate needed?
- Does the client's directional light stay sane indoors and at night, or does it need gating?
- Do fog state overrides fight anything — does the client re-set fog per draw, or per zone?

## Build

Same as comfygrass. **32-bit only** — the 1.12 client is x86.

```
cmake -B build -A Win32
cmake --build build --config Release
```
