# Comfy Atmosphere

> **Bugs, questions and screenshots. Ty for testing!: [join our Discord](https://discord.gg/zvSKGrKsz).**
>
> [![Discord](https://img.shields.io/badge/Discord-ComfyCraft-5865F2?logo=discord&logoColor=white&style=for-the-badge)](https://discord.gg/zvSKGrKsz)


Atmosphere mod the World of Warcraft 1.12 client. Volumetric lighting, fog, shadows, sun rays, water, and more.

https://github.com/user-attachments/assets/67551de4-3ee5-4ea1-83ed-b72592bfce1d

## Status / Tests
Latest test results: https://aloofbit.github.io/comfyatmosphere/runs/20261006-160606/

## Roadmap

| | Stage | Status |
| --- | --- | --- |
| 1 | Prototypes of the lighting mod and the grass mod | ✅ done |
| 2 | Prototypes of the other features | ✅ done |
| 3 | Tests and repairs until each effect looks correct | 🔧 **now** |
| 4 | Performance | ⬜ next |
| 5 | Maintenance | ⬜ later |

## Features
- Volumetric Light
- Realtime shadows
- Sun rays
- Fog
- Lamps (From 16 to 256)
- Night Config
- Moving Grass
- Settings Addon for it all

[![Lamps along a Duskwood road at night, the effects off and then on. Click for the full video.](media/lamps-night.gif)](media/lamps-night.mp4)

*Lamps and night in Duskwood: the effects off, then on.*

[![A harbour walkway with lanterns and sun shadows. Click for the full video.](media/harbour-shadows.gif)](media/harbour-shadows.mp4)

*Sun shadows and lanterns on a harbour walkway.*

## Compatibility

Tested on a fresh OctoWoW client with no other mods:

| | Tested with |
| --- | --- |
| Client | `WoW.exe` 1.12.1 (build 5875), 4812 KB, hash `c1d1205e0a984ca4` |
| Launcher | VanillaFixes, with only `comfyatmos.dll` in `dlls.txt` |
| DXVK | v2.7.1-1-gplasync, the `d3d9.dll` that comes with the client |
| Data | 18 MPQs: the base archives, `patch.MPQ` and `patch-1` to `patch-5` |
| Addons | ComfyAtmosphere and the client's Blizzard addons |
| System | Windows 10 (build 19045), NVIDIA GeForce RTX 2080 Super, driver 576.28 |

`/atmos probe` (or F12) writes a report of your client into `comfyatmos.log`, under `client report`: the same
parts, with a hash of each file and every DLL loaded. Compare it with this table.

## Install

Download the zip from [Releases](https://github.com/aloofbit/comfyatmosphere/releases), or build it (below).

1. Copy `comfyatmos.dll` and `comfyatmos.ini` to the client folder.
2. Add the line `comfyatmos.dll` to `dlls.txt`. ⚠️remove comfyfog/comfygrass if you have them
3. Copy the folder `addon/ComfyAtmosphere` to `Interface\AddOns`.
4. Start the game with `VanillaFixes.exe`.

## Config

Use `/atmos options` in game to enable/disable and tune features of the mod.

![The comfyatmosphere controls in the /atmos options window: volumetric light, its strength, quality, density, distance and direction, lamp glow and lamp distance.](media/settings-example.png)


## In-game controls `/atmos`

`/atmos` reads and sets any value in `comfyatmos.ini` from the game's chat. A change shows at once.

| Command | |
| --- | --- |
| `/atmos` | The commands and the sections |
| `/atmos options` | Open or close the settings window |
| `/atmos debug` | Open or close the debug panel (below) |
| `/atmos stats` | Show or hide the stats panel (below) |
| `/atmos probe` | Log one frame to `comfyatmos.log`, as F12 does |
| `/atmos bench` | Run the benchmark, as Alt+F12 does |
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

The **Debug** button in the settings window, or `/atmos debug`, opens the debug panel:

![debug menu](https://private-user-images.githubusercontent.com/58625258/662715071-1a258734-3082-4f42-912e-0fd274bc9104.png?jwt=eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.eyJpc3MiOiJnaXRodWIuY29tIiwiYXVkIjoicmF3LmdpdGh1YnVzZXJjb250ZW50LmNvbSIsImtleSI6ImtleTUiLCJleHAiOjE3OTEzMjU5ODQsIm5iZiI6MTc5MTMyNTY4NCwicGF0aCI6Ii81ODYyNTI1OC82NjI3MTUwNzEtMWEyNTg3MzQtMzA4Mi00ZjQyLTkxMmUtMGZkMjc0YmM5MTA0LnBuZz9YLUFtei1BbGdvcml0aG09QVdTNC1ITUFDLVNIQTI1NiZYLUFtei1DcmVkZW50aWFsPUFLSUFWQ09EWUxTQTUzUFFLNFpBJTJGMjAyNjEwMDYlMkZ1cy1lYXN0LTElMkZzMyUyRmF3czRfcmVxdWVzdCZYLUFtei1EYXRlPTIwMjYxMDA2VDIyMjgwNFomWC1BbXotRXhwaXJlcz0zMDAmWC1BbXotU2lnbmF0dXJlPTEyMzhlMWE3OTY4MjA3MDZlMzBlNmNkZWYzZTM2ZDNhZjIzNjk2MjA5MDAyYzEzYzAwZWI0ZjlmOTk4ZDgyYTgmWC1BbXotU2lnbmVkSGVhZGVycz1ob3N0JnJlc3BvbnNlLWNvbnRlbnQtdHlwZT1pbWFnZSUyRnBuZyJ9.L0DrYZI9XT3PEn6v-vxtLy0u8239nS4SihDtQR5MQAM)

## Keys

| Key | |
| --- | --- |
| F11 | Reload `comfyatmos.ini` |
| Ctrl+F11 | Sun rays on / off |
| Alt+F11 | Volumetric light on / off |
| F12 | Log one frame of diagnostics to `comfyatmos.log` |
| Alt+F12 | Run the benchmark (below) |

## Benchmark

Alt+F12, or `/atmos bench`, measures what each feature costs on your computer. It takes 30 seconds.

1. Turn on the volumetric light and play for a minute, so its shadow cache fills as in normal play.
2. Go outside in daylight. Stand still and face the sun.
3. Press Alt+F12. Do not move the mouse until the chat says it is done.
4. Open `comfyatmos.log` in the client folder. The table is on the lines that start with `bench:`.

It runs three steps: rays + volumetric light (with the fog), rays, and nothing. For each step the table
gives:

- **fps** and **ms/frame**: the frame rate.
- **slowest 1%**: the time of the slowest frames, in milliseconds. Stutter shows here.
- **our GPU ms** and **our CPU ms**: the time of our own passes in each frame.

Below the table, one line splits the light into its parts: the shadow map and the light passes on the GPU,
and on the CPU the recording of the game's draws, the shadow cache and its replay.

Keep the game in front while it runs: in the background the client caps its frame rate. Under any cap (vsync,
a frame limiter, the background cap) the frame rate stays the same in every step, and the GPU slows its clocks,
so its times read too high. The CPU column still shows the cost. Turn the cap off for a benchmark.

After the run, the settings go back as they were. F11 stops the run.

## Build

Visual Studio 2022 and CMake. **32-bit only**: the 1.12 client is x86.

```
cmake -B build -A Win32
cmake --build build --config Release
```

`comfyatmos.dll` is written to the project root, next to `comfyatmos.ini`.

## Licence

GPL-3.0. See [LICENSE](LICENSE).
