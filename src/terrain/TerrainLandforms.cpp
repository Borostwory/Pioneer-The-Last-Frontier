// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainLandforms.h"

#include <algorithm>
#include <cmath>

// The styles as terrain-tile.glsl has them (see there for what each part is), with the hashes made anew (see the
// header). Heights in cells of the octave: the height whose slope the tiles use, less its mean where it has one, so
// the landforms don't lift the ground.
namespace {
	using v3 = vector3d;

	inline double Mix(const double a, const double b, const double t) { return a + (b - a) * t; }
	inline double SmoothStep(const double e0, const double e1, const double x)
	{
		const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	// Three whole numbers to three well-mixed ones (pcg3d: Jarzynski and Olano, "Hash Functions for GPU Rendering",
	// 2020). The same on every machine, and cheap.
	struct Hash {
		uint32_t x, y, z;
	};
	inline Hash Pcg3d(uint32_t x, uint32_t y, uint32_t z)
	{
		x = x * 1664525u + 1013904223u;
		y = y * 1664525u + 1013904223u;
		z = z * 1664525u + 1013904223u;
		x += y * z;
		y += z * x;
		z += x * y;
		x ^= x >> 16u;
		y ^= y >> 16u;
		z ^= z >> 16u;
		x += y * z;
		y += z * x;
		z += x * y;
		return { x, y, z };
	}

	// a cell, counted from the planet's centre; salt: which octave and which use
	inline Hash CellHash(const int64_t cx, const int64_t cy, const int64_t cz, const uint32_t salt)
	{
		return Pcg3d(uint32_t(cx) ^ (salt * 0x9E3779B9u), uint32_t(cy) ^ (salt * 0x85EBCA6Bu), uint32_t(cz) ^ (salt * 0xC2B2AE35u));
	}

	// six numbers 0..1 from a hash: the low and high halves of its three
	constexpr double TO_UNIT = 1.0 / 65535.0;
	inline double Lo(const uint32_t v) { return double(v & 0xffffu) * TO_UNIT; }
	inline double Hi(const uint32_t v) { return double(v >> 16u) * TO_UNIT; }

	struct Cell {
		int64_t c[3];
		v3 f; // where the point is across the cell, 0..1
	};
	inline Cell CellOf(const v3 &q)
	{
		const v3 fl(std::floor(q.x), std::floor(q.y), std::floor(q.z));
		return { { int64_t(fl.x), int64_t(fl.y), int64_t(fl.z) }, q - fl };
	}

	struct Noise {
		double v = 0.0;
		v3 d = v3(0.0);
	};

	// gradient noise with its derivatives (as tileNoised), from the gradients at the cell's eight corners, in the
	// order (0,0,0), (1,0,0), (0,1,0), (1,1,0), (0,0,1), (1,0,1), (0,1,1), (1,1,1)
	Noise NoiseFrom(const v3 g[8], const v3 &f, const v3 &u, const v3 &du)
	{
		const v3 &ga = g[0], &gb = g[1], &gc = g[2], &gd = g[3], &ge = g[4], &gf = g[5], &gg = g[6], &gh = g[7];
		const double va = ga.Dot(f);
		const double vb = gb.Dot(f - v3(1, 0, 0));
		const double vc = gc.Dot(f - v3(0, 1, 0));
		const double vd = gd.Dot(f - v3(1, 1, 0));
		const double ve = ge.Dot(f - v3(0, 0, 1));
		const double vf = gf.Dot(f - v3(1, 0, 1));
		const double vg = gg.Dot(f - v3(0, 1, 1));
		const double vh = gh.Dot(f - v3(1, 1, 1));
		Noise n;
		n.v = va + u.x * (vb - va) + u.y * (vc - va) + u.z * (ve - va) +
			u.x * u.y * (va - vb - vc + vd) + u.y * u.z * (va - vc - ve + vg) +
			u.z * u.x * (va - vb - ve + vf) + (-va + vb + vc - vd + ve - vf - vg + vh) * u.x * u.y * u.z;
		const v3 uyzx(u.y, u.z, u.x), uzxy(u.z, u.x, u.y);
		n.d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.z * (ge - ga) +
			u.x * u.y * (ga - gb - gc + gd) + u.y * u.z * (ga - gc - ge + gg) +
			u.z * u.x * (ga - gb - ge + gf) + (-ga + gb + gc - gd + ge - gf - gg + gh) * (u.x * u.y * u.z) +
			du * (v3(vb, vc, ve) - va +
				uyzx * v3(va - vb - vc + vd, va - vc - ve + vg, va - vb - ve + vf) +
				uzxy * v3(va - vb - ve + vf, va - vb - vc + vd, va - vc - ve + vg) +
				uyzx * uzxy * (-va + vb + vc - vd + ve - vf - vg + vh));
		return n;
	}

	// one noise at x (in cells), or two independent ones from the same corner hashes (the low and high halves of
	// their bits)
	void GradientNoise(const v3 &x, const uint32_t salt, const bool two, Noise &a, Noise &b)
	{
		const Cell cell = CellOf(x);
		const v3 &f = cell.f;
		const v3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
		const v3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
		v3 ga[8], gb[8];
		for (int k = 0; k < 8; k++) {
			const Hash h = CellHash(cell.c[0] + (k & 1), cell.c[1] + ((k >> 1) & 1), cell.c[2] + ((k >> 2) & 1), salt);
			ga[k] = v3(Lo(h.x), Lo(h.y), Lo(h.z)) * 2.0 - 1.0;
			if (two)
				gb[k] = v3(Hi(h.x), Hi(h.y), Hi(h.z)) * 2.0 - 1.0;
		}
		a = NoiseFrom(ga, f, u, du);
		if (two)
			b = NoiseFrom(gb, f, u, du);
	}

	// one octave of a style: slope (cells per cell, so metres per metre), landform value (-1..1), height (cells)
	struct Octave {
		v3 g = v3(0.0);
		double form = 0.0;
		double height = 0.0;

		void Add(const double weight, const Octave &o)
		{
			g += o.g * weight;
			form += o.form * weight;
			height += o.height * weight;
		}
	};

	Octave Rolling(const Noise &n, v3 &accumulated)
	{
		// eroded: damped where the larger octaves are steep
		accumulated += n.d;
		const double damp = 1.0 / (1.0 + 0.6 * accumulated.LengthSqr());
		return { n.d * (0.45 * damp), n.v * 1.4 * damp, n.v * 0.45 * damp };
	}

	// about the mean of r^2 below, so the ridges don't lift the ground (to be set in game in P3)
	constexpr double RIDGE_MEAN = 0.6;
	Octave Ridged(const Noise &n)
	{
		const double r = 1.0 - std::min(std::abs(n.v) * 1.6, 1.0);
		const double sign = n.v > 0.0 ? 1.0 : (n.v < 0.0 ? -1.0 : 0.0);
		return { n.d * (0.09 * 2.0 * r * (-sign * 1.6)), r * r * 2.0 - 1.0, 0.09 * (r * r - RIDGE_MEAN) };
	}

	// the mean of s^2 below, for a phase spread evenly
	constexpr double DUNE_MEAN = 0.375;
	Octave Dunes(const v3 &q, const uint32_t salt)
	{
		Noise n, unused;
		GradientNoise(q * 0.5, salt, false, n, unused);
		const double phase = 6.2831853 * 2.5 * n.v;
		const v3 dphase = n.d * (6.2831853 * 2.5 * 0.5);
		const double s = 0.5 + 0.5 * std::sin(phase);
		return { dphase * (0.075 * 2.0 * s * 0.5 * std::cos(phase)), s * s * 2.0 - 1.0, 0.075 * (s * s - DUNE_MEAN) };
	}

	constexpr double BLOCK_CRACK = 0.1;
	Octave Blocks(const Cell &cell, const v3 &q, const uint32_t salt)
	{
		double d1 = 1e9, d2 = 1e9;
		v3 p1(0.0), p2(0.0);
		int64_t c1[3] = { 0, 0, 0 };
		for (int z = -1; z <= 1; ++z)
			for (int y = -1; y <= 1; ++y)
				for (int x = -1; x <= 1; ++x) {
					const int64_t c[3] = { cell.c[0] + x, cell.c[1] + y, cell.c[2] + z };
					const Hash h = CellHash(c[0], c[1], c[2], salt);
					// (relative to the point's cell, to keep the precision)
					const v3 p = v3(double(x), double(y), double(z)) + 0.15 + 0.7 * v3(Lo(h.x), Lo(h.y), Lo(h.z));
					const double d = (cell.f - p).LengthSqr();
					if (d < d1) {
						d2 = d1;
						p2 = p1;
						d1 = d;
						p1 = p;
						std::copy(c, c + 3, c1);
					} else if (d < d2) {
						d2 = d;
						p2 = p;
					}
				}
		// the plate's own height and tilt
		const Hash h = CellHash(c1[0], c1[1], c1[2], salt + 1u);
		const v3 tilt = (v3(Hi(h.x), Lo(h.y), Hi(h.y)) - 0.5) * 0.5;
		const v3 &f = cell.f;
		const double top = (Lo(h.x) - 0.5) * 0.6 + (f - p1).Dot(tilt);
		const v3 axis = p2 - p1;
		const double len = std::max(axis.Length(), 1e-4);
		const double toCrack = (0.5 * (p1 + p2).Dot(axis) - f.Dot(axis)) / len;
		const double t = std::clamp(toCrack / BLOCK_CRACK, 0.0, 1.0);
		const double whole = t * t * (3.0 - 2.0 * t);
		const double floorHeight = -0.35;
		const double height = Mix(floorHeight, top, whole);
		const v3 grad = whole * tilt + (-axis / len) * ((top - floorHeight) * (6.0 * t * (1.0 - t) / BLOCK_CRACK));
		(void)q;
		return { grad * 0.12, std::clamp(height * 1.5, -1.0, 1.0), 0.12 * height };
	}

	// where small craters lie thick or thin, 0..1: smooth, from the place on the body alone
	double CraterRegion(const v3 &up)
	{
		double sum = 0.0, amplitude = 0.65, frequency = 9.0;
		uint32_t salt = 0x51ED27u;
		for (int octave = 0; octave < 2; ++octave) {
			const Cell cell = CellOf(up * frequency);
			v3 t = cell.f;
			t = t * t * (3.0 - 2.0 * t);
			const auto at = [&](const int x, const int y, const int z) { return Lo(CellHash(cell.c[0] + x, cell.c[1] + y, cell.c[2] + z, salt).x); };
			const double low = Mix(Mix(at(0, 0, 0), at(1, 0, 0), t.x), Mix(at(0, 1, 0), at(1, 1, 0), t.x), t.y);
			const double high = Mix(Mix(at(0, 0, 1), at(1, 0, 1), t.x), Mix(at(0, 1, 1), at(1, 1, 1), t.x), t.y);
			sum += amplitude * Mix(low, high, t.z);
			amplitude = 0.35;
			frequency = 37.0;
			salt = 0x51ED28u;
		}
		return SmoothStep(0.3, 0.7, sum);
	}

	// As the tiles' craters, with the cells pruned as in P2: a centre lies 0.25 to 0.75 across its cell and a crater
	// reaches at most 0.8 cells. One hash per cell gives all six of its numbers.
	constexpr double CRATER_REACH = 0.8;
	Octave Craters(const Cell &cell, const uint32_t salt, const double density, const bool varied, double &inside)
	{
		const v3 &f = cell.f;
		int lo[3], hi[3];
		for (int a = 0; a < 3; a++) {
			lo[a] = f[a] + 0.25 <= CRATER_REACH + 1e-9 ? -1 : 0;
			hi[a] = 1.25 - f[a] <= CRATER_REACH + 1e-9 ? 1 : 0;
		}
		Octave o;
		double bumps = 0.0; // bowls and rims, in cells
		inside = 0.0;
		for (int z = lo[2]; z <= hi[2]; ++z)
			for (int y = lo[1]; y <= hi[1]; ++y)
				for (int x = lo[0]; x <= hi[0]; ++x) {
					const Hash h = CellHash(cell.c[0] + x, cell.c[1] + y, cell.c[2] + z, salt);
					if (Lo(h.x) > density)
						continue;
					const double size = Hi(h.x), age = Lo(h.y);
					const v3 centre = v3(double(x), double(y), double(z)) + 0.25 + 0.5 * v3(Hi(h.y), Lo(h.z), Hi(h.z));
					const double radius = varied ? 0.1 + 0.4 * size * size : 0.18 + 0.3 * size;
					const double fresh = varied ? age * age : 1.0;
					const v3 d = f - centre;
					const double dist = d.Length();
					const double x01 = dist / radius;
					inside = std::max(inside, 1.0 - SmoothStep(0.7, 1.0, x01));
					if (x01 > 1.6 || dist < 1e-5)
						continue;
					// bowl inside, raised rim around the edge, fading outside
					const double depth = 0.22 * radius * Mix(0.12, 1.0, fresh), rim = 0.08 * radius * Mix(0.02, 1.0, fresh);
					const double rimWidth = Mix(0.4, 0.25, fresh);
					const double bowl = x01 < 1.0 ? depth * (x01 * x01 - 1.0) : 0.0;
					const double dbowl = x01 < 1.0 ? depth * 2.0 * x01 : 0.0;
					const double e = (x01 - 1.0) / rimWidth;
					const double rimH = rim * std::exp(-e * e);
					const double drim = rimH * (-2.0 * e / rimWidth);
					o.g += d * ((dbowl + drim) / radius / dist);
					o.form += (bowl + rimH) / (0.1 * radius);
					bumps += bowl + rimH;
				}
		o.g = o.g * 1.2;
		o.form = std::clamp(o.form, -1.0, 1.0);
		o.height = 1.2 * bumps;
		return o;
	}

	constexpr double CRATER_MAX_NEST = 2.0;
} // namespace

TerrainLandforms::Sample TerrainLandforms::Evaluate(const Params &p, const vector3d &up, const double minWavelength, const double maxWavelength)
{
	Sample result;
	const v3 position = up * p.radius;
	v3 grad(0.0), accumulated(0.0);
	double height = 0.0, form = 0.0, formWeight = 0.0, nest = 0.0;
	const bool variedCraters = p.craterTop > 0.0;
	double craterDensity = 0.35 * std::min(double(p.style[3]), 1.0);
	if (variedCraters && p.style[3] > 0.0f)
		craterDensity *= Mix(0.05, 0.95, CraterRegion(up));
	const bool rolling = p.style[0] > 0.0f, ridged = p.style[1] > 0.0f;
	for (int i = p.count - 1; i >= 0; --i) {
		const double lambda = p.finestWave * std::pow(2.0, double(i));
		// finer ones from here on
		if (lambda < minWavelength * 0.99)
			break;
		if (maxWavelength > 0.0 && lambda > maxWavelength * 1.01)
			continue;
		const v3 q = position / lambda;
		const Cell cell = CellOf(q);
		// a range of salts for each octave: one for each use
		const uint32_t salt = uint32_t(p.finest + i + 4096) * 16u;
		const double w = i == 0 ? 0.6 : 1.0;
		Octave o;
		bool added = false;
		if (p.landformTop > 0.0 ? lambda <= p.landformTop * 1.01 : (i < p.band && (p.craterTop <= 0.0 || lambda <= p.craterTop * 1.01))) {
			if (rolling || ridged) {
				Noise a, b;
				GradientNoise(q, salt, rolling && ridged, a, b);
				if (rolling)
					o.Add(p.style[0], Rolling(a, accumulated));
				if (ridged)
					o.Add(p.style[1], Ridged(rolling ? b : a));
			}
			if (p.style[2] > 0.0f)
				o.Add(p.style[2], Dunes(q, salt + 2u));
			if (p.blocks > 0.0)
				o.Add(p.blocks, Blocks(cell, q, salt + 3u));
			added = true;
		}
		if (p.style[3] > 0.0f && (p.craterTop <= 0.0 || lambda <= p.craterTop * 1.01)) {
			// faded craters do not count towards the nesting
			const double allowed = 1.0 - SmoothStep(CRATER_MAX_NEST - 0.75, CRATER_MAX_NEST, nest);
			if (allowed > 0.0) {
				double inside;
				o.Add(allowed, Craters(cell, salt + 5u, craterDensity, variedCraters, inside));
				nest += allowed * inside;
				added = true;
			}
		}
		if (added)
			result.octaves++;
		grad += o.g * w;
		height += o.height * w * lambda;
		if (i < p.band) {
			// weighted towards the band's longest octave
			const double scale = std::pow(2.0, double(p.band - 1 - i));
			form += w * o.form / scale;
			formWeight += w / scale;
		}
	}
	result.height = height;
	result.slope = grad - up * grad.Dot(up);
	result.form = std::clamp(form / std::max(formWeight, 1e-6), -1.0, 1.0);
	return result;
}
