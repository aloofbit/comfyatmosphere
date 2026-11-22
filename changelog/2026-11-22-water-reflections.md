# Water Reflections

## What changed

- **What stands on the screen, mirrored in the water.** Until now the water reflected the sky alone. From each water
  pixel the mirrored line of sight is followed out in growing steps, each put on the screen with the world camera
  and tested against the depth before the water (`sUnder`); where it first passes behind what the screen holds, the
  step is halved back four times and that pixel's colour is taken from the screen before the water (`sScene`).
  Trees, bridges, houses, players and mounts show in the water; the sky's reflection stays where nothing is met.
- **A share of its own.** Through the sky's Fresnel (Sky Reflection 0.15) the reflection was 2% looking down, and
  nothing could be recognised. Where the line meets something it takes at least 30% at full strength, toward all of
  it at a glancing look, as calm water shows the shore. It follows a calmer surface, a third of the waves' tilt, so
  the shapes stay whole and still ripple.
- **Only what is on the screen can be mirrored.** The weight fades at the screen's edges and with the distance.
- **Its own shader** (`water_ps_ssr`, `WATER_SSR`), so where it does not compile (ps_3_0 has 32 registers) the
  water is drawn as before; the log says which. It compiles with Proton Experimental's compiler (vkd3d), with no
  `continue` and no `break` in its loop. Its constants are c100 to c104 (c120 to c223 are all taken).
- **Controls:** Water Reflections on the Water page, 0 to 100 (default 80); in `[water]`: `objects`,
  `objectsReach` (yards followed, default 400) and `objectsDebug` (the water red where something was met, blue where
  not).

## Why

The water showed the sky and nothing else of the world: no tree, bridge, house or player stood in it.
