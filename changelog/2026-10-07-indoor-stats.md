# Inside a building, on the stats panel

The stats panel (`/atmos stats`, or Stats on the debug panel) shows the DLL's indoor test. The test decides when the
sun shadows fade (`[sunshadows] indoor`), when Indoor Lamps applies, and when Night Darkness is off.

## The rows

Under Position:

| Row | Shows |
| --- | --- |
| Inside building | `true` or `false`: the answer the effects get |
| Building | the building file that got furthest through the test |
| Why | the step that decided |

What Why can say, from the earliest step to the last:

| Why | Meaning |
| --- | --- |
| `no building here` | the player is in no building's box |
| `not loaded yet` | the building's file is not loaded yet |
| `no rooms` | the building has no group that counts as a room |
| `in no room's box` | the player is in the building, but in no room's box |
| `room N, nothing above you` | the room has no triangle over the player |
| `room N, no ceiling (lowest X yd up)` | a triangle is above, but not 1.5 to 40 yards up |
| `room N, ceiling X yd up` | indoors |
| `room N, by its box alone` | indoors: the room kept no triangles, so its box decides |

N counts the building's rooms, not its group files.

## Other changes

- The test gives the same answers as before. It writes the reason only for the stats panel, once a second.
- The stats text is 1000 characters, up from 600: the new pair pushed the fog figures off the end. The DLL and
  the addon must be installed together. With an older addon, the panel waits for the DLL and shows nothing.
