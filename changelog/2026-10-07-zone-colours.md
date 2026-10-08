# The zone's sky and water colours

Branch `zone-colours`, not released.

## What changed

- **The water reflects the game's own sky.** The sky the water shows was one fixed blue (`[water] skyColor`). It is
  now the game's sky about 20 degrees up, read from its sky dome four times a second. It follows the zone and the
  hour: purple at a Stormwind dusk, amber in the Swamp of Sorrows. `[water] skyFromGame = 0` puts the fixed colour
  back.
- **Zone Water**, a slider on the Atmosphere page (`comfyWaterZone`, `[water] zone`, default 50). It leans the
  colour deep water turns toward the game's own water colour for the zone and the hour: turquoise at Tel'Abim,
  grey at Darkshore, brown in the swamp's lakes. Only the hue is taken. The brightness stays Water Brightness's.
  0 is Water Colour alone.
- Both colours ease over about half a second, so they do not jump at a zone's edge.
- The probe logs the sky dome's colours by elevation, the game's water ramp, and the colours in use
  (`water: zone colours`).

## Why

The fog already took the game's colour for the zone and the hour. The water did not, so a dusk or a dark zone
showed a midday blue in the water. The readings are in `NOTES.md` ("The zone's sky and water colours").
