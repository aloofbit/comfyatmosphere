# No pale dashes or white dots on the sea

In v0.12.0-alpha (branch `0.12.0`).

**Commits:** `b77a663`, `a19f4aa`, `96755ef`

## What changed

- **No pale dashes along the horizon.** Some far chunks of the open sea carry NaN for the map's depth
  (`uv.y x 148`), and the shader's cut dropped those pixels whole. Such a depth now counts as 30 yards.
- **No white dots on the sea as the camera moves.** The game's water has cracks a pixel or two wide where the sky or
  the sea bed shows through. In the fog's composite, a pixel with something at least a fifth nearer within 2 pixels
  on both sides of it (above and below, or left and right) is a crack. It takes the nearer depth, and its own colour
  is not let through.
- **No dark specks on a lighthouse's edges.** A crack counts only where its nearer side lies on the water's surface.
  It takes the colour 2 pixels off on its farther side, from a copy of the screen, seen through the fog as there.
- `[fog] debug 3` marks the cracks in magenta over the picture as drawn.

## Why

- At Stormwind's harbour, about 1200 yards out, whole chunks of our water were missing, a pixel or two tall, and the
  game's water showed through in the sky's colour.
- As the sky, a crack took the far fog's full brightness; as the sea bed, farther than the water, it took thicker
  fog. Either way it was a light dot among the water that flickered as the camera moved (the owner, at the harbour
  at sunset, with everything off but the fog).
- The first crack test also took the sky in the notches of a lighthouse's roof and the corners of its windows.
  Covered by the fog's light alone, they were darker than the lighthouse behind its fog.
