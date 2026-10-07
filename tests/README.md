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
   .\Run-Tests.ps1 -Accept                # and take this run's screenshots and recordings as the expected ones
   ```

The runner restarts the test client and logs in once, unless `-NoLogin` is given, and runs every test in that one session. It logs in again only for a test that wants another character, and casts flight when a test's `flight` differs from the state it is in. A run ends with flight off and the camera behind the character. It sends `/atmos reset` before and after each test,
and puts every CVar it set back as it was, so a test leaves no values behind. It clears the weather at each
test's start (`.wchange 0 0`), and turns GM mode on first (`.gm on`), so a hostile creature leaves the character alone. With comfytest.dll in the client (wow-test-tool's `dll\`) a run needs no mouse or
keyboard, and the game may stay behind other windows; without it, do not touch the mouse or keyboard while a test
runs. The result, the script it ran, the probe and the screenshots go to `results\`, which git ignores.

## The page

Each test's result is also saved on its own, as `results\<time>-<test>.record.json`. `.\Build-Latest.ps1` makes one page
of every test's latest result, whichever run it came from, with the time of that run beside each test:
`results\latest-report.html`, also copied to `results\last-results.html`. `-Name` takes only some tests.
`-Card` also writes the README's results card, `..\media\test-results.svg` (`Write-Card.ps1`): the frame rate at each
place with the effects on and off, the sign-in, and each test's latest result. It follows the reader's light or dark
theme. It is written only when every test is taken. A test
last run before the records were saved is read back from its text report, `results\<time>-<test>.txt`.

Each run writes `results\<time>-report.html` and opens it (`-NoOpen` does not). The same page is copied to `results\last-results.html`: keep a tab open on it and refresh it after each run. It shows every test, its checks
and what they measured, and each screenshot beside the expected one, with the Debug View it was taken in. A
check that reads a screenshot sits with that screenshot. The expected shots are in `expected\<test>-<n>.jpg`,
in git. Each recording plays beside its expected one, `expected\<test>-rec<n>.mp4`. When a change is meant to
alter a test's look, look at the page, then run again with `-Accept` to take the new shots and recordings as
expected.

At the top of every page is the **Performance** panel (2026-10-06). It shows each number the performance tests
record, grouped by test: the latest value, its limit, a mark for pass or fail, the change from the run before, and
the last 20 runs as a small graph with the limit as a dashed line. It reads `results\perf-history.json`, which
each run adds to, so it shows on every page, also when a run did not include the performance tests. A value
with no limit is recorded only and shows in a neutral colour.

## A test

A test is a `.json` file here:

| Part | What it holds |
| --- | --- |
| `config` | `character` (the slot on character select), `flight` (cast "Toggle GM Flight Mode"), `camera` (0 first person, 1 to 9 that many notches of the mouse wheel back out, 10 all the way out; 1 notch is 2.5 yards, 2 is 3.4, 4 is 5.3), `cameraZoom` (the camera's own distance, the field the mouse wheel moves, set exactly by comfytest.dll; a snapshot records it), `flySpeed` (yards a second), `start` (`map`, `x`, `y`, `z`, by `.go xyz`; `facing`, the character's heading in degrees, counter-clockwise from +x), `sun` (`azimuth`, `elevation`: a fixed sun), `ini` (`"section.key": value` pairs, by `/atmos`), `debugView` (the Debug View number), `cvars` (`"name": value` pairs for the Atmosphere page's controls, which `/atmos` refuses), `summon` (`entry`, `name`, `x`, `y`, `z`: a creature summoned there before the start and deleted by its name after the steps; only a creature with no world spawn, since `.npc delete` removes a world spawn from the database), `flightFrom` (`x`, `y`, `z`: with flight, where it is turned on, on land, before the start; for a start in the air over deep water), `restart` (true: the client is restarted and signed in before the test, at the start, and the log checks read the whole log since the client started) |
| `steps` | one key each: `wait` seconds, `down` and `up` (hold a key such as `W` over the steps between), `jump` times, `hop` (`.go xyz` along a line), `fly` yards, `back` yards, `turn` degrees to the left (by `face`), `face` (`heading`, `pitch`: the camera, in degrees, checked against comfyatmos.dll's stats; `leftDrag` tilts it by left-drags, which leave a swimmer level), `probe`, `screenshot`, `record` (`seconds`, `label`: a video without the UI, shown on the page and played at once; by comfytest.dll when the client has it, so the game may be covered), `atmos`, `cvar`, `chat`, `type` (a slash command typed into the chat box), `target` (the unit with that exact name, as `/target`), `clearTarget`, `camera` (the camera's distance partway through, as `config.camera`), `go` (`x`, `y`, `z`, `facing`, `map`: to another place within the test, waiting until the character is there) |
| `expect` | checks against the last probe (or the one `probeAt` names, from 1): `probe` is a pattern for its lines, `max` and `min` the count allowed. Or against a screenshot: `shot` (which, from 1), `row`, `from`, `to` (columns), and `min` (the darkest pixel allowed), `mean` (the least average) or `meanMax` (the most average, for a row that must stay in shade) along that row. Or `shot` and `box` (`[left, top, right, bottom]`) with `jumpMax`, the percent of pixels allowed that differ sharply from the next one (speckle), or `warmMax`, the percent of pixels allowed with more red than green (ground seen through a gap in the sea's surface). Or against comfyatmos.log: `log` is a pattern with one number in brackets, `at` which match (from 1; the last when left out), `max` and `min` the limits (neither: recorded only), `metric` a name that puts the number into `results\perf-history.json` for the Performance panel, and `unit` |

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
| `stormwind-harbour-ridge` | First person on Stormwind's dock, toward the lighthouse. A ridge under the sea showed as a dark shape with a hard edge against the open sea: the sun shadows' Sunlight and tint faded with the bed over the ridge. Shots in the normal view and in Debug View 28 (2026-10-06). |
| `stormwind-harbour-ship-shadow` | Flying over the harbour, looking down at a moored ship. Its shadow and the dock's must show on the sea floor and softly on the water. Shots in the normal view and Debug Views 5, 24 and 28; flight goes on at the dock (`flightFrom`) (2026-10-06). |
| `load-performance` | Restarts the client at the harbour and reads the first 90 s in the world: the compiling left on the game's thread (at most 100 ms), the worst frame once the effects start (at most 1,500 ms) and the frames over 40 ms from then (at most 12). Before the shader cache, 11.1 s of compiling froze two frames for 5.2 and 7.0 s (2026-10-06). |
| `water-cost` | Frame times at the harbour, 10 s each, with every effect on between the others: without the water's sand part, its water part, its bodies' draw (`[water] debugSkip`), all three, and the water. The frame log gives each one's GPU time; the readings drift as the GPU warms, so compare each with the runs beside it. It needs the game window in front. Records nothing (2026-10-06). |
| `volume-cost` | Frame times at the harbour and on Elwynn's ridges, 10 s each, with the Volumetric Light box on, off, on, off, on, and every other control as frame-rate-performance sets it: what the light costs. The frame log gives each pass's GPU and CPU time and the shadow pipeline's CPU part by part. It needs the game window in front. Records nothing (2026-10-07). |
| `volume-parts` | The same two places, 10 s each, with every effect on between the others: without the sun shadows' pass, the march's reads of the maps, drawing the maps, the cache's upkeep (`[shadow] debugSkip` 1, 2, 4, 8), and without the light. It needs the game window in front. Records nothing (2026-10-07). |
| `frame-rate-performance` | Frame times over 10 s at the harbour, Elwynn's ridges and the road outside Ironforge, with the owner's controls and a fixed sun, and again with every effect off, to show what the effects cost: at least 50 fps and the slowest 1% at most 40 ms at each; our CPU time a frame is recorded. **It needs the game window in front:** behind other windows the game holds 60 fps, effects on or off. So it brings the window to the front for each 10-second log (`gameFront`, `gameBack`) and hands the focus back after; do not type in another window while it runs (2026-10-06). |
| `ironforge-ah-glow` | On the road outside Ironforge's gate, facing the statue in the hall. The auction house braziers behind it took the fog's cap (x54.6) as buried under the mountain, and their glow came through the hall's walls. After 45 s for the city to load, no lamp may take the cap, and the braziers must be in the probe's list. Shots in the normal view and in Debug View 6 (2026-10-05). |
| `stormwind-tower-speckle` | Stand on the bridge into Stormwind facing the sun over the gate. The left tower, where the sun grazes it, must show no speckled shade (2026-10-04). |
| `stormwind-canal-wake` | Swim through a canal inside Stormwind, the camera looking down. Ripples and a wake must trail the swimmer, on a building's water as on the map's (2026-10-04). |
| `water-standing` | From the owner's snapshot at the edge of Redridge's lake: a shot there, then 5 yards forward into the shallows and a shot standing still. No foam may lie round the character: the foam round objects took it for a post. Each shot also in Debug View 17 (2026-10-06). |
| `redridge-grass` | From the owner's snapshot in Redridge's grass, with Foliage Density at 128 (put back at the end). The probe must show the grass drawn by comfyatmos.dll and the fill loop patched. Recordings with the grass on, off and while walking through it; a shot in Debug View 30, the bend (2026-10-06). |
| `elwynn-trough-shadow` | Stand in the Eastvale water trough, the camera zoomed in. The trough must not be marked a body, so its shadow has no character's extra darkness (2026-10-04). |
| `development-horse-seethrough` | On the development map with a summoned Warhorse and Charger beside you. No horse may be taken as see-through, and the Warhorse must stay a body in every frame (2026-10-04). |
| `development-guard-stealth` | On the development map, the owner's Stormwind City Guard given Stealth. A recording as Stealth goes on, a shot in stealth, a recording as it goes off, then a shot with the character inside him. His draws, his helmet, shoulders and weapons among them, must go into the scratch depth, and none of them may cast a shadow (2026-10-05). |
| `development-saturation` | On the development map, the Day Saturation and Night Saturation sliders. Five shots: the game's own colour, Day Saturation 0 (grey), 200, Night Saturation 0 by day, which must change nothing, and Day Saturation 0 with the Color Effects box off. The probe must show the pass at 0 and at 2, and not at 1 or with the box off (2026-10-05). |
| `elwynn-own-face` | At the Eastvale paddock with the camera 1.8 yards behind the character, where the game fades it. Nothing may be taken as a stealthed unit, so the face never shows through the head (2026-10-04). |
| `development-windmill` | Stand 17 yards from a Westfall windmill in its blades' shade, then 200 yards out, the camera turned round, and back. All its parts must be in the far map, no older pose of it kept, its tower solid and its sails leaves (2026-10-04). Beside it, before and after, a 5 s recording in the normal view and one in Debug View 5; from 90 yards, two shots (2026-10-05). |
| `westfall-beach-swash` | Fly 40 yards over a Westfall beach, looking down at the shore. 10 second recordings at the swash's defaults, then with Swash Height 100 (no higher than the wet sand), Length 10 and Speed 300, to compare by eye (2026-10-05). |
| `westfall-far-waterline` | Fly 67 yards over the same beach, the shore about 90 yards away. Our water's edge must follow the ground, not step, beside a shot of the game's own water (2026-10-05). |
| `darkshore-water-split` | First person on the shore by Auberdine. A chunk the map counts as a lake met the sea's chunks, and only the sea's had the swell: the surface split along their edge and the sea floor showed through. No ground may show in the open water with Wave Height 8; shot 2 has Wave Height 0 (2026-10-07). |
