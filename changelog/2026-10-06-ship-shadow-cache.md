# A ship's shadow left behind

**Commit:** `87acd0d`

## What changed

A ship leaving Stormwind's harbour no longer leaves its shadow on the water. Its sails, rigging and lanterns now
move with it in the shadow cache, and their shade goes as soon as the game stops drawing them.

## The cause

Two probes showed 9 shadow cache entries for the ship's sails, kept at points along its path for up to 19.5
seconds. Easing away from the dock, the sails moved less than 0.3 yards a frame. The cache's still rule held each
entry in place, and a new entry took over further on. An entry not drawn is dropped only when it is near, brief,
or old, and the age rules wait while its shade is in view.

## How it works

The ship is one object with its parts. The hull is followed from its own draws, as the ship wake does. That
tracker now always runs. A hull moving over 0.4 yards a second at the water is a ship under way, and every draw
within 35 yards of it is one of its parts: moving from the first frame, never held by the still rule, and gone
the moment it is not drawn.

The game's object list was tried first. It holds the ships (transports, type 15), but their place fields are 0.

## Probe

A probe (F12) lists each ship under way, and each transport in the object list with its update fields.
