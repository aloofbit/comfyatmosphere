# Wet sand along a river

In v0.12.0-alpha (branch `0.12.0`).

**Commits:** `a28b98a`, `3bd15e4`

## What changed

- **The wet sand follows a river's waterline.** Each 8-yard cell of the map's water now takes the mean height of
  its four points, not the chunk's highest level. The fog's ground and its glows read the same heights.
- **The band no longer steps at each cell's edge.** The wet pass weighs the wet cells round a point by how near
  their middles are, in place of the highest of the nine.
- **No hard dark line far off.** A pixel far off covers more height than the wet band, and took the band whole from
  its middle. It now darkens only its share of the pixel's height.
- **Wet Sand defaults to 20** (was 45), the owner's value at the river in Old Hillsbrad.

## Why

- A river runs downhill across a chunk, and the map files give every cell the chunk's highest water level. The
  cells downstream stood up to a yard over the water. On a gentle bank the wet band lay as a contour on the grass
  well over the river (Old Hillsbrad, the owner).
- Where Stormwind's lighthouse rock meets the sea, some 300 yards off, the band drew a hard dark line.

## Controls

| Control | Tab | Range | Default |
| --- | --- | --- | --- |
| Wet Sand | Atmosphere | 0 to 100% | 20 |

`comfyatmos.ini`: `[water] wetSand`.

Test: `telabim-wet-sand` has the river as a second place: rows on the far bank's grass dark and rows at its
waterline bright, in the wet sand alone.
