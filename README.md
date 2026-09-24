# comfyatmosphere

> **Early alpha.** Tested on one computer, with one 1.12 client build (VanillaFixes + DXVK).
> comfyatmosphere hooks deep into the game's rendering. Expect bugs. Back up your client folder first.
> To remove it, delete the `comfyfog.dll` line from `dlls.txt`.

Atmosphere for the World of Warcraft 1.12 client: thicker fog, and volumetric light through the trees. It is
one DLL and one ini file, `comfyfog.dll` and `comfyfog.ini`. The names come from the first version, which only
did fog.

[![Sun shafts through the forest canopy, in game. Click for the full video.](media/comfyatmosphere.gif)](media/comfyatmosphere.mp4)

*Click the preview for the full video.*

It loads like comfygrass (moving grass for the same client): VanillaFixes loads the DLL, and the DLL patches
the Direct3D 9 device of DXVK's `d3d9.dll`.

## Features

| | What it does | Default |
| --- | --- | --- |
| **Fog** | One 0–100 `thickness` dial: haze near the camera, increasing gently with distance. Trees and characters get the same fog as the terrain. | on |
| **Volumetric light** | Fog is lit where sunlight reaches it and dark where leaves and walls shade it. It stays fixed in the world when the camera moves. Needs `[depth]` and `[shadow]`. | off |
| **Clouds** | `[sky] clouds = 0` hides the cloud layer. | shown |

All settings are in `comfyfog.ini`. **F11 reloads it in game.**

## In-game controls

The addon in [`addon/ComfyAtmosphere`](addon/ComfyAtmosphere) adds these controls to **Video > Shaders** in the
game's options:

| Control | Setting in `comfyfog.ini` |
| --- | --- |
| Atmospheric Fog, Fog Thickness | `[fog] enabled`, `thickness` |
| Volumetric Light, Volumetric Light Strength | `[volume] enabled`, `strength` |
| Clouds | `[sky] clouds` |

A change shows in the world while you move the slider. **Cancel** puts the old values back. **Defaults** puts
the values from `comfyfog.ini` back.

- A control you move wins over `comfyfog.ini`, also after F11.
- The **Volumetric Light** box also turns on `[depth]` and `[shadow]`, which the light needs.
- The addon needs `comfyfog.dll`. Without the DLL, it adds no controls.
- The other settings stay in `comfyfog.ini` only.

The addon needs the Turtle WoW options window, which builds its pages from a table the addon can add to.

## Keys

| Key | |
| --- | --- |
| F11 | Reload `comfyfog.ini` |
| Shift+F11 | Fog on / off (to compare) |
| Alt+F11 | Volumetric light on / off |
| F12 | Log one frame of diagnostics to `comfyfog.log` |

## Install

Download the zip from [Releases](https://github.com/aloofbit/comfyatmosphere/releases), or build it (below).

1. Copy `comfyfog.dll` and `comfyfog.ini` to the client folder, next to `WoW.exe` and `d3d9.dll`.
2. Add the line `comfyfog.dll` to `dlls.txt`. If you use comfygrass, put it **after** `comfygrass.dll`.
   comfyfog chains on top of comfygrass, so the grass gets the new fog.
3. For the in-game controls, copy the folder `addon/ComfyAtmosphere` to `Interface\AddOns`.
4. Start the game with `VanillaFixes.exe`.

To turn on volumetric light, set `enabled = 1` under `[depth]`, `[shadow]` and `[volume]` in the ini, or tick
**Volumetric Light** in Video > Shaders.

## Build

Visual Studio 2022 and CMake. **32-bit only**: the 1.12 client is x86.

```
cmake -B build -A Win32
cmake --build build --config Release
```

`comfyfog.dll` is written to the project root, next to `comfyfog.ini`.

## Caveats

- **Made for one client build.** The volumetric light uses camera and player addresses from one `WoW.exe`.
  Another build moves them. The ini exposes them.
- **Volumetric light** draws the world's solid geometry again from the sun each frame. It is the most
  expensive feature. If the frame rate drops, turn it off with Alt+F11.

[NOTES.md](NOTES.md) explains how it works, what was measured in the client, and what did not work.

## Time of day

Time of day is a separate DLL: [comfytime](https://github.com/aloofbit/comfytime). Use it to test the light
at noon, or to keep the sun where you want it. The volumetric light follows the sun in the sky, so it moves
with the time comfytime sets.

## Licence

GPL-3.0. See [LICENSE](LICENSE).
