# Rain Darkness

**Commit:** `b66b1ef`

## What changed

At night, while it rains, the world and the sky are darker again by Rain Darkness. The amount follows how hard
it rains and the Night Darkness clock. It does nothing by day.

## Why

The game's rain turns the night a lighter grey. That grey came through Night Darkness, so a rainy night looked
brighter than a clear one.

## Controls

| Control | Tab | Range | Default |
| --- | --- | --- | --- |
| Rain Darkness | Sky | 0 to 90% | 30 |

The sky takes the full amount: the moons and stars are behind the clouds while it rains. The rain amount comes
from the game's own rain draws, the same reading that Rain on Water uses.

`comfyfog.ini`: `[night] rain`.
