// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainPhysicalVolcanoes.h"

#include "MathUtil.h"
#include "SphereCells.h"
#include "TerrainNoise.h"
#include "TerrainPhysicalPlates.h"

#include "../FileSystem.h"
#include "../Random.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace {
	constexpr int INDEX_ROWS = 48, INDEX_COLUMNS = 96;
	// half the diagonal of an index cell at the equator, on the unit sphere
	constexpr double INDEX_CELL_REACH = 0.0463;
	constexpr double RADIUS_KM = 6378.135;

	double Soft(const double x, const double low, const double width)
	{
		const double t = std::clamp((x - low) / std::max(width, 1e-9), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	vector3d RandomDirection(Random &rng)
	{
		const double y = 2.0 * rng.Double() - 1.0;
		const double angle = 2.0 * M_PI * rng.Double();
		const double r = std::sqrt(std::max(0.0, 1.0 - y * y));
		return vector3d(r * std::cos(angle), y, r * std::sin(angle));
	}

	double RandomNormal(Random &rng)
	{
		return std::sqrt(-2.0 * std::log(std::max(1.0 - rng.Double(), 1e-12))) * std::cos(2.0 * M_PI * rng.Double());
	}

	void Axes(const vector3d &centre, vector3d &a, vector3d &b)
	{
		a = (std::abs(centre.y) < 0.9 ? centre.Cross(vector3d(0.0, 1.0, 0.0)) : centre.Cross(vector3d(1.0, 0.0, 0.0))).Normalized();
		b = centre.Cross(a);
	}

	double Spacing()
	{
		const double band = SphereCells::RoundedBand(TerrainNoise::s_maxFrequency);
		return band > 0.0 ? 0.5 / band : 0.0;
	}

	// shows between 1 and 2 samples to its radius
	double Fade(const double radius, const double spacing)
	{
		return spacing > 0.0 ? Soft(radius / spacing, 1.0, 1.0) : 1.0;
	}

	// an irregular, partly polygonal outline, as real rims are: slumps and scallops
	double Outline(const double theta, const double seed, const double amount)
	{
		const double k = seed * 2.0 * M_PI;
		const double w = 0.55 * std::sin(2.0 * theta + k * 3.1) + 0.5 * std::sin(3.0 * theta + k * 7.3) + 0.4 * std::sin(5.0 * theta + k * 11.9) +
			0.22 * std::sin(9.0 * theta + k * 17.7) + 0.12 * std::sin(17.0 * theta + k * 29.3);
		return 1.0 + amount * w;
	}

	// a heightmap from tools/volcano-tiles, with copies each half the size of the last
	constexpr double MAP_REACH = 1.3; // radii from the middle to the edge of a map
	constexpr int FLOW_COPIES = 4; // lava's paths don't show on copies coarser than this

	struct VolcanoMap {
		int size = 0;
		int levels = 0;
		double top = 0.0; // its height, in radii
		double rim = 0.0; // its crater's, in radii
		double floor = 0.0; // the height of its crater's floor, 1 at the top
		std::vector<std::pair<double, double>> ends; // where its lava paths end, in radii from the middle
		std::vector<std::vector<uint16_t>> heights;
		std::vector<std::vector<uint8_t>> flows;

		void Load(const std::string &name, const double topRadii, const double rimRadii, const double floorShare, const std::vector<std::pair<double, double>> &pathEnds)
		{
			ends = pathEnds;
			top = topRadii;
			rim = rimRadii;
			floor = floorShare;
			RefCountedPtr<FileSystem::FileData> heightFile = FileSystem::gameDataFiles.ReadFile("terrain/" + name + ".bin");
			RefCountedPtr<FileSystem::FileData> flowFile = FileSystem::gameDataFiles.ReadFile("terrain/" + name + "_flow.bin");
			if (!heightFile || !flowFile)
				return;
			const size_t count = heightFile->GetSize() / 2;
			const int side = int(std::lround(std::sqrt(double(count))));
			if (side < 2 || size_t(side) * side != count || (side & (side - 1)) != 0 || flowFile->GetSize() != count)
				return;
			const unsigned char *bytes = reinterpret_cast<const unsigned char *>(heightFile->GetData());
			heights.emplace_back(count);
			for (size_t i = 0; i < count; i++)
				heights[0][i] = uint16_t(int(bytes[2 * i]) | (int(bytes[2 * i + 1]) << 8));
			const unsigned char *flowBytes = reinterpret_cast<const unsigned char *>(flowFile->GetData());
			flows.emplace_back(flowBytes, flowBytes + count);
			for (int n = side; n > 2; n /= 2) {
				const std::vector<uint16_t> &from = heights.back();
				std::vector<uint16_t> to(size_t(n / 2) * (n / 2));
				for (int y = 0; y < n / 2; y++) {
					for (int x = 0; x < n / 2; x++) {
						const uint16_t *a = &from[size_t(2 * y) * n + 2 * x], *b = a + n;
						to[size_t(y) * (n / 2) + x] = uint16_t((int(a[0]) + a[1] + b[0] + b[1] + 2) / 4);
					}
				}
				heights.push_back(std::move(to));
				if (int(flows.size()) < FLOW_COPIES) {
					const std::vector<uint8_t> &water = flows.back();
					std::vector<uint8_t> less(size_t(n / 2) * (n / 2));
					for (int y = 0; y < n / 2; y++) {
						for (int x = 0; x < n / 2; x++) {
							const uint8_t *a = &water[size_t(2 * y) * n + 2 * x], *b = a + n;
							less[size_t(y) * (n / 2) + x] = uint8_t((int(a[0]) + a[1] + b[0] + b[1] + 2) / 4);
						}
					}
					flows.push_back(std::move(less));
				}
			}
			levels = int(heights.size());
			size = side;
		}

		template <typename T>
		static double Bilinear(const std::vector<T> &values, const int n, const double x, const double y)
		{
			const double fx = std::clamp(x, 0.0, double(n - 1)), fy = std::clamp(y, 0.0, double(n - 1));
			const int ix = std::min(int(fx), n - 2), iy = std::min(int(fy), n - 2);
			const double u = fx - ix, v = fy - iy;
			const T *a = &values[size_t(iy) * n + ix], *b = a + n;
			return (double(a[0]) * (1.0 - u) + double(a[1]) * u) * (1.0 - v) + (double(b[0]) * (1.0 - u) + double(b[1]) * u) * v;
		}

		// x, y: in cells of the full map. level: which copy, between two of them.
		template <typename T>
		double Sample(const std::vector<std::vector<T>> &copies, const double x, const double y, const double level) const
		{
			const double last = double(int(copies.size()) - 1);
			const double wanted = std::clamp(level, 0.0, last);
			const int low = std::min(int(wanted), int(copies.size()) - 1), high = std::min(low + 1, int(copies.size()) - 1);
			const auto copy = [&](const int k) {
				const double scale = 1.0 / double(1 << k);
				return Bilinear(copies[k], std::max(size >> k, 2), (x + 0.5) * scale - 0.5, (y + 0.5) * scale - 0.5);
			};
			const double a = copy(low);
			return high == low ? a : a + (copy(high) - a) * (wanted - low);
		}

		// 0 to 1, 1 at the top
		double HeightAt(const double x, const double y, const double level) const { return Sample(heights, x, y, level) / 65535.0; }
		// where lava runs: 0 to 1, 1 along the middle of a path
		double FlowAt(const double x, const double y, const double level) const { return Sample(flows, x, y, level) / 255.0; }
	};

	enum MapIndex {
		MAP_CONE_FRESH,
		MAP_CONE_MID,
		MAP_CONE_WORN,
		MAP_CONE_OLD,
		MAP_SHIELD_FRESH,
		MAP_SHIELD_WORN,
		MAP_COUNT
	};

	// loaded once, when the first volcano is made. The numbers are what volcano_tiles printed for each.
	const VolcanoMap *VolcanoMaps()
	{
		static const std::array<VolcanoMap, MAP_COUNT> maps = [] {
			std::array<VolcanoMap, MAP_COUNT> loaded;
			loaded[MAP_CONE_FRESH].Load("volcano_cone_fresh", 0.3822, 0.0568, 0.9231, { { 0.7246, 0.6586 }, { -0.2483, 0.7488 }, { -0.5062, -0.8212 }, { 0.3182, -0.8923 } });
			loaded[MAP_CONE_MID].Load("volcano_cone_mid", 0.3962, 0.0892, 0.9328, { { 0.6548, 0.4058 }, { -0.4287, 0.7272 }, { -0.5862, -0.6637 }, { 0.1543, -0.6598 }, { 0.9291, -0.2496 } });
			loaded[MAP_CONE_WORN].Load("volcano_cone_worn", 0.3964, 0.0550, 0.8900, { { -0.6357, -0.6878 }, { 0.2128, -1.0168 }, { 0.0362, 0.9278 }, { -0.3194, 0.5138 } });
			loaded[MAP_CONE_OLD].Load("volcano_cone_old", 0.4677, 0.0524, 0.8981, { { -0.7043, 0.4020 }, { -0.8516, -0.2826 }, { 0.5468, -0.6319 }, { 0.5062, -0.5392 }, { 0.5824, 0.5849 } });
			loaded[MAP_SHIELD_FRESH].Load("volcano_shield_fresh", 0.2340, 0.1047, 0.9231, { { 0.1035, -0.8110 }, { 0.8580, 0.4871 }, { 0.2140, 0.6598 }, { -0.8237, 0.5163 }, { -0.6281, -0.4109 } });
			loaded[MAP_SHIELD_WORN].Load("volcano_shield_worn", 0.2314, 0.0988, 0.9333, { { -0.7564, 0.2940 }, { -0.8313, -0.5913 }, { 0.5062, -0.4896 }, { 0.5239, -0.6700 }, { 0.5697, 0.5125 } });
			return loaded;
		}();
		return maps.data();
	}

	TerrainVolcanoes::Item MakeItem(const vector3d &centre, const double radius, const double reach, const double height, const double seed)
	{
		TerrainVolcanoes::Item item;
		item.centre = centre;
		Axes(centre, item.axisA, item.axisB);
		item.radius = radius;
		item.cosReach = std::cos(std::min(radius * reach, M_PI));
		item.height = height;
		item.seed = seed;
		return item;
	}
} // namespace

void TerrainVolcanoes::Index::Add(const vector3d &centre, const double reach, const uint16_t id)
{
	if (cells.empty())
		cells.resize(size_t(INDEX_ROWS) * INDEX_COLUMNS);
	const double cosReach = std::cos(std::min(reach + INDEX_CELL_REACH, M_PI));
	for (int r = 0; r < INDEX_ROWS; r++) {
		const double lat = (r + 0.5) / INDEX_ROWS * M_PI - M_PI / 2.0;
		for (int c = 0; c < INDEX_COLUMNS; c++) {
			const double lon = (c + 0.5) / INDEX_COLUMNS * 2.0 * M_PI - M_PI;
			const vector3d middle(std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon));
			if (middle.Dot(centre) >= cosReach)
				cells[size_t(r) * INDEX_COLUMNS + c].push_back(id);
		}
	}
}

const std::vector<uint16_t> *TerrainVolcanoes::Index::At(const vector3d &p) const
{
	if (cells.empty())
		return nullptr;
	const int r = std::clamp(int((std::asin(std::clamp(p.y, -1.0, 1.0)) + M_PI / 2.0) / M_PI * INDEX_ROWS), 0, INDEX_ROWS - 1);
	const int c = std::clamp(int((std::atan2(p.z, p.x) + M_PI) / (2.0 * M_PI) * INDEX_COLUMNS), 0, INDEX_COLUMNS - 1);
	return &cells[size_t(r) * INDEX_COLUMNS + c];
}

void TerrainVolcanoes::BuildConstructs(const PlanetPhysics::Core &core, const PlanetPhysics::Interior &it, const uint32_t noiseSeed, const double reliefLimit)
{
	m_noiseSeed = noiseSeed;
	const double radiusMetres = core.radius * RADIUS_KM * 1000.0;
	Random rng(core.seed ^ 0xC0u);
	const bool heatPipe = it.heatPipe > 0.5;
	const auto addShields = [&](const int count, const double low, const double high, const double heightLow, const double heightHigh) {
		for (int i = 0; i < count; i++) {
			const vector3d centre = RandomDirection(rng);
			const double radius = low + (high - low) * rng.Double();
			const double tall = (heightLow + (heightHigh - heightLow) * rng.Double()) * reliefLimit;
			// low on heat-pipe worlds, and never huge for the size of the body
			const double height = std::min(tall, (heatPipe ? 0.0015 : 0.0065) * radiusMetres);
			// (a limit on its height takes its width with it, or it would be a flat disc)
			const double kept = heatPipe ? radius : std::min(radius, height * (7.0 + 5.0 * rng.Double()) / radiusMetres);
			const uint16_t id = uint16_t(m_shields.size());
			m_shields.push_back(MakeItem(centre, kept, 1.5, height, rng.Double()));
			m_shieldIndex.Add(centre, 1.5 * kept, id);
		}
	};
	if (core.asteroid || it.iceCrust)
		return;
	if (heatPipe) {
		// Mountain blocks as Io's (Schenk 2001): most a few km high, the tallest 17.5 km (Boosaule Montes), lower
		// under stronger gravity and never more than that share of the radius. The tilt and roughness of a top lift
		// its high edge to about BLOCK_TOP_MOST times the height it is given.
		constexpr double IO_TALLEST_M = 17500.0, IO_G = 1.80, IO_TALLEST_PER_RADIUS = 0.0096, IO_LOWEST_SHARE = 0.1;
		constexpr double BLOCK_TOP_MOST = 1.6;
		const double tallest = std::min(IO_TALLEST_M * IO_G / std::max(it.gravity, 0.01), IO_TALLEST_PER_RADIUS * radiusMetres);
		for (int i = 0; i < 90; i++) {
			const vector3d centre = RandomDirection(rng);
			const double radius = 0.03 + 0.07 * rng.Double();
			const double u = rng.Double();
			const double height = (IO_LOWEST_SHARE + (1.0 - IO_LOWEST_SHARE) * u * u) * tallest / BLOCK_TOP_MOST;
			const uint16_t id = uint16_t(m_blocks.size());
			m_blocks.push_back(MakeItem(centre, radius, 1.6, height, rng.Double()));
			m_blockIndex.Add(centre, 1.6 * radius, id);
		}
		for (int i = 0; i < 140; i++) {
			const vector3d centre = RandomDirection(rng);
			const double radius = 0.012 + 0.048 * rng.Double();
			const double depth = std::min(0.1 * reliefLimit, 0.0007 * radiusMetres);
			const uint16_t id = uint16_t(m_paterae.size());
			m_paterae.push_back(MakeItem(centre, radius, 1.5, depth, rng.Double()));
			m_pateraIndex.Add(centre, 1.5 * radius, id);
		}
		addShields(45, 0.015, 0.06, 0.1, 0.35);
	}
	if (it.lid || it.dichotomy)
		addShields(8, 0.06, 0.2, 0.3, 0.9);
}

void TerrainVolcanoes::BuildVents(const PlanetPhysics::Core &core, const PlanetPhysics::SurfaceInputs &inputs, const PlanetPhysics::Interior &it,
	const PlanetPhysics::Surface &st, const double reliefLimit, const double seaLevel, const TerrainPlates *plates,
	const std::function<double(const vector3d &)> &groundAt)
{
	m_wear = st.wear;
	m_glowing = st.glow > 0.3;
	m_weather = st.weather;

	const double volcanicity = core.asteroid ? 0.0 : (st.forcedLava ? 0.18 : std::max(std::clamp(inputs.volcanicity, 0.0, 1.0), st.melted));
	// (fewer on a lava planet, where each would be a dot on a smooth sea)
	const int count = volcanicity > 0.005 ? int((4.0 + 56.0 * volcanicity) * (st.lavaPlanet > 0.5 ? 0.25 : 1.0)) : 0;
	if (count == 0)
		return;

	// where they go
	Random rng(core.seed ^ 0x77u);
	std::vector<vector3d> candidates(size_t(count) * 6);
	for (vector3d &c : candidates)
		c = RandomDirection(rng);
	int arcVents = -1;
	if (st.lavaPlanet > 0.5) {
		// on the high ground that stands above the lava
		std::vector<std::pair<double, int>> scores(candidates.size());
		for (size_t i = 0; i < candidates.size(); i++)
			scores[i] = { -groundAt(candidates[i]), int(i) };
		std::sort(scores.begin(), scores.end());
		for (int i = 0; i < count; i++)
			m_places.push_back(candidates[scores[i].second]);
	} else if (it.iceCrust) {
		// cold vents gather at one pole, along the tidal axis
		for (int i = 0; i < count; i++) {
			const vector3d shift(RandomNormal(rng), RandomNormal(rng), RandomNormal(rng));
			m_places.push_back((vector3d(0.0, -1.0, 0.0) + shift * 0.35).Normalized());
		}
	} else if (plates) {
		// mostly over sinking plates, a few at hotspots away from any boundary
		std::vector<std::pair<double, int>> scores(candidates.size());
		for (size_t i = 0; i < candidates.size(); i++) {
			const TerrainPlates::Bands bands = plates->At(candidates[i], 0.0);
			scores[i] = { -(bands.arc + bands.islandArc + 0.5 * bands.coastRange), int(i) };
		}
		std::sort(scores.begin(), scores.end());
		arcVents = std::min(int(count * 0.8) + 1, count);
		for (int i = 0; i < arcVents; i++)
			m_places.push_back(candidates[scores[i].second]);
		for (int i = arcVents; i < count; i++)
			m_places.push_back(candidates[candidates.size() - 1 - size_t(i)]);
	} else if (!m_shields.empty()) {
		// around the interior's shields
		std::vector<std::pair<double, int>> scores(candidates.size());
		for (size_t i = 0; i < candidates.size(); i++) {
			double nearest = -1.0;
			for (const Item &shield : m_shields)
				nearest = std::max(nearest, candidates[i].Dot(shield.centre));
			scores[i] = { -nearest, int(i) };
		}
		std::sort(scores.begin(), scores.end());
		// (off the mountain blocks, or a pit's level floor would cut a sheer wall through one)
		for (const std::pair<double, int> &score : scores) {
			if (int(m_places.size()) >= count)
				break;
			const vector3d &place = candidates[score.second];
			bool clear = true;
			for (const Item &block : m_blocks) {
				if (place.Dot(block.centre) >= std::cos(std::min(1.35 * block.radius + 0.05, M_PI))) {
					clear = false;
					break;
				}
			}
			if (clear)
				m_places.push_back(place);
		}
	} else {
		for (int i = 0; i < count; i++)
			m_places.push_back(candidates[i]);
	}
	m_spread = std::sqrt(std::max(0.04, 2.0 / double(count)) / 6.0);

	if (it.iceCrust)
		return;

	// what stands there: the same number of draws for each
	Random draws((core.seed ^ 0x3Cu) & 0x7fffffffu);
	const double gravityScale = std::sqrt(9.81 / std::max(it.gravity, 0.05));
	const double radiusKm = core.radius * RADIUS_KM;
	const double radiusMetres = radiusKm * 1000.0;
	const bool pits = it.heatPipe > 0.5 || st.forcedLava;
	const double chanceShield = arcVents < 0 ? 0.7 : 0.15 * Soft(volcanicity, 0.4, 0.4);
	const double erupts = pits ? 0.4 : (st.active ? (st.glow > 0.0 ? 0.25 : 0.1) : 0.0);
	for (int i = 0; i < int(m_places.size()); i++) {
		double u[11];
		for (double &value : u)
			value = draws.Double();
		Vent v;
		v.centre = m_places[i];
		Axes(v.centre, v.axisA, v.axisB);
		if (pits)
			v.kind = PIT;
		else if (arcVents >= 0 && i >= arcVents)
			v.kind = u[0] < 0.5 ? SHIELD : CONE; // away from any plate boundary: a hotspot
		else
			v.kind = u[0] < chanceShield ? SHIELD : CONE;
		double tall;
		if (v.kind == CONE) {
			v.km = std::min((6.0 + 12.0 * u[1]) * gravityScale, 0.03 * radiusKm);
			// (tall and spiky: for looks, more than a real cone's usual height)
			tall = v.km * 1000.0 / (2.5 + 1.0 * u[2]);
			v.rim = 0.035 + 0.03 * u[3];
		} else if (v.kind == SHIELD) {
			v.km = std::min((40.0 + 80.0 * u[1]) * gravityScale * (1.0 + 1.5 * std::pow(u[4], 4.0)), 0.09 * radiusKm);
			// (real shields are lower still, and look flat)
			tall = v.km * 1000.0 / (7.0 + 5.0 * u[2]);
			v.rim = 0.06 + 0.06 * u[3];
		} else {
			v.km = std::min((15.0 + 55.0 * u[1]) * (1.0 + 1.2 * std::pow(u[4], 4.0)), 0.05 * radiusKm);
			tall = 300.0 + 1100.0 * u[2];
			v.rim = 0.35 + 0.2 * u[3];
		}
		v.seed = u[5];
		v.erupting = u[6] < erupts;
		// what is erupting is young; the rest are of any age. Only weather wears them, and more where there is more
		const double age = v.erupting ? 0.0 : u[9] * (st.weather ? 0.35 + 0.65 * st.wearSmall : 0.0);
		v.recent = u[7] < 0.4 && age < 0.5;
		v.map = -1;
		if (v.kind == CONE)
			v.map = age < 0.2 ? MAP_CONE_FRESH : (age < 0.45 ? MAP_CONE_MID : (age < 0.75 ? MAP_CONE_WORN : MAP_CONE_OLD));
		else if (v.kind == SHIELD)
			v.map = age < 0.4 ? MAP_SHIELD_FRESH : MAP_SHIELD_WORN;
		const double turn = u[8] * 2.0 * M_PI;
		v.turnCos = std::cos(turn);
		v.turnSin = std::sin(turn);
		v.mirror = u[10] < 0.5 ? 1.0 : -1.0;
		if (v.map >= 0)
			tall = v.km * 1000.0 * VolcanoMaps()[v.map].top;
		const double wanted = tall;
		v.height = std::min({ wanted, 0.6 * reliefLimit, 0.0065 * radiusMetres });
		// a limit on its height takes its width with it, or a large one would be a flat disc
		if (v.kind != PIT && v.height < wanted)
			v.km *= v.height / wanted;
		v.radius = v.km / radiusKm;
		// (its lava reaches further than the volcano)
		v.cosReach = std::cos(std::min(3.2 * v.radius, M_PI));
		v.ground = groundAt(v.centre);
		if (v.kind == PIT && (v.erupting || v.recent)) {
			Random tongues(uint32_t(v.seed * 1e9));
			const int tongueCount = 5 + int(v.seed * 5.0);
			const double reach = v.erupting ? 1.0 : 0.8 * (1.0 - 0.4 * m_wear);
			for (int k = 0; k < tongueCount; k++) {
				Tongue tongue;
				tongue.direction = 2.0 * M_PI * tongues.Double();
				tongue.length = (0.35 + 0.8 * tongues.Double()) * reach;
				tongue.bend = -0.6 + 1.2 * tongues.Double();
				tongue.width = 0.7 + 0.6 * tongues.Double();
				v.tongues.push_back(tongue);
			}
		}
		m_tallest = std::max(m_tallest, v.height);
		m_ventIndex.Add(v.centre, 3.2 * v.radius, uint16_t(m_vents.size()));
		m_vents.push_back(std::move(v));
	}
	BuildLakes(core, st, pits, reliefLimit, seaLevel, groundAt);
}

// Lava lakes are rare and small, around erupting vents: most real ones are a kilometre or two across, and only a lava
// planet has seas. A world like Io also has a few great ones, like Loki.
void TerrainVolcanoes::BuildLakes(const PlanetPhysics::Core &core, const PlanetPhysics::Surface &st, const bool pits, const double reliefLimit, const double seaLevel, const std::function<double(const vector3d &)> &groundAt)
{
	if (m_vents.empty() || st.glow <= 0.3 || st.molten <= 0.0 || st.molten > 0.3 || st.lavaPlanet > 0.5)
		return;
	const double radiusKm = core.radius * RADIUS_KM;
	const double keepOff = 0.004 * std::min(reliefLimit, 7000.0);
	Random rng((core.seed ^ 0x1Fu) & 0x7fffffffu);
	const auto clearOfOthers = [this](const vector3d &centre, const double radius, const Vent *own) {
		for (const Vent &vent : m_vents) {
			if (&vent != own && centre.Dot(vent.centre) >= std::cos(std::min(1.6 * vent.radius + 1.4 * radius, M_PI)))
				return false;
		}
		for (const Item &block : m_blocks) {
			if (centre.Dot(block.centre) >= std::cos(std::min(1.35 * block.radius + 1.4 * radius, M_PI)))
				return false;
		}
		return true;
	};

	if (!pits) {
		// A pool lies where a flow ends, so it has a reason: one for each erupting volcano, on the path that feeds it
		for (const Vent &source : m_vents) {
			if (source.map < 0 || !source.erupting)
				continue;
			const VolcanoMap &map = VolcanoMaps()[source.map];
			if (map.ends.empty())
				continue;
			const std::pair<double, double> &end = map.ends[std::min(size_t(source.seed * double(map.ends.size())), map.ends.size() - 1)];
			// from the map back to the planet, as AddMapped goes the other way
			const double mirrored = end.second * source.mirror;
			const double along = end.first * source.turnCos + mirrored * source.turnSin;
			const double across = -end.first * source.turnSin + mirrored * source.turnCos;
			const double t0 = std::sqrt(along * along + across * across);
			const double theta = std::atan2(across, along);
			const vector3d centre = (source.centre + (source.axisA * std::cos(theta) + source.axisB * std::sin(theta)) * std::tan(std::min(t0 * source.radius, 1.2))).Normalized();
			const double radius = std::min((1.0 + 2.0 * rng.Double()) / radiusKm, 0.03);
			const double seed = rng.Double();
			const double ground = groundAt(centre);
			if (ground < seaLevel || !clearOfOthers(centre, radius, &source))
				continue;
			const double cells = double(map.size - 1) / (2.0 * MAP_REACH);
			const double top = map.size > 0 ? map.HeightAt((end.first + MAP_REACH) * cells, (end.second + MAP_REACH) * cells, 0.0) : 0.0;
			m_lakeIndex.Add(centre, 1.4 * radius, uint16_t(m_lakes.size()));
			m_lakes.push_back(MakeItem(centre, radius, 1.4, ground + source.height * top - keepOff, seed));
		}
		return;
	}

	// On a world like Io, lakes lie in the open around its pits, a few of them great ones like Loki
	std::vector<const Vent *> sources;
	for (const Vent &vent : m_vents) {
		if (vent.erupting)
			sources.push_back(&vent);
	}
	if (sources.empty())
		for (const Vent &vent : m_vents)
			sources.push_back(&vent);
	const int great = 5 + rng.Int32(6);
	std::vector<const Vent *> beside;
	for (int i = 0; i < great; i++)
		beside.push_back(sources[rng.Int32(int(sources.size()))]);
	for (const Vent *vent : sources) {
		const int lakes = 1 + rng.Int32(3);
		for (int i = 0; i < lakes; i++)
			beside.push_back(vent);
	}
	for (int i = 0; i < int(beside.size()); i++) {
		const Vent &source = *beside[i];
		const double km = i < great ? 10.0 + 50.0 * rng.Double() : 1.0 + 5.0 * rng.Double();
		const double radius = std::min(km / radiusKm, 0.03);
		// a few tries for a place on land and off the volcanoes, close beside this one
		for (int attempt = 0; attempt < 8; attempt++) {
			const double direction = 2.0 * M_PI * rng.Double();
			const double away = (1.7 + 1.3 * rng.Double()) * source.radius + 1.4 * radius;
			const vector3d centre = (source.centre + (source.axisA * std::cos(direction) + source.axisB * std::sin(direction)) * std::tan(std::min(away, 1.2))).Normalized();
			const double seed = rng.Double();
			const double ground = groundAt(centre);
			if (ground < seaLevel || !clearOfOthers(centre, radius, nullptr))
				continue;
			m_lakeIndex.Add(centre, 1.4 * radius, uint16_t(m_lakes.size()));
			m_lakes.push_back(MakeItem(centre, radius, 1.4, ground - keepOff, seed));
			break;
		}
	}
}

void TerrainVolcanoes::AddVent(const Vent &v, const vector3d &p, const double spacing, Sample &out) const
{
	const double cosine = p.Dot(v.centre);
	if (cosine < v.cosReach)
		return;
	const double fade = Fade(v.radius, spacing);
	if (fade <= 0.0)
		return;
	const double t0 = std::acos(std::clamp(cosine, -1.0, 1.0)) / v.radius;
	const double theta = std::atan2(p.Dot(v.axisB), p.Dot(v.axisA));
	const double seed = v.seed;
	if (t0 < 2.0)
		out.nearby = true;
	// the lava it has spread around itself, in uneven lobes; none around an old one
	if (v.erupting || v.recent) {
		const double patches = std::clamp(SphereCells::Fbm(p, 2.5 / v.radius, 5, m_noiseSeed + 169, 0.55) / 0.35, -1.0, 1.0);
		const double x = 0.5 * t0 / Outline(theta, seed, 0.35) + 0.3 * patches;
		const double amount = v.erupting ? 1.0 : 0.55;
		out.field = std::max(out.field, amount * std::exp(-x * x) * (1.0 - Soft(t0, 2.6, 0.6)) * fade);
	}
	if (t0 >= 1.5)
		return;
	if (v.map >= 0) {
		AddMapped(v, p, spacing, fade, t0, theta, out);
		return;
	}

	// A pit: a low rim around a wide flat floor
	const double wobble = std::clamp(SphereCells::Fbm(p, 300.0, 7, m_noiseSeed + 161, 0.6) / 0.35, -1.0, 1.0);
	// the outline is irregular at the foot only: towards the summit it would fan out as star lines
	const double t = std::max(t0 / (1.0 + (Outline(theta, seed, 0.18) - 1.0) * std::clamp(t0 / 0.6, 0.0, 1.0)) + 0.02 * wobble, 0.0);
	const double edge = std::max(0.006, 1.2 * spacing / v.radius);
	const double rim = v.rim, H = v.height;
	const double x = std::min(t, 1.0);
	const double bowlBottom = 0.2 * H;
	const double h = t < rim ? H - 0.8 * H * (1.0 - std::pow(t / rim, 8.0)) : std::pow(1.0 - x, 1.2) / std::pow(1.0 - rim, 1.2) * H;
	out.height += h * fade;

	const double inside = std::clamp((1.45 - t0) / 0.35, 0.0, 1.0);
	const double influence = inside * inside * (3.0 - 2.0 * inside) * fade;
	if (influence > out.influence) {
		out.influence = influence;
		out.kind = v.kind;
		out.erupting = v.erupting;
	}
	out.slope = std::max(out.slope, Soft(1.05 - t, 0.0, 0.2) * fade);
	out.smooth = std::max(out.smooth, 0.8 * influence);

	// flows are separate tongues from the vent, with older ground between them; they spill over its rim, not across its floor
	if (!v.tongues.empty()) {
		const double tail = std::clamp((1.5 - t0) / 0.2, 0.0, 1.0);
		double m = 0.0;
		for (const Tongue &tongue : v.tongues) {
			const double raw = theta - tongue.direction - tongue.bend * t - 0.12 * std::sin(t * 9.0 + tongue.direction);
			const double a = raw - 2.0 * M_PI * std::floor((raw + M_PI) / (2.0 * M_PI));
			const double width = (0.01 + 0.05 * std::min(t / tongue.length, 1.1)) * tongue.width;
			m = std::max(m, Soft(width + 0.012 * wobble - std::abs(a) * t, 0.0, edge) * Soft(tongue.length - t + 0.05 * wobble, 0.0, 0.03));
		}
		m *= Soft(t, rim, 0.02) * tail * fade;
		if (v.erupting)
			out.flow = std::max(out.flow, m);
		else
			out.old = std::max(out.old, m);
	}

	// the floor, once the samples can show it
	const double crater = (spacing > 0.0 ? Soft(rim * v.radius / spacing, 0.5, 0.5) : 1.0) * fade;
	if (crater <= 0.0)
		return;
	const double floor = Soft(rim * 0.9 - t, 0.0, rim * 0.06) * crater;
	// a crescent of molten lava along one side of a dark floor
	if (v.erupting)
		out.lava = std::max(out.lava, floor * Soft(std::cos(theta - seed * 40.0) + 0.6 * wobble, 0.1, 0.15));
	out.old = std::max(out.old, floor);
	if (floor > out.floor) {
		out.floor = floor;
		out.level = v.ground + bowlBottom;
	}
}

void TerrainVolcanoes::AddMapped(const Vent &v, const vector3d &p, const double spacing, const double fade, const double t0, const double theta, Sample &out) const
{
	const VolcanoMap &map = VolcanoMaps()[v.map];
	if (map.size == 0 || t0 >= MAP_REACH)
		return;
	const double along = t0 * std::cos(theta), across = t0 * std::sin(theta);
	const double mapX = along * v.turnCos - across * v.turnSin;
	const double mapY = (along * v.turnSin + across * v.turnCos) * v.mirror;
	if (std::abs(mapX) >= MAP_REACH || std::abs(mapY) >= MAP_REACH)
		return;

	// The copy of the map whose cells are the size of the samples, so a volcano looks the same from afar as up close
	const double cells = double(map.size - 1) / (2.0 * MAP_REACH);
	const double cellSize = v.radius / cells;
	const double level = spacing > 0.0 ? std::log2(std::max(spacing / cellSize, 1.0)) : 0.0;
	const double x = (mapX + MAP_REACH) * cells, y = (mapY + MAP_REACH) * cells;
	const double top = map.HeightAt(x, y, level);
	out.height += v.height * top * fade;

	// (following the map, so a volcano's outline is the uneven one the map has)
	const double influence = Soft(top, 0.0, 0.04) * fade;
	if (influence > out.influence) {
		out.influence = influence;
		out.kind = v.kind;
		out.erupting = v.erupting;
	}
	out.slope = std::max(out.slope, Soft(top, 0.01, 0.04) * fade);
	// the map has its own shape down to its cells; finer than that, the ground's small relief goes on
	out.smooth = std::max(out.smooth, 0.5 * influence);

	// lava runs down the map's paths from the crater, with older ground between them; further on the younger it is
	if (v.erupting || v.recent) {
		const double path = map.FlowAt(x, y, level);
		const double reach = Soft((v.erupting ? 1.05 : 0.7) - t0, 0.0, 0.2);
		const double m = Soft(path, 0.25, 0.2) * reach * fade;
		if (v.erupting) {
			out.flow = std::max(out.flow, m);
			// molten near the vent where lava glows, cooling on the way down, a little further on some
			if (m_glowing)
				out.lava = std::max(out.lava, Soft(path, 0.4, 0.2) * (1.0 - Soft(t0, 0.15 + 0.3 * v.seed, 0.15)) * fade);
		} else {
			out.old = std::max(out.old, m);
		}
	}
	if (v.erupting && m_weather && v.kind == CONE) {
		// ash falls downwind and in patches, and thins out with no edge
		const double downwind = std::max(std::cos(theta - v.seed * 2.0 * M_PI), 0.0);
		const double patches = std::clamp(SphereCells::Fbm(p, 3.0 / v.radius, 4, m_noiseSeed + 163, 0.55) / 0.35, -1.0, 1.0);
		out.ash = std::max(out.ash, 0.8 * Soft(0.22 + 0.55 * downwind + 0.25 * patches - t0, 0.0, 0.4) * fade);
	}

	// the crater, once the samples can show it. The map has its shape; here, what lies in it.
	const double crater = (spacing > 0.0 ? Soft(map.rim * v.radius / spacing, 0.5, 0.5) : 1.0) * fade;
	if (crater <= 0.0)
		return;
	const double r = t0 / map.rim;
	if (v.erupting) {
		// the lowest part of its floor; its level is fixed, so lava lies flat
		// (only well inside the rim: the flank outside it is as low)
		const double pool = Soft(map.floor + 0.25 * (1.0 - map.floor) - top, 0.0, 0.01) * (1.0 - Soft(r, 0.45, 0.2)) * crater;
		out.lava = std::max(out.lava, pool);
		if (pool > out.floor) {
			out.floor = pool;
			out.level = v.ground + v.height * (map.floor + 0.25 * (1.0 - map.floor));
		}
	}
	out.rock = std::max(out.rock, (1.0 - Soft(r, 1.3, 0.8)) * crater);
}

TerrainVolcanoes::Sample TerrainVolcanoes::At(const vector3d &p) const
{
	Sample out;
	const std::vector<uint16_t> *vents = m_ventIndex.At(p);
	const std::vector<uint16_t> *lakes = m_lakeIndex.At(p);
	const bool anyVents = vents && !vents->empty(), anyLakes = lakes && !lakes->empty();
	if (!anyVents && !anyLakes)
		return out;
	const double spacing = Spacing();
	if (anyVents) {
		for (const uint16_t id : *vents)
			AddVent(m_vents[id], p, spacing, out);
	}
	if (anyLakes) {
		for (const uint16_t id : *lakes) {
			const Item &lake = m_lakes[id];
			const double cosine = p.Dot(lake.centre);
			if (cosine < lake.cosReach)
				continue;
			const double fade = Fade(lake.radius, spacing);
			if (fade <= 0.0)
				continue;
			const double theta = std::atan2(p.Dot(lake.axisB), p.Dot(lake.axisA));
			const double uneven = std::clamp(SphereCells::Fbm(p, 3.0 / lake.radius, 5, m_noiseSeed + 165, 0.55) / 0.35, -1.0, 1.0);
			const double d = std::acos(std::clamp(cosine, -1.0, 1.0)) / lake.radius / Outline(theta, lake.seed, 0.3) + 0.14 * uneven;
			const double m = Soft(1.0 - d, 0.0, std::max(0.08, 2.5 * spacing / lake.radius)) * fade;
			if (m <= 0.0)
				continue;
			out.lava = std::max(out.lava, m);
			if (m > out.floor) {
				out.floor = m;
				out.level = lake.height;
			}
		}
	}
	return out;
}

TerrainVolcanoes::Construct TerrainVolcanoes::ConstructAt(const vector3d &p) const
{
	Construct out;
	if (m_shields.empty() && m_blocks.empty() && m_paterae.empty())
		return out;
	const double spacing = Spacing();
	const auto distanceRatio = [&p](const Item &item, const double amount, const double fade) {
		const double angle = std::acos(std::clamp(p.Dot(item.centre), -1.0, 1.0));
		const double theta = std::atan2(p.Dot(item.axisB), p.Dot(item.axisA));
		const double t0 = angle / item.radius;
		// (for fade 0 the outline is irregular all the way; otherwise at the foot only)
		const double outline = Outline(theta, item.seed, amount) - 1.0;
		return t0 / (1.0 + outline * (fade > 0.0 ? std::clamp(t0 / fade, 0.0, 1.0) : 1.0));
	};
	if (const std::vector<uint16_t> *nearby = m_shieldIndex.At(p)) {
		for (const uint16_t id : *nearby) {
			const Item &item = m_shields[id];
			if (p.Dot(item.centre) < item.cosReach)
				continue;
			const double fade = Fade(item.radius, spacing);
			if (fade <= 0.0)
				continue;
			const double d = distanceRatio(item, 0.16, 0.6);
			const double cone = std::pow(std::max(1.0 - d, 0.0), 1.6) * item.height;
			// a summit pit
			const double pit = std::exp(-(d / 0.09) * (d / 0.09)) * 0.25 * item.height;
			out.height += (cone - pit) * fade;
		}
	}
	if (const std::vector<uint16_t> *nearby = m_blockIndex.At(p)) {
		// where blocks overlap, the higher one stands: one block doesn't sit on another
		double blocks = 0.0;
		for (const uint16_t id : *nearby) {
			const Item &item = m_blocks[id];
			if (p.Dot(item.centre) < item.cosReach)
				continue;
			const double fade = Fade(item.radius, spacing);
			if (fade <= 0.0)
				continue;
			const double uneven = std::clamp(SphereCells::Fbm(p, 3.0 / item.radius, 4, m_noiseSeed + 167, 0.55) / 0.35, -1.0, 1.0);
			const double d = distanceRatio(item, 0.3, 0.0) + 0.07 * uneven;
			// a scarp of real width, and debris fallen from it at the foot
			const double scarp = Soft(1.0 - d, 0.0, std::max(0.18, 2.5 * spacing / item.radius));
			const double foot = std::clamp((1.35 - d) / 0.35, 0.0, 1.0);
			const double apron = 0.15 * foot * foot;
			// a tilted top, and rough, not a flat plate
			const double tilt = 1.0 + 0.5 * (p - item.centre).Dot(vector3d(0.6, 0.3, -0.7)) / item.radius;
			const double rough = std::clamp(SphereCells::Fbm(p, 5.0 / item.radius, 7, m_noiseSeed + 168, 0.58) / 0.35, -1.0, 1.0);
			const double top = scarp * tilt * (1.0 + 0.3 * rough);
			blocks = std::max(blocks, item.height * std::max(top, apron) * fade);
			out.block = std::max(out.block, std::max(scarp, 2.0 * apron) * fade);
		}
		out.height += blocks;
	}
	if (const std::vector<uint16_t> *nearby = m_pateraIndex.At(p)) {
		for (const uint16_t id : *nearby) {
			const Item &item = m_paterae[id];
			if (p.Dot(item.centre) < item.cosReach)
				continue;
			const double fade = Fade(item.radius, spacing);
			if (fade <= 0.0)
				continue;
			const double d = distanceRatio(item, 0.25, 0.0);
			const double m = Soft(1.0 - d, 0.0, std::max(0.08, 2.5 * spacing / item.radius)) * fade;
			out.height -= item.height * m;
			out.patera = std::max(out.patera, m);
			// most are dark and quiet; a quarter hold lava
			if (item.seed < 0.25)
				out.pateraLava = std::max(out.pateraLava, m);
		}
	}
	return out;
}
