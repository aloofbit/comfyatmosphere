# ComfyFogAndRays — design notes

**Status: Part 1 (fog) built as `comfyfog.dll` and working on terrain; M2 (tree/character) fog via the
`c30` shader constant just added, untested in game. Sun shafts not started.**

Answered by the first log: the client fogs linear (`FOGVERTEXMODE` 3), re-sets fog many times per frame
(zone colour ↔ black for additive passes), and M2 shaders fog from `c30 = (-1/(end-start), end/(end-start))`.

comfyfog: one `thickness` dial (0 = stock, 100 = heaviest) in `comfyfog.ini`. F11 reloads the ini,
Shift+F11 toggles for A/B, F12 logs a frame of fog state. It waits for comfygrass to finish patching and
chains on top, so it is the outer hook and grass sees the rewritten fog.

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
