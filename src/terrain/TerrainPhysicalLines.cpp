// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainPhysicalLines.h"

#include "MathUtil.h"
#include "SphereCells.h"
#include "TerrainNoise.h"

#include "../Random.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

namespace {
	constexpr double RADIUS_KM = 6378.135;
	// A line is at least as wide as the samples can show, so it stays in view from afar. How far that goes is
	// limited by how far around it a large line is listed.
	constexpr double LARGE_MARGIN = 0.09; // on the unit sphere
	constexpr double FADE_FROM = 0.03, FADE_WIDTH = 0.03;
	constexpr double WIDEST_SPACING = FADE_FROM + FADE_WIDTH;
	constexpr double REACH = 1.8; // in widths
	constexpr double SCARP_LOBES = 0.006; // on the unit sphere
	// a fault valley: how much of its half-width a wall takes, how high its rim is for its depth, how far out that goes
	constexpr double GRABEN_WALL = 0.5, GRABEN_RIM = 0.1, GRABEN_RIM_REACH = 0.7;
	// the spacing where each tier starts to fade out, and is gone; REACH * 1.6 * that around it is how far it's listed
	constexpr double TIER_FROM[3] = { FADE_FROM, 0.006, 0.003 };
	constexpr double TIER_GONE[3] = { WIDEST_SPACING, 0.0104, 0.00625 };
	constexpr double TIER_MARGIN[3] = { LARGE_MARGIN, 0.03, 0.018 };
	constexpr int TIER_ROWS[3] = { 96, 192, 360 };
	constexpr double FRECKLE_GONE = 0.008; // the spacing beyond which a freckle doesn't show

	// beside a line, at a distance that changes slowly: a line and its partner, as Europa's often run in pairs
	std::vector<vector3d> Beside(const std::vector<vector3d> &line, const double distance, const double seed)
	{
		std::vector<vector3d> out;
		out.reserve(line.size());
		for (size_t i = 0; i < line.size(); i++) {
			const vector3d &p = line[i];
			const vector3d t = (line[std::min(i + 1, line.size() - 1)] - line[i > 0 ? i - 1 : 0]);
			const vector3d side = p.Cross(t).Normalized();
			const double along = double(i) / double(std::max<size_t>(line.size() - 1, 1));
			out.push_back((p + side * (distance * (1.0 + 0.4 * std::sin(along * 7.0 + seed)))).Normalized());
		}
		return out;
	}

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

	double Between(Random &rng, const double low, const double high)
	{
		return low + (high - low) * rng.Double();
	}

	using Stress = std::function<vector3d(const vector3d &)>;

	// the direction of the stress, turning from place to place and from one generation to the next
	Stress MakeStress(Random &rng, const int generation)
	{
		const vector3d a = RandomDirection(rng);
		const double g = double(generation);
		return [a, g](const vector3d &p) {
			return a + vector3d(std::sin(3.0 * p.y + g), std::cos(2.0 * p.x), std::sin(2.0 * p.z - g)) * 0.5;
		};
	}

	// a crack that wanders along the stress
	std::vector<vector3d> Walk(Random &rng, const vector3d &start, const Stress &stress, const int steps, const double step, const double wander, const double follow = 0.25)
	{
		std::vector<vector3d> out;
		vector3d p = start.Normalized();
		out.push_back(p);
		vector3d t = p.Cross(stress(p));
		t = t.Length() > 1e-6 ? t.Normalized() : p.Cross(RandomDirection(rng)).Normalized();
		for (int i = 0; i < steps; i++) {
			vector3d f = p.Cross(stress(p));
			if (f.Length() > 1e-6) {
				f = f.Normalized();
				if (f.Dot(t) < 0.0)
					f = -f;
				t = t * (1.0 - follow) + f * follow;
			}
			t = t + vector3d(RandomNormal(rng), RandomNormal(rng), RandomNormal(rng)) * wander;
			t = t - p * t.Dot(p);
			if (t.Length() < 1e-9)
				t = p.Cross(RandomDirection(rng));
			t = t.Normalized();
			p = (p + t * step).Normalized();
			out.push_back(p);
		}
		return out;
	}

	// a chain of arcs with sharp cusps between them, as Europa's cycloids
	std::vector<vector3d> Cycloid(Random &rng, const vector3d &start, const int arcs, const double arcLength)
	{
		std::vector<vector3d> out;
		vector3d p = start.Normalized();
		out.push_back(p);
		vector3d t = p.Cross(RandomDirection(rng)).Normalized();
		const double side = rng.Double() < 0.5 ? 1.0 : -1.0;
		constexpr int STEPS = 8;
		const double step = arcLength / STEPS;
		for (int arc = 0; arc < arcs; arc++) {
			for (int i = 0; i < STEPS; i++) {
				// bends one way along each arc...
				const double bend = side * 0.22;
				t = (t * std::cos(bend) + p.Cross(t) * std::sin(bend)).Normalized();
				p = (p + t * step).Normalized();
				t = (t - p * t.Dot(p)).Normalized();
				out.push_back(p);
			}
			// ...and turns sharply back at each cusp
			const double cusp = -side * Between(rng, 1.9, 2.3);
			t = (t * std::cos(cusp) + p.Cross(t) * std::sin(cusp)).Normalized();
		}
		return out;
	}
} // namespace

void TerrainLines::Index::Resize(const int r, const int c)
{
	rows = r;
	columns = c;
	cells.resize(size_t(r) * c);
}

void TerrainLines::Index::Add(const vector3d &a, const vector3d &b, const double margin, const uint32_t id)
{
	const double rowSize = M_PI / rows, columnSize = 2.0 * M_PI / columns;
	const double length = (b - a).Length();
	const int samples = std::max(1, int(std::ceil(length / (0.5 * rowSize))));
	for (int s = 0; s <= samples; s++) {
		const vector3d q = (a + (b - a) * (double(s) / samples)).Normalized();
		const double lat = std::asin(std::clamp(q.y, -1.0, 1.0));
		const int row = int((lat + M_PI / 2.0) / rowSize);
		const int column = int((std::atan2(q.z, q.x) + M_PI) / columnSize);
		const int reachRows = int(margin / rowSize) + 1;
		for (int r = std::max(row - reachRows, 0); r <= std::min(row + reachRows, rows - 1); r++) {
			// (the columns get narrower towards the poles)
			const double rowLat = (r + 0.5) * rowSize - M_PI / 2.0;
			const double width = columnSize * std::max(std::cos(rowLat), 1e-3);
			const int reachColumns = std::min(int(margin / width) + 1, columns / 2);
			for (int c = column - reachColumns; c <= column + reachColumns; c++) {
				std::vector<uint32_t> &cell = cells[size_t(r) * columns + ((c % columns) + columns) % columns];
				if (cell.empty() || cell.back() != id)
					cell.push_back(id);
			}
		}
	}
}

const std::vector<uint32_t> *TerrainLines::Index::At(const vector3d &p) const
{
	if (cells.empty())
		return nullptr;
	const int r = std::clamp(int((std::asin(std::clamp(p.y, -1.0, 1.0)) + M_PI / 2.0) / M_PI * rows), 0, rows - 1);
	const int c = std::clamp(int((std::atan2(p.z, p.x) + M_PI) / (2.0 * M_PI) * columns), 0, columns - 1);
	return &cells[size_t(r) * columns + c];
}

void TerrainLines::AddLine(const std::vector<vector3d> &points, const Kind kind, const int generation, const double width, const double height, const double tone, const Tier tier)
{
	if (points.size() < 2)
		return;
	const int line = int(m_lines.size());
	const int pieces = int(points.size()) - 1;
	m_lines.push_back({ kind, int8_t(generation), float(width), float(height), float(pieces), float(tone) });
	const double margin = TIER_MARGIN[tier] + 2.0 * width;
	Index &index = m_index[tier];
	double length = 0.0;
	for (int i = 0; i < pieces; i++) {
		const uint32_t id = uint32_t(m_pieces.size());
		m_pieces.push_back({ points[i], points[i + 1], std::max((points[i + 1] - points[i]).LengthSqr(), 1e-16), line, float(i) });
		index.Add(points[i], points[i + 1], margin, id);
		length += (points[i + 1] - points[i]).Length();
	}
	if (tier == LARGE)
		m_middles.push_back({ points[points.size() / 2], length });
	else
		m_fineCount++;
	if (kind == FRACTURE || kind == GRABEN || kind == BAND)
		m_deepest = std::max(m_deepest, height);
	else
		m_tallest = std::max(m_tallest, height);
	if (kind == GRABEN)
		m_tallest = std::max(m_tallest, GRABEN_RIM * height);
	if (kind == SCARP || kind == GRABEN)
		m_lobed = true;
}

void TerrainLines::BuildActiveShell(const PlanetPhysics::Core &core, const double strength, const uint32_t seed)
{
	const double radiusKm = core.radius * RADIUS_KM;
	const auto widthOf = [radiusKm](const double km) { return std::min(km / radiusKm, 0.015); };
	Random rng(seed & 0x7fffffffu);
	m_index[MEDIUM].Resize(TIER_ROWS[MEDIUM], 2 * TIER_ROWS[MEDIUM]);
	m_index[MESH].Resize(TIER_ROWS[MESH], 2 * TIER_ROWS[MESH]);
	m_stainAxis = RandomDirection(rng);

	const Stress fields[3] = { MakeStress(rng, 0), MakeStress(rng, 1), MakeStress(rng, 2) };
	// A long smooth arc, with hardly any wander: pieces 30 km long, or less on a small moon. It only leans towards the
	// stress: what follows it closely goes round in a small circle.
	const double step = std::min(30.0 / radiusKm, 0.03);
	const auto arc = [&](const int generation, const double km, const double wander) {
		return Walk(rng, RandomDirection(rng), fields[generation], std::max(int(km / radiusKm / step), 3), step, wander, 0.035);
	};

	// the strong reddish lines, bands 10 to 30 km wide
	const int strong = int(30.0 + 25.0 * strength);
	for (int i = 0; i < strong; i++) {
		const int generation = i % 3;
		const std::vector<vector3d> points = arc(generation, Between(rng, 1500.0, 5000.0), 0.012);
		if (rng.Double() < 0.5) {
			const double width = widthOf(Between(rng, 4.5, 13.0));
			const double height = Between(rng, 100.0, 400.0);
			AddLine(points, BAND, generation, width, height, 1.0, LARGE);
			if (rng.Double() < 0.35)
				AddLine(Beside(points, Between(rng, 3.0, 8.0) * width, rng.Double() * 6.0), BAND, generation, 0.6 * width, height, 0.8, LARGE);
		} else {
			// a ridge of several strands, on coloured ground
			AddLine(points, BAND, generation, widthOf(Between(rng, 5.0, 10.0)), Between(rng, 50.0, 110.0), 0.6, LARGE);
			const double width = widthOf(Between(rng, 0.6, 1.6));
			const double separation = width * Between(rng, 2.4, 3.0);
			const double wobble = rng.Double() * 6.0;
			const int strands = 2 + int(rng.Int32(4));
			for (int k = 0; k < strands; k++)
				AddLine(Beside(points, (double(k) - 0.5 * double(strands - 1)) * separation, wobble), RIDGE, generation + 1, width, Between(rng, 150.0, 300.0), 0.9, LARGE);
		}
	}

	// shorter dark bands with sharp sides, wedges that open from one end
	const int wedges = int(20.0 + 30.0 * strength);
	for (int i = 0; i < wedges; i++) {
		const int generation = i % 3;
		AddLine(arc(generation, Between(rng, 80.0, 500.0), 0.05), BAND, generation, widthOf(Between(rng, 3.0, 9.0)), Between(rng, 100.0, 300.0), 0.9, MEDIUM);
		m_lines.back().wedge = true;
	}

	// the thinner ones, double ridges 1 to 3 km across and fractures, some in pairs, with short branches
	const int thin = int(200.0 + 250.0 * strength);
	for (int i = 0; i < thin; i++) {
		const int generation = i % 3;
		if (rng.Double() < 0.1) {
			AddLine(Cycloid(rng, RandomDirection(rng), 6 + int(rng.Int32(9)), Between(rng, 100.0, 300.0) / radiusKm), RIDGE, generation, widthOf(Between(rng, 0.6, 1.6)), Between(rng, 100.0, 300.0), 0.6, MEDIUM);
			continue;
		}
		const std::vector<vector3d> points = arc(generation, Between(rng, 500.0, 3000.0), 0.015);
		const bool ridge = rng.Double() < 0.7;
		const double width = ridge ? widthOf(Between(rng, 0.6, 1.6)) : widthOf(Between(rng, 0.2, 1.0));
		const double height = ridge ? Between(rng, 100.0, 300.0) : Between(rng, 100.0, 450.0);
		AddLine(points, ridge ? RIDGE : FRACTURE, generation, width, height, 0.6, MEDIUM);
		if (ridge && rng.Double() < 0.6) {
			const double separation = width * Between(rng, 2.4, 3.0);
			const double wobble = rng.Double() * 6.0;
			const int extra = 1 + int(rng.Int32(2));
			for (int k = 1; k <= extra; k++)
				AddLine(Beside(points, double(k) * separation, wobble), RIDGE, generation, width, height * Between(rng, 0.7, 1.05), 0.55, MEDIUM);
		}
		if (rng.Double() < 0.3)
			AddLine(Beside(points, Between(rng, 8.0, 30.0) / radiusKm, rng.Double() * 6.0), ridge ? RIDGE : FRACTURE, generation, width, 0.7 * height, 0.45, MEDIUM);
		// short lines only as branches off a long one
		if (rng.Double() < 0.5) {
			const vector3d &from = points[rng.Int32(int(points.size()))];
			const double km = Between(rng, 60.0, 250.0);
			AddLine(Walk(rng, from, fields[(generation + 1) % 3], std::max(int(km / radiusKm / step), 3), step, 0.05), FRACTURE, generation - 1, widthOf(Between(rng, 0.05, 0.4)), Between(rng, 60.0, 200.0), 0.3, MESH);
		}
	}

	// the faint mesh: long, thin and hardly darker than the ice
	const int mesh = int(800.0 + 1200.0 * strength);
	for (int i = 0; i < mesh; i++) {
		const int generation = i % 3;
		const std::vector<vector3d> points = arc(generation, Between(rng, 300.0, 2000.0), 0.02);
		const bool ridge = rng.Double() < 0.4;
		AddLine(points, ridge ? RIDGE : FRACTURE, generation - 3, ridge ? widthOf(Between(rng, 0.2, 0.6)) : widthOf(Between(rng, 0.05, 0.4)), ridge ? Between(rng, 40.0, 130.0) : Between(rng, 60.0, 250.0), 0.28, MESH);
	}

	// Deep cracks, the youngest of all, branching where they meet. Their walls are about 55 degrees, as steep as the
	// mesh draws well, so they are wider than a real one this deep. Mostly rust, some blue ice.
	const int fractures = 20 + int(30.0 * strength);
	for (int i = 0; i < fractures; i++) {
		const double depth = Between(rng, 1500.0, 3000.0);
		const double km = Between(rng, 500.0, 2000.0);
		const bool blue = rng.Double() < 0.25;
		const std::vector<vector3d> points = Walk(rng, RandomDirection(rng), fields[i % 3], std::max(int(km / radiusKm / step), 3), step, 0.02, 0.05);
		AddLine(points, FRACTURE, 3, widthOf(depth / 1000.0 / 1.4), depth, 1.0, LARGE);
		m_lines.back().blue = blue;
		const int branches = 2 + int(rng.Int32(3));
		for (int k = 0; k < branches; k++) {
			const double branchDepth = depth * Between(rng, 0.3, 0.55);
			const double branchKm = Between(rng, 150.0, 600.0);
			AddLine(Walk(rng, points[rng.Int32(int(points.size()))], fields[(i + 1 + k) % 3], std::max(int(branchKm / radiusKm / step), 3), step, 0.04, 0.1), FRACTURE, 3, widthOf(branchDepth / 1000.0 / 1.4), branchDepth, 0.8, LARGE);
			m_lines.back().blue = blue;
		}
	}

	// small dark reddish freckles, in clusters, and a few low pale domes
	m_domeIndex.Resize(TIER_ROWS[MEDIUM], 2 * TIER_ROWS[MEDIUM]);
	const auto addSpot = [&](const vector3d &centre, const double km, const double height, const bool freckle) {
		Dome dome;
		dome.centre = centre;
		dome.radius = km / radiusKm;
		const double reach = 1.5 * std::max(dome.radius, 1.2 * FRECKLE_GONE);
		dome.cosReach = std::cos(std::min(reach, M_PI));
		dome.height = height;
		dome.freckle = freckle;
		m_domeIndex.Add(centre, centre, reach, uint32_t(m_domes.size()));
		m_domes.push_back(dome);
	};
	const int clusters = int(30.0 + 40.0 * strength);
	for (int c = 0; c < clusters; c++) {
		const vector3d middle = RandomDirection(rng);
		const double spread = Between(rng, 0.04, 0.16);
		const int count = 15 + int(rng.Int32(70));
		for (int i = 0; i < count; i++) {
			const vector3d shift(RandomNormal(rng), RandomNormal(rng), RandomNormal(rng));
			addSpot((middle + shift * spread).Normalized(), Between(rng, 2.5, 7.5), Between(rng, 30.0, 120.0), true);
		}
	}
	const int domes = int(10.0 + 25.0 * strength);
	for (int i = 0; i < domes; i++)
		addSpot(RandomDirection(rng), Between(rng, 4.0, 14.0), Between(rng, 100.0, 320.0), false);
}

// Around one pole, a few long parallel troughs with a ridge along each side and blue ice in them, forking at their ends,
// as Enceladus's tiger stripes, 130 km long and 35 km apart there; many ridges between and around them; and a scalloped
// margin of ridges with sharp cusps. Only a few long cracks cross the old ground elsewhere.
void TerrainLines::BuildPolarShell(const PlanetPhysics::Core &core, const double strength, const uint32_t seed)
{
	const double radiusKm = core.radius * RADIUS_KM;
	const auto widthOf = [radiusKm](const double km) { return std::min(km / radiusKm, 0.015); };
	Random rng(seed & 0x7fffffffu);
	m_index[MEDIUM].Resize(TIER_ROWS[MEDIUM], 2 * TIER_ROWS[MEDIUM]);
	m_index[MESH].Resize(TIER_ROWS[MESH], 2 * TIER_ROWS[MESH]);
	m_polar = true;
	const Stress fields[3] = { MakeStress(rng, 0), MakeStress(rng, 1), MakeStress(rng, 2) };
	m_pole = vector3d(0.0, rng.Double() < 0.75 ? -1.0 : 1.0, 0.0);
	const vector3d e1 = RandomDirection(rng).Cross(m_pole).Normalized();
	const vector3d e2 = m_pole.Cross(e1);

	// no two alike: uneven spacing and lengths, winding, some hooked at an end, each with its own forks
	const int stripes = rng.Double() < 0.3 ? 5 : 4;
	const double bow = Between(rng, 0.1, 0.3);
	double offset = -0.5 * double(stripes - 1) * 0.14;
	for (int k = 0; k < stripes; k++) {
		const double middle = Between(rng, -0.07, 0.07);
		const double reach = Between(rng, 0.36, 0.64);
		const double wobble = 6.28 * rng.Double(), wobble2 = 6.28 * rng.Double();
		const double hookAt = rng.Double() < 0.5 ? 1.0 : -1.0, hook = Between(rng, -0.5, 0.5);
		std::vector<vector3d> points;
		constexpr int PIECES = 32;
		for (int i = 0; i <= PIECES; i++) {
			const double u = double(i) / PIECES - 0.5;
			const double s = middle + u * reach;
			const double late = std::max(2.0 * u * hookAt - 0.6, 0.0);
			const double wind = 0.016 * std::sin(8.0 * s + wobble) + 0.007 * std::sin(23.0 * s + wobble2);
			points.push_back((m_pole + e1 * s + e2 * (offset + bow * s * s + wind + hook * late * late)).Normalized());
		}
		AddLine(points, RIDGE, 1, widthOf(Between(rng, 2.2, 3.4)), Between(rng, 120.0, 220.0), 0.6, LARGE);
		AddLine(points, FRACTURE, 3, widthOf(Between(rng, 1.1, 1.6)), Between(rng, 700.0, 1000.0), 1.0, LARGE);
		m_lines.back().blue = true;
		for (int end = 0; end < 2; end++) {
			const vector3d &tip = end == 0 ? points.front() : points.back();
			const vector3d &before = end == 0 ? points[2] : points[PIECES - 2];
			const vector3d onward = (tip - before).Normalized();
			const int forks = int(rng.Int32(3));
			for (int f = 0; f < forks; f++) {
				const vector3d heading = onward + e2 * Between(rng, -1.0, 1.0);
				const int pieces = 3 + int(rng.Int32(7));
				const double bend = Between(rng, -0.0015, 0.0015);
				std::vector<vector3d> fork;
				for (int i = 0; i <= pieces; i++)
					fork.push_back((tip + heading * (0.012 * i) + e2 * (bend * i * i)).Normalized());
				AddLine(fork, FRACTURE, 3, widthOf(Between(rng, 0.5, 0.9)), Between(rng, 250.0, 500.0), 0.7, LARGE);
				m_lines.back().blue = true;
			}
		}
		offset += Between(rng, 0.09, 0.19);
	}

	// ridges over all the terrain around them
	const int ridges = int((40.0 + 40.0 * strength));
	for (int i = 0; i < ridges; i++) {
		const double away = 0.5 * rng.Double(), turn = 6.28 * rng.Double();
		const vector3d from = (m_pole + (e1 * std::cos(turn) + e2 * std::sin(turn)) * away).Normalized();
		AddLine(Walk(rng, from, fields[i % 3], int(Between(rng, 0.12, 0.5) / 0.012), 0.012, 0.05, 0.035), RIDGE, i % 3, widthOf(Between(rng, 0.5, 1.2)), Between(rng, 60.0, 160.0), 0.4, MEDIUM);
	}

	// Its margin: low scallops of uneven width with sharp cusps, at a distance from the pole that wanders, with gaps,
	// and only here and there a second strand
	const double wander1 = 6.28 * rng.Double(), wander2 = 6.28 * rng.Double();
	const auto marginAngle = [&](const double around) { return 0.58 + 0.05 * std::sin(around + wander1) + 0.025 * std::sin(3.0 * around + wander2); };
	double around = 6.28 * rng.Double();
	const double last = around + 2.0 * M_PI;
	while (around < last - 0.2) {
		const double wide = std::min(Between(rng, 0.45, 1.4), last - around);
		if (rng.Double() < 0.75) {
			const double bulge = Between(rng, 0.03, 0.09) * wide;
			std::vector<vector3d> points;
			constexpr int PIECES = 20;
			for (int i = 0; i <= PIECES; i++) {
				const double t = double(i) / PIECES;
				const double at = around + t * wide;
				const double angle = marginAngle(at) + bulge * (1.0 - (2.0 * t - 1.0) * (2.0 * t - 1.0));
				points.push_back(m_pole * std::cos(angle) + (e1 * std::cos(at) + e2 * std::sin(at)) * std::sin(angle));
			}
			const double height = Between(rng, 50.0, 130.0), width = widthOf(Between(rng, 1.0, 2.2));
			AddLine(points, RIDGE, 2, width, height, 0.35, LARGE);
			if (rng.Double() < 0.3)
				AddLine(Beside(points, Between(rng, 0.012, 0.03), rng.Double() * 6.0), RIDGE, 2, 0.8 * width, 0.7 * height, 0.3, LARGE);
		}
		around += wide;
	}

	// a few long cracks elsewhere
	const double step = 0.03;
	const int cracks = 12 + int(10.0 * strength);
	for (int i = 0; i < cracks; i++) {
		const bool ridge = rng.Double() < 0.5;
		AddLine(Walk(rng, RandomDirection(rng), fields[i % 3], int(Between(rng, 0.8, 2.6) / step), step, 0.015, 0.035), ridge ? RIDGE : FRACTURE, i % 3, widthOf(Between(rng, 0.5, 1.5)), Between(rng, 80.0, 300.0), 0.45, MEDIUM);
	}
	const int deep = 4 + int(4.0 * strength);
	for (int i = 0; i < deep; i++) {
		AddLine(Walk(rng, RandomDirection(rng), fields[i % 3], int(Between(rng, 1.0, 2.0) / step), step, 0.02, 0.05), FRACTURE, 3, widthOf(Between(rng, 0.8, 1.5)), Between(rng, 500.0, 900.0), 0.7, LARGE);
		m_lines.back().blue = true;
	}
	const int mesh = 50 + int(50.0 * strength);
	for (int i = 0; i < mesh; i++)
		AddLine(Walk(rng, RandomDirection(rng), fields[i % 3], int(Between(rng, 0.3, 1.2) / step), step, 0.02, 0.035), FRACTURE, i % 3 - 3, widthOf(Between(rng, 0.1, 0.4)), Between(rng, 40.0, 150.0), 0.25, MESH);
}

void TerrainLines::Build(const PlanetPhysics::Core &core, const PlanetPhysics::Interior &it, const uint32_t noiseSeed, const bool grooved)
{
	m_noiseSeed = noiseSeed;
	if (core.asteroid)
		return;
	const double radiusKm = core.radius * RADIUS_KM;
	const double env = std::max(it.reliefLimit, 1.0);
	// (a line isn't wide for the size of a small moon)
	const auto widthOf = [radiusKm](const double km) { return std::min(km / radiusKm, 0.015); };
	Random rng((core.seed ^ 0x51A5u) & 0x7fffffffu);
	m_index[LARGE].Resize(TIER_ROWS[LARGE], 2 * TIER_ROWS[LARGE]);

	const bool active = it.iceCrust && it.iceActivity >= 0.05;
	m_activeShell = active;
	if (active) {
		if (core.radius < 0.1)
			BuildPolarShell(core, it.shellStrength, core.seed ^ 0x51A6u);
		else
			BuildActiveShell(core, it.shellStrength, core.seed ^ 0x51A6u);
	} else if (it.shellStrength > 0.0 && !grooved) {
		for (int generation = 0; generation < 3; generation++) {
			const Stress stress = MakeStress(rng, generation);
			for (int i = 0; i < 4; i++) {
				const double h = std::min(900.0 * (0.5 + 0.5 * rng.Double()), 0.3 * env);
				const double w = 12.0 * Between(rng, 0.6, 1.6);
				const double length = Between(rng, 0.3, 1.8);
				AddLine(Walk(rng, RandomDirection(rng), stress, int(length / 0.05), 0.05, 0.18), GRABEN, generation, widthOf(w), h, 1.0, LARGE);
			}
		}
	}

	if (it.contraction > 0.05) {
		const Stress stress = MakeStress(rng, 0);
		const int count = int(40.0 * it.contraction) + 4;
		for (int i = 0; i < count; i++) {
			const std::vector<vector3d> points = Walk(rng, RandomDirection(rng), stress, int(Between(rng, 6.0, 22.0)), 0.04, 0.12);
			AddLine(points, SCARP, 0, widthOf(18.0), std::min(1600.0 * it.contraction, 0.15 * env), 1.0, LARGE);
		}
	}

	if (it.iceCrust && it.iceActivity < 0.05 && !it.past && rng.Double() < 0.3) {
		const Stress stress = MakeStress(rng, 2);
		const int count = 2 + int(rng.Int32(5));
		for (int i = 0; i < count; i++) {
			const std::vector<vector3d> points = Walk(rng, RandomDirection(rng), stress, int(Between(rng, 6.0, 24.0)), 0.04, 0.14);
			AddLine(points, GRABEN, 0, widthOf(std::max(radiusKm * 0.012, 4.0)), std::min(env * 0.15, 1500.0), 1.0, LARGE);
		}
	}

	if (it.rift) {
		// Nearly straight, as Ithaca Chasma on Tethys and Valles Marineris on Mars: following the stress would turn a
		// line this long round into a ring. It's wide for a small moon, and younger than the valleys it cuts. It isn't
		// one line: overlapping stretches stepped sideways from each other, of their own widths and depths, with
		// narrower valleys branching off.
		const Stress stress = MakeStress(rng, 1);
		const double length = std::clamp(2000.0 / radiusKm, 1.0, 3.6);
		const std::vector<vector3d> spine = Walk(rng, RandomDirection(rng), stress, int(length / 0.05), 0.05, 0.05, 0.0);
		const double depth = radiusKm < 1000.0 ? Between(rng, 3000.0, 5000.0) : 6000.0;
		const double wide = std::min(radiusKm * 0.095, 120.0) / radiusKm;
		const double deepest = std::min(env * 0.5, depth);
		const int count = int(spine.size());
		const int stretches = 3 + int(rng.Int32(3));
		for (int s = 0; s < stretches; s++) {
			const int from = std::max(int((double(s) - 0.15) / stretches * count), 0);
			const int to = std::min(int((double(s) + 1.15) / stretches * count), count);
			const std::vector<vector3d> stretch(spine.begin() + from, spine.begin() + to);
			AddLine(Beside(stretch, Between(rng, -0.6, 0.6) * wide, rng.Double() * 6.0), GRABEN, 3, wide * Between(rng, 0.7, 1.15), deepest * Between(rng, 0.7, 1.0), 1.0, LARGE);
		}
		const int branches = 6 + int(rng.Int32(5));
		for (int b = 0; b < branches; b++) {
			const Stress away = MakeStress(rng, 2);
			const vector3d &start = spine[rng.Int32(count)];
			AddLine(Walk(rng, start, away, std::max(int(Between(rng, 0.2, 0.7) * length / 0.05 / 3.0), 3), 0.05, 0.08, 0.0), GRABEN, 3, wide * Between(rng, 0.2, 0.4), deepest * Between(rng, 0.25, 0.55), 1.0, LARGE);
		}
	}
}

double TerrainLines::CutReach(const Kind kind)
{
	switch (kind) {
	case RIDGE: return 1.4;
	case BAND: return 1.15;
	case GRABEN: return 1.0 + GRABEN_RIM_REACH;
	default: return 1.8;
	}
}

double TerrainLines::CrackShape(const Nearest &crack, const double spacing)
{
	// steep walls; a deep one is V-shaped, with a narrow floor; at the sample size if they are finer
	const double wall = std::max(crack.height > 800.0 ? 0.85 : 0.5, 1.2 * spacing / crack.width);
	return Soft(1.0 - crack.distance, 0.0, std::min(wall, 1.0));
}

double TerrainLines::GrabenCut(const Nearest &line, const double spacing)
{
	// (walls no finer than the samples can show, or the mesh draws them as stripes)
	const double wall = std::min(std::max(GRABEN_WALL, 2.0 * spacing / line.width), 1.0);
	const double cut = Soft(1.0 - line.distance, 0.0, wall);
	// the walls have slumped into a few terraces, once the samples can show them
	const double show = 1.0 - Soft(spacing / (wall * line.width / 3.0), 0.3, 0.4);
	if (show <= 0.0)
		return cut;
	const double t = 3.0 * cut, step = std::floor(t);
	return cut + 0.6 * show * ((step + Soft(t - step, 0.25, 0.5)) / 3.0 - cut);
}

void TerrainLines::Gather(const std::vector<uint32_t> &nearby, const vector3d &p, const vector3d &lobed, const double spacing, const double tierFade, Reached &reached, Nearest &deepest) const
{
	int unevenLine = -1;
	double uneven = 0.0;
	for (const uint32_t id : nearby) {
		const Piece &piece = m_pieces[id];
		const Line &line = m_lines[piece.line];
		const vector3d &q = line.kind == SCARP || line.kind == GRABEN ? lobed : p;
		const vector3d ab = piece.b - piece.a;
		const double t = std::clamp((q - piece.a).Dot(ab) / piece.lengthSqr, 0.0, 1.0);
		const double along = (double(piece.first) + t) / double(line.pieces);
		double lineWidth = double(line.width) * (line.wedge ? 0.15 + 0.85 * along : 1.0);
		// a fault valley narrows and deepens, and widens and shoals, along its length
		if (line.kind == GRABEN) {
			if (piece.line != unevenLine) {
				unevenLine = piece.line;
				uneven = std::clamp(SphereCells::Fbm(q, 0.25 / double(line.width), 3, m_noiseSeed + 70 + uint32_t(piece.line % 8)) / 0.35, -1.0, 1.0);
			}
			lineWidth *= 1.0 + 0.3 * uneven;
		}
		const double width = std::max(lineWidth, 1.6 * spacing);
		// (a piece is a chord, under the surface. A scarp changes side on its line, so there it's measured along the
		// surface, which reaches 0: from the chord, the two sides don't meet, and the ground steps from one to the other)
		const double d = (line.kind == SCARP && t > 0.0 && t < 1.0 ? std::abs(q.Dot(piece.a.Cross(ab).Normalized())) : (q - (piece.a + ab * t)).Length()) / width;
		if (d >= REACH)
			continue;
		// strength tapers to both ends of the line, continuously
		const double fade = std::min(1.0, std::min(along, 1.0 - along) * 6.0 + 0.05) * tierFade;
		if (fade <= 0.0)
			continue;
		Nearest here;
		here.distance = d;
		here.width = width;
		here.height = double(line.height) * fade * (line.kind == GRABEN ? 1.0 - 0.2 * uneven : 1.0);
		here.fade = fade;
		here.tone = line.tone;
		here.coverage = std::min(lineWidth / width, 1.0);
		here.blue = line.blue;
		here.kind = line.kind;
		here.generation = line.generation;
		here.side = (q - piece.a).Dot(piece.a.Cross(ab)) > 0.0 ? 1.0 : -1.0;
		if (line.kind == GRABEN) {
			here.carve = here.height * GrabenCut(here, spacing);
			here.rim = GRABEN_RIM * here.height * (1.0 - Soft(d, 1.0, GRABEN_RIM_REACH));
		}

		// Cracks cut into all else, and where two meet the deeper wins: its depth, not its nearness, or the ground
		// would jump from one depth to the other
		if (line.kind == FRACTURE) {
			if (d < 1.0) {
				here.carve = here.height * CrackShape(here, spacing);
				if (here.carve > deepest.carve)
					deepest = here;
			}
			continue;
		}
		// Each line once, at its nearest piece. They are all drawn, and none takes the place of another of its age: a
		// ridge in a band rises from the band's floor (the nearest in widths of each won, and where a thin ridge took
		// over from a wide band the band's depth was gone at once, in a cliff).
		here.line = piece.line;
		Nearest *same = nullptr;
		for (int i = 0; i < reached.count && !same; i++) {
			const Nearest &other = reached.items[i];
			if (other.line == here.line || (here.kind == GRABEN && other.kind == GRABEN && other.generation == here.generation))
				same = &reached.items[i];
		}
		if (same) {
			if (here.kind == GRABEN && same->kind == GRABEN) {
				// where two valleys meet the deeper wins and the taller rim stays, or the ground would jump
				const double rim = std::max(here.rim, same->rim), nearest = std::min(here.distance, same->distance);
				if (here.carve > same->carve)
					*same = here;
				same->rim = rim;
				same->distance = nearest;
			} else if (d < same->distance) {
				*same = here;
			}
		} else if (reached.count < Reached::MOST) {
			reached.items[reached.count++] = here;
		} else {
			// (hardly ever: the farthest makes way)
			Nearest *farthest = &reached.items[0];
			for (int i = 1; i < reached.count; i++)
				if (reached.items[i].distance > farthest->distance)
					farthest = &reached.items[i];
			if (d < farthest->distance)
				*farthest = here;
		}
	}
}

double TerrainLines::Cover(const Nearest &line)
{
	// (a faint line, as at its ends, takes away as little as it shows)
	return (1.0 - Soft(line.distance, 0.7, CutReach(line.kind) - 0.7)) * line.fade;
}

void TerrainLines::Draw(const Nearest &line, const vector3d &p, const double spacing, const double scale, Sample &out) const
{
	const double d = line.distance, h = line.height;
	// a line finer than the samples is drawn wider, so it's coloured the fainter
	const double colour = line.tone * line.fade * std::sqrt(line.coverage) * scale;
	// the crack itself is sharp, down to the sample size
	const double thin = std::max(0.14, 1.1 * spacing / line.width);
	switch (line.kind) {
	case RIDGE: {
		const double crest = (d - 0.6) / 0.24, trough = d / thin;
		// (fading to nothing at the edge of its reach, so there's no step)
		out.height += scale * h * (std::exp(-crest * crest) - 1.3 * std::exp(-trough * trough)) * (1.0 - Soft(d, 1.0, 0.4));
		out.crack = std::max(out.crack, std::exp(-trough * trough) * colour);
		// salts on its flanks, stronger in some stretches than others
		const double stretch = Soft(SphereCells::Fbm(p, 9.0, 3, m_noiseSeed + 60) / 0.35, -0.5, 1.0);
		const double flank = d / 1.2;
		out.band = std::max(out.band, std::exp(-flank * flank) * (0.35 + 0.65 * stretch) * colour);
		break;
	}
	case BAND: {
		// sharp sides, down to the sample size
		const double t = Soft(1.15 - d, 0.0, std::max(0.1, 1.2 * spacing / line.width));
		out.dark = std::max(out.dark, t * colour);
		// fine lines along it, once the samples can show them
		const double signedOffset = line.side * d + 0.05 * SphereCells::Fbm(p, 40.0, 3, m_noiseSeed + 61) / 0.35;
		const double lines = 0.5 + 0.5 * std::cos(signedOffset * 25.0);
		const double show = spacing > 0.0 ? Soft(0.25 * line.width / spacing, 2.0, 2.0) : 1.0;
		out.height += -0.5 * scale * h * t * (1.0 - 0.15 * show * lines);
		const double stretch = Soft(SphereCells::Fbm(p, 6.0, 3, m_noiseSeed + 62) / 0.35, -0.8, 1.2);
		// a darker core with soft edges
		out.band = std::max(out.band, t * (0.55 + 0.45 * Soft(0.75 - d, 0.0, 0.5)) * (0.72 + 0.28 * (1.0 - show + show * lines)) * (0.6 + 0.4 * stretch) * colour);
		break;
	}
	case GRABEN: {
		// a flat floor, steep walls and a low rim
		const double cut = h > 0.0 ? line.carve / h : 0.0;
		out.height += scale * (line.rim * (1.0 - cut) - line.carve);
		break;
	}
	case SCARP: {
		// a long gentle back, and a front of real width: as a sheer step, the mesh draws it as stripes
		const double front = std::max(0.25, 2.0 * spacing / line.width);
		const double across = line.side * d;
		if (across >= 0.0) {
			const double t = std::clamp(1.0 - across / 1.6, 0.0, 1.0);
			out.height += scale * h * t * t;
		} else {
			const double t = std::clamp(1.0 + across / front, 0.0, 1.0);
			out.height += scale * h * t * t * (3.0 - 2.0 * t);
		}
		break;
	}
	case FRACTURE: {
		const double t = CrackShape(line, spacing);
		out.height += -scale * h * t;
		out.crack = std::max(out.crack, t * colour);
		// (the deep ones have a coloured floor, and walls that darken on the way down)
		const double slot = scale * t * t * Soft(h, 450.0, 350.0);
		if (slot > out.slot) {
			out.slot = slot;
			out.slotBlue = line.blue;
		}
		break;
	}
	}
}

TerrainLines::Sample TerrainLines::At(const vector3d &p) const
{
	Sample out;
	if (m_lines.empty())
		return out;
	const double band = SphereCells::RoundedBand(TerrainNoise::s_maxFrequency);
	const double spacing = band > 0.0 ? 0.5 / band : 0.0;
	// freckles and low domes, under any line that crosses them
	if (spacing < FRECKLE_GONE) {
		if (const std::vector<uint32_t> *spots = m_domeIndex.At(p)) {
			for (const uint32_t id : *spots) {
				const Dome &dome = m_domes[id];
				const double cosine = p.Dot(dome.centre);
				if (cosine < dome.cosReach)
					continue;
				// (never finer than the samples can show, then the fainter)
				const double reach = std::max(dome.radius, 1.2 * spacing);
				// not round: lobed, and of no fixed shape
				const double r = std::max(std::acos(std::clamp(cosine, -1.0, 1.0)) / reach + 1.1 * SphereCells::Fbm(p, 1.2 / dome.radius, 3, m_noiseSeed + 63, 0.6), 0.0);
				if (r >= 1.0)
					continue;
				const double f = 1.0 - r * r;
				const double show = spacing > 0.0 ? Soft(dome.radius / spacing, 1.0, 1.0) : 1.0;
				if (dome.freckle) {
					out.height -= dome.height * f * f * show;
					// (the fainter when drawn wider than it is)
					out.band = std::max(out.band, Soft(1.0 - r, 0.0, 0.5) * dome.radius / reach);
				} else {
					out.height += dome.height * f * f * show;
					out.pale = std::max(out.pale, Soft(1.0 - r, 0.0, 0.25) * show);
				}
			}
		}
	}

	const auto tierFade = [spacing](const int tier) { return 1.0 - Soft(spacing, TIER_FROM[tier], TIER_GONE[tier] - TIER_FROM[tier]); };
	const std::vector<uint32_t> *large = spacing < TIER_GONE[LARGE] ? m_index[LARGE].At(p) : nullptr;
	const std::vector<uint32_t> *medium = spacing < TIER_GONE[MEDIUM] ? m_index[MEDIUM].At(p) : nullptr;
	const std::vector<uint32_t> *mesh = spacing < TIER_GONE[MESH] ? m_index[MESH].At(p) : nullptr;
	if ((!large || large->empty()) && (!medium || medium->empty()) && (!mesh || mesh->empty()))
		return out;

	// the line of a scarp or a valley's wall wanders in close views, where the samples can show it
	vector3d lobed = p;
	if (m_lobed && spacing < 1.0 / 256.0 && large && !large->empty()) {
		const vector3d shift(SphereCells::Fbm(p, 64.0, 8, m_noiseSeed + 51, 0.6), SphereCells::Fbm(p, 64.0, 8, m_noiseSeed + 52, 0.6), SphereCells::Fbm(p, 64.0, 8, m_noiseSeed + 53, 0.6));
		lobed = (p + shift * SCARP_LOBES).Normalized();
	}

	Reached reached;
	Nearest deepest;
	if (large)
		Gather(*large, p, lobed, spacing, tierFade(LARGE), reached, deepest);
	if (medium)
		Gather(*medium, p, p, spacing, tierFade(MEDIUM), reached, deepest);
	if (mesh)
		Gather(*mesh, p, p, spacing, tierFade(MESH), reached, deepest);

	// a crack takes away what it cuts through
	const double kept = deepest.carve > 0.0 ? 1.0 - CrackShape(deepest, spacing) : 1.0;
	// Every line in reach, less what younger lines across it take away; lines of one age add up. Where bands cross
	// the deepest is kept, not their depths together. All of it changes smoothly, so the ground never steps.
	double bandHeight = 0.0;
	for (int i = 0; i < reached.count; i++) {
		const Nearest &line = reached.items[i];
		double covered = 0.0;
		for (int j = 0; j < reached.count; j++)
			if (reached.items[j].generation > line.generation)
				covered = std::max(covered, Cover(reached.items[j]));
		const double scale = kept * (1.0 - covered);
		if (scale <= 0.0)
			continue;
		if (line.kind == BAND) {
			Sample band;
			Draw(line, p, spacing, scale, band);
			bandHeight = std::min(bandHeight, band.height);
			out.band = std::max(out.band, band.band);
			out.dark = std::max(out.dark, band.dark);
		} else {
			Draw(line, p, spacing, scale, out);
		}
	}
	out.height += bandHeight;
	if (deepest.carve > 0.0)
		Draw(deepest, p, spacing, 1.0, out);
	return out;
}

std::vector<std::string> TerrainLines::DescribeStep(const vector3d &a, const vector3d &b) const
{
	static const char *const KINDS[] = { "ridge", "fault valley", "scarp", "band", "fracture" };
	std::vector<std::string> out;
	const double band = SphereCells::RoundedBand(TerrainNoise::s_maxFrequency);
	const double spacing = band > 0.0 ? 0.5 / band : 0.0;
	char text[240];
	for (int pass = 0; pass < 2; pass++) {
		const vector3d &p = pass == 0 ? a : b;
		std::snprintf(text, sizeof(text), "%s: the lines' height %.3f m", pass == 0 ? "before" : "after", At(p).height);
		out.emplace_back(text);
		// as At and Gather have them: the nearest piece of each line in reach
		struct Reached {
			int line;
			double distance, side, fade;
		};
		std::vector<Reached> reached;
		for (int tier = 0; tier < TIER_COUNT; tier++) {
			const std::vector<uint32_t> *nearby = spacing < TIER_GONE[tier] ? m_index[tier].At(p) : nullptr;
			if (!nearby)
				continue;
			vector3d lobed = p;
			if (m_lobed && spacing < 1.0 / 256.0 && tier == LARGE && !nearby->empty()) {
				const vector3d shift(SphereCells::Fbm(p, 64.0, 8, m_noiseSeed + 51, 0.6), SphereCells::Fbm(p, 64.0, 8, m_noiseSeed + 52, 0.6), SphereCells::Fbm(p, 64.0, 8, m_noiseSeed + 53, 0.6));
				lobed = (p + shift * SCARP_LOBES).Normalized();
			}
			for (const uint32_t id : *nearby) {
				const Piece &piece = m_pieces[id];
				const Line &line = m_lines[piece.line];
				const vector3d &q = line.kind == SCARP || line.kind == GRABEN ? lobed : p;
				const vector3d ab = piece.b - piece.a;
				const double t = std::clamp((q - piece.a).Dot(ab) / piece.lengthSqr, 0.0, 1.0);
				const double along = (double(piece.first) + t) / double(line.pieces);
				const double lineWidth = double(line.width) * (line.wedge ? 0.15 + 0.85 * along : 1.0);
				const double d = (line.kind == SCARP && t > 0.0 && t < 1.0 ? std::abs(q.Dot(piece.a.Cross(ab).Normalized())) : (q - (piece.a + ab * t)).Length()) / std::max(lineWidth, 1.6 * spacing);
				if (d >= REACH)
					continue;
				const double fade = std::min(1.0, std::min(along, 1.0 - along) * 6.0 + 0.05);
				const double side = (q - piece.a).Dot(piece.a.Cross(ab)) > 0.0 ? 1.0 : -1.0;
				auto it = std::find_if(reached.begin(), reached.end(), [&piece](const Reached &r) { return r.line == piece.line; });
				if (it == reached.end())
					reached.push_back({ piece.line, d, side, fade });
				else if (d < it->distance)
					*it = { piece.line, d, side, fade };
			}
		}
		for (const Reached &r : reached) {
			const Line &line = m_lines[r.line];
			std::snprintf(text, sizeof(text), "  line %d, %s, generation %d, %.0f m high, %.5f wide: %.4f widths away on side %+.0f, faded to %.3f",
				r.line, KINDS[line.kind], int(line.generation), double(line.height), double(line.width), r.distance, r.side, r.fade);
			out.emplace_back(text);
		}
	}
	return out;
}
