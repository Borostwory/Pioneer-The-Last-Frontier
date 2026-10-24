// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainErosion.h"

#include "Terrain.h"
#include "TerrainNoise.h"

#include "FileSystem.h"
#include "GameConfig.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Random.h"
#include "core/Log.h"
#include "galaxy/SystemBody.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
	// deepest cut into a valley floor
	constexpr double CUT_DEPTH_M = 1180.0;
	// share of the terrain height a cut may remove at most (keeps land above the sea)
	constexpr double MAX_CUT_SHARE = 0.6;

	// The mask reads the terrain without features under MASK_WAVELENGTH_M.
	// Its slope comes from heights MASK_STEP_M apart: a difference of heights
	// stays continuous where the noise has slope kinks (simplex cell edges),
	// the derivative would turn them into steps in the cut.
	constexpr double MASK_WAVELENGTH_M = 20000.0;
	constexpr double MASK_STEP_M = 5000.0;
	// rugged ground gets the full cut
	constexpr double RELIEF_SLOPE_MIN = 0.015;
	constexpr double RELIEF_SLOPE_MAX = 0.06;
	// high ground gets BASE_CUT of it, so plateaus show the valleys from afar
	constexpr double HIGH_GROUND_MIN_M = 700.0;
	constexpr double HIGH_GROUND_MAX_M = 2000.0;
	constexpr double BASE_CUT = 0.5;
	// heights where the cut fades in at the coast
	constexpr double COAST_FADE_M = 400.0;
	// Around surface starports the terrain stays as the height fractal makes
	// it, so cities spread over the ground they were designed for (the
	// largest reach ~15 km, data/configs/buildings) and starport relocation
	// (Space.cpp) sees the same slopes as without erosion. Cuts fade back in
	// between the two distances.
	constexpr double CITY_KEEP_M = 15000.0;
	constexpr double CITY_FADE_M = 30000.0;

	// sharpness of the blend between cube faces (weights |p.axis|^24)
	constexpr int FACE_BLEND_POWER = 24;

	const char *const VALLEYS_FILE = "heightmaps/erosion_valleys.bin";

	double SmoothStep(const double edge0, const double edge1, const double x)
	{
		const double t = Clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	double CatmullRom(const double p0, const double p1, const double p2, const double p3, const double t)
	{
		const double a1 = 0.5 * (p2 - p0);
		const double a2 = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
		const double a3 = 0.5 * (p3 - p0) + 1.5 * (p1 - p2);
		return p1 + t * (a1 + t * (a2 + t * a3));
	}
} // namespace

// The valley field with its mip levels; loaded once, shared by all bodies.
struct TerrainErosion::Valleys {
	int size = 0; // level 0, a power of two
	double tileMetres = 0.0;
	double mean = 0.5;
	std::vector<std::vector<uint16_t>> levels;

	static std::shared_ptr<const Valleys> Load();

	// texel (x, y) of a level, wrapping, 0..1
	double Texel(const int level, const int x, const int y) const
	{
		const int n = size >> level;
		return levels[level][size_t(y & (n - 1)) * n + size_t(x & (n - 1))] * (1.0 / 65535.0);
	}

	// at (s, t) in tiles
	double Bicubic(const double s, const double t) const
	{
		const double x = s * size - 0.5, y = t * size - 0.5;
		const double fx = floor(x), fy = floor(y);
		const int ix = int(fx), iy = int(fy);
		double c[4];
		for (int j = 0; j < 4; j++)
			c[j] = CatmullRom(Texel(0, ix - 1, iy + j - 1), Texel(0, ix, iy + j - 1), Texel(0, ix + 1, iy + j - 1), Texel(0, ix + 2, iy + j - 1), x - fx);
		return Clamp(CatmullRom(c[0], c[1], c[2], c[3], y - fy), 0.0, 1.0);
	}

	double Bilinear(const int level, const double s, const double t) const
	{
		const int n = size >> level;
		const double x = s * n - 0.5, y = t * n - 0.5;
		const double fx = floor(x), fy = floor(y);
		const int ix = int(fx), iy = int(fy);
		const double dx = x - fx, dy = y - fy;
		const double a = Texel(level, ix, iy) + dx * (Texel(level, ix + 1, iy) - Texel(level, ix, iy));
		const double b = Texel(level, ix, iy + 1) + dx * (Texel(level, ix + 1, iy + 1) - Texel(level, ix, iy + 1));
		return a + dy * (b - a);
	}

	// trilinear between levels, bicubic at full resolution
	double Sample(const double s, const double t, const double lod) const
	{
		if (lod <= 0.0)
			return Bicubic(s, t);
		const int last = int(levels.size()) - 1;
		const int l0 = std::min(int(floor(lod)), last);
		const double v0 = (l0 == 0) ? Bicubic(s, t) : Bilinear(l0, s, t);
		const double f = lod - floor(lod);
		if (l0 >= last || f <= 0.0)
			return v0;
		return v0 + f * (Bilinear(l0 + 1, s, t) - v0);
	}
};

std::shared_ptr<const TerrainErosion::Valleys> TerrainErosion::Valleys::Load()
{
	RefCountedPtr<FileSystem::FileData> fdata = FileSystem::gameDataFiles.ReadFile(VALLEYS_FILE);
	if (!fdata) {
		Log::Warning("Terrain erosion: could not open {}, erosion is off\n", VALLEYS_FILE);
		return nullptr;
	}

	// "EVAL", uint32 size, float32 tile size (m), float32 mean, size * size uint16
	const char *data = fdata->GetData();
	const size_t bytes = fdata->GetSize();
	uint32_t size = 0;
	float tileMetres = 0.0f, mean = 0.0f;
	if (bytes >= 16) {
		memcpy(&size, data + 4, 4);
		memcpy(&tileMetres, data + 8, 4);
		memcpy(&mean, data + 12, 4);
	}
	const bool powerOfTwo = size >= 16 && (size & (size - 1)) == 0;
	if (bytes < 16 || memcmp(data, "EVAL", 4) != 0 || !powerOfTwo || tileMetres <= 0.0f ||
		bytes != 16 + size_t(size) * size * 2) {
		Log::Warning("Terrain erosion: {} is not a valid valley file, erosion is off\n", VALLEYS_FILE);
		return nullptr;
	}

	auto v = std::make_shared<Valleys>();
	v->size = int(size);
	v->tileMetres = tileMetres;
	v->mean = mean;
	v->levels.emplace_back(size_t(size) * size);
	memcpy(v->levels[0].data(), data + 16, size_t(size) * size * 2);

	// mip levels down to 16 x 16, box filtered
	for (int n = int(size) / 2; n >= 16; n /= 2) {
		const std::vector<uint16_t> &src = v->levels.back();
		std::vector<uint16_t> dst(size_t(n) * n);
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				const size_t i = size_t(2 * y) * (2 * n) + 2 * x;
				dst[size_t(y) * n + x] = uint16_t((uint32_t(src[i]) + src[i + 1] + src[i + 2 * n] + src[i + 2 * n + 1] + 2) / 4);
			}
		v->levels.push_back(std::move(dst));
	}
	return v;
}

std::unique_ptr<TerrainErosion> TerrainErosion::Create(const SystemBody *body, const double planetRadius)
{
	if (body->GetSuperType() != SystemBody::SUPERTYPE_ROCKY_PLANET)
		return nullptr;

	// rivers need liquid water at the surface: some liquid, an atmosphere and
	// a temperature between freezing (with some margin for warm seasons and
	// the tropics) and boiling
	const double liquid = body->GetVolatileLiquid();
	const int temperature = body->GetAverageTemp();
	if (liquid < 0.05 || body->GetVolatileGas() < 0.05 || temperature < 250 || temperature > 380)
		return nullptr;

	if (Pi::config->Int("TerrainErosion", 1) == 0)
		return nullptr;

	static const std::shared_ptr<const Valleys> valleys = Valleys::Load();
	if (!valleys)
		return nullptr;

	const double strength = SmoothStep(0.05, 0.3, liquid);
	std::unique_ptr<TerrainErosion> erosion(new TerrainErosion(valleys, planetRadius, strength, body->GetSeed()));

	// where the surface starports stand: as defined, or where Space.cpp moved
	// them when the terrain is made again (detail level change); moved ones
	// are added with AddCity when the system is set up
	auto cities = std::make_shared<std::vector<vector3d>>();
	for (const SystemBody *kid : body->GetChildren())
		if (kid->GetType() == SystemBody::TYPE_STARPORT_SURFACE)
			cities->push_back(kid->GetOrbit().GetPlane() * vector3d(0.0, 1.0, 0.0));
	erosion->m_cities = std::move(cities);
	return erosion;
}

TerrainErosion::TerrainErosion(std::shared_ptr<const Valleys> valleys, const double planetRadius, const double strength, const unsigned int seed) :
	m_valleys(std::move(valleys)),
	m_radius(planetRadius),
	m_strength(strength)
{
	// its own generator, so the terrain's random sequence is unchanged
	Random rand(seed ^ 0x6e7a3c15u);
	for (vector2d &offset : m_faceOffset)
		offset = vector2d(rand.Double(), rand.Double());
}

TerrainErosion::~TerrainErosion()
{
}

double TerrainErosion::ValleyAt(const vector3d &p, const double lod) const
{
	// each cube face maps its own copy of the valleys with equal-angle
	// coordinates; near the face diagonals the copies are blended, keeping
	// the contrast (variance-preserving blend around the mean)
	static_assert(FACE_BLEND_POWER == 24, "the weights below are |p.axis|^24");
	const double absP[3] = { fabs(p.x), fabs(p.y), fabs(p.z) };
	double weight[3];
	double total = 0.0;
	for (int a = 0; a < 3; a++) {
		const double w2 = absP[a] * absP[a], w4 = w2 * w2, w8 = w4 * w4;
		weight[a] = w8 * w8 * w8;
		total += weight[a];
	}

	const double tilesPerRadian = m_radius / m_valleys->tileMetres;
	const double mean = m_valleys->mean;
	double sum = 0.0, sumSquares = 0.0;
	for (int a = 0; a < 3; a++) {
		const double w = weight[a] / total;
		if (w < 0.002)
			continue;
		// the face's axis and the two across it
		const double major = (a == 0) ? p.x : (a == 1) ? p.y : p.z;
		const double u = (a == 0) ? p.y : (a == 1) ? p.z : p.x;
		const double v = (a == 0) ? p.z : (a == 1) ? p.x : p.y;
		const vector2d &offset = m_faceOffset[2 * a + (major < 0.0 ? 1 : 0)];
		const double s = atan2(u, fabs(major)) * tilesPerRadian + offset.x;
		const double t = atan2(v, fabs(major)) * tilesPerRadian + offset.y;
		sum += w * (m_valleys->Sample(s, t, lod) - mean);
		sumSquares += w * w;
	}
	// dividing by the root of the summed squared weights keeps the valleys as
	// deep in the blend as on one face (and normalises the weights used)
	return Clamp(mean + sum / sqrt(sumSquares), 0.0, 1.0);
}

void TerrainErosion::Apply(const Terrain &terrain, const vector3d *vP, double *heightsOut, const size_t count) const
{
	// valley detail to the band limit the heights are generated with
	double lod = 0.0;
	const double maxFrequency = TerrainNoise::s_maxFrequency;
	if (maxFrequency > 0.0) {
		const double spacingMetres = m_radius / (2.0 * maxFrequency);
		lod = std::max(0.0, log2(spacingMetres * m_valleys->size / m_valleys->tileMetres));
	}

	// only land away from the cities is cut
	const std::shared_ptr<const std::vector<vector3d>> cities = Cities();
	thread_local std::vector<size_t> land;
	thread_local std::vector<double> keep;
	thread_local std::vector<vector3d> samples;
	thread_local std::vector<double> sampleHeights;
	land.clear();
	keep.clear();
	for (size_t i = 0; i < count; i++) {
		if (heightsOut[i] <= 0.0)
			continue;
		const double away = CityFade(*cities, vP[i]);
		if (away <= 0.0)
			continue;
		land.push_back(i);
		keep.push_back(away);
	}
	if (land.empty())
		return;

	// large-scale terrain around each point: +-MASK_STEP_M along two tangents
	samples.resize(land.size() * 4);
	sampleHeights.resize(land.size() * 4);
	const double step = MASK_STEP_M / m_radius;
	for (size_t k = 0; k < land.size(); k++) {
		const vector3d &p = vP[land[k]];
		const vector3d ref = (fabs(p.y) < 0.99) ? vector3d(0.0, 1.0, 0.0) : vector3d(1.0, 0.0, 0.0);
		const vector3d t1 = ref.Cross(p).Normalized();
		const vector3d t2 = p.Cross(t1);
		samples[4 * k + 0] = (p + t1 * step).Normalized();
		samples[4 * k + 1] = (p - t1 * step).Normalized();
		samples[4 * k + 2] = (p + t2 * step).Normalized();
		samples[4 * k + 3] = (p - t2 * step).Normalized();
	}
	{
		// the same at every level of detail and for collision
		TerrainNoise::ScopedBandLimit bandLimit(m_radius / MASK_WAVELENGTH_M);
		terrain.GetBaseHeights(samples.data(), sampleHeights.data(), samples.size());
	}

	for (size_t k = 0; k < land.size(); k++) {
		const size_t i = land[k];
		const double *h = &sampleHeights[4 * k];
		const double gx = (h[0] - h[1]) * m_radius, gy = (h[2] - h[3]) * m_radius;
		const double slope = sqrt(gx * gx + gy * gy) / (2.0 * MASK_STEP_M);
		const double largeScale = 0.25 * (h[0] + h[1] + h[2] + h[3]) * m_radius;
		const double height = heightsOut[i] * m_radius;

		const double relief = SmoothStep(RELIEF_SLOPE_MIN, RELIEF_SLOPE_MAX, slope) * SmoothStep(0.0, COAST_FADE_M, height);
		const double highGround = BASE_CUT * SmoothStep(HIGH_GROUND_MIN_M, HIGH_GROUND_MAX_M, largeScale);
		const double mask = std::max(relief, highGround) * m_strength * keep[k];
		if (mask <= 0.0)
			continue;

		const double cut = std::min(CUT_DEPTH_M * ValleyAt(vP[i], lod), MAX_CUT_SHARE * height);
		heightsOut[i] -= mask * cut / m_radius;
	}
}

std::shared_ptr<const std::vector<vector3d>> TerrainErosion::Cities() const
{
	std::lock_guard<std::mutex> lock(m_citiesLock);
	return m_cities;
}

void TerrainErosion::AddCity(const vector3d &p)
{
	std::lock_guard<std::mutex> lock(m_citiesLock);
	auto cities = std::make_shared<std::vector<vector3d>>(*m_cities);
	cities->push_back(p.Normalized());
	m_cities = std::move(cities);
}

double TerrainErosion::CityFade(const std::vector<vector3d> &cities, const vector3d &p) const
{
	double fade = 1.0;
	for (const vector3d &city : cities) {
		// distance along the surface, from the chord (exact enough here)
		const double distance = (p - city).Length() * m_radius;
		if (distance < CITY_FADE_M)
			fade = std::min(fade, SmoothStep(CITY_KEEP_M, CITY_FADE_M, distance));
	}
	return fade;
}

void TerrainErosion::Dump() const
{
	Output("  Erosion: strength %.2f, valleys up to %.0f m deep, tile %.0f km, %d x %d, %d cities kept\n",
		m_strength, CUT_DEPTH_M * m_strength, m_valleys->tileMetres / 1000.0, m_valleys->size, m_valleys->size, int(Cities()->size()));
}
