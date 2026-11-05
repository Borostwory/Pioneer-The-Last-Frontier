// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainPhysicalHydro.h"

#include "MathUtil.h"
#include "FracDef.h"
#include "SphereCells.h"
#include "TerrainNoise.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <numeric>
#include <queue>
#include <thread>

using SphereCells::Fbm;

namespace {
	std::atomic<uint64_t> s_nextId(1);

	// a river's width as a share of the body's radius, smallest to largest on that body
	constexpr double WIDTH_SMALL = 0.00006;
	constexpr double WIDTH_LARGE = 0.0008;
	// how far either side of a main river the ground is cut down to it, in metres, at least
	constexpr double MAIN_VALLEY = 6000.0;
	// pieces of a river are never shorter than this, in metres
	constexpr double SMALLEST_PIECE = 250.0;
	constexpr double SETTLE_PIECE = 12000.0; // metres between the points a river's level is checked at
	constexpr double SETTLE_ACROSS_ABOVE = 2000.0; // half widths above this, metres, are checked at the banks too
	constexpr double MAX_LAKE_SHARE = 0.0005; // of the land, the most one lake on the map can cover: on New Hope, about 575 km across
	// the grids
	constexpr int MOISTURE_WIDTH = 240;

	double Soft(const double x, const double low, const double width)
	{
		const double t = std::clamp((x - low) / std::max(width, 1e-9), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	vector3d GridPoint(const int r, const int c, const int w, const int h)
	{
		const double lat = M_PI / 2.0 - (r + 0.5) / h * M_PI;
		const double lon = (c + 0.5) / w * 2.0 * M_PI - M_PI;
		return vector3d(std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon));
	}

	double GridLatitude(const int r, const int h)
	{
		return M_PI / 2.0 - (r + 0.5) / h * M_PI;
	}

	double WeightedPercentile(const std::vector<float> &v, const std::vector<float> &w, const double q)
	{
		std::vector<int> order(v.size());
		std::iota(order.begin(), order.end(), 0);
		std::sort(order.begin(), order.end(), [&](const int a, const int b) { return v[a] < v[b]; });
		double total = 0.0;
		for (const float x : w)
			total += x;
		double sum = 0.0;
		for (const int i : order) {
			sum += w[i];
			if (sum >= q * total)
				return v[i];
		}
		return order.empty() ? 0.0 : v[order.back()];
	}
} // namespace

void ParallelRows(const int rows, const std::function<void(int)> &row)
{
	const int count = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
	std::atomic<int> next(0);
	auto work = [&]() {
		for (int r = next++; r < rows; r = next++)
			row(r);
	};
	std::vector<std::thread> threads;
	for (int i = 1; i < count; i++)
		threads.emplace_back(work);
	work();
	for (std::thread &t : threads)
		t.join();
}

TerrainHydro::Place TerrainHydro::PlaceOf(const vector3d &p)
{
	Place place;
	place.row01 = (M_PI / 2.0 - std::asin(std::clamp(p.y, -1.0, 1.0))) / M_PI;
	place.col01 = (std::atan2(p.z, p.x) + M_PI) / (2.0 * M_PI);
	return place;
}

void TerrainHydro::Field::Resize(const int w, const int h)
{
	width = w;
	height = h;
	values.assign(size_t(w) * h, 0.f);
}

void TerrainHydro::Field::FindRange()
{
	low = 1e30f;
	high = -1e30f;
	for (const float v : values) {
		low = std::min(low, v);
		high = std::max(high, v);
	}
}

double TerrainHydro::Field::Linear(const Place &place) const
{
	if (values.empty())
		return 0.0;
	const double r = std::clamp(place.row01 * height - 0.5, 0.0, double(height - 1));
	const double c = place.col01 * width - 0.5;
	const int r0 = std::min(int(r), height - 2 < 0 ? 0 : height - 2);
	const int c0 = int(std::floor(c));
	const double fr = r - r0, fc = c - c0;
	const int c1 = (c0 + 1 + width) % width, c2 = (c0 + width) % width;
	const int r1 = std::min(r0 + 1, height - 1);
	const double top = At(r0, c2) * (1.0 - fc) + At(r0, c1) * fc;
	const double bottom = At(r1, c2) * (1.0 - fc) + At(r1, c1) * fc;
	return top * (1.0 - fr) + bottom * fr;
}

double TerrainHydro::Field::Smooth(const Place &place) const
{
	if (values.empty())
		return 0.0;
	const double r = std::clamp(place.row01 * height - 0.5, 0.0, double(height - 1));
	const double c = place.col01 * width - 0.5;
	const int r0 = int(std::floor(r)), c0 = int(std::floor(c));
	const double fr = r - r0, fc = c - c0;
	auto weights = [](const double t, double w[4]) {
		const double t2 = t * t, t3 = t2 * t;
		w[0] = -0.5 * t3 + t2 - 0.5 * t;
		w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
		w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
		w[3] = 0.5 * t3 - 0.5 * t2;
	};
	double wr[4], wc[4];
	weights(fr, wr);
	weights(fc, wc);
	double sum = 0.0;
	for (int i = 0; i < 4; i++) {
		const int rr = std::clamp(r0 - 1 + i, 0, height - 1);
		double line = 0.0;
		for (int j = 0; j < 4; j++) {
			const int cc = ((c0 - 1 + j) % width + width) % width;
			line += wc[j] * At(rr, cc);
		}
		sum += wr[i] * line;
	}
	return std::clamp(sum, double(low), double(high));
}

void TerrainHydro::Field::Blur(const double sigma)
{
	const int radius = std::max(1, int(4.0 * sigma + 0.5));
	std::vector<double> kernel(radius * 2 + 1);
	double total = 0.0;
	for (int i = -radius; i <= radius; i++) {
		kernel[i + radius] = std::exp(-0.5 * i * i / (sigma * sigma));
		total += kernel[i + radius];
	}
	for (double &k : kernel)
		k /= total;

	std::vector<float> line(values.size());
	for (int r = 0; r < height; r++) {
		for (int c = 0; c < width; c++) {
			double sum = 0.0;
			for (int i = -radius; i <= radius; i++)
				sum += kernel[i + radius] * At(r, ((c + i) % width + width) % width);
			line[size_t(r) * width + c] = float(sum);
		}
	}
	for (int r = 0; r < height; r++) {
		for (int c = 0; c < width; c++) {
			double sum = 0.0;
			for (int i = -radius; i <= radius; i++)
				sum += kernel[i + radius] * line[size_t(std::clamp(r + i, 0, height - 1)) * width + c];
			At(r, c) = float(sum);
		}
	}
}

// Rain: air carries moisture from the sea along the wind, loses it over land and
// on the way up mountains, and falls by latitude belt, temperature and air density.
void TerrainHydro::BuildMoisture(const Params &P, const Field &h0, const std::function<double(const vector3d &)> &warpAt)
{
	const int W = MOISTURE_WIDTH, H = W / 2;
	const double env = std::max(P.reliefLimit, 1.0);
	Field ground, warp, dx, dy;
	ground.Resize(W, H);
	warp.Resize(W, H);
	dx.Resize(W, H);
	dy.Resize(W, H);
	for (int r = 0; r < H; r++) {
		const double lat = GridLatitude(r, H);
		const double absLat = std::abs(lat);
		for (int c = 0; c < W; c++) {
			const vector3d p = GridPoint(r, c, W, H);
			ground.At(r, c) = float(h0.Linear(PlaceOf(p)));
			const double w = warpAt(p);
			warp.At(r, c) = float(w);
			// the wind: bands from rotation and latitude
			const double zonal = absLat < 0.52 ? -1.0 : (absLat < 1.05 ? 1.0 : -0.6);
			const double side = lat > 0.0 ? 1.0 : -1.0;
			const double merid = 0.35 * side * (absLat < 0.52 ? -1.0 : 1.0) + 0.6 * w;
			const double length = std::max(std::sqrt(zonal * zonal + merid * merid), 1e-6);
			dx.At(r, c) = float(zonal / length / std::max(std::cos(lat), 0.12));
			dy.At(r, c) = float(-merid / length);
		}
	}
	auto back = [&](const std::vector<float> &a, const int r, const int c) {
		const double y = std::clamp(r - dy.At(r, c) * 1.5, 0.0, double(H - 1));
		const double x = std::fmod(c - dx.At(r, c) * 1.5 + W * 8.0, double(W));
		const int y0 = std::min(int(y), H - 1), y1 = std::min(y0 + 1, H - 1);
		const int x0 = int(x) % W, x1 = (x0 + 1) % W;
		const double fy = y - y0, fx = x - std::floor(x);
		const double top = a[size_t(y0) * W + x0] * (1.0 - fx) + a[size_t(y0) * W + x1] * fx;
		const double bottom = a[size_t(y1) * W + x0] * (1.0 - fx) + a[size_t(y1) * W + x1] * fx;
		return top * (1.0 - fy) + bottom * fy;
	};

	std::vector<float> carried(size_t(W) * H), next(size_t(W) * H), rise(size_t(W) * H);
	for (int r = 0; r < H; r++) {
		for (int c = 0; c < W; c++) {
			const size_t i = size_t(r) * W + c;
			carried[i] = ground.values[i] < P.seaLevel ? 1.f : 0.f;
			rise[i] = float(std::clamp((ground.values[i] - back(ground.values, r, c)) / (0.14 * env), 0.0, 1.0));
		}
	}
	for (int step = 0; step < 70; step++) {
		for (int r = 0; r < H; r++) {
			for (int c = 0; c < W; c++) {
				const size_t i = size_t(r) * W + c;
				next[i] = ground.values[i] < P.seaLevel ? 1.f : float(back(carried, r, c) * 0.992 * (1.0 - 0.3 * rise[i]));
			}
		}
		carried.swap(next);
	}

	// wet equator, dry belts at 20-35 degrees, wet mid-latitudes, dry poles
	// hot worlds evaporate more than falls; thin air and a small sea carry little
	const double hot = 1.0 - Soft(P.temperature, 305.0, 45.0);
	const double airCarry = Soft(P.airDensity, 0.1, 0.9);
	const double seaCarry = 0.35 + 0.65 * std::min(P.seaShare / 0.5, 1.0);
	moisture.Resize(W, H);
	for (int r = 0; r < H; r++) {
		const double lat = GridLatitude(r, H);
		for (int c = 0; c < W; c++) {
			const size_t i = size_t(r) * W + c;
			const vector3d p = GridPoint(r, c, W, H);
			const double w = warp.values[i];
			const double l = std::abs(std::abs(lat) + 0.16 * w + 0.1 * Fbm(p, 3.1, 3, P.noiseSeed + 66));
			const double a = l / 0.24, b = (l - 0.95) / 0.24, d = (l - 0.48) / 0.13;
			const double belt = 0.95 * std::exp(-a * a) + 0.28 + 0.62 * std::exp(-b * b) - 0.25 * std::exp(-d * d) - 0.22 * Soft(l, 1.25, 0.2) + 0.14 * w;
			moisture.values[i] = float(carried[i] * std::max(belt, 0.0) * (1.0 + 0.7 * rise[i]) * hot * (0.35 + 0.65 * airCarry) * seaCarry);
		}
	}
	moisture.Blur(1.0);
	for (float &v : moisture.values)
		v = std::clamp(v * 1.7f, 0.f, 1.f);
	moisture.FindRange();
}

// Priority flood from the sea finds where every cell drains and which basins fill;
// dry basins become salt flats and mud. A world with no rain has neither, though the
// interior can leave it old dry valleys.
void TerrainHydro::BuildDrainage(const Params &P, const Field &h0)
{
	m_id = s_nextId++;
	m_seed = P.noiseSeed;
	m_radius = P.radiusMetres;
	m_seaLevel = P.seaLevel;
	const int W = h0.width, H = h0.height;
	const size_t N = size_t(W) * H;
	const double env = std::max(P.reliefLimit, 1.0);

	// routing height: a little mid-scale variation, so rivers curve as real ones do
	std::vector<float> h(N), area(N), rain(N);
	ParallelRows(H, [&](const int r) {
		TerrainNoise::ScopedBandLimit bandLimit(43.0);
		const double cosLat = std::cos(GridLatitude(r, H));
		for (int c = 0; c < W; c++) {
			const size_t i = size_t(r) * W + c;
			const vector3d p = GridPoint(r, c, W, H);
			h[i] = float(h0.values[i] + 0.012 * env * Fbm(p, 26.0, 3, P.noiseSeed + 61));
			area[i] = float(cosLat);
			rain[i] = P.wet ? float(moisture.Linear(PlaceOf(p))) : 0.4f;
		}
	});

	const bool hasSea = P.seaLevel > -1e29;
	const double sea = hasSea ? P.seaLevel : WeightedPercentile(h, area, 0.015);
	const double low = P.seaShare > 0.0 ? WeightedPercentile(h0.values, area, std::max(P.seaShare * 0.72, 0.004)) : sea;

	std::vector<float> fill;
	std::vector<int> parent, order;
	std::vector<double> acc;
	using Entry = std::pair<float, int>;
	// sinks: cells that water ends in besides the sea, and their levels
	auto flood = [&](const std::vector<std::pair<int, float>> &sinks) {
		fill = h;
		parent.assign(N, -1);
		order.clear();
		order.reserve(N);
		std::vector<char> done(N, 0);
		std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
		for (size_t i = 0; i < N; i++) {
			if (h0.values[i] < low) {
				done[i] = 1;
				heap.push({ h[i], int(i) });
			}
		}
		if (heap.empty()) {
			std::vector<float> uniform(N, 1.f);
			const double lowest = WeightedPercentile(h0.values, uniform, 0.01);
			for (size_t i = 0; i < N; i++) {
				if (h0.values[i] <= lowest) {
					done[i] = 1;
					heap.push({ h[i], int(i) });
				}
			}
		}
		for (const auto &sink : sinks) {
			done[sink.first] = 1;
			fill[sink.first] = sink.second;
			heap.push({ sink.second, sink.first });
		}
		while (!heap.empty()) {
			const Entry top = heap.top();
			heap.pop();
			order.push_back(top.second);
			const int r = top.second / W, c = top.second % W;
			for (int dr = -1; dr <= 1; dr++) {
				const int rr = r + dr;
				if (rr < 0 || rr >= H)
					continue;
				for (int dc = -1; dc <= 1; dc++) {
					if (dr == 0 && dc == 0)
						continue;
					const int cc = (c + dc + W) % W;
					const size_t k = size_t(rr) * W + cc;
					if (done[k])
						continue;
					done[k] = 1;
					const float v = std::max(top.first, h[k]);
					fill[k] = v;
					parent[k] = top.second;
					heap.push({ v, int(k) });
				}
			}
		}

		acc.assign(N, 0.0);
		for (size_t i = 0; i < N; i++)
			acc[i] = h0.values[i] >= low ? double(area[i]) * rain[i] : 0.0;
		for (auto it = order.rbegin(); it != order.rend(); ++it) {
			if (parent[*it] >= 0)
				acc[parent[*it]] += acc[*it];
		}
	};
	flood({});

	// basins: where flooding raised the surface. Rain decides lake or dry flat
	Field lakeGrid, dryGrid;
	lakeGrid.Resize(W, H);
	dryGrid.Resize(W, H);
	{
		std::vector<int> label;
		struct Basin {
			double cells = 0.0, inflow = 0.0, rainSum = 0.0;
			int count = 0;
			bool holdsSink = false;
		};
		std::vector<Basin> basins;
		const double deep = 0.006 * env;
		auto findBasins = [&]() {
			label.assign(N, -1);
			basins.clear();
			std::vector<int> stack;
			for (size_t i = 0; i < N; i++) {
				if (label[i] >= 0 || fill[i] - h[i] <= deep)
					continue;
				const int id = int(basins.size());
				basins.emplace_back();
				Basin &basin = basins.back();
				label[i] = id;
				stack.push_back(int(i));
				while (!stack.empty()) {
					const int k = stack.back();
					stack.pop_back();
					basin.cells += area[k];
					basin.inflow = std::max(basin.inflow, acc[k]);
					basin.rainSum += rain[k];
					basin.count++;
					const int r = k / W, c = k % W;
					const int around[4] = { r > 0 ? k - W : -1, r + 1 < H ? k + W : -1, c > 0 ? k - 1 : -1, c + 1 < W ? k + 1 : -1 };
					for (const int n : around) {
						if (n >= 0 && label[n] < 0 && fill[n] - h[n] > deep) {
							label[n] = id;
							stack.push_back(n);
						}
					}
				}
			}
		};
		findBasins();
		double landCells = 0.0;
		for (size_t i = 0; i < N; i++) {
			if (!hasSea || h0.values[i] >= sea)
				landCells += area[i];
		}
		landCells = std::max(landCells, 1.0);
		const double largestLake = MAX_LAKE_SHARE * landCells;
		const auto wantsLake = [&](const Basin &b) {
			return P.wet && b.rainSum / b.count >= 0.16 && b.inflow > 2.0 * b.cells;
		};

		// A basin too large to fill keeps a lake as large as it can be: its water rises from the lowest
		// cell until it covers that much. Then it doesn't spill: all around drains to it, and the rivers
		// are worked out again, so none runs across the ground it no longer covers.
		std::vector<std::pair<int, float>> sinks;
		for (int j = 0; j < int(basins.size()); j++) {
			if (!wantsLake(basins[j]) || basins[j].cells <= largestLake)
				continue;
			int lowest = -1;
			for (size_t i = 0; i < N; i++) {
				if (label[i] == j && (lowest < 0 || h[i] < h[lowest]))
					lowest = int(i);
			}
			std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> rising;
			std::vector<char> seen(N, 0);
			std::vector<int> covered;
			rising.push({ h[lowest], lowest });
			seen[lowest] = 1;
			double grown = 0.0;
			float level = h[lowest];
			while (!rising.empty() && grown < largestLake) {
				const int k = rising.top().second;
				rising.pop();
				covered.push_back(k);
				grown += area[k];
				level = std::max(level, h[k]);
				const int r = k / W, c = k % W;
				const int around[4] = { r > 0 ? k - W : -1, r + 1 < H ? k + W : -1, c > 0 ? k - 1 : -1, c + 1 < W ? k + 1 : -1 };
				for (const int n : around) {
					if (n >= 0 && !seen[n] && label[n] == j) {
						seen[n] = 1;
						rising.push({ h[n], n });
					}
				}
			}
			for (const int k : covered)
				sinks.emplace_back(k, level);
		}
		if (!sinks.empty()) {
			flood(sinks);
			findBasins();
			for (const auto &sink : sinks) {
				if (label[sink.first] >= 0)
					basins[label[sink.first]].holdsSink = true;
			}
		}

		std::vector<int> ranked(basins.size());
		std::iota(ranked.begin(), ranked.end(), 0);
		std::sort(ranked.begin(), ranked.end(), [&](const int a, const int b) { return basins[a].cells > basins[b].cells; });
		std::vector<char> kind(basins.size(), 0); // 1 lake, 2 dry
		double lakeArea = 0.0, dryArea = 0.0;
		for (const int j : ranked) {
			const Basin &b = basins[j];
			// only its lowest part is water, the sinks: the hollows around it joined to them stay land
			if (b.holdsSink) {
				lakeArea += largestLake;
				continue;
			}
			if (b.cells < 3.0)
				continue;
			if (wantsLake(b) && b.cells <= largestLake && lakeArea + b.cells < 0.035 * landCells) {
				kind[j] = 1;
				lakeArea += b.cells;
			} else if (dryArea + b.cells < 0.025 * landCells) {
				kind[j] = 2;
				dryArea += b.cells;
			}
		}
		for (size_t i = 0; i < N; i++) {
			if (label[i] < 0)
				continue;
			if (kind[label[i]] == 1)
				lakeGrid.values[i] = 1.f;
			else if (kind[label[i]] == 2)
				dryGrid.values[i] = 1.f;
		}
		for (const auto &sink : sinks)
			lakeGrid.values[sink.first] = 1.f;
	}

	auto aboveSea = [&](Field &field) {
		// a valley on land reaches down to the sea at most
		if (!hasSea)
			return;
		for (size_t i = 0; i < N; i++) {
			if (fill[i] >= sea)
				field.values[i] = std::min(field.values[i], float(std::max(fill[i] - sea - 5.0, 0.0)));
		}
	};

	valley.Resize(W, H);
	if (P.wet) {
		std::vector<double> landAcc;
		for (size_t i = 0; i < N; i++) {
			if (h0.values[i] >= sea)
				landAcc.push_back(acc[i]);
		}
		double threshold = 1.0;
		if (!landAcc.empty()) {
			const size_t at = size_t(0.96 * double(landAcc.size() - 1));
			std::nth_element(landAcc.begin(), landAcc.begin() + at, landAcc.end());
			threshold = std::max(landAcc[at], 1.0);
		}
		std::vector<float> river(N);
		drowned.Resize(W, H);
		mouth.Resize(W, H);
		corridor.Resize(W, H);
		Field seaNear;
		seaNear.Resize(W, H);
		for (size_t i = 0; i < N; i++) {
			const bool land = h0.values[i] >= sea;
			const double q = std::clamp(std::log(std::max(acc[i] / threshold, 1e-3)) / std::log(40.0), 0.0, 1.0);
			const double strength = std::clamp(0.45 + q, 0.0, 1.0);
			const bool big = acc[i] >= threshold;
			// streams run dry in deserts; large rivers can cross them
			river[i] = (big && land && lakeGrid.values[i] < 0.5f) ? float(strength) : 0.f;
			corridor.values[i] = river[i];
			const double carved = (big && h0.values[i] >= low) ? strength : 0.0;
			valley.values[i] = float(carved);
			drowned.values[i] = float(carved * (q > 0.42 ? std::clamp((q - 0.42) / 0.35, 0.35, 1.0) : 0.0));
			seaNear.values[i] = land ? 0.f : 1.f;
		}
		seaNear.Blur(1.2);
		for (size_t i = 0; i < N; i++) {
			const bool land = h0.values[i] >= sea;
			mouth.values[i] = (acc[i] >= threshold * 3.0 && land && seaNear.values[i] > 0.02f) ? 1.f : 0.f;
		}
		mouth.Blur(2.2);
		corridor.Blur(2.4);
		valley.Blur(0.7);
		for (float &v : valley.values)
			v *= float(0.14 * env);
		aboveSea(valley);
		drowned.Blur(0.6);
		for (float &v : drowned.values)
			v *= float(0.11 * env);

		// the rivers as lines: each river cell joined to the next downstream, with its node moved
		// off the grid, at the level of their carved valleys, which never rises downstream
		std::vector<int> cells;
		double smallest = 1e30;
		for (size_t i = 0; i < N; i++) {
			if (river[i] > 0.f && parent[i] >= 0) {
				cells.push_back(int(i));
				smallest = std::min(smallest, acc[i]);
			}
		}
		std::sort(cells.begin(), cells.end(), [&](const int a, const int b) { return acc[a] < acc[b]; });
		std::vector<double> level(N);
		for (size_t i = 0; i < N; i++)
			level[i] = double(fill[i]) - valley.values[i];
		for (const int i : cells)
			level[parent[i]] = std::min(level[parent[i]], level[i]);
		const double cell = 2.0 * M_PI / W;
		auto node = [&](const int i) {
			const int r = i / W, c = i % W;
			const vector3d jitter(SphereCells::Hash(r, c, 0, m_seed), SphereCells::Hash(r, c, 0, m_seed + 1), SphereCells::Hash(r, c, 0, m_seed + 2));
			return (GridPoint(r, c, W, H) + jitter * (0.4 * cell)).Normalized();
		};
		m_segments.reserve(cells.size());
		for (const int i : cells) {
			Segment s;
			s.a = node(i);
			s.b = node(parent[i]);
			s.middle = (s.a + s.b).Normalized();
			s.levelA = float(level[i]);
			s.levelB = float(level[parent[i]]);
			const double big = std::clamp(std::log(std::max(acc[i] / std::max(smallest, 1e-9), 1.0)) / std::log(40.0), 0.0, 1.0);
			const double width = P.radiusMetres * (WIDTH_SMALL + (WIDTH_LARGE - WIDTH_SMALL) * big);
			s.halfWidth = float(0.5 * width);
			// half its length, the bends' room, and its valley
			s.reach = float(0.8 * (s.a - s.b).Length() + std::max(MAIN_VALLEY, 4.0 * width) / P.radiusMetres);
			m_segments.push_back(s);
		}
	} else {
		std::vector<double> landAcc;
		for (size_t i = 0; i < N; i++) {
			if (h0.values[i] >= sea)
				landAcc.push_back(acc[i]);
		}
		double threshold = 1.0;
		if (!landAcc.empty()) {
			const size_t at = size_t(0.985 * double(landAcc.size() - 1));
			std::nth_element(landAcc.begin(), landAcc.begin() + at, landAcc.end());
			threshold = std::max(landAcc[at], 1.0);
		}
		for (size_t i = 0; i < N; i++)
			valley.values[i] = (acc[i] >= threshold && h0.values[i] >= sea) ? 1.f : 0.f;
		valley.Blur(0.7);
		for (float &v : valley.values)
			v *= float(0.05 * env);
		aboveSea(valley);
	}

	// the height of the water in a river or a lake, from the map's own valleys
	surface.Resize(W, H);
	for (size_t i = 0; i < N; i++)
		surface.values[i] = fill[i] - valley.values[i];
	surface.Blur(1.0);
	if (hasSea) {
		for (float &v : surface.values)
			v = std::max(v, float(sea + 2.0));
	}

	lakeGrid.Blur(0.5);
	lake = std::move(lakeGrid);
	dryGrid.Blur(0.5);
	dry = std::move(dryGrid);

	for (Field *f : { &valley, &drowned, &mouth, &corridor, &lake, &dry, &surface }) {
		if (!f->Empty())
			f->FindRange();
	}
}

void TerrainHydro::BuildSlope(const Field &h0, const double radiusMetres)
{
	const int W = h0.width, H = h0.height;
	const double cell = 2.0 * M_PI / W * radiusMetres;
	slope.Resize(W, H);
	for (int r = 0; r < H; r++) {
		const double cosLat = std::max(std::cos(GridLatitude(r, H)), 0.05);
		const int up = std::max(r - 1, 0), down = std::min(r + 1, H - 1);
		for (int c = 0; c < W; c++) {
			const double dx = (h0.At(r, (c + 1) % W) - h0.At(r, (c - 1 + W) % W)) / (2.0 * cell * cosLat);
			const double dy = (h0.At(down, c) - h0.At(up, c)) / (std::max(down - up, 1) * cell);
			slope.At(r, c) = float(std::min(std::sqrt(dx * dx + dy * dy), 0.03));
		}
	}
	slope.Blur(1.0);
	slope.FindRange();
}

void TerrainHydro::SetSeaLevel(const double seaLevel)
{
	m_seaLevel = seaLevel;
	if (!surface.Empty()) {
		for (float &v : surface.values)
			v = std::max(v, float(seaLevel + 2.0));
		surface.FindRange();
	}
}

// Splits until no piece is longer than the target. Each new point's bend depends only on where it
// is, so a line is the same in every view, with more of its detail shown closer up.
void TerrainHydro::SplitLine(const Segment &s, const double target, std::vector<vector3d> &line, std::vector<vector3d> &next) const
{
	line.clear();
	line.push_back(s.a);
	line.push_back(s.b);
	const double length = (s.a - s.b).Length();
	const int steps = std::min(int(std::ceil(std::log2(std::max(length / target, 1.0)))), 10);
	double amount = 0.28;
	for (int k = 0; k < steps; k++) {
		next.clear();
		next.push_back(line[0]);
		for (size_t i = 0; i + 1 < line.size(); i++) {
			const vector3d seg = line[i + 1] - line[i];
			const vector3d mid = (line[i + 1] + line[i]).Normalized();
			vector3d side = mid.Cross(seg);
			side = side * (1.0 / std::max(side.Length(), 1e-12));
			const double scale = 67108864.0; // 2^26
			const double bend = SphereCells::Hash(int64_t(std::floor(mid.x * scale)), int64_t(std::floor(mid.y * scale)), int64_t(std::floor(mid.z * scale)), m_seed + 7);
			next.push_back((mid + side * (seg.Length() * amount * bend)).Normalized());
			next.push_back(line[i + 1]);
		}
		line.swap(next);
		amount *= 0.9;
	}
}

void TerrainHydro::SettleLevels(const std::function<double(const vector3d &)> &ground)
{
	if (m_segments.empty())
		return;
	const double target = SETTLE_PIECE / m_radius;
	struct Low {
		float firstHalf, all;
	};
	std::vector<Low> lows(m_segments.size());
	ParallelRows(int(m_segments.size()), [&](const int i) {
		const Segment &s = m_segments[i];
		std::vector<vector3d> line, next;
		SplitLine(s, target, line, next);
		const double across = s.halfWidth / m_radius;
		const bool wide = s.halfWidth > SETTLE_ACROSS_ABOVE;
		double firstHalf = 1e30, all = 1e30;
		for (size_t j = 0; j < line.size(); j++) {
			const vector3d &p = line[j];
			double low = ground(p);
			if (wide) {
				const vector3d along = line[j + 1 < line.size() ? j + 1 : j - 1] - p;
				vector3d side = p.Cross(along);
				side = side * (1.0 / std::max(side.Length(), 1e-12));
				low = std::min(low, std::min(ground((p + side * across).Normalized()), ground((p - side * across).Normalized())));
			}
			all = std::min(all, low);
			if (2 * j <= line.size())
				firstHalf = std::min(firstHalf, low);
		}
		lows[i] = { float(firstHalf), float(all) };
	});

	// A river's end is exactly the start of the next one downstream, so they share one level. The segments
	// are in order of the water they carry, so what is upstream comes first.
	std::map<std::array<double, 3>, double> nodes;
	auto nodeAt = [&](const vector3d &p) -> double & {
		return nodes.try_emplace(std::array<double, 3>{ p.x, p.y, p.z }, 1e30).first->second;
	};
	for (size_t i = 0; i < m_segments.size(); i++) {
		const Segment &s = m_segments[i];
		double &a = nodeAt(s.a);
		a = std::min(a, std::min(double(s.levelA), double(lows[i].firstHalf)));
		double &b = nodeAt(s.b);
		b = std::min(b, std::min(double(s.levelB), double(lows[i].all)));
	}
	for (const Segment &s : m_segments) {
		const double a = nodeAt(s.a);
		double &b = nodeAt(s.b);
		b = std::min(b, a);
	}
	for (Segment &s : m_segments) {
		s.levelA = float(nodeAt(s.a));
		s.levelB = float(nodeAt(s.b));
	}
}

void TerrainHydro::Gather(const vector3d &centre, const double radius, const double spacing, std::vector<Piece> &out) const
{
	out.clear();
	const double target = std::max(spacing, SMALLEST_PIECE / m_radius);
	std::vector<vector3d> line, next;
	for (const Segment &s : m_segments) {
		const double reach = radius + s.reach;
		if (centre.Dot(s.middle) < std::cos(std::min(reach, M_PI)))
			continue;
		SplitLine(s, target, line, next);
		const size_t pieces = line.size() - 1;
		for (size_t i = 0; i < pieces; i++) {
			const double t0 = double(i) / pieces, t1 = double(i + 1) / pieces;
			Piece piece;
			piece.a = line[i];
			piece.b = line[i + 1];
			piece.levelA = float(s.levelA + (s.levelB - s.levelA) * t0);
			piece.levelB = float(s.levelA + (s.levelB - s.levelA) * t1);
			piece.halfWidth = s.halfWidth;
			out.push_back(piece);
		}
	}
}

void TerrainHydro::RiverNodes(std::vector<RiverNode> &all, std::vector<RiverNode> &forks, std::vector<RiverNode> &ends) const
{
	struct Node {
		vector3d position;
		double width = 0.0;
		int in = 0, out = 0;
	};
	// a segment's end is exactly the start of the next one downstream
	std::map<std::array<double, 3>, Node> nodes;
	auto nodeAt = [&](const vector3d &p, const double width) -> Node & {
		Node &node = nodes[{ p.x, p.y, p.z }];
		node.position = p;
		node.width = std::max(node.width, width);
		return node;
	};
	for (const Segment &s : m_segments) {
		nodeAt(s.a, 2.0 * s.halfWidth).out++;
		nodeAt(s.b, 2.0 * s.halfWidth).in++;
	}
	for (const auto &entry : nodes) {
		const Node &node = entry.second;
		const RiverNode out = { node.position, node.width };
		all.push_back(out);
		if (node.in >= 2)
			forks.push_back(out);
		if (node.out == 0)
			ends.push_back(out);
	}
}

void TerrainHydro::RiverPoints(const vector3d &p, const double radius, const double pieceMetres, std::vector<vector3d> &points, std::vector<vector3d> &tangents) const
{
	if (m_segments.empty())
		return;
	std::vector<Piece> pieces;
	Gather(p, radius, pieceMetres / m_radius, pieces);
	points.reserve(points.size() + pieces.size());
	tangents.reserve(tangents.size() + pieces.size());
	for (const Piece &piece : pieces) {
		const vector3d along = piece.b - piece.a;
		points.push_back(piece.a);
		tangents.push_back(along * (1.0 / std::max(along.Length(), 1e-12)));
	}
}

bool TerrainHydro::RiverAt(const vector3d &p, River &out) const
{
	if (m_segments.empty())
		return false;
	// Queries come in runs of nearby points, so the river pieces around them are kept per thread, for the
	// few band limits a patch uses in turn. The radius covers a whole grid of two patches from its corner.
	thread_local Nearby views[3];
	thread_local uint32_t clock = 0;
	const double band = SphereCells::RoundedBand(TerrainNoise::s_maxFrequency);
	Nearby *view = nullptr;
	for (Nearby &n : views) {
		if (n.owner == m_id && n.band == band && p.Dot(n.centre) >= n.cosRadius) {
			view = &n;
			break;
		}
	}
	if (!view) {
		view = &views[0];
		for (Nearby &n : views) {
			if (n.used < view->used)
				view = &n;
		}
		const double spacing = band > 0.0 ? 0.5 / band : 0.0;
		const double radius = std::min(std::max(100.0 * spacing, 40000.0 / m_radius), M_PI);
		Gather(p, radius, spacing, view->pieces);
		view->owner = m_id;
		view->band = band;
		view->centre = p;
		view->cosRadius = std::cos(radius);
		view->cosMargin = 2.0;
		view->marginMetres = radius / 8.0 * m_radius;
		// A river narrower than the samples would show as dashes, or not at all. So it's drawn as wide as
		// the samples can show until it's close enough to have its real width. Its line stays the same.
		view->smallestHalfWidth = spacing * m_radius;
	}
	const double smallestHalfWidth = view->smallestHalfWidth;
	view->used = ++clock;
	if (p.Dot(view->activeCentre) < view->cosMargin) {
		// for any point within the margin, the nearest edge is at most twice the margin further from here
		double best = 1e30;
		for (const Piece &piece : view->pieces) {
			const vector3d ab = piece.b - piece.a;
			const double t = std::clamp((p - piece.a).Dot(ab) / std::max(ab.Dot(ab), 1e-18), 0.0, 1.0);
			best = std::min(best, (p - (piece.a + ab * t)).Length() * m_radius - std::max(double(piece.halfWidth), smallestHalfWidth));
		}
		view->active.clear();
		for (const Piece &piece : view->pieces) {
			const vector3d ab = piece.b - piece.a;
			const double t = std::clamp((p - piece.a).Dot(ab) / std::max(ab.Dot(ab), 1e-18), 0.0, 1.0);
			if ((p - (piece.a + ab * t)).Length() * m_radius - std::max(double(piece.halfWidth), smallestHalfWidth) <= best + 2.0 * view->marginMetres)
				view->active.push_back(&piece);
		}
		view->activeCentre = p;
		view->cosMargin = std::cos(view->marginMetres / m_radius);
	}
	bool found = false;
	for (const Piece *pointer : view->active) {
		const Piece &piece = *pointer;
		const vector3d ab = piece.b - piece.a;
		const double t = std::clamp((p - piece.a).Dot(ab) / std::max(ab.Dot(ab), 1e-18), 0.0, 1.0);
		const double centre = (p - (piece.a + ab * t)).Length() * m_radius;
		// (each piece keeps its own width: taken from the nearest line, rivers would come out beaded)
		const double halfWidth = std::max(double(piece.halfWidth), smallestHalfWidth);
		const double edge = centre - halfWidth;
		if (edge < out.distance) {
			out.distance = edge;
			out.trueDistance = centre - piece.halfWidth;
			out.centre = centre;
			out.halfWidth = halfWidth;
			out.valley = std::max(MAIN_VALLEY, 8.0 * double(piece.halfWidth));
			out.level = piece.levelA + (piece.levelB - piece.levelA) * t;
			found = true;
		}
	}
	return found;
}
