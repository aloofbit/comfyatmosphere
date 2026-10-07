# Water: no split, no broken wet sand, no camera inside a wave

**Commits:** `3af2e79`, `79d6e72`, `d31092d`, `a9930aa`

## What changed

- **The surface no longer splits** where a lake's chunk meets the sea's. The swell is now chosen for each point of
  a chunk, by the sea cells round it. A chunk is the sea when any of its own cells is.
- **The wet sand follows the whole shore.** It now reaches up banks of up to 60 degrees, and fades out by 72.
- **A Wet Sand slider** sets how much darker the sand is just above the water. 0 turns it off.
- **A swimmer's camera stays out of the waves.** Within 15 yards of the camera the swell rises no higher than 0.3
  yards under it, and it grows back to full height by 40 yards. A wave further out that stands as high as the
  camera is drawn as water.

## Why

- By Auberdine the sea floor showed through straight gaps in the water: one chunk counted as a lake and stayed flat,
  while the sea's chunk beside it rose and fell.
- On a steep beach east of Tanaris the wet sand lay in pieces with straight edges. It was drawn only on ground under
  32 degrees, one terrain triangle at a time.
- Off the Westfall coast with Wave Height 30 the sea floor showed as dry sand in clear air. The game keeps a
  swimmer's camera just over its own flat water, and our swell rose over the camera.

## Controls

| Control | Tab | Range | Default |
| --- | --- | --- | --- |
| Wet Sand | Water | 0 to 100% | 45 |

`comfyatmos.ini`: `[water] wetSand`.

Tests: `darkshore-water-split`, `telabim-wet-sand`, `westfall-under-the-swell`.
