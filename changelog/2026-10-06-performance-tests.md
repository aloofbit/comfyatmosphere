# Performance tests

## What changed

Two tests measure performance, and the test page shows the results at the top.

- **`load-performance`:** restarts the client at Stormwind's harbour and reads the first 90 seconds in the world.
  It checks the compiling left on the game's thread (at most 100 ms), the longest frame once the effects start (at
  most 1.5 s) and the frames over 40 ms after it (at most 12). It records when the effects started and how long
  the shader worker took.
- **`frame-rate-performance`:** measures the frame rate for 10 seconds at three places, with fixed test settings
  and a fixed sun, then again with every effect off, to show what the effects cost: Stormwind's harbour (water,
  ships, shadows), Elwynn's ridges (trees and shade) and the road outside Ironforge's gate (lamps and fog). It checks
  at least 50 frames a second and the slowest 1% of frames at most 40 ms at each place, and records the mod's CPU
  time a frame and the frame rate without effects.

**The frame-rate test needs the game window in front.** Behind other windows the game holds 60 frames a second,
with the effects on or off. The test brings the window to the front for each measurement and gives the focus back
after. Do not type in another window while it runs; the runner prints a note at the start.

## The Performance panel

At the top of every test page and of `results/last-results.html`, for people reading the results:

- **A verdict:** all measurements within their limits, or which are not.
- **Test system:** the GPU, CPU, memory, Windows build, the game window (resolution, multisampling, vsync), the
  client and the mod's build, from the client's start-up report. `comfyfog.ini` opens a dialog with every setting of
  the test client's file; **Test settings** opens one with the controls the frame-rate test applies, by tab.
- **Signing in:** "No freeze" or "Freezes", the largest stutter, the slow frames after it, the compiling left in the
  game, and a chart of the largest stutter over the runs.
- **Frame rate:** a card for each place with its frames a second and a bar: its whole length the rate without
  effects, grey to the rate with them, orange what the effects take. Then three charts with a coloured line for each
  place: frames a second, the effects' cost in ms a frame, and the stutter.
- **All numbers:** every recorded value, folded away.

The numbers are kept in `results/perf-history.json`. The performance tests have no section of their own on the
page; their pills link to the panel.

## Results so far

| | Before the shader cache | With it |
| --- | --- | --- |
| Compiling on the game's thread at sign-in | 11,091 ms | 2 to 3 ms |
| Largest stutter as the effects start | 6,985 ms | 0.93 to 0.95 s |

With the game in front (2026-10-06, vsync off):

| Place | With effects | Without | Effects cost |
| --- | --- | --- | --- |
| Harbour | 80 fps | 120 fps | 4.1 ms a frame |
| Elwynn | 77 fps | 117 fps | 4.5 ms a frame |
| Ironforge | 120 fps | 120 fps | none measurable |

## Runner and tool additions

- `tests/Run-Tests.ps1`:
  - `restart` in a test's config: the client is restarted and signed in first.
  - A `go` step: to another place within a test.
  - `gameFront` and `gameBack` steps: the game window to the front and the focus back.
  - A `log` check: a number read from `comfyfog.log`, against a limit, and recorded with `metric`.
- `tools/wow-test-tool/Run-Test.ps1` in comfy-wow: the `front` and `back` commands.
- The DLL's start-up summary gives when the effects started and the longest frame from then on.
