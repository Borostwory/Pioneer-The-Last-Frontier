// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _PLANETPHYSICS_H
#define _PLANETPHYSICS_H

#include "TerrainMaterials.h"

#include <cstdint>
#include <string>
#include <vector>

class SystemBody;

// What a body's surface comes from, in two groups that stay apart: the interior
// is built from the core values alone, so it cannot read a surface parameter.
// Metallicity, volcanicity, air, ocean, ice and life describe the surface only.
// Values come from the planet lab (physics.py); randomness comes from the body's
// seed, so a body agrees with the lab in kind, not draw for draw.
namespace PlanetPhysics {

	enum class ParentKind {
		STAR,
		GAS,
		ROCK
	};

	struct Core {
		bool authored = false; // from the custom systems, not generated
		bool asteroid = false;
		double mass = 1.0; // Earth masses
		double radius = 1.0; // Earth radii
		double orbit = 1.0; // AU, around its parent
		double eccentricity = 0.0;
		double orbitAroundStar = 1.0; // AU, its planet's orbit if it is a moon
		ParentKind parentKind = ParentKind::STAR;
		double parentMass = 332946.0; // Earth masses
		double starTemperature = 5778.0;
		double starRadius = 1.0; // solar radii
		double rotationDays = 1.0;
		double tilt = 0.0; // radians
		double temperature = 288.0; // K
		uint32_t seed = 0;
	};

	struct SurfaceInputs {
		double metallicity = 0.0;
		double volcanicity = 0.0;
		double airDensity = 0.0; // kg/m3
		double oxidizing = 0.0;
		double ocean = 0.0;
		double ice = 0.0;
		double life = 0.0;
	};

	struct Interior {
		double gravity = 0.0;
		double formationTemperature = 0.0;
		double radiogenicHeat = 0.0;
		double tidalHeat = 0.0;
		double heat = 0.0;
		bool iceCrust = false;
		bool shell = false;
		double siliconActivity = 0.0;
		double heatPipe = 0.0;
		double iceActivity = 0.0;
		bool past = false;
		double plains = 0.0;
		bool plates = false;
		bool lid = false;
		double contraction = 0.0;
		bool dichotomy = false;
		bool fossil = false;
		double reliefLimit = 0.0; // metres
		double transitionDiameter = 0.0; // km
		double cratering = 0.0; // 0..1 of saturation
		double shellStrength = 0.0; // 0..1
		bool basin = false; // one great basin on the old crust
		bool hemispheres = false; // two-toned, like Iapetus
		bool rift = false; // one great rift
		bool porous = false; // a small body of ice, more holes than ground, like Hyperion
		const char *analogue = "";
	};

	struct Surface {
		double air = 0.0;
		bool weather = false;
		double sea = 0.0;
		const char *seaState = "none";
		double frozen = 0.0;
		double wetness = 0.0;
		double wear = 0.0;
		double wearLarge = 0.0, wearSmall = 0.0; // of craters, from air, liquid and life
		double equatorTemperature = 0.0;
		double poleTemperature = 0.0;
		double ice = 0.0;
		double lava = 0.0;
		double glow = 0.0;
		double lavaPlanet = 0.0;
		bool active = false;
		double molten = 0.0;
		double melted = 0.0; // 0..1, how far heat alone has melted the crust
		bool sulphur = false;
		TerrainMaterialTable::Layer ground = TerrainMaterialTable::REGOLITH;
		double lapse = 0.0;
		double rain = 0.0;
		bool forcedLava = false;
	};

	Core CoreOf(const SystemBody *body);
	SurfaceInputs SurfaceInputsOf(const SystemBody *body);

	Interior MakeInterior(const Core &core);
	Surface MakeSurface(const Core &core, const SurfaceInputs &inputs, const Interior &interior);
	// What the surface does to the interior's craters, once both are known: on a world with plates, air and water
	// wear away the old craters the plates leave on the continents
	void WearCraters(Interior &interior, const Surface &surface);

	const char *LayerName(TerrainMaterialTable::Layer layer);
	// readable lines for the Terrain Debug window
	std::vector<std::string> Dump(const Core &core, const SurfaceInputs &inputs, const Interior &interior, const Surface &surface);

} // namespace PlanetPhysics

#endif // _PLANETPHYSICS_H
