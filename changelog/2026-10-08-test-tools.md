# Test tools

**Commits:** `6d8e3f6`, `25db792`, `7e0d60b`, `57724c2`, `e9d89f8`, `33f9cb3` (comfyatmosphere), `18f246a`
(comfy-wow)

## The tools from a worktree

`tests/Run-Tests.ps1` and `tests/Snapshot.ps1` took wow-test-tool three folders up. That is comfy-wow from
`mods\comfyatmosphere`, but not from a worktree under `comfy-wow\.claude\worktrees`. They now take the first
`tools\wow-test-tool` in a folder over the tests.

## The window's size

The game draws at its window's size, not at `gxResolution`. With 1152x864 in `Config.wtf`, the window came out at
1280 x 720, and `darkshore-water-split` and `telabim-wet-sand` failed: their checks read pixel rows of shots taken
at 1152 x 864.

- `Login.ps1` sets the client area to 1280 x 800 when it starts the client (`-Width`, `-Height`).
- `Run-Tests.ps1` sets it before each test to the size of the test's first expected shot, or to `config.window`
  (`"1152x864"`). A test with no expected shot runs at 1280 x 800.
- wow-test-tool finds the test client by the process at the server end of comfytest.dll's pipe, or by the id
  `Login.ps1` writes into `Imports\comfytest-pid.txt` when it starts the client. It took the newest WoW.exe
  before. With the live client started after the test client, `Login.ps1 -Restart` would have closed the live one.

Both tests pass again on 0.12.0.

## A test's hour is 12:00 when it gives none

`config.sun` turns only the shadows' sun. The game lit the world by the clock, so a run at 22:26 came out at night
beside expected shots taken by day. A test with no `hour` now runs at 12:00.

## A cameraZoom step

A test can set the camera's own distance partway through, as `cameraZoom` sets it at the start.

## Before and after pictures

`tools\wow-test-tool\Before-After.ps1` in comfy-wow puts two screenshots side by side, as the release notes show
them: `Before-After.ps1 <before> <after>`, with `-BeforeVersion` and `-AfterVersion` for the labels, `-Out` for the
file and `-Clipboard` to copy it. Each shot is at half size, with a 6 pixel divider.

Made with it for the v0.12.0 release notes, in `media\`:

- `v0.12.0-sunset.jpg` and `v0.12.0-sunset-booty-bay.jpg`: v0.11.3-alpha with comfytime at the same hour, against
  0.12.
- `v0.12.0-stealth-shadow.jpg`: your own character in stealth, with its shadow on v0.11.3-alpha and without on 0.12.

## New tests and parts

- `sunset`: the sun setting over the sea off Tirisfal Glades at 20:30, and at the mouth of Booty Bay at 20:25, with
  the owner's settings. One shot at each place; no check reads them yet. It was `tirisfal-sunset`.
- `telabim-wet-sand`: the river in Old Hillsbrad as a second place.
- `development-guard-stealth`: your own character in stealth, out of the building's shade. The probe lists it as
  stealthed, and in the shade alone the row across its shadow reads 7 before stealth and 255 in it.
- `redridge-grass-shadow`: the grass and ferns in Redridge at Shadow Resolution 2048. A 5 second recording and two
  shots; no check reads them yet.

## Left undone

- White specks along a river bank.
- A before and after of the grass and ferns against v0.11.3-alpha.
