# Lighthouses: no second light, and a distance

**Commits:** `8662a63`

## What changed

- **The game's own lighthouse light is left out again** in Stormwind's harbour, and our lamp stands in the lamp
  room. The search for the game's light now counts only lighthouse doodads.
- **A Lighthouse Distance slider** sets how far off a lighthouse shows. It fades out over the last quarter of that
  distance: the beacon, the beams, the glitter on the water and the light on the tower.
- **The beam's light on the water ends where the beam ends** (Beam Length).

## Why

- From the harbour shore the game's light showed beside ours. The search took the first 64 animated doodads of any
  name, and the harbour has more, so the lighthouse's doodad was not found.
- Every lighthouse within 1500 yards was drawn, and the lit wave faces ran along the beam to the horizon: glare on
  the screen from far off.

## Controls

| Control | Tab | Range | Default |
| --- | --- | --- | --- |
| Lighthouse Distance | Lamps | 100 to 1500 yards | 600 |

`comfyatmos.ini`: `[lighthouse] reach`.

Test: `stormwind-lighthouse-light`.
