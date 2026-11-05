// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainPhysical.h"

#include "MathUtil.h"
#include "SphereCells.h"
#include "TerrainErosion.h"

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
		case M::LAVA:
		case M::CRACKED_MUD:
		case M::SALT: return 0.0;
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

	m_craters.Build(m_core, m_interior, m_state, m_noiseSeed);
	if (m_interior.plates)
		m_plates.Build(m_core.seed);
	BuildLevels();
	BuildTones();
	BuildMaterials();
	BuildShares();

	if (m_state.sea > 0.0)
		m_surfaceEffects |= EFFECT_WATER;
	if (m_state.ground == M::LAVA)
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
		if (m_interior.plains > 0.02 && !m_state.sulphur && m_state.wear < 0.2)
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
	// bare steep ground is the crust's material; a sea has beaches
	m_materials.SetSteep(cryo ? M::ICE : M::ROCK);
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
		m_hydro.BuildDrainage(params, ground);
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
	m_datum = m_state.sea > 0.0 ? m_seaLevel : double(ground.low) - 0.15 * m_reliefLimit;

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
			return std::max(shore - m_hydro.valley.Smooth(point.place) + (g.craters.small + g.small) * KeepOf(m_state.ground) + OwnRelief(p, g), sea);
		});
	}

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
		if (m_state.ground == M::ICE && m_interior.iceActivity < 0.05 && m_inputs.ice < 0.95)
			m_darkLevel = LevelAbove(darkField, std::min(darkShare, std::max(0.0, 1.0 - std::min(m_inputs.ice, 1.0))));
		if (m_state.ground == M::DIRTY_ICE && m_inputs.ice <= 0.0)
			m_brightLevel = LevelAbove(darkField, brightShare);
	}

	// room for what the coarse grid can't see
	m_maxHeightInMeters = std::min(std::max(100.0, double(ground.high) - m_datum + 0.25 * m_reliefLimit), m_planetRadius);
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

	// vegetation by the star's colour
	const double starT = m_core.starTemperature;
	if (starT < 3750.0)
		m_vegetation = vector3d(0.32, 0.26, 0.08);
	else if (starT < 5100.0)
		m_vegetation = vector3d(0.26, 0.32, 0.07);
	else if (starT < 6350.0)
		m_vegetation = vector3d(0.12, 0.32, 0.08);
	else if (starT < 9500.0)
		m_vegetation = vector3d(0.05, 0.28, 0.15);
	else
		m_vegetation = vector3d(0.04, 0.22, 0.24);
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
	set(M::LAVA, vector3d(1.3, 0.42, 0.08));
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
		const double grime = m_interior.iceActivity >= 0.05 ? 0.0 : std::min(1.0, std::pow(draw, 0.7) * 0.85);
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

	// old plains flood the lows, with the large craters' rims standing out of them
	if (m_floodLevel > NONE_LOW) {
		const double before = h + 0.5 * g.craters.large + 0.7 * std::min(env, 13000.0) * g.warp;
		g.plains = Soft(m_floodLevel - before, 0.0, std::max(0.04 * env, 40.0));
		h += g.plains * std::max(m_floodLevel - before, 0.0) * 0.9;
	}

	g.large = h;
	g.base = h + g.craters.large;
	g.small = 0.08 * env * Fbm(p, 18.0, 5, m_noiseSeed + 41, 0.55);
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
	point.valley = H > 0.0 ? std::clamp((cut.cut + cut.average) / std::max(0.55 * H, 1.0), 0.0, 1.0) : 0.0;
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
	return 0.1 * H * (std::clamp(ridged, -1.0, 1.0) - 0.15) * std::pow(1.0 - point.valley, 1.5) * high * calm * cut.quiet;
}

double TerrainPhysical::Keep(const Ground &g) const
{
	const double ground = KeepOf(m_state.ground);
	if (m_slots[M::BASALT] < 0 || g.plains <= 0.0)
		return ground;
	return ground + (KeepOf(M::BASALT) - ground) * g.plains;
}

double TerrainPhysical::OwnRelief(const vector3d &p, const Ground &g) const
{
	const double cap = 0.05 * m_reliefLimit;
	const double fMin = std::max(m_core.radius * 6378.135 / 100.0, 3.0);
	auto own = [&](const double amplitude, const double frequency, const int octaves, const uint32_t seed, const bool ridged) {
		const double n = Fbm(p, std::max(frequency, fMin), octaves, m_noiseSeed + seed);
		return amplitude * (ridged ? 1.0 - 2.0 * std::abs(n) * 2.2 : n);
	};
	double out = 0.0;
	switch (m_state.ground) {
	case M::ROCK: out = own(cap, fMin * 1.5, 5, 201, true); break;
	case M::DIRTY_ICE: out = own(0.35 * cap, fMin * 2.0, 3, 202, false) + own(0.25 * cap, fMin * 9.0, 5, 208, true); break;
	case M::GRAVEL: out = own(0.2 * cap, fMin * 8.0, 3, 203, false); break;
	case M::LAVA_FLOW: out = own(0.25 * cap, fMin * 2.0, 3, 204, false); break;
	case M::BASALT: out = own(0.1 * cap, fMin * 2.0, 3, 205, false); break;
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

// separate dune seas near the equator in the lows, with highlands between them
double TerrainPhysical::DuneField(const vector3d &p, const Ground &g) const
{
	const double latitude = std::abs(std::asin(std::clamp(p.y, -1.0, 1.0))) + 0.12 * g.warp;
	return -0.9 * latitude + 1.5 * SphereCells::FbmStretched(p, 1.1, 5, m_noiseSeed + 83) - 0.3 * g.base / m_reliefLimit;
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
	// valleys and peaks die away towards a river, so what is beside it stays near its level
	out.hasRiver = m_hydro.RiverAt(p, out.river);
	const double calm = out.hasRiver ? std::clamp(out.river.trueDistance / out.river.valley, 0.0, 1.0) : 1.0;
	const double peaks = m_valleyAmount > 0.0 ? CutValleys(p, out, plainGround, calm) : 0.0;
	const double relief = ((g.craters.small + g.small) * Keep(g) + OwnRelief(p, g)) * out.quiet;

	if (m_state.sea <= 0.0) {
		// the surface of a frozen sea is level; relief fades in on land
		const double ground = g.base - valley + peaks;
		if (m_seaLevel > NONE_LOW)
			out.height = std::max(ground, m_seaLevel) + relief * Soft(ground - m_seaLevel, 0.0, 0.02 * m_reliefLimit);
		else
			out.height = ground + relief;
		out.shore = g.base;
		return;
	}

	out.shore = g.base + out.coast;
	out.underSea = out.shore < m_seaLevel;
	if (out.underSea) {
		// the sea floor stays flat at sea level; the real depth travels in the altitude
		out.height = m_seaLevel;
		return;
	}

	// land relief fades out towards the water, so a sharp waterline doesn't turn it into a false cliff
	const double broad = std::max(3.0 * m_hydro.slope.Linear(out.place), 0.001);
	const double band = std::clamp(1e4 * broad, 1.0, std::max(0.015 * m_reliefLimit, 1.0));
	double h = out.shore - valley + peaks + relief * Soft(out.shore - m_seaLevel, 0.0, band);

	// the map's lakes are flat, with flat ground beside them, so the ground meets the water at its edge
	if (!m_hydro.lake.Empty()) {
		const double field = m_hydro.lake.Smooth(out.place);
		if (field > 0.0) {
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
	out.height = std::max(h, m_seaLevel);
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
	const double h0 = point.g.base;
	double m = point.rain;
	if (!m_hydro.corridor.Empty())
		m += 0.55 * m_hydro.corridor.Smooth(point.place);
	if (point.carved) {
		// water gathers on valley floors, so they are greener than the ridges. The noise keeps the
		// edge from following the valley's distance, which would draw straight lines and arcs.
		m += 0.4 * point.valley * point.valley - 0.12 * (1.0 - point.valley) + 0.1 * std::clamp(Fbm(p, 60.0, 8, m_noiseSeed + 73, 0.6) / 0.35, -1.0, 1.0);
		// crests of high ground are bare, rock above scree
		const double alpine = (h0 - sea) / std::min(m_reliefLimit, 7000.0) + 0.07 * Fbm(p, 40.0, 6, m_noiseSeed + 72, 0.6) / 0.35;
		const double crest = 1.0 - Soft(point.valley, 0.35, 0.3);
		paint(M::ROCK, Soft(alpine, 0.3, 0.06) * crest);
		paint(M::GRAVEL, Soft(alpine, 0.24, 0.06) * crest);
	}
	const double T = point.temperature - m_state.lapse * 0.6 * std::max(h0 - sea, 0.0);
	const double desert = 1.0 - Soft(m, 0.13, 0.025);
	const double hot = Soft(T, 280.0, 5.0);
	// dune seas in the hot desert's lows; bare stony plains only where it is driest. (Not under the sea:
	// sand there is drawn in place of the water, for barrier islands.)
	if (desert > 0.0 && hot > 0.0 && !point.underSea) {
		const double dunes = Soft(Fbm(p, 3.2, 3, m_noiseSeed + 71) - 0.9 * (h0 - sea) / m_reliefLimit, -0.05, 0.06);
		paint(M::SAND, desert * hot * dunes);
	}
	paint(M::GRAVEL, 1.0 - Soft(m, 0.05, 0.02));
	paint(M::SOIL, desert);
	const double life = m_inputs.life;
	if (life >= 0.02) {
		// where life reaches: the wetter it must be, the less of it there is
		const double need = 0.15 / std::pow(std::max(life, 0.03), 0.75);
		const double green = Soft(m, need, 0.03);
		const double warm = Soft(T, 268.0, 2.0);
		if (life >= 0.3) {
			// rainforest and temperate forest where wet and warm, boreal forest where cold and wet,
			// savanna and prairie where drier, tundra beyond the trees
			const double trees = Soft(m, 0.42 + 0.1 * Soft(T, 292.0, 6.0), 0.03) * Soft(T, 270.0, 3.0);
			paint(M::FOREST, green * trees);
			paint(M::TUNDRA, green * (1.0 - warm));
			paint(M::GRASS, green);
		} else {
			// thin life: lichen and low growth only
			paint(M::TUNDRA, green);
		}
	}
	paint(M::SOIL, 1.0 - Soft(m, 0.2, 0.03));
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

	// a frozen sea
	if (m_state.sea <= 0.0 && m_state.frozen > 0.0 && g.base < m_seaLevel)
		paint(M::ICE, 1.0);

	const double thin = (cryo && airless) || m_iceLevel < NONE_HIGH ? Thinning(p, g) : 0.0;
	if (cryo && airless) {
		// frost at the poles
		const double frostLine = p.y > 0.0 ? m_northFrost : m_southFrost;
		paint(M::SNOW, Soft(std::abs(std::asin(std::clamp(p.y, -1.0, 1.0))) + 0.22 * g.warp + 0.05 * thin, frostLine, 0.015));
		// old ice moons: dark regions; a past active episode leaves bright lanes along their edges
		if (m_darkLevel < NONE_HIGH) {
			double dark = Soft(DarkField(p, g.warp) - 0.1 * thin, m_darkLevel, 0.015);
			if (m_interior.past)
				dark *= Soft(g.unitEdge, 0.22, 0.2);
			paint(M::DIRTY_ICE, dark);
		}
		if (ground == M::DIRTY_ICE && m_interior.past)
			paint(M::ICE, 1.0 - Soft(g.unitEdge, 0.22, 0.2));
		// dark moons: where the lag is thin, ice shows through in patches
		if (m_brightLevel < NONE_HIGH)
			paint(M::ICE, Soft(DarkField(p, g.warp) + 0.1 * thin, m_brightLevel, 0.015));
	}

	// ice and snow, coldest first
	if (m_iceLevel < NONE_HIGH) {
		const double spread = m_state.equatorTemperature - m_state.poleTemperature;
		const double field = cryo ? CrustIceField(p, g) + 0.1 * thin : IceField(p, g) + 0.08 * spread * thin;
		const double edge = cryo ? 0.015 : 1.5;
		const double core = Soft(field, m_iceCoreLevel, edge);
		const double all = Soft(field, m_iceLevel, edge);
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
		const double d = m_hydro.dry.Smooth(point.place);
		if (d > 0.0) {
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
		if (m_slots[M::BASALT] >= 0)
			paint(M::BASALT, g.plains);
		if (m_interior.lid)
			paint(M::ROCK, g.tessera);
		if (airless) {
			paint(M::GRAVEL, Soft(ejecta, 0.3, 0.4));
			paint(M::DUST, g.craters.floor * 0.9);
		} else if (m_state.wetness > 0.0) {
			Climate(p, point, paint);
		} else {
			const double edge = std::max(0.012 * m_reliefLimit, 10.0);
			if (m_core.temperature > 450.0) {
				paint(M::DUST, Soft(-g.base, m_sandLevel, edge));
			} else {
				if (m_interior.fossil || m_core.temperature < 400.0) {
					paint(M::SALT, Soft(-g.base, m_saltLevel, edge));
					paint(M::CRACKED_MUD, Soft(-g.base, m_mudLevel, edge));
				}
				paint(M::SAND, Soft(-g.base, m_sandLevel, edge));
			}
		}
	}
	paint(ground, 1.0);
}

// Steep ground is bare rock on worlds with weather, with the slope at a fixed scale, so
// the colour doesn't change with the size of a patch
void TerrainPhysical::Steep(const vector3d &p, const vector3d &normal, Shares &shares) const
{
	if (!m_state.weather || m_interior.iceCrust)
		return;
	const double c = std::clamp(p.Dot(normal), 0.05, 1.0);
	const double tangent = std::sqrt(std::max(1.0 - c * c, 0.0)) / c;
	const double steep = Soft(tangent, m_steepSlope, 0.6 * m_steepSlope);
	if (steep <= 0.0)
		return;
	double moved = 0.0;
	for (int i = 0; i < M::LAYER_COUNT; i++) {
		if (i == M::ROCK || shares[i] <= 0.0)
			continue;
		const double k = (i == M::SNOW ? 0.7 : 1.0) * steep * shares[i];
		shares[i] -= k;
		moved += k;
	}
	shares[M::ROCK] += moved;
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

vector3d TerrainPhysical::ColourFrom(const Point &point, const Shares &shares, const double altitude) const
{
	const Ground &g = point.g;
	vector3d c(0.0, 0.0, 0.0);
	if (altitude < 0.0 && shares[M::SAND] < 0.5) {
		// water. Deep is darker; shelves are shallow and pale. (The engine takes red for land here.)
		if (point.underSea) {
			const double d = std::clamp((m_seaLevel - point.shore) / std::max(m_reliefLimit * 0.2, 1.0), 0.0, 1.0);
			const double e = std::pow(1.0 - d, 2.2);
			c = (m_shallowTone * e + m_seaTone * (1.0 - e)) * (1.0 - 0.35 * d);
		} else {
			c = point.lakeField > 0.5 || point.smallLakeEdge < 0.0 ? m_lakeTone : m_riverTone;
		}
		return vector3d(std::clamp(c.x, 0.0, 0.05), std::clamp(c.y, 0.0, 1.0), std::clamp(c.z, 0.0, 1.0));
	}

	for (int i = 0; i < M::LAYER_COUNT; i++) {
		if (shares[i] > 0.0)
			c += m_tones[i] * shares[i];
	}
	// plants: wetter is deeper green; drier grass turns straw; cold forest is darker and bluer
	const double plants = shares[M::FOREST] + shares[M::GRASS] + shares[M::TUNDRA];
	if (plants > 0.0) {
		const double wet = std::clamp((point.rain - 0.25) / 0.6, 0.0, 1.0);
		const double thin = std::min(std::sqrt(std::max(m_inputs.life, 0.05)), 1.0);
		const vector3d straw(0.62, 0.5, 0.24), soil = m_tones[M::SOIL];
		auto plant = [&](const M::Layer layer, const double dryness) {
			if (shares[layer] <= 0.0)
				return;
			vector3d col = m_vegetation * (1.0 + 0.5 * (1.0 - wet));
			const double d = dryness * (1.0 - wet);
			col = col * (1.0 - d) + straw * d;
			if (layer == M::FOREST) {
				col = col * 0.62;
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
	c = c * (1.0 + 0.13 * (g.unitClass - 0.5) * hard);
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
	colourOut = ColourFrom(point, shares, altitude);

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
	// gentle ground below the mesh, and craters too small for the mesh, at the body's density
	style[0] = m_state.ground == M::ROCK ? 0.3f : 0.15f;
	style[1] = 0.0f;
	style[2] = 0.0f;
	const double craters = std::clamp(m_interior.cratering * (1.0 - 0.97 * m_state.wearSmall), 0.0, 1.0);
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
	snprintf(line, sizeof(line), "Plates: %s; river pieces %d; rain %s; valleys %.1f; built in %.0f ms", m_plates.Active() ? "yes" : "no", int(m_hydro.RiverPieces()), m_hydro.HasMoisture() ? "yes" : "no", m_valleyAmount, m_buildMilliseconds);
	out.emplace_back(line);
	out.push_back(m_sharesReadout);
	char speed[200];
	snprintf(speed, sizeof(speed), "Speed: a point %.1f us, of which valleys %.1f us; its materials %.1f us; %llu valley chunks made in %.0f ms",
		s_pointSpeed.Microseconds(), TerrainValleys::CutSpeed().Microseconds(), s_paintSpeed.Microseconds(), (unsigned long long)TerrainValleys::ChunksBuilt(), TerrainValleys::ChunkMilliseconds());
	out.emplace_back(speed);
	return out;
}
