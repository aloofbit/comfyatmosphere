# Creatures under the sea

**Commits:** `49d3022`, `5ebaa69`

## What changed

- **Creatures and objects under the water keep their texture** and show dim through it. Seen from above the surface
  they showed as flat, pale shapes, and more so with fog.
- **The fog ends at the water's surface.** The water gives what lies under it its colour and its fade.
- **Underwater Cover adds water only over the first yard.** Legs just under the surface look as before.
- **The water's surface hides what lies under it from the passes after the water.** The volumetric light and the sun
  shadows now stop at the surface over a creature, as they do beside it.

## Why

- The water's draw over a creature writes no depth, so the fog went on under the surface into its thickest layer
  and laid pale mist over the creature.
- Underwater Cover tripled the water in front of a body at any depth. A creature 10 yards down was behind 30 yards
  of water and showed only the water's deep colour.
- The volumetric light and the sun shadows read the creature's depth, under the surface, and drew over the water
  there: a pale shape (or a dark one with shadows on), even at Water Clarity 25.

Test: `darkshore-underwater-npcs`.
