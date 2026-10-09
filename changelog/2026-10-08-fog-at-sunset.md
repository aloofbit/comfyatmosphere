# The fog at sunset

In v0.12.0-alpha (branch `0.12.0`).

**Commits:** `8c856ee`, `3adde28`, `3f76848`, `c9fa07f`, `c64a140`, `4825dd4`, `d3450a8`, `adcd43f`, `8ce8199`,
`cf2a008`, `1dde227`, `231b40a`, `cef88b3`, `70981b8`, `472889d`, `59bfd96`, `d6a1013`, `5893de0`, `8af5f78`,
`7bdb24e`, `064902d`

## What changed

- **Fog Toward the Sun**, a slider on the Fog page (`comfyMistToward`, `[fog] toward`, 0 to 90%, default 30%). The
  fog's sunlight has its own phase, apart from the volumetric light's: the fog is brighter looking toward the sun
  and darker looking away.
- **The fog's sunlight has its own gain**, 0.25 x Fog Sunlight, whatever Light Strength is. It is full until the
  sun's centre is 1 degree under the horizon, and none at 2.6 degrees under. Before, it was lit at Light Strength
  and faded out below a sun height of 0.1, and half set the glows were down to a fifth.
- **Fog Horizon Glow**, a slider on the Fog page (`comfyMistHorizon`, `[fog] horizonGlow`, 0 to 200%, default
  100%). With the sun low the fog glows along the horizon, brightest under the sun, in the hue of the sky's glow.
  - It sits on the water's horizon as seen 2000 yards off, where the game's far mesh meets the sky, not at eye
    level. It reaches 7 degrees over it across far land and the sky, and 1.5 degrees over near terrain.
  - It falls on water and far things only. Near dry land (the ground texture's dry cells) takes none, and nor does
    anything more than a yard over the water's surface (a boat, a pier).
- **A narrow glow round the sun**, half at 3 degrees, so the fog under a sun on the sea's horizon continues the
  game's aura and does not end it in a flat bright edge. It falls on water and far things only, as the horizon
  glow: an island 200 yards off no longer shows the sun through it.
- The sky is cleared round the sun's disc out to 4 degrees, not 9. A low sun shows no round hole in the fog cut off
  flat by the sea's horizon.
- Far off, the fog takes the sky's colour just over the horizon at full brightness, not at Fog Brightness. The open
  sea shows no dark strip between the sky and the water.
- **The mist toward the sky's glow.** Where the game's fog colour is dark, the mist's light moves toward the sky's
  glow over the horizon. In Tirisfal Glades at 20:00 the fog colour is 0x222226 under a green-grey sky, and the
  mist was as dark as the sea.
- **The fog's ground level over water** is the water's surface, not the floor under it. Over Stormwind's harbour it
  lay 86 yards under the sea, and the fog over the water was a tenth as thick.
- A light value past 16 is capped, not set to zero, so a strong glow at the sun cannot draw a black spot.
- The probe logs the glows' sea height and horizon.

## Why

The owner tuned the fog and the water at sunset off Tirisfal Glades (now the test `sunset`). The fog stopped at a
flat edge under the sun's aura, a low sun cut a round hole in it, and the far sea showed a dark strip at the
horizon.

## Tried and reverted

Four water changes (`7d73abb`, `68840b2`, `46817ba`, `ffe434a`) put the sun's halo into the water's reflected sky
and a band of light into the haze the far water fades into. They were reverted (`0e52504`, `39e4c82`, `80026c7`,
`72761bb`): the fog's glows took that job. A limit on Fog Horizon Glow by how far the view reaches (`7986298`) was
reverted too (`ce1b4dc`).

## Controls

| Control | Tab | Range | Default |
| --- | --- | --- | --- |
| Fog Toward the Sun | Fog | 0 to 90% | 30 |
| Fog Horizon Glow | Fog | 0 to 200% | 100 |

`comfyatmos.ini`: `[fog] toward`, `[fog] horizonGlow`.

Test: `sunset`.
