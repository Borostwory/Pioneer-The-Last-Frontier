// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainPhysical.h"

#include "MathUtil.h"
#include "SphereCells.h"

#include "../Random.h"
#include "../galaxy/SystemBody.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using SphereCells::Fbm;
using M = TerrainMaterialTable;

namespace {
	constexpr double NONE_LOW = -1e30;
	constexpr double NONE_HIGH = 1e30;
	// points the levels are set from, spread evenly over the sphere
	constexpr int LEVEL_SAMPLES = 4096;
	// their band limit: features finer than they can see are left out
	constexpr double LEVEL_MAX_FREQUENCY = 9.0;

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

	// the value a share of the samples lies above
	double LevelAbove(std::vector<double> values, const double share)
	{
		if (share <= 0.0)
			return NONE_HIGH;
		std::sort(values.begin(), values.end());
		const size_t i = size_t(std::clamp((1.0 - share) * double(values.size()), 0.0, double(values.size() - 1)));
		return values[i];
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
	BuildLevels();
	BuildTones();
	BuildMaterials();

	if (m_state.sea > 0.0)
		m_surfaceEffects |= EFFECT_WATER;
	if (m_state.ground == M::LAVA)
		m_surfaceEffects |= EFFECT_LAVA;
	m_sealevel = m_state.sea;
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
	} else {
		if (m_interior.plains > 0.02 && !m_state.sulphur && m_state.wear < 0.2)
			m_materials.Add(M::BASALT);
		if (m_interior.lid)
			m_materials.Add(M::ROCK);
		if (airless) {
			m_materials.Add(M::GRAVEL);
			m_materials.Add(M::DUST);
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
	// on a coarse sample, so a level doesn't depend on any view
	TerrainNoise::ScopedBandLimit bandLimit(LEVEL_MAX_FREQUENCY);

	if (m_interior.plains > 0.02) {
		// flood plains fill the lows, up to the level that covers their share of the body
		std::vector<double> before(LEVEL_SAMPLES);
		for (int i = 0; i < LEVEL_SAMPLES; i++) {
			const Ground g = GroundAt(EvenPoint(i, LEVEL_SAMPLES));
			before[i] = g.large + 0.5 * g.craters.large + 0.7 * std::min(m_reliefLimit, 13000.0) * g.warp;
		}
		m_floodLevel = LevelAbove(before, 1.0 - m_interior.plains);
	}

	std::vector<double> heights(LEVEL_SAMPLES), iceField(LEVEL_SAMPLES), darkField(LEVEL_SAMPLES);
	std::vector<Ground> grounds(LEVEL_SAMPLES);
	double lowest = NONE_HIGH, highest = NONE_LOW;
	for (int i = 0; i < LEVEL_SAMPLES; i++) {
		const vector3d p = EvenPoint(i, LEVEL_SAMPLES);
		grounds[i] = GroundAt(p);
		heights[i] = grounds[i].base;
		lowest = std::min(lowest, heights[i]);
		highest = std::max(highest, heights[i]);
	}
	const double seaShare = std::max(m_state.sea, m_state.frozen);
	if (seaShare > 0.0)
		m_seaLevel = LevelAbove(heights, 1.0 - seaShare);

	// land is above 0; the sea floor lies at 0 on water worlds
	m_datum = m_state.sea > 0.0 ? m_seaLevel : lowest - 0.15 * m_reliefLimit;

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

	// room for what the coarse sample can't see
	m_maxHeightInMeters = std::min(std::max(100.0, highest - m_datum + 0.25 * m_reliefLimit), m_planetRadius);
	m_maxHeight = m_maxHeightInMeters / m_planetRadius;
	m_invMaxHeight = 1.0 / m_maxHeight;
}

void TerrainPhysical::BuildTones()
{
	Random rng(m_core.seed ^ 0xBEEu);
	const vector3d tint(1.0 + (rng.Double() * 0.12 - 0.06), 1.0 + (rng.Double() * 0.12 - 0.06), 1.0 + (rng.Double() * 0.12 - 0.06));
	const double bright = 0.88 + 0.24 * rng.Double();
	const double metal = std::min(m_inputs.metallicity, 1.0);
	const double iron = metal * Soft(std::min(m_inputs.oxidizing, 1.0), 0.2, 0.5);
	const vector3d rock = vector3d(0.42, 0.39, 0.35) * (1.25 - 0.55 * metal) + vector3d(0.16, 0.05, -0.04) * iron;

	// vegetation by the star's colour
	vector3d veg;
	const double starT = m_core.starTemperature;
	if (starT < 3750.0)
		veg = vector3d(0.32, 0.26, 0.08);
	else if (starT < 5100.0)
		veg = vector3d(0.26, 0.32, 0.07);
	else if (starT < 6350.0)
		veg = vector3d(0.12, 0.32, 0.08);
	else if (starT < 9500.0)
		veg = vector3d(0.05, 0.28, 0.15);
	else
		veg = vector3d(0.04, 0.22, 0.24);

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
	if (m_state.seaState == std::string("liquid hydrocarbon"))
		m_seaTone = vector3d(0.05, 0.04, 0.03);

	for (vector3d &c : m_tones)
		c = vector3d(c.x * tint.x, c.y * tint.y, c.z * tint.z) * bright;
	m_seaTone = vector3d(m_seaTone.x * tint.x, m_seaTone.y * tint.y, m_seaTone.z * tint.z) * bright;
}

double TerrainPhysical::Warp(const vector3d &p) const
{
	return Fbm(p, 2.2, 3, m_noiseSeed + 5);
}

double TerrainPhysical::Lag(const vector3d &p) const
{
	return Fbm(p, 3.1, 3, m_noiseSeed + 84);
}

TerrainPhysical::Ground TerrainPhysical::GroundAt(const vector3d &p) const
{
	Ground g;
	g.warp = Warp(p);
	double h = 0.28 * m_reliefLimit * Fbm(p, 1.3, 4, m_noiseSeed + 1);
	if (m_interior.dichotomy) {
		// the highlands and lowlands of a Mars-like world
		const vector3d axis = vector3d(0.2, -0.8, 0.5).Normalized();
		h += 0.25 * m_reliefLimit * std::tanh((p.Dot(axis) + 0.35 * g.warp) * 5.0);
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
		h += 0.12 * m_reliefLimit * g.tessera * (1.0 + std::abs(Fbm(p, 12.0, 3, m_noiseSeed + 31)));
	}

	g.craters = m_craters.At(p);

	// old plains flood the lows, with the large craters' rims standing out of them
	if (m_floodLevel > NONE_LOW) {
		const double before = h + 0.5 * g.craters.large + 0.7 * std::min(m_reliefLimit, 13000.0) * g.warp;
		g.plains = Soft(m_floodLevel - before, 0.0, std::max(0.04 * m_reliefLimit, 40.0));
		h += g.plains * std::max(m_floodLevel - before, 0.0) * 0.9;
	}

	g.large = h;
	g.base = h + g.craters.large;
	g.small = 0.08 * m_reliefLimit * Fbm(p, 18.0, 5, m_noiseSeed + 41, 0.55);
	return g;
}

double TerrainPhysical::Keep(const Ground &g) const
{
	const double ground = KeepOf(m_state.ground);
	if (m_slots[M::BASALT] < 0 || g.plains <= 0.0)
		return ground;
	return ground + (KeepOf(M::BASALT) - ground) * g.plains;
}

double TerrainPhysical::OwnRelief(const vector3d &p) const
{
	const double cap = 0.05 * m_reliefLimit;
	const double fMin = std::max(m_core.radius * 6378.135 / 100.0, 3.0);
	auto own = [&](const double amplitude, const double frequency, const int octaves, const uint32_t seed, const bool ridged) {
		const double n = Fbm(p, std::max(frequency, fMin), octaves, m_noiseSeed + seed);
		return amplitude * (ridged ? 1.0 - 2.0 * std::abs(n) * 2.2 : n);
	};
	switch (m_state.ground) {
	case M::ROCK: return own(cap, fMin * 1.5, 5, 201, true);
	case M::DIRTY_ICE: return own(0.35 * cap, fMin * 2.0, 3, 202, false) + own(0.25 * cap, fMin * 9.0, 5, 208, true);
	case M::GRAVEL: return own(0.2 * cap, fMin * 8.0, 3, 203, false);
	case M::LAVA_FLOW: return own(0.25 * cap, fMin * 2.0, 3, 204, false);
	case M::BASALT: return own(0.1 * cap, fMin * 2.0, 3, 205, false);
	case M::METAL: return own(cap, fMin * 2.0, 4, 206, true);
	case M::SOIL: return own(0.2 * cap, fMin * 1.5, 3, 207, false);
	default: return 0.0;
	}
}

double TerrainPhysical::IceField(const vector3d &p, const Ground &g) const
{
	const double te = m_state.equatorTemperature, tp = m_state.poleTemperature, spread = te - tp;
	const double latitude = std::asin(std::clamp(p.y, -1.0, 1.0));
	const double wavy = std::abs(latitude) + 0.05 * g.warp;
	const double T = te - spread * std::clamp(wavy / (M_PI / 2.0), 0.0, 1.1);
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

// Where ice shows first as the dark lag thins: in hollows and at random, not on rims.
// Added to a region's field, it turns a hard outline into scattered patches.
double TerrainPhysical::Thinning(const vector3d &p, const Ground &g) const
{
	const double relief = g.craters.large + g.craters.small;
	const double hollow = std::clamp(-relief / std::max(0.03 * m_reliefLimit, 1.0), -1.0, 1.0);
	return 1.5 * Fbm(p, 16.0, 4, m_noiseSeed + 81, 0.6) + 0.5 * hollow;
}

void TerrainPhysical::GetBaseHeights(const vector3d *vP, double *heightsOut, const size_t count) const
{
	const double invRadius = 1.0 / m_planetRadius;
	const bool flooded = m_seaLevel > NONE_LOW;
	for (size_t i = 0; i < count; i++) {
		const vector3d p = vP[i].Normalized();
		const Ground g = GroundAt(p);
		const double relief = (g.craters.small + g.small) * Keep(g) + OwnRelief(p);
		double h;
		if (flooded) {
			// the surface of the sea or its ice is level; relief fades in on land
			h = std::max(g.base, m_seaLevel) + relief * Soft(g.base - m_seaLevel, 0.0, 0.02 * m_reliefLimit);
		} else {
			h = g.base + relief;
		}
		heightsOut[i] = std::max(h - m_datum, 0.0) * invRadius;
	}
}

// What lies on the ground, first come first served, as in the lab (materials.py, stage2)
void TerrainPhysical::Paint(const vector3d &p, const Ground &g, Shares &shares) const
{
	shares.fill(0.0);
	double left = 1.0;
	auto paint = [&](const M::Layer layer, const double amount) {
		const double a = left * std::clamp(amount, 0.0, 1.0);
		shares[layer] += a;
		left -= a;
	};
	const bool cryo = m_interior.iceCrust;
	const bool airless = !m_state.weather;
	const double ejecta = g.craters.ejecta;
	const M::Layer ground = m_state.ground;

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
			paint(M::ICE, core);
			paint(m_state.weather ? M::SNOW : M::ICE, all);
		}
	}

	if (cryo) {
		if (ground == M::DIRTY_ICE && airless) {
			paint(M::ICE, Soft(ejecta, 0.45, 0.45));
			paint(M::GRAVEL, Soft(ejecta, 0.1, 0.35));
		}
	} else {
		if (m_slots[M::BASALT] >= 0)
			paint(M::BASALT, g.plains);
		if (m_interior.lid)
			paint(M::ROCK, g.tessera);
		if (airless) {
			paint(M::GRAVEL, Soft(ejecta, 0.3, 0.4));
			paint(M::DUST, g.craters.floor * 0.9);
		}
	}
	paint(ground, 1.0);
}

vector3d TerrainPhysical::ColourFrom(const Ground &g, const Shares &shares, const bool underWater) const
{
	if (underWater)
		return m_seaTone;
	vector3d c(0.0, 0.0, 0.0);
	for (int i = 0; i < M::LAYER_COUNT; i++) {
		if (shares[i] > 0.0)
			c += m_tones[i] * shares[i];
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

void TerrainPhysical::GetSurface(const vector3d &p, const double height, const vector3d &, vector3d &colourOut, uint8_t weightsOut[M::SLOT_COUNT]) const
{
	const vector3d q = p.Normalized();
	const Ground g = GroundAt(q);
	const bool underWater = m_state.sea > 0.0 && height <= 0.0;
	Shares shares;
	Paint(q, g, shares);
	colourOut = ColourFrom(g, shares, underWater);

	std::fill(weightsOut, weightsOut + M::SLOT_COUNT, uint8_t(0));
	for (int i = 0; i < M::LAYER_COUNT; i++) {
		if (shares[i] > 0.0 && m_slots[i] >= 0)
			weightsOut[m_slots[i]] = uint8_t(std::clamp(int(weightsOut[m_slots[i]]) + int(shares[i] * 255.0 + 0.5), 0, 255));
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
	return out;
}
