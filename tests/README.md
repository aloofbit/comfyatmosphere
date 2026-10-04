# Tests

Each test runs in the WoW test client with no one at the keyboard and says PASS or FAIL. It uses
wow-test-tool (`comfy-wow\tools\wow-test-tool`): its `Login.ps1` logs in, and its `Run-Test.ps1` runs the
steps in the game.

## Run

1. Install the ComfyTest addon into the test client once: `..\..\..\tools\wow-test-tool\Run-Test.ps1 -Install`.
2. Run the tests:

   ```
   .\Run-Tests.ps1                       # every test
   .\Run-Tests.ps1 far-terrain-cache     # one test
   .\Run-Tests.ps1 far-terrain-cache -NoLogin   # the client is already in the world
   ```

The runner restarts the test client unless `-NoLogin` is given. It sends `/atmos reset` before and after each test,
and sets the Debug View back to 0, so a test leaves no values behind. Do not touch the mouse or keyboard while a test
runs. The result, the script it ran, the probe and the screenshots go to `results\`, which git ignores.

## A test

A test is a `.json` file here:

| Part | What it holds |
| --- | --- |
| `config` | `character` (the slot on character select), `flight` (cast "Toggle GM Flight Mode"), `camera` (0 first person, 1 to 9 that many steps back out, 10 all the way out), `flySpeed` (yards a second), `start` (`map`, `x`, `y`, `z`, by `.go xyz`), `sun` (`azimuth`, `elevation`: a fixed sun), `ini` (`"section.key": value` pairs, by `/atmos`), `debugView` (the Debug View number) |
| `steps` | one key each: `wait` seconds, `hop` (`.go xyz` along a line), `fly` yards, `back` yards, `turn` degrees to the left, `face` (`heading`, `pitch`: the camera, in degrees, checked against comfyfog.dll's stats), `probe`, `screenshot`, `atmos`, `cvar`, `chat` |
| `expect` | checks against the last probe: `probe` is a pattern for its lines, `max` and `min` the count allowed. Or against a screenshot: `shot` (which, from 1), `row`, `from`, `to` (columns), and `min`, the least brightness allowed along that row |

## The tests

| Test | What it checks |
| --- | --- |
| `far-terrain-cache` | Hop 300 yards from a hill in the Barrens and back. The cache must hold none of the coarse ground the game drew from far off (2026-10-03). |
| `ashenvale-edge-strips` | Fly over the Barrens at the Ashenvale border, facing a sun at 29 degrees over the ridges. No long strip of shade may cross the slope that faces away from the sun (2026-10-04). |
