# Test tools

**Commits:** `cfd4e38`, `279ee33` (comfyatmosphere), `80bba1b` (comfytime)

## A test can set the time of day

The sky's hour comes from comfytime, not from the server. A test without an hour ran at whatever hour the client
showed: the lighthouse test, made at night, ran by day on the release DLL.

- A test's config can now name `hour` (0 to 24). `tests/Run-Tests.ps1` writes it into the client's
  `comfytime.ini` before the test and puts the file back after it.
- comfytime reads `comfytime.ini` again when the file changes (it looks twice a second). This build is in the test
  client only: players do not need it, and it waits for comfytime's next release.
- `tests/Snapshot.ps1` records the hour the sky shows, from the probe's `night: game time` line.

## No failed shader compile at each start

The sun cover's depth shader (`cover_depth`) was compiled for `ps_2_0` first, then for `ps_2_b`. It needs 81
instruction slots and `ps_2_0` allows 64, so the first compile always failed: a `FAILED` line in the log and 43 ms
of the shader worker at each start. It is now compiled for `ps_2_b` only. The sun cover works as before.

## New tests

- `stormwind-lighthouse-light`: the harbour's lighthouse from the shore at 1:00, a shot and a 10 s video.
- `darkshore-underwater-npcs`: creatures under the sea, seen from the surface.
