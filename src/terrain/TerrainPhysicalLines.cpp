// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainPhysicalLines.h"

#include "MathUtil.h"
#include "SphereCells.h"
#include "TerrainNoise.h"

#include "../Random.h"

#include <algorithm>
#include <cmath>
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
	constexpr double GRABEN_WALL = 0.3, GRABEN_RIM = 0.1, GRABEN_RIM_REACH = 0.7;
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
		// line this long round into a ring. It's wide for a small moon, and younger than the valleys it cuts.
		const Stress stress = MakeStress(rng, 1);
		const double length = std::clamp(2000.0 / radiusKm, 1.0, 3.6);
		const std::vector<vector3d> points = Walk(rng, RandomDirection(rng), stress, int(length / 0.05), 0.05, 0.03, 0.0);
		const double depth = radiusKm < 1000.0 ? Between(rng, 3000.0, 5000.0) : 6000.0;
		AddLine(points, GRABEN, 3, std::min(radiusKm * 0.095, 120.0) / radiusKm, std::min(env * 0.5, depth), 1.0, LARGE);
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
	return Soft(1.0 - line.distance, 0.0, wall);
}

void TerrainLines::Gather(const std::vector<uint32_t> &nearby, const vector3d &p, const vector3d &lobed, const double spacing, const double tierFade, Nearest &top, Nearest &under, Nearest &deepest) const
{
	for (const uint32_t id : nearby) {
		const Piece &piece = m_pieces[id];
		const Line &line = m_lines[piece.line];
		const vector3d &q = line.kind == SCARP || line.kind == GRABEN ? lobed : p;
		const vector3d ab = piece.b - piece.a;
		const double t = std::clamp((q - piece.a).Dot(ab) / piece.lengthSqr, 0.0, 1.0);
		const double along = (double(piece.first) + t) / double(line.pieces);
		const double lineWidth = double(line.width) * (line.wedge ? 0.15 + 0.85 * along : 1.0);
		const double width = std::max(lineWidth, 1.6 * spacing);
		const double d = (q - (piece.a + ab * t)).Length() / width;
		if (d >= REACH)
			continue;
		// strength tapers to both ends of the line, continuously
		const double fade = std::min(1.0, std::min(along, 1.0 - along) * 6.0 + 0.05) * tierFade;
		if (fade <= 0.0)
			continue;
		Nearest here;
		here.distance = d;
		here.width = width;
		here.height = double(line.height) * fade;
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
		// Of the same age, the nearest. A younger line cuts an older one, which goes on beyond its edge.
		if (top.distance >= REACH) {
			top = here;
		} else if (here.generation == top.generation) {
			if (here.kind == GRABEN && top.kind == GRABEN) {
				// where two valleys meet the deeper wins and the taller rim stays, or the ground would jump
				const double rim = std::max(here.rim, top.rim), nearest = std::min(here.distance, top.distance);
				if (here.carve > top.carve)
					top = here;
				top.rim = rim;
				top.distance = nearest;
			} else if (d < top.distance) {
				top = here;
			}
		} else if (here.generation > top.generation) {
			if (d < CutReach(here.kind)) {
				under = top;
				top = here;
			}
		} else if (top.distance >= CutReach(top.kind)) {
			top = here;
		} else if (d < under.distance) {
			under = here;
		}
	}
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

	Nearest top, under, deepest;
	if (large)
		Gather(*large, p, lobed, spacing, tierFade(LARGE), top, under, deepest);
	if (medium)
		Gather(*medium, p, p, spacing, tierFade(MEDIUM), top, under, deepest);
	if (mesh)
		Gather(*mesh, p, p, spacing, tierFade(MESH), top, under, deepest);

	// a crack takes away what it cuts through
	const double kept = deepest.carve > 0.0 ? 1.0 - CrackShape(deepest, spacing) : 1.0;
	if (top.distance < REACH) {
		if (under.distance < REACH) {
			const double cover = 1.0 - Soft(top.distance, 0.7, CutReach(top.kind) - 0.7);
			if (cover < 1.0)
				Draw(under, p, spacing, kept * (1.0 - cover), out);
		}
		Draw(top, p, spacing, kept, out);
	}
	if (deepest.carve > 0.0)
		Draw(deepest, p, spacing, 1.0, out);
	return out;
}
