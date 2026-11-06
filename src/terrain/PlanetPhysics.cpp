// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "PlanetPhysics.h"

#include "MathUtil.h"

#include "../Random.h"
#include "../galaxy/SystemBody.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
	constexpr double SOL_MASS_IN_EARTHS = 332946.0;
	constexpr double EARTH_RADIUS_M = 6378135.0;
	constexpr double EARTH_RADIUS_KM = 6378.135;

	double Smooth(const double a, const double b, const double x)
	{
		const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	struct Tides {
		double solid = 0.0; // W/m2
		double shell = 0.0; // with a decoupled shell
	};

	// Peale form, scaled so Io is 2.5 W/m2
	Tides TidalHeat(const PlanetPhysics::Core &core, const double eccentricity)
	{
		if (core.parentKind == PlanetPhysics::ParentKind::STAR && core.orbit > 0.2)
			return {};
		const double radiusKm = core.radius * EARTH_RADIUS_KM;
		const double density = std::clamp(5514.0 * core.mass / std::pow(std::max(core.radius, 1e-9), 3.0), 900.0, 9000.0);
		const double k = std::min(1.0, std::pow(density / 3528.0, 2.0) * std::pow(radiusKm / 1821.0, 2.0));
		const double base = 2.5 * std::pow(core.parentMass / 317.8, 2.5) * std::pow(radiusKm / 1821.0, 3.0) *
			std::pow(eccentricity / 0.0041, 2.0) * std::pow(std::max(core.orbit, 1e-6) * 149598000.0 / 421700.0, -7.5);
		return { base * k, base * std::max(k, 0.3) };
	}
} // namespace

namespace PlanetPhysics {

	Core CoreOf(const SystemBody *body)
	{
		Core core;
		core.authored = body->IsCustomBody();
		core.asteroid = body->GetType() == SystemBody::TYPE_PLANET_ASTEROID;
		core.mass = body->GetMassAsFixed().ToDouble();
		core.radius = body->GetRadiusAsFixed().ToDouble();
		core.orbit = body->GetSemiMajorAxisAsFixed().ToDouble();
		core.eccentricity = body->GetEccentricityAsFixed().ToDouble();
		core.rotationDays = body->GetRotationPeriodInDays();
		core.tilt = body->GetAxialTilt();
		core.temperature = double(body->GetAverageTemp());
		core.seed = body->GetSeed() & 0x7fffffffu;

		const SystemBody *parent = body->GetParent();
		if (parent) {
			const bool parentStar = parent->GetSuperType() == SystemBody::SUPERTYPE_STAR || parent->GetType() == SystemBody::TYPE_GRAVPOINT;
			core.parentKind = parentStar ? ParentKind::STAR : parent->GetSuperType() == SystemBody::SUPERTYPE_GAS_GIANT ? ParentKind::GAS :
																														  ParentKind::ROCK;
			core.parentMass = parent->GetMassAsFixed().ToDouble() * (parentStar ? SOL_MASS_IN_EARTHS : 1.0);
		}

		// the star, and this body's planet if it is a moon
		const SystemBody *planet = body;
		for (const SystemBody *up = body->GetParent(); up; up = up->GetParent()) {
			if (up->GetSuperType() == SystemBody::SUPERTYPE_STAR) {
				core.starTemperature = double(up->GetAverageTemp());
				core.starRadius = up->GetRadiusAsFixed().ToDouble();
				break;
			}
			if (up->GetType() != SystemBody::TYPE_GRAVPOINT)
				planet = up;
		}
		core.orbitAroundStar = planet->GetSemiMajorAxisAsFixed().ToDouble();
		if (core.orbitAroundStar <= 0.0)
			core.orbitAroundStar = core.orbit;
		return core;
	}

	SurfaceInputs SurfaceInputsOf(const SystemBody *body)
	{
		SurfaceInputs s;
		s.metallicity = std::min(body->GetMetallicity(), 1.0);
		s.volcanicity = std::min(body->GetVolcanicity(), 1.0);
		s.airDensity = body->GetVolatileGas();
		s.oxidizing = body->GetAtmosOxidizing();
		s.ocean = body->GetVolatileLiquid();
		s.ice = body->GetVolatileIces();
		s.life = body->GetLife();
		return s;
	}

	Interior MakeInterior(const Core &core)
	{
		Interior it;
		Random rng(core.seed ^ 0x51edu);
		const double radius = std::max(core.radius, 1e-9);
		const double densityReal = 5514.0 * core.mass / std::pow(radius, 3.0);
		const double mass = (densityReal >= 900.0 && densityReal <= 9000.0) ? core.mass : std::clamp(densityReal, 900.0, 9000.0) / 5514.0 * std::pow(core.radius, 3.0);
		it.gravity = 9.81 * mass / (radius * radius);
		const double luminosity = core.starRadius * core.starRadius * std::pow(core.starTemperature / 5778.0, 4.0);
		it.formationTemperature = 278.0 * std::pow(luminosity, 0.25) / std::sqrt(std::max(core.orbitAroundStar, 1e-3));
		const Tides tides = TidalHeat(core, core.eccentricity);
		it.iceCrust = !core.asteroid && it.formationTemperature < 170.0 + 30.0 * rng.Double() && tides.solid < 0.3;
		const bool ice = it.iceCrust;
		const double crustShare = ice ? 0.5 : 1.0;
		it.radiogenicHeat = 0.09 * mass / (radius * radius) * crustShare * (0.7 + 0.6 * rng.Double());
		// a decoupled shell needs tidal heat a small ice body can hold
		it.shell = ice && tides.shell >= 0.005 && tides.shell <= 0.3;
		it.tidalHeat = it.shell ? tides.shell : tides.solid;
		it.heat = it.radiogenicHeat + it.tidalHeat;
		const double q = it.heat;
		it.siliconActivity = ice ? 0.0 : Smooth(0.03, 0.15, q);
		it.heatPipe = ice ? 0.0 : Smooth(0.4, 1.0, q);
		if (!ice && !core.authored && core.parentKind == ParentKind::GAS)
			it.heatPipe = std::max(it.heatPipe, Smooth(-5.4, -4.2, std::log10(std::max(tides.solid, 1e-12))));
		// cracks come from tidal stress, not from the heat of a large moon's rock.
		// The generator's moons have far weaker tides than Sol's, so random bodies are
		// ranked on the scale of their own population
		if (!ice)
			it.iceActivity = 0.0;
		else if (core.authored)
			it.iceActivity = Smooth(0.005, 0.04, it.tidalHeat);
		else
			it.iceActivity = Smooth(-5.9, -4.6, std::log10(std::max(it.tidalHeat, 1e-12)));
		const double tideResonant = TidalHeat(core, std::max(core.eccentricity, 0.015)).solid;
		it.past = ice && it.iceActivity < 0.05 && tideResonant >= (core.authored ? 0.002 : 1e-6);
		const double plainsDraw = rng.Double();
		it.plains = ice ? 0.0 : 0.75 * Smooth(0.0, 0.07, q) * (0.5 + 0.5 * plainsDraw);
		const double platesDraw = rng.Double();
		it.plates = !ice && !core.asteroid && q >= 0.05 && q < 0.4 && core.temperature < 500.0 && platesDraw < 0.85;
		it.lid = it.siliconActivity > 0.2 && !it.plates && it.heatPipe < 0.5;
		if (!ice && !it.plates && it.heatPipe < 0.1)
			it.contraction = (1.0 - it.siliconActivity) * Smooth(0.002, 0.02, q) * (0.4 + 0.6 * rng.Double());
		const double dichotomyDraw = rng.Double(), dichotomyDraw2 = rng.Double();
		it.dichotomy = (it.lid && core.mass < 0.4 && dichotomyDraw < 0.5) || (!ice && !it.plates && core.mass > 0.03 && core.mass < 0.4 && dichotomyDraw2 < 0.35);
		const double fossilDraw = rng.Double();
		it.fossil = !ice && it.formationTemperature > 180.0 && it.formationTemperature < 400.0 && core.mass > 0.02 && core.mass < 3.0 && fossilDraw < 0.6;
		const double relax = ice ? 1.0 / (1.0 + q / 0.01) : 1.0;
		it.reliefLimit = std::min(9000.0 * (9.81 / std::max(it.gravity, 0.01)) * (ice ? 0.5 : 1.0) * relax, 0.06 * core.radius * EARTH_RADIUS_M);
		it.transitionDiameter = 18.8 * (1.62 / std::max(it.gravity, 0.01)) * (ice ? 0.2 : 1.0);
		const double cratersDraw = rng.Double();
		if (it.heatPipe > 0.5)
			it.cratering = 0.0;
		else if (ice)
			it.cratering = (1.0 - 0.97 * Smooth(0.05, 0.4, it.iceActivity)) * (0.8 + 0.4 * cratersDraw) * (it.past ? 0.6 : 1.0);
		else if (it.plates)
			it.cratering = 0.06;
		else if (it.lid && it.plains > 0.6)
			it.cratering = 0.12;
		else
			it.cratering = (0.8 + 0.4 * cratersDraw) * (1.0 - 0.8 * it.siliconActivity);
		it.shellStrength = std::max(it.iceActivity >= 0.05 ? std::sqrt(it.iceActivity) : 0.0, it.past ? 0.25 : 0.0);
		// what marks this body
		if (!it.plates && it.heatPipe <= 0.5) {
			const bool canRift = it.past || it.contraction > 0.3;
			const int pick = rng.Int32(canRift ? 3 : 2);
			if (pick == 1 && ice && !it.past && it.iceActivity < 0.05 && rng.Double() < 0.3)
				it.hemispheres = true;
			else if (pick < 2)
				it.basin = true;
			else {
				it.rift = true;
				// (a small old ice moon can have both, as Tethys has)
				if (ice && core.radius < 0.2)
					it.basin = true;
			}
		}
		// old, small, slow-spinning ice bodies can be two-toned, like Iapetus
		if (ice && !it.past && it.iceActivity < 0.05 && core.radius > 0.05 && core.radius < 0.2 && core.rotationDays > 20.0 && rng.Double() < 0.5) {
			it.hemispheres = true;
			it.basin = false;
			it.rift = false;
		}
		if (ice)
			it.analogue = it.iceActivity >= 0.3 ? "Europa" : (it.iceActivity >= 0.05 && core.radius < 0.1) ? "Enceladus" :
				(it.past && core.radius > 0.2)                                                             ? "Ganymede" :
				it.past                                                                                    ? "Tethys" :
				core.radius > 0.2                                                                          ? "Callisto" :
																											 "Mimas";
		else if (it.plates)
			it.analogue = "Earth";
		else if (it.heatPipe > 0.5)
			it.analogue = "Io";
		else if (it.lid)
			it.analogue = core.temperature > 500.0 ? "Venus" : "Mars";
		else if (core.asteroid)
			it.analogue = "Asteroid";
		else
			it.analogue = it.contraction > 0.2 ? "Mercury" : "Moon";
		return it;
	}

	Surface MakeSurface(const Core &core, const SurfaceInputs &s, const Interior &it)
	{
		using M = TerrainMaterialTable;
		Surface st;
		const double T = core.temperature;
		const double low = T >= 200.0 ? 0.01 : 0.2;
		st.air = Smooth(low, low * 5.0, s.airDensity);
		st.weather = st.air > 0.0;
		const double ocean = std::clamp(s.ocean, 0.0, 1.0);
		if (ocean > 0.0) {
			if (T >= 273.0 && T <= 373.0) {
				st.seaState = "liquid water";
				st.sea = ocean;
			} else if (T >= 90.0 && T <= 180.0 && st.weather) {
				st.seaState = "liquid hydrocarbon";
				st.sea = ocean;
			} else if (T < 273.0) {
				st.seaState = "frozen";
				st.frozen = ocean;
			} else {
				st.seaState = "too hot";
			}
		}
		st.wetness = st.sea > 0.0 ? std::min(1.0, ocean / 0.3) : 0.0;
		st.wear = st.air * (0.25 + 0.75 * st.wetness);
		const double heatTransport = s.airDensity / (s.airDensity + 1.0);
		const double spread = 80.0 * (1.0 - 0.5 * heatTransport) * (1.0 - 0.4 * std::sin(std::abs(core.tilt)));
		st.equatorTemperature = T + 0.35 * spread;
		st.poleTemperature = T - 0.65 * spread;
		const double iceValue = std::clamp(s.ice, 0.0, 1.0);
		const double limit = 263.0 + 15.0 * iceValue;
		const double te = st.equatorTemperature, tp = st.poleTemperature;
		const double cap = 90.0 * std::clamp((te - limit) / std::max(1.0, te - tp), 0.0, 1.0);
		const double cold = 1.0 - std::sin(cap * M_PI / 180.0);
		st.ice = std::min(std::max(iceValue, st.frozen), cold);

		const double volc = core.asteroid ? 0.0 : std::clamp(s.volcanicity, 0.0, 1.0);
		const double land = 1.0 - st.sea;
		const bool cryo = it.iceCrust;
		st.lava = volc * volc * land;
		st.glow = cryo ? 0.0 : Smooth(0.5, 0.7, volc);
		st.lavaPlanet = cryo ? 0.0 : Smooth(0.85, 1.0, volc);
		st.active = !cryo && (volc >= 0.6 || (it.siliconActivity > 0.1 && volc >= 0.05));
		if (st.active)
			st.molten = std::max(st.lava * st.glow * (0.008 + 0.7 * st.lavaPlanet), std::min(0.002, st.lava));
		st.sulphur = !cryo && core.parentKind == ParentKind::GAS && volc > 0.5 && s.ice < 0.2;
		// Random moons with Io's tides get a little fresh lava, though the generator
		// gives them almost no volcanicity: pits and vents only
		st.forcedLava = !core.authored && it.heatPipe > 0.5 && !cryo && volc < 0.5;
		if (st.forcedLava) {
			Random rng(core.seed ^ 0x1Au);
			st.lava = 0.01 + 0.02 * rng.Double();
			st.molten = st.lava * 0.25;
			st.glow = 1.0;
			st.active = true;
			st.sulphur = true;
		}

		if (core.asteroid)
			st.ground = M::REGOLITH;
		else if (st.lavaPlanet > 0.5 && !cryo)
			st.ground = M::LAVA;
		else if (st.sulphur)
			st.ground = M::SALT;
		else if (cryo && st.weather)
			st.ground = M::DIRTY_ICE;
		else if (cryo && (it.iceActivity >= 0.05 || iceValue >= 0.6))
			st.ground = M::ICE; // an active shell, or a surface the data says is mostly ice
		else if (cryo)
			st.ground = M::DIRTY_ICE; // old and quiet: a dark, gravelly lag over ice
		else if (!st.weather)
			st.ground = M::REGOLITH;
		else if (T > 450.0)
			st.ground = M::BASALT;
		else if (st.wetness <= 0.0)
			st.ground = M::DUST;
		else
			st.ground = M::SOIL;

		// wear on craters, from air, liquid and life only
		st.wearSmall = std::min(1.0, st.wear + 0.3 * s.life * st.air);
		st.wearLarge = std::min(1.0, 0.5 * st.wear + 0.1 * s.life * st.air);
		st.lapse = s.airDensity > 0.01 ? it.gravity / 1000.5 : 0.0;
		// rivers and lakes need air and rain: a liquid at a temperature it can fall at
		if (st.sea > 0.0 && st.weather) {
			const double falls = st.seaState == std::string("liquid water") ? Smooth(255.0, 285.0, T) : 0.6;
			st.rain = st.air * std::min(1.0, ocean / 0.05) * falls * (1.0 - Smooth(340.0, 373.0, T));
		}
		return st;
	}

	const char *LayerName(const TerrainMaterialTable::Layer layer)
	{
		static const char *const names[TerrainMaterialTable::LAYER_COUNT] = {
			"ROCK", "GRAVEL", "SAND", "SOIL", "GRASS", "FOREST", "SNOW", "ICE", "BASALT", "LAVA", "ASH", "REGOLITH", "SALT",
			"TUNDRA", "DIRTY_ICE", "LAVA_FLOW", "CRACKED_MUD", "DUST", "METAL", "FOREST_FAR"
		};
		return names[layer];
	}

	std::vector<std::string> Dump(const Core &core, const SurfaceInputs &in, const Interior &it, const Surface &st)
	{
		char line[256];
		std::vector<std::string> out;
		auto add = [&]() { out.emplace_back(line); };
		snprintf(line, sizeof(line), "Core: %.3f Earth masses, %.3f radii, %.0f K, %s", core.mass, core.radius, core.temperature, core.authored ? "authored" : "generated");
		add();
		snprintf(line, sizeof(line), "Interior: like %s, g %.2f, formed at %.0f K, %s crust", it.analogue, it.gravity, it.formationTemperature, it.iceCrust ? "ice" : "rock");
		add();
		snprintf(line, sizeof(line), "Heat: radiogenic %.4f, tidal %.4f W/m2; activity rock %.2f, ice %.2f, heat pipe %.2f", it.radiogenicHeat, it.tidalHeat, it.siliconActivity, it.iceActivity, it.heatPipe);
		add();
		snprintf(line, sizeof(line), "Plates %d, lid %d, past activity %d, dichotomy %d, fossil valleys %d, shell %d, basin %d, rift %d, two-toned %d", it.plates, it.lid, it.past, it.dichotomy, it.fossil, it.shell, it.basin, it.rift, it.hemispheres);
		add();
		snprintf(line, sizeof(line), "Relief limit %.0f m, crater transition %.1f km, cratering %.2f, plains %.2f", it.reliefLimit, it.transitionDiameter, it.cratering, it.plains);
		add();
		snprintf(line, sizeof(line), "Surface data: metal %.2f, volcanic %.2f, air %.3f kg/m3, ocean %.2f, ice %.2f, life %.2f", in.metallicity, in.volcanicity, in.airDensity, in.ocean, in.ice, in.life);
		add();
		snprintf(line, sizeof(line), "Became: ground %s, sea %.2f (%s), ice %.2f, lava %.3f, molten %.3f, glow %.2f, rain %.2f", LayerName(st.ground), st.sea, st.seaState, st.ice, st.lava, st.molten, st.glow, st.rain);
		add();
		return out;
	}

} // namespace PlanetPhysics
