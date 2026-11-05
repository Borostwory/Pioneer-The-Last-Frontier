// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainPhysicalPlates.h"

#include "MathUtil.h"

#include "../Random.h"

#include <algorithm>
#include <cmath>

namespace {
	// plates further than this past the nearest have no band here
	constexpr double BAND_REACH = 0.45;

	vector3d RandomDirection(Random &rng)
	{
		const double x = rng.Normal(), y = rng.Normal(), z = rng.Normal();
		const vector3d v(x, y, z);
		const double length = v.Length();
		return length > 1e-9 ? v * (1.0 / length) : vector3d(0.0, 1.0, 0.0);
	}

	double Smooth01(const double t)
	{
		return t * t * (3.0 - 2.0 * t);
	}

	double Clamp01(const double x)
	{
		return std::clamp(x, 0.0, 1.0);
	}

	// a line that wanders along a smooth field
	std::vector<vector3d> Walk(Random &rng, const vector3d &start, const vector3d &axis, const double generation, const int steps, const double step, const double wander)
	{
		auto heading = [&](const vector3d &p) {
			return axis + vector3d(std::sin(3.0 * p.y + generation), std::cos(2.0 * p.x), std::sin(2.0 * p.z - generation)) * 0.6;
		};
		vector3d p = start.Normalized();
		std::vector<vector3d> points;
		points.reserve(steps + 1);
		points.push_back(p);
		vector3d t = p.Cross(heading(p));
		t = t.Length() > 1e-6 ? t.Normalized() : RandomDirection(rng);
		for (int i = 0; i < steps; i++) {
			vector3d f = p.Cross(heading(p));
			const double n = f.Length();
			if (n > 1e-6) {
				f = f * (1.0 / n);
				if (f.Dot(t) < 0.0)
					f = -f;
				t = t * 0.75 + f * 0.25;
			}
			const double wx = rng.Normal(), wy = rng.Normal(), wz = rng.Normal();
			t = t + vector3d(wx, wy, wz) * wander;
			t = t - p * t.Dot(p);
			t = t * (1.0 / std::max(t.Length(), 1e-9));
			p = (p + t * step).Normalized();
			points.push_back(p);
		}
		return points;
	}
} // namespace

void TerrainPlates::Build(const uint32_t seed)
{
	Random rng(seed ^ 0xA5A6u);
	m_count = 8 + int(rng.Int32(8));
	m_seeds.resize(m_count);
	m_continental.resize(m_count);
	for (int i = 0; i < m_count; i++) {
		m_seeds[i] = RandomDirection(rng);
		m_continental[i] = rng.Double() < 0.45 ? 1 : 0;
	}

	// each boundary converges, spreads, or slides past (and builds little)
	const int pairs = m_count * m_count;
	m_converging.assign(pairs, 0);
	m_diverging.assign(pairs, 0);
	m_amplitude.assign(pairs, 0.f);
	m_offset.assign(pairs, 0.f);
	for (int i = 0; i < m_count; i++) {
		for (int j = i + 1; j < m_count; j++) {
			const double kind = rng.Double();
			const double amplitude = 0.3 + 0.7 * rng.Double();
			const double offset = 0.02 + 0.12 * rng.Double();
			const char converging = kind > 0.62 ? 1 : 0;
			const char diverging = (kind > 0.28 && kind <= 0.62) ? 1 : 0;
			for (const int k : { i * m_count + j, j * m_count + i }) {
				m_converging[k] = converging;
				m_diverging[k] = diverging;
				m_amplitude[k] = float(amplitude);
				m_offset[k] = float(offset);
			}
		}
	}

	auto addLine = [&](std::vector<Line> &to, const int generation, const int steps, const double step, const double wander, const double height, const double width) {
		const vector3d axis = RandomDirection(rng);
		const vector3d start = RandomDirection(rng);
		Line line;
		line.points = Walk(rng, start, axis, double(generation), steps, step, wander);
		vector3d sum(0.0, 0.0, 0.0);
		for (const vector3d &q : line.points)
			sum += q;
		line.centre = sum.Normalized();
		double furthest = 1.0;
		for (const vector3d &q : line.points)
			furthest = std::min(furthest, q.Dot(line.centre));
		line.cosReach = std::cos(std::min(std::acos(std::clamp(furthest, -1.0, 1.0)) + width * 2.2, M_PI));
		line.height = height;
		line.width = width;
		to.push_back(std::move(line));
	};
	// old worn ranges across the interiors
	const int ranges = 5 + int(rng.Int32(5));
	for (int g = 0; g < ranges; g++)
		addLine(m_ranges, g, 8 + int(rng.Int32(16)), 0.05, 0.2, 0.12 + 0.2 * rng.Double(), 0.035 + 0.035 * rng.Double());
	// rift valleys, where lakes gather
	const int rifts = 2 + int(rng.Int32(3));
	for (int g = 0; g < rifts; g++)
		addLine(m_rifts, g + 20, 6 + int(rng.Int32(10)), 0.04, 0.12, 0.1 + 0.12 * rng.Double(), 0.012 + 0.013 * rng.Double());
	const int plateaus = 2 + int(rng.Int32(3));
	for (int i = 0; i < plateaus; i++) {
		Plateau plateau;
		plateau.centre = RandomDirection(rng);
		plateau.radius = 0.2 + 0.22 * rng.Double();
		plateau.height = 0.14 + 0.16 * rng.Double();
		m_plateaus.push_back(plateau);
	}
}

TerrainPlates::Bands TerrainPlates::At(const vector3d &p, const double warp) const
{
	Bands out;
	const vector3d q = (p + vector3d(warp, warp, warp) * 0.22).Normalized();

	// distances, and the nearest three plates
	double d[32];
	int i1 = 0, i2 = 0, i3 = 0;
	double d1 = 1e9, d2 = 1e9, d3 = 1e9;
	double weightSum = 0.0, continent = 0.0;
	for (int i = 0; i < m_count; i++) {
		d[i] = std::acos(std::clamp(q.Dot(m_seeds[i]), -1.0, 1.0));
		const double w = std::exp(-d[i] * 4.5);
		weightSum += w;
		if (m_continental[i])
			continent += w;
		if (d[i] < d1) {
			d3 = d2;
			i3 = i2;
			d2 = d1;
			i2 = i1;
			d1 = d[i];
			i1 = i;
		} else if (d[i] < d2) {
			d3 = d2;
			i3 = i2;
			d2 = d[i];
			i2 = i;
		} else if (d[i] < d3) {
			d3 = d[i];
			i3 = i;
		}
	}
	(void)i3;
	out.continent = continent / weightSum;

	const bool c1 = m_continental[i1] != 0;
	const int row = i1 * m_count;
	for (int j = 0; j < m_count; j++) {
		if (j == i1)
			continue;
		const double gap = d[j] - d1;
		if (gap > BAND_REACH)
			continue;
		const bool conv = m_converging[row + j] != 0;
		const bool div = m_diverging[row + j] != 0;
		if (!conv && !div)
			continue;
		const bool c2 = m_continental[j] != 0;
		const double amp = m_amplitude[row + j], off = m_offset[row + j];
		auto band = [gap](const double at, const double wide) {
			const double e = (gap - at) / wide;
			return std::exp(-e * e);
		};
		// The ground must not jump where the nearest plate changes. A band on one plate only
		// reaches zero at its own boundary with the other plate...
		const double start = Smooth01(Clamp01(gap / 0.015));
		// ...and a band shared with this plate fades out where a third plate takes over as the
		// nearest: past that boundary the pair is a different one, with its own kind and strength
		const double pair = Smooth01(Clamp01((j == i2 ? d3 - d1 : d2 - d1) / 0.03));
		if (conv) {
			if (c1 && c2) {
				out.collision = std::max(out.collision, band(off + 0.05, 0.095) * pair * amp);
			} else if (c1 && !c2) {
				out.coastRange = std::max(out.coastRange, band(0.035, 0.03) * start * pair * amp);
				out.arc = std::max(out.arc, band(0.085 + 0.4 * off, 0.022) * pair * amp);
			} else if (!c1 && c2) {
				// the trench is on the sinking plate, just off the coast
				out.trench = std::max(out.trench, band(0.02, 0.018) * start * pair);
			} else if (i1 < j) {
				out.islandArc = std::max(out.islandArc, band(0.06, 0.018) * pair);
			} else {
				out.trench = std::max(out.trench, band(0.02, 0.016) * start * pair);
			}
		} else if (!c1 && !c2) {
			out.ridge = std::max(out.ridge, band(0.0, 0.07) * pair);
			out.ridgeRift = std::max(out.ridgeRift, band(0.0, 0.012) * pair);
		} else if (c1 && c2) {
			out.rift = std::max(out.rift, band(0.0, 0.022) * pair);
		}
	}
	return out;
}

double TerrainPlates::Along(const std::vector<Line> &lines, const vector3d &p) const
{
	double sum = 0.0;
	for (const Line &line : lines) {
		if (p.Dot(line.centre) < line.cosReach)
			continue;
		const int n = int(line.points.size()) - 1;
		double best = 0.0;
		for (int i = 0; i < n; i++) {
			const vector3d &a = line.points[i], &b = line.points[i + 1];
			const vector3d ab = b - a;
			const double t = Clamp01((p - a).Dot(ab) / std::max(ab.Dot(ab), 1e-12));
			const double dist = (p - (a + ab * t)).Length() / line.width;
			if (dist > 2.3)
				continue;
			const double along = (i + t) / std::max(n, 1);
			const double fade = std::min(1.0, std::min(along, 1.0 - along) * 5.0 + 0.1);
			// reaches exactly zero at its edge, or it leaves a straight step in the ground
			best = std::max(best, std::max(std::exp(-dist * dist) - 0.008, 0.0) / 0.992 * fade);
		}
		sum += line.height * best;
	}
	return sum;
}
