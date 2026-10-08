# Plan: the time of day in comfyatmos.dll, and keys players can turn off

Branch `time`, cut from `main` at b9267e5 on 2026-10-07. Worktree: `comfy-wow\.claude\worktrees\atmos-time`.
This file is removed at the merge. What it found goes into NOTES.md and the changelog then.

Four stages, each one commit or more, each tested in `octow - Copy` before the next starts.

| Stage | What | State |
| --- | --- | --- |
| 1 | comfytime moves into comfyatmos.dll, as it is | built, not tested in the client |
| 2 | A time slider in the debug panel | built, not tested in the client |
| 3 | Key bindings: the DLL's own keys off by default, every action in Key Bindings | built, not tested in the client |
| 4 | Release v0.12.0-alpha, retire comfytime.dll in the launcher | to do |

## Stage 1: comfytime inside comfyatmos.dll

Move the code with no change in behaviour. The keys stay the same.

### What moves where

| comfytime | comfyatmosphere |
| --- | --- |
| `src/timeofday.cpp`, `timeofday.h` | `src/timeofday.cpp`, `timeofday.h`. Both use `Log` and `Now` from `common.h`, which comfyatmos has with the same signatures. |
| `TimeSettings` in `config.h` | `TimeSettings time;` in `Settings`, read from `[time]` in `config.cpp` with the same keys and defaults |
| `comfytime.ini` `[time]` | `[time]` in `comfyatmos.ini`, comments kept |
| `saveKey`, `dayNightKey`, `scanKey` | `[general]`, beside `reloadKey` and `probeKey` |
| `PollKeys` in `comfytime.cpp` | `PollKeys` in `comfyatmos.cpp` |
| comfytime's README | a NOTES.md section, "The time of day (from comfytime, 2026-10-07)" |

Not carried over:

- `log`, `hook`, `reloadKey`: comfyatmos has its own.
- `chainWaitMs` and `WaitForSiblings`: there is no second DLL to wait for.
- `WatchIni` (comfytime's unreleased 80bba1b). The test runner sends `/atmos time.hour <h>` instead (below).
- The attach code: comfyatmos already hooks `Present` and `BeginScene`.

### Order inside the hooks

comfytime chains on top of comfyatmos today, so its hooks run first. The new code keeps that order:

- `TimeApply("BeginScene")` is the first line of comfyatmos's `hkBeginScene`.
- `TimeApply("Present")` and `TimeScanTick()` are the first lines of `hkPresent`.

`sun.cpp` and the night code read the clock at `[client] clockAddr`. They must read the hour that was just
written, not the server's.

### Keys

| Key | Before | After stage 1 |
| --- | --- | --- |
| F11 | comfyatmos and comfytime each reload their ini | one reload, which also calls `TimeReload` |
| Ctrl+F12 | comfytime's clock search; comfyatmos skips it | the clock search, in comfyatmos |
| Ctrl+PageUp, Ctrl+PageDown, Ctrl+End | comfytime | the same keys, in comfyatmos |
| Ctrl+Home | writes `hour` into `comfytime.ini` | writes `[time] hour` into `comfyatmos.ini` |

### /atmos

- The `[time]` keys reach `/atmos` through `ConfigKeys` with no more work.
- A `/atmos time.*` change must call `TimeReload`, as F11 does. Find where tune applies a value
  (`CVarsAfterLoad`) and call it there.
- The probe's client report gets one time line: the hour shown, and whether the addresses passed.

### An old comfytime.dll still in dlls.txt

Two writers would fight over the same three addresses. Do what was done for comfygrass:

- If `comfytime.dll` is loaded, the time code in comfyatmos stays off this run.
- Log one line that names the fix: take `comfytime.dll` out of `dlls.txt`, then delete it and
  `dlls.txt.cache`.

### The test runner

- `tests/Run-Tests.ps1` lines 386 to 397 write `comfytime.ini`. Replace them with two `/atmos` lines
  in the test's script: `time.enabled 1` and `time.hour <h>`. The `/atmos reset` that each test
  already runs puts them back.
- `tests/Snapshot.ps1` says "comfytime sets" in two comments. Change them.

### Check

- `Logs\comfyatmos.log`: `time: addresses check out`, and no `comfytime` line.
- Ctrl+PageUp moves the sun. Ctrl+End goes to night and back. F11 keeps the hour.
- All tests pass, `stormwind-lighthouse-light` among them (it runs at night).
- With the old comfytime.dll also in `dlls.txt`: the log line, and the time still moves (by the old DLL).

## Stage 2: a time slider in the debug panel

Only in the debug panel (`/atmos debug`). The Video > Atmosphere page does not get it.

### Controls

Under the Debug view row:

- **Time of day**: a slider from 0 to 24 hours, with the time as HH:MM beside it.
- `<` and `>` buttons: one `step` earlier or later.
- **Day/Night**: does what Ctrl+End does.
- **Lock time** (the owner, 2026-10-07, in place of Save hour and a Game time box): ticked, the hour stays where
  it is put and is kept in the ini; unticked, the server's time shows. Ctrl+Home and `saveKey` are gone.

### How the slider talks to the DLL

Use a CVar, `comfyTimeHour`, registered by the DLL as `comfyDebugView` is. Not `/atmos`, for two reasons:

- The slider must show the hour that the keys moved. The DLL writes the CVar back when Ctrl+PageUp moves
  the time, and the panel reads it.
- A `/atmos` command is a round trip with a queue in the addon. A dragged slider sends one per frame.

The DLL reads the CVar once a frame. A value that differs from the last one it wrote becomes `g_hour`.

## Stage 3: keys players can turn off

### The problem

The DLL reads its keys with `GetAsyncKeyState`. A player cannot unbind them, and they fire whatever the
game has bound to the same key. Today that is ten keys:

| Key | Action |
| --- | --- |
| F11 | reload `comfyatmos.ini` |
| Ctrl+F11 | sun rays on or off |
| Alt+F11 | volumetric light on or off |
| F12 | probe (one frame into the log) |
| Alt+F12 | benchmark |
| Ctrl+F12 | clock search (six minutes, writes only the log) |
| Ctrl+PageUp, Ctrl+PageDown | time later, earlier |
| Ctrl+End | day or night |
| Ctrl+Home | save the hour |

### The change

1. **A setting, `[general] hotkeys = 0`.** At 0 the DLL reads none of the keys above. The owner's test
   client sets it to 1.
2. **A check box in the debug panel, "Developer keys",** that sets it.
3. **Every action in Key Bindings > ComfyAtmosphere** (`Bindings.xml`), with no key bound by default.
   Probe, Stats and the debug panel are there already. Add:
   - Reload settings
   - Sun rays on or off
   - Volumetric light on or off
   - Benchmark
   - Time later, Time earlier
   - Day or night
   - Save the hour

   The clock search gets a button in the debug panel and no binding.
4. **New `/atmos` commands** for the bindings to call: `reload`, `rays`, `volume`, `daynight`, `savehour`.
   `probe` and `bench` exist already.
5. **Time later and Time earlier move the `comfyTimeHour` CVar from stage 2**, not an `/atmos` command.
   While the key is held, the addon repeats the step in an OnUpdate (`runOnUp="true"`, `keystate`), with the
   same wait and rate as the DLL: 0.35 s, then one step every 0.03 s.

### Checks before building

- The test runner sends `/atmos probe`, not F12. Confirm that no test or tool depends on a raw key.
- `tools/wow-test-tool` (comfy-wow) presses keys with `Robot.psm1`. Search it for F11, F12 and PageUp.

## Stage 4: release, and retire comfytime

In this order:

1. **comfyatmosphere**: merge `time` into `main`, release v0.12.0-alpha. The ini adds `[time]` and four keys
   in `[general]`. Fold this plan into NOTES.md and `changelog/`, then delete this file.
2. **comfytime**: a README line, "Part of comfyatmosphere since v0.12.0-alpha". 80bba1b is not released.
3. **launcher**:
   - Remove the "Time of day" row from `lists/mods.json`.
   - Add `comfytime.dll` and `comfytime.ini` to comfyatmosphere's `supersedes_files`.
   - `Pin-Mod.ps1 comfyatmosphere v0.12.0-alpha`, then `Build-Manifest.ps1`, then a deploy.
4. **comfy-wow**: the comfytime row in `mods/README.md` says it is part of comfyatmosphere, as the comfygrass row does.

## Open questions for the owner

1. **A player's comfytime.ini settings.** Copy `hour`, `dayHour`, `nightHour`, `step` and `enabled` into
   `[time]` at the first start, or start from the defaults? A copy needs code that runs once and a marker.

Decided on 2026-10-07:

- All ten keys are off by default, the time keys among them (the owner: "disable keybinds by default and have
  checkbox in debug to turn them on").
- "Developer keys" is the control CVar `comfyHotkeys`, so the client keeps it in Config.wtf. No saved variable.
- The debug panel's box is "Set the time of day" (ticked: ours), not "Game time".
