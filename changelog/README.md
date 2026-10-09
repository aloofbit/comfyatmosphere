# Changelog

One file for each big change, named `date-short-description.md`. A file says what changed, why, and which
controls it adds. The reasoning and the measurements are in `NOTES.md`, one section for each problem.

## 2026-10-08

- [Your own stealth shadow, and the shade on grass and ferns](2026-10-08-stealth-and-foliage-shade.md): your own
  character in stealth casts no shadow, and the ground's grass and ferns are dark under cover or lit as a whole,
  with four sliders on the Shadows page.
- [Wet sand along a river](2026-10-08-wet-sand-rivers.md): the wet sand follows a river's waterline, no hard dark
  line far off, and Wet Sand defaults to 20.
- [No pale dashes or white dots on the sea](2026-10-08-sea-specks.md): no gaps along the horizon, and no white dots
  on the water as the camera moves.
- [The fog at sunset](2026-10-08-fog-at-sunset.md): Fog Toward the Sun, Fog Horizon Glow, a glow round the sun, and
  the fog over water as thick as over land.
- [Test tools](2026-10-08-test-tools.md): a cameraZoom step, Before-After.ps1 for the release notes, and the tests
  `sunset` and `redridge-grass-shadow`.

## 2026-10-07

- [The zone's sky and water colours](2026-10-07-zone-colours.md): the water reflects the game's own sky and its
  glow at dusk, and Zone Colour gives the water the game's own colour for the zone and the hour.
- [A windmill's frozen copy from login](2026-10-07-windmill-at-login.md): draws made before a model was known no
  longer stay in the shadow cache; development-windmill no longer fails now and then.
- [The time of day, and key bindings](2026-10-07-time-of-day.md): comfytime is part of comfyatmos.dll, a time
  slider and Lock time in the debug panel, every action in Key Bindings, the DLL's own keys off, and a probe
  copies its lines to the clipboard.
- [Test tools](2026-10-07-test-tools.md): a test can set the time of day (`hour`), Snapshot records it, and no
  failed shader compile at each start.
- [Creatures under the sea](2026-10-07-creatures-under-water.md): creatures under the water keep their texture and
  show dim, not flat and pale; the fog ends at the water's surface.
- [Lighthouses](2026-10-07-lighthouse-distance.md): the game's own light is left out again, a Lighthouse Distance
  slider, and the beam's light on the water ends where the beam ends.
- [Water fixes](2026-10-07-water-fixes.md): the surface no longer splits where a lake meets the sea, the wet sand
  follows steep banks, a Wet Sand slider, and a swimmer's camera stays out of the waves.
- [Inside a building, on the stats panel](2026-10-07-indoor-stats.md): the indoor test's answer, and the step that
  decided it.

## 2026-10-06

- [The log in the Logs folder](2026-10-06-logs-folder.md): `Logs\comfyatmos.log`, the shader cache in
  `WDB\comfyatmos`, and the benchmark's results in the chat.
- [Rain Darkness](2026-10-06-rain-darkness.md): a rainy night is no longer brighter than a clear one.
- [Ship wake](2026-10-06-ship-wake.md): a ship under way leaves a wake. Off by default.
- [A ridge under the sea](2026-10-06-ridge-under-the-sea.md): far terrain under the water no longer shows as a dark
  shape with a hard edge. New debug views for what lies under the water.
- [Shadows on and under the water](2026-10-06-shadows-on-water.md): Shadow on Water, and the shade on the sea floor
  stays in view as long as the floor does.
- [A ship's shadow left behind](2026-10-06-ship-shadow-cache.md): a ship's sails and rigging move with the ship in
  the shadow cache.
- [No freeze at sign-in](2026-10-06-shader-cache.md): shaders compile in the background and are kept on disk.
- [Test tools](2026-10-06-test-tools.md): the login no longer hangs, and a test can fly over deep water.
- [Performance tests](2026-10-06-performance-tests.md): load-performance and frame-rate-performance, and the
  Performance panel at the top of the test page.
- [Faster water and shadows](2026-10-06-faster-water-and-shadows.md): the harbour from 73 to 84 fps, timers for each
  pass in the frame log, and the water-cost test.
- [Grass in the wind](2026-10-06-grass.md): comfygrass is part of comfyfog.dll, with a Grass tab of controls.
- [New file names](2026-10-06-rename.md): `comfyatmos.dll`, `comfyatmos.ini` and `comfyatmos.log` in place of
  `comfyfog.*`.
