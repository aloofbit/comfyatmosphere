# The zone's sky and water colours

Branch `zone-colours`, not released.

## What changed

- **The water reflects the game's own sky.** The sky the water shows was one fixed blue (`[water] skyColor`). It is
  now the game's sky, read from its sky dome four times a second: the fog colour at the horizon, the glow just
  over it (1 to 6 degrees), and the sky about 20 degrees up. It follows the zone and the hour: the orange band
  and a purple sky at a Stormwind dusk, amber in the Swamp of Sorrows. `[water] skyFromGame = 0` puts the fixed
  colour back.
- **The sun's glint takes the glow's hue:** near white at noon, as before, and orange with the sun low.
- **Zone Colour**, a checkbox on the Atmosphere page (`comfyWaterZone`, `[water] zone`, on by default). The colour
  deep water turns is the game's own water colour for the zone and the hour, in place of Water Colour: turquoise
  at Tel'Abim, grey at Darkshore, brown in the swamp's lakes. The game's hue is taken at 1.75 times its
  saturation, and warmed by the sky's glow with the sun low. Water Brightness still sets how light it is. Water
  Colour is greyed out while it is on, and is used until the game's water is read.
- The colours ease over about half a second, so they do not jump at a zone's edge.
- The probe logs the sky dome's colours by elevation, the game's water ramp, and the colours in use
  (`water: zone colours`).
- The addon: a control's `dependency` may hold more than one CVar and value; every one must hold.

## Why

The fog already took the game's colour for the zone and the hour. The water did not, so a dusk or a dark zone
showed a midday blue in the water (the owner, at Booty Bay: "the water should be more reddish with sun low"). A
slider that mixed the two was tried first and read as too blue at 50. The readings are in `NOTES.md` ("The zone's
sky and water colours").
