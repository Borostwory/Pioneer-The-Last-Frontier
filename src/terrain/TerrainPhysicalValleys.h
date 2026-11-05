// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINPHYSICALVALLEYS_H
#define _TERRAINPHYSICALVALLEYS_H

#include "../vector3.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

class TerrainHydro;

// Time spent in a part of the generator, for the debug window. Only one call in 16 is timed,
// so reading the clock costs next to nothing.
struct TerrainSpeedCounter {
	std::atomic<uint64_t> calls{ 0 };
	std::atomic<uint64_t> nanoseconds{ 0 };

	double Microseconds() const
	{
		const uint64_t n = calls.load(std::memory_order_relaxed);
		return n ? double(nanoseconds.load(std::memory_order_relaxed)) / double(n) / 1000.0 : 0.0;
	}
	void Reset()
	{
		calls = 0;
		nanoseconds = 0;
	}
};

class TerrainSampledTimer {
public:
	explicit TerrainSampledTimer(TerrainSpeedCounter &counter) :
		m_counter(counter)
	{
		thread_local uint32_t count = 0;
		m_active = (++count & 15u) == 0;
		if (m_active)
			m_start = std::chrono::steady_clock::now();
	}
	~TerrainSampledTimer()
	{
		if (!m_active)
			return;
		const auto elapsed = std::chrono::steady_clock::now() - m_start;
		m_counter.nanoseconds.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()), std::memory_order_relaxed);
		m_counter.calls.fetch_add(1, std::memory_order_relaxed);
	}
	TerrainSampledTimer(const TerrainSampledTimer &) = delete;
	TerrainSampledTimer &operator=(const TerrainSampledTimer &) = delete;

private:
	TerrainSpeedCounter &m_counter;
	std::chrono::steady_clock::time_point m_start;
	bool m_active = false;
};

// The drainage network of the physical surface generator (planet lab: valleys.py):
// one fixed system of valleys, the same whatever the view. Each level is a set of
// points fixed in space by a hash, with a height that depends only on the place and
// the level. Each point flows to its lowest neighbour or joins a larger valley. It
// needs only nearby points, with no solve over the body, so every patch agrees.
// Levels too fine for the band limit still lower the ground by their average
// depth, so getting closer reveals detail and doesn't move anything.
//
// The network is made in chunks of a fixed lattice, once each, and shared by all
// threads and views. Safe to read from any thread after Build.
class TerrainValleys {
public:
	static constexpr int LEVEL_COUNT = 9;

	struct Cut {
		double cut = 0.0; // metres below the map's height; below 0 on the ridges
		double average = 0.0; // the depth the valleys remove on average here
	};
	// the height of the network's points, for one level: from the map and detail fixed in space
	using GroundAt = std::function<double(const vector3d &p, double spacingKm)>;
	// the chance of a valley at a point
	using KeepAt = std::function<double(const vector3d &p)>;

	void Build(uint32_t noiseSeed, double radiusKm, double reliefLimit, double seaLevel, const TerrainHydro *hydro, GroundAt ground, KeepAt keep);
	bool Active() const { return m_id != 0; }

	// height: how much ground the valleys may cut into, metres. fjord: 0..1 where the main valleys also
	// cut near the sea. share: the share of valleys that exist here.
	Cut At(const vector3d &p, double height, double fjord, double share) const;

	// for the debug window, over all bodies
	static TerrainSpeedCounter &CutSpeed();
	static uint64_t ChunksBuilt();
	static double ChunkMilliseconds();
	static void ResetStats();

private:
	struct Piece {
		vector3d a, b; // km
	};
	// the branches that start in a cube of the lattice, on a grid of their own for lookups
	struct Chunk {
		vector3d low, high; // around the pieces
		double pad = 0.0; // half the longest piece
		int nx = 0, ny = 0, nz = 0;
		std::vector<uint16_t> start; // where each grid cell's pieces begin
		std::vector<Piece> pieces;
		mutable std::atomic<uint32_t> used{ 0 };
	};
	using ChunkPtr = std::shared_ptr<const Chunk>;
	// one thread's view of a level: the chunks around it, and the pieces that can be
	// the nearest anywhere within a margin of a point
	struct LevelView {
		std::array<int64_t, 6> box{};
		bool hasBox = false;
		std::vector<ChunkPtr> chunks;
		vector3d centre = vector3d(0.0, 0.0, 0.0);
		double margin = -1.0;
		std::vector<Piece> list;
	};
	struct ThreadView {
		uint64_t owner = 0;
		LevelView levels[LEVEL_COUNT];
	};

	ChunkPtr GetChunk(int k, int64_t cx, int64_t cy, int64_t cz) const;
	ChunkPtr MakeChunk(int k, int64_t cx, int64_t cy, int64_t cz) const;
	void Refresh(int k, LevelView &view, const vector3d &q, double margin) const;

	uint64_t m_id = 0;
	uint32_t m_seed = 0;
	double m_radiusKm = 1.0;
	double m_reliefLimit = 1.0;
	double m_seaLevel = -1e30;
	const TerrainHydro *m_hydro = nullptr;
	GroundAt m_ground;
	KeepAt m_keep;

	mutable std::shared_mutex m_lock; // of the chunk maps
	mutable std::recursive_mutex m_buildLock; // one chunk is made at a time, and each only once
	mutable std::unordered_map<uint64_t, ChunkPtr> m_chunks[LEVEL_COUNT];
	mutable size_t m_chunkCount = 0;
	mutable std::atomic<uint32_t> m_clock{ 0 };
};

#endif // _TERRAINPHYSICALVALLEYS_H
