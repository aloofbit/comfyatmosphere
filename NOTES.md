# ComfyFogAndRays design notes

**Status: working in game, all in one DLL (`comfyfog.dll`, sources in `src/`).** Fog, volumetric light
(depth + sun shadow map + ray-march), and cloud removal, with controls in the game's options through the
ComfyAtmosphere addon. The screen-space sun rays (`rays.cpp`) were removed on 2026-09-23: they were a step
toward the volumetric light, which replaced them. Git history keeps them. The sun direction and the camera
they tracked moved to `sun.cpp`, their light colour to `[volume] color`. Time-of-day control, first
built here, is now its own DLL: comfytime (https://github.com/aloofbit/comfytime). Everything is tuned from
`comfyfog.ini` and reloads with F11. The sections after *What was found* are the original feasibility
write-up, kept for the reasoning. Where they disagree with *What was found*, *What was found* is what
measurement showed.

| Piece | File | Keys |
| --- | --- | --- |
| Fog: one `thickness` dial, haze floor + gentler climb | `comfyfog.cpp` | Shift+F11 toggle |
| Sun direction and world camera, for the shadow map and the light | `sun.cpp` | none |
| Volumetric light: fog lit by the sun, shaded by the shadow map | `volume.cpp` (+ `depth.cpp`, `shadow.cpp`) | Alt+F11 toggle |
| Clouds off: `[sky] clouds = 0` | `comfyfog.cpp` | none |
| In-game controls: CVars for the addon's sliders | `cvars.cpp` | none |
| Diagnostics | all | F12: one-frame probe, then a 180-frame trace of the light and the map |

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

What the light still does not do: the glow is smoothed over time (`[volume] smooth`, eased off as the
camera turns) because the march is noisy and the map changes under it; and the bias grows with how
steeply a line of sight runs into the map, without which a low sun flickered badly around itself.

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
- Does the client's directional light stay sane indoors and at night, or does it need gating?
- Do fog state overrides fight anything? Does the client set fog per draw, or per zone?

## Build

Same as comfygrass. **32-bit only**: the 1.12 client is x86.

```
cmake -B build -A Win32
cmake --build build --config Release
```
