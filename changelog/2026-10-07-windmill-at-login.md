# A windmill's frozen copy from login

## What changed

The shade of a far batch of windmills drawn at login no longer stays frozen under the turning windmill. At login a
windmill's model is not known for a few seconds, and its first draws went to the shadow cache. Once the model was
known, its draws went to its record, and the cache's copy was never drawn again or taken out. Now such an entry is
filed under its object, or dropped when the object has a newer draw. Each frame looks at a 30th of the cache.

`development-windmill` had failed now and then since perf-1 (v0.11.0-alpha) for this reason.

The probe's "object table" line counts the entries moved or dropped since the start.

## Test

`development-windmill`. The reasoning is in `NOTES.md`, "A far batch of windmills left in the cache at login".
