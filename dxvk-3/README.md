# DXVK 3.x

Status: on hold since 2026-10-13. comfyatmos supports DXVK 2.7.1 (the `d3d9.dll` that ships with the client,
v2.7.1-1-gplasync). Players report that the mod "does not work" on DXVK 3.0. What fails is not known.

Findings doc: https://claude.ai/code/artifact/8c70f5e5-163f-447b-aebc-31213d4cfd50

## Measurement, 2026-10-13

The newest release was DXVK 3.1.1 (2026-09-15). It runs the plain client: it starts, logs in and writes no
errors to `WoW_d3d9.log`.

Setup:

- Test client `octow - Copy`, with comfyatmos off (`comfyhot.dll` removed from `dlls.txt`).
- NVIDIA driver 576.28, 1920x1080 desktop, game at 1152x864 with MSAA 2.
- `dxvk-frames.txt` with the ComfyTest `frames` timer. It visits the three places of `frame-rate-performance`:
  Stormwind harbour, Elwynn and the Ironforge gate. At each place it records 8 s from the arrival, 10 s
  standing and 6 s turning.
- `Run-Dxvk.ps1` ran six alternating sessions: 2.7.1, 3.1.1, 2.7.1, 3.1.1, 2.7.1, 3.1.1.
- Raw lines: `frames-2026-10-13.txt`. The tag `271` is 2.7.1, `311` is 3.1.1.

Mean frame rate of three sessions:

| Place | 2.7.1 | 3.1.1 | Change |
|---|---|---|---|
| Harbour, standing | 200 | 213 | +6% |
| Harbour, turning | 177 | 177 | 0% |
| Elwynn, standing | 151 | 147 | -3% |
| Elwynn, turning | 147 | 148 | +1% |
| Ironforge, standing | 311 | 329 | +6% |
| Ironforge, turning | 265 | 279 | +5% |

- The slowest 1% of frames did not change (4 to 12 ms).
- Both versions had the 2 to 3 s hitches after a teleport.
- The GPU load median was 32%: these runs were CPU-bound. With comfyatmos on, the game is GPU-bound, so the
  gain is likely smaller.
- On 3.1.1 the Ironforge gate got faster each session (310, 332, 347 fps). The new shader cache filling is a
  possible cause.
- Driver 576.28 is too old for the descriptor heap of 3.x (it needs 595.84 or later). 3.1.1 used descriptor
  buffers.

The owner switched between the two versions in Goldshire at 1920x1080 and found them close.

## Driver 617.42

After the measurement the driver went to 617.42. 3.1.1 then logs `Binding model: Descriptor heap`. The first
look at 3.1.1 on it was "it might be worse". There is no measurement on 617.42 yet, for either version, and
the shader caches were empty after the driver update.

## What 3.x changes

- A fix for crashes from the 32-bit address space.
- Descriptor heaps on NVIDIA (driver 595.84 or later).
- Fixed-function ubershaders.
- Fog fixes.
- `D3DRS_MULTISAMPLEANTIALIAS` support.
- `DXVK_DEBUG=hang`, to find GPU hangs.

## What 3.x costs

- No gplasync build of 3.x exists. The DXVK-GPLALL fork cancelled its 3.x branch.
- 3.x needs Vulkan 1.4.
- Antivirus programs flag the 32-bit builds.
- comfyatmos is reported not to work on it.

## Next steps

1. Measure again on driver 617.42: six alternating sessions with comfyatmos off, then six with it on.
   Re-measure 2.7.1 too. All earlier numbers, `perf-history.json` included, were taken on 576.28.
2. Install comfyatmos on 3.1.1 and read `Logs\comfyatmos.log`. The hook patches the shared device vtable
   through a probe device (`AttachToDxvk` in `src/comfyatmos.cpp`); look there first.
3. Get `Logs\comfyatmos.log` and `WoW_d3d9.log` from a player who tried 3.x.
4. Ask the players whether they see ERROR #132 or out-of-memory crashes.

## Running the script

`Run-Dxvk.ps1` closes the test client, copies a `d3d9.dll` in, logs in with wow-test-tool and runs
`dxvk-frames.txt`. The game window comes to the front. Six sessions take about 30 minutes.

```powershell
.\dxvk-3\Run-Dxvk.ps1 -Old <path to the 2.7.1 d3d9.dll> -New <path to the 3.x d3d9.dll>
```

Results go to `dxvk-3\results` (not tracked). Keep a copy of the client's own `d3d9.dll` first, and put it
back after the run.
