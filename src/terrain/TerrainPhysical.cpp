// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainPhysical.h"

#include "MathUtil.h"
#include "SphereCells.h"
#include "TerrainAsteroid.h"
#include "TerrainErosion.h"

#include "../FileSystem.h"
#include "../Random.h"
#include "../galaxy/SystemBody.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>

using SphereCells::Fbm;
using M = TerrainMaterialTable;

namespace {
	constexpr double NONE_LOW = -1e30;
	constexpr double NONE_HIGH = 1e30;
	// the share of active shells with ice in parts only whose ice is cracked as Europa's
	constexpr double ICE_CAP_CRACKS_SHARE = 0.35;
	// lava plains up close (LavaPlainsRelief): flows about this many km across, their fronts this high (m) and as wide as this
	// share of a flow's step, shown fully where the samples are this close (km) and gone by twice it; their tops rough by this
	// much (m), in hummocks from this size (km) down
	constexpr double FLOW_SIZE_KM = 5.0;
	constexpr double FLOW_LEVELS = 3.0;
	constexpr double FLOW_FRONT_M = 5.0;
	constexpr double FLOW_FRONT_EDGE = 0.05;
	constexpr double FLOW_FRONT_SHOW_KM = 0.06;
	constexpr double FLOW_ROUGH_M = 20.0;
	constexpr double FLOW_ROUGH_KM = 2.0;
	// 2 km down to about 125 m on old lava, and on young lava on down to about 16 m: tumuli and pressure ridges
	constexpr int FLOW_ROUGH_OCTAVES_OLD = 5, FLOW_ROUGH_OCTAVES_YOUNG = 8;
	// Young lava is rough at every size (Hawaii, Idaho's Craters of the Moon): slabs, tumuli and blocks. Old lava plains
	// on airless worlds are ground to dust by billions of years of small impacts (the Moon's maria), and stay as gentle as
	// before. Young lava ground keeps up to these shares of the base relief, the small relief and its own relief, and the
	// GPU tiles draw this much of their landforms as cracked slabs (up to; rock gets ROCK_TILE_BLOCKS of them).
	constexpr double YOUNG_LAVA_BASE = 1.0, YOUNG_LAVA_KEEP = 0.8, YOUNG_LAVA_OWN = 0.2;
	constexpr double LAVA_TILE_BLOCKS = 0.8, ROCK_TILE_BLOCKS = 0.5;
	// Base relief (BaseRelief): all solid ground is rough at every size, as real ground is: a fractal this high (m) at
	// features BASE_RELIEF_KM across, each halving of the size a power of two lower, down to the samples. Only liquid is
	// flat. Young ground (plains, frozen seas, worn or resurfaced worlds) is as the Moon's maria, old battered crust as
	// its highlands (LRO, Rosenburg 2011): median slopes over 17 m of 2.0 and 7.5 degrees, Hurst exponents 0.76 and 0.95,
	// so the highlands stay rough at every size while the maria smooth out. Set so the bend over 30 m, 300 m and 3 km
	// is about 1.9, 1.1 and 0.6 degrees on young ground and 3.8, 3.4 and 3.0 on old.
	constexpr double BASE_RELIEF_M = 60.0;
	constexpr double BASE_RELIEF_KM = 10.0;
	constexpr double BASE_RELIEF_ROUGH = 0.76;
	constexpr double OLD_RELIEF_M = 360.0;
	constexpr double OLD_RELIEF_ROUGH = 0.95;
	constexpr int BASE_RELIEF_OCTAVES = 12; // 10 km down to about 5 m
	// crust counts as old by its craters: none below this cratering, all of it this much higher (Moon and Mercury
	// are at about 1)
	constexpr double OLD_FROM_CRATERING = 0.2, OLD_CRATERING_WIDTH = 0.6;
	constexpr double BASE_RELIEF_PLAINS = 1.0; // the share of young ground's relief flood plains and frozen seas keep
	constexpr double BASE_RELIEF_MAPPED = 0.3; // at most, where an active shell or grooves have maps of their own
	// Small bodies get rounder as they grow, their own gravity pulling them in: an asteroid's lumps shrink as
	// 1 / (1 + (radius / this)^2), so about 40 % of the radius under 50 km (Amalthea, Phobos), a quarter at 150 km
	// and about 15 % at 250 km (Proteus 10 %, Vesta 15 %)
	constexpr double ASTEROID_ROUNDING_KM = 150.0;
	constexpr double CRUST_ROOM = 1000.0; // metres the maps of an active shell's ground may reach from its level
	// points the levels are set from, spread evenly over the sphere
	constexpr int LEVEL_SAMPLES = 4096;
	// their band limit: features finer than they can see are left out
	constexpr double LEVEL_MAX_FREQUENCY = 9.0;
	// the grid of the ground that sea level, rain and drainage are worked out on, and its band limit
	constexpr int GROUND_WIDTH = 540;
	constexpr double GROUND_MAX_FREQUENCY = 43.0;
	// how far the fixed shape of a water edge moves it, in the lake field's units
	constexpr double WATER_EDGE = 0.14;
	// what a metre of depth is worth in the altitude that tells water from land, at a lake's or river's edge
	constexpr double LAKE_DEPTH = 400.0;
	constexpr double RIVER_BANK = 0.2;
	constexpr double RIVER_DEEPEST = 30.0;
	constexpr double RIVER_SLOPE = 0.12;
	constexpr double RIVER_LEVEE = 300.0; // metres, at least, from a bank to ground lower than the river
	thread_local bool t_findingLakeLevel = false;

	// Dunes come from heightmaps of a simulation (tools/dune-tiles), which join themselves
	constexpr double DUNE_CELL_KM = 0.015; // the tile's cells, on the ground
	constexpr double DUNE_SMALL = 0.25; // the tile again, this much smaller and lower, in the flats
	constexpr double DUNE_SAMPLES = 1.5; // a patch reads the copy of the tile with cells this many of its samples wide

	double Soft(const double x, const double low, const double width)
	{
		const double t = std::clamp((x - low) / std::max(width, 1e-9), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	vector3d EvenPoint(const int i, const int count)
	{
		const double y = 1.0 - (2.0 * i + 1.0) / count;
		const double r = std::sqrt(std::max(0.0, 1.0 - y * y));
		const double angle = i * M_PI * (3.0 - std::sqrt(5.0));
		return vector3d(r * std::cos(angle), y, r * std::sin(angle));
	}

	vector3d GridPoint(const int r, const int c, const int w, const int h)
	{
		const double lat = M_PI / 2.0 - (r + 0.5) / h * M_PI;
		const double lon = (c + 0.5) / w * 2.0 * M_PI - M_PI;
		return vector3d(std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon));
	}

	// the value a share of the samples lies above
	double LevelAbove(std::vector<double> values, const double share)
	{
		if (share <= 0.0)
			return NONE_HIGH;
		std::sort(values.begin(), values.end());
		const size_t i = size_t(std::clamp((1.0 - share) * double(values.size()), 0.0, double(values.size() - 1)));
		return values[i];
	}

	// the value a share of the whole area lies above
	double WeightedLevelAbove(const std::vector<float> &values, const std::vector<float> &areas, const double share)
	{
		if (share <= 0.0)
			return NONE_HIGH;
		std::vector<int> order(values.size());
		for (size_t i = 0; i < order.size(); i++)
			order[i] = int(i);
		std::sort(order.begin(), order.end(), [&](const int a, const int b) { return values[a] > values[b]; });
		double total = 0.0;
		for (const float a : areas)
			total += a;
		double sum = 0.0;
		for (const int i : order) {
			sum += areas[i];
			if (sum >= share * total)
				return values[i];
		}
		return order.empty() ? NONE_HIGH : values[order.back()];
	}

	// the share of existing small relief that stays under a material
	double KeepOf(const M::Layer layer)
	{
		switch (layer) {
		case M::CRACKED_MUD: return 0.0;
		// (an Io-like world's sulphur plains and a lava world's crust aren't flat; molten lava settles level anyway)
		case M::LAVA:
		case M::SALT: return 0.3;
		case M::ICE: return 0.1;
		case M::LAVA_FLOW: return 0.2;
		case M::DUST:
		case M::SAND:
		case M::SNOW: return 0.3;
		case M::BASALT:
		case M::ASH: return 0.5;
		case M::SOIL:
		case M::GRASS:
		case M::FOREST:
		case M::FOREST_FAR:
		case M::TUNDRA: return 0.7;
		case M::REGOLITH: return 0.8;
		default: return 1.0;
		}
	}

	// the share of the base relief (BaseRelief) a world's ground keeps: rough rock, softer where something lies on it
	double BaseOf(const M::Layer layer)
	{
		switch (layer) {
		case M::ROCK:
		case M::METAL: return 1.2;
		case M::LAVA:
		case M::SALT:
		case M::CRACKED_MUD:
		case M::SAND: return 0.3;
		case M::SNOW: return 0.4;
		case M::ICE:
		case M::BASALT:
		case M::ASH: return 0.6;
		case M::SOIL:
		case M::GRASS:
		case M::FOREST:
		case M::FOREST_FAR:
		case M::TUNDRA: return 0.7;
		case M::DUST:
		case M::LAVA_FLOW: return 0.8;
		default: return 1.0; // regolith, gravel, dirty ice
		}
	}

	vector3d Times(const vector3d &a, const vector3d &b)
	{
		return vector3d(a.x * b.x, a.y * b.y, a.z * b.z);
	}
} // namespace

TerrainPhysical::TerrainPhysical(const SystemBody *body) :
	Terrain(body),
	m_core(PlanetPhysics::CoreOf(body)),
	m_inputs(PlanetPhysics::SurfaceInputsOf(body)),
	m_interior(PlanetPhysics::MakeInterior(m_core)),
	m_state(PlanetPhysics::MakeSurface(m_core, m_inputs, m_interior)),
	m_noiseSeed(m_core.seed & 0xfffffffu),
	m_reliefLimit(std::max(m_interior.reliefLimit, 1.0))
{
	const auto started = std::chrono::steady_clock::now();
	// this generator carves its own valleys
	m_erosion.reset();
	PlanetPhysics::WearCraters(m_interior, m_inputs, m_state);

	{
		// Europa's cracks always where a shell is ice all over; where it's ice in parts only, as caps on a warm, wet world,
		// only on some worlds. Drawn apart from all else, so the seed's other draws stay as they were.
		Random cracks((m_core.seed ^ 0xC4ACu) & 0x7fffffffu);
		m_iceCapCracks = cracks.Double() < ICE_CAP_CRACKS_SHARE;
	}

	if (m_core.asteroid) {
		// the potato shapes of the classic asteroids; all that's on them comes from here
		const double rounding = m_planetRadius / 1000.0 / ASTEROID_ROUNDING_KM;
		m_asteroidRelief = m_maxHeightInMeters / (1.0 + rounding * rounding);
		Random shape((m_core.seed ^ 0x90F7u) & 0x7fffffffu);
		const double lobeWidth = shape.Double(1.2, 2.5) * m_planetRadius;
		SetFracDef(0, m_asteroidRelief, lobeWidth, lobeWidth / 8.0);
		SetFracDef(1, shape.Double(0.0, TerrainAsteroid::MAX_STRETCH) * m_asteroidRelief, m_planetRadius);
		m_quatDefs[1].SetAxisAngle(shape.Double(0.0, 2.0 * M_PI), TerrainNoise::GetVector3Random(shape));
	}

	Random rng(m_core.seed ^ 0x75u);
	for (int i = 0; i < UNIT_COUNT; i++) {
		const double y = 2.0 * rng.Double() - 1.0;
		const double angle = 2.0 * M_PI * rng.Double();
		const double r = std::sqrt(std::max(0.0, 1.0 - y * y));
		m_unitSeeds[i] = vector3d(r * std::cos(angle), y, r * std::sin(angle));
		m_unitClass[i] = rng.Double();
	}
	m_hemispheres = m_interior.hemispheres;
	if (m_hemispheres) {
		const double y = 2.0 * rng.Double() - 1.0;
		const double angle = 2.0 * M_PI * rng.Double();
		const double r = std::sqrt(std::max(0.0, 1.0 - y * y));
		m_hemisphereAxis = vector3d(r * std::cos(angle), y, r * std::sin(angle));
	}

	m_grooved = m_interior.iceCrust && m_interior.past && m_core.radius > 0.2 && !m_state.weather;
	if (m_grooved)
		BuildGrooves();

	m_craters.Build(m_core, m_interior, m_state, m_noiseSeed, m_grooved);
	if (m_interior.plates)
		m_plates.Build(m_core.seed);
	m_plainsAreBasalt = m_interior.plains > 0.02 && !m_state.sulphur && m_state.wear < 0.2;
	m_volcanoes.BuildConstructs(m_core, m_interior, m_noiseSeed, m_reliefLimit);
	m_lines.Build(m_core, m_interior, m_noiseSeed, m_grooved);
	BuildLevels();
	BuildTones();
	BuildMaterials();
	BuildBorders();
	BuildSandSeas();
	BuildShares();

	if (m_state.sea > 0.0)
		m_surfaceEffects |= EFFECT_WATER;
	if (m_slots[M::LAVA] >= 0)
		m_surfaceEffects |= EFFECT_LAVA;
	m_sealevel = m_state.sea;
	m_buildMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

void TerrainPhysical::BuildMaterials()
{
	const bool cryo = m_interior.iceCrust;
	const bool airless = !m_state.weather;
	// the materials this body can show
	m_materials.Add(m_state.ground);
	if (m_iceLevel < NONE_HIGH || m_state.frozen > 0.0 || m_brightLevel < NONE_HIGH)
		m_materials.Add(M::ICE);
	if (m_iceLevel < NONE_HIGH || (cryo && airless))
		m_materials.Add(M::SNOW);
	if (m_grooved) {
		m_materials.Add(M::ICE);
		m_materials.Add(M::DIRTY_ICE);
	}
	if (cryo) {
		if (m_darkLevel < NONE_HIGH)
			m_materials.Add(M::DIRTY_ICE);
		if (airless && m_state.ground == M::DIRTY_ICE) {
			m_materials.Add(M::ICE);
			m_materials.Add(M::GRAVEL);
		}
		if (m_state.weather)
			m_materials.Add(M::SAND);
	} else {
		if (m_plainsAreBasalt)
			m_materials.Add(M::BASALT);
		if (m_interior.lid)
			m_materials.Add(M::ROCK);
		if (airless) {
			m_materials.Add(M::GRAVEL);
			m_materials.Add(M::DUST);
		} else if (m_state.wetness > 0.0) {
			m_materials.Add(M::SOIL);
			m_materials.Add(M::GRAVEL);
			m_materials.Add(M::SAND);
			if (!m_hydro.dry.Empty()) {
				m_materials.Add(M::SALT);
				m_materials.Add(M::CRACKED_MUD);
			}
			if (m_inputs.life >= 0.02)
				m_materials.Add(M::TUNDRA);
			if (m_inputs.life >= 0.3) {
				m_materials.Add(M::FOREST);
				m_materials.Add(M::GRASS);
			}
		} else if (m_core.temperature > 450.0) {
			m_materials.Add(M::DUST);
		} else {
			m_materials.Add(M::SAND);
			if (m_interior.fossil || m_core.temperature < 400.0) {
				m_materials.Add(M::SALT);
				m_materials.Add(M::CRACKED_MUD);
			}
		}
	}
	if (!cryo && (!m_volcanoes.Vents().empty() || !m_volcanoes.Paterae().empty() || m_moltenLevel < NONE_HIGH)) {
		bool lava = m_moltenLevel < NONE_HIGH || !m_volcanoes.Lakes().empty();
		for (const TerrainVolcanoes::Vent &vent : m_volcanoes.Vents())
			lava = lava || vent.erupting;
		if (lava || m_state.glow > 0.3)
			m_materials.Add(M::LAVA);
		m_materials.Add(M::LAVA_FLOW);
		m_materials.Add(M::BASALT);
		if (m_state.active)
			m_materials.Add(M::ASH);
	} else if (cryo && m_lavaLevel < NONE_HIGH) {
		m_materials.Add(M::ICE);
		m_materials.Add(M::SNOW);
	}
	if (!m_volcanoes.Blocks().empty())
		m_materials.Add(M::ROCK);
	if (m_lines.IsActiveShell())
		m_materials.Add(M::SALT);
	if (m_core.asteroid) {
		m_materials.Add(M::GRAVEL);
		m_materials.Add(M::DUST);
		if (m_interior.porous)
			m_materials.Add(M::ICE);
		else
			m_materials.Add(M::ROCK);
		if (m_inputs.metallicity > 0.7 && !m_interior.porous)
			m_materials.Add(M::METAL);
	}
	// bare steep ground is the crust's material; a sea has beaches
	m_materials.SetSteep(cryo || m_interior.porous ? M::ICE : M::ROCK);
	if (m_state.sea > 0.0)
		m_materials.Add(M::SAND);
	for (int i = 0; i < M::LAYER_COUNT; i++)
		m_slots[i] = m_materials.SlotOf(M::Layer(i));
}

void TerrainPhysical::BuildLevels()
{
	// what counts as steep at the scale of the colours' slope (COLOUR_SLOPE_SCALE, 1/512 of the radius)
	const double sampleKm = std::max(m_core.radius * 6378.135 / 512.0, 0.3);
	m_steepSlope = 0.7 * std::pow(sampleKm / 0.3, -0.35);

	if (m_interior.plains > 0.02) {
		// flood plains fill the lows, up to the level that covers their share of the body
		TerrainNoise::ScopedBandLimit bandLimit(LEVEL_MAX_FREQUENCY);
		std::vector<double> before(LEVEL_SAMPLES);
		for (int i = 0; i < LEVEL_SAMPLES; i++) {
			const Ground g = GroundAt(EvenPoint(i, LEVEL_SAMPLES));
			before[i] = g.large + 0.5 * g.craters.large + 0.7 * std::min(m_reliefLimit, 13000.0) * g.warp;
		}
		m_floodLevel = LevelAbove(before, 1.0 - m_interior.plains);
	}

	// the ground on a grid: sea level, rain and drainage are all worked out on this
	const int W = GROUND_WIDTH, H = W / 2;
	TerrainHydro::Field ground;
	ground.Resize(W, H);
	std::vector<float> areas(size_t(W) * H);
	ParallelRows(H, [&](const int r) {
		TerrainNoise::ScopedBandLimit bandLimit(GROUND_MAX_FREQUENCY);
		const double area = std::cos(M_PI / 2.0 - (r + 0.5) / H * M_PI);
		for (int c = 0; c < W; c++) {
			ground.At(r, c) = float(GroundAt(GridPoint(r, c, W, H)).base);
			areas[size_t(r) * W + c] = float(area);
		}
	});
	ground.FindRange();

	const double seaShare = std::max(m_state.sea, m_state.frozen);
	if (seaShare > 0.0)
		m_seaLevel = WeightedLevelAbove(ground.values, areas, std::max(1.0 - seaShare, 0.001));

	m_volcanoes.BuildVents(m_core, m_inputs, m_interior, m_state, m_reliefLimit, m_seaLevel, m_plates.Active() ? &m_plates : nullptr,
		[&ground](const vector3d &p) { return ground.Linear(TerrainHydro::PlaceOf(p)); });

	m_hydro.BuildSlope(ground, m_planetRadius);
	const bool asteroid = m_core.asteroid;
	const bool wet = m_state.rain > 0.02 && m_state.sea > 0.0 && m_state.weather && !asteroid;
	const bool fossil = !wet && m_state.weather && m_interior.fossil && !m_interior.iceCrust && !asteroid && m_core.temperature < 400.0;
	if (wet || fossil) {
		TerrainHydro::Params params;
		params.noiseSeed = m_noiseSeed;
		params.radiusMetres = m_planetRadius;
		params.reliefLimit = m_reliefLimit;
		params.seaLevel = m_seaLevel;
		params.seaShare = m_state.sea;
		params.temperature = m_core.temperature;
		params.airDensity = m_inputs.airDensity;
		params.wet = wet;
		if (wet)
			m_hydro.BuildMoisture(params, ground, [this](const vector3d &p) { return Warp(p); });
		// rivers find their way around volcanoes, not across them
		TerrainHydro::Field routed = ground;
		const std::vector<TerrainVolcanoes::Vent> &vents = m_volcanoes.Vents();
		if (!vents.empty()) {
			const double sea = m_seaLevel;
			const float rise = float(0.15 * m_reliefLimit);
			// (at least a cell of the grid, or the map wouldn't see a small volcano)
			const double cosCell = std::cos(2.0 * M_PI / W);
			ParallelRows(H, [&](const int r) {
				for (int c = 0; c < W; c++) {
					const vector3d p = GridPoint(r, c, W, H);
					for (const TerrainVolcanoes::Vent &vent : vents) {
						if (p.Dot(vent.centre) >= std::min(vent.cosReach, cosCell) && ground.At(r, c) >= sea) {
							routed.At(r, c) = ground.At(r, c) + rise;
							break;
						}
					}
				}
			});
			routed.FindRange();
		}
		m_hydro.BuildDrainage(params, routed);
	}

	// the sea floods the land with its coasts, so its level is set on that
	if (m_state.sea > 0.0) {
		std::vector<float> shore(size_t(W) * H);
		ParallelRows(H, [&](const int r) {
			TerrainNoise::ScopedBandLimit bandLimit(GROUND_MAX_FREQUENCY);
			for (int c = 0; c < W; c++) {
				const vector3d p = GridPoint(r, c, W, H);
				Point point;
				point.g.warp = Warp(p);
				point.g.base = ground.At(r, c);
				point.place = TerrainHydro::PlaceOf(p);
				point.temperature = Temperature(p, point.g.warp);
				point.rain = m_hydro.HasMoisture() ? m_hydro.moisture.Smooth(point.place) : 0.3;
				shore[size_t(r) * W + c] = float(point.g.base + Coast(p, point, point.g.base));
			}
		});
		m_seaLevel = WeightedLevelAbove(shore, areas, std::max(1.0 - m_state.sea, 0.001));
		m_hydro.SetSeaLevel(m_seaLevel);
	}

	// land is above 0; the sea floor lies at 0 on water worlds
	m_datum = m_state.sea > 0.0 ? m_seaLevel : double(ground.low) - 0.15 * m_reliefLimit - m_lines.Deepest() - (m_lines.IsActiveShell() || m_grooved ? CRUST_ROOM : 0.0);

	if (wet) {
		const double sea = m_seaLevel;
		m_hydro.SettleLevels([this, sea](const vector3d &p) {
			// the ground as Evaluate makes it, without the valley network, which stays out of a river's way
			TerrainNoise::ScopedBandLimit limit(m_planetRadius / 16000.0);
			Point point;
			point.g = GroundAt(p);
			point.place = TerrainHydro::PlaceOf(p);
			point.temperature = Temperature(p, point.g.warp);
			point.rain = m_hydro.moisture.Smooth(point.place);
			const Ground &g = point.g;
			const double shore = g.base + Coast(p, point, g.base);
			return std::max(shore - m_hydro.valley.Smooth(point.place) + (g.craters.small + g.small) * KeepOf(m_state.ground) + OwnRelief(p, g) +
					LavaPlainsRelief(p, g) + BaseShare(g) * BaseRelief(p, OldShare(g)),
				sea);
		});
	}

	BuildLavaFields(ground, areas);
	BuildPlumes();

	// the valley network, where water runs now or once did
	m_valleyAmount = wet ? 1.0 : (fossil ? 0.3 : 0.0);
	if (m_valleyAmount > 0.0) {
		m_ground = ground;
		const double seaOrLow = m_seaLevel > NONE_LOW ? m_seaLevel : WeightedLevelAbove(ground.values, areas, 0.75);
		m_valleySea = seaOrLow;
		const double radiusKm = m_core.radius * 6378.135;
		m_valleys.Build(
			m_noiseSeed, radiusKm, m_reliefLimit, seaOrLow, &m_hydro,
			[this, radiusKm](const vector3d &p, const double spacingKm) {
				// from the map and detail fixed in space, band-limited by the level alone, so a point
				// flows the same way in every view
				TerrainNoise::ScopedBandLimit limit(4.0 * radiusKm / spacingKm);
				return m_ground.Linear(TerrainHydro::PlaceOf(p)) + 0.08 * m_reliefLimit * Fbm(p, 18.0, 5, m_noiseSeed + 41, 0.55) +
					400.0 * Fbm(p, radiusKm / 200.0, 8, m_noiseSeed + 191, 0.62);
			},
			[this, wet](const vector3d &p) {
				return wet ? std::clamp((m_hydro.moisture.Linear(TerrainHydro::PlaceOf(p)) - 0.05) / 0.25, 0.0, 1.0) : 1.0;
			});
	}

	// lows of a dry world, for sand, mud and salt, and the dune belts of a hazy ice world
	if (m_state.weather && m_state.wetness <= 0.0) {
		std::vector<float> lowness(ground.values.size());
		for (size_t i = 0; i < lowness.size(); i++) {
			const bool land = m_seaLevel <= NONE_LOW || ground.values[i] >= m_seaLevel;
			lowness[i] = land ? -ground.values[i] : -1e20f;
		}
		const double land = 1.0 - m_state.sea;
		m_sandLevel = WeightedLevelAbove(lowness, areas, 0.14 * land);
		m_mudLevel = WeightedLevelAbove(lowness, areas, 0.035 * land);
		m_saltLevel = WeightedLevelAbove(lowness, areas, 0.006 * land);
	}

	// rubble gathers in a small body's lows
	if (m_core.asteroid) {
		std::vector<float> lowness(ground.values.size());
		for (size_t i = 0; i < lowness.size(); i++)
			lowness[i] = -ground.values[i];
		m_gravelLevel = WeightedLevelAbove(lowness, areas, 0.2);
	}

	TerrainNoise::ScopedBandLimit bandLimit(LEVEL_MAX_FREQUENCY);
	std::vector<double> iceField(LEVEL_SAMPLES), darkField(LEVEL_SAMPLES);
	std::vector<Ground> grounds(LEVEL_SAMPLES);
	for (int i = 0; i < LEVEL_SAMPLES; i++)
		grounds[i] = GroundAt(EvenPoint(i, LEVEL_SAMPLES));

	const bool cryo = m_interior.iceCrust;
	if (m_state.ice > 0.0) {
		for (int i = 0; i < LEVEL_SAMPLES; i++) {
			const vector3d p = EvenPoint(i, LEVEL_SAMPLES);
			iceField[i] = cryo ? CrustIceField(p, grounds[i]) : IceField(p, grounds[i]);
		}
		m_iceLevel = LevelAbove(iceField, m_state.ice);
		const double snowShare = m_state.weather ? 0.15 + 0.6 * m_state.air : 0.0;
		m_iceCoreLevel = LevelAbove(iceField, m_state.ice * (1.0 - snowShare));
	}

	if (cryo && m_state.weather) {
		std::vector<double> belts(LEVEL_SAMPLES);
		for (int i = 0; i < LEVEL_SAMPLES; i++)
			belts[i] = DuneField(EvenPoint(i, LEVEL_SAMPLES), grounds[i]);
		m_beltLevel = LevelAbove(belts, 0.25);
	}

	if (cryo && !m_state.weather) {
		// dark and bright regions of an ice moon; where the data doesn't decide, the seed picks a style
		Random rng(m_core.seed ^ 0xD1u);
		const MoonStyle styles[] = { MoonStyle::MOTTLED, MoonStyle::BELTS, MoonStyle::PATCHWORK, MoonStyle::POLAR, MoonStyle::STREAKS };
		m_moonStyle = m_interior.past ? MoonStyle::PATCHWORK : styles[rng.Int32(5)];
		const double draw = rng.Double();
		const double brightShare = m_hemispheres ? 0.42 : 0.14 * draw * draw;
		const double darkDraw = rng.Double();
		double darkShare = m_hemispheres ? 0.38 : 0.08 + 0.28 * darkDraw;
		if (m_interior.past)
			darkShare = std::max(darkShare, 0.34);
		m_northFrost = 1.3 + 0.3 * rng.Double();
		m_southFrost = 1.3 + 0.3 * rng.Double();
		for (int i = 0; i < LEVEL_SAMPLES; i++)
			darkField[i] = DarkField(EvenPoint(i, LEVEL_SAMPLES), grounds[i].warp);
		if (m_grooved) {
			std::vector<double> darkBase(LEVEL_SAMPLES);
			for (int i = 0; i < LEVEL_SAMPLES; i++)
				darkBase[i] = GrooveDarkBase(EvenPoint(i, LEVEL_SAMPLES));
			m_grooveDarkLevel = LevelAbove(darkBase, 0.36);
		} else if (m_state.ground == M::ICE && m_interior.iceActivity < 0.05 && m_inputs.ice < 0.95)
			m_darkLevel = LevelAbove(darkField, std::min(darkShare, std::max(0.0, 1.0 - std::min(m_inputs.ice, 1.0))));
		if (!m_grooved && m_state.ground == M::DIRTY_ICE && m_inputs.ice <= 0.0)
			m_brightLevel = LevelAbove(darkField, brightShare);
	}

	// room for what the coarse grid can't see
	m_maxHeightInMeters = std::min(std::max(100.0, double(ground.high) - m_datum + 0.25 * m_reliefLimit + m_volcanoes.Tallest() + m_lines.Tallest() + (m_lines.IsActiveShell() || m_grooved ? CRUST_ROOM : 0.0)), m_planetRadius);
	m_maxHeight = m_maxHeightInMeters / m_planetRadius;
	m_invMaxHeight = 1.0 / m_maxHeight;
}

void TerrainPhysical::BuildTones()
{
	Random rng(m_core.seed ^ 0xBEEu);
	m_tint = vector3d(1.0 + (rng.Double() * 0.12 - 0.06), 1.0 + (rng.Double() * 0.12 - 0.06), 1.0 + (rng.Double() * 0.12 - 0.06));
	m_bright = 0.88 + 0.24 * rng.Double();
	const double metal = std::min(m_inputs.metallicity, 1.0);
	const double iron = metal * Soft(std::min(m_inputs.oxidizing, 1.0), 0.2, 0.5);
	const vector3d rock = vector3d(0.42, 0.39, 0.35) * (1.25 - 0.55 * metal) + vector3d(0.16, 0.05, -0.04) * iron;

	// vegetation by the star's colour (PlanetPhysics, so the debug tools can ask for it too)
	m_vegetation = PlanetPhysics::PlantColour(m_core);
	const vector3d veg = m_vegetation;

	auto set = [&](const M::Layer layer, const vector3d &c) { m_tones[layer] = c; };
	set(M::ROCK, rock);
	set(M::GRAVEL, m_state.wetness > 0.0 ? rock * 0.55 + vector3d(0.34, 0.27, 0.18) : rock * 1.1);
	set(M::SAND, vector3d(0.78, 0.64, 0.40) + vector3d(0.05, -0.16, -0.16) * iron);
	set(M::SOIL, vector3d(0.38, 0.28, 0.18) + vector3d(0.06, -0.05, -0.04) * iron);
	set(M::GRASS, veg);
	set(M::FOREST, veg * 0.55);
	set(M::FOREST_FAR, veg * 0.55);
	set(M::TUNDRA, veg * 0.55 + vector3d(0.2, 0.18, 0.13));
	set(M::SNOW, vector3d(0.93, 0.95, 0.98));
	set(M::ICE, vector3d(0.70, 0.82, 0.90));
	set(M::DIRTY_ICE, vector3d(0.33, 0.31, 0.28));
	set(M::BASALT, vector3d(0.12, 0.11, 0.11));
	// a dark cooled crust; the glow is the shader's, from the lava texture's cracks, and a dim average from afar
	set(M::LAVA, vector3d(0.07, 0.045, 0.04));
	set(M::LAVA_FLOW, vector3d(0.09, 0.07, 0.07));
	set(M::ASH, vector3d(0.17, 0.16, 0.16));
	set(M::REGOLITH, rock * 1.05);
	set(M::DUST, vector3d(0.62, 0.44, 0.28) * (0.8 + 0.4 * iron) + vector3d(0.1, -0.03, -0.08) * iron);
	set(M::CRACKED_MUD, vector3d(0.60, 0.50, 0.38));
	set(M::SALT, vector3d(0.88, 0.82, 0.68));
	set(M::METAL, vector3d(0.42, 0.40, 0.37));
	m_seaTone = vector3d(0.02, 0.08, 0.20);
	m_shallowTone = vector3d(0.05, 0.34, 0.40);
	m_lakeTone = vector3d(0.05, 0.15, 0.27);
	m_riverTone = vector3d(0.05, 0.19, 0.29);

	if (m_interior.iceCrust) {
		// how much dark dust is mixed into the ice: none on active, freshly
		// resurfaced shells, a lot on old quiet ones, set by the seed
		const double draw = rng.Double();
		// (a large active shell has had time to gather some; a small one like Enceladus is kept clean by its plumes)
		m_iceDirt = m_interior.iceActivity >= 0.05 && m_core.radius >= 0.1 ? 0.4 : 0.0;
		const double grime = m_interior.iceActivity >= 0.05 ? m_iceDirt : std::min(1.0, std::pow(draw, 0.7) * 0.85);
		if (m_state.ground == M::ICE) {
			set(M::ICE, m_tones[M::ICE] * (1.0 - 0.62 * grime) + vector3d(0.30, 0.28, 0.25) * 0.62 * grime);
			set(M::SNOW, m_tones[M::SNOW] * (1.0 - 0.45 * grime) + vector3d(0.42, 0.40, 0.37) * 0.45 * grime);
		}
		set(M::DIRTY_ICE, vector3d(0.115, 0.108, 0.098) * (1.5 - 0.8 * grime));
		set(M::GRAVEL, vector3d(0.07, 0.066, 0.062) * (1.4 - 0.6 * grime));
		if (m_state.weather) {
			// organic haze settles on everything
			set(M::DIRTY_ICE, vector3d(0.42, 0.31, 0.18));
			set(M::SAND, vector3d(0.17, 0.115, 0.07));
			set(M::GRAVEL, vector3d(0.33, 0.24, 0.14));
			set(M::ICE, vector3d(0.50, 0.41, 0.27));
			set(M::SNOW, vector3d(0.55, 0.46, 0.32));
		}
		set(M::SALT, vector3d(0.42, 0.22, 0.13));
		if (m_grooved) {
			// warm pale ground, and grey-purple old ground
			set(M::ICE, vector3d(0.70, 0.66, 0.55));
			set(M::DIRTY_ICE, vector3d(0.34, 0.30, 0.31));
			set(M::SNOW, vector3d(0.95, 0.94, 0.91));
		}
	}
	// What stains this body's ground comes from what it's made of: a usual stain, a darker one and a paler one. Drawn
	// apart from all else here, so the seed's other draws stay as they were.
	{
		Random dirt((m_core.seed ^ 0xD17Au) & 0x7fffffffu);
		const vector3d(*hues)[3] = nullptr;
		static const vector3d ICE_HUES[3] = { vector3d(1.22, 0.98, 0.72), vector3d(1.15, 0.95, 0.82), vector3d(0.92, 1.0, 1.12) };
		static const vector3d SULPHUR_HUES[3] = { vector3d(1.35, 1.0, 0.35), vector3d(1.5, 0.8, 0.45), vector3d(1.1, 1.02, 0.8) };
		static const vector3d IRON_HUES[3] = { vector3d(1.5, 0.82, 0.5), vector3d(1.2, 0.92, 0.8), vector3d(1.15, 1.0, 0.8) };
		static const vector3d ROCK_HUES[3] = { vector3d(1.2, 0.98, 0.78), vector3d(0.95, 1.0, 1.1), vector3d(1.08, 1.0, 0.9) };
		hues = m_interior.iceCrust ? &ICE_HUES : (m_state.sulphur ? &SULPHUR_HUES : (iron > 0.25 ? &IRON_HUES : &ROCK_HUES));
		static const double LIGHT[3] = { 1.0, 0.62, 1.25 };
		for (int i = 0; i < 3; i++) {
			m_dirtHues[i] = vector3d((*hues)[i].x * (0.9 + 0.2 * dirt.Double()), (*hues)[i].y * (0.9 + 0.2 * dirt.Double()), (*hues)[i].z * (0.9 + 0.2 * dirt.Double()));
			m_dirtLight[i] = LIGHT[i];
		}
		m_dirtStrength = 0.6 + 0.2 * dirt.Double();
		// (what's already been judged on its own, or is mostly plants and sea, needs less)
		if (m_lines.IsActiveShell())
			m_dirtStrength *= m_iceDirt > 0.0 ? 0.45 : 0.2;
		else if (m_interior.plates)
			m_dirtStrength *= 0.7;
	}
	if (!m_state.weather && !m_interior.iceCrust) {
		// bare rock with no air is dark (the plan's highland albedo, 0.2 to 0.3)
		set(M::ROCK, m_tones[M::ROCK] * 0.6);
		set(M::GRAVEL, m_tones[M::GRAVEL] * 0.6);
		set(M::REGOLITH, m_tones[M::REGOLITH] * 0.6);
		// with no air to oxidise it, dust is the ground's own powder; old plains are dark, not black
		set(M::DUST, m_tones[M::REGOLITH] * 0.9);
		set(M::BASALT, m_tones[M::BASALT] * 0.9 + m_tones[M::REGOLITH] * 0.1);
	}
	if (m_interior.porous) {
		// pale walls and rims, and dark reddish dust on the floors of the holes, as on Hyperion
		set(M::ICE, vector3d(0.60, 0.56, 0.50));
		set(M::DIRTY_ICE, vector3d(0.36, 0.32, 0.27));
		set(M::GRAVEL, vector3d(0.30, 0.27, 0.23));
		set(M::DUST, vector3d(0.10, 0.075, 0.055));
	}
	if (m_state.sulphur)
		set(M::SALT, vector3d(0.90, 0.78, 0.18));
	if (m_state.seaState == std::string("liquid hydrocarbon")) {
		m_seaTone = vector3d(0.05, 0.04, 0.03);
		m_shallowTone = vector3d(0.05, 0.05, 0.04);
		m_lakeTone = m_seaTone;
		m_riverTone = m_seaTone;
	}

	for (vector3d &c : m_tones)
		c = Times(c, m_tint) * m_bright;
	for (vector3d *c : { &m_seaTone, &m_shallowTone, &m_lakeTone, &m_riverTone })
		*c = Times(*c, m_tint) * m_bright;
}

double TerrainPhysical::Warp(const vector3d &p) const
{
	return Fbm(p, 2.2, 3, m_noiseSeed + 5);
}

double TerrainPhysical::Lag(const vector3d &p) const
{
	return Fbm(p, 3.1, 3, m_noiseSeed + 84);
}

double TerrainPhysical::WaterEdge(const vector3d &p) const
{
	return WATER_EDGE * std::clamp(Fbm(p, 57.0, 10, m_noiseSeed + 211, 0.6) / 0.35, -1.0, 1.0);
}

double TerrainPhysical::Temperature(const vector3d &p, const double warp) const
{
	const double latitude = std::asin(std::clamp(p.y, -1.0, 1.0));
	const double wavy = std::abs(latitude) + 0.05 * warp;
	const double te = m_state.equatorTemperature, tp = m_state.poleTemperature;
	return te - (te - tp) * std::clamp(wavy / (M_PI / 2.0), 0.0, 1.1);
}

TerrainPhysical::Ground TerrainPhysical::GroundAt(const vector3d &p) const
{
	Ground g;
	g.warp = Warp(p);
	const double env = m_reliefLimit;
	double h = 0.28 * env * Fbm(p, 1.3, 4, m_noiseSeed + 1);
	if (m_core.asteroid)
		h += m_asteroidRelief * TerrainAsteroid::shape(m_fracdef[0], m_fracdef[1], m_quatDefs[1], p);

	if (m_plates.Active()) {
		const TerrainPlates::Bands bands = m_plates.At(p, g.warp);
		// coasts follow their own shape, not the plates
		double land = std::clamp((bands.continent - 0.42) * 2.2 + 1.7 * Fbm(p, 1.2, 6, m_noiseSeed + 9, 0.6) + 0.3, 0.0, 1.0);
		land = land * (1.6 - 0.6 * land);
		h += env * (-0.42 + 0.62 * land);
		const double rough = std::abs(Fbm(p, 6.0, 4, m_noiseSeed + 21));
		const double gaps = std::clamp(1.2 + 3.0 * Fbm(p, 3.5, 3, m_noiseSeed + 23), 0.0, 1.0);
		const double ragged = 0.3 + 1.5 * rough;
		h += 0.7 * env * bands.collision * gaps * ragged * (0.25 + 0.75 * land);
		h += 0.42 * env * bands.coastRange * gaps * ragged;
		if (bands.arc > 0.0 || bands.islandArc > 0.0) {
			const double spots = std::clamp(0.5 + 3.5 * Fbm(p, 11.0, 3, m_noiseSeed + 24), 0.0, 1.0);
			h += 0.32 * env * bands.arc * spots * gaps;
			h += 0.5 * env * bands.islandArc * spots;
		}
		h -= 0.32 * env * bands.trench;
		h += 0.13 * env * bands.ridge - 0.05 * env * bands.ridgeRift;
		h -= 0.2 * env * bands.rift * gaps * (0.15 + 0.85 * land);
		for (const TerrainPlates::Plateau &plateau : m_plates.Plateaus()) {
			const double cosine = p.Dot(plateau.centre);
			if (cosine < std::cos(std::min(plateau.radius * 1.3, M_PI)))
				continue;
			const double t = std::acos(std::clamp(cosine, -1.0, 1.0)) / plateau.radius + 0.35 * g.warp;
			const double top = std::clamp((1.05 - t) / 0.35, 0.0, 1.0);
			h += plateau.height * env * top * top * (0.2 + 0.8 * land);
		}
		h += env * m_plates.Ranges(p) * (0.45 + 1.1 * rough) * (0.1 + 0.9 * land);
		h -= env * m_plates.Rifts(p) * (0.1 + 0.9 * land);
	}

	if (m_interior.dichotomy) {
		// the highlands and lowlands of a Mars-like world
		const vector3d axis = vector3d(0.2, -0.8, 0.5).Normalized();
		h += 0.25 * env * std::tanh((p.Dot(axis) + 0.35 * g.warp) * 5.0);
	}

	// crustal units: the warped nearest seed, 0 on the edge between two
	const vector3d q = (p + vector3d(0.22, 0.22, 0.22) * g.warp).Normalized();
	int best = 0;
	double first = -2.0, second = -2.0;
	for (int i = 0; i < UNIT_COUNT; i++) {
		const double d = q.Dot(m_unitSeeds[i]);
		if (d > first) {
			second = first;
			first = d;
			best = i;
		} else if (d > second) {
			second = d;
		}
	}
	g.unitClass = m_unitClass[best];
	g.unitEdge = std::clamp((first - second) / 0.04, 0.0, 1.0);
	if (m_interior.lid && g.unitClass > 0.84) {
		g.tessera = g.unitEdge;
		h += 0.12 * env * g.tessera * (1.0 + std::abs(Fbm(p, 12.0, 3, m_noiseSeed + 31)));
	}

	g.craters = m_craters.At(p);
	// the active pole of a small shell is too young for any
	if (m_lines.IsPolar()) {
		{
			TerrainNoise::ScopedBandLimit fullDetail(0.0);
			const double angle = std::acos(std::clamp(p.Dot(m_lines.Pole()), -1.0, 1.0)) + 0.06 * Fbm(p, 5.0, 3, m_noiseSeed + 351);
			g.polarCap = 1.0 - Soft(angle, 0.54, 0.12);
		}
		const double kept = 1.0 - g.polarCap;
		g.craters.large *= kept;
		g.craters.small *= kept;
		g.craters.floor *= kept;
		g.craters.ejecta *= kept;
		g.craters.rays *= kept;
	}

	// old plains flood the lows, with the large craters' rims standing out of them
	if (m_floodLevel > NONE_LOW) {
		const double before = h + 0.5 * g.craters.large + 0.7 * std::min(env, 13000.0) * g.warp;
		g.plains = Soft(m_floodLevel - before, 0.0, std::max(0.04 * env, 40.0));
		h += g.plains * std::max(m_floodLevel - before, 0.0) * 0.9;
	}

	// what the interior built stands on the plains: old shields, and Io's mountain blocks and paterae
	const TerrainVolcanoes::Construct built = m_volcanoes.ConstructAt(p);
	h += built.height;
	g.block = built.block;
	g.patera = built.patera;
	g.pateraLava = built.pateraLava;

	g.large = h;
	g.base = h + g.craters.large;
	g.small = 0.08 * env * Fbm(p, 18.0, 5, m_noiseSeed + 41, 0.55);
	// (a small active shell is smooth with fresh snow, though its old craters still show)
	if (m_lines.IsPolar())
		g.small *= 0.3;
	else if (IsSmallOldIce())
		g.small *= 0.4;
	return g;
}

// How the shore's height differs from the ground: its character varies along the coast and
// has causes. Cold, steep and wet coasts are jagged, with fjords; low, dry or wave-exposed
// ones are smooth. Valleys carved before flooding become inlets, and rivers build deltas.
double TerrainPhysical::Coast(const vector3d &p, const Point &point, const double plainGround) const
{
	const double env = m_reliefLimit;
	const double sea = m_seaLevel;
	const double near = std::exp(-std::abs(plainGround - sea) / (0.28 * env));
	const double shore = std::exp(-std::max(plainGround - sea, 0.0) / (0.05 * env));
	const double cold = 1.0 - Soft(point.temperature, 258.0, 18.0);
	double out = 0.0;
	double character = 0.0, wet = 0.0, high = 0.0;
	if (near > 0.01) {
		character = Fbm(p, 2.6, 3, m_noiseSeed + 111);
		const double rough = Fbm(p, 8.0, 6, m_noiseSeed + 112, 0.62);
		const double ridged = 1.0 - 3.2 * std::abs(Fbm(p, 12.0, 5, m_noiseSeed + 113, 0.6));
		high = Soft(plainGround - sea, 0.06 * env, 0.22 * env);
		wet = Soft(point.rain, 0.08, 0.25);
		const double amount = env * (0.018 + 0.085 * Soft(character, -0.12, 0.3) * wet + 0.10 * cold + 0.07 * high);
		// drowned glacial valleys: deep steep inlets on cold coasts
		const double delta = amount * rough * 2.4 - cold * 0.16 * env * Soft(ridged, 0.45, 0.3);
		out = delta * near;
	}
	if (!m_hydro.drowned.Empty())
		out -= m_hydro.drowned.Smooth(point.place) * shore * (1.0 + 1.2 * cold);
	if (!m_hydro.mouth.Empty())
		out += m_hydro.mouth.Smooth(point.place) * 0.1 * env;

	// finer bays and headlands. They move the shoreline sideways by a limited distance, set by
	// how rugged the coast is, and only near the water, so there are no ponds far from the shore.
	const double broad = std::max(3.0 * m_hydro.slope.Linear(point.place), 0.001);
	const double rugged = std::clamp(Soft(character, -0.12, 0.3) * wet + cold + high, 0.0, 1.0);
	const double reach = m_planetRadius / 512.0 * (0.25 + 0.35 * rugged);
	const double gate = std::exp(-std::abs(sea - (plainGround + out)) / std::max(3.0 * broad * reach, 1.0));
	if (gate > 0.01)
		out += 2.5 * broad * reach * Fbm(p, 512.0, 9, m_noiseSeed + 115, 0.55) * gate;
	return out;
}

double TerrainPhysical::CutValleys(const vector3d &p, Point &point, const double plainGround, const double calm) const
{
	const double above = plainGround - m_valleySea;
	const double H = m_valleyAmount * std::max(above, 0.0) * Soft(above, 400.0, 500.0);
	double fjord = 0.0;
	if (m_state.sea > 0.0)
		fjord = m_valleyAmount * (1.0 - Soft(point.temperature, 272.0, 22.0)) * (1.0 - Soft(above, 0.02 * m_reliefLimit, 0.04 * m_reliefLimit));
	if (H <= 0.0 && fjord <= 0.01)
		return 0.0;
	const double share = m_hydro.HasMoisture() ? std::clamp((m_hydro.moisture.Linear(point.place) - 0.05) / 0.25, 0.0, 1.0) : 1.0;
	const TerrainValleys::Cut cut = m_valleys.At(p, H, fjord, share, calm);
	point.carved = true;
	point.sideEdge = cut.riverEdge;
	point.sideWidth = cut.riverWidth;
	point.quiet = cut.quiet;
	point.smallLakeRatio = cut.lakeRatio;
	point.smallLakeRadius = cut.lakeRadius;
	point.smallLakePlace = cut.lakePlace;
	// (As a share of what they may cut. Inlets don't count: they don't grow with that, so as it
	// came to nothing, all the ground would turn into valley floor at once.)
	point.valley = H > 0.0 ? std::clamp(cut.carved / std::max(0.55 * H, 1.0), 0.0, 1.0) : 0.0;
	point.valleyRoom = H;
	point.g.base -= cut.cut * calm;

	// Peaks along the ridges, sharp where the noise crosses zero, none on the floors. They average out
	// too, so they don't lift the ground above the map. (The noise is bent first, or its lattice
	// shows as rows of bumps; only high ground has peaks.)
	const double high = Soft(H, 1200.0, 1500.0);
	if (high <= 0.0)
		return 0.0;
	const double radiusKm = m_core.radius * 6378.135;
	const double shift = 8.0 / radiusKm;
	const vector3d bent = (p + vector3d(Fbm(p, radiusKm / 40.0, 3, m_noiseSeed + 141, 0.5), Fbm(p, radiusKm / 40.0, 3, m_noiseSeed + 142, 0.5), Fbm(p, radiusKm / 40.0, 3, m_noiseSeed + 143, 0.5)) * shift).Normalized();
	const double ridged = 1.0 - std::abs(Fbm(bent, radiusKm / 10.0, 7, m_noiseSeed + 131, 0.5)) / 0.22;
	// (where life has weathered it, hills are rounder)
	const double weathered = 1.0 - 0.35 * std::min(m_inputs.life, 1.0) * std::min(m_state.rain, 1.0);
	return weathered * 0.1 * H * (std::clamp(ridged, -1.0, 1.0) - 0.15) * std::pow(1.0 - point.valley, 1.5) * high * calm * cut.quiet;
}

double TerrainPhysical::Keep(const Ground &g) const
{
	// (nothing has smoothed an old ice moon, unlike an ice sheet or a frozen sea)
	double ground = m_core.asteroid || (m_interior.iceCrust && !m_state.weather && (!m_lines.IsActiveShell() || m_lines.IsPolar())) ? 1.0 : KeepOf(m_state.ground);
	// young lava keeps more of it
	const double basalt = KeepOf(M::BASALT) + (std::max(KeepOf(M::BASALT), YOUNG_LAVA_KEEP) - KeepOf(M::BASALT)) * LavaYouth();
	if (IsLavaGround())
		ground = std::max(ground, ground + (YOUNG_LAVA_KEEP - ground) * LavaYouth());
	if (!m_plainsAreBasalt || g.plains <= 0.0)
		return ground;
	return ground + (basalt - ground) * g.plains;
}

namespace {
	// A heightmap of dunes: heights in thousandths of a cell, on a grid that joins itself, with a chain of
	// copies, each averaged to half the size. A patch reads the copy that suits its samples, so at every
	// detail the tile keeps its shape and its average, as it fades towards that average from afar.
	struct DuneTile {
		int size = 0;
		int levels = 0;
		double mean = 0.0;
		std::vector<std::vector<uint16_t>> copies;

		bool Load(const char *path, const bool oneByte = false)
		{
			RefCountedPtr<FileSystem::FileData> file = FileSystem::gameDataFiles.ReadFile(path);
			if (!file)
				return false;
			const size_t count = file->GetSize() / (oneByte ? 1 : 2);
			const int side = int(std::lround(std::sqrt(double(count))));
			if (side < 2 || size_t(side) * side != count || (side & (side - 1)) != 0)
				return false;
			const unsigned char *bytes = reinterpret_cast<const unsigned char *>(file->GetData());
			copies.emplace_back(count);
			double sum = 0.0;
			for (size_t i = 0; i < count; i++) {
				copies[0][i] = oneByte ? uint16_t(int(bytes[i]) * 257) : uint16_t(int(bytes[2 * i]) | (int(bytes[2 * i + 1]) << 8));
				sum += copies[0][i];
			}
			for (int n = side; n > 1; n /= 2) {
				const std::vector<uint16_t> &from = copies.back();
				std::vector<uint16_t> to(size_t(n / 2) * (n / 2));
				for (int y = 0; y < n / 2; y++) {
					for (int x = 0; x < n / 2; x++) {
						const uint16_t *a = &from[size_t(2 * y) * n + 2 * x], *b = a + n;
						to[size_t(y) * (n / 2) + x] = uint16_t((int(a[0]) + a[1] + b[0] + b[1] + 2) / 4);
					}
				}
				copies.push_back(std::move(to));
			}
			levels = int(copies.size());
			size = side;
			mean = sum / double(count) * 0.001;
			return true;
		}
		// smooth, and through the grid's points, so the crests stay sharp. x, y: in cells of the tile.
		double Copy(const int k, const double x, const double y) const
		{
			const double scale = 1.0 / double(1 << k);
			const double cx = (x + 0.5) * scale - 0.5, cy = (y + 0.5) * scale - 0.5;
			const double fx = std::floor(cx), fy = std::floor(cy);
			const double tx = cx - fx, ty = cy - fy;
			const int n = size >> k, mask = n - 1;
			const int ix = int(int64_t(fx) & mask), iy = int(int64_t(fy) & mask);
			const auto weights = [](const double t, double w[4]) {
				w[0] = t * (-0.5 + t * (1.0 - 0.5 * t));
				w[1] = 1.0 + t * t * (-2.5 + 1.5 * t);
				w[2] = t * (0.5 + t * (2.0 - 1.5 * t));
				w[3] = t * t * (-0.5 + 0.5 * t);
			};
			double wx[4], wy[4];
			weights(tx, wx);
			weights(ty, wy);
			const std::vector<uint16_t> &heights = copies[k];
			double out = 0.0;
			for (int j = 0; j < 4; j++) {
				const uint16_t *row = &heights[size_t((iy + j - 1) & mask) * n];
				double line = 0.0;
				for (int i = 0; i < 4; i++)
					line += wx[i] * row[(ix + i - 1) & mask];
				out += wy[j] * line;
			}
			return out * 0.001;
		}
		// level: 0 is the tile itself; each one up has cells twice as wide
		double At(const double x, const double y, const double level) const
		{
			if (level >= double(levels - 1))
				return mean;
			const int k = int(level);
			const double t = level - k;
			double out = Copy(k, x, y);
			if (t > 0.01)
				out += t * (Copy(k + 1, x, y) - out);
			return std::max(out, 0.0);
		}
		double LevelFor(const double samplesKm, const double cellKm) const
		{
			return samplesKm > 0.0 ? std::log2(std::max(DUNE_SAMPLES * samplesKm / cellKm, 1.0)) : 0.0;
		}
	};

	const DuneTile *DuneTiles()
	{
		static const std::array<DuneTile, 2> tiles = [] {
			std::array<DuneTile, 2> loaded;
			loaded[0].Load("terrain/dunes_medium.bin");
			loaded[1].Load("terrain/dunes_rich.bin");
			return loaded;
		}();
		return tiles[0].size > 0 && tiles[1].size > 0 ? tiles.data() : nullptr;
	}

	// A tile laid on the body from its three axes, whichever suits a place best, in cells at a scale, turned by an
	// angle so that the wind differs. shift, km: bends what would otherwise be straight repeats.
	double DuneTileAt(const DuneTile &tile, const vector3d &p, const double radiusKm, const vector3d &shift, const double cellKm, const double angle, const double samplesKm)
	{
		const double level = tile.LevelFor(samplesKm, cellKm);
		if (level >= double(tile.levels - 1))
			return tile.mean;
		const vector3d q = p * radiusKm + shift;
		const double c = std::cos(angle), s = std::sin(angle);
		const double weights[3] = { Soft(std::abs(p.x), 0.45, 0.25), Soft(std::abs(p.y), 0.45, 0.25), Soft(std::abs(p.z), 0.45, 0.25) };
		const double uses[3][2] = { { q.y, q.z }, { q.z, q.x }, { q.x, q.y } };
		double sum = 0.0, total = 0.0;
		for (int k = 0; k < 3; k++) {
			if (weights[k] <= 0.0)
				continue;
			const double u = uses[k][0] / cellKm + 311.0 * k, v = uses[k][1] / cellKm + 173.0 * k;
			sum += weights[k] * tile.At(c * u - s * v, s * u + c * v, level);
			total += weights[k];
		}
		return total > 0.0 ? sum / total : 0.0;
	}

	// The rough ground of an active shell, from tools/europa-tiles. Each map is 61.44 km across. (A tile reads in
	// thousandths, so 65535 reads as this.)
	constexpr double CRUST_FULL = 65.535;
	constexpr double CRUST_CELL_KM = 0.03;
	// metres, as the tool printed
	constexpr double CRUST_PLAINS_TOP = 744.0;
	constexpr double CRUST_CHAOS_TOPS[4] = { 929.3, 1022.9, 847.3, 424.2 };
	// each kind of chaos lies its own way
	constexpr double CRUST_CHAOS_ANGLES[4] = { 1.1, 0.2, 2.4, 1.7 };
	enum CrustMap {
		CRUST_PLAINS,
		CRUST_PLAINS_MASK,
		CRUST_CHAOS, // and its mask, then the same for the other three kinds
		CRUST_MAP_COUNT = CRUST_CHAOS + 8
	};

	const DuneTile *CrustTiles()
	{
		static const std::array<DuneTile, CRUST_MAP_COUNT> tiles = [] {
			std::array<DuneTile, CRUST_MAP_COUNT> loaded;
			loaded[CRUST_PLAINS].Load("terrain/europa_plains.bin");
			loaded[CRUST_PLAINS_MASK].Load("terrain/europa_plains_mask.bin", true);
			loaded[CRUST_CHAOS].Load("terrain/europa_chaos.bin");
			loaded[CRUST_CHAOS + 1].Load("terrain/europa_chaos_mask.bin", true);
			loaded[CRUST_CHAOS + 2].Load("terrain/europa_chaos_large.bin");
			loaded[CRUST_CHAOS + 3].Load("terrain/europa_chaos_large_mask.bin", true);
			loaded[CRUST_CHAOS + 4].Load("terrain/europa_chaos_rubble.bin");
			loaded[CRUST_CHAOS + 5].Load("terrain/europa_chaos_rubble_mask.bin", true);
			loaded[CRUST_CHAOS + 6].Load("terrain/europa_chaos_fresh.bin");
			loaded[CRUST_CHAOS + 7].Load("terrain/europa_chaos_fresh_mask.bin", true);
			return loaded;
		}();
		for (const DuneTile &tile : tiles) {
			if (tile.size == 0)
				return nullptr;
		}
		return tiles.data();
	}

	// Ganymede's grooves, from tools/europa-tiles: the maps' grooves run along y, and each is 61.44 km across
	enum GrooveMap {
		GROOVE_TILT, // young, sharp and triangular
		GROOVE_HORST, // older and subdued, with flat tops and floors
		GROOVE_SMOOTH,
		GROOVE_MAP_COUNT
	};
	constexpr double GROOVE_TOPS[GROOVE_MAP_COUNT] = { 500.6, 517.6, 327.9 }; // metres, as the tool printed

	const DuneTile *GrooveTiles()
	{
		static const std::array<DuneTile, GROOVE_MAP_COUNT> tiles = [] {
			std::array<DuneTile, GROOVE_MAP_COUNT> loaded;
			loaded[GROOVE_TILT].Load("terrain/ganymede_tilt.bin");
			loaded[GROOVE_HORST].Load("terrain/ganymede_horst.bin");
			loaded[GROOVE_SMOOTH].Load("terrain/ganymede_smooth.bin");
			return loaded;
		}();
		for (const DuneTile &tile : tiles) {
			if (tile.size == 0)
				return nullptr;
		}
		return tiles.data();
	}

	inline uint32_t GrooveMix(const uint32_t a, const uint32_t b)
	{
		uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u;
		h ^= h >> 15;
		h *= 0x2C1B3C6Du;
		h ^= h >> 13;
		h *= 0x297A2D39u;
		return h ^ (h >> 16);
	}
	inline double GrooveUnit(const uint32_t id, const uint32_t salt)
	{
		return double(GrooveMix(id, salt) & 0xFFFFFFu) / 16777215.0;
	}

	// The seeds around a place, of cells in space. They only decide which way the grooves run, so they are bent and of
	// uneven weight, and their edges are neither straight nor evenly spaced.
	struct GrooveSite {
		vector3d at;
		uint32_t id;
		double distance;
	};
	constexpr int GROOVE_SITES = 27;

	void FindGrooveSites(const vector3d &p, const double frequency, const uint32_t seed, GrooveSite sites[GROOVE_SITES])
	{
		const int64_t ix = int64_t(std::floor(p.x * frequency)), iy = int64_t(std::floor(p.y * frequency)), iz = int64_t(std::floor(p.z * frequency));
		int count = 0, nearest = 0;
		for (int dz = -1; dz <= 1; dz++) {
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					const int64_t x = ix + dx, y = iy + dy, z = iz + dz;
					const uint32_t id = GrooveMix(GrooveMix(GrooveMix(seed, uint32_t(x)), uint32_t(y)), uint32_t(z));
					GrooveSite &site = sites[count];
					site.id = id;
					site.at = vector3d(double(x) + 0.05 + 0.9 * GrooveUnit(id, 1), double(y) + 0.05 + 0.9 * GrooveUnit(id, 2), double(z) + 0.05 + 0.9 * GrooveUnit(id, 3)) * (1.0 / frequency);
					site.distance = (p - site.at).Length() * (0.7 + 0.6 * GrooveUnit(id, 20));
					if (site.distance < sites[nearest].distance)
						nearest = count;
					count++;
				}
			}
		}
		std::swap(sites[0], sites[nearest]);
	}

	// a stretch of grooves: its directions on the body, and how it reads a map
	struct GrooveSwath {
		vector3d across, along;
		double u, v, scale, tone;
		int map;
	};

	GrooveSwath MakeGrooveSwath(const uint32_t id, const vector3d &middle, const vector3d &direction, const double toneLeast, const double toneMore)
	{
		GrooveSwath s;
		const vector3d up = middle.Normalized();
		s.across = (direction - up * direction.Dot(up)).Normalized();
		// (tilted the one way or the other)
		if (GrooveUnit(id, 7) < 0.5)
			s.across = -s.across;
		s.along = up.Cross(s.across);
		s.u = 2048.0 * GrooveUnit(id, 8);
		s.v = 2048.0 * GrooveUnit(id, 9);
		s.scale = 0.75 + 0.65 * GrooveUnit(id, 10);
		const double t = GrooveUnit(id, 11);
		s.tone = toneLeast + toneMore * GrooveUnit(id, 12);
		s.map = t < 0.45 ? GROOVE_TILT : (t < 0.85 ? GROOVE_HORST : GROOVE_SMOOTH);
		return s;
	}
} // namespace

// Soft materials spread downwind over what is next to them, up to hundreds of km, in streaks along
// the wind, which is further than a patch can look. So it's worked out once, on a map.
void TerrainPhysical::BuildBorders()
{
	if (!m_state.weather)
		return;
	static constexpr int W = 1080, H = 540, STEPS = 7;
	const double radiusKm = m_core.radius * 6378.135;
	const double spacing = 2.0 * M_PI / W;
	const double reachKm = std::min(40.0 + 160.0 * m_state.air, radiusKm * 0.12);
	const double stepCells = std::min(reachKm / radiusKm / spacing / STEPS, 7.0);

	struct Carried {
		M::Layer layer;
		double power;
	};
	std::vector<Carried> carried;
	for (const Carried &c : { Carried{ M::SAND, 1.0 }, Carried{ M::DUST, 0.8 }, Carried{ M::SNOW, 0.7 }, Carried{ M::ASH, 1.0 } }) {
		if (c.layer != m_state.ground && m_slots[c.layer] >= 0)
			carried.push_back(c);
	}

	const size_t cells = size_t(W) * H;
	std::vector<std::vector<float>> shareMaps(carried.size(), std::vector<float>(cells));
	std::vector<float> windX(cells), windY(cells), heights(cells), onLand(cells);
	ParallelRows(H, [&](const int r) {
		TerrainNoise::ScopedBandLimit bandLimit(0.5 / spacing);
		const double latitude = M_PI / 2.0 - (r + 0.5) / H * M_PI;
		const double away = std::abs(latitude), side = latitude < 0.0 ? -1.0 : 1.0;
		// bands of wind, from the body's turning, each blowing towards the next near its edge, bent by the land's warp
		const double band = -1.0 + 2.0 * Soft(away, 0.42, 0.2);
		const double zonal = band - 1.6 * Soft(away, 0.95, 0.2);
		const double cosLat = std::max(std::cos(latitude), 0.12);
		for (int c = 0; c < W; c++) {
			const size_t i = size_t(r) * W + c;
			const vector3d p = GridPoint(r, c, W, H);
			Point point;
			Evaluate(p, point);
			Shares shares;
			Paint(p, point, shares);
			for (size_t k = 0; k < carried.size(); k++)
				shareMaps[k][i] = float(shares[carried[k].layer]);
			heights[i] = float(point.height);
			onLand[i] = point.underSea ? 0.f : 1.f;
			const double meridional = 0.35 * side * band + 0.6 * point.g.warp;
			const double length = std::max(std::sqrt(zonal * zonal + meridional * meridional), 1e-6);
			// in cells, on the map: east along columns, north against rows
			windX[i] = float(zonal / length / cosLat);
			windY[i] = float(-meridional / length);
		}
	});

	// How much lower or higher the ground lies than what is around it, at two sizes, each scaled by its own
	// range on this body. It's on land only, so that a coast isn't taken for a rise.
	{
		constexpr int w = W / 2, h = H / 2;
		constexpr double SIZES[2] = { 1.5, 5.0 }; // in cells of this map
		TerrainHydro::Field height, land;
		height.Resize(w, h);
		land.Resize(w, h);
		for (int r = 0; r < H; r++) {
			for (int c = 0; c < W; c++) {
				const size_t i = size_t(r) * W + c;
				height.At(r / 2, c / 2) += 0.25f * heights[i] * onLand[i];
				land.At(r / 2, c / 2) += 0.25f * onLand[i];
			}
		}
		TerrainHydro::Field blurred[4] = { height, land, height, land };
		ParallelRows(4, [&](const int k) { blurred[k].Blur(SIZES[k / 2]); });
		m_landform.Resize(w, h);
		for (int k = 0; k < 2; k++) {
			std::vector<float> difference(land.values.size(), 0.f), sizes;
			for (size_t i = 0; i < difference.size(); i++) {
				if (land.values[i] < 0.25f)
					continue;
				difference[i] = height.values[i] / land.values[i] - blurred[2 * k].values[i] / std::max(blurred[2 * k + 1].values[i], 0.01f);
				sizes.push_back(std::abs(difference[i]));
			}
			if (sizes.empty())
				continue;
			const size_t at = size_t(0.98 * double(sizes.size() - 1));
			std::nth_element(sizes.begin(), sizes.begin() + at, sizes.end());
			const float range = std::max(sizes[at], 1.f);
			for (size_t i = 0; i < difference.size(); i++)
				m_landform.values[i] += 0.5f * land.values[i] * difference[i] / range;
		}
		for (float &v : m_landform.values)
			v = std::clamp(v, -1.f, 1.f);
		m_landform.FindRange();
	}
	if (stepCells < 0.25)
		return;
	const auto sample = [](const std::vector<float> &a, double x, double y) {
		y = std::clamp(y, 0.0, double(H - 1));
		x -= std::floor(x / W) * W;
		const int ix = std::min(int(x), W - 1), iy = std::min(int(y), H - 1);
		const int nextX = (ix + 1) % W, nextY = std::min(iy + 1, H - 1);
		const double tx = x - ix, ty = y - iy;
		const double top = a[size_t(iy) * W + ix] * (1.0 - tx) + a[size_t(iy) * W + nextX] * tx;
		const double bottom = a[size_t(nextY) * W + ix] * (1.0 - tx) + a[size_t(nextY) * W + nextX] * tx;
		return top * (1.0 - ty) + bottom * ty;
	};

	// streaks along the wind: noise fixed to the map's cells, smeared along it
	TerrainHydro::Field noise;
	noise.Resize(W, H);
	Random rng(m_core.seed ^ 0x51u);
	for (float &v : noise.values)
		v = float(rng.Double());
	noise.Blur(2.5);
	std::vector<float> streak(cells);
	ParallelRows(H, [&](const int r) {
		for (int c = 0; c < W; c++) {
			const size_t i = size_t(r) * W + c;
			double sum = noise.values[i];
			for (int k = 1; k < 12; k++) {
				const double d = k * 0.5 * stepCells;
				sum += sample(noise.values, c + windX[i] * d, r + windY[i] * d) + sample(noise.values, c - windX[i] * d, r - windY[i] * d);
			}
			streak[i] = float(sum);
		}
	});
	std::vector<float> sorted(streak);
	std::sort(sorted.begin(), sorted.end());
	const double low = sorted[cells / 20], high = std::max(double(sorted[cells - 1 - cells / 20]), low + 1e-6);
	for (size_t i = 0; i < cells; i++)
		streak[i] = float(std::clamp((streak[i] - low) / (high - low), 0.0, 1.0));

	for (size_t k = 0; k < carried.size(); k++) {
		const std::vector<float> &share = shareMaps[k];
		if (*std::max_element(share.begin(), share.end()) < 0.05f)
			continue;
		WindBorder border;
		border.layer = carried[k].layer;
		border.gain.Resize(W, H);
		const bool scoured = carried[k].layer == M::SNOW;
		if (scoured)
			border.loss.Resize(W, H);
		const double power = carried[k].power * (0.45 + 0.55 * m_state.air);
		ParallelRows(H, [&](const int r) {
			for (int c = 0; c < W; c++) {
				const size_t i = size_t(r) * W + c;
				double down = 0.0;
				for (int s = 1; s <= STEPS; s++) {
					const double d = s * stepCells;
					down = std::max(down, sample(share, c - windX[i] * d, r - windY[i] * d) * (1.0 - double(s - 1) / STEPS));
				}
				// the wind carries it where the streak says so, weaker at the far end
				border.gain.values[i] = float(Soft(down * power, 0.2 + 0.7 * streak[i], 0.08));
				if (scoured) {
					const double up = sample(share, c + windX[i] * 2.0 * stepCells, r + windY[i] * 2.0 * stepCells);
					border.loss.values[i] = float(Soft(1.0 - up, 0.35 + 0.5 * streak[i], 0.1));
				}
			}
		});
		border.gain.Blur(0.7);
		border.gain.FindRange();
		if (scoured)
			border.loss.FindRange();
		m_borders.push_back(std::move(border));
	}
}

void TerrainPhysical::WindBorders(const vector3d &p, const Point &point, Shares &shares) const
{
	if (m_borders.empty() || point.underSea || point.lakeField > 0.5 || point.smallLakeEdge < 0.0 || point.sideEdge < 0.0 || (point.hasRiver && point.river.distance < 0.0))
		return;
	double fine = 0.0;
	bool hasFine = false;
	for (const WindBorder &border : m_borders) {
		const double gain = border.gain.Smooth(point.place);
		const double loss = border.loss.Empty() ? 0.0 : border.loss.Smooth(point.place);
		if (gain < 0.01 && loss < 0.01)
			continue;
		// the map's edge, finer close up
		if (!hasFine) {
			fine = 0.1 * std::clamp(Fbm(p, 64.0, 7, m_noiseSeed + 172, 0.6) / 0.35, -1.0, 1.0);
			hasFine = true;
		}
		const double amount = Soft(gain + fine, 0.3, 0.3);
		// (snow doesn't drift over ice)
		const bool snow = border.layer == M::SNOW;
		if (amount > 0.0) {
			for (int i = 0; i < M::LAYER_COUNT; i++) {
				if (i == border.layer || (snow && i == M::ICE) || shares[i] <= 0.0)
					continue;
				const double moved = shares[i] * amount;
				shares[i] -= moved;
				shares[border.layer] += moved;
			}
		}
		if (loss >= 0.01) {
			const double scoured = shares[border.layer] * Soft(loss + fine, 0.3, 0.3);
			shares[border.layer] -= scoured;
			shares[m_state.ground] += scoured;
		}
	}
}

// Dunes need a sand sea, so there are none on beaches and barrier islands.
void TerrainPhysical::BuildSandSeas()
{
	if (!m_state.weather)
		return;
	constexpr int W = 360, H = 180;
	TerrainHydro::Field sand;
	sand.Resize(W, H);
	ParallelRows(H, [&](const int r) {
		TerrainNoise::ScopedBandLimit bandLimit(GROUND_MAX_FREQUENCY);
		for (int c = 0; c < W; c++) {
			const vector3d p = GridPoint(r, c, W, H);
			Point point;
			Evaluate(p, point);
			Shares shares;
			Paint(p, point, shares);
			sand.At(r, c) = float(shares[M::SAND]);
		}
	});
	const double cellKm = 2.0 * M_PI * m_core.radius * 6378.135 / W;
	sand.Blur(std::max(60.0 / cellKm, 1.0));
	sand.FindRange();
	if (sand.high >= 0.3f)
		m_sandSea = std::move(sand);
}

// They only rise above the ground, so they also lie on low plains at the sea's level, where nothing can go lower
double TerrainPhysical::Dunes(const vector3d &p, const Point &point, const double calm) const
{
	if (m_sandSea.Empty() || calm <= 0.0 || point.quiet <= 0.0 || point.lakeField > 0.0 || point.smallLakeRatio < TerrainValleys::LAKE_REACH)
		return 0.0;
	const DuneTile *tiles = DuneTiles();
	if (!tiles)
		return 0.0;
	const double radiusKm = m_core.radius * 6378.135;
	const double maxFrequency = TerrainNoise::s_maxFrequency;
	const double samples = maxFrequency > 0.0 ? 0.5 / maxFrequency * radiusKm : 0.0;

	double amount = calm * point.quiet * Soft(m_sandSea.Smooth(point.place), 0.3, 0.3);
	if (m_state.sea > 0.0)
		amount *= Soft(point.shore - m_seaLevel, 0.0, 100.0);
	if (amount <= 0.0)
		return 0.0;
	// only on sand, whatever lies beside it
	Shares shares;
	Paint(p, point, shares);
	amount *= shares[M::SAND];
	if (amount <= 0.0)
		return 0.0;

	// Bent, so that the tile's repeats don't line up, and two tiles with their winds at an angle, region by region.
	// All of this at full detail, or the dunes would move and change height from one patch to the next.
	vector3d shift(0.0, 0.0, 0.0);
	double rich, size;
	{
		TerrainNoise::ScopedBandLimit fullDetail(0.0);
		shift = vector3d(8.0 * Fbm(p, radiusKm / 60.0, 4, m_noiseSeed + 304), 8.0 * Fbm(p, radiusKm / 60.0, 4, m_noiseSeed + 305), 8.0 * Fbm(p, radiusKm / 60.0, 4, m_noiseSeed + 306));
		rich = Soft(Fbm(p, radiusKm / 150.0, 3, m_noiseSeed + 307), -0.1, 0.2);
		// lower in places, over a few dozen km
		size = 0.55 + 0.45 * Soft(Fbm(p, radiusKm / 25.0, 4, m_noiseSeed + 303, 0.6), -0.2, 0.3);
	}
	double cells = 0.0, typical = 0.0;
	if (rich < 1.0) {
		cells += (1.0 - rich) * DuneTileAt(tiles[0], p, radiusKm, shift, DUNE_CELL_KM, 0.3, samples);
		typical += (1.0 - rich) * tiles[0].mean;
	}
	if (rich > 0.0) {
		cells += rich * DuneTileAt(tiles[1], p, radiusKm, shift, DUNE_CELL_KM, 1.0, samples);
		typical += rich * tiles[1].mean;
	}
	double height = cells;
	// smaller ones in the flats between them, which add nothing on average
	const double flat = 1.0 - Soft(cells, 0.7 * typical, 0.6 * typical);
	if (flat > 0.0)
		height += flat * DUNE_SMALL * (DuneTileAt(tiles[0], p, radiusKm, shift, DUNE_SMALL * DUNE_CELL_KM, 2.2, samples) - tiles[0].mean);

	// on bodies with little relief they are lower
	const double limit = std::min(0.02 * m_reliefLimit / 150.0, 1.0);
	const double metres = amount * size * limit * DUNE_CELL_KM * 1000.0;
	// They average out to the ground, so that patches of any detail meet. The exception is low ground, which has
	// no room to go lower: there they rise above it, or they would be cut off flat at the sea's level.
	const double room = m_seaLevel > NONE_LOW ? std::max(point.height - m_seaLevel, 0.0) : 1e30;
	return std::max(metres * height - std::min(metres * typical, room), -room);
}

double TerrainPhysical::CrustChaos(const vector3d &p) const
{
	// (at full detail, or its edge would move from one patch to the next)
	TerrainNoise::ScopedBandLimit fullDetail(0.0);
	const double radiusKm = m_core.radius * 6378.135;
	return Soft(Fbm(p, std::max(radiusKm / 240.0, 2.0), 4, m_noiseSeed + 314, 0.55), 0.1, 0.1);
}

double TerrainPhysical::CrustMaps(const vector3d &p, Point &point) const
{
	const DuneTile *tiles = CrustTiles();
	if (!tiles)
		return 0.0;
	const double radiusKm = m_core.radius * 6378.135;
	const double maxFrequency = TerrainNoise::s_maxFrequency;
	const double samples = maxFrequency > 0.0 ? 0.5 / maxFrequency * radiusKm : 0.0;

	// Bent, so that the tile's repeats don't line up, and turned differently region by region. At full detail, or
	// all this would move from one patch to the next.
	// (on a small shell, the rough ground is around its active pole, and chaos only there)
	const double polar = point.g.polarCap;
	const double chaos = CrustChaos(p) * (m_lines.IsPolar() ? polar : 1.0);
	vector3d shift(0.0, 0.0, 0.0);
	double second;
	{
		TerrainNoise::ScopedBandLimit fullDetail(0.0);
		const double bend = radiusKm / 80.0;
		shift = vector3d(14.0 * Fbm(p, bend, 3, m_noiseSeed + 311), 14.0 * Fbm(p, bend, 3, m_noiseSeed + 312), 14.0 * Fbm(p, bend, 3, m_noiseSeed + 313));
		second = Soft(Fbm(p, std::max(radiusKm / 170.0, 2.0), 3, m_noiseSeed + 315), -0.05, 0.1);
	}
	const auto layer = [&](const int map, const double top, const double angle, double &mask) {
		const DuneTile &tile = tiles[map];
		mask = DuneTileAt(tiles[map + 1], p, radiusKm, shift, CRUST_CELL_KM, angle, samples) / CRUST_FULL;
		return (DuneTileAt(tile, p, radiusKm, shift, CRUST_CELL_KM, angle, samples) - tile.mean) / CRUST_FULL * top;
	};
	double metres = 0.0, mask = 0.0;
	if (chaos < 1.0) {
		if (second < 1.0) {
			metres += (1.0 - chaos) * (1.0 - second) * layer(CRUST_PLAINS, CRUST_PLAINS_TOP, 0.4, mask);
			point.crustCrack += (1.0 - chaos) * (1.0 - second) * mask;
		}
		if (second > 0.0) {
			metres += (1.0 - chaos) * second * layer(CRUST_PLAINS, CRUST_PLAINS_TOP, 1.9, mask);
			point.crustCrack += (1.0 - chaos) * second * mask;
		}
	}
	if (chaos > 0.0) {
		// Region by region its own kind of chaos and its own turn, and the brown goes with what is there. From afar
		// it is broad and uneven, or the map's rubble would repeat as a lace.
		double kind, other, turn, broad;
		{
			TerrainNoise::ScopedBandLimit fullDetail(0.0);
			const double regions = std::max(radiusKm / 300.0, 2.0);
			kind = Soft(Fbm(p, regions, 3, m_noiseSeed + 316), -0.03, 0.06);
			other = Soft(Fbm(p, regions, 3, m_noiseSeed + 317), -0.03, 0.06);
			turn = Soft(Fbm(p, regions, 3, m_noiseSeed + 318), -0.03, 0.06);
			broad = Soft(Fbm(p, std::max(radiusKm / 35.0, 4.0), 4, m_noiseSeed + 319, 0.6), -0.25, 0.5);
		}
		double rubble = 0.0, typical = 0.0;
		for (int style = 0; style < 4; style++) {
			const double share = ((style & 1) ? kind : 1.0 - kind) * ((style & 2) ? other : 1.0 - other);
			if (share < 0.005)
				continue;
			const int map = CRUST_CHAOS + 2 * style;
			typical += share * tiles[map + 1].mean / CRUST_FULL;
			for (int way = 0; way < 2; way++) {
				const double weight = share * (way ? turn : 1.0 - turn);
				if (weight < 0.005)
					continue;
				metres += chaos * weight * layer(map, CRUST_CHAOS_TOPS[style], CRUST_CHAOS_ANGLES[style] + 2.1 * way, mask);
				rubble += weight * mask;
			}
		}
		const double close = 1.0 - Soft(samples, 0.4, 1.1);
		point.crustRubble = chaos * (close * rubble + (1.0 - close) * std::min(1.3 * typical * (0.45 + 0.55 * broad), 1.0));
	}
	// a small moon has less relief
	return metres * std::min(0.4 * m_reliefLimit / CRUST_ROOM, 1.0) * (m_lines.IsPolar() ? 0.15 + 0.45 * polar : 1.0);
}

void TerrainPhysical::BuildGrooves()
{
	Random rng((m_core.seed ^ 0x6A0Fu) & 0x7fffffffu);
	const auto direction = [&rng]() {
		const double y = 2.0 * rng.Double() - 1.0;
		const double angle = 2.0 * M_PI * rng.Double();
		const double r = std::sqrt(std::max(0.0, 1.0 - y * y));
		return vector3d(r * std::cos(angle), y, r * std::sin(angle));
	};
	m_grooveAxes[0] = direction();
	m_grooveAxes[1] = direction();
	m_furrowCentre = direction();
	const double radiusKm = m_core.radius * 6378.135;
	const double share = (radiusKm / 2631.0) * (radiusKm / 2631.0);
	const auto add = [&](const int count, const double lengthLeast, const double lengthMore, const double halfLeast, const double halfMore, const double dropLeast, const double dropMore, const double toneLeast, const double toneMore) {
		for (int i = 0; i < count; i++) {
			GrooveBand band;
			band.middle = direction();
			// most run the way the dark regions are drawn out, so they split them lengthways
			const double pick = rng.Double();
			const vector3d axis = pick < 0.45 ? m_grooveAxes[0] : (pick < 0.8 ? m_grooveAxes[1] : direction());
			band.normal = (band.middle.Cross(axis) + direction() * 0.2).Normalized();
			band.offset = band.middle.Dot(band.normal);
			band.reach = std::min(0.5 * (lengthLeast + lengthMore * rng.Double()) / radiusKm, 2.0);
			const double wide = rng.Double();
			band.half = halfLeast + halfMore * wide * wide;
			band.drop = dropLeast + (dropMore - dropLeast) * rng.Double();
			band.toneLeast = toneLeast;
			band.toneMore = toneMore;
			band.id = rng.Int32();
			m_grooveBands.push_back(band);
		}
	};
	add(int(34.0 * share) + 3, 700.0, 3300.0, 14.0, 36.0, 60.0, 180.0, -0.06, 0.16);
	add(int(130.0 * share) + 6, 150.0, 750.0, 2.5, 5.5, 20.0, 70.0, -0.08, 0.2);
}

double TerrainPhysical::GrooveDarkBase(const vector3d &p) const
{
	// (at full detail, or its edge would move from one patch to the next)
	TerrainNoise::ScopedBandLimit fullDetail(0.0);
	// shapes of all sizes, drawn out two ways, each over a part of the moon
	const auto stretched = [&](const vector3d &axis, const uint32_t seed) {
		return Fbm(p - axis * (0.7 * p.Dot(axis)), 1.7, 5, m_noiseSeed + seed, 0.55);
	};
	const double which = Soft(Fbm(p, 0.9, 2, m_noiseSeed + 333), -0.12, 0.24);
	return stretched(m_grooveAxes[0], 331) * (1.0 - which) + stretched(m_grooveAxes[1], 332) * which;
}

// Bright grooved ground, whose grooves run a different way from place to place, which only shows close up. Old dark
// regions of every size have jagged edges, and long lanes of grooves split them, and the bright ground too.
double TerrainPhysical::Grooves(const vector3d &p, Point &point) const
{
	const DuneTile *tiles = GrooveTiles();
	if (!tiles)
		return 0.0;
	const double radiusKm = m_core.radius * 6378.135;
	const double maxFrequency = TerrainNoise::s_maxFrequency;
	const double samples = maxFrequency > 0.0 ? 0.5 / maxFrequency * radiusKm : 0.0;
	const double edgeKm = std::max(1.0, 2.5 * samples);

	// lines bend, and the bright ground's tone changes softly, at full detail or they would move between patches
	vector3d bent;
	double area;
	{
		TerrainNoise::ScopedBandLimit fullDetail(0.0);
		const double bend = radiusKm / 500.0;
		bent = p + vector3d(Fbm(p, bend, 3, m_noiseSeed + 321), Fbm(p, bend, 3, m_noiseSeed + 322), Fbm(p, bend, 3, m_noiseSeed + 323)) * 0.07;
		area = Soft(Fbm(p, 2.2, 4, m_noiseSeed + 324, 0.55), -0.2, 0.4);
	}
	const vector3d bentUnit = bent.Normalized();

	// metres, and how much dark shows: dark lag in the troughs, once they can be seen, and faint long streaks
	const double troughs = 1.1 * (1.0 - Soft(samples, 0.5, 1.5));
	const double streaks = 0.03 * (1.0 - Soft(samples, 6.0, 10.0));
	const double brightTone = 0.03 + 0.2 * area;
	const auto readSwath = [&](const GrooveSwath &s, double &dark) {
		const DuneTile &tile = tiles[s.map];
		const double cell = CRUST_CELL_KM * s.scale;
		const double acrossKm = bent.Dot(s.across) * radiusKm, alongKm = bent.Dot(s.along) * radiusKm;
		const double relative = (tile.At(acrossKm / cell + s.u, alongKm / cell + s.v, tile.LevelFor(samples, cell)) - tile.mean) / CRUST_FULL;
		const double streak = streaks > 0.0 ? streaks * SphereCells::Noise(vector3d(acrossKm / 22.0, alongKm / 500.0, s.u), uint32_t(s.v)) : 0.0;
		dark = std::clamp(brightTone + s.tone - troughs * relative + streak, 0.0, 1.0);
		return relative * GROOVE_TOPS[s.map] * std::sqrt(s.scale);
	};

	// the dark regions, with jagged edges at every size
	const double base = GrooveDarkBase(p);
	const double darkEdge = std::max(0.004, 1.5 * samples / radiusKm);
	double darkShare = base > m_grooveDarkLevel ? 1.0 : 0.0;
	if (std::abs(base - m_grooveDarkLevel) < 0.04 + darkEdge)
		darkShare = Soft(base + 0.05 * Fbm(p, radiusKm / 90.0, 9, m_noiseSeed + 325, 0.58), m_grooveDarkLevel, darkEdge);

	// the bright ground, a little lower
	double metres = 0.0, dark = 0.0;
	if (darkShare < 1.0) {
		GrooveSite sites[GROOVE_SITES];
		FindGrooveSites(bent, std::max(radiusKm / 700.0, 2.0), m_noiseSeed + 327, sites);
		double total = 0.0;
		for (int i = 0; i < GROOVE_SITES; i++) {
			const double fromEdgeKm = 0.5 * (sites[i].distance - sites[0].distance) * radiusKm;
			if (fromEdgeKm >= edgeKm)
				continue;
			const double weight = 1.0 - Soft(fromEdgeKm, 0.0, edgeKm);
			const GrooveSite &site = sites[i];
			double darkHere;
			metres += weight * (readSwath(MakeGrooveSwath(site.id, site.at, vector3d(GrooveUnit(site.id, 4) - 0.5, GrooveUnit(site.id, 5) - 0.5, GrooveUnit(site.id, 6) - 0.5), -0.03, 0.06), darkHere) - 80.0);
			dark += weight * darkHere;
			total += weight;
		}
		metres /= total;
		dark /= total;
	}
	// the dark ground, with faint arcs of furrows around one point of the moon, from a map of grooves laid large
	if (darkShare > 0.0) {
		const double angle = std::acos(std::clamp(bentUnit.Dot(m_furrowCentre), -1.0, 1.0));
		double furrows = 0.0;
		if (angle < 1.9) {
			const DuneTile &tile = tiles[GROOVE_HORST];
			const double cell = CRUST_CELL_KM * 9.0;
			const double alongKm = bent.Dot(m_furrowCentre.Cross(vector3d(0.31, 0.87, 0.38)).Normalized()) * radiusKm;
			furrows = 0.25 * (1.0 - Soft(angle, 1.3, 0.6)) * (tile.At(angle * radiusKm / cell, alongKm / cell, tile.LevelFor(samples, cell)) - tile.mean) / CRUST_FULL * GROOVE_TOPS[GROOVE_HORST];
		}
		metres += darkShare * (furrows - metres);
		dark += darkShare * (1.0 - dark);
	}

	// the long lanes across it all, the younger cutting the older; each narrows to nothing at its ends
	double rough = 2.0;
	for (const GrooveBand &band : m_grooveBands) {
		const double fromMiddleKm = std::abs(bent.Dot(band.normal) - band.offset) * radiusKm;
		if (fromMiddleKm >= 1.3 * band.half + edgeKm)
			continue;
		const double along = std::acos(std::clamp(bentUnit.Dot(band.middle), -1.0, 1.0)) / band.reach;
		if (along >= 1.0)
			continue;
		if (rough > 1.5)
			rough = std::clamp(Fbm(p, radiusKm / 25.0, 6, m_noiseSeed + 326, 0.55) / 0.35, -1.0, 1.0);
		const double width = band.half * Soft(1.0 - along, 0.0, 0.35) * (1.0 + 0.25 * rough);
		const double mask = Soft(width - fromMiddleKm, 0.0, edgeKm);
		if (mask <= 0.0)
			continue;
		double darkHere;
		const double height = readSwath(MakeGrooveSwath(band.id, band.middle, band.normal, band.toneLeast, band.toneMore), darkHere) - band.drop;
		metres += mask * (height - metres);
		dark += mask * (darkHere - dark);
	}
	point.grooveDark = dark;
	return metres * std::min(0.4 * m_reliefLimit / CRUST_ROOM, 1.0);
}

// Lava plains are flows laid one over another: a lobed front a few metres high where each stopped, and a rough, hummocky
// top. Without them a flood plain is flat to the horizon up close. Fixed in space. The fronts are steps, too sharp for
// samples far apart, where they would draw thin wandering lines: they fade out as the samples grow past FLOW_FRONT_SHOW_KM.
double TerrainPhysical::LavaPlainsRelief(const vector3d &p, const Ground &g) const
{
	const bool lavaGround = m_state.ground == M::BASALT || m_state.ground == M::LAVA_FLOW;
	const double weight = lavaGround ? 1.0 : g.plains;
	if (weight <= 0.0)
		return 0.0;
	const double radiusKm = m_core.radius * 6378.135;
	// (weather wears them down)
	const double kept = 1.0 - 0.7 * std::min(m_state.wear, 1.0);

	// steps at the contours of a smooth field, which wander and lobe as flow fronts do; from afar the field itself, which
	// the steps follow on average, so the ground doesn't rise or sink as they come in
	const double u = FLOW_LEVELS * Fbm(p, radiusKm / FLOW_SIZE_KM, 5, m_noiseSeed + 451, 0.55) / 0.35;
	const double maxFrequency = TerrainNoise::s_maxFrequency;
	const double samplesKm = maxFrequency > 0.0 ? 0.5 / maxFrequency * radiusKm : 0.0;
	const double show = 1.0 - Soft(samplesKm, FLOW_FRONT_SHOW_KM, FLOW_FRONT_SHOW_KM);
	double steps = u - 0.5;
	if (show > 0.0) {
		const double level = std::floor(u);
		steps += show * (level + Soft(u - level, 1.0 - FLOW_FRONT_EDGE, FLOW_FRONT_EDGE) - u);
	}
	const double fronts = FLOW_FRONT_M * steps;
	const int octaves = FLOW_ROUGH_OCTAVES_OLD + int(std::lround((FLOW_ROUGH_OCTAVES_YOUNG - FLOW_ROUGH_OCTAVES_OLD) * LavaYouth()));
	const double rough = FLOW_ROUGH_M * Fbm(p, radiusKm / FLOW_ROUGH_KM, octaves, m_noiseSeed + 452, 0.65) / 0.35;
	return weight * kept * (fronts + rough);
}

// Real ground is rough at every size, more at large sizes than small in a steady ratio. Without this, a world whose
// geology adds nothing between a few km and the samples (few craters, no weather, ground with no relief of its own)
// came out flat up close. Fixed in space; the octaves finer than the samples are left out (Fbm).
namespace {
	// (a small body's begins at its own size, as high as the steady ratio makes it there)
	double BaseFractal(const vector3d &p, const double radiusKm, const double metres, const double rough, const uint32_t seed)
	{
		const double startKm = std::min(BASE_RELIEF_KM, radiusKm / 8.0);
		const double height = metres * std::pow(startKm / BASE_RELIEF_KM, rough);
		return height * Fbm(p, radiusKm / startKm, BASE_RELIEF_OCTAVES, seed, std::pow(2.0, -rough)) / 0.35;
	}
} // namespace

double TerrainPhysical::YoungRelief(const vector3d &p) const
{
	return BaseFractal(p, m_core.radius * 6378.135, BASE_RELIEF_M, BASE_RELIEF_ROUGH, m_noiseSeed + 461);
}

double TerrainPhysical::OldRelief(const vector3d &p) const
{
	return BaseFractal(p, m_core.radius * 6378.135, OLD_RELIEF_M, OLD_RELIEF_ROUGH, m_noiseSeed + 462);
}

double TerrainPhysical::BaseRelief(const vector3d &p, const double oldShare) const
{
	const double young = YoungRelief(p);
	return oldShare > 0.0 ? young + (OldRelief(p) - young) * oldShare : young;
}

double TerrainPhysical::LavaYouth() const
{
	return 1.0 - Soft(m_interior.cratering, OLD_FROM_CRATERING, OLD_CRATERING_WIDTH);
}

bool TerrainPhysical::IsLavaGround() const
{
	return m_state.ground == M::BASALT || m_state.ground == M::LAVA_FLOW;
}

double TerrainPhysical::GetTileBlocks() const
{
	// (what has maps of its own keeps the look it was judged with)
	if (m_lines.IsActiveShell() || m_grooved)
		return 0.0;
	if (IsLavaGround() || m_state.ground == M::LAVA || m_state.ground == M::SALT)
		return LAVA_TILE_BLOCKS * LavaYouth();
	if (m_state.ground == M::ROCK || m_state.ground == M::METAL)
		return ROCK_TILE_BLOCKS;
	return 0.0;
}

double TerrainPhysical::OldShare(const Ground &g) const
{
	// (what has maps of its own keeps the look it was judged with)
	if (m_lines.IsActiveShell() || m_grooved)
		return 0.0;
	return Soft(m_interior.cratering, OLD_FROM_CRATERING, OLD_CRATERING_WIDTH) * (1.0 - g.plains);
}

double TerrainPhysical::BaseShare(const Ground &g) const
{
	double share = BaseOf(m_state.ground);
	if (IsLavaGround())
		share += (std::max(share, YOUNG_LAVA_BASE) - share) * LavaYouth();
	// (what has maps of its own keeps the look it was judged with)
	if (m_lines.IsActiveShell() || m_grooved)
		share = std::min(share, BASE_RELIEF_MAPPED);
	// flood plains are smoother, never smooth
	return share + (std::min(share, BASE_RELIEF_PLAINS) - share) * g.plains;
}

double TerrainPhysical::OwnRelief(const vector3d &p, const Ground &g) const
{
	// (a small body's relief is its shape and its craters)
	if (m_core.asteroid)
		return 0.0;
	const double cap = 0.05 * m_reliefLimit;
	const double fMin = std::max(m_core.radius * 6378.135 / 100.0, 3.0);
	constexpr double ROUND_CREST = 0.05; // of the noise, how far to each side of a crest it's rounded
	auto own = [&](const double amplitude, const double frequency, const int octaves, const uint32_t seed, const bool ridged) {
		const double n = Fbm(p, std::max(frequency, fMin), octaves, m_noiseSeed + seed);
		return amplitude * (ridged ? 1.0 - 2.0 * std::abs(n) * 2.2 : n);
	};
	// An ice crust's ridges. On a small old ice world they are lower, with round crests: sharp ones come out as thin
	// wandering lines, and from orbit, where the light takes slopes as steeper, as worm-like wrinkles all over it.
	auto iceRidges = [&]() {
		if (!IsSmallOldIce())
			return own(0.25 * cap, fMin * 9.0, 5, 208, true);
		const double n = Fbm(p, fMin * 9.0, 5, m_noiseSeed + 208);
		return 0.12 * cap * (1.0 - 2.0 * (std::sqrt(n * n + ROUND_CREST * ROUND_CREST) - ROUND_CREST) * 2.2);
	};
	double out = 0.0;
	switch (m_state.ground) {
	case M::ROCK: out = own(cap, fMin * 1.5, 5, 201, true); break;
	case M::ICE:
		// the broken crust of an active shell is low but rough
		if (m_lines.IsActiveShell())
			out = own((m_lines.IsPolar() ? 0.2 : 0.7) * cap, fMin * 1.5, 4, 211, false);
		else if (m_interior.iceCrust && !m_state.weather)
			out = own(0.35 * cap, fMin * 2.0, 3, 202, false) + iceRidges();
		break;
	case M::DIRTY_ICE: out = own(0.35 * cap, fMin * 2.0, 3, 202, false) + iceRidges(); break;
	case M::GRAVEL: out = own(0.2 * cap, fMin * 8.0, 3, 203, false); break;
	case M::LAVA_FLOW: out = own((0.25 + YOUNG_LAVA_OWN * LavaYouth()) * cap, fMin * 2.0, 3, 204, false); break;
	case M::BASALT: out = own((0.1 + YOUNG_LAVA_OWN * LavaYouth()) * cap, fMin * 2.0, 3, 205, false); break;
	case M::METAL: out = own(cap, fMin * 2.0, 4, 206, true); break;
	case M::SOIL: out = own(0.2 * cap, fMin * 1.5, 3, 207, false); break;
	default: break;
	}
	// high ground is rock with a ridged relief of its own, on worlds with weather
	if (m_state.weather && !m_interior.iceCrust && m_state.ground != M::ROCK) {
		const double ground = m_seaLevel > NONE_LOW ? m_seaLevel : 0.0;
		const double high = Soft((g.base - ground) / std::min(m_reliefLimit, 7000.0), 0.25, 0.15);
		if (high > 0.0)
			out += high * own(cap, fMin * 1.5, 5, 201, true);
	}
	return out;
}

double TerrainPhysical::IceField(const vector3d &p, const Ground &g) const
{
	const double te = m_state.equatorTemperature, tp = m_state.poleTemperature, spread = te - tp;
	const double T = Temperature(p, g.warp);
	const double wobble = spread * (0.2 * g.warp + 0.09 * Lag(p)) / (M_PI / 2.0);
	const double ground = m_seaLevel > NONE_LOW ? m_seaLevel : 0.0;
	const double high = (0.1 + 0.45 * (1.0 - Soft(m_core.temperature, 245.0, 30.0))) * spread * (g.base - ground) / m_reliefLimit;
	return -(T - wobble - 0.4 * m_state.lapse * std::max(g.base - ground, 0.0) - high);
}

// cold everywhere, so bright ice follows the geological units
double TerrainPhysical::CrustIceField(const vector3d &p, const Ground &g) const
{
	const double latitude = std::asin(std::clamp(p.y, -1.0, 1.0));
	return g.warp * 0.9 + 0.35 * Lag(p) + 0.05 * (std::abs(latitude) + 0.05 * g.warp);
}

double TerrainPhysical::DarkField(const vector3d &p, const double warp) const
{
	if (m_hemispheres)
		return p.Dot(m_hemisphereAxis) + 0.3 * warp + 0.12 * Lag(p);
	switch (m_moonStyle) {
	case MoonStyle::BELTS: {
		const vector3d axis = vector3d(0.2, 0.95, 0.1).Normalized();
		return std::sin((p.Dot(axis) + 0.18 * warp) * 5.5 + double(m_noiseSeed % 7));
	}
	case MoonStyle::PATCHWORK: return Fbm(p, 2.4, 4, m_noiseSeed + 79, 0.6);
	case MoonStyle::POLAR: return std::abs(p.Dot(vector3d(0.3, 0.9, 0.3))) + 0.3 * Fbm(p, 1.1, 3, m_noiseSeed + 77);
	case MoonStyle::STREAKS: return Fbm(p, 3.4, 4, m_noiseSeed + 78, 0.6);
	default: return Fbm(p, 1.1, 3, m_noiseSeed + 77);
	}
}

// The border of a two-toned moon is sharp from afar. Close up it's a wide zone of spots of both, with the dark lag
// gathered in the hollows and worn off the rims, as on Iapetus.
double TerrainPhysical::TwoToneSide(const vector3d &p, const Ground &g, const double level, const double darker) const
{
	const double field = DarkField(p, g.warp);
	const double radiusKm = m_core.radius * 6378.135;
	const double zone = std::min(75.0 / radiusKm, 0.2); // half its width, on the unit sphere
	const double hollows = 0.8 * zone;
	const double maxFrequency = TerrainNoise::s_maxFrequency;
	// (as sharp as the samples can show)
	const double edge = std::max(0.0006, maxFrequency > 0.0 ? 0.75 / maxFrequency : 0.0);
	// far from the border, nothing below could change the side
	const double reach = 1.5 * zone + hollows + edge;
	if (field > level + reach)
		return 1.0;
	if (field < level - reach)
		return 0.0;
	const double spots = std::clamp(Fbm(p, radiusKm / 80.0, 10, m_noiseSeed + 85, 0.62) / 0.35, -1.5, 1.5);
	const double relief = g.craters.large + g.craters.small;
	const double hollow = std::clamp(-relief / std::max(0.03 * m_reliefLimit, 1.0), -1.0, 1.0);
	return Soft(field + zone * spots + darker * hollows * hollow, level, edge);
}

// separate dune seas near the equator in the lows, with highlands between them
double TerrainPhysical::DuneField(const vector3d &p, const Ground &g) const
{
	const double latitude = std::abs(std::asin(std::clamp(p.y, -1.0, 1.0))) + 0.12 * g.warp;
	return -0.9 * latitude + 1.5 * SphereCells::FbmStretched(p, 1.1, 5, m_noiseSeed + 83) - 0.3 * g.base / m_reliefLimit;
}

double TerrainPhysical::ShellIce(const vector3d &p, const Ground &g) const
{
	if (m_state.ground == M::ICE)
		return 1.0;
	if (m_iceLevel >= NONE_HIGH || !m_iceCapCracks)
		return 0.0;
	// as Paint lays the ice of a crust
	return Soft(CrustIceField(p, g) + 0.1 * Thinning(p, g), m_iceLevel, 0.015);
}

// Where ice shows first as the dark lag thins: in hollows and at random, not on rims.
// Added to a region's field, it turns a hard outline into scattered patches.
double TerrainPhysical::Thinning(const vector3d &p, const Ground &g) const
{
	const double relief = g.craters.large + g.craters.small;
	const double hollow = std::clamp(-relief / std::max(0.03 * m_reliefLimit, 1.0), -1.0, 1.0);
	return 1.5 * Fbm(p, 16.0, 4, m_noiseSeed + 81, 0.6) + 0.5 * hollow;
}

namespace {
	TerrainSpeedCounter s_pointSpeed, s_paintSpeed;
} // namespace

Terrain::SpeedTotals Terrain::GetSpeedTotals()
{
	SpeedTotals out;
	out.pointCalls = s_pointSpeed.calls;
	out.pointNanos = s_pointSpeed.nanoseconds;
	out.valleyCalls = TerrainValleys::CutSpeed().calls;
	out.valleyNanos = TerrainValleys::CutSpeed().nanoseconds;
	out.paintCalls = s_paintSpeed.calls;
	out.paintNanos = s_paintSpeed.nanoseconds;
	out.chunks = TerrainValleys::ChunksBuilt();
	out.chunkMicros = uint64_t(TerrainValleys::ChunkMilliseconds() * 1000.0);
	static_assert(sizeof(out.chunksAtLevel) / sizeof(out.chunksAtLevel[0]) == TerrainValleys::LEVEL_COUNT);
	for (int level = 0; level < TerrainValleys::LEVEL_COUNT; level++)
		out.chunksAtLevel[level] = TerrainValleys::ChunksBuilt(level);
	return out;
}

void Terrain::ResetSpeedCounters()
{
	s_pointSpeed.Reset();
	s_paintSpeed.Reset();
	TerrainValleys::ResetStats();
}

// The ground at its middle, at a detail set by the lake's size alone. (The patches around a lake are made at
// many band limits, and they would have seams if each found a level of its own.)
double TerrainPhysical::SmallLakeLevel(const vector3d &place, const double radius) const
{
	struct Known {
		const TerrainPhysical *owner = nullptr;
		double x = 0.0, y = 0.0, z = 0.0;
		double level = 0.0;
	};
	thread_local Known known[8];
	thread_local int next = 0;
	for (const Known &k : known) {
		if (k.owner == this && k.x == place.x && k.y == place.y && k.z == place.z)
			return k.level;
	}
	TerrainNoise::ScopedBandLimit bandLimit(0.5 * m_planetRadius / (0.25 * radius));
	t_findingLakeLevel = true;
	Point point;
	Evaluate(m_valleys.PlaceBeforeWander(place), point);
	t_findingLakeLevel = false;
	Known &slot = known[next];
	next = (next + 1) % 8;
	slot.owner = this;
	slot.x = place.x;
	slot.y = place.y;
	slot.z = place.z;
	slot.level = point.height;
	return point.height;
}

void TerrainPhysical::Evaluate(const vector3d &p, Point &out) const
{
	TerrainSampledTimer timer(s_pointSpeed);
	out.g = GroundAt(p);
	const Ground &g = out.g;
	out.place = TerrainHydro::PlaceOf(p);
	const double valley = m_hydro.valley.Empty() ? 0.0 : m_hydro.valley.Smooth(out.place);
	const double plainGround = g.base;
	if (m_state.sea > 0.0) {
		out.temperature = Temperature(p, g.warp);
		out.rain = m_hydro.HasMoisture() ? m_hydro.moisture.Smooth(out.place) : 0.3;
		// the coast's character reads the height before valleys are cut, or its roughness
		// switches on and off along the valleys' distance lines, far inland
		out.coast = Coast(p, out, plainGround);
	}
	out.volcano = m_volcanoes.At(p);
	const TerrainVolcanoes::Sample &volcano = out.volcano;
	out.lines = m_lines.At(p);
	// (an active shell's cracks, ridges and chaos only where it's bare ice: not across the lag, sand or soil of a warm, wet one)
	if (m_lines.IsActiveShell())
		out.shellIce = ShellIce(p, out.g);
	// cliffs and cracks are sharp on bare ground only: weather wears them down, and a volcano buries them
	double lineHeight = out.lines.height * out.shellIce * (1.0 - 0.85 * m_state.wearSmall) * (1.0 - volcano.influence);
	if (m_lines.IsActiveShell())
		out.mapRelief = CrustMaps(p, out) * out.shellIce;
	else if (m_grooved)
		out.mapRelief = Grooves(p, out);
	lineHeight += out.mapRelief;
	if (m_lavaAroundVolcanoes)
		out.lavaField = volcano.field;
	else if (!m_lavaField.Empty())
		out.lavaField = m_lavaField.Smooth(out.place) + 0.25 * Fbm(p, 64.0, 7, m_noiseSeed + 92, 0.6);
	// valleys and peaks die away towards a river, so what is beside it stays near its level
	out.hasRiver = m_hydro.RiverAt(p, out.river);
	const double calm = out.hasRiver ? std::clamp(out.river.trueDistance / out.river.valley, 0.0, 1.0) : 1.0;
	// no valleys across a volcano or fresh lava, and no water on a volcano
	const double fresh = m_oldLevel < NONE_HIGH ? Soft(out.lavaField, m_oldLevel - 0.08, 0.08) : 0.0;
	const double uncut = (1.0 - volcano.influence) * (1.0 - fresh);
	const double peaks = m_valleyAmount > 0.0 ? CutValleys(p, out, plainGround, calm * uncut) : 0.0;
	const bool lavaCovers = volcano.lava > 0.5 || volcano.flow > 0.5 || out.lavaField >= m_flowLevel || out.lavaField >= m_moltenLevel;
	const bool noWater = volcano.nearby || lavaCovers;
	if (noWater) {
		out.hasRiver = false;
		out.sideEdge = 1e30;
		out.smallLakeRatio = 1e30;
	}
	// a volcano keeps its own shape
	// (the grooved ground is younger than the dark, with less on it)
	const double youngRelief = YoungRelief(p);
	const double oldShare = OldShare(g);
	const double baseRelief = oldShare > 0.0 ? youngRelief + (OldRelief(p) - youngRelief) * oldShare : youngRelief;
	const double reliefScale = out.quiet * (1.0 - volcano.smooth) * (m_grooved ? 0.4 + 0.6 * out.grooveDark : 1.0);
	const double base = BaseShare(g) * baseRelief * reliefScale;
	const double relief = ((g.craters.small + g.small) * Keep(g) + OwnRelief(p, g) + LavaPlainsRelief(p, g)) * reliefScale + base;

	if (m_state.sea <= 0.0) {
		// the surface of a frozen sea is level; relief fades in on land
		const double ground = g.base - valley + peaks + volcano.height;
		double settledBase = base;
		if (m_seaLevel > NONE_LOW) {
			// (a frozen sea is young ice, as rough as a flood plain)
			const double land = Soft(ground - m_seaLevel, 0.0, 0.02 * m_reliefLimit);
			const double iceRelief = BASE_RELIEF_PLAINS * youngRelief * (1.0 - land);
			out.height = std::max(ground, m_seaLevel) + relief * land + iceRelief;
			settledBase = base * land + iceRelief;
		} else
			out.height = ground + relief;
		out.shore = g.base + volcano.height;
		out.height = SettleLava(out, out.height, settledBase) + lineHeight;
		out.height += Dunes(p, out, calm);
		return;
	}

	// (a volcano counts as ground for the sea, so it can make an island; the sea's level was set without them)
	out.shore = g.base + out.coast + volcano.height;
	out.underSea = out.shore < m_seaLevel;
	if (out.underSea) {
		// the sea floor stays flat at sea level; the real depth travels in the altitude
		out.height = m_seaLevel;
		return;
	}

	// land relief fades out towards the water, so a sharp waterline doesn't turn it into a false cliff
	const double broad = std::max(3.0 * m_hydro.slope.Linear(out.place), 0.001);
	const double band = std::clamp(1e4 * broad, 1.0, std::max(0.015 * m_reliefLimit, 1.0));
	const double toWater = Soft(out.shore - m_seaLevel, 0.0, band);
	double h = out.shore - valley + peaks + relief * toWater;

	// the map's lakes are flat, with flat ground beside them, so the ground meets the water at its edge
	if (!m_hydro.lake.Empty()) {
		const double field = m_hydro.lake.Smooth(out.place);
		if (field > 0.0 && !noWater) {
			out.lakeField = field + WaterEdge(p);
			const double surface = m_hydro.surface.Smooth(out.place);
			const double plain = Soft(out.lakeField, 0.08, 0.42);
			h = surface + (h - surface) * (1.0 - plain);
			if (out.lakeField > 0.5)
				h = surface;
		}
	}

	// Small lakes in the network's hollows: the ground is only cut down to the water, never lifted, so
	// no lake stands up out of low ground. They keep clear of main rivers and the map's lakes.
	if (out.smallLakeRatio < TerrainValleys::LAKE_REACH && calm >= 1.0 && out.lakeField <= 0.0 && !t_findingLakeLevel) {
		const double level = std::max(SmallLakeLevel(out.smallLakePlace, out.smallLakeRadius), m_seaLevel + 1.0);
		const double target = level + RIVER_SLOPE * std::max(out.smallLakeRatio - 1.0, 0.0) * out.smallLakeRadius;
		h -= std::clamp(TerrainValleys::LAKE_REACH - out.smallLakeRatio, 0.0, 1.0) * std::max(h - target, 0.0);
		if (out.smallLakeRatio < 1.0) {
			h = std::min(h, level);
			out.smallLakeEdge = (out.smallLakeRatio - 1.0) * out.smallLakeRadius;
		}
	}

	// Rivers. The water is drawn on the ground, so under its real width the ground is the water's level, or
	// water runs up and down hills. (SettleLevels keeps that level at or below the ground, so this doesn't
	// raise a ridge of water.) Beside it the ground comes down to that level by the bank, with no cliff.
	if (out.hasRiver) {
		const TerrainHydro::River &river = out.river;
		const double level = std::max(river.level, m_seaLevel + 1.0);
		if (river.trueDistance < 0.0) {
			h = level;
		} else if (river.centre < river.valley) {
			const double target = level + RIVER_SLOPE * river.trueDistance;
			const double trueHalfWidth = river.centre - river.trueDistance;
			if (h > target) {
				const double fade = std::clamp(1.0 - river.trueDistance / std::max(river.valley - trueHalfWidth, 1.0), 0.0, 1.0);
				h -= fade * fade * (h - target);
			} else {
				const double fade = std::clamp(1.0 - river.trueDistance / std::max(RIVER_LEVEE, trueHalfWidth), 0.0, 1.0);
				h += fade * fade * (target - h);
			}
		}
	}
	out.height = std::max(SettleLava(out, h, base * toWater) + lineHeight, m_seaLevel);
	out.height += Dunes(p, out, calm);
}

double TerrainPhysical::SettleLava(const Point &point, const double h, const double base) const
{
	const TerrainVolcanoes::Sample &volcano = point.volcano;
	double out = h;
	if (m_moltenLevel < NONE_HIGH && !m_ground.Empty()) {
		const double share = Soft(point.lavaField, m_moltenLevel, 0.02) * (1.0 - volcano.slope);
		if (share > 0.0) {
			// Lava flows downhill, so it doesn't stand above the ground around it: it lies at the map's smooth
			// ground there, a little lower, or at the ground itself where that is lower. Only where most of the
			// surface is molten is there one sea, whose level high ground is cut down to. (A liquid, so without the
			// base relief.)
			const double env = m_reliefLimit;
			const double level = h - base;
			const double settled = m_state.molten > 0.5 ? level + std::max(m_lavaSeaLevel - level, -0.25 * env) : std::min(level, m_ground.Smooth(point.place) - 0.004 * std::min(env, 7000.0));
			out = h + (settled - h) * share;
		}
	}
	// (a mountain block stands above any floor that reaches it)
	if (volcano.floor > 0.0)
		out += (std::min(out, volcano.level) - out) * std::clamp(volcano.floor, 0.0, 1.0) * (1.0 - point.g.block);
	return out;
}

// Where lava lies. Around the vents and in the lows; on a lava planet, mostly in the lows. Each kind of lava takes
// a share of the surface, from the physics, so the levels are found on the map.
void TerrainPhysical::BuildLavaFields(const TerrainHydro::Field &ground, const std::vector<float> &areas)
{
	const std::vector<vector3d> &places = m_volcanoes.FieldPlaces();
	if (m_state.lava <= 0.0 || places.empty())
		return;
	// Except on a world like Io or a lava planet, lava lies close around each volcano: the map's grid is far
	// too coarse for that, so each volcano works it out at the point. These are values of Sample::field.
	m_lavaAroundVolcanoes = !m_interior.iceCrust && !m_state.sulphur && !m_state.forcedLava && m_interior.heatPipe <= 0.5 && m_state.lavaPlanet <= 0.5;
	if (m_lavaAroundVolcanoes) {
		m_lavaLevel = 0.25;
		m_flowLevel = 0.6;
		m_oldLevel = 0.4;
		m_iceClearLevel = 0.25;
		return;
	}
	const int W = ground.width, H = ground.height;
	const double spread = m_volcanoes.FieldSpread();
	const double cosReach = std::cos(std::min(3.0 * spread, M_PI));
	const double env = m_reliefLimit;
	const double lowness = 0.3 + 2.5 * m_state.lavaPlanet;
	const double sea = m_seaLevel;
	m_lavaField.Resize(W, H);
	std::vector<float> values(size_t(W) * H);
	ParallelRows(H, [&](const int r) {
		TerrainNoise::ScopedBandLimit bandLimit(GROUND_MAX_FREQUENCY);
		for (int c = 0; c < W; c++) {
			const vector3d p = GridPoint(r, c, W, H);
			double sum = 0.0;
			for (const vector3d &place : places) {
				const double cosine = p.Dot(place);
				if (cosine < cosReach)
					continue;
				const double d = std::acos(std::clamp(cosine, -1.0, 1.0)) / spread;
				sum += std::exp(-d * d);
			}
			// (a lava planet's seas lie in the lows, not around its volcanoes, which stand on the high ground)
			const double v = sum * (1.0 - m_state.lavaPlanet) - lowness * ground.At(r, c) / env + 0.25 * Warp(p) + 0.45 * Fbm(p, 5.0, 3, m_noiseSeed + 91);
			m_lavaField.At(r, c) = float(v);
			const bool land = sea <= NONE_LOW || ground.At(r, c) >= sea;
			values[size_t(r) * W + c] = land ? float(v + 0.25 * Fbm(p, 64.0, 7, m_noiseSeed + 92, 0.6)) : -1e20f;
		}
	});
	m_lavaField.FindRange();

	std::vector<int> order(values.size());
	for (size_t i = 0; i < order.size(); i++)
		order[i] = int(i);
	std::sort(order.begin(), order.end(), [&values](const int a, const int b) { return values[a] > values[b]; });
	std::vector<double> sums(order.size());
	double total = 0.0, sum = 0.0;
	for (const float a : areas)
		total += a;
	for (size_t i = 0; i < order.size(); i++) {
		sum += areas[order[i]];
		sums[i] = sum / total;
	}
	const auto levelAt = [&](const double share) {
		if (share <= 0.0)
			return NONE_HIGH;
		const size_t i = std::min(size_t(std::lower_bound(sums.begin(), sums.end(), share) - sums.begin()), order.size() - 1);
		return double(values[order[i]]);
	};

	const double volcanicity = std::max(std::clamp(m_inputs.volcanicity, 0.0, 1.0), m_state.melted);
	const double molten = m_state.molten;
	// (on a world like Io, sulphur settles over all but the youngest lava, which stays yellow)
	const double sulphurCover = m_state.sulphur ? 0.25 : 1.0;
	const double bare = sulphurCover * (m_state.forcedLava ? m_state.lava - molten : (m_state.lava - molten) * (1.0 - 0.7 * m_state.wear));
	const double young = m_state.forcedLava ? 0.75 : (m_state.active ? 0.35 + 0.4 * Soft(volcanicity, 0.3, 0.6) : 0.0);
	m_lavaLevel = levelAt(m_state.lava);
	if (m_interior.iceCrust) {
		m_cryoLevel = levelAt(0.2 * m_state.lava);
		return;
	}
	// Seas only where it's a lava planet; otherwise it lies in lakes around the vents (TerrainVolcanoes)
	const bool seas = m_state.glow > 0.3 && (molten > 0.3 || m_state.lavaPlanet > 0.5);
	const double onField = seas ? molten : 0.0;
	m_moltenLevel = levelAt(onField);
	if (m_state.active && bare > 0.0) {
		m_flowLevel = levelAt(onField + bare * young);
		m_oldLevel = levelAt(onField + bare);
		m_ashLevel = levelAt(onField + bare + sulphurCover * 0.12 * std::sqrt(volcanicity) * (1.0 - m_state.sea));
	}
	m_iceClearLevel = levelAt(std::min(1.0, m_state.lava * 1.3 + 0.01));
	if (seas)
		m_ground = ground;
	if (molten > 0.5)
		m_lavaSeaLevel = WeightedLevelAbove(ground.values, areas, 0.65);
}

// Colour only, on Io: red rings around the largest vents (Pele), white frost halos around others (Prometheus).
void TerrainPhysical::BuildPlumes()
{
	if (!m_state.sulphur)
		return;
	std::vector<const TerrainVolcanoes::Vent *> erupting;
	for (const TerrainVolcanoes::Vent &vent : m_volcanoes.Vents()) {
		if (vent.erupting)
			erupting.push_back(&vent);
	}
	if (erupting.empty())
		return;
	std::sort(erupting.begin(), erupting.end(), [](const TerrainVolcanoes::Vent *a, const TerrainVolcanoes::Vent *b) { return a->km > b->km; });
	const size_t reds = std::min(erupting.size(), size_t(4));
	const size_t whites = std::min(erupting.size(), size_t(12));
	const int W = GROUND_WIDTH, H = W / 2;
	m_plumeRed.Resize(W, H);
	m_plumeWhite.Resize(W, H);
	ParallelRows(H, [&](const int r) {
		TerrainNoise::ScopedBandLimit bandLimit(GROUND_MAX_FREQUENCY);
		for (int c = 0; c < W; c++) {
			const vector3d p = GridPoint(r, c, W, H);
			const double uneven = std::clamp(Fbm(p, 24.0, 4, m_noiseSeed + 95, 0.55) / 0.35, -1.0, 1.0);
			double red = 0.0, white = 0.0;
			for (size_t i = 0; i < whites; i++) {
				const TerrainVolcanoes::Vent &vent = *erupting[i];
				const double cosine = p.Dot(vent.centre);
				if (cosine < 0.8)
					continue;
				const double angle = std::acos(std::clamp(cosine, -1.0, 1.0));
				if (i < reds) {
					// wider across than high, and open on one side, as plumes fall
					const double ringRadius = 0.27 + 0.11 * vent.seed;
					const double theta = std::atan2(p.Dot(vent.axisB), p.Dot(vent.axisA));
					const double d = (angle - ringRadius * (1.0 + 0.16 * std::sin(2.0 * theta + vent.seed * 40.0)) + 0.012 * uneven) / 0.03;
					red = std::max(red, std::exp(-d * d) * (0.55 + 0.45 * std::sin(theta + vent.seed * 90.0)));
				} else {
					// fallen frost: fading with distance, in streaks out from the vent
					const double haloRadius = 0.07 + 0.07 * vent.seed;
					const double theta = std::atan2(p.Dot(vent.axisB), p.Dot(vent.axisA));
					const double streaks = 0.5 + 0.5 * std::sin((14.0 + 10.0 * vent.seed) * theta + 2.5 * uneven + vent.seed * 60.0);
					const double fall = std::max(1.0 - angle / haloRadius, 0.0);
					white = std::max(white, std::pow(fall, 1.5) * (0.35 + 0.65 * streaks));
				}
			}
			m_plumeRed.At(r, c) = float(red);
			m_plumeWhite.At(r, c) = float(white);
		}
	});
	m_plumeRed.FindRange();
	m_plumeWhite.FindRange();
}

void TerrainPhysical::GetBaseHeights(const vector3d *vP, double *heightsOut, const size_t count) const
{
	const double invRadius = 1.0 / m_planetRadius;
	Point point;
	for (size_t i = 0; i < count; i++) {
		const vector3d p = vP[i].Normalized();
		point = Point();
		Evaluate(p, point);
		heightsOut[i] = std::max(point.height - m_datum, 0.0) * invRadius;
	}
}

// Biomes from real moisture and temperature; green along rivers. Deserts: dunes where it
// is hot and sand gathers, stony plains elsewhere, soil at the edges.
void TerrainPhysical::Climate(const vector3d &p, const Point &point, const std::function<void(M::Layer, double)> &paint) const
{
	const double sea = m_seaLevel > NONE_LOW ? m_seaLevel : 0.0;
	// (a volcano's height cools too, so plants stop on its way up)
	const double h0 = point.g.base + point.volcano.height;
	double m = point.rain;
	if (!m_hydro.corridor.Empty())
		m += 0.55 * m_hydro.corridor.Smooth(point.place);
	// The noise keeps the edges of deserts and forests from following anything smooth, such as
	// a valley's distance or a line of equal height, which would draw straight lines and arcs.
	m += 0.1 * std::clamp(Fbm(p, 60.0, 8, m_noiseSeed + 73, 0.6) / 0.35, -1.0, 1.0) + 0.06 * std::clamp(Fbm(p, 9.0, 4, m_noiseSeed + 74, 0.55) / 0.35, -1.0, 1.0);
	if (point.carved) {
		// Water gathers on valley floors, so they are greener than the ridges. This grows with the valleys,
		// which only begin at some height: all at once, it would draw an edge along that height.
		m += Soft(point.valleyRoom, 0.0, 300.0) * (0.4 * point.valley * point.valley - 0.12 * (1.0 - point.valley));
		// crests of high ground are bare, rock above scree
		const double alpine = (h0 - sea) / std::min(m_reliefLimit, 7000.0) + 0.07 * Fbm(p, 40.0, 6, m_noiseSeed + 72, 0.6) / 0.35;
		const double crest = 1.0 - Soft(point.valley, 0.35, 0.3);
		// (soils are thicker where life has weathered the rock, with less of it bare)
		const double soilDepth = 0.08 * std::min(m_inputs.life, 1.0);
		paint(M::ROCK, Soft(alpine, 0.3 + soilDepth, 0.06) * crest);
		paint(M::GRAVEL, Soft(alpine, 0.24 + soilDepth, 0.06) * crest);
	}
	const double T = point.temperature - m_state.lapse * 0.6 * std::max(h0 - sea, 0.0);
	const double desert = 1.0 - Soft(m, 0.10, 0.10);
	const double hot = Soft(T, 280.0, 5.0);
	// dune seas in the hot desert's lows; bare stony plains only where it is driest. (Not under the sea:
	// sand there is drawn in place of the water, for barrier islands.)
	if (desert > 0.0 && hot > 0.0 && !point.underSea) {
		const double dunes = Soft(Fbm(p, 3.2, 3, m_noiseSeed + 71) - 0.9 * (h0 - sea) / m_reliefLimit, -0.10, 0.20);
		paint(M::SAND, desert * hot * dunes);
	}
	paint(M::GRAVEL, 1.0 - Soft(m, 0.03, 0.08));
	paint(M::SOIL, desert);
	const double life = m_inputs.life;
	if (life >= 0.02) {
		// where life reaches: the wetter it must be, the less of it there is
		const double need = 0.15 / std::pow(std::max(life, 0.03), 0.75);
		// (no plants on a volcano, of any age)
		const double green = Soft(m, need - 0.03, 0.10) * (1.0 - point.volcano.influence);
		const double warm = Soft(T, 268.0, 2.0);
		if (life >= 0.3) {
			// rainforest and temperate forest where wet and warm, boreal forest where cold and wet,
			// savanna and prairie where drier, tundra beyond the trees
			const double trees = Soft(m, 0.38 + 0.1 * Soft(T, 292.0, 6.0), 0.12) * Soft(T, 270.0, 3.0);
			paint(M::FOREST, green * trees);
			paint(M::TUNDRA, green * (1.0 - warm));
			paint(M::GRASS, green);
		} else {
			// thin life: lichen and low growth only
			paint(M::TUNDRA, green);
		}
	}
	paint(M::SOIL, 1.0 - Soft(m, 0.17, 0.10));
}

// What lies on the ground, first come first served, as in the lab (materials.py, stage2)
void TerrainPhysical::Paint(const vector3d &p, const Point &point, Shares &shares) const
{
	shares.fill(0.0);
	double left = 1.0;
	auto paint = [&](const M::Layer layer, const double amount) {
		const double a = left * std::clamp(amount, 0.0, 1.0);
		shares[layer] += a;
		left -= a;
	};
	const Ground &g = point.g;
	const bool cryo = m_interior.iceCrust;
	const bool airless = !m_state.weather;
	const double ejecta = g.craters.ejecta;
	const M::Layer ground = m_state.ground;

	// barrier islands off low, smooth, sandy shores: separate islands at wandering depths, not one
	// strip along a single depth. Thin strips: not over a flat shelf, where one depth covers
	// a wide area, nor in a low basin of a lake.
	if (point.underSea && m_state.rain > 0.02 && (m_hydro.lake.Empty() || m_hydro.lake.Smooth(point.place) < 0.05)) {
		const double raw = (m_seaLevel - point.shore) / m_reliefLimit;
		if (raw < 0.012) {
			const double depth = raw + 0.006 * Fbm(p, 40.0, 9, m_noiseSeed + 116, 0.6);
			const double band = Soft(depth, 0.004, 0.001) * (1.0 - Soft(depth, 0.006, 0.001)) * Soft(m_hydro.slope.Linear(point.place), 0.0008, 0.0004);
			if (band > 0.0) {
				const double smooth = 1.0 - Soft(Fbm(p, 2.6, 3, m_noiseSeed + 111), -0.12, 0.3);
				const double gate = Soft(Fbm(p, 16.0, 3, m_noiseSeed + 114), 0.03, 0.05) * Soft(Fbm(p, 70.0, 7, m_noiseSeed + 117, 0.6), 0.02, 0.03);
				paint(M::SAND, Soft(band * smooth * gate * Soft(point.temperature, 272.0, 6.0), 0.3, 0.1));
			}
		}
	}

	// An active shell: clean pale ice on domes, reddish salts along its lines and in its freckles, and
	// a soft low stain over one side, fading into the clean ice
	if (m_lines.IsActiveShell()) {
		paint(M::ICE, point.lines.pale * point.shellIce);
		// (a small shell's plumes keep it clean)
		if (!m_lines.IsPolar()) {
			const double uneven = std::clamp(Fbm(p, 1.6, 4, m_noiseSeed + 301, 0.55) / 0.35, -1.0, 1.0);
			const double stain = 0.42 * Soft(p.Dot(m_lines.StainAxis()) + 0.3 * uneven, 0.1, 0.8);
			// (the rubble of chaos is brown, the plates in it are not)
			paint(M::SALT, point.shellIce * std::max({ 0.9 * point.lines.band, stain, 0.75 * point.crustRubble }));
		}
	}

	// a frozen sea
	if (m_state.sea <= 0.0 && m_state.frozen > 0.0 && g.base + point.volcano.height < m_seaLevel)
		paint(M::ICE, 1.0);

	// Volcanoes and lava come before ice: lava clears a band around itself, and flows lie on a volcano's snow.
	const TerrainVolcanoes::Sample &volcano = point.volcano;
	paint(M::ROCK, g.block);
	paint(M::LAVA, volcano.lava);
	paint(M::ROCK, volcano.rock);
	paint(M::LAVA_FLOW, volcano.flow);
	paint(M::BASALT, volcano.old);
	paint(M::ASH, volcano.ash);
	const double lavaField = point.lavaField;
	if ((!m_lavaField.Empty() || m_lavaAroundVolcanoes) && !point.underSea) {
		const double edge = 0.02;
		if (cryo) {
			paint(M::SNOW, Soft(lavaField, m_cryoLevel, edge));
			paint(M::ICE, Soft(lavaField, m_lavaLevel, edge));
		} else {
			// (not on a volcano's slopes, which it would run off; its flows are there instead)
			paint(M::LAVA, Soft(lavaField, m_moltenLevel, edge) * (1.0 - volcano.slope));
			if (m_flowLevel < NONE_HIGH) {
				// (around a volcano, it lies beyond the slopes; on them are its own flows, and its own ground)
				const double room = m_lavaAroundVolcanoes ? 1.0 - volcano.slope : 1.0;
				paint(M::LAVA_FLOW, Soft(lavaField, m_flowLevel, edge) * room);
				paint(M::BASALT, Soft(lavaField, m_oldLevel, edge) * room);
				// (ash thins out, with no edge)
				paint(M::ASH, Soft(lavaField, m_ashLevel, 0.12));
			}
		}
	}
	// Io's paterae have dark floors, and a few hold lava
	if (g.patera > 0.0) {
		if (m_state.glow > 0.3)
			paint(M::LAVA, 0.95 * g.pateraLava * (1.0 - volcano.slope));
		paint(M::BASALT, 0.95 * g.patera);
	}

	const double thin = (cryo && airless) || m_iceLevel < NONE_HIGH ? Thinning(p, g) : 0.0;
	if (cryo && airless) {
		// frost at the poles
		const double frostLine = p.y > 0.0 ? m_northFrost : m_southFrost;
		paint(M::SNOW, Soft(std::abs(std::asin(std::clamp(p.y, -1.0, 1.0))) + 0.22 * g.warp + 0.05 * thin, frostLine, 0.015));
		if (m_grooved) {
			// young craters throw clean white ice
			paint(M::SNOW, std::min(std::max(0.85 * Soft(ejecta, 0.05, 0.4), 1.4 * g.craters.rays), 1.0));
			if (ground == M::DIRTY_ICE)
				paint(M::ICE, 1.0 - point.grooveDark);
			else
				paint(M::DIRTY_ICE, point.grooveDark);
		}
		// old ice moons: dark regions; a past active episode leaves bright lanes along their edges
		if (m_darkLevel < NONE_HIGH) {
			double dark = m_hemispheres ? TwoToneSide(p, g, m_darkLevel, 1.0) : Soft(DarkField(p, g.warp) - 0.1 * thin, m_darkLevel, 0.015);
			if (m_interior.past)
				dark *= Soft(g.unitEdge, 0.22, 0.2);
			paint(M::DIRTY_ICE, dark);
		}
		if (ground == M::DIRTY_ICE && m_interior.past && !m_grooved)
			paint(M::ICE, 1.0 - Soft(g.unitEdge, 0.22, 0.2));
		// dark moons: where the lag is thin, ice shows through in patches
		if (m_brightLevel < NONE_HIGH)
			paint(M::ICE, m_hemispheres ? TwoToneSide(p, g, m_brightLevel, -1.0) : Soft(DarkField(p, g.warp) + 0.1 * thin, m_brightLevel, 0.015));
	}

	// ice and snow, coldest first
	if (m_iceLevel < NONE_HIGH) {
		const double spread = m_state.equatorTemperature - m_state.poleTemperature;
		// (a volcano's height cools, so tall ones have white tops)
		Ground withVolcano = g;
		withVolcano.base += volcano.height;
		const double field = cryo ? CrustIceField(p, g) + 0.1 * thin : IceField(p, withVolcano) + 0.08 * spread * thin;
		const double edge = cryo ? 0.015 : 1.5;
		const double cleared = m_iceClearLevel < NONE_HIGH && (!m_lavaField.Empty() || m_lavaAroundVolcanoes) ? 1.0 - Soft(lavaField, m_iceClearLevel, 0.02) * (1.0 - volcano.slope) : 1.0;
		const double core = Soft(field, m_iceCoreLevel, edge) * cleared;
		const double all = Soft(field, m_iceLevel, edge) * cleared;
		if (cryo && m_state.ice < 1.0) {
			paint(M::ICE, all);
		} else {
			// frozen lakes and rivers are bare ice, so they can still be told from the snow around them
			if (point.lakeField > 0.5 || point.smallLakeEdge < 0.0 || point.sideEdge < 0.0 || (point.hasRiver && point.river.distance < 0.0))
				paint(M::ICE, all);
			paint(M::ICE, core);
			paint(m_state.weather ? M::SNOW : M::ICE, all);
		}
	}

	// dry basins and deltas
	if (!m_hydro.dry.Empty()) {
		const double field = m_hydro.dry.Smooth(point.place);
		if (field > 0.0) {
			// (the field is coarse and smooth: without this, its edges are simple rings)
			const double d = field + 0.3 * Soft(field, 0.0, 0.2) * std::clamp(Fbm(p, 40.0, 6, m_noiseSeed + 173, 0.6) / 0.35, -1.0, 1.0);
			paint(M::SALT, Soft(d, 0.85, 0.1));
			paint(M::CRACKED_MUD, Soft(d, 0.35, 0.15));
		}
	}
	if (!m_hydro.mouth.Empty())
		paint(M::SOIL, Soft(m_hydro.mouth.Smooth(point.place), 0.12, 0.08));

	if (cryo) {
		if (ground == M::DIRTY_ICE && airless) {
			paint(M::ICE, Soft(ejecta, 0.45, 0.45));
			paint(M::GRAVEL, Soft(ejecta, 0.1, 0.35));
		}
		if (m_state.weather)
			paint(M::SAND, Soft(DuneField(p, g), m_beltLevel, 0.025));
	} else {
		if (m_plainsAreBasalt)
			paint(M::BASALT, g.plains);
		if (m_interior.lid)
			paint(M::ROCK, g.tessera);
		if (airless) {
			// the metal of a metal-rich small body shows in patches, and where a young crater has dug it up
			if (m_core.asteroid && m_inputs.metallicity > 0.7 && !m_interior.porous) {
				const double patches = Soft(Fbm(p, 5.0, 5, m_noiseSeed + 401, 0.55) / 0.35, 0.35, 0.25);
				paint(M::METAL, Soft(m_inputs.metallicity, 0.7, 0.15) * std::max(patches, Soft(ejecta, 0.25, 0.4)));
			}
			paint(M::GRAVEL, Soft(ejecta, 0.3, 0.4));
			paint(M::DUST, g.craters.floor * 0.9);
			if (m_core.asteroid && !m_interior.porous) {
				const double edge = std::max(0.05 * m_reliefLimit, 10.0);
				// (on a surface that wanders, or its edges are simple rings of equal height)
				const double lowness = -g.base + 2.0 * edge * std::clamp(Fbm(p, 12.0, 6, m_noiseSeed + 402, 0.6) / 0.35, -1.0, 1.0);
				paint(M::GRAVEL, Soft(lowness, m_gravelLevel, edge));
			}
		} else if (m_state.wetness > 0.0) {
			Climate(p, point, paint);
		} else {
			const double edge = std::max(0.012 * m_reliefLimit, 10.0);
			// the lows, on a surface that wanders, or their edges are simple rings of equal height
			const double lowness = -g.base + 3.0 * edge * std::clamp(Fbm(p, 40.0, 6, m_noiseSeed + 173, 0.6) / 0.35, -1.0, 1.0);
			if (m_core.temperature > 450.0) {
				paint(M::DUST, Soft(lowness, m_sandLevel, edge));
			} else {
				if (m_interior.fossil || m_core.temperature < 400.0) {
					paint(M::SALT, Soft(lowness, m_saltLevel, edge));
					paint(M::CRACKED_MUD, Soft(lowness, m_mudLevel, edge));
				}
				paint(M::SAND, Soft(lowness, m_sandLevel, edge));
			}
		}
	}
	paint(ground, 1.0);
	WindBorders(p, point, shares);
}

// Steep ground is bare rock on worlds with weather, with the slope at a fixed scale, so
// the colour doesn't change with the size of a patch
void TerrainPhysical::Steep(const vector3d &p, const vector3d &normal, Shares &shares) const
{
	// (a small body's walls are bare too, with nothing to wear them)
	if ((!m_state.weather && !m_core.asteroid) || m_interior.iceCrust)
		return;
	const M::Layer bare = m_interior.porous ? M::ICE : M::ROCK;
	const double c = std::clamp(p.Dot(normal), 0.05, 1.0);
	const double tangent = std::sqrt(std::max(1.0 - c * c, 0.0)) / c;
	const double steep = Soft(tangent, m_steepSlope, 0.6 * m_steepSlope);
	if (steep <= 0.0)
		return;
	double moved = 0.0;
	for (int i = 0; i < M::LAYER_COUNT; i++) {
		// (lava runs down steep ground, and stays there)
		if (i == bare || i == M::LAVA || i == M::LAVA_FLOW || shares[i] <= 0.0)
			continue;
		const double k = (i == M::SNOW ? 0.7 : 1.0) * steep * shares[i];
		shares[i] -= k;
		moved += k;
	}
	shares[bare] += moved;
}

// metres above the water, below 0 in it: the sea's real depth, and an edge for lakes and rivers
double TerrainPhysical::AltitudeOf(const Point &point, const Shares &shares) const
{
	if (point.underSea)
		return std::min(point.shore - m_seaLevel, -1.0);
	double altitude = std::max(point.height - m_seaLevel, 0.5);
	// under snow and ice, water is frozen and hidden
	if (shares[M::ICE] + shares[M::SNOW] < 0.5) {
		if (point.lakeField > 0.0)
			altitude = std::min(altitude, (0.5 - point.lakeField) * LAKE_DEPTH);
		if (point.hasRiver)
			altitude = std::min(altitude, std::max(point.river.distance * RIVER_BANK, -RIVER_DEEPEST));
		if (point.sideEdge < 1e29)
			altitude = std::min(altitude, std::max(point.sideEdge * RIVER_BANK, -RIVER_DEEPEST));
		if (point.smallLakeEdge < 0.0)
			altitude = std::min(altitude, std::max(point.smallLakeEdge * RIVER_BANK, -RIVER_DEEPEST));
	}
	return altitude;
}

vector3d TerrainPhysical::Dirty(const vector3d &p, const Point &point, const Shares &shares, const vector3d &colour) const
{
	// how much it shows: all on bare ground, less on what's clean or growing, none on what has just melted
	const double weight = std::clamp(shares[M::ROCK] + shares[M::REGOLITH] + shares[M::DUST] + shares[M::GRAVEL] + shares[M::BASALT] + shares[M::DIRTY_ICE] + shares[M::METAL] + shares[M::SAND] + shares[M::SOIL] + shares[M::CRACKED_MUD] + shares[M::ASH] + 0.5 * (shares[M::ICE] + shares[M::SNOW] + shares[M::SALT]) + 0.25 * (shares[M::GRASS] + shares[M::FOREST] + shares[M::FOREST_FAR] + shares[M::TUNDRA]), 0.0, 1.0) * m_dirtStrength;
	if (weight <= 0.0)
		return colour;
	const Ground &g = point.g;
	const double large = Fbm(p, 1.6, 3, m_noiseSeed + 341, 0.55) / 0.35, other = Fbm(p, 2.3, 3, m_noiseSeed + 344, 0.55) / 0.35;
	const double middle = Fbm(p, 7.0, 3, m_noiseSeed + 342, 0.55) / 0.35, small = Fbm(p, 40.0, 9, m_noiseSeed + 343, 0.6) / 0.35;
	// it gathers in low ground and in hollows; rims stand clear
	const double low = std::clamp(-g.large / std::max(0.2 * m_reliefLimit, 1.0), -1.0, 1.0);
	const double hollow = std::clamp(-(g.craters.large + g.craters.small) / std::max(0.03 * m_reliefLimit, 1.0), -1.0, 1.0);
	const double cause = 0.25 * low + 0.3 * hollow;
	// and what a young crater has thrown out is clean
	const double kept = weight * (1.0 - 0.85 * std::min(g.craters.ejecta + g.craters.rays, 1.0));
	const double shares3[3] = {
		0.7 * kept * Soft(large + 0.5 * middle + 0.25 * small + cause, -0.1, 0.7),
		0.6 * kept * Soft(other + 0.6 * middle - 0.4 * small + cause, 0.2, 0.7),
		0.45 * kept * Soft(-large - 0.4 * middle + 0.5 * small - cause, 0.35, 0.6),
	};
	vector3d c = colour;
	for (int i = 0; i < 3; i++) {
		if (shares3[i] <= 0.0)
			continue;
		const double brightness = 0.3 * c.x + 0.6 * c.y + 0.1 * c.z;
		c = c * (1.0 - shares3[i]) + m_dirtHues[i] * (brightness * m_dirtLight[i] * shares3[i]);
	}
	// and it's never quite even, at any size
	return c * (1.0 + 0.3 * kept * std::clamp(0.6 * middle + 0.6 * small, -1.0, 1.0));
}

vector3d TerrainPhysical::ColourFrom(const vector3d &p, const Point &point, const Shares &shares, const double altitude) const
{
	const Ground &g = point.g;
	vector3d c(0.0, 0.0, 0.0);
	if (altitude < 0.0 && shares[M::SAND] < 0.5) {
		// water. Deep is darker; shelves are shallow and pale. (The engine takes red for land here.)
		if (point.underSea) {
			const double d = std::clamp((m_seaLevel - point.shore) / std::max(m_reliefLimit * 0.2, 1.0), 0.0, 1.0);
			const double e = std::pow(1.0 - d, 2.2);
			c = (m_shallowTone * e + m_seaTone * (1.0 - e)) * (1.0 - 0.35 * d);
			// reefs: bright rims in the shallows of warm seas, broken into patches
			if (m_inputs.life >= 0.3 && d < 0.05 && point.temperature > 289.0) {
				const double rim = Soft(d, 0.001, 0.005) * (1.0 - Soft(d, 0.02, 0.03)) * Soft(point.temperature, 289.0, 5.0);
				const double patches = Soft(Fbm(p, 45.0, 5, m_noiseSeed + 363, 0.6), -0.06, 0.16);
				const double reef = 0.7 * rim * patches * std::min(m_inputs.life / 0.6, 1.0);
				c = c * (1.0 - reef) + vector3d(0.05, 0.62, 0.60) * reef;
			}
		} else {
			c = point.lakeField > 0.5 || point.smallLakeEdge < 0.0 ? m_lakeTone : m_riverTone;
		}
		return vector3d(std::clamp(c.x, 0.0, 0.05), std::clamp(c.y, 0.0, 1.0), std::clamp(c.z, 0.0, 1.0));
	}

	for (int i = 0; i < M::LAYER_COUNT; i++) {
		if (shares[i] > 0.0)
			c += m_tones[i] * shares[i];
	}
	// thin dark cracks, and the dark floors of deep fractures
	if (m_lines.IsActiveShell()) {
		// dirt in uneven patches at all sizes, so the ice isn't one even tone
		if (m_iceDirt > 0.0) {
			const double dirt = 0.5 + 0.5 * std::clamp(Fbm(p, 7.0, 9, m_noiseSeed + 321, 0.62) / 0.35, -1.0, 1.0);
			const double share = 0.55 * m_iceDirt * 2.0 * dirt;
			c = c * (1.0 - share) + Times(vector3d(0.27, 0.235, 0.20), m_tint) * m_bright * share;
		}
		c = c * (1.0 - 0.6 * point.shellIce * std::max(point.lines.crack, point.crustCrack)) * (1.0 - 0.35 * point.shellIce * point.lines.dark);
		// coarse fresh ice around an active pole looks bluer
		if (m_lines.IsPolar())
			c = c * (1.0 - 0.22 * g.polarCap) + Times(vector3d(0.70, 0.86, 0.96), m_tint) * m_bright * (0.22 * g.polarCap);
		const double slot = point.lines.slot * point.shellIce;
		const vector3d floorTone = point.lines.slotBlue ? vector3d(0.30, 0.55, 0.66) : vector3d(0.28, 0.13, 0.06);
		c = c * (1.0 - slot) + Times(floorTone, m_tint) * m_bright * slot;
	}
	// what the plumes laid down; not on molten lava, which is younger
	if (!m_plumeRed.Empty()) {
		const double uncovered = 1.0 - shares[M::LAVA];
		const double red = 0.6 * m_plumeRed.Smooth(point.place) * uncovered;
		const double white = 0.45 * std::clamp(m_plumeWhite.Smooth(point.place), 0.0, 1.0) * uncovered;
		c = c * (1.0 - red) + Times(vector3d(0.50, 0.15, 0.06), m_tint) * m_bright * red;
		c = c * (1.0 - white) + Times(vector3d(0.92, 0.90, 0.82), m_tint) * m_bright * white;
	}
	// plants: wetter is deeper green; drier grass turns straw; cold forest is darker and bluer
	const double plants = shares[M::FOREST] + shares[M::GRASS] + shares[M::TUNDRA];
	if (plants > 0.0) {
		const double wet = std::clamp((point.rain - 0.25) / 0.6, 0.0, 1.0);
		const double thin = std::min(std::sqrt(std::max(m_inputs.life, 0.05)), 1.0);
		const vector3d straw(0.62, 0.5, 0.24), soil = m_tones[M::SOIL];
		// no two regions quite alike: yellower or bluer, paler or deeper
		const double region = std::clamp(Fbm(p, 3.0, 4, m_noiseSeed + 361, 0.55) / 0.35, -1.0, 1.0);
		const double local = std::clamp(Fbm(p, 18.0, 4, m_noiseSeed + 362, 0.55) / 0.35, -1.0, 1.0);
		const vector3d regional(1.0 + 0.22 * region, 1.0 - 0.04 * region, 1.0 - 0.25 * region);
		// wetlands: dark and rich in deltas and along rivers on low ground
		double wetland = 0.0;
		if (!m_hydro.mouth.Empty())
			wetland = 1.2 * m_hydro.mouth.Smooth(point.place);
		if (!m_hydro.corridor.Empty() && m_seaLevel > NONE_LOW)
			wetland += 0.5 * m_hydro.corridor.Smooth(point.place) * (1.0 - Soft((point.shore - m_seaLevel) / std::max(m_reliefLimit, 1.0), 0.0, 0.03));
		wetland = 0.6 * std::clamp(wetland, 0.0, 1.0);
		auto plant = [&](const M::Layer layer, const double dryness) {
			if (shares[layer] <= 0.0)
				return;
			vector3d col = m_vegetation * (1.0 + 0.5 * (1.0 - wet));
			const double d = dryness * (1.0 - wet);
			col = col * (1.0 - d) + straw * d;
			col = Times(col, regional) * (1.0 + 0.15 * local);
			if (layer != M::TUNDRA)
				col = col * (1.0 - wetland) + Times(m_vegetation, vector3d(0.6, 0.55, 0.9)) * wetland;
			if (layer == M::FOREST) {
				// (rainforest is deep, temperate forest paler)
				col = col * (0.72 - 0.14 * Soft(point.temperature, 285.0, 12.0));
				const double cold = 0.4 * (1.0 - Soft(point.temperature, 280.0, 8.0));
				col = col * (1.0 - cold) + vector3d(0.04, 0.10, 0.10) * cold;
			} else if (layer == M::TUNDRA) {
				col = col * 0.55 + vector3d(0.22, 0.2, 0.14);
			}
			col = Times(col, m_tint) * m_bright * thin + soil * (1.0 - thin);
			c += (col - m_tones[layer]) * shares[layer];
		};
		plant(M::FOREST, 0.0);
		plant(M::GRASS, 0.55);
		plant(M::TUNDRA, 0.3);
	}
	// shades with a cause: the crustal units on hard ground, fresh ejecta and rays with no air to dull them
	const double hard = shares[M::REGOLITH] + shares[M::ROCK] + shares[M::BASALT] + shares[M::DIRTY_ICE] + shares[M::ICE];
	// (not on a grooved moon, where it would draw the units' outlines over what has its own)
	if (!m_grooved)
		c = c * (1.0 + 0.13 * (g.unitClass - 0.5) * hard);
	// Soft ground shows what lies under it: older units are darker and redder. They fade out towards a unit's
	// edge, as loose ground has no sharp edges of its own. Wet soil is darker.
	const double soft = shares[M::SOIL] + shares[M::SAND] + shares[M::DUST] + shares[M::GRAVEL] + shares[M::GRASS] + shares[M::FOREST] + shares[M::TUNDRA] + shares[M::CRACKED_MUD] + shares[M::SALT];
	if (soft > 0.0) {
		const double unit = (g.unitClass - 0.5) * Soft(g.unitEdge, 0.0, 1.0);
		const double shade = soft * 0.28 * unit;
		c = Times(c, vector3d(1.0 + 0.8 * shade + 0.1 * soft * unit, 1.0 + 0.8 * shade, 1.0 + 0.8 * shade - 0.12 * soft * unit));
	}
	if (shares[M::SOIL] > 0.0 && m_state.wetness > 0.0)
		c = c * (1.0 - 0.3 * shares[M::SOIL] * std::clamp(point.rain, 0.0, 1.0));
	if (m_state.weather) {
		// Ground lit by the air shows its shape in its colour, at noon too: hollows and valley floors hold
		// shade, damp and fine dark ground, and rises and crests are paler. Less so under snow and ice.
		const double frozen = std::clamp(shares[M::SNOW] + shares[M::ICE], 0.0, 1.0);
		const double strength = 1.0 - 0.6 * frozen;
		double shape = m_landform.Empty() ? 0.0 : 0.3 * m_landform.Smooth(point.place);
		if (point.carved)
			shape -= 0.14 * Soft(point.valleyRoom, 0.0, 300.0) * (point.valley - 0.4);
		// Then what a colour of its own can't say: ground varies from place to place at every
		// size, in how light it is and how warm, and the height of the land pales it a little.
		const double light = std::clamp(Fbm(p, 4.0, 7, m_noiseSeed + 181, 0.62) / 0.35, -1.0, 1.0);
		const double warm = std::clamp(Fbm(p, 6.0, 4, m_noiseSeed + 182, 0.6) / 0.35, -1.0, 1.0);
		const double ground = m_seaLevel > NONE_LOW ? m_seaLevel : m_datum;
		const double high = std::clamp((point.height - ground) / std::min(m_reliefLimit, 7000.0), 0.0, 1.0);
		const double overall = 1.0 + strength * (shape + 0.16 * light + 0.12 * high);
		c = Times(c, vector3d(overall * (1.0 + 0.08 * strength * warm), overall, overall * (1.0 - 0.1 * strength * warm)));
	}
	// dark sand seas, as on Mars and Titan
	if (shares[M::SAND] > 0.0 && !m_sandSea.Empty())
		c = c * (1.0 - 0.2 * shares[M::SAND] * Soft(m_sandSea.Smooth(point.place), 0.3, 0.4));
	c = Dirty(p, point, shares, c);
	// Hollows see less of the sky, at noon too, the less so the more air lights them from all round. From the ground's
	// own shape at this place, so vertices and tiles agree at any detail, and nothing shows the patches' squares: craters
	// (large basins only a little, or their whole floors would go dark) and the low ground of the maps. Not lines: drawn
	// as wide as the samples from afar, they would go dark as thick strokes across the whole body.
	{
		const double smallHollow = Keep(g) * Soft(-g.craters.small / std::max(0.008 * m_reliefLimit, 1.0), 0.0, 1.0);
		const double largeHollow = Soft(-g.craters.large / std::max(0.05 * m_reliefLimit, 1.0), 0.0, 1.0);
		// (only close up: from afar, the same map's low ground would repeat across the body as a faint grid)
		const double maxFrequency = TerrainNoise::s_maxFrequency;
		const double samplesKm = maxFrequency > 0.0 ? 0.5 / maxFrequency * m_core.radius * 6378.135 : 0.0;
		const double mapHollow = Soft(-point.mapRelief / 120.0, 0.0, 1.0) * (1.0 - Soft(samplesKm, 0.4, 1.1));
		c = c * (1.0 - 0.45 * (1.0 - 0.6 * std::min(m_state.air, 1.0)) * std::min(smallHollow + 0.4 * largeHollow + mapHollow, 1.0));
	}
	if (!m_state.weather)
		c = c * (1.0 + 0.3 * g.craters.ejecta + 0.55 * g.craters.rays);
	return vector3d(std::clamp(c.x, 0.0, 1.0), std::clamp(c.y, 0.0, 1.0), std::clamp(c.z, 0.0, 1.0));
}

vector3d TerrainPhysical::GetColor(const vector3d &p, const double height, const vector3d &norm) const
{
	vector3d colour;
	uint8_t weights[M::SLOT_COUNT];
	GetSurface(p, height, norm, colour, weights);
	return colour;
}

namespace {
	// The engine asks for a point's colour, then its altitude. Both come from one evaluation, so
	// the altitude of the point just coloured is kept, per thread.
	struct LastAltitude {
		const void *owner = nullptr;
		vector3d p = vector3d(0.0, 0.0, 0.0);
		double altitude = 0.0;
	};
	thread_local LastAltitude s_lastAltitude;
} // namespace

void TerrainPhysical::GetSurface(const vector3d &p, const double, const vector3d &normal, vector3d &colourOut, uint8_t weightsOut[M::SLOT_COUNT]) const
{
	const vector3d q = p.Normalized();
	Point point;
	Evaluate(q, point);
	Shares shares;
	{
		TerrainSampledTimer timer(s_paintSpeed);
		Paint(q, point, shares);
	}
	Steep(q, normal, shares);
	const double altitude = m_state.sea > 0.0 ? AltitudeOf(point, shares) : 1.0;
	colourOut = ColourFrom(q, point, shares, altitude);

	s_lastAltitude.owner = this;
	s_lastAltitude.p = p;
	s_lastAltitude.altitude = altitude;

	std::fill(weightsOut, weightsOut + M::SLOT_COUNT, uint8_t(0));
	for (int i = 0; i < M::LAYER_COUNT; i++) {
		if (shares[i] > 0.0 && m_slots[i] >= 0)
			weightsOut[m_slots[i]] = uint8_t(std::clamp(int(weightsOut[m_slots[i]]) + int(shares[i] * 255.0 + 0.5), 0, 255));
	}
}

double TerrainPhysical::GetSurfaceAltitude(const vector3d &p, const double height) const
{
	if (m_state.sea <= 0.0)
		return height * m_planetRadius;
	if (s_lastAltitude.owner == this && s_lastAltitude.p.x == p.x && s_lastAltitude.p.y == p.y && s_lastAltitude.p.z == p.z)
		return s_lastAltitude.altitude;
	const vector3d q = p.Normalized();
	Point point;
	Evaluate(q, point);
	Shares shares;
	Paint(q, point, shares);
	return AltitudeOf(point, shares);
}

std::vector<Terrain::Feature> TerrainPhysical::FindFeatures(const std::string &kind, const vector3d &around) const
{
	std::vector<Feature> out;
	if (kind == "volcano") {
		// those erupting first
		for (const bool erupting : { true, false }) {
			for (const TerrainVolcanoes::Vent &vent : m_volcanoes.Vents()) {
				if (vent.erupting == erupting)
					out.push_back({ vent.centre, 2000.0 * vent.km });
			}
		}
		return out;
	}
	if (kind == "lane") {
		if (!m_grooved)
			return out;
		for (int i = 0; i < 3000 && out.size() < 300; i++) {
			const vector3d place = EvenPoint(i, 3000);
			Point point;
			Grooves(place, point);
			if (point.grooveDark < 0.3)
				out.push_back({ place, 2.0 * std::sqrt(4.0 * M_PI / 3000.0) * m_planetRadius });
		}
		return out;
	}
	if (kind == "chaos") {
		if (!m_lines.IsActiveShell())
			return out;
		for (int i = 0; i < 3000 && out.size() < 300; i++) {
			const vector3d place = EvenPoint(i, 3000);
			if (CrustChaos(place) > 0.95)
				out.push_back({ place, 2.0 * std::sqrt(4.0 * M_PI / 3000.0) * m_planetRadius });
		}
		return out;
	}
	if (kind == "stripe") {
		if (m_lines.IsPolar())
			out.push_back({ m_lines.Pole(), 0.3 * m_planetRadius });
		return out;
	}
	if (kind == "line") {
		for (const std::pair<vector3d, double> &middle : m_lines.Middles())
			out.push_back({ middle.first, middle.second * m_planetRadius });
		return out;
	}
	if (kind == "patera" || kind == "block") {
		for (const TerrainVolcanoes::Item &item : kind == "patera" ? m_volcanoes.Paterae() : m_volcanoes.Blocks())
			out.push_back({ item.centre, 2.0 * item.radius * m_planetRadius });
		return out;
	}
	if (kind == "lavafield") {
		for (const TerrainVolcanoes::Item &lake : m_volcanoes.Lakes())
			out.push_back({ lake.centre, 2.0 * lake.radius * m_planetRadius });
		if (m_lavaField.Empty() || m_moltenLevel >= NONE_HIGH)
			return out;
		const int W = m_lavaField.width, H = m_lavaField.height;
		const int step = 3;
		for (int r = 0; r < H; r += step) {
			for (int c = 0; c < W; c += step) {
				if (m_lavaField.At(r, c) < m_moltenLevel + 0.1)
					continue;
				if (m_seaLevel > NONE_LOW && m_ground.At(r, c) < m_seaLevel)
					continue;
				out.push_back({ GridPoint(r, c, W, H), 2.0 * M_PI * m_planetRadius / W * step });
			}
		}
		return out;
	}
	if (kind == "smalllake") {
		if (!m_valleys.Active())
			return out;
		std::vector<TerrainValleys::LakeSpot> spots;
		m_valleys.LakesAround(around, spots);
		std::sort(spots.begin(), spots.end(), [&around](const TerrainValleys::LakeSpot &a, const TerrainValleys::LakeSpot &b) {
			return a.place.Dot(around) > b.place.Dot(around);
		});
		// only those that are there: some are under the sea, or too near a river
		int tried = 0;
		for (const TerrainValleys::LakeSpot &spot : spots) {
			if (tried++ >= 200 || out.size() >= 30)
				break;
			TerrainNoise::ScopedBandLimit bandLimit(0.5 * m_planetRadius / (0.25 * spot.radius));
			const vector3d place = m_valleys.PlaceBeforeWander(spot.place);
			Point point;
			Evaluate(place, point);
			if (point.smallLakeEdge < 0.0)
				out.push_back({ place, 2.0 * spot.radius });
		}
		return out;
	}
	if (kind == "river" || kind == "fork" || kind == "mouth") {
		std::vector<TerrainHydro::RiverNode> all, forks, ends;
		m_hydro.RiverNodes(all, forks, ends);
		for (const TerrainHydro::RiverNode &node : kind == "river" ? all : (kind == "fork" ? forks : ends))
			out.push_back({ node.position, node.width });
	} else if (kind == "lake" && !m_hydro.lake.Empty()) {
		// one place for each lake: the cell nearest its middle
		const TerrainHydro::Field &lake = m_hydro.lake;
		const int W = lake.width, H = lake.height;
		const double cellMetres = 2.0 * M_PI * m_planetRadius / W;
		std::vector<char> seen(lake.values.size(), 0);
		std::vector<int> open, cells;
		for (size_t start = 0; start < lake.values.size(); start++) {
			if (seen[start] || lake.values[start] < 0.5f)
				continue;
			seen[start] = 1;
			open.assign(1, int(start));
			cells.clear();
			double area = 0.0;
			vector3d sum(0.0, 0.0, 0.0);
			while (!open.empty()) {
				const int i = open.back();
				open.pop_back();
				cells.push_back(i);
				const int r = i / W, c = i % W;
				const vector3d q = GridPoint(r, c, W, H);
				sum += q;
				area += std::cos(M_PI / 2.0 - (r + 0.5) / H * M_PI) * cellMetres * cellMetres;
				const int around[4] = { r > 0 ? i - W : -1, r + 1 < H ? i + W : -1, r * W + (c + 1) % W, r * W + (c + W - 1) % W };
				for (const int j : around) {
					if (j >= 0 && !seen[j] && lake.values[j] >= 0.5f) {
						seen[j] = 1;
						open.push_back(j);
					}
				}
			}
			const vector3d middle = sum.Normalized();
			int best = cells[0];
			double bestDot = -2.0;
			for (const int i : cells) {
				const double d = GridPoint(i / W, i % W, W, H).Dot(middle);
				if (d > bestDot) {
					bestDot = d;
					best = i;
				}
			}
			out.push_back({ GridPoint(best / W, best % W, W, H), std::sqrt(area) });
		}
	} else if (kind == "coast" && m_state.sea > 0.0 && !m_ground.Empty()) {
		// where the map's ground crosses sea level, then along that line to the shore, at the map's detail
		TerrainNoise::ScopedBandLimit bandLimit(GROUND_MAX_FREQUENCY);
		const int W = m_ground.width, H = m_ground.height;
		const auto aboveSea = [this](const vector3d &p) {
			Point point;
			point.g = GroundAt(p);
			point.place = TerrainHydro::PlaceOf(p);
			point.temperature = Temperature(p, point.g.warp);
			point.rain = m_hydro.HasMoisture() ? m_hydro.moisture.Smooth(point.place) : 0.3;
			return point.g.base + Coast(p, point, point.g.base) - m_seaLevel;
		};
		for (int r = 0; r < H; r++) {
			for (int c = 0; c < W; c++) {
				// not all of them, or this takes seconds
				if ((r + c) % 6 != 0)
					continue;
				const bool land = m_ground.values[size_t(r) * W + c] >= m_seaLevel;
				const int e = (c + 1) % W, s = std::min(r + 1, H - 1);
				const bool edgeEast = (m_ground.values[size_t(r) * W + e] >= m_seaLevel) != land;
				const bool edgeSouth = (m_ground.values[size_t(s) * W + c] >= m_seaLevel) != land;
				if (!edgeEast && !edgeSouth)
					continue;
				vector3d a = GridPoint(r, c, W, H), b = edgeEast ? GridPoint(r, e, W, H) : GridPoint(s, c, W, H);
				const bool aLand = aboveSea(a) >= 0.0;
				if ((aboveSea(b) >= 0.0) == aLand)
					continue;
				for (int i = 0; i < 6; i++) {
					const vector3d m = (a + b).Normalized();
					if ((aboveSea(m) >= 0.0) == aLand)
						a = m;
					else
						b = m;
				}
				out.push_back({ (a + b).Normalized(), 0.0 });
			}
		}
	} else if (kind == "peak" && !m_ground.Empty()) {
		// the map's high points; close up they are the highest ground around, not one summit
		const int W = m_ground.width, H = m_ground.height;
		const double high = m_valleySea + 0.25 * (double(m_ground.high) - m_valleySea);
		for (int r = 1; r + 1 < H; r++) {
			for (int c = 0; c < W; c++) {
				const float v = m_ground.values[size_t(r) * W + c];
				if (v < high)
					continue;
				bool top = true;
				for (int y = -1; y <= 1 && top; y++) {
					for (int x = -1; x <= 1; x++) {
						if ((x != 0 || y != 0) && m_ground.values[size_t(r + y) * W + (c + x + W) % W] > v) {
							top = false;
							break;
						}
					}
				}
				if (top)
					out.push_back({ GridPoint(r, c, W, H), double(v) - m_valleySea });
			}
		}
	}
	return out;
}

bool TerrainPhysical::GetHeightLayers(const vector3d &p, HeightLayers &out) const
{
	Point point;
	Evaluate(p.Normalized(), point);
	out.total = point.height;
	out.shape = point.g.large;
	out.largeCraters = point.g.craters.large;
	out.smallCraters = point.g.craters.small;
	out.smallRelief = point.g.small;
	out.lines = point.lines.height;
	out.maps = point.mapRelief;
	out.volcano = point.volcano.height;
	return true;
}

std::string TerrainPhysical::DescribePoint(const vector3d &p0) const
{
	const vector3d p = p0.Normalized();
	Point point;
	Evaluate(p, point);
	Shares shares;
	Paint(p, point, shares);
	const double altitude = m_state.sea > 0.0 ? AltitudeOf(point, shares) : 1.0;

	char line[160];
	std::string out;
	if (point.underSea) {
		snprintf(line, sizeof(line), "Sea, %.0f m deep", m_seaLevel - point.shore);
		out = line;
	} else {
		if (point.lakeField > 0.5) {
			snprintf(line, sizeof(line), "Lake%s", altitude < 0.0 ? "" : " (frozen)");
			out = line;
		} else if (point.hasRiver && point.river.distance < 0.0) {
			snprintf(line, sizeof(line), "River%s, %.0f m wide, %.0f m from its middle", altitude < 0.0 ? "" : " (frozen)", 2.0 * point.river.halfWidth, point.river.centre);
			out = line;
		} else if (point.smallLakeEdge < 0.0) {
			snprintf(line, sizeof(line), "Small lake%s, %.0f m across", altitude < 0.0 ? "" : " (frozen)", 2.0 * point.smallLakeRadius);
			out = line;
		} else if (point.sideEdge < 0.0) {
			snprintf(line, sizeof(line), "Side river%s, %.0f m wide", altitude < 0.0 ? "" : " (frozen)", point.sideWidth);
			out = line;
		} else if (point.hasRiver) {
			snprintf(line, sizeof(line), "Land, %.0f m from a river %.0f m wide", point.river.distance, 2.0 * point.river.halfWidth);
			out = line;
		} else {
			out = "Land, no main river near";
		}
		const double level = m_seaLevel > NONE_LOW ? m_seaLevel : m_datum;
		snprintf(line, sizeof(line), "\nGround %.0f m above %s", point.height - level, m_seaLevel > NONE_LOW ? "sea" : "the lowest ground");
		out += line;
	}
	if (m_state.sea > 0.0) {
		snprintf(line, sizeof(line), "\n%.0f K, rain %.2f", point.temperature, point.rain);
		out += line;
	}
	if (point.carved) {
		snprintf(line, sizeof(line), "\n%s (%.2f; 1 is a valley floor)", point.valley > 0.5 ? "Valley" : "Ridge", point.valley);
		out += line;
	}
	if (point.volcano.kind >= 0) {
		static const char *const kinds[] = { "cone", "shield", "pit" };
		snprintf(line, sizeof(line), "\nOn a %s, %s", kinds[point.volcano.kind], point.volcano.erupting ? "erupting" : "quiet");
		out += line;
	}
	std::string materials;
	for (int k = 0; k < M::LAYER_COUNT; k++) {
		if (shares[k] >= 0.05) {
			snprintf(line, sizeof(line), " %s %.0f %%", PlanetPhysics::LayerName(M::Layer(k)), 100.0 * shares[k]);
			materials += line;
		}
	}
	if (!materials.empty())
		out += "\nMaterials:" + materials;
	return out;
}

bool TerrainPhysical::HasWaterAround(const vector3d &p0, const double radiusMetres) const
{
	if (m_state.sea <= 0.0)
		return false;
	const vector3d p = p0.Normalized();
	const vector3d east = p.Cross(std::abs(p.y) < 0.9 ? vector3d(0.0, 1.0, 0.0) : vector3d(1.0, 0.0, 0.0)).Normalized();
	const vector3d north = p.Cross(east);
	const double reach = radiusMetres / m_planetRadius;
	// What a site needs is dry ground in every view: from afar, near, and on foot. A coast or a river
	// moves a little between them, and from the height of a city a tiny island is still open water.
	const double limits[3] = { m_planetRadius / 20000.0, m_planetRadius / 500.0, 0.0 };
	const auto wetAt = [this](const vector3d &q) {
		Point point;
		Evaluate(q, point);
		Shares shares;
		Paint(q, point, shares);
		// (a side river can run through a city; only the larger water can't)
		point.sideEdge = 1e30;
		return AltitudeOf(point, shares) < 0.0;
	};
	for (const double limit : limits) {
		TerrainNoise::ScopedBandLimit bandLimit(limit);
		if (wetAt(p))
			return true;
		for (int ring = 1; ring <= 2; ring++) {
			const int count = ring * 6;
			const double distance = reach * ring * 0.5;
			for (int i = 0; i < count; i++) {
				const double a = 2.0 * M_PI * i / count;
				if (wetAt((p + (east * std::cos(a) + north * std::sin(a)) * distance).Normalized()))
					return true;
			}
		}
	}
	return false;
}

// The shares of the materials over the whole body, as the lab reports them, for comparing the two
void TerrainPhysical::BuildShares()
{
	TerrainNoise::ScopedBandLimit bandLimit(LEVEL_MAX_FREQUENCY);
	const int samples = 1024;
	Shares total;
	total.fill(0.0);
	double water = 0.0, rain = 0.0, landCount = 0.0;
	for (int i = 0; i < samples; i++) {
		const vector3d p = EvenPoint(i, samples);
		Point point;
		Evaluate(p, point);
		Shares shares;
		Paint(p, point, shares);
		if (point.underSea && shares[M::SAND] < 0.5) {
			water += 1.0;
			continue;
		}
		landCount += 1.0;
		rain += point.rain;
		for (int k = 0; k < M::LAYER_COUNT; k++)
			total[k] += shares[k];
	}
	char line[80];
	snprintf(line, sizeof(line), "Sea %.0f %%; on land: rain %.2f,", 100.0 * water / samples, rain / std::max(landCount, 1.0));
	m_sharesReadout = line;
	for (int k = 0; k < M::LAYER_COUNT; k++) {
		if (total[k] / samples >= 0.01) {
			snprintf(line, sizeof(line), " %s %.0f %%", PlanetPhysics::LayerName(M::Layer(k)), 100.0 * total[k] / samples);
			m_sharesReadout += line;
		}
	}
}

bool TerrainPhysical::GetTileStyle(float style[4]) const
{
	// ground below what the mesh and the craters show, as rough as what it's made of, and craters too small for the
	// mesh, at the body's density
	style[0] = 0.3f;
	style[1] = 0.0f;
	style[2] = 0.0f;
	switch (m_state.ground) {
	case M::ROCK:
	case M::REGOLITH:
		style[0] = 0.4f;
		style[1] = 0.15f;
		break;
	case M::BASALT:
		style[1] = 0.25f;
		break;
	case M::ICE:
		style[0] = 0.2f;
		break;
	case M::SAND:
		style[0] = 0.15f;
		break;
	default: break;
	}
	// (what has its own maps keeps to the gentle small sizes it was judged with)
	if (m_lines.IsActiveShell() || m_grooved) {
		style[0] = 0.15f;
		style[1] = 0.0f;
	}
	const double craters = std::clamp(m_interior.cratering * (1.0 - 0.97 * m_state.wearSmall) * (1.0 - m_state.lavaPlanet), 0.0, 1.0);
	// (a young surface such as Europa's has next to none, and the tiles can't place so few)
	style[3] = craters < 0.05 ? 0.0f : float(craters);
	return true;
}

std::vector<std::string> TerrainPhysical::GetGeologyInfo() const
{
	std::vector<std::string> out = PlanetPhysics::Dump(m_core, m_inputs, m_interior, m_state);
	char line[200];
	if (m_seaLevel > NONE_LOW)
		snprintf(line, sizeof(line), "Levels: sea %.0f m, datum %.0f m, highest %.0f m above it", m_seaLevel, m_datum, m_maxHeightInMeters);
	else
		snprintf(line, sizeof(line), "Levels: no sea, datum %.0f m, highest %.0f m above it", m_datum, m_maxHeightInMeters);
	out.emplace_back(line);
	snprintf(line, sizeof(line), "Craters: %d listed, %d smaller sizes on lattices, down to %.1f km", int(m_craters.ListedCount()), int(m_craters.SizeCount()), TerrainCraters::SMALLEST_KM);
	out.emplace_back(line);
	int erupting = 0, kinds[3] = { 0, 0, 0 };
	for (const TerrainVolcanoes::Vent &vent : m_volcanoes.Vents()) {
		kinds[vent.kind]++;
		erupting += vent.erupting ? 1 : 0;
	}
	snprintf(line, sizeof(line), "Volcanoes: %d cones, %d shields, %d pits, %d erupting; %d lava lakes; %d blocks, %d paterae; tallest %.0f m", kinds[0], kinds[1], kinds[2], erupting, int(m_volcanoes.Lakes().size()), int(m_volcanoes.Blocks().size()), int(m_volcanoes.Paterae().size()), m_volcanoes.Tallest());
	out.emplace_back(line);
	if (m_lines.IsActiveShell())
		out.emplace_back(CrustTiles() ? "Ground maps: loaded" : "Ground maps: MISSING");
	if (m_grooved)
		out.emplace_back(GrooveTiles() ? "Groove maps: loaded" : "Groove maps: MISSING");
	out.emplace_back("Dirt: " + std::to_string(int(100.0 * m_dirtStrength)) + " %");
	snprintf(line, sizeof(line), "Lines: %d large, %d fine, %d domes; tallest %.0f m, deepest %.0f m", int(m_lines.LineCount()), int(m_lines.FineCount()), int(m_lines.DomeCount()), m_lines.Tallest(), m_lines.Deepest());
	out.emplace_back(line);
	snprintf(line, sizeof(line), "Plates: %s; river pieces %d; rain %s; valleys %.1f; built in %.0f ms", m_plates.Active() ? "yes" : "no", int(m_hydro.RiverPieces()), m_hydro.HasMoisture() ? "yes" : "no", m_valleyAmount, m_buildMilliseconds);
	out.emplace_back(line);
	out.push_back(m_sharesReadout);
	char speed[200];
	snprintf(speed, sizeof(speed), "Speed: a point %.1f us, of which valleys %.1f us; its materials %.1f us; %llu valley chunks made in %.0f ms",
		s_pointSpeed.Microseconds(), TerrainValleys::CutSpeed().Microseconds(), s_paintSpeed.Microseconds(), (unsigned long long)TerrainValleys::ChunksBuilt(), TerrainValleys::ChunkMilliseconds());
	out.emplace_back(speed);
	return out;
}
