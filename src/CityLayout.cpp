// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "CityLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace {
	constexpr double kPi = 3.14159265358979323846;
	constexpr float BLOCKED = std::numeric_limits<float>::infinity();

	// ground more than this out of level over a cell (m) is left unbuilt
	constexpr double STEEP_CELL = 10.0;
	// streets don't climb more than this between neighbouring cells (m)
	constexpr double STREET_CLIMB = 5.0;
	// slope below which ground counts as flat
	constexpr double FLAT_SLOPE = 0.08;
	// cells that need this much of the city's density to be built up
	constexpr double DEVELOPED = 0.36;
	// built-up patches smaller than this (cells) are dropped
	constexpr size_t MIN_PATCH = 12;

	// roads: carriageway width, pavement on each side (m)
	constexpr float ARTERIAL_WIDTH = 18.0f, ARTERIAL_WALK = 3.0f;
	constexpr float CENTRE_WIDTH = 12.0f, CENTRE_WALK = 3.0f;
	constexpr float LOCAL_WIDTH = 9.0f, LOCAL_WALK = 2.0f;
	// beyond the pavement, what a building keeps from a street (m)
	constexpr double SETBACK_MARGIN = 1.5;
	// main roads' paths are straightened to this (m), then rounded
	constexpr double ROAD_SIMPLIFY = 30.0;

	template <typename T>
	T ClampTo(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

	double Smooth(double a, double b, double x)
	{
		const double t = ClampTo((x - a) / (b - a), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	double LayoutHash(int64_t x, int64_t y, uint32_t seed)
	{
		uint64_t h = uint64_t(x) * 374761393ull + uint64_t(y) * 668265263ull + uint64_t(seed) * 2246822519ull;
		h = (h ^ (h >> 13)) * 1274126177ull;
		h ^= h >> 16;
		return double(h & 0xffffff) / double(0xffffff);
	}

	// value noise, -1..1
	double LayoutNoise(double x, double y, uint32_t seed)
	{
		const double fx = std::floor(x), fy = std::floor(y);
		const int64_t ix = int64_t(fx), iy = int64_t(fy);
		const double tx = x - fx, ty = y - fy;
		const double ux = tx * tx * (3.0 - 2.0 * tx), uy = ty * ty * (3.0 - 2.0 * ty);
		const double a = LayoutHash(ix, iy, seed), b = LayoutHash(ix + 1, iy, seed);
		const double c = LayoutHash(ix, iy + 1, seed), d = LayoutHash(ix + 1, iy + 1, seed);
		return (a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy) * 2.0 - 1.0;
	}

	double LayoutFbm(double x, double y, int octaves, uint32_t seed)
	{
		double sum = 0.0, amp = 0.5, norm = 0.0;
		for (int i = 0; i < octaves; i++) {
			sum += amp * LayoutNoise(x, y, seed + 101 * i);
			norm += amp;
			x = x * 2.03 + 17.1;
			y = y * 2.03 + 3.7;
			amp *= 0.5;
		}
		return sum / norm;
	}

	double DistToSegment(double px, double pz, double x0, double z0, double x1, double z1)
	{
		const double dx = x1 - x0, dz = z1 - z0;
		const double l2 = dx * dx + dz * dz;
		const double t = l2 > 1e-9 ? ClampTo(((px - x0) * dx + (pz - z0) * dz) / l2, 0.0, 1.0) : 0.0;
		return std::hypot(px - x0 - t * dx, pz - z0 - t * dz);
	}

	struct Pt {
		double x, z;
	};

	// Ramer-Douglas-Peucker
	void Simplify(const std::vector<Pt> &in, size_t a, size_t b, double eps, std::vector<Pt> &out)
	{
		double best = -1.0;
		size_t k = a;
		for (size_t i = a + 1; i < b; i++) {
			const double d = DistToSegment(in[i].x, in[i].z, in[a].x, in[a].z, in[b].x, in[b].z);
			if (d > best) {
				best = d;
				k = i;
			}
		}
		if (best > eps) {
			Simplify(in, a, k, eps, out);
			Simplify(in, k, b, eps, out);
		} else {
			out.push_back(in[b]);
		}
	}

	std::vector<Pt> Simplified(const std::vector<Pt> &in, double eps)
	{
		if (in.size() < 3)
			return in;
		std::vector<Pt> out = { in.front() };
		Simplify(in, 0, in.size() - 1, eps, out);
		return out;
	}

	// corner cutting, keeping the ends
	std::vector<Pt> Rounded(std::vector<Pt> pts, int iterations)
	{
		for (int it = 0; it < iterations && pts.size() >= 3; it++) {
			std::vector<Pt> q = { pts.front() };
			for (size_t i = 0; i + 1 < pts.size(); i++) {
				const Pt &a = pts[i], &b = pts[i + 1];
				q.push_back({ a.x * 0.75 + b.x * 0.25, a.z * 0.75 + b.z * 0.25 });
				q.push_back({ a.x * 0.25 + b.x * 0.75, a.z * 0.25 + b.z * 0.75 });
			}
			q.push_back(pts.back());
			pts.swap(q);
		}
		return pts;
	}
} // namespace

struct CityLayout::Analysis {
	double seaShare = 0.0;				 // of the ground round the starport
	double seaDist = 1e9;				 // to the nearest sea, m
	double seaDirX = 0.0, seaDirZ = 0.0; // towards it
	double flatShare = 0.0;				 // of the land
	double landX = 0.0, landZ = 0.0;	 // middle of the flat land
	double slope = 0.0;					 // of a plane through the land, m/m
	double slopeFit = 0.0;				 // share of the land's relief it explains
	double slopeYaw = 0.0;				 // uphill
	double valleyDepth = 0.0;			 // m
	double valleyYaw = 0.0;
	bool crater = false;
	double craterX = 0.0, craterZ = 0.0, craterR = 0.0;
	double flatYaw = 0.0; // towards the flat land
};

const char *CityLayout::ShapeName(Shape shape)
{
	switch (shape) {
	case Shape::ROUND: return "sprawl";
	case Shape::COASTAL: return "coastal strip";
	case Shape::ISLANDS: return "islands";
	case Shape::VALLEY: return "valley line";
	case Shape::HILLSIDE: return "hillside";
	case Shape::GRID: return "grid capital";
	case Shape::HIGHWAY: return "highway string";
	case Shape::DOME: return "inside the dome";
	case Shape::DOME_CLUSTER: return "dome cluster";
	case Shape::CRATER: return "crater city";
	}
	return "?";
}

CityLayout::CityLayout(const Facts &facts, const Ground &ground) :
	m_facts(facts),
	m_rng(facts.seed ^ 0x5c17a7e5u)
{
	const double R = facts.radius;
	m_ext = ClampTo(int(std::ceil((R * 1.35 + 1500.0) / CELL)), 40, 400);
	m_n = 2 * m_ext + 1;
	m_x0 = -(m_ext + 0.5) * CELL;
	const size_t cells = size_t(m_n) * m_n;
	m_port.assign(cells, 0);
	m_developed.assign(cells, 0);
	m_roadCell.assign(cells, 0);
	m_cellDistrict.assign(cells, District::NONE);
	m_cellSeed.assign(cells, -1);
	m_lots.assign(cells, LOT_NONE);
	m_density.assign(cells, 0.0f);
	m_mainDist.assign(cells, 1e9f);
	m_mainYaw.assign(cells, 0.0f);
	m_vEdge.assign(size_t(m_n) * (m_n + 1), 0);
	m_hEdge.assign(size_t(m_n + 1) * m_n, 0);

	// the starport's cells, as CityOnPlanet reserves them
	const int pi0 = m_ext - int(facts.portCells[0] / 2), pj0 = m_ext - int(facts.portCells[1] / 2);
	for (int j = pj0; j < pj0 + int(facts.portCells[1]); j++)
		for (int i = pi0; i < pi0 + int(facts.portCells[0]); i++)
			if (InGrid(i, j))
				m_port[Idx(i, j)] = 1;

	SampleGround(ground);
	Analysis a;
	Analyse(a);
	ChooseShape(a);
	Outline(a);
	PlaceDistricts();
	LayMainRoads(a);
	LayStreets();
	BuildLookup();
}

// ------------------------------------------------------------------ the ground
void CityLayout::SampleGround(const Ground &ground)
{
	// coarser for bigger cities, so the sampling stays cheap
	m_step = CELL * ClampTo(std::round(m_facts.radius / 2500.0), 2.0, 6.0);
	m_m = int(std::ceil(m_n * CELL / m_step)) + 2;
	std::vector<double> xz(size_t(m_m) * m_m * 2);
	for (int v = 0; v < m_m; v++) {
		for (int u = 0; u < m_m; u++) {
			const size_t k = size_t(v) * m_m + u;
			xz[2 * k] = m_x0 + u * m_step;
			xz[2 * k + 1] = m_x0 + v * m_step;
		}
	}
	ground(xz, m_coarse, m_coarseSea, m_step);
	m_coarse.resize(size_t(m_m) * m_m, 0.0);
	m_coarseSea.resize(size_t(m_m) * m_m, 0);

	const size_t cells = size_t(m_n) * m_n;
	m_height.assign(cells, 0.0);
	m_slope.assign(cells, 0.0f);
	m_sea.assign(cells, 0);
	for (int j = 0; j < m_n; j++) {
		for (int i = 0; i < m_n; i++) {
			const double x = CellX(i), z = CellZ(j);
			m_height[Idx(i, j)] = GroundAt(x, z);
			double lo = 1e30, hi = -1e30;
			for (int c = 0; c < 4; c++) {
				const double h = GroundAt(x + ((c & 1) ? 0.5 : -0.5) * CELL, z + ((c & 2) ? 0.5 : -0.5) * CELL);
				lo = std::min(lo, h);
				hi = std::max(hi, h);
			}
			m_slope[Idx(i, j)] = float(hi - lo);
			const int u = ClampTo(int(std::lround((x - m_x0) / m_step)), 0, m_m - 1);
			const int v = ClampTo(int(std::lround((z - m_x0) / m_step)), 0, m_m - 1);
			m_sea[Idx(i, j)] = m_coarseSea[size_t(v) * m_m + u];
		}
	}
}

double CityLayout::GroundAt(double x, double z) const
{
	const double u = ClampTo((x - m_x0) / m_step, 0.0, m_m - 1.001);
	const double v = ClampTo((z - m_x0) / m_step, 0.0, m_m - 1.001);
	const int iu = int(u), iv = int(v);
	const double fu = u - iu, fv = v - iv;
	const double *h = &m_coarse[size_t(iv) * m_m + iu];
	return (h[0] * (1.0 - fu) + h[1] * fu) * (1.0 - fv) + (h[m_m] * (1.0 - fu) + h[m_m + 1] * fu) * fv;
}

double CityLayout::DistanceToCentre(double x, double z) const
{
	return std::hypot(x - m_cx, z - m_cz);
}

// ------------------------------------------------------------------ the shape
void CityLayout::Analyse(Analysis &a) const
{
	const double R = m_facts.radius;
	const double reach = ClampTo(R, 2000.0, std::min(5000.0, (m_ext - 2) * CELL));
	struct Sample {
		double x, z, h, slope;
		bool sea;
	};
	std::vector<Sample> s;
	for (int v = 1; v + 1 < m_m; v++) {
		for (int u = 1; u + 1 < m_m; u++) {
			const double x = m_x0 + u * m_step, z = m_x0 + v * m_step;
			if (x * x + z * z > reach * reach)
				continue;
			const size_t k = size_t(v) * m_m + u;
			const double gx = (m_coarse[k + 1] - m_coarse[k - 1]) / (2.0 * m_step);
			const double gz = (m_coarse[k + m_m] - m_coarse[k - m_m]) / (2.0 * m_step);
			s.push_back({ x, z, m_coarse[k], std::hypot(gx, gz), m_coarseSea[k] != 0 });
		}
	}
	if (s.empty())
		return;

	// the sea
	size_t sea = 0;
	double sx = 0.0, sz = 0.0;
	for (const Sample &p : s) {
		if (!p.sea)
			continue;
		sea++;
		sx += p.x;
		sz += p.z;
		a.seaDist = std::min(a.seaDist, std::hypot(p.x, p.z));
	}
	a.seaShare = double(sea) / s.size();
	if (sea) {
		const double l = std::hypot(sx, sz);
		if (l > 1e-6) {
			a.seaDirX = sx / l;
			a.seaDirZ = sz / l;
		}
	}

	// the land: how flat, where the flat land is, a plane through it
	size_t land = 0, flat = 0;
	double fx = 0.0, fz = 0.0;
	double n = 0, mx = 0, mz = 0, mh = 0;
	for (const Sample &p : s) {
		if (p.sea)
			continue;
		land++;
		n += 1.0;
		mx += p.x;
		mz += p.z;
		mh += p.h;
		if (p.slope < FLAT_SLOPE) {
			flat++;
			fx += p.x;
			fz += p.z;
		}
	}
	if (!land)
		return;
	a.flatShare = double(flat) / land;
	if (flat) {
		a.landX = fx / flat;
		a.landZ = fz / flat;
	}
	a.flatYaw = std::atan2(a.landZ, a.landX);
	mx /= n;
	mz /= n;
	mh /= n;
	double sxx = 0, sxz = 0, szz = 0, sxh = 0, szh = 0, shh = 0;
	for (const Sample &p : s) {
		if (p.sea)
			continue;
		const double dx = p.x - mx, dz = p.z - mz, dh = p.h - mh;
		sxx += dx * dx;
		sxz += dx * dz;
		szz += dz * dz;
		sxh += dx * dh;
		szh += dz * dh;
		shh += dh * dh;
	}
	const double det = sxx * szz - sxz * sxz;
	if (det > 1e-6 && shh > 1e-6) {
		const double gx = (sxh * szz - szh * sxz) / det;
		const double gz = (szh * sxx - sxh * sxz) / det;
		a.slope = std::hypot(gx, gz);
		a.slopeYaw = std::atan2(gz, gx);
		a.slopeFit = ClampTo((gx * sxh + gz * szh) / shh, 0.0, 1.0);
	}

	// a valley: low ground along a line through the starport, higher on both sides
	for (int k = 0; k < 12; k++) {
		const double yaw = kPi * k / 12.0;
		const double ux = std::cos(yaw), uz = std::sin(yaw);
		double band = 0, nb = 0, side = 0, ns = 0;
		for (const Sample &p : s) {
			if (p.sea)
				continue;
			const double across = std::abs(-p.x * uz + p.z * ux);
			if (across < 300.0) {
				band += p.h;
				nb++;
			} else if (across > 1200.0 && across < 2600.0) {
				side += p.h;
				ns++;
			}
		}
		if (nb > 4 && ns > 8) {
			const double depth = side / ns - band / nb;
			if (depth > a.valleyDepth) {
				a.valleyDepth = depth;
				a.valleyYaw = yaw;
			}
		}
	}

	// a crater nearby: a low floor ringed by a raised rim
	const double half = (m_ext - 2) * CELL;
	double best = 0.0;
	for (double cz = -3000.0; cz <= 3000.0; cz += 500.0) {
		for (double cx = -3000.0; cx <= 3000.0; cx += 500.0) {
			for (double r = 1200.0; r <= 4000.0; r += 400.0) {
				if (std::max(std::abs(cx), std::abs(cz)) + 1.4 * r > half)
					continue;
				double floorH = 0.0, rim = 0.0, outside = 0.0;
				for (int t = 0; t < 24; t++) {
					const double ang = 2.0 * kPi * t / 24.0, c = std::cos(ang), sn = std::sin(ang);
					floorH += GroundAt(cx + c * r * 0.3, cz + sn * r * 0.3) + GroundAt(cx + c * r * 0.5, cz + sn * r * 0.5);
					rim += GroundAt(cx + c * r * 0.95, cz + sn * r * 0.95);
					outside += GroundAt(cx + c * r * 1.4, cz + sn * r * 1.4);
				}
				floorH /= 48.0;
				rim /= 24.0;
				outside /= 24.0;
				const double depth = (rim - floorH) / r;
				if (depth > 0.07 && rim - outside > 0.01 * r && depth > best) {
					best = depth;
					a.crater = true;
					a.craterX = cx;
					a.craterZ = cz;
					a.craterR = r;
				}
			}
		}
	}
}

void CityLayout::ChooseShape(const Analysis &a)
{
	const Facts &f = m_facts;
	struct Option {
		Shape shape;
		double score;
	};
	std::vector<Option> options;
	if (f.needsDome) {
		options.push_back({ Shape::DOME, 1.5 + (f.population > 0.001 ? 1.0 : 0.0) + a.flatShare });
		options.push_back({ Shape::DOME_CLUSTER, 1.0 + (f.population < 0.001 ? 1.5 : 0.0) + (f.gravity < 0.3 ? 1.0 : 0.0) + (1.0 - a.flatShare) });
		if (a.crater && f.gravity < 0.5)
			options.push_back({ Shape::CRATER, 3.0 });
	} else {
		options.push_back({ Shape::ROUND, 1.0 });
		if (a.seaShare > 0.55 && f.water > 0.4)
			options.push_back({ Shape::ISLANDS, 2.0 + 2.0 * a.seaShare });
		if (a.seaShare > 0.03 && a.seaShare < 0.6 && a.seaDist < 1800.0)
			options.push_back({ Shape::COASTAL, 2.5 + a.seaShare });
		if (f.population > 2.0)
			options.push_back({ Shape::GRID, 1.2 + f.humanProx + 0.5 * f.industrial + a.flatShare });
		if (f.population < 0.05 && (f.agricultural > 0.4 || f.humanProx < 0.4))
			options.push_back({ Shape::HIGHWAY, 1.2 + a.flatShare });
		if (a.valleyDepth > 35.0)
			options.push_back({ Shape::VALLEY, 1.5 + std::min(a.valleyDepth / 100.0, 1.5) });
		if (a.slope > 0.04 && a.slope < 0.25 && a.slopeFit > 0.5 && f.gravity < 1.3)
			options.push_back({ Shape::HILLSIDE, 1.5 + a.slopeFit });
	}
	double best = -1.0;
	for (const Option &o : options) {
		const double score = o.score + 0.6 * m_rng.Double();
		if (score > best) {
			best = score;
			m_shape = o.shape;
		}
	}
}

void CityLayout::Outline(const Analysis &a)
{
	const double R = m_facts.radius;
	const uint32_t seed = m_facts.seed;
	const double portHalf = 0.5 * CELL * std::hypot(double(m_facts.portCells[0]), double(m_facts.portCells[1]));

	// the heart: drawn towards the flat land round the starport
	const double landLen = std::hypot(a.landX, a.landZ);
	const double pull = std::min(landLen, 0.35 * R);
	const double dirX = landLen > 1.0 ? a.landX / landLen : std::cos(2.0 * kPi * m_rng.Double());
	const double dirZ = landLen > 1.0 ? a.landZ / landLen : std::sin(2.0 * kPi * m_rng.Double());
	m_cx = dirX * pull;
	m_cz = dirZ * pull;

	auto flatAt = [&](double x, double z) {
		const int i = ClampTo(int((x - m_x0) / CELL), 0, m_n - 1), j = ClampTo(int((z - m_x0) / CELL), 0, m_n - 1);
		return !m_sea[Idx(i, j)] && m_slope[Idx(i, j)] < STEEP_CELL;
	};

	switch (m_shape) {
	case Shape::COASTAL:
		m_axis = std::atan2(a.seaDirZ, a.seaDirX) + kPi / 2.0;
		m_cx = -a.seaDirX * std::min(0.2 * R, 700.0);
		m_cz = -a.seaDirZ * std::min(0.2 * R, 700.0);
		break;
	case Shape::VALLEY:
		m_axis = a.valleyYaw;
		m_cx = m_cz = 0.0;
		break;
	case Shape::HIGHWAY: {
		m_axis = a.flatYaw;
		m_cx = m_cz = 0.0;
		// towns along it, the starport's the biggest
		const double ux = std::cos(m_axis), uz = std::sin(m_axis);
		const int towns = ClampTo(int(R / 900.0) + 2, 3, 6);
		for (int t = 0; t < towns; t++) {
			const double along = (t - (towns - 1) * 0.5) * (2.2 * R / std::max(towns - 1, 1)) + m_rng.Double(-200.0, 200.0);
			const double off = m_rng.Double(-250.0, 250.0);
			const double x = ux * along - uz * off, z = uz * along + ux * off;
			const int i = ClampTo(int((x - m_x0) / CELL), 1, m_n - 2), j = ClampTo(int((z - m_x0) / CELL), 1, m_n - 2);
			if (!m_sea[Idx(i, j)])
				m_towns.push_back(int(Idx(i, j)));
		}
		break;
	}
	case Shape::DOME: {
		double r = ClampTo(0.8 * R, 600.0, 6000.0);
		r = std::max(r, (portHalf + 60.0) / 0.5);
		m_cx = dirX * 0.5 * r;
		m_cz = dirZ * 0.5 * r;
		m_domes.push_back({ m_cx, m_cz, r });
		break;
	}
	case Shape::DOME_CLUSTER: {
		// the first over the starport, the others round it
		const int want = ClampTo(int(R / 1500.0) + 3, 3, 7);
		const double r0 = ClampTo(0.35 * R, std::max(700.0, 2.0 * (portHalf + 80.0)), 2000.0);
		const double d0 = std::min(0.45 * r0, r0 - portHalf - 80.0);
		m_domes.push_back({ dirX * d0, dirZ * d0, r0 });
		for (int tries = 0; tries < 80 && int(m_domes.size()) < want; tries++) {
			const Dome &parent = m_domes[m_rng.Int32(int(m_domes.size()))];
			const double r = r0 * m_rng.Double(0.55, 0.9);
			const double ang = 2.0 * kPi * m_rng.Double();
			const double d = parent.radius + r + m_rng.Double(150.0, 450.0);
			const double x = parent.x + std::cos(ang) * d, z = parent.z + std::sin(ang) * d;
			bool ok = flatAt(x, z) && std::hypot(x, z) + r < (m_ext - 4) * CELL;
			for (const Dome &o : m_domes)
				ok = ok && std::hypot(x - o.x, z - o.z) > o.radius + r + 100.0;
			if (ok)
				m_domes.push_back({ x, z, r });
		}
		m_cx = m_domes[0].x;
		m_cz = m_domes[0].z;
		break;
	}
	case Shape::CRATER: {
		double r = 0.55 * a.craterR;
		const double d = std::hypot(a.craterX, a.craterZ);
		// the starport inside the dome or clear of it
		if (std::abs(d - r) < portHalf + 60.0)
			r = d < r ? d + portHalf + 60.0 : std::max(400.0, d - portHalf - 60.0);
		m_cx = a.craterX;
		m_cz = a.craterZ;
		m_domes.push_back({ m_cx, m_cz, r });
		break;
	}
	default:
		break;
	}

	// the starport is always under a dome: the nearest grows to take it in,
	// or it gets one of its own
	if (!m_domes.empty()) {
		auto need = [&](const Dome &d) { return std::hypot(d.x, d.z) + portHalf + 60.0; };
		bool held = false;
		size_t nearest = 0;
		for (size_t d = 0; d < m_domes.size(); d++) {
			held = held || need(m_domes[d]) <= m_domes[d].radius;
			if (need(m_domes[d]) - m_domes[d].radius < need(m_domes[nearest]) - m_domes[nearest].radius)
				nearest = d;
		}
		if (!held) {
			Dome &near = m_domes[nearest];
			const Dome own = { 0.0, 0.0, portHalf + 250.0 };
			bool clear = true;
			for (const Dome &d : m_domes)
				clear = clear && std::hypot(d.x, d.z) > d.radius + own.radius + 60.0;
			if (need(near) > 1.5 * near.radius && clear)
				m_domes.push_back(own);
			else
				near.radius = need(near);
		}
	}

	// cities in the open air: a heart and a few lobes round it, stretched
	// along one way, with ragged edges; never a circle
	struct Lobe {
		double x, z, r;
	};
	std::vector<Lobe> lobes;
	double stretch = 1.0;
	const bool lobed = m_shape == Shape::ROUND || m_shape == Shape::HILLSIDE || m_shape == Shape::ISLANDS || m_shape == Shape::GRID;
	if (lobed) {
		m_axis = m_shape == Shape::HILLSIDE ? a.slopeYaw + kPi / 2.0 : 2.0 * kPi * m_rng.Double();
		stretch = m_rng.Double(1.3, 1.7);
		lobes.push_back({ m_cx, m_cz, 0.6 * R });
		const double room = (m_ext - 3) * CELL;
		const int extra = 2 + int(m_rng.Int32(3));
		for (int k = 0; k < extra; k++) {
			for (int tries = 0; tries < 8; tries++) {
				const double ang = 2.0 * kPi * m_rng.Double(), d = R * m_rng.Double(0.35, 0.75);
				const double r = R * m_rng.Double(0.25, 0.4);
				const double x = m_cx + std::cos(ang) * d, z = m_cz + std::sin(ang) * d;
				if (std::hypot(x, z) + r * stretch < room && flatAt(x, z)) {
					lobes.push_back({ x, z, r });
					break;
				}
			}
		}
	}

	// coastal: how far each cell is from the sea (chamfer distance, m)
	std::vector<float> coast;
	if (m_shape == Shape::COASTAL) {
		coast.assign(size_t(m_n) * m_n, 1e9f);
		for (size_t k = 0; k < coast.size(); k++)
			if (m_sea[k])
				coast[k] = 0.0f;
		const float d1 = float(CELL), d2 = float(CELL * 1.41421356);
		for (int pass = 0; pass < 2; pass++) {
			const int step = pass ? -1 : 1;
			for (int j = pass ? m_n - 1 : 0; j >= 0 && j < m_n; j += step) {
				for (int i = pass ? m_n - 1 : 0; i >= 0 && i < m_n; i += step) {
					float &c = coast[Idx(i, j)];
					const int ni[4] = { i - step, i - step, i, i + step };
					const int nj[4] = { j, j - step, j - step, j - step };
					for (int k = 0; k < 4; k++)
						if (InGrid(ni[k], nj[k]))
							c = std::min(c, coast[Idx(ni[k], nj[k])] + ((ni[k] != i && nj[k] != j) ? d2 : d1));
				}
			}
		}
	}
	// valley: its floor
	double valleyFloor = 0.0;
	if (m_shape == Shape::VALLEY) {
		std::vector<double> hs;
		for (int j = 0; j < m_n; j++)
			for (int i = 0; i < m_n; i++)
				if (!m_sea[Idx(i, j)] && std::hypot(CellX(i), CellZ(j)) < R)
					hs.push_back(m_height[Idx(i, j)]);
		if (!hs.empty()) {
			std::nth_element(hs.begin(), hs.begin() + hs.size() / 20, hs.end());
			valleyFloor = hs[hs.size() / 20];
		}
	}

	const double ux = std::cos(m_axis), uz = std::sin(m_axis);
	double threshold = DEVELOPED;
	for (int j = 0; j < m_n; j++) {
		for (int i = 0; i < m_n; i++) {
			const size_t k = Idx(i, j);
			const double x = CellX(i), z = CellZ(j);
			const double n1 = LayoutFbm(x / 1500.0, z / 1500.0, 4, seed);
			const double n2 = LayoutFbm(x / 500.0, z / 500.0, 3, seed + 7);
			const double along = (x - m_cx) * ux + (z - m_cz) * uz;
			const double across = -(x - m_cx) * uz + (z - m_cz) * ux;
			// the lobes, over coordinates warped for ragged edges
			double lobe = 0.0;
			if (lobed) {
				const double wx = x + 350.0 * LayoutFbm(x / 2000.0 + 3.1, z / 2000.0, 3, seed + 11);
				const double wz = z + 350.0 * LayoutFbm(x / 2000.0, z / 2000.0 + 7.7, 3, seed + 13);
				for (const Lobe &l : lobes) {
					const double dx = wx - l.x, dz = wz - l.z;
					const double la = (dx * ux + dz * uz) / stretch, lc = -dx * uz + dz * ux;
					lobe = std::max(lobe, std::pow(ClampTo(1.0 - std::hypot(la, lc) / l.r, 0.0, 1.0), 0.7));
				}
			}
			double d = 0.0;
			switch (m_shape) {
			case Shape::ROUND:
			case Shape::HILLSIDE:
			case Shape::ISLANDS:
				d = lobe * (0.78 + 0.6 * n1) + 0.12 * n2;
				break;
			case Shape::GRID:
				d = std::min(1.0, 1.6 * lobe) * (0.95 + 0.15 * n1) + 0.05 * n2;
				break;
			case Shape::COASTAL:
				d = std::exp(-std::pow(std::max(coast[k] - 100.0, 0.0) / (0.45 * R), 2.0)) *
						std::exp(-std::pow(along / R, 4.0)) * (0.85 + 0.5 * n1) +
					0.1 * n2;
				break;
			case Shape::VALLEY:
				d = std::exp(-std::pow(std::max(m_height[k] - valleyFloor - 6.0, 0.0) / 30.0, 2.0)) *
						std::exp(-std::pow(along / (1.2 * R), 4.0)) * (0.85 + 0.45 * n1) +
					0.08 * n2;
				break;
			case Shape::HIGHWAY: {
				d = std::abs(along) < 1.2 * R ? 0.55 * std::exp(-std::pow(across / 110.0, 2.0)) : 0.0;
				for (int t : m_towns) {
					const double tr = 300.0 + 0.15 * R;
					const double dt = std::hypot(x - CellX(t % m_n), z - CellZ(t / m_n));
					d += std::pow(ClampTo(1.0 - dt / tr, 0.0, 1.0), 0.6) * (0.9 + 0.4 * n1);
				}
				d += 0.08 * n2;
				break;
			}
			case Shape::DOME:
			case Shape::DOME_CLUSTER:
			case Shape::CRATER:
				for (const Dome &dome : m_domes)
					if (std::hypot(x - dome.x, z - dome.z) < dome.radius - 70.0)
						d = 1.0 + 0.1 * n1;
				threshold = 0.5;
				break;
			}
			// drawn to the starport
			if (m_shape != Shape::DOME_CLUSTER && m_shape != Shape::CRATER)
				d += 0.3 * std::exp(-std::pow(std::hypot(x, z) / 600.0, 2.0));
			d *= 1.0 - Smooth(6.0, 16.0, m_slope[k]);
			if (m_sea[k] || m_port[k])
				d = 0.0;
			m_density[k] = float(d);
			m_developed[k] = d > threshold;
		}
	}

	// drop the small patches
	std::vector<int> stack;
	std::vector<uint8_t> seen(size_t(m_n) * m_n, 0);
	for (int start = 0; start < m_n * m_n; start++) {
		if (!m_developed[start] || seen[start])
			continue;
		std::vector<int> patch;
		stack.push_back(start);
		seen[start] = 1;
		while (!stack.empty()) {
			const int c = stack.back();
			stack.pop_back();
			patch.push_back(c);
			const int ci = c % m_n, cj = c / m_n;
			const int ni[4] = { ci - 1, ci + 1, ci, ci };
			const int nj[4] = { cj, cj, cj - 1, cj + 1 };
			for (int k = 0; k < 4; k++) {
				if (!InGrid(ni[k], nj[k]))
					continue;
				const int nc = int(Idx(ni[k], nj[k]));
				if (m_developed[nc] && !seen[nc]) {
					seen[nc] = 1;
					stack.push_back(nc);
				}
			}
		}
		if (patch.size() < MIN_PATCH)
			for (int c : patch)
				m_developed[c] = 0;
	}
}

// ------------------------------------------------------------------ districts
void CityLayout::PlaceDistricts()
{
	const double R = m_facts.radius;
	auto cellOf = [&](double x, double z) {
		return Idx(ClampTo(int((x - m_x0) / CELL), 0, m_n - 1), ClampTo(int((z - m_x0) / CELL), 0, m_n - 1));
	};
	const bool domed = !m_domes.empty();

	if (m_shape == Shape::DOME_CLUSTER) {
		for (size_t d = 0; d < m_domes.size(); d++) {
			const size_t c = cellOf(m_domes[d].x, m_domes[d].z);
			m_seeds.push_back({ int(c % m_n), int(c / m_n), d ? District::RESIDENTIAL : District::DOWNTOWN, 0, 0, 0 });
		}
	} else if (m_shape == Shape::HIGHWAY) {
		for (int t : m_towns)
			m_seeds.push_back({ t % m_n, t / m_n, District::DOWNTOWN, 0, 0, 0 });
	} else {
		double best = -1.0;
		size_t at = cellOf(m_cx, m_cz);
		for (size_t k = 0; k < m_developed.size(); k++) {
			if (!m_developed[k])
				continue;
			const double d = DistanceToCentre(CellX(int(k % m_n)), CellZ(int(k / m_n)));
			const double score = m_density[k] * std::exp(-std::pow(d / 1300.0, 2.0));
			if (score > best) {
				best = score;
				at = k;
			}
		}
		m_seeds.push_back({ int(at % m_n), int(at / m_n), District::DOWNTOWN, 0, 0, 0 });
	}
	if (m_seeds.empty())
		m_seeds.push_back({ m_ext, m_ext, District::DOWNTOWN, 0, 0, 0 });

	// the starport's gate: the cell beside it towards the first district
	{
		const double tx = CellX(m_seeds[0].i), tz = CellZ(m_seeds[0].j);
		double vx = tx, vz = tz;
		const double l = std::hypot(vx, vz);
		if (l > 1.0) {
			vx /= l;
			vz /= l;
		} else {
			vx = 1.0;
			vz = 0.0;
		}
		const double reach = (0.5 * CELL * std::max(m_facts.portCells[0], m_facts.portCells[1]) + 0.6 * CELL) /
			std::max(std::abs(vx), std::abs(vz));
		m_gate = int(cellOf(vx * reach, vz * reach));
	}
	// warehouses at the gate, unless the starport stands out on its own
	const bool portInCity = !domed || DomeAt(0.0, 0.0) >= 0;
	if (m_shape != Shape::DOME_CLUSTER && portInCity && !m_sea[m_gate])
		m_seeds.push_back({ m_gate % m_n, m_gate / m_n, District::LOGISTICS, 0, 0, 0 });

	// the rest spread over the city
	size_t area = 0;
	for (uint8_t d : m_developed)
		area += d;
	const double km2 = area * CELL * CELL / 1e6;
	const size_t want = m_seeds.size() + size_t(ClampTo(km2 / 1.1, 2.0, 38.0));
	const double spacing = std::max(550.0, R / 8.0);
	std::vector<double> dmin(m_developed.size(), 1e30);
	auto addDistances = [&](const Seed &s) {
		const double sx = CellX(s.i), sz = CellZ(s.j);
		for (size_t k = 0; k < dmin.size(); k++)
			if (m_developed[k])
				dmin[k] = std::min(dmin[k], std::hypot(CellX(int(k % m_n)) - sx, CellZ(int(k / m_n)) - sz));
	};
	for (const Seed &s : m_seeds)
		addDistances(s);
	if (m_shape != Shape::DOME_CLUSTER) {
		while (m_seeds.size() < want) {
			double best = -1.0;
			size_t at = 0;
			for (size_t k = 0; k < dmin.size(); k++) {
				if (!m_developed[k])
					continue;
				const double score = dmin[k] * (0.4 + m_density[k]);
				if (score > best) {
					best = score;
					at = k;
				}
			}
			if (best < 0.0 || dmin[at] < spacing)
				break;
			m_seeds.push_back({ int(at % m_n), int(at / m_n), District::RESIDENTIAL, 0, 0, 0 });
			addDistances(m_seeds.back());
		}
	}

	// industry: the districts nearest the starport, more on industrial worlds
	if (!domed) {
		std::vector<size_t> res;
		for (size_t s = 0; s < m_seeds.size(); s++)
			if (m_seeds[s].kind == District::RESIDENTIAL)
				res.push_back(s);
		std::sort(res.begin(), res.end(), [&](size_t a, size_t b) {
			return std::hypot(CellX(m_seeds[a].i), CellZ(m_seeds[a].j)) < std::hypot(CellX(m_seeds[b].i), CellZ(m_seeds[b].j));
		});
		const size_t industry = res.empty() ? 0 : std::max<size_t>(1, size_t(std::lround(res.size() * (0.15 + 0.25 * m_facts.industrial))));
		for (size_t k = 0; k < industry && k < res.size(); k++)
			m_seeds[res[k]].kind = District::INDUSTRY;
	}

	// block sizes and where each district's street grid starts
	for (Seed &s : m_seeds) {
		if (m_shape == Shape::GRID) {
			s.block = 3;
			continue;
		}
		switch (s.kind) {
		case District::DOWNTOWN: s.block = 2; break;
		case District::INDUSTRY:
		case District::LOGISTICS: s.block = 4; break;
		default: s.block = m_rng.Double() < 0.6 ? 3 : 4; break;
		}
		s.offsetX = m_rng.Int32(s.block);
		s.offsetZ = m_rng.Int32(s.block);
	}

	// each cell to its nearest seed, borders made ragged (by two noise
	// fields, mixed differently for each seed)
	for (int j = 0; j < m_n; j++) {
		for (int i = 0; i < m_n; i++) {
			const size_t k = Idx(i, j);
			if (!m_developed[k])
				continue;
			const double x = CellX(i), z = CellZ(j);
			const double na = LayoutFbm(x / 900.0, z / 900.0, 3, m_facts.seed + 31);
			const double nb = LayoutFbm(x / 900.0 + 5.2, z / 900.0 + 1.3, 3, m_facts.seed + 57);
			double best = 1e30;
			int pick = 0;
			for (size_t s = 0; s < m_seeds.size(); s++) {
				double d;
				if (m_shape == Shape::DOME_CLUSTER)
					d = std::hypot(x - m_domes[s].x, z - m_domes[s].z) - m_domes[s].radius;
				else
					d = std::hypot(x - CellX(m_seeds[s].i), z - CellZ(m_seeds[s].j)) +
						220.0 * (na * std::cos(1.7 * s) + nb * std::sin(1.7 * s));
				if (d < best) {
					best = d;
					pick = int(s);
				}
			}
			m_cellSeed[k] = int16_t(pick);
			m_cellDistrict[k] = m_seeds[pick].kind;
		}
	}
}

// ------------------------------------------------------------------ roads
void CityLayout::RoadDims(RoadType type, float &width, float &walk) const
{
	switch (type) {
	case ROAD_ARTERIAL:
		width = ARTERIAL_WIDTH;
		walk = ARTERIAL_WALK;
		break;
	case ROAD_CENTRE:
		width = CENTRE_WIDTH;
		walk = CENTRE_WALK;
		break;
	default:
		width = LOCAL_WIDTH;
		walk = LOCAL_WALK;
		break;
	}
}

void CityLayout::AddRoad(const std::vector<double> &xz, RoadType type)
{
	std::vector<Pt> pts;
	for (size_t k = 0; k + 1 < xz.size(); k += 2)
		pts.push_back({ xz[k], xz[k + 1] });
	if (pts.size() < 2)
		return;
	if (type == ROAD_ARTERIAL && pts.size() > 2)
		pts = Simplified(Rounded(Simplified(pts, ROAD_SIMPLIFY), 3), 1.0);
	float width, walk;
	RoadDims(type, width, walk);
	const uint32_t line = m_lines++;
	float along = 0.0f;
	for (size_t k = 0; k + 1 < pts.size(); k++) {
		const Pt &a = pts[k], &b = pts[k + 1];
		const float len = float(std::hypot(b.x - a.x, b.z - a.z));
		if (len < 0.01f)
			continue;
		m_segments.push_back({ float(a.x), float(a.z), float(b.x), float(b.z), width, walk, uint8_t(type), line, along });
		along += len;
	}
}

std::vector<int> CityLayout::FindPath(int from, int to, const std::vector<float> &cost, const std::vector<uint8_t> &used) const
{
	const size_t cells = size_t(m_n) * m_n;
	std::vector<float> g(cells, BLOCKED);
	std::vector<int> came(cells, -1);
	using Entry = std::pair<float, int>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
	const int ti = to % m_n, tj = to / m_n;
	auto heuristic = [&](int c) {
		return float(0.6 * CELL * std::hypot(double(c % m_n - ti), double(c / m_n - tj)));
	};
	g[from] = 0.0f;
	open.push({ heuristic(from), from });
	while (!open.empty()) {
		const Entry e = open.top();
		open.pop();
		const int c = e.second;
		if (c == to)
			break;
		const float gc = g[c];
		if (e.first - heuristic(c) > gc + 1e-3f)
			continue;
		const int ci = c % m_n, cj = c / m_n;
		for (int dj = -1; dj <= 1; dj++) {
			for (int di = -1; di <= 1; di++) {
				if (!di && !dj)
					continue;
				const int ni = ci + di, nj = cj + dj;
				if (!InGrid(ni, nj))
					continue;
				const int nc = int(Idx(ni, nj));
				if (cost[nc] == BLOCKED && nc != to)
					continue;
				const double len = CELL * ((di && dj) ? 1.41421356 : 1.0);
				const double grade = std::abs(m_height[nc] - m_height[c]) / len;
				const double m = (1.0 + std::pow(grade * 25.0, 2.0)) * (used[nc] ? 0.4 : (cost[nc] == BLOCKED ? 1.0 : cost[nc]));
				const float ng = gc + float(len * m);
				if (ng < g[nc]) {
					g[nc] = ng;
					came[nc] = c;
					open.push({ ng + heuristic(nc), nc });
				}
			}
		}
	}
	std::vector<int> path;
	if (came[to] < 0 && to != from)
		return path;
	for (int c = to; c != -1; c = came[c]) {
		path.push_back(c);
		if (c == from)
			break;
	}
	std::reverse(path.begin(), path.end());
	return path;
}

void CityLayout::LayMainRoads(const Analysis &)
{
	const double R = m_facts.radius;
	const size_t cells = size_t(m_n) * m_n;
	std::vector<float> cost(cells, 1.0f);
	for (size_t k = 0; k < cells; k++)
		if (m_sea[k] || m_port[k])
			cost[k] = BLOCKED;
	cost[m_gate] = 1.0f;
	// under domes the roads stay inside them, but for those joining two domes
	const bool domed = !m_domes.empty();
	std::vector<float> between;
	if (domed) {
		between = cost;
		for (size_t k = 0; k < cells; k++)
			if (cost[k] != BLOCKED && DomeAt(CellX(int(k % m_n)), CellZ(int(k / m_n))) < 0)
				cost[k] = BLOCKED;
		cost[m_gate] = 1.0f;
	}
	std::vector<uint8_t> used(cells, 0);

	auto cellOf = [&](double x, double z) {
		return int(Idx(ClampTo(int((x - m_x0) / CELL), 0, m_n - 1), ClampTo(int((z - m_x0) / CELL), 0, m_n - 1)));
	};
	auto markLine = [&](const std::vector<double> &xz) {
		for (size_t k = 0; k + 3 < xz.size(); k += 2) {
			const double len = std::hypot(xz[k + 2] - xz[k], xz[k + 3] - xz[k + 1]);
			const int steps = std::max(1, int(len / (CELL * 0.5)));
			for (int s = 0; s <= steps; s++) {
				const double t = double(s) / steps;
				used[cellOf(xz[k] + (xz[k + 2] - xz[k]) * t, xz[k + 1] + (xz[k + 3] - xz[k + 1]) * t)] = 1;
			}
		}
	};
	// a road between two cells, the parts not on a road already
	auto route = [&](int from, int to) {
		const bool across = domed && DomeAt(CellX(from % m_n), CellZ(from / m_n)) != DomeAt(CellX(to % m_n), CellZ(to / m_n));
		const std::vector<int> path = FindPath(from, to, across ? between : cost, used);
		std::vector<double> piece;
		auto flush = [&]() {
			if (piece.size() >= 4)
				AddRoad(piece, ROAD_ARTERIAL);
			piece.clear();
		};
		for (size_t k = 0; k < path.size(); k++) {
			const int c = path[k];
			if (used[c]) {
				if (!piece.empty()) {
					piece.push_back(CellX(c % m_n));
					piece.push_back(CellZ(c / m_n));
					flush();
				}
				continue;
			}
			if (piece.empty() && k > 0 && used[path[k - 1]]) {
				piece.push_back(CellX(path[k - 1] % m_n));
				piece.push_back(CellZ(path[k - 1] / m_n));
			}
			piece.push_back(CellX(c % m_n));
			piece.push_back(CellZ(c / m_n));
		}
		flush();
		for (int c : path)
			used[c] = 1;
	};

	// the ways out into the country (none from domes)
	std::vector<int> exits;
	if (!domed) {
		const double dist = std::min(1.15 * R + 700.0, (m_ext - 3) * CELL);
		std::vector<double> yaws;
		if (m_shape == Shape::HIGHWAY || m_shape == Shape::VALLEY || m_shape == Shape::COASTAL) {
			yaws = { m_axis, m_axis + kPi };
		} else {
			const int count = R > 3000.0 ? 3 : 2;
			const double y0 = 2.0 * kPi * m_rng.Double();
			for (int k = 0; k < count; k++)
				yaws.push_back(y0 + 2.0 * kPi * k / count + m_rng.Double(-0.3, 0.3));
		}
		for (double yaw : yaws) {
			for (int tries = 0; tries < 9; tries++) {
				const double y = yaw + (tries & 1 ? 1 : -1) * 0.26 * ((tries + 1) / 2);
				const int c = cellOf(std::cos(y) * dist, std::sin(y) * dist);
				if (cost[c] != BLOCKED && m_slope[c] < STEEP_CELL) {
					exits.push_back(c);
					break;
				}
			}
		}
	}

	// a planned road through the points, the parts over land (and over
	// the city, for builtUp)
	auto addPlanned = [&](const std::vector<double> &xz, bool builtUp) {
		std::vector<double> piece;
		auto flush = [&]() {
			if (piece.size() >= 4) {
				AddRoad(piece, ROAD_ARTERIAL);
				markLine(piece);
			}
			piece.clear();
		};
		for (size_t k = 0; k + 3 < xz.size(); k += 2) {
			const double len = std::hypot(xz[k + 2] - xz[k], xz[k + 3] - xz[k + 1]);
			const int steps = std::max(1, int(len / 25.0));
			for (int s = (k ? 1 : 0); s <= steps; s++) {
				const double t = double(s) / steps;
				const double x = xz[k] + (xz[k + 2] - xz[k]) * t, z = xz[k + 1] + (xz[k + 3] - xz[k + 1]) * t;
				const int c = cellOf(x, z);
				if (cost[c] == BLOCKED || (builtUp && !m_developed[c])) {
					flush();
					continue;
				}
				// a point where the road bends, or every so often, keeps it straight
				if (s == 0 || s == steps || piece.size() < 2 || (s % 8) == 0) {
					piece.push_back(x);
					piece.push_back(z);
				}
			}
		}
		flush();
	};

	// the through road first, where the city lies along one
	if ((m_shape == Shape::HIGHWAY || m_shape == Shape::VALLEY) && exits.size() >= 2)
		route(exits[0], exits[1]);
	// a planned capital: avenues and two diagonals, through the city
	if (m_shape == Shape::GRID) {
		const double r = 2.0 * R;
		const double step = 9.0 * CELL;
		for (int k = -int(r / step); k <= int(r / step); k++) {
			const double c = k * step;
			const double h = std::sqrt(std::max(r * r - c * c, 0.0));
			if (h < 300.0)
				continue;
			addPlanned({ m_cx + c, m_cz - h, m_cx + c, m_cz + h }, true);
			addPlanned({ m_cx - h, m_cz + c, m_cx + h, m_cz + c }, true);
		}
		for (double yaw : { kPi / 4.0, 3.0 * kPi / 4.0 })
			addPlanned({ m_cx - std::cos(yaw) * r, m_cz - std::sin(yaw) * r, m_cx + std::cos(yaw) * r, m_cz + std::sin(yaw) * r }, true);
	}
	// a ring road inside a city's dome
	if (m_shape == Shape::DOME) {
		const double r = 0.72 * m_domes[0].radius;
		std::vector<double> ring;
		for (int k = 0; k <= 64; k++) {
			ring.push_back(m_cx + std::cos(2.0 * kPi * k / 64.0) * r);
			ring.push_back(m_cz + std::sin(2.0 * kPi * k / 64.0) * r);
		}
		addPlanned(ring, false);
	}

	// between the districts: a spanning tree and a few loops; the starport; the exits
	std::vector<int> nodes;
	for (const Seed &s : m_seeds)
		nodes.push_back(int(Idx(s.i, s.j)));
	auto dist = [&](int p, int q) { return std::hypot(double(p % m_n - q % m_n), double(p / m_n - q / m_n)); };
	std::vector<std::pair<int, int>> edges;
	std::vector<uint8_t> in(nodes.size(), 0);
	in[0] = 1;
	for (size_t added = 1; added < nodes.size(); added++) {
		double best = 1e30;
		size_t from = 0, to = 0;
		for (size_t p = 0; p < nodes.size(); p++) {
			if (!in[p])
				continue;
			for (size_t q = 0; q < nodes.size(); q++) {
				if (in[q])
					continue;
				const double d = dist(nodes[p], nodes[q]);
				if (d < best) {
					best = d;
					from = p;
					to = q;
				}
			}
		}
		in[to] = 1;
		edges.push_back({ nodes[from], nodes[to] });
	}
	if (m_shape != Shape::HIGHWAY && m_shape != Shape::DOME_CLUSTER) {
		std::vector<std::pair<double, std::pair<int, int>>> extra;
		for (size_t p = 0; p < nodes.size(); p++)
			for (size_t q = p + 1; q < nodes.size(); q++)
				extra.push_back({ dist(nodes[p], nodes[q]), { nodes[p], nodes[q] } });
		std::sort(extra.begin(), extra.end());
		size_t loops = std::max<size_t>(1, nodes.size() / 3);
		for (const auto &e : extra) {
			if (!loops)
				break;
			bool have = false;
			for (const auto &f : edges)
				have = have || (f.first == e.second.first && f.second == e.second.second) || (f.first == e.second.second && f.second == e.second.first);
			if (!have) {
				edges.push_back(e.second);
				loops--;
			}
		}
	}
	for (int e : exits) {
		size_t near = 0;
		for (size_t p = 1; p < nodes.size(); p++)
			if (dist(nodes[p], e) < dist(nodes[near], e))
				near = p;
		edges.push_back({ nodes[near], e });
	}
	if (std::find(nodes.begin(), nodes.end(), m_gate) == nodes.end())
		edges.push_back({ m_gate, nodes[0] });
	std::sort(edges.begin(), edges.end(), [&](const std::pair<int, int> &p, const std::pair<int, int> &q) {
		return dist(p.first, p.second) < dist(q.first, q.second);
	});
	for (const auto &e : edges)
		route(e.first, e.second);

	// the cells beside the main roads: distance and direction to the nearest;
	// the ones they run through are left for them
	m_mainClear = 0.5 * ARTERIAL_WIDTH + ARTERIAL_WALK + 22.0;
	const double reach = m_mainClear + 3.0 * CELL;
	for (const Segment &s : m_segments) {
		const int i0 = ClampTo(int((std::min(s.x0, s.x1) - reach - m_x0) / CELL), 0, m_n - 1);
		const int i1 = ClampTo(int((std::max(s.x0, s.x1) + reach - m_x0) / CELL), 0, m_n - 1);
		const int j0 = ClampTo(int((std::min(s.z0, s.z1) - reach - m_x0) / CELL), 0, m_n - 1);
		const int j1 = ClampTo(int((std::max(s.z0, s.z1) + reach - m_x0) / CELL), 0, m_n - 1);
		const float yaw = float(std::atan2(s.z1 - s.z0, s.x1 - s.x0));
		for (int j = j0; j <= j1; j++) {
			for (int i = i0; i <= i1; i++) {
				const float d = float(DistToSegment(CellX(i), CellZ(j), s.x0, s.z0, s.x1, s.z1));
				const size_t k = Idx(i, j);
				if (d < m_mainDist[k]) {
					m_mainDist[k] = d;
					m_mainYaw[k] = yaw;
				}
			}
		}
	}
	for (size_t k = 0; k < cells; k++)
		m_roadCell[k] = m_mainDist[k] < m_mainClear && !m_port[k];
}

void CityLayout::LayStreets()
{
	const int n = m_n;
	auto urban = [&](int i, int j) {
		const size_t k = Idx(i, j);
		return (m_developed[k] || m_roadCell[k]) && !m_sea[k] && !m_port[k];
	};
	auto onGrid = [&](int seed, int k, int axis) {
		if (seed < 0)
			return false;
		const Seed &s = m_seeds[seed];
		return ((k - (axis ? s.offsetZ : s.offsetX)) % s.block + s.block) % s.block == 0;
	};
	auto typeOf = [&](int seed) {
		return m_seeds[seed].kind == District::DOWNTOWN ? ROAD_CENTRE : ROAD_LOCAL;
	};
	// a street can't run along a main road, only across it
	auto alongMain = [&](size_t a, size_t b, bool vertical) {
		if (m_mainDist[a] > m_mainClear || m_mainDist[b] > m_mainClear)
			return false;
		const double s = std::abs(std::sin(m_mainYaw[a]));
		return vertical ? s > 0.8 : s < 0.6;
	};
	auto edge = [&](int ia, int ja, int ib, int jb, int k, int axis) -> uint8_t {
		if (!urban(ia, ja) || !urban(ib, jb))
			return 0;
		const size_t a = Idx(ia, ja), b = Idx(ib, jb);
		if (std::abs(m_height[a] - m_height[b]) > STREET_CLIMB || alongMain(a, b, axis == 0))
			return 0;
		const int sa = m_cellSeed[a], sb = m_cellSeed[b];
		if (!onGrid(sa, k, axis) && !onGrid(sb, k, axis))
			return 0;
		const int s = sa >= 0 ? sa : sb;
		const RoadType t = (sa >= 0 && typeOf(sa) == ROAD_CENTRE) || (sb >= 0 && typeOf(sb) == ROAD_CENTRE) ? ROAD_CENTRE : typeOf(s);
		return uint8_t(t + 1);
	};
	for (int j = 0; j < n; j++)
		for (int k = 1; k < n; k++)
			m_vEdge[size_t(j) * (n + 1) + k] = edge(k - 1, j, k, j, k, 0);
	for (int k = 1; k < n; k++)
		for (int i = 0; i < n; i++)
			m_hEdge[size_t(k) * n + i] = edge(i, k - 1, i, k, k, 1);

	// straight runs of one width, no single stubs
	const double x0 = m_x0;
	for (int k = 0; k <= n; k++) {
		for (int j = 0; j < n;) {
			uint8_t &e = m_vEdge[size_t(j) * (n + 1) + k];
			if (!e) {
				j++;
				continue;
			}
			int j1 = j;
			while (j1 + 1 < n && m_vEdge[size_t(j1 + 1) * (n + 1) + k] == e)
				j1++;
			if (j1 > j)
				AddRoad({ x0 + k * CELL, x0 + j * CELL, x0 + k * CELL, x0 + (j1 + 1) * CELL }, RoadType(e - 1));
			else
				e = 0;
			j = j1 + 1;
		}
	}
	for (int k = 0; k <= n; k++) {
		for (int i = 0; i < n;) {
			uint8_t &e = m_hEdge[size_t(k) * n + i];
			if (!e) {
				i++;
				continue;
			}
			int i1 = i;
			while (i1 + 1 < n && m_hEdge[size_t(k) * n + i1 + 1] == e)
				i1++;
			if (i1 > i)
				AddRoad({ x0 + i * CELL, x0 + k * CELL, x0 + (i1 + 1) * CELL, x0 + k * CELL }, RoadType(e - 1));
			else
				e = 0;
			i = i1 + 1;
		}
	}
}

void CityLayout::BuildLookup()
{
	m_lookN = int(std::ceil(m_n * CELL / LOOKUP_CELL));
	m_lookup.assign(size_t(m_lookN) * m_lookN, {});
	for (uint32_t s = 0; s < m_segments.size(); s++) {
		const Segment &g = m_segments[s];
		const double reach = 0.5 * g.width + g.walk + 2.0;
		const int i0 = ClampTo(int((std::min(g.x0, g.x1) - reach - m_x0) / LOOKUP_CELL), 0, m_lookN - 1);
		const int i1 = ClampTo(int((std::max(g.x0, g.x1) + reach - m_x0) / LOOKUP_CELL), 0, m_lookN - 1);
		const int j0 = ClampTo(int((std::min(g.z0, g.z1) - reach - m_x0) / LOOKUP_CELL), 0, m_lookN - 1);
		const int j1 = ClampTo(int((std::max(g.z0, g.z1) + reach - m_x0) / LOOKUP_CELL), 0, m_lookN - 1);
		for (int j = j0; j <= j1; j++) {
			for (int i = i0; i <= i1; i++) {
				// the nearest point of the cell to the segment, near enough
				const double cx = m_x0 + (i + 0.5) * LOOKUP_CELL, cz = m_x0 + (j + 0.5) * LOOKUP_CELL;
				if (DistToSegment(cx, cz, g.x0, g.z0, g.x1, g.z1) < reach + LOOKUP_CELL * 0.7072)
					m_lookup[size_t(j) * m_lookN + i].push_back(s);
			}
		}
	}
}

// ------------------------------------------------------------------ queries
bool CityLayout::IsBuildable(int i, int j) const
{
	const size_t k = Idx(i, j);
	return m_developed[k] && !m_sea[k] && !m_roadCell[k] && !m_port[k] && m_slope[k] < STEEP_CELL;
}

bool CityLayout::IsFringe(int i, int j) const
{
	const size_t k = Idx(i, j);
	return !m_developed[k] && !m_sea[k] && !m_roadCell[k] && !m_port[k] && m_domes.empty() &&
		m_mainDist[k] < m_mainClear + 70.0 && m_slope[k] < 0.7 * STEEP_CELL &&
		DistanceToCentre(CellX(i), CellZ(j)) < 1.4 * m_facts.radius;
}

void CityLayout::Setbacks(int i, int j, double out[4]) const
{
	const uint8_t codes[4] = {
		m_vEdge[size_t(j) * (m_n + 1) + i],
		m_vEdge[size_t(j) * (m_n + 1) + i + 1],
		m_hEdge[size_t(j) * m_n + i],
		m_hEdge[size_t(j + 1) * m_n + i],
	};
	for (int s = 0; s < 4; s++) {
		out[s] = 0.0;
		if (!codes[s])
			continue;
		float width, walk;
		RoadDims(RoadType(codes[s] - 1), width, walk);
		out[s] = 0.5 * width + walk + SETBACK_MARGIN;
	}
}

bool CityLayout::StreetBetween(int i0, int j0, int i1, int j1) const
{
	if (j0 == j1 && std::abs(i1 - i0) == 1)
		return m_vEdge[size_t(j0) * (m_n + 1) + std::max(i0, i1)] != 0;
	if (i0 == i1 && std::abs(j1 - j0) == 1)
		return m_hEdge[size_t(std::max(j0, j1)) * m_n + i0] != 0;
	return false;
}

bool CityLayout::BesideMainRoad(int i, int j, double &yaw, double &distance) const
{
	const size_t k = Idx(i, j);
	if (m_roadCell[k] || m_mainDist[k] > m_mainClear + 2.0 * CELL)
		return false;
	yaw = m_mainYaw[k];
	distance = m_mainDist[k];
	return true;
}

int CityLayout::DomeAt(double x, double z) const
{
	for (size_t d = 0; d < m_domes.size(); d++)
		if (std::hypot(x - m_domes[d].x, z - m_domes[d].z) < m_domes[d].radius)
			return int(d);
	return -1;
}

double CityLayout::HeightCap(double x, double z) const
{
	const int d = DomeAt(x, z);
	if (d < 0)
		return 1e9;
	const Dome &dome = m_domes[d];
	// a cap of a sphere radius a at the ground, h high
	const double a = dome.radius, h = a * m_facts.domeHeightRatio;
	const double rho = (a * a + h * h) / (2.0 * h);
	const double r = std::hypot(x - dome.x, z - dome.z) + 25.0;
	if (r >= rho)
		return 0.0;
	return h - rho + std::sqrt(rho * rho - r * r) - m_facts.domeClearance;
}

double CityLayout::Clearance(const double *xz, size_t count) const
{
	double best = 1e9;
	for (size_t p = 0; p < count; p++) {
		const double x = xz[2 * p], z = xz[2 * p + 1];
		const int i = int(std::floor((x - m_x0) / LOOKUP_CELL)), j = int(std::floor((z - m_x0) / LOOKUP_CELL));
		if (i < 0 || j < 0 || i >= m_lookN || j >= m_lookN)
			continue;
		for (uint32_t s : m_lookup[size_t(j) * m_lookN + i]) {
			const Segment &g = m_segments[s];
			best = std::min(best, DistToSegment(x, z, g.x0, g.z0, g.x1, g.z1) - (0.5 * g.width + g.walk));
		}
	}
	return best;
}

void CityLayout::PackRoads(std::vector<float> &atlas, float info[4], float rect[4]) const
{
	const size_t cells = size_t(m_lookN) * m_lookN;
	std::vector<uint32_t> firsts(cells), counts(cells);
	std::vector<uint32_t> indices;
	for (size_t c = 0; c < cells; c++) {
		const std::vector<uint32_t> &list = m_lookup[c];
		firsts[c] = uint32_t(indices.size());
		counts[c] = uint32_t(std::min<size_t>(list.size(), LOOKUP_MAX));
		indices.insert(indices.end(), list.begin(), list.begin() + counts[c]);
	}
	const uint32_t headerTexels = uint32_t((cells + 1) / 2);
	const uint32_t indexTexels = uint32_t((indices.size() + 3) / 4);
	const uint32_t segmentTexels = uint32_t(m_segments.size() * 2);
	const uint32_t rows = std::max(1u, (headerTexels + indexTexels + segmentTexels + TEXEL_WIDTH - 1) / TEXEL_WIDTH);

	atlas.assign(size_t(rows) * TEXEL_WIDTH * 4, 0.0f);
	for (size_t c = 0; c < cells; c++) {
		atlas[2 * c] = float(firsts[c]);
		atlas[2 * c + 1] = float(counts[c]);
	}
	float *index = atlas.data() + size_t(headerTexels) * 4;
	for (size_t k = 0; k < indices.size(); k++)
		index[k] = float(indices[k]);
	float *texel = atlas.data() + size_t(headerTexels + indexTexels) * 4;
	for (const Segment &s : m_segments) {
		*texel++ = s.x0;
		*texel++ = s.z0;
		*texel++ = s.x1;
		*texel++ = s.z1;
		*texel++ = s.width;
		*texel++ = s.walk;
		*texel++ = float(s.line * 4 + s.type);
		*texel++ = s.along;
	}
	info[0] = float(m_lookN);
	info[1] = float(headerTexels);
	info[2] = float(headerTexels + indexTexels);
	info[3] = float(rows);
	rect[0] = float(m_x0);
	rect[1] = float(m_x0);
	rect[2] = float(LOOKUP_CELL);
	rect[3] = float(m_lookN);
}
