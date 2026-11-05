// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainPhysicalValleys.h"

#include "MathUtil.h"
#include "FracDef.h"
#include "SphereCells.h"
#include "TerrainPhysicalHydro.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
	std::atomic<uint64_t> s_nextId(1);

	// km between valleys of each level
	constexpr double SPACING[TerrainValleys::LEVEL_COUNT] = { 96.0, 48.0, 24.0, 12.0, 6.0, 3.0, 1.5, 0.75, 0.375 };
	// depth per km of spacing, so walls keep about the same angle at every scale
	constexpr double WALL[TerrainValleys::LEVEL_COUNT] = { 0.02, 0.03, 0.06, 0.1, 0.1, 0.1, 0.1, 0.09, 0.08 };
	// the average of each level's valley shape over the ground, measured in the lab
	constexpr double AVERAGE[TerrainValleys::LEVEL_COUNT] = { 0.53, 0.51, 0.38, 0.39, 0.39, 0.39, 0.39, 0.39, 0.39 };
	// a narrow trench along the main valleys only: metres deep, km wide
	constexpr double TRENCH_DEPTH[2] = { 1200.0, 900.0 };
	constexpr double TRENCH_WIDTH[2] = { 6.0, 4.0 };
	constexpr double TRENCH_AVERAGE = 0.09;
	constexpr double FLOOR = 0.15; // valleys stop at this share of the height above sea
	constexpr double FULL = 3000.0; // ground this high above sea gets the full depth
	constexpr double FJORD[2] = { 0.02, 0.015 }; // share of the relief limit the main valleys cut near the sea
	constexpr int SAMPLES = 17; // along a branch
	constexpr double MIN_PIXELS = 6.0; // a level shows only where it spans this many samples
	constexpr double BENDS[8] = { 0.17, 0.1, 0.07, 0.05, 0.035, 0.025, 0.018, 0.013 }; // as a share of a branch's length

	// in spacings of the level
	constexpr int CHUNK_CELLS = 8; // the side of a chunk
	constexpr int HALO_CELLS = 5; // a site's neighbours can be in cells this far away
	constexpr double BRANCH_MARGIN = 6.0; // no branch leaves its chunk by more than this
	constexpr double JOIN_REACH = 10.0; // larger valleys to join are looked for this far around a chunk
	constexpr double SHAPE_END = 0.61; // beyond this a valley has no effect, however the wobble stretches it

	constexpr size_t MAX_CHUNKS = 3000; // about 100 MB at most; the least used go first

	std::atomic<uint64_t> s_chunksBuilt(0);
	std::atomic<uint64_t> s_chunkMicroseconds(0);
	TerrainSpeedCounter s_cutSpeed;

	uint64_t ChunkKey(const int64_t x, const int64_t y, const int64_t z)
	{
		return (uint64_t(x + 32768) << 32) | (uint64_t(y + 32768) << 16) | uint64_t(z + 32768);
	}

	int64_t FloorTo(const double v, const double size)
	{
		return int64_t(std::floor(v / size));
	}

	struct Site {
		vector3d position;
		int64_t ix, iy, iz;
		double height;
		bool wet;
		bool inChunk;
	};

	// the points of larger valleys, for smaller ones to join
	struct PointGrid {
		double x0 = 0.0, y0 = 0.0, z0 = 0.0, cell = 1.0;
		int nx = 0, ny = 0, nz = 0;
		std::vector<int> head, next;
		const std::vector<vector3d> *points = nullptr;

		void Build(const std::vector<vector3d> &pts, const vector3d &centre, const double around, const double cellSize)
		{
			points = &pts;
			cell = cellSize;
			x0 = centre.x - around;
			y0 = centre.y - around;
			z0 = centre.z - around;
			nx = ny = nz = int(2.0 * around / cell) + 2;
			head.assign(size_t(nx) * ny * nz, -1);
			next.assign(pts.size(), -1);
			for (int i = 0; i < int(pts.size()); i++) {
				if (pts[i].x < x0 || pts[i].y < y0 || pts[i].z < z0)
					continue;
				const int a = int((pts[i].x - x0) / cell), b = int((pts[i].y - y0) / cell), c = int((pts[i].z - z0) / cell);
				if (a >= nx || b >= ny || c >= nz)
					continue;
				const size_t k = (size_t(c) * ny + b) * nx + a;
				next[i] = head[k];
				head[k] = i;
			}
		}
		// the nearest point within one cell or so, or -1
		int Nearest(const vector3d &q, double &distance) const
		{
			const int a = int(std::floor((q.x - x0) / cell)), b = int(std::floor((q.y - y0) / cell)), c = int(std::floor((q.z - z0) / cell));
			double best = 1e30;
			int found = -1;
			for (int z = std::max(c - 1, 0); z <= std::min(c + 1, nz - 1); z++) {
				for (int y = std::max(b - 1, 0); y <= std::min(b + 1, ny - 1); y++) {
					for (int x = std::max(a - 1, 0); x <= std::min(a + 1, nx - 1); x++) {
						for (int i = head[(size_t(z) * ny + y) * nx + x]; i >= 0; i = next[i]) {
							const double d = ((*points)[i] - q).LengthSqr();
							if (d < best) {
								best = d;
								found = i;
							}
						}
					}
				}
			}
			distance = std::sqrt(best);
			return found;
		}
	};
} // namespace

TerrainSpeedCounter &TerrainValleys::CutSpeed()
{
	return s_cutSpeed;
}

uint64_t TerrainValleys::ChunksBuilt()
{
	return s_chunksBuilt.load(std::memory_order_relaxed);
}

double TerrainValleys::ChunkMilliseconds()
{
	return double(s_chunkMicroseconds.load(std::memory_order_relaxed)) / 1000.0;
}

void TerrainValleys::ResetStats()
{
	s_cutSpeed.Reset();
	s_chunksBuilt = 0;
	s_chunkMicroseconds = 0;
}

void TerrainValleys::Build(const uint32_t noiseSeed, const double radiusKm, const double reliefLimit, const double seaLevel, const TerrainHydro *hydro, GroundAt ground, KeepAt keep)
{
	m_id = s_nextId++;
	m_seed = noiseSeed;
	m_radiusKm = radiusKm;
	m_reliefLimit = std::max(reliefLimit, 1.0);
	m_seaLevel = seaLevel;
	m_hydro = hydro;
	m_ground = std::move(ground);
	m_keep = std::move(keep);
	for (auto &level : m_chunks)
		level.clear();
	m_chunkCount = 0;
}

TerrainValleys::ChunkPtr TerrainValleys::GetChunk(const int k, const int64_t cx, const int64_t cy, const int64_t cz) const
{
	const double lam = SPACING[k];
	const double size = CHUNK_CELLS * lam;
	// the surface doesn't pass through it
	const vector3d middle((double(cx) + 0.5) * size, (double(cy) + 0.5) * size, (double(cz) + 0.5) * size);
	if (std::abs(middle.Length() - m_radiusKm) > 0.87 * size + lam)
		return nullptr;

	const uint64_t key = ChunkKey(cx, cy, cz);
	const uint32_t now = m_clock.fetch_add(1, std::memory_order_relaxed);
	auto look = [&](ChunkPtr &out) {
		std::shared_lock<std::shared_mutex> lock(m_lock);
		const auto it = m_chunks[k].find(key);
		if (it == m_chunks[k].end())
			return false;
		out = it->second;
		if (out)
			out->used.store(now, std::memory_order_relaxed);
		return true;
	};
	ChunkPtr chunk;
	if (look(chunk))
		return chunk;

	// Another thread may be making this one, or one it needs: wait for it, as it's only ever made once
	std::lock_guard<std::recursive_mutex> building(m_buildLock);
	if (look(chunk))
		return chunk;

	// the chunks of larger valleys that this one needs are timed as part of it
	thread_local int depth = 0;
	const auto start = std::chrono::steady_clock::now();
	depth++;
	chunk = MakeChunk(k, cx, cy, cz);
	depth--;
	if (depth == 0) {
		const auto elapsed = std::chrono::steady_clock::now() - start;
		s_chunkMicroseconds.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()), std::memory_order_relaxed);
	}
	s_chunksBuilt.fetch_add(1, std::memory_order_relaxed);
	if (chunk)
		chunk->used.store(now, std::memory_order_relaxed);

	std::unique_lock<std::shared_mutex> lock(m_lock);
	if (m_chunkCount >= MAX_CHUNKS) {
		// the older half. A thread still using one keeps it until it moves on; a chunk made again comes out the same.
		std::vector<uint32_t> ages;
		ages.reserve(m_chunkCount);
		for (int level = 0; level < LEVEL_COUNT; level++) {
			for (const auto &entry : m_chunks[level])
				ages.push_back(entry.second ? entry.second->used.load(std::memory_order_relaxed) : 0u);
		}
		if (!ages.empty()) {
			std::nth_element(ages.begin(), ages.begin() + ages.size() / 2, ages.end());
			const uint32_t limit = ages[ages.size() / 2];
			for (int level = 0; level < LEVEL_COUNT; level++) {
				auto &map = m_chunks[level];
				for (auto it = map.begin(); it != map.end();) {
					if (!it->second || it->second->used.load(std::memory_order_relaxed) < limit) {
						it = map.erase(it);
						m_chunkCount--;
					} else {
						++it;
					}
				}
			}
		}
	}
	if (m_chunks[k].emplace(key, chunk).second)
		m_chunkCount++;
	return chunk;
}

TerrainValleys::ChunkPtr TerrainValleys::MakeChunk(const int k, const int64_t cx, const int64_t cy, const int64_t cz) const
{
	const double R = m_radiusKm;
	const double lam = SPACING[k];
	const double size = CHUNK_CELLS * lam;
	const uint32_t sd = m_seed + 1000u * uint32_t(k + 1);
	const int64_t x0 = cx * CHUNK_CELLS, y0 = cy * CHUNK_CELLS, z0 = cz * CHUNK_CELLS;
	const vector3d low(double(cx) * size, double(cy) * size, double(cz) * size);
	const vector3d high = low + vector3d(size, size, size);
	const vector3d middle = (low + high) * 0.5;

	// the level's points here and around: one for each cell of a fixed lattice that the surface passes through
	std::vector<Site> sites;
	std::vector<int> own;
	for (int64_t x = x0 - HALO_CELLS; x < x0 + CHUNK_CELLS + HALO_CELLS; x++) {
		for (int64_t y = y0 - HALO_CELLS; y < y0 + CHUNK_CELLS + HALO_CELLS; y++) {
			for (int64_t z = z0 - HALO_CELLS; z < z0 + CHUNK_CELLS + HALO_CELLS; z++) {
				const vector3d cell((double(x) + 0.5) * lam, (double(y) + 0.5) * lam, (double(z) + 0.5) * lam);
				if (std::abs(cell.Length() - R) > 0.5 * lam)
					continue;
				// a third of the side valleys are left out, or their even spacing shows as rows of bumps
				if (k >= 2 && SphereCells::Hash(x, y, z, sd + 9) <= -0.35)
					continue;
				const vector3d jitter(SphereCells::Hash(x, y, z, sd), SphereCells::Hash(x, y, z, sd + 1), SphereCells::Hash(x, y, z, sd + 2));
				const vector3d unit = (cell + jitter * (0.45 * lam)).Normalized();
				Site site;
				site.position = unit * R;
				site.ix = x;
				site.iy = y;
				site.iz = z;
				// the height of a point depends on the place and the level only
				site.height = m_ground(unit, lam) + 0.004 * lam * 1000.0 * SphereCells::Hash(x, y, z, sd + 5);
				site.wet = SphereCells::Hash(x, y, z, sd + 6) * 0.5 + 0.5 < m_keep(unit);
				site.inChunk = x >= x0 && x < x0 + CHUNK_CELLS && y >= y0 && y < y0 + CHUNK_CELLS && z >= z0 && z < z0 + CHUNK_CELLS;
				if (site.inChunk && site.wet && !(m_seaLevel > -1e29 && site.height <= m_seaLevel))
					own.push_back(int(sites.size()));
				sites.push_back(site);
			}
		}
	}
	if (own.empty())
		return nullptr;

	// the main rivers and the larger valleys around here, for these to join
	const double joinReach = JOIN_REACH * lam;
	std::vector<vector3d> parents, tangents;
	if (m_hydro)
		m_hydro->RiverPoints(middle.Normalized(), std::min((0.87 * size + joinReach) / R, M_PI), 8000.0, parents, tangents);
	for (vector3d &q : parents)
		q = q * R;
	for (int j = 0; j < k; j++) {
		const double spacing = SPACING[j], chunkSize = CHUNK_CELLS * spacing;
		const double outer = joinReach + BRANCH_MARGIN * spacing;
		const int64_t ax = FloorTo(low.x - outer, chunkSize), bx = FloorTo(high.x + outer, chunkSize);
		const int64_t ay = FloorTo(low.y - outer, chunkSize), by = FloorTo(high.y + outer, chunkSize);
		const int64_t az = FloorTo(low.z - outer, chunkSize), bz = FloorTo(high.z + outer, chunkSize);
		for (int64_t x = ax; x <= bx; x++) {
			for (int64_t y = ay; y <= by; y++) {
				for (int64_t z = az; z <= bz; z++) {
					const ChunkPtr chunk = GetChunk(j, x, y, z);
					if (!chunk)
						continue;
					for (const Piece &piece : chunk->pieces) {
						const vector3d &a = piece.a;
						if (a.x < low.x - joinReach || a.x > high.x + joinReach || a.y < low.y - joinReach || a.y > high.y + joinReach || a.z < low.z - joinReach || a.z > high.z + joinReach)
							continue;
						const vector3d along = piece.b - a;
						parents.push_back(a);
						tangents.push_back(along * (1.0 / std::max(along.Length(), 1e-9)));
					}
				}
			}
		}
	}
	PointGrid grid;
	const bool hasParents = !parents.empty();
	if (hasParents)
		grid.Build(parents, middle, 0.5 * size + joinReach + 3.0 * lam, 3.0 * lam);

	std::vector<Piece> pieces;
	pieces.reserve(own.size() * (SAMPLES - 1));
	std::vector<std::pair<double, int>> around;
	const int count = int(sites.size());
	const double near2 = (2.2 * lam) * (2.2 * lam);
	const double limit = BRANCH_MARGIN * lam;
	for (const int i : own) {
		const Site &site = sites[i];
		// the lowest of its neighbours, if any is lower
		around.clear();
		for (int j = 0; j < count; j++) {
			const double d = (sites[j].position - site.position).LengthSqr();
			if (d < near2)
				around.emplace_back(d, j);
		}
		if (around.size() > 9) {
			std::nth_element(around.begin(), around.begin() + 8, around.end());
			around.resize(9);
		}
		int lowest = i;
		for (const auto &n : around) {
			if (sites[n.second].height < sites[lowest].height)
				lowest = n.second;
		}
		const bool hasLow = lowest != i;
		vector3d end = site.position;
		bool flows = false;
		// or the larger valley nearby, a little downstream of its closest spot
		if (hasParents) {
			double d = 0.0;
			const int at = grid.Nearest(site.position, d);
			if (at >= 0 && (d < 1.1 * lam || (!hasLow && d < 3.0 * lam))) {
				const vector3d ahead = (parents[at] + tangents[at] * (0.8 * d)).Normalized() * R;
				double ignored = 0.0;
				const int to = grid.Nearest(ahead, ignored);
				end = parents[to >= 0 ? to : at];
				flows = true;
			}
		}
		if (!flows && hasLow) {
			end = sites[lowest].position;
			flows = true;
		}
		if (!flows)
			continue;

		// a curved branch from its head to where it joins
		const vector3d run = end - site.position;
		const double length = run.Length();
		vector3d side = (site.position * (1.0 / R)).Cross(run);
		side = side * (1.0 / std::max(side.Length(), 1e-9));
		double bend[8];
		for (int b = 0; b < 8; b++)
			bend[b] = SphereCells::Hash(site.ix, site.iy, site.iz, sd + 7 + uint32_t(b)) * BENDS[b];
		const size_t before = pieces.size();
		vector3d previous = site.position;
		bool inside = true;
		for (int s = 1; s < SAMPLES && inside; s++) {
			const double t = double(s) / (SAMPLES - 1);
			double offset = 0.0;
			for (int b = 0; b < 8; b++)
				offset += bend[b] * std::sin((b + 1) * M_PI * t);
			const vector3d q = (site.position + run * t + side * (offset * length)).Normalized() * R;
			// a branch this long would be missed by what looks for one around a chunk, differently from each side
			inside = q.x >= low.x - limit && q.x <= high.x + limit && q.y >= low.y - limit && q.y <= high.y + limit && q.z >= low.z - limit && q.z <= high.z + limit;
			Piece piece;
			piece.a = previous;
			piece.b = q;
			pieces.push_back(piece);
			previous = q;
		}
		if (!inside)
			pieces.resize(before);
	}
	if (pieces.empty())
		return nullptr;

	// sorted into a grid, one spacing to the cell
	std::shared_ptr<Chunk> chunk = std::make_shared<Chunk>();
	vector3d boxLow = pieces[0].a, boxHigh = pieces[0].a;
	double longest = 0.0;
	for (const Piece &piece : pieces) {
		for (const vector3d *v : { &piece.a, &piece.b }) {
			boxLow = vector3d(std::min(boxLow.x, v->x), std::min(boxLow.y, v->y), std::min(boxLow.z, v->z));
			boxHigh = vector3d(std::max(boxHigh.x, v->x), std::max(boxHigh.y, v->y), std::max(boxHigh.z, v->z));
		}
		longest = std::max(longest, (piece.b - piece.a).LengthSqr());
	}
	chunk->low = boxLow;
	chunk->high = boxHigh;
	chunk->pad = 0.5 * std::sqrt(longest);
	chunk->nx = int((boxHigh.x - boxLow.x) / lam) + 1;
	chunk->ny = int((boxHigh.y - boxLow.y) / lam) + 1;
	chunk->nz = int((boxHigh.z - boxLow.z) / lam) + 1;
	const size_t cells = size_t(chunk->nx) * chunk->ny * chunk->nz;
	auto cellOf = [&](const Piece &piece) {
		const vector3d m = (piece.a + piece.b) * 0.5;
		const int a = std::min(int((m.x - boxLow.x) / lam), chunk->nx - 1);
		const int b = std::min(int((m.y - boxLow.y) / lam), chunk->ny - 1);
		const int c = std::min(int((m.z - boxLow.z) / lam), chunk->nz - 1);
		return (size_t(c) * chunk->ny + b) * chunk->nx + a;
	};
	std::vector<uint32_t> counts(cells + 1, 0u);
	for (const Piece &piece : pieces)
		counts[cellOf(piece) + 1]++;
	for (size_t c = 0; c < cells; c++)
		counts[c + 1] += counts[c];
	chunk->start.resize(cells + 1);
	for (size_t c = 0; c <= cells; c++)
		chunk->start[c] = uint16_t(counts[c]);
	chunk->pieces.resize(pieces.size());
	for (const Piece &piece : pieces)
		chunk->pieces[counts[cellOf(piece)]++] = piece;
	return chunk;
}

void TerrainValleys::Refresh(const int k, LevelView &view, const vector3d &q, const double margin) const
{
	const double lam = SPACING[k];
	const double size = CHUNK_CELLS * lam;
	const double search = SHAPE_END * lam + margin;
	const double outer = search + BRANCH_MARGIN * lam;
	const std::array<int64_t, 6> box = {
		FloorTo(q.x - outer, size), FloorTo(q.x + outer, size),
		FloorTo(q.y - outer, size), FloorTo(q.y + outer, size),
		FloorTo(q.z - outer, size), FloorTo(q.z + outer, size)
	};
	if (!view.hasBox || box != view.box) {
		view.chunks.clear();
		for (int64_t x = box[0]; x <= box[1]; x++) {
			for (int64_t y = box[2]; y <= box[3]; y++) {
				for (int64_t z = box[4]; z <= box[5]; z++) {
					ChunkPtr chunk = GetChunk(k, x, y, z);
					if (chunk)
						view.chunks.push_back(std::move(chunk));
				}
			}
		}
		view.box = box;
		view.hasBox = true;
	}

	// For any point within the margin, the nearest piece is at most twice the margin further from
	// here than the nearest piece is. Usually that leaves a handful, however long the valley.
	thread_local std::vector<std::pair<double, const Piece *>> found;
	found.clear();
	double best = 1e30;
	for (const ChunkPtr &chunk : view.chunks) {
		const double reach = search + chunk->pad;
		if (q.x < chunk->low.x - reach || q.x > chunk->high.x + reach || q.y < chunk->low.y - reach || q.y > chunk->high.y + reach || q.z < chunk->low.z - reach || q.z > chunk->high.z + reach)
			continue;
		const int ax = std::max(int((q.x - reach - chunk->low.x) / lam), 0), bx = std::min(int((q.x + reach - chunk->low.x) / lam), chunk->nx - 1);
		const int ay = std::max(int((q.y - reach - chunk->low.y) / lam), 0), by = std::min(int((q.y + reach - chunk->low.y) / lam), chunk->ny - 1);
		const int az = std::max(int((q.z - reach - chunk->low.z) / lam), 0), bz = std::min(int((q.z + reach - chunk->low.z) / lam), chunk->nz - 1);
		for (int z = az; z <= bz; z++) {
			for (int y = ay; y <= by; y++) {
				const size_t row = (size_t(z) * chunk->ny + y) * chunk->nx;
				for (int x = ax; x <= bx; x++) {
					const int first = chunk->start[row + x], last = chunk->start[row + x + 1];
					for (int i = first; i < last; i++) {
						const Piece &piece = chunk->pieces[i];
						const vector3d ab = piece.b - piece.a;
						const double t = std::clamp((q - piece.a).Dot(ab) / std::max(ab.Dot(ab), 1e-12), 0.0, 1.0);
						const double d = (q - (piece.a + ab * t)).Length();
						if (d <= search) {
							found.emplace_back(d, &piece);
							best = std::min(best, d);
						}
					}
				}
			}
		}
	}
	view.list.clear();
	for (const auto &entry : found) {
		if (entry.first <= best + 2.0 * margin)
			view.list.push_back(*entry.second);
	}
	view.centre = q;
	view.margin = margin;
}

TerrainValleys::Cut TerrainValleys::At(const vector3d &p, const double height, const double fjord, const double share) const
{
	Cut out;
	if (m_id == 0)
		return out;
	TerrainSampledTimer timer(s_cutSpeed);
	// queries come in runs of nearby points, so each thread keeps what it found around the last one
	thread_local ThreadView thread;
	if (thread.owner != m_id) {
		for (LevelView &view : thread.levels)
			view = LevelView();
		thread.owner = m_id;
	}
	const double maxFrequency = TerrainNoise::s_maxFrequency;
	const double px = maxFrequency > 0.0 ? 0.5 / maxFrequency * m_radiusKm : 0.0;

	// Valleys may cut about two thirds of the ground there is, whatever its height. Wide valleys cost much
	// height for little steepness, so low ground gives up those first and keeps its narrow, steep ones.
	const double above = std::max(height, 0.0);
	const double wideReach = std::pow(std::min(above / (2.0 * FULL), 1.0), 1.2);
	const double narrowReach = std::min(above / FULL, 1.0);
	const vector3d q = p * m_radiusKm;
	double stretch = 0.0; // worked out when first needed
	double depth = 0.0, expect = 0.0, drown = 0.0;
	for (int k = 0; k < LEVEL_COUNT; k++) {
		const double lam = SPACING[k];
		const double reach = k <= 2 ? wideReach : narrowReach;
		const double mean = (AVERAGE[k] * WALL[k] * lam * 1000.0 + (k < 2 ? TRENCH_AVERAGE * TRENCH_DEPTH[k] : 0.0)) * reach * share;
		expect += mean;
		double show = px > 0.0 ? std::clamp((lam / px - MIN_PIXELS) / MIN_PIXELS, 0.0, 1.0) : 1.0;
		show = show * show * (3.0 - 2.0 * show);
		if (show <= 0.0) {
			// too fine to see: it still lowers the ground by its average depth
			depth += mean;
			continue;
		}
		LevelView &view = thread.levels[k];
		const double margin = std::min(lam / 3.0, std::max(lam / 24.0, 16.0 * px));
		if (view.margin < 0.0 || (q - view.centre).LengthSqr() > view.margin * view.margin)
			Refresh(k, view, q, margin);
		double best = 1e30;
		for (const Piece &piece : view.list) {
			const vector3d ab = piece.b - piece.a;
			const double t = std::clamp((q - piece.a).Dot(ab) / std::max(ab.Dot(ab), 1e-12), 0.0, 1.0);
			best = std::min(best, (q - (piece.a + ab * t)).LengthSqr());
		}
		double cut = 0.0;
		double shape = 0.0;
		if (best < 1e29) {
			if (stretch == 0.0)
				stretch = 1.0 + 0.18 * std::clamp(SphereCells::Fbm(p, 400.0, 8, m_seed + 121, 0.6) / 0.35, -1.0, 1.0);
			const double dist = std::sqrt(best) * stretch;
			// straight walls from a flat floor, and a sharp crest where the walls of two valleys meet
			const double x = std::min(dist / (0.5 * lam), 1.0);
			shape = 1.0 - std::pow(std::clamp((x - 0.1) / 0.9, 0.0, 1.0), 1.1);
			cut = WALL[k] * lam * 1000.0 * shape;
			if (k < 2) {
				// the long main valleys stand out from their branches, flat-floored and wandering
				cut += TRENCH_DEPTH[k] * (1.0 - std::pow(std::clamp((dist - 0.12 * TRENCH_WIDTH[k]) / (0.88 * TRENCH_WIDTH[k]), 0.0, 1.0), 1.2));
			}
		}
		if (k < 2)
			drown += show * FJORD[k] * m_reliefLimit * fjord * shape;
		depth += show * cut * reach + (1.0 - show) * mean;
	}
	// inland valleys stay above the sea; only the fjord cut can take them below it
	const double room = std::max((1.0 - FLOOR) * above, 1.0);
	auto limited = [room](const double d) {
		const double r = d / room;
		const double r2 = r * r;
		return d / std::sqrt(std::sqrt(1.0 + r2 * r2));
	};
	// the whole-body map is the ground truth, so detail averages out to it: valleys lie below
	// the map's height and ridges above it, by the valleys' average depth
	out.average = limited(expect);
	out.cut = limited(depth) + drown - out.average;
	return out;
}
