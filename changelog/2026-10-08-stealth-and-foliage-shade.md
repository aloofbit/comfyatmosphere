# Your own stealth shadow, and the shade on grass and ferns

In v0.12.0-alpha (branch `0.12.0`).

**Commits:** `aa96980`, `f043456`

## Your own character in stealth casts no shadow

A player's report: a rogue in stealth cast a shadow of their own, and no other stealthed unit did. The player was
left out of the stealthed units on 2026-10-04, when the see-through rule took the client's fade of your character
and your mount, zoomed in, for stealth. The rule reads the stealth flag now, and a fade without it is not taken. The
player is a stealthed unit again. Zoomed in, `[depth] seeThroughNear` still keeps your own depth.

## Grass and ferns dark under cover or lit as a whole

In Redridge at Shadow Resolution 2048 the ferns were striped with shade, and the stripes crawled as the ferns swayed
(the owner). The ground's grass and ferns (the client's clutter, drawn through the wind shader) now carry a stencil
bit of their own, blue in the body mask, and the sun shadows treat them apart:

- A plant's shade is the share of 8 points on a disc round it that sees the sun, pushed toward dark or lit. Every
  pixel of a plant reads near the same disc, so the plant is dark under cover or lit as a whole. It has no dapples,
  and a shadow's edge does not cut it in two.
- It takes no shade from leaves nearer than Foliage Self Shade: its own and its neighbours'. A tree's crown higher
  up still shades it.
- It takes no normal offset: a leaf card's facing from the depth is noise.
- Creatures' shadows do not fall on it by default.

The first try marked every alpha tested model. The trees' crowns changed and the ferns did not (the owner). The trees
keep the leaf maps' shade as before.

## Tried and dropped

- **A rim at the feet and the head over a shadow.** A thin white outline showed under the player's feet and round
  the head where it stood over the guard's shadow. The fixes made the edges of every shadow move while standing
  still, static shadows too. They were all dropped.
- **Shadows that jump between two points** were not in the code. The test client's `comfyatmos.ini` had
  `[shadow] sunStep = 0`, which moves the shadow grid every frame. It is 0.05 again (the default).

## Controls

| Control | Tab | Range | Default |
| --- | --- | --- | --- |
| Foliage Shade Size | Shadows | 0 to 5 yards | 1.9 |
| Foliage Shade Edge | Shadows | 0 to 50% | 35 |
| Foliage Self Shade | Shadows | 0 to 5 yards | 1.5 |
| Creature Shadow on Foliage | Shadows | 0 to 100% | 0 |

The defaults are the owner's values. Size and Self Shade are set in tenths of a yard (`comfyFoliageShadeSize`,
`comfyFoliageSelfShade`). Foliage Shade Size 0 lets the shade fall on the plants as it falls.

`comfyatmos.ini`: `[sunshadows] foliageRadius`, `foliageEdge`, `foliageSlack`, `foliageUnits`.

Tests: `development-guard-stealth` (the stealth part), `redridge-grass-shadow`.
