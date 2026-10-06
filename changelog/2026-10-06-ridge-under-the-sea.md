# A ridge under the sea

**Commit:** `87acd0d`

## What changed

In Stormwind's harbour a ridge under the sea showed as a dark shape with a hard edge against the open sea beside
it, at any Water Clarity. It no longer does: the water is one colour from the open sea across the ridge.

## The cause

The sun shadows add Sunlight (a brightening) and a warm sun tint to what the sun reaches. Under the water they
work on the bed seen through the water, and they faded both with the bed. So over the ridge the water lost its
Sunlight and tint. The open sea beside it has no bed under it, so it kept both on its surface. The edge between
the two was the ridge's outline.

Now only the bed's shade fades with the water. The Sunlight and the tint go by the water's surface. Away from the
water nothing changes.

## Tried and taken out

- Water Clarity acting by its square under 100.
- Fading the bed by the water's distance from the camera.
- **Underwater Fog**, a slider that faded the bed into the water's colour by its distance from the camera. It hid
  the ridge but not the edge, and it did the same job as Water Clarity, which is the physical one.

## Debug views

| View | Shows |
| --- | --- |
| 16 | The water as drawn, with marks over what lies under it: magenta for the far terrain slice, orange for untextured terrain, cyan for textured terrain past 80 yards |
| 19 | What lies under the water: red for the far terrain slice, blue where only sky is behind |
| 28 | Where the sun shadows find the water over a bed (blue), and where they do not (grey) |
| 29 | The depth under the water on a doubling scale, blue to red over 0 to 256 yards |

View 16 needed a new stencil bit, 0x08, for a draw with no shader and no texture. The game draws its untextured
terrain far off that way.

## Test

`tests/stormwind-harbour-ridge.json`: the normal view and View 28 at the place where the edge showed.
