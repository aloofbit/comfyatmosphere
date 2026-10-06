# Test tools

**Commits:** `87acd0d` (comfyatmosphere), `44b357a` (comfy-wow)

## The login no longer hangs

`tools/wow-test-tool/Login.ps1` in comfy-wow waited for a `state ready` line that the ComfyTest addon writes once,
then checked the character with one reading. When that reading came while the world still loaded, the script
took the character for gone, deleted the line, and waited out its whole time with the character in the world.

Now it also reads the game's own state, and stops as soon as that reads `world` and comfytest.dll can read the
character. The reading after `state ready` is retried for 10 seconds.

## A test can fly over deep water

`tests/Run-Tests.ps1` turns flight on with the character on the ground. A test that started in the air over deep
water dropped the character into the water, and the wait for ground never ended. A test's config can now name
`flightFrom`, a place on land where flight goes on before the test goes to its start.

## New tests

- `stormwind-harbour-ridge`: the ridge under the sea, no edge.
- `stormwind-harbour-ship-shadow`: a moored ship's shadow on and under the water. It uses `flightFrom`.
