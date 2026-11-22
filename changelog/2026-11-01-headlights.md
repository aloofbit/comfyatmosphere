# Headlights on the mechanostriders

Since v0.17.1-alpha.

## What changed

- **A mechanostrider's lamp throws a cone of light at night.** The model has the lamp as geometry only: no M2 light,
  and its particle emitters are the exhaust. For every mounted unit whose mount's model file holds a word of
  `[headlight] models` (default `MechaStrider`: the plain, gnome and PvP mechanostriders), one full-screen pass lights
  the ground and walls ahead of it and shows the cone in the air as a haze. Other players' mounts too, up to four, the
  nearest, within `reach` yards.
- **Where the lamp is** is set from the unit's place and facing (`forward`, `height`, `tilt`), as the model's bones
  move with the animation.
- **Controls:** a Headlights box and four sliders (Headlight Strength, Headlight Beam, Headlight Length, Headlight
  Angle) on the Lamps page; the rest in `[headlight]` of `comfyatmos.ini`.

## Also in this build

- **The water draws under Wine.** `water_ps` used `continue` in two loops, which Wine's shader compiler (vkd3d) does not
  implement, so the water shader never built. The loops now skip with an `if` block.
- **A 1.4 GB log.** The rain splash's texture cache refused new entries once it held 256, so every draw with another
  texture read 65536 texels and logged a line, every frame. A full cache is emptied now.
- **The minimap button's menu on the 1.12 client**, which has no `UIDropDownMenu_CreateInfo`.
