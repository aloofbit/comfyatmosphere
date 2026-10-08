# The time of day, and key bindings

## What changed

- comfyatmos.dll sets the time of day. It was the separate mod comfytime until now. The time shows on your screen
  only: the server keeps its own, and other players see no change.
- The debug panel (`/atmos debug`) sets the time: a slider, a step either way, Day/Night, and **Lock time**.
  Ticked, the hour stays where you put it, also after a restart. Unticked, the server's time shows.
- Every action is in Key Bindings > ComfyAtmosphere, with no key bound at first.
- The DLL's own keys (F11, F12, Ctrl+PageUp and the rest) are off. They are read past the game's key bindings,
  so a player could not unbind them. The debug panel's **Developer keys** box turns them on.
- A probe (F12, `/atmos probe`, the Probe button) also copies its log lines to the clipboard.

## Install

- Remove the line `comfytime.dll` from `dlls.txt`, then delete `comfytime.dll` and `dlls.txt.cache`. While
  comfytime.dll is loaded, it sets the time and the `[time]` section does nothing.
- `comfytime.ini` is not read. Its values go in the `[time]` section of `comfyatmos.ini`, with the same names.

## Key bindings

| Binding | Before, as a key |
| --- | --- |
| Probe (log one frame) | F12 |
| Show or hide the stats | |
| Show or hide the debug panel | |
| Read comfyatmos.ini again | F11 |
| Sun rays on or off | Ctrl+F11 |
| Volumetric light on or off | Alt+F11 |
| Run the benchmark | Alt+F12 |
| Time of day later, earlier (held, they repeat) | Ctrl+PageUp, Ctrl+PageDown |
| Day or night | Ctrl+End |

Ctrl+F12, the search for the game clock in another `WoW.exe`, has no binding. comfytime's Ctrl+Home (save the
hour) is gone: Lock time keeps the hour.

## Settings

| Setting in `comfyatmos.ini` | |
| --- | --- |
| `[time] enabled` | 0: the server's time. Lock time sets it |
| `[time] hour` | The hour shown, 0..24. While the time is locked, an hour you move is written here a second later |
| `[time] step` | Hours per step |
| `[time] dayHour`, `nightHour` | The two hours Day/Night switches between |
| `[time] addrMinutes`, `addrFraction`, `addrMinutesF` | Where this `WoW.exe` keeps the time |
| `[general] hotkeys` | 1: the DLL's own keys work. The Developer keys box sets it |
| `[general] scanKey`, `dayNightKey` | comfytime's keys, with Ctrl |

New `/atmos` commands: `reload`, `rays`, `volume`.

## Test runner

A test's `hour` is set with `/atmos time.hour`, not written into `comfytime.ini`.
