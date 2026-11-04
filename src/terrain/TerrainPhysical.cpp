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

	BuildLevels();
	BuildTones();

	// the materials this body can show
	m_materials.Add(m_state.ground);
	if (m_iceLevel < NONE_HIGH || m_state.frozen > 0.0) {
		m_materials.Add(M::ICE);
		m_materials.Add(M::SNOW);
	}
	// bare steep ground is the crust's material; a sea has beaches
	m_materials.SetSteep(m_interior.iceCrust ? M::ICE : M::ROCK);
	if (m_state.sea > 0.0)
		m_materials.Add(M::SAND);

	if (m_state.sea > 0.0)
		m_surfaceEffects |= EFFECT_WATER;
	if (m_state.ground == M::LAVA)
		m_surfaceEffects |= EFFECT_LAVA;
	m_sealevel = m_state.sea;
}

void TerrainPhysical::BuildLevels()
{
	std::vector<double> heights(LEVEL_SAMPLES);
	double lowest = NONE_HIGH, highest = NONE_LOW;
	for (int i = 0; i < LEVEL_SAMPLES; i++) {
		heights[i] = LargeHeight(EvenPoint(i, LEVEL_SAMPLES));
		lowest = std::min(lowest, heights[i]);
		highest = std::max(highest, heights[i]);
	}
	const double seaShare = std::max(m_state.sea, m_state.frozen);
	if (seaShare > 0.0)
		m_seaLevel = LevelAbove(heights, 1.0 - seaShare);

	// land is above 0; the sea floor lies at 0 on water worlds
	m_datum = m_state.sea > 0.0 ? m_seaLevel : lowest - 0.1 * m_reliefLimit;

	if (m_state.ice > 0.0 && !m_interior.iceCrust) {
		std::vector<double> field(LEVEL_SAMPLES);
		for (int i = 0; i < LEVEL_SAMPLES; i++)
			field[i] = IceField(EvenPoint(i, LEVEL_SAMPLES), heights[i]);
		m_iceLevel = LevelAbove(field, m_state.ice);
		const double snowShare = m_state.weather ? 0.15 + 0.6 * m_state.air : 0.0;
		m_iceCoreLevel = LevelAbove(field, m_state.ice * (1.0 - snowShare));
	}

	// room for what can be added on top of the samples, and never below what the classic code needs
	m_maxHeightInMeters = std::max(100.0, highest - m_datum + 0.1 * m_reliefLimit);
	m_maxHeightInMeters = std::min(m_maxHeightInMeters, m_planetRadius);
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
	if (m_state.sulphur)
		set(M::SALT, vector3d(0.86, 0.74, 0.32));
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

double TerrainPhysical::LargeHeight(const vector3d &p) const
{
	double h = 0.28 * m_reliefLimit * Fbm(p, 1.3, 4, m_noiseSeed + 1);
	if (m_interior.dichotomy) {
		// the highlands and lowlands of a Mars-like world
		const vector3d axis = vector3d(0.2, -0.8, 0.5).Normalized();
		h += 0.25 * m_reliefLimit * std::tanh((p.Dot(axis) + 0.35 * Warp(p)) * 5.0);
	}
	return h;
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

double TerrainPhysical::IceField(const vector3d &p, const double largeHeight) const
{
	const double te = m_state.equatorTemperature, tp = m_state.poleTemperature, spread = te - tp;
	const double warp = Warp(p);
	const double latitude = std::asin(std::clamp(p.y, -1.0, 1.0));
	const double wavy = std::abs(latitude) + 0.05 * warp;
	const double T = te - spread * std::clamp(wavy / (M_PI / 2.0), 0.0, 1.1);
	const double lag = Fbm(p, 3.1, 3, m_noiseSeed + 84);
	const double wobble = spread * (0.2 * warp + 0.09 * lag) / (M_PI / 2.0);
	const double ground = m_seaLevel > NONE_LOW ? m_seaLevel : 0.0;
	const double high = (0.1 + 0.45 * (1.0 - Soft(m_core.temperature, 245.0, 30.0))) * spread * (largeHeight - ground) / m_reliefLimit;
	if (m_state.frozen > 0.0 && largeHeight < m_seaLevel)
		return NONE_HIGH;
	return -(T - wobble - 0.4 * m_state.lapse * std::max(largeHeight - ground, 0.0) - high);
}

double TerrainPhysical::UnitClass(const vector3d &p) const
{
	const vector3d q = (p + vector3d(0.22, 0.22, 0.22) * Warp(p)).Normalized();
	int best = 0;
	double bestDot = -2.0;
	for (int i = 0; i < UNIT_COUNT; i++) {
		const double d = q.Dot(m_unitSeeds[i]);
		if (d > bestDot) {
			bestDot = d;
			best = i;
		}
	}
	return m_unitClass[best];
}

void TerrainPhysical::GetBaseHeights(const vector3d *vP, double *heightsOut, const size_t count) const
{
	const double invRadius = 1.0 / m_planetRadius;
	const bool flooded = m_seaLevel > NONE_LOW;
	for (size_t i = 0; i < count; i++) {
		const vector3d p = vP[i].Normalized();
		const double large = LargeHeight(p);
		double h = large;
		if (flooded) {
			// the surface of the sea or its ice is level; relief fades in on land
			h = std::max(large, m_seaLevel) + OwnRelief(p) * Soft(large - m_seaLevel, 0.0, 0.02 * m_reliefLimit);
		} else {
			h = large + OwnRelief(p);
		}
		heightsOut[i] = std::max(h - m_datum, 0.0) * invRadius;
	}
}

void TerrainPhysical::Shares(const vector3d &p, const double largeHeight, double &ice, double &snow) const
{
	ice = 0.0;
	snow = 0.0;
	if (m_state.frozen > 0.0 && largeHeight < m_seaLevel) {
		ice = 1.0;
		return;
	}
	if (m_iceLevel >= NONE_HIGH)
		return;
	const double field = IceField(p, largeHeight);
	constexpr double EDGE = 1.5;
	const double core = Soft(field, m_iceCoreLevel, EDGE);
	const double all = Soft(field, m_iceLevel, EDGE);
	if (m_state.weather) {
		ice = core;
		snow = (1.0 - core) * all;
	} else {
		ice = std::max(core, all);
	}
}

vector3d TerrainPhysical::ColourFrom(const vector3d &p, const double ice, const double snow, const bool underWater) const
{
	if (underWater)
		return m_seaTone;
	const double ground = std::max(0.0, 1.0 - ice - snow);
	vector3d c = m_tones[M::ICE] * ice + m_tones[M::SNOW] * snow + m_tones[m_state.ground] * ground;
	// shades with a cause: the crustal units, on hard ground
	const M::Layer g = m_state.ground;
	if (g == M::REGOLITH || g == M::ROCK || g == M::BASALT || g == M::DIRTY_ICE || g == M::ICE)
		c = c * (1.0 + 0.13 * (UnitClass(p) - 0.5) * ground);
	return vector3d(std::clamp(c.x, 0.0, 1.0), std::clamp(c.y, 0.0, 1.0), std::clamp(c.z, 0.0, 1.0));
}

vector3d TerrainPhysical::GetColor(const vector3d &p, const double height, const vector3d &norm) const
{
	vector3d colour;
	uint8_t weights[M::SLOT_COUNT];
	GetSurface(p, height, norm, colour, weights);
	return colour;
}

void TerrainPhysical::GetSurface(const vector3d &p, const double height, const vector3d &normal, vector3d &colourOut, uint8_t weightsOut[M::SLOT_COUNT]) const
{
	const vector3d q = p.Normalized();
	const double large = LargeHeight(q);
	const bool underWater = m_state.sea > 0.0 && height <= 0.0;
	double ice = 0.0, snow = 0.0;
	if (!underWater)
		Shares(q, large, ice, snow);
	colourOut = ColourFrom(q, ice, snow, underWater);

	std::fill(weightsOut, weightsOut + M::SLOT_COUNT, uint8_t(0));
	auto put = [&](const M::Layer layer, const double share) {
		const int slot = m_materials.SlotOf(layer);
		if (slot >= 0)
			weightsOut[slot] = uint8_t(std::clamp(int(weightsOut[slot]) + int(share * 255.0 + 0.5), 0, 255));
	};
	const double ground = std::max(0.0, 1.0 - ice - snow);
	put(M::ICE, ice);
	put(M::SNOW, snow);
	put(m_state.ground, ground);
}

bool TerrainPhysical::GetTileStyle(float style[4]) const
{
	// the crater and line layers will fill the tiles; for now gentle rolling ground only
	style[0] = m_state.ground == M::ROCK || m_state.ground == M::SOIL ? 0.5f : 0.25f;
	style[1] = 0.0f;
	style[2] = 0.0f;
	style[3] = 0.0f;
	return true;
}

std::vector<std::string> TerrainPhysical::GetGeologyInfo() const
{
	std::vector<std::string> out = PlanetPhysics::Dump(m_core, m_inputs, m_interior, m_state);
	char line[160];
	if (m_seaLevel > NONE_LOW)
		snprintf(line, sizeof(line), "Levels: sea %.0f m, datum %.0f m, highest %.0f m above it", m_seaLevel, m_datum, m_maxHeightInMeters);
	else
		snprintf(line, sizeof(line), "Levels: no sea, datum %.0f m, highest %.0f m above it", m_datum, m_maxHeightInMeters);
	out.emplace_back(line);
	return out;
}
