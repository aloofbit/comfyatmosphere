# The log in the Logs folder, and the benchmark in the chat

## The log moves to Logs

`comfyatmos.log` is now written to the client's `Logs` folder, with the game's own chat and combat logs:
`Logs\comfyatmos.log`. The DLL makes the folder when it is not there. When it cannot make it, the log stays in the
client folder.

At each start the DLL deletes a `comfyatmos.log` that an older version left in the client folder, so no tool reads
it as the current one. The shader cache, `comfyatmos-cache`, stays in the client folder.

The test scripts (`Run-Tests.ps1`, `Snapshot.ps1`, `Write-Report.ps1`, `Write-Card.ps1`) and wow-test-tool in
comfy-wow (`Login.ps1`, `Run-Test.ps1`) read the newest of `Logs\comfyatmos.log` and `comfyatmos.log`, so they work
with an older DLL too.

## The benchmark's results in the chat

When the benchmark ends, the chat gives its main results:

- The frame rate with every effect and with none, what the effects take a frame, and the slowest 1% of frames.
- Each part's GPU time a frame: the shadow map, the light and fog, the sun shadows, the rays and the lamps.
- A line when the rays or the light did not draw for most of the run, or when the frame rate is capped.

The full table stays in the log, on the lines that start with `bench:`.
