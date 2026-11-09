// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainTilesCPU.h"

#include "GeoSphere.h"
#include "TerrainTiles.h"
#include "galaxy/SystemBody.h"
#include "terrain/Terrain.h"

#include <algorithm>
#include <cmath>

// Line by line as terrain-tile.glsl and terrain_tile.frag; see there for what each part is.
namespace {
	using v3 = vector3d;

	inline double Fract(const double x) { return x - std::floor(x); }
	inline double Mod(const double x, const double y) { return x - y * std::floor(x / y); }
	inline v3 Floor(const v3 &v) { return v3(std::floor(v.x), std::floor(v.y), std::floor(v.z)); }
	inline v3 Mod(const v3 &v, const double y) { return v3(Mod(v.x, y), Mod(v.y, y), Mod(v.z, y)); }
	inline double Mix(const double a, const double b, const double t) { return a + (b - a) * t; }
	inline double SmoothStep(const double e0, const double e1, const double x)
	{
		const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	v3 TileGrad(const v3 &c, const double salt)
	{
		v3 p = Mod(c, 256.0) + v3(17.0, 59.0, 113.0) * salt;
		p = v3(Fract(p.x * 0.1031), Fract(p.y * 0.1030), Fract(p.z * 0.0973));
		p = p + p.Dot(v3(p.y, p.x, p.z) + 33.33);
		// fract((p.xxy + p.yxx) * p.zyx)
		return v3(Fract((p.x + p.y) * p.z), Fract((p.x + p.x) * p.y), Fract((p.y + p.x) * p.x)) * 2.0 - 1.0;
	}

	struct Noise {
		double v;
		v3 d;
	};

	Noise TileNoised(const v3 &x, const double salt)
	{
		const v3 i = Floor(x);
		const v3 f = x - i;
		const v3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
		const v3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);

		const v3 ga = TileGrad(i + v3(0, 0, 0), salt);
		const v3 gb = TileGrad(i + v3(1, 0, 0), salt);
		const v3 gc = TileGrad(i + v3(0, 1, 0), salt);
		const v3 gd = TileGrad(i + v3(1, 1, 0), salt);
		const v3 ge = TileGrad(i + v3(0, 0, 1), salt);
		const v3 gf = TileGrad(i + v3(1, 0, 1), salt);
		const v3 gg = TileGrad(i + v3(0, 1, 1), salt);
		const v3 gh = TileGrad(i + v3(1, 1, 1), salt);

		const double va = ga.Dot(f - v3(0, 0, 0));
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

	inline v3 TileHash33(const v3 &c, const double salt)
	{
		return TileGrad(c, salt + 7.0) * 0.5 + 0.5;
	}

	// gradient, and the landform value
	struct Octave {
		v3 g = v3(0.0);
		double form = 0.0;
	};

	Octave TileRolling(const v3 &q, const double salt, v3 &accumulated)
	{
		const Noise n = TileNoised(q, salt);
		accumulated += n.d;
		const double damp = 1.0 / (1.0 + 0.6 * accumulated.LengthSqr());
		return { n.d * (0.45 * damp), n.v * 1.4 * damp };
	}

	Octave TileRidged(const v3 &q, const double salt)
	{
		const Noise n = TileNoised(q, salt + 31.0);
		const double r = 1.0 - std::min(std::abs(n.v) * 1.6, 1.0);
		const double sign = n.v > 0.0 ? 1.0 : (n.v < 0.0 ? -1.0 : 0.0);
		const v3 g = n.d * (2.0 * r * (-sign * 1.6));
		return { g * 0.09, r * r * 2.0 - 1.0 };
	}

	Octave TileDunes(const v3 &q, const double salt)
	{
		const Noise n = TileNoised(q * 0.5, salt + 61.0);
		const double phase = 6.2831853 * 2.5 * n.v;
		const v3 dphase = n.d * (6.2831853 * 2.5 * 0.5);
		const double s = 0.5 + 0.5 * std::sin(phase);
		const double h = s * s;
		const v3 g = dphase * (2.0 * s * 0.5 * std::cos(phase));
		return { g * 0.075, h * 2.0 - 1.0 };
	}

	constexpr double BLOCK_CRACK = 0.1;
	Octave TileBlocks(const v3 &q, const double salt)
	{
		const v3 cell = Floor(q);
		double d1 = 1e9, d2 = 1e9;
		v3 p1(0.0), p2(0.0), c1(0.0);
		for (int z = -1; z <= 1; ++z)
			for (int y = -1; y <= 1; ++y)
				for (int x = -1; x <= 1; ++x) {
					const v3 c = cell + v3(x, y, z);
					const v3 p = c + 0.15 + 0.7 * TileHash33(c, salt + 131.0);
					const v3 r = q - p;
					const double d = r.LengthSqr();
					if (d < d1) {
						d2 = d1;
						p2 = p1;
						d1 = d;
						p1 = p;
						c1 = c;
					} else if (d < d2) {
						d2 = d;
						p2 = p;
					}
				}
		const v3 h = TileHash33(c1, salt + 137.0);
		const v3 tilt = (TileHash33(c1, salt + 139.0) - 0.5) * 0.5;
		const double top = (h.x - 0.5) * 0.6 + (q - p1).Dot(tilt);
		const v3 axis = p2 - p1;
		const double len = std::max(axis.Length(), 1e-4);
		const double toCrack = (0.5 * (p1 + p2).Dot(axis) - q.Dot(axis)) / len;
		const double t = std::clamp(toCrack / BLOCK_CRACK, 0.0, 1.0);
		const double whole = t * t * (3.0 - 2.0 * t);
		const double floorHeight = -0.35;
		const double height = Mix(floorHeight, top, whole);
		const v3 grad = whole * tilt + (-axis / len) * ((top - floorHeight) * (6.0 * t * (1.0 - t) / BLOCK_CRACK));
		return { grad * 0.12, std::clamp(height * 1.5, -1.0, 1.0) };
	}

	double TileCraterRegion(const v3 &up)
	{
		double sum = 0.0, amplitude = 0.65, frequency = 9.0, salt = 311.0;
		for (int octave = 0; octave < 2; ++octave) {
			const v3 q = up * frequency, cell = Floor(q);
			v3 t = q - cell;
			t = t * t * (3.0 - 2.0 * t);
			const auto at = [&](const double x, const double y, const double z) { return TileHash33(cell + v3(x, y, z), salt).x; };
			const double low = Mix(Mix(at(0, 0, 0), at(1, 0, 0), t.x), Mix(at(0, 1, 0), at(1, 1, 0), t.x), t.y);
			const double high = Mix(Mix(at(0, 0, 1), at(1, 0, 1), t.x), Mix(at(0, 1, 1), at(1, 1, 1), t.x), t.y);
			sum += amplitude * Mix(low, high, t.z);
			amplitude = 0.35;
			frequency = 37.0;
			salt = 317.0;
		}
		return SmoothStep(0.3, 0.7, sum);
	}

	// A crater's centre lies 0.25 to 0.75 across its cell, and it reaches at most 0.8 cells from it (1.6 radii of at
	// most 0.5). So along each axis only the neighbouring cell on the near side can reach a point at f (0..1 across its
	// own cell), or both near the middle: about 9 cells to look at instead of 27, with the same result (as
	// tileCraterCellsFrom/To in terrain-tile.glsl).
	constexpr double CRATER_REACH = 0.8;
	inline void CraterCellRange(const double f, int &lo, int &hi)
	{
		lo = f + 0.25 <= CRATER_REACH + 1e-9 ? -1 : 0;
		hi = 1.25 - f <= CRATER_REACH + 1e-9 ? 1 : 0;
	}

	// allCells: all 27 cells around, as before P2, to check that the result is the same
	Octave TileCraters(const v3 &q, const double salt, const double density, const bool varied, double &inside, const bool allCells = false)
	{
		const v3 cell = Floor(q);
		v3 grad(0.0);
		double form = 0.0;
		inside = 0.0;
		int lo[3] = { -1, -1, -1 }, hi[3] = { 1, 1, 1 };
		if (!allCells) {
			const v3 f = q - cell;
			CraterCellRange(f.x, lo[0], hi[0]);
			CraterCellRange(f.y, lo[1], hi[1]);
			CraterCellRange(f.z, lo[2], hi[2]);
		}
		for (int z = lo[2]; z <= hi[2]; ++z)
			for (int y = lo[1]; y <= hi[1]; ++y)
				for (int x = lo[0]; x <= hi[0]; ++x) {
					const v3 c = cell + v3(x, y, z);
					const v3 h = TileHash33(c, salt + 97.0);
					if (h.x > density)
						continue;
					const v3 centre = c + 0.25 + 0.5 * TileHash33(c, salt + 131.0);
					const double radius = varied ? 0.1 + 0.4 * h.y * h.y : 0.18 + 0.3 * h.y;
					const double fresh = varied ? h.z * h.z : 1.0;
					const v3 d = q - centre;
					const double dist = d.Length();
					const double x01 = dist / radius;
					inside = std::max(inside, 1.0 - SmoothStep(0.7, 1.0, x01));
					if (x01 > 1.6 || dist < 1e-5)
						continue;
					const double depth = 0.22 * radius * Mix(0.12, 1.0, fresh), rim = 0.08 * radius * Mix(0.02, 1.0, fresh);
					const double rimWidth = Mix(0.4, 0.25, fresh);
					const double bowl = x01 < 1.0 ? depth * (x01 * x01 - 1.0) : 0.0;
					const double dbowl = x01 < 1.0 ? depth * 2.0 * x01 : 0.0;
					const double e = (x01 - 1.0) / rimWidth;
					const double rimH = rim * std::exp(-e * e);
					const double drim = rimH * (-2.0 * e / rimWidth);
					const double dh = (dbowl + drim) / radius;
					grad += d * (dh / dist);
					form += (bowl + rimH) / (0.1 * radius);
				}
		return { grad * 1.2, std::clamp(form, -1.0, 1.0) };
	}

	constexpr double CRATER_MAX_NEST = 2.0;
} // namespace

TerrainTilesCPU::Params TerrainTilesCPU::ParamsFor(const GeoSphere *sphere, const double vertexSpacing)
{
	Params p;
	const Terrain *terrain = sphere->GetTerrain();
	p.radius = sphere->GetSystemBody()->GetRadius();
	// as TerrainTiles::BandIndex and TerrainTiles::Bake, near the ground (the band set by the spacing alone)
	const double ladderBase = sphere->GetLegacyDeepestSpacing() * 0.5;
	const int band = int(std::round(std::log2(std::max(2.0 * vertexSpacing / ladderBase, 1.0))));
	p.finest = band - (TerrainTiles::BAND_OCTAVES - 1);
	p.finestWave = ladderBase * std::pow(2.0, double(p.finest));
	const float *style = sphere->GetTileStyle();
	for (int i = 0; i < 4; i++)
		p.style[i] = style[i];
	p.band = TerrainTiles::BAND_OCTAVES;
	p.count = TerrainTiles::BAND_OCTAVES;
	p.craterTop = terrain->GetTileCraterTop();
	p.landformTop = terrain->GetTileLandformTop();
	p.blocks = terrain->GetTileBlocks();
	if (style[3] > 0.0f || p.landformTop > 0.0) {
		const double largest = std::max(style[3] > 0.0f ? (p.craterTop > 0.0 ? p.craterTop : p.radius * TerrainTiles::CRATER_TOP_PER_RADIUS) : 0.0, p.landformTop);
		const int top = int(std::floor(std::log2(std::max(largest / ladderBase, 1.0))));
		p.count = std::min(std::max(top - p.finest + 1, TerrainTiles::BAND_OCTAVES), TerrainTiles::MAX_OCTAVES);
	}
	return p;
}

TerrainTilesCPU::Result TerrainTilesCPU::Evaluate(const Params &p, const vector3d &up, const double minWavelength, const bool allCraterCells)
{
	Result result;
	const v3 position = up * p.radius;
	v3 grad(0.0), accumulated(0.0);
	double form = 0.0, formWeight = 0.0, nest = 0.0;
	const bool variedCraters = p.craterTop > 0.0;
	double craterDensity = 0.35 * std::min(double(p.style[3]), 1.0);
	if (variedCraters && p.style[3] > 0.0f)
		craterDensity *= Mix(0.05, 0.95, TileCraterRegion(up));
	for (int i = p.count - 1; i >= 0; --i) {
		const double lambda = p.finestWave * std::pow(2.0, double(i));
		// finer ones from here on
		if (lambda < minWavelength * 0.99)
			break;
		const v3 q = Mod(position / lambda, 256.0);
		const double salt = double(p.finest + i);
		const double w = i == 0 ? 0.6 : 1.0;
		Octave o;
		bool added = false;
		if (p.landformTop > 0.0 ? lambda <= p.landformTop * 1.01 : (i < p.band && (p.craterTop <= 0.0 || lambda <= p.craterTop * 1.01))) {
			const auto add = [&o](const double weight, const Octave &s) {
				o.g += s.g * weight;
				o.form += s.form * weight;
			};
			if (p.style[0] > 0.0f)
				add(p.style[0], TileRolling(q, salt, accumulated));
			if (p.style[1] > 0.0f)
				add(p.style[1], TileRidged(q, salt));
			if (p.style[2] > 0.0f)
				add(p.style[2], TileDunes(q, salt));
			if (p.blocks > 0.0)
				add(p.blocks, TileBlocks(q, salt));
			added = true;
		}
		if (p.style[3] > 0.0f && (p.craterTop <= 0.0 || lambda <= p.craterTop * 1.01)) {
			const double allowed = 1.0 - SmoothStep(CRATER_MAX_NEST - 0.75, CRATER_MAX_NEST, nest);
			if (allowed > 0.0) {
				double inside;
				const Octave c = TileCraters(q, salt, craterDensity, variedCraters, inside, allCraterCells);
				o.g += c.g * allowed;
				o.form += c.form * allowed;
				nest += allowed * inside;
				added = true;
			}
		}
		if (added)
			result.octaves++;
		grad += o.g * w;
		if (i < p.band) {
			const double scale = std::pow(2.0, double(p.band - 1 - i));
			form += w * o.form / scale;
			formWeight += w / scale;
		}
	}
	result.slope = grad - up * grad.Dot(up);
	result.form = std::clamp(form / std::max(formWeight, 1e-6), -1.0, 1.0);
	return result;
}
