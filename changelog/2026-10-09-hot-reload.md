# A new build in a running game

In v0.12.0-alpha (branch `0.12.0`).

## What changed

- **comfyatmos.dll exports `ComfyHotDetach`.** wow-test-tool's comfyhot.dll (test clients only, in the comfy-wow
  repo) loads a copy of the DLL, and on a new build asks that copy to let go, then loads the new one. With
  `tools\wow-test-tool\Reload-Dll.ps1` a build reaches the game in about 60 ms, with no restart and no login.
- **At the next frame the copy lets go of everything it changed:**
  - the device's slots;
  - the grass patch in the client's code, with the client's own bytes put back;
  - the client's own depth surface, bound again;
  - everything made on the device, as at a Reset;
  - the map terrain's meshes;
  - its caches on the heap: the terrain loader frees its models and DBC tables, closes the MPQ archives and
    ends; the fog's noise array and the compiled shaders go too.
- **The next copy keeps a test's settings.** The values `/atmos` set and the hour shown go to it. The time's address
  check is handed over too: mid-session the three addresses hold a mix of the client's time and ours, and the
  check refused them (as at F11 before 2026-10-07).
- **The hooks go through comfyhot.dll** when it is loaded (`ComfyHotHookSlot`, `ComfyHotUnhook`). comfyaim and
  comfytest hook over comfyatmos, so a new copy cannot take the slots. The first copy's hook functions jump to the
  newest copy's hooks instead, and the order of the chain does not change.
- **A copy in `comfyhot\<n>\` reads `comfyatmos.ini` beside WoW.exe**, and writes its log in the client's `Logs`.
- **A notice skips the numbers already registered.** A new copy counts from 1 again, and `comfyNotice1` was taken.

## Measured (test client, 2026-10-09)

- 16 reloads in one session, each 50 to 77 ms from the new file to the new copy loaded.
- `ashenvale-edge-strips` and `far-terrain-cache` pass on copy 15, and on copy 29 with the memory freed.
- Memory. A copy is never unloaded: the client keeps pointers to the CVar names and values given to it.
  - First, each reload kept about 30 MB (1728, 1745, 1779, 1812, 1844 MB, 15 s after each).
  - A measurement build (`-DCOMFY_MEMTRACE=ON`, `src/memtrace.cpp`) counts each heap block by its call stack.
    A copy held 272 MB before its detach and 30.5 MB after. Of that, 19.7 MB was the 18 MPQs' hash and block
    tables, 2.2 MB the loader's last file, about 4 MB M2 models, 1.4 MB DBC tables and 1 MB the fog's noise.
  - With those freed, a copy keeps 0.8 MB of heap, and the game grows about 2.4 MB a reload (2141, 2143, 2145,
    2149, 2150, 2153 MB, 20 s after each).

## Not done

- comfytest.dll and the other mods have no `ComfyHotDetach`, and still need a restart.
- A change to the addon's Lua needs `/reload`, as before.
