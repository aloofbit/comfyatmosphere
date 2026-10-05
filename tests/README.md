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
   .\Run-Tests.ps1 -Accept                # and take this run's screenshots as the expected ones
   ```

The runner restarts the test client and logs in once, unless `-NoLogin` is given, and runs every test in that one session. It logs in again only for a test that wants another character, and casts flight when a test's `flight` differs from the state it is in. A run ends with flight off and the camera behind the character. It sends `/atmos reset` before and after each test,
and puts every CVar it set back as it was, so a test leaves no values behind. Do not touch the mouse or keyboard while a test
runs. The result, the script it ran, the probe and the screenshots go to `results\`, which git ignores.

## The page

Each run writes `results\<time>-report.html` and opens it (`-NoOpen` does not). It shows every test, its checks
and what they measured, and each screenshot beside the expected one, with the Debug View it was taken in. A
check that reads a screenshot sits with that screenshot. The expected shots are in `expected\<test>-<n>.jpg`,
in git. When a change is meant to alter a test's look, look at the page, then run again with `-Accept` to take
the new shots as expected.

## A test

A test is a `.json` file here:

| Part | What it holds |
| --- | --- |
| `config` | `character` (the slot on character select), `flight` (cast "Toggle GM Flight Mode"), `camera` (0 first person, 1 to 9 that many notches of the mouse wheel back out, 10 all the way out; 1 notch is 2.5 yards, 2 is 3.4, 4 is 5.3), `flySpeed` (yards a second), `start` (`map`, `x`, `y`, `z`, by `.go xyz`; `facing`, the character's heading in degrees, counter-clockwise from +x), `sun` (`azimuth`, `elevation`: a fixed sun), `ini` (`"section.key": value` pairs, by `/atmos`), `debugView` (the Debug View number), `cvars` (`"name": value` pairs for the Atmosphere page's controls, which `/atmos` refuses), `summon` (`entry`, `name`, `x`, `y`, `z`: a creature summoned there before the start and deleted by its name after the steps; only a creature with no world spawn, since `.npc delete` removes a world spawn from the database) |
| `steps` | one key each: `wait` seconds, `down` and `up` (hold a key such as `W` over the steps between), `jump` times, `hop` (`.go xyz` along a line), `fly` yards, `back` yards, `turn` degrees to the left, `face` (`heading`, `pitch`: the camera, in degrees, checked against comfyfog.dll's stats; `leftDrag` tilts it by left-drags, which leave a swimmer level), `probe`, `screenshot`, `atmos`, `cvar`, `chat`, `type` (a slash command typed into the chat box, such as `/target Pinto`), `camera` (the camera's distance partway through, as `config.camera`) |
| `expect` | checks against the last probe (or the one `probeAt` names, from 1): `probe` is a pattern for its lines, `max` and `min` the count allowed. Or against a screenshot: `shot` (which, from 1), `row`, `from`, `to` (columns), and `min` (the darkest pixel allowed), `mean` (the least average) or `meanMax` (the most average, for a row that must stay in shade) along that row. Or `shot` and `box` (`[left, top, right, bottom]`) with `jumpMax`, the percent of pixels allowed that differ sharply from the next one (speckle) |

## Snapshot

When the owner says **"snapshot"**, run `.\Snapshot.ps1` while they stand where the test is to start. It reads,
without moving anything: the place and map, the camera's heading, pitch and distance, flight (more than 3 yards
over the ground), the sun the shadows use, and every Atmosphere page control. It prints a test's `config` block
and the first `face` step, and saves them to `results\<time>-snapshot.json`. `cameraDistance` (yards) sets the
camera as far back as the owner had it; under 1.5 yards it is `camera: 0`.

## The tests

| Test | What it checks |
| --- | --- |
| `far-terrain-cache` | Hop 300 yards from a hill in the Barrens and back. The cache must hold none of the coarse ground the game drew from far off (2026-10-03). |
| `ashenvale-edge-strips` | Fly over the Barrens at the Ashenvale border, facing a sun at 29 degrees over the ridges. No long strip of shade may cross the slope that faces away from the sun (2026-10-04). |
| `stormwind-canal-fog` | Stand on the canal's bank outside Stormwind's gate. The fog over the water must let the far side show (2026-10-04). |
| `ironforge-gate-fog` | Stand on the road outside Ironforge's gate, under the mountain. The fog must let the road and the trees show (2026-10-04). |
| `stormwind-tower-speckle` | Stand on the bridge into Stormwind facing the sun over the gate. The left tower, where the sun grazes it, must show no speckled shade (2026-10-04). |
| `stormwind-canal-wake` | Swim through a canal inside Stormwind, the camera looking down. Ripples and a wake must trail the swimmer, on a building's water as on the map's (2026-10-04). |
| `elwynn-trough-shadow` | Stand in the Eastvale water trough, the camera zoomed in. The trough must not be marked a body, so its shadow has no character's extra darkness (2026-10-04). |
| `development-horse-seethrough` | On the development map with a summoned Warhorse and Charger beside you. No horse may be taken as see-through, and the Warhorse must stay a body in every frame (2026-10-04). |
| `development-windmill` | Stand 17 yards from a Westfall windmill in its blades' shade, then 200 yards out, the camera turned round, and back. All its parts must be in the far map, no older pose of it kept, its tower solid and its sails leaves (2026-10-04). |
