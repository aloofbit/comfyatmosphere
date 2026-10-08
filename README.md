# Comfy Atmosphere

> **Bugs, questions and screenshots. Ty for testing!: [join our Discord](https://discord.gg/uhefX2efB7).**
>
> [![Discord](https://img.shields.io/badge/Discord-ComfyCraft-5865F2?logo=discord&logoColor=white&style=for-the-badge)](https://discord.gg/uhefX2efB7)


Atmosphere mod the World of Warcraft 1.12 client. Volumetric lighting, fog, shadows, sun rays, water, and more.

https://github.com/user-attachments/assets/67551de4-3ee5-4ea1-83ed-b72592bfce1d

## Testing and Benchmarks

When bugs are reported we implement automation testing to reduce regression.

[![Test results](media/test-badge.svg)](https://aloofbit.github.io/comfyatmosphere/latest/)

## Roadmap

*Now*: Testing and implementing repairs until each effect is cohesive with the game. Thank you for help testing!

![Roadmap: 1 Feasibility Study, done. 2 Prototype Effects, done. 3 Tests and Repairs, now. 4 Performance, next. 5 Maintenance, later.](media/roadmap.svg)

## Features
- Volumetric Light
- Realtime shadows
- Sun rays
- Fog
- Lamps (From 16 to 256)
- Night Config
- Moving Grass
- Time of day, on your screen only
- Settings Addon for it all

[![Lamps along a Duskwood road at night, the effects off and then on. Click for the full video.](media/lamps-night.gif)](media/lamps-night.mp4)

*Lamps and night in Duskwood: the effects off, then on.*

[![A harbour walkway with lanterns and sun shadows. Click for the full video.](media/harbour-shadows.gif)](media/harbour-shadows.mp4)

*Sun shadows and lanterns on a harbour walkway.*

## Compatibility
> ⚠ Please start here if having problems. Start with a fresh client, install DXVK 2.7.1, then comfyatmos.

Tested on a fresh OctoWoW client with no other mods:

| | Tested with |
| --- | --- |
| Client | `WoW.exe` 1.12.1 (build 5875), 4812 KB, hash `c1d1205e0a984ca4` |
| Launcher | VanillaFixes, with only `comfyatmos.dll` in `dlls.txt` |
| DXVK | v2.7.1-1-gplasync, the `d3d9.dll` that comes with the client |
| Data | 18 MPQs: the base archives, `patch.MPQ` and `patch-1` to `patch-5` |
| Addons | ComfyAtmosphere and the client's Blizzard addons |
| System | Windows 10 (build 19045), NVIDIA GeForce RTX 2080 Super, driver 576.28 |

`/atmos probe` (or F12) writes a report of your client into `Logs\comfyatmos.log`, under `client report`: the same
parts, with a hash of each file and every DLL loaded. Compare it with this table.

## Install

Download the zip from [Releases](https://github.com/aloofbit/comfyatmosphere/releases), or build it (below).

1. Copy `comfyatmos.dll` and `comfyatmos.ini` to the client folder.
2. Add the line `comfyatmos.dll` to `dlls.txt`. ⚠️remove comfyfog/comfygrass/comfytime if you have them
3. Copy the folder `addon/ComfyAtmosphere` to `Interface\AddOns`.
4. Start the game with `VanillaFixes.exe`.

## Options

Use `/atmos options` in game to enable/disable and tune features of the mod.

<img src="media/settings-example.png" width="200"/>


## Chat commands `/atmos`

| Command | |
| --- | --- |
| `/atmos` | The commands and the sections |
| `/atmos options` | Open or close the settings window |
| `/atmos debug` | Open or close the debug panel (below) |
| `/atmos stats` | Show or hide the stats panel (below) |
| `/atmos probe` | Log one frame to `Logs\comfyatmos.log`, as F12 does |
| `/atmos bench` | Run the benchmark, as Alt+F12 does |
| `/atmos reload` | Read `comfyatmos.ini` again, as F11 does |
| `/atmos rays`, `/atmos volume` | The sun rays or the volumetric light on or off, as Ctrl+F11 and Alt+F11 do |
| `/atmos framelog [seconds]` | Time every frame for that long (10), then log the slowest frames and our share of each |
| `/atmos <section>` | Every value in a section |
| `/atmos <section>.<key>` | One value, and where it came from |
| `/atmos <section>.<key> <value>` | Set it. The key alone will do when no other section has it |
| `/atmos list` | The values set with `/atmos` |
| `/atmos reset` | Drop them. `comfyatmos.ini` applies again |
| `/atmos save` | Write them into `comfyatmos.ini`. The comment on each line stays |

A value set with `/atmos` stays until `reset` or `save`, also after F11. A value that a control on the
Atmosphere page sets is refused: use the control.

### Debugging Help

The **Debug** button in the settings window, or `/atmos debug`, opens the debug panel. It also sets the time of
day (a slider, a step either way, Day/Night and Save hour), and has the Developer keys box:

![debug menu](media/debug-menu.png)

## Keys

Key Bindings > ComfyAtmosphere has every action: the probe, the stats, the debug panel, reading `comfyatmos.ini`
again, the sun rays and the volumetric light on or off, the benchmark, the time of day later and earlier, day or
night, and saving the hour. No key is bound at first.

The keys below are for development. They are read past the game's key bindings, so they are off. Tick
**Developer keys** in the debug panel to turn them on.

| Key | |
| --- | --- |
| F11 | Reload `comfyatmos.ini` |
| Ctrl+F11 | Sun rays on / off |
| Alt+F11 | Volumetric light on / off |
| F12 | Log one frame of diagnostics to `Logs\comfyatmos.log` |
| Alt+F12 | Run the benchmark (below) |
| Ctrl+PageUp / PageDown | The time of day later / earlier by `[time] step` hours; hold to keep moving |
| Ctrl+End | Switch between day (`[time] dayHour`) and night (`[time] nightHour`) |
| Ctrl+Home | Save the time being shown into `[time] hour` in `comfyatmos.ini` |
| Ctrl+F12 | Search memory for the game clock (read-only; for a different `WoW.exe`, see NOTES) |

## Benchmark

Alt+F12, or `/atmos bench`, measures what each feature costs on your computer. It takes 40 seconds. Writes results to the chat and to `Logs\comfyatmos.log`

## Build

Visual Studio 2022 and CMake. **32-bit only**: the 1.12 client is x86.

```
cmake -B build -A Win32
cmake --build build --config Release
```

`comfyatmos.dll` is written to the project root, next to `comfyatmos.ini`.

## Licence

GPL-3.0. See [LICENSE](LICENSE).
