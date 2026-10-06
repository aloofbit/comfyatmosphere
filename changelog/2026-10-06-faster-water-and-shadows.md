# Faster water and shadows

## What changed

The water and the shadows cost less. The frame log now times each pass on the GPU and the CPU.

| Place | Before | After | Without effects |
| --- | --- | --- | --- |
| Stormwind's harbour | 73 fps | 84 fps | 178 fps |
| Elwynn's ridges | 72 fps | 75 fps | 121 fps |
| Ironforge's gate road | 144 fps | 156 fps | 271 fps |

Measured with `frame-rate-performance`, the game window in front, vsync off.

## The water

- The water shader's early cuts work. In DXVK a pixel that `clip()` drops runs the rest of the shader, so each of the
  three draws over a chunk shaded every pixel of the chunk. A branch now leaves after the cuts. The water's GPU time
  went from about 9 to 6.5 ms a frame at the harbour.
- The draw over bodies in the water writes no depth, so the stencil turns other pixels away before the shader runs.
- Each chunk's wet cells are kept, not read from the game's index buffer every frame (1 ms of CPU a frame).
- The state that is the same for the whole frame is set once a frame, not for each of about 400 chunks.

## The shadows

- Ground the map files hold is refused before it is copied.
- A model draw's constants are copied only as far as the model uses.
- The replay lists the cache once for the seven maps and skips binds that are already in place.
- The maps clear depth and stencil together.
- The map files' buildings are drawn by group and their doodads by blocks of 66 yards, each only where it can reach the
  map.
- The player is looked up once a frame.

## For measuring

- `/atmos framelog` and Alt+F12 give our CPU time in all, our GPU time for each pass and each shadow map, the GPU's
  whole frame, and the water's split.
- `[water] debugSkip` leaves out one of the water's draws, to measure it.
- `tests/water-cost.json` measures the water's parts at the harbour, with every effect on between them.
