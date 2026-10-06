# Shadows on and under the water

**Commit:** `87acd0d`

## What changed

- **Shadow on Water.** A shadow on the water's own surface is soft and lighter than one on the ground. Before, a
  ship's shadow lay on the open sea as hard and as dark as on land.
- **The shade on the sea floor.** It now stays in view as long as the floor itself does. Before, a moored ship's
  shadow on the harbour floor was too faint to see, while the floor showed clearly.

## How it works

- **Water with no bed in view** (the open sea): the shade falls on the surface and is scaled by Shadow on Water.
- **Water with a bed in view** (the harbour, the shallows): the bed keeps its own shade, seen through the water.
  The surface's own shade is added at the Shadow on Water strength, read from the shadow maps with a softer edge.
  So a dock casts a shadow on the water beside it, not only on the floor yards below.
- **The floor's shade fade** now uses the water channel that keeps the floor in view, the same absorption the water
  shader uses at the current Water Colour and Clarity. It was 0.25 a yard divided by Clarity, about 2.5 times
  sooner than the floor itself faded.

## Controls

| Control | Tab | Range | Default |
| --- | --- | --- | --- |
| Shadow on Water | Shadows | 0 to 100% | 40 |

`comfyfog.ini`: `[sunshadows] water`.

## Test

`tests/stormwind-harbour-ship-shadow.json`: a moored ship seen from above the harbour, with the normal view and
Debug Views 5, 24 and 28.
