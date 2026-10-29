[![License GPLv3](https://img.shields.io/badge/license-GPL_v3-green.svg)](http://www.gnu.org/licenses/gpl-3.0.html)
[![Latest release](https://img.shields.io/github/v/release/Borostwory/Pioneer-The-Last-Frontier?include_prereleases&label=release)](https://github.com/Borostwory/Pioneer-The-Last-Frontier/releases)

![Pioneer – The Last Frontier](.github/images/TLF_Logo.jpg)


# Pioneer – The Last Frontier

**The Last Frontier** is a modified fork of [Pioneer Space Simulator](https://github.com/pioneerspacesim/pioneer).

> **Status:** work in progress, and a fun project to test LLM capabilities.
> It is 100% vibe coded: all code was written with AI assistance.
> Expect bugs, and expect modules and settings to change.
>
> This fork is not affiliated with or endorsed by the Pioneer developers.
> Please don't report bugs from this fork to the upstream Pioneer project.


## What it adds

Click a screenshot to see it larger.

### Orbit Planner

Chains of planned burns, rendezvous and flyby planning, closed-loop autoburn,
a burn HUD and a 3D orbit map with patched trajectories.

<p>
  <a href=".github/images/screens/orbit-planner-1.jpg"><img src=".github/images/screens/orbit-planner-1.jpg" width="49%" alt="Orbit Planner"></a>
  <a href=".github/images/screens/orbit-planner-2.jpg"><img src=".github/images/screens/orbit-planner-2.jpg" width="49%" alt="Orbit Planner, 3D orbit map"></a>
</p>

### Surface Exploration

Leave the ship on foot or on EVA with a jetpack, and drive a rover carried by
ships with a vehicle bay.

<p>
  <a href=".github/images/screens/eva.jpg"><img src=".github/images/screens/eva.jpg" width="49%" alt="EVA"></a>
  <a href=".github/images/screens/eva-rover.jpg"><img src=".github/images/screens/eva-rover.jpg" width="49%" alt="EVA and the rover"></a>
</p>

### Hangars, Storage and Crew Lounges

Lease hangars at stations to store ships and swap your active ship, keep
equipment and cargo in each hangar's store, and have ships, equipment and crew
delivered between stations. Every station has a crew lounge, where crew who
aren't aboard wait on half wages; hire them straight into the lounge or send
them to another station's lounge. A Fleet tab shows everything you own.

<p>
  <a href=".github/images/screens/hangar.jpg"><img src=".github/images/screens/hangar.jpg" width="49%" alt="Hangar"></a>
  <a href=".github/images/screens/fleet.jpg"><img src=".github/images/screens/fleet.jpg" width="49%" alt="Fleet tab"></a>
</p>

### Weather and Clouds

Volumetric multi-layer clouds, weather fronts and cloud shadows.

<p>
  <a href=".github/images/screens/clouds-1.jpg"><img src=".github/images/screens/clouds-1.jpg" width="49%" alt="Volumetric clouds"></a>
  <a href=".github/images/screens/clouds-2.jpg"><img src=".github/images/screens/clouds-2.jpg" width="49%" alt="Volumetric clouds"></a>
</p>

### Planet Terrain

Per-pixel ground and water, photo-based ground materials, river valleys and
detailed terrain from orbit to the ground.

<p>
  <a href=".github/images/screens/terrain.jpg"><img src=".github/images/screens/terrain.jpg" width="49%" alt="Terrain improvements"></a>
  <a href=".github/images/screens/terrain-clouds.jpg"><img src=".github/images/screens/terrain-clouds.jpg" width="49%" alt="Terrain and clouds"></a>
</p>

### Cities

Layouts shaped by their planet, forcefield domes and gardens.

<p>
  <a href=".github/images/screens/cities-1.jpg"><img src=".github/images/screens/cities-1.jpg" width="49%" alt="Cities"></a>
  <a href=".github/images/screens/cities-2.jpg"><img src=".github/images/screens/cities-2.jpg" width="49%" alt="Cities"></a>
</p>
<p>
  <a href=".github/images/screens/greenhouse-domes.jpg"><img src=".github/images/screens/greenhouse-domes.jpg" width="49%" alt="Greenhouse domes"></a>
  <a href=".github/images/screens/clouds-cities.jpg"><img src=".github/images/screens/clouds-cities.jpg" width="49%" alt="Clouds and cities"></a>
</p>

### Lighting and Shadows

Sun shadows, city lights, ship lights and animated stars.

<a href=".github/images/screens/sun.jpg"><img src=".github/images/screens/sun.jpg" width="49%" alt="Animated star surface"></a>

### Flight HUD

A wider reticle with a heading tape, and near planets a fighter-jet style
pitch ladder with headings along the horizon and speed and altitude tapes.
KSP-inspired direction markers, coloured in the heading-lock menu.

<p>
  <a href=".github/images/screens/flight-hud.jpg"><img src=".github/images/screens/flight-hud.jpg" width="49%" alt="Flight HUD over Mars"></a>
  <a href=".github/images/screens/direction-markers.jpg"><img src=".github/images/screens/direction-markers.jpg" width="49%" alt="Direction markers in the heading-lock menu"></a>
</p>

### And more

- **Hyperspace** – a relativistic jump effect and new jump sounds.
- **Audio** – wind, rover and hyperspace sounds.
- **Developer Tools** – travel tab, GPU timers and terrain debugging.


## Bugs

Bugs in The Last Frontier should be reported as issues on this repository,
not on the upstream Pioneer project.


## Getting The Last Frontier

**Latest release: [v0.1.0 – early preview](https://github.com/Borostwory/Pioneer-The-Last-Frontier/releases/tag/v0.1.0)**
(Windows, 64-bit). All releases are on the
[Releases page](https://github.com/Borostwory/Pioneer-The-Last-Frontier/releases).

1. Download the `.zip` from the release's assets.
2. Unzip it anywhere.
3. Run `pioneer.exe`.

The mod's options are in their own **The Last Frontier (mod)** tab in the
settings window.

### Where your settings and saves live

The Last Frontier keeps its settings, saved games, mods and data overrides in
its own folder, separate from the original Pioneer and from other forks, so
they don't overwrite each other:

| System  | Folder |
|---------|--------|
| Windows | `Documents\PioneerTLF` |
| macOS   | `~/Library/Application Support/PioneerTLF` |
| Linux   | `~/.local/share/pioneertlf` (or `$XDG_DATA_HOME/pioneertlf`) |

To build from source instead, use the `the-last-frontier` branch; see
[COMPILING.txt](COMPILING.txt).


## License

Like Pioneer, this fork is licensed under the GNU GPL version 3. See
[licenses/GPL-3.txt](licenses/GPL-3.txt). Assets keep their original licenses;
see the [licenses](licenses) folder and [AUTHORS.txt](AUTHORS.txt).

The original Pioneer code and assets are by the Pioneer developers. The changes
in this fork are by the fork's author and are recorded in the git history.


---


# About Pioneer Space Simulator

![](https://github.com/pioneerspacesim/pioneer/blob/master/data/icons/badge.png)

Pioneer is a space adventure game set in the Milky Way galaxy at the turn of
the 31st century.

The game is open-ended, and you are free to explore the millions of star
systems in the game. You can land on planets, slingshot past gas giants, and
burn yourself to a crisp flying between binary star systems. You can try your
hand at piracy, make your fortune trading between systems, or do missions for
the various factions fighting for power, freedom or self-determination.

For more information, see:
  http://pioneerspacesim.net/
