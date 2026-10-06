# No freeze at sign-in

**Commit:** the one that adds this changelog.

## What changed

Signing in no longer freezes the game while the effects start. The shaders compile in the background from the
moment the DLL loads, and are kept on disk for the next start.

## The measurement

The first frames with the effects compiled 44 shaders on the game's own thread: 11.1 seconds in all, in two
frames that froze for 5.2 and 7.0 seconds. The largest:

| Shader | Time |
| --- | --- |
| The water | 4.1 s |
| The sun shadows | 3.5 s |
| The lamp glow, 10 variants | 2.3 s |
| The volumetric light's march | 0.5 s |

The render targets, the masks and the map files made no slow frame of their own.

## How it works

- **The cache.** Every compile goes through it, keyed by a hash of the source, the name, the profile and the
  defines. A result is kept for the session and on disk in `comfyfog-cache\` in the client folder. A changed
  shader gets a new key, so an update never reads an old one.
- **The worker.** Each pass lists its shaders. A thread below normal priority compiles them, or reads them from the
  cache, while the client is at its login screen. A shader the game asks for while the worker has it waits for
  the worker, not compiles it twice.

## Result

The first run with an empty cache: 21 ms of compiling on the game's thread (the terrain's copies, built from the
game's own shaders), and one frame of about 1 second as the effects start. That frame creates the shader objects
and the render targets, the 4096 x 4096 shadow maps among them.

The log shows each shader's source and time (`shaders:` lines), and for the first 90 seconds in the world each
frame over 40 ms (`startup:` lines).
