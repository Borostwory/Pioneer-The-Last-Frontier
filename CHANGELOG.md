# Changelog

## v0.2.0

> **Your previous saves won't work in this version. Start a new game.**

### General & engine
- **New OpenGL.** The game now draws with OpenGL 4.6, a newer version of the
  graphics standard your graphics card uses. It's what makes the next three
  possible. Your graphics driver must support it; most cards from the last
  several years do.
- **Tessellation.** Near you, the ground is split into many more, smaller
  pieces, so its stones and lumps can stand up out of it instead of being
  painted flat. Your boots and the rover's wheels ride over them.
- **Bump mapping.** The ground's textures carry the height of their stones
  and lumps, so they catch the sun and cast small shadows.
- **Textures.** Ground textures four times sharper, with shine that follows
  how rough each surface is. They're laid in random turns, so they no longer
  repeat in a visible grid.
- **New anti-aliasing.** Two new ways to smooth jagged edges, in the Video
  tab: FXAA, which costs the least, and SMAA, which is sharper and the
  default. Unlike the old multisampling, both work with the clouds, so
  multisampling is now locked off.

Tessellation and bump mapping share one switch in the mod's settings tab.

### New features
- **New planet generator.** Each world starts from a few facts about its
  surface: how much rock, metal, ice, water and air it has, how hot and how
  volcanic it is, and whether there is life. From those, the generator works
  out the rest, step by step:
  - Plates and sea level decide where the land and seas are.
  - The climate decides what covers the ground: deserts, forests, tundra
    and ice caps.
  - Rivers run down valleys into lakes.
  - Impacts leave craters, from huge basins down to a few hundred metres.
    Air, water and lava wear them away, so old airless worlds keep the most.
  - Volcanic worlds get volcanoes, lava flows and lava seas. The hottest
    rock worlds melt.
  - Ice moons crack into ridges, grooves and stripes, like Europa, Ganymede
    and Enceladus.
  - Wind piles sand into dunes and spreads dust and snow in streaks.
  - Plants take a colour that suits their star.

  Everything is fixed to the world, so the mountain you see from orbit is
  the one you land on. Mountains and craters are close to real heights and
  depths.
- **Ascent guidance.** Set the orbit's height and your launch heading, lift
  off and follow the diamond. It guides the climb and the turn into orbit,
  and can plan the final burn for you.
- **New atmosphere logic.** Each world's air has its own main gas and haze.
  They set the colour of its sky and clouds, the light on the ground, and how
  thick the air is to fly through. Venus has an orange sky from the ground,
  and Titan an orange smog. Gas giants have no ground: fly down into their
  clouds, and it grows darker the deeper you go.

### Fixes
- **Missing bodies in Sol.** Mimas, Enceladus and Miranda added.
- **Incorrect data in Sol.** Sol's planets and moons corrected with NASA's
  numbers: orbits, spins, masses, sizes and temperatures.
- **AI landing bug.** AI ships could set down on surface starport pads they
  had no clearance for, and under time acceleration you heard them crash
  while docked. Pads now work as in the original game.
- **Olympus Mons** stands at its true height.

### QOL
- **Faster loading.** The game keeps the graphics programs it builds, so
  after the first start they load in moments. Starting a game no longer
  freezes while each world's terrain is built.
- **Better performance.** The ground costs less to draw, and new planets
  and clouds come into view without the game freezing.
- **Long hyperjump routes are planned in moments.**