// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINPHYSICALHYDRO_H
#define _TERRAINPHYSICALHYDRO_H

#include "../vector3.h"

#include <cstdint>
#include <functional>
#include <vector>

// Rain, drainage, lakes and main rivers of the physical surface generator
// (planet lab: materials.py, _moisture, _hydrology; rivers.py). Worked out once
// when the body is built, on coarse grids; read-only after that, and safe to read
// from any thread. Rivers and lakes are the same in every view: a view only decides
// which of a line's bends it can resolve.
class TerrainHydro {
public:
	// where a point falls on a latitude-longitude grid, worked out once per point
	struct Place {
		double row01, col01;
	};
	static Place PlaceOf(const vector3d &p);

	// a field on a latitude-longitude grid, wrapping east to west
	struct Field {
		int width = 0, height = 0;
		std::vector<float> values;
		float low = 0.f, high = 0.f;

		void Resize(int w, int h);
		bool Empty() const { return values.empty(); }
		float &At(const int r, const int c) { return values[size_t(r) * width + c]; }
		float At(const int r, const int c) const { return values[size_t(r) * width + c]; }
		void FindRange();
		// straight-line, for fields read while routing; 0 if empty
		double Linear(const Place &place) const;
		// smooth, because straight-line sampling leaves the grid's cells visible in close relief
		double Smooth(const Place &place) const;
		void Blur(double sigma);
	};

	struct River {
		double distance = 1e30; // metres from the edge it's drawn with, below 0 in it
		double trueDistance = 1e30; // metres from its real edge: from afar it's drawn wider than it is
		double centre = 1e30; // metres from its middle
		double halfWidth = 0.0; // metres
		double valley = 0.0; // how far its valley reaches, metres
		double level = 0.0; // the water's height, metres
	};

	struct Params {
		uint32_t noiseSeed = 0;
		double radiusMetres = 1.0;
		double reliefLimit = 1.0;
		double seaLevel = -1e30; // metres; -1e30: none
		double seaShare = 0.0;
		double temperature = 288.0;
		double airDensity = 0.0;
		bool wet = false; // it rains: rivers and lakes, as well as valleys
	};

	// h0: the ground on a grid, warp: the body's warp noise
	void BuildMoisture(const Params &params, const Field &h0, const std::function<double(const vector3d &)> &warp);
	void BuildDrainage(const Params &params, const Field &h0);
	void BuildSlope(const Field &h0, double radiusMetres);
	// the sea level changed: rivers and valleys keep above it
	void SetSeaLevel(double seaLevel);

	bool HasMoisture() const { return !moisture.Empty(); }
	size_t RiverPieces() const { return m_segments.size(); }

	// Rivers on the map run at the map's levels, but the ground is more detailed than the map, so here and
	// there it lies below them, and level water would stand up out of it. This brings each river's level
	// down to the lowest ground along it, never rising downstream. ground: the height at a point.
	void SettleLevels(const std::function<double(const vector3d &)> &ground);

	// the river nearest p. False if there is none near. Follows the band limit of the calling thread.
	bool RiverAt(const vector3d &p, River &out) const;
	// the rivers' nodes, for the debug tools: all, those where rivers join, and those where a river ends
	struct RiverNode {
		vector3d position;
		double width; // metres
	};
	void RiverNodes(std::vector<RiverNode> &all, std::vector<RiverNode> &forks, std::vector<RiverNode> &ends) const;
	// points along the main rivers near p, and their directions, for side valleys to join
	void RiverPoints(const vector3d &p, double radius, double pieceMetres, std::vector<vector3d> &points, std::vector<vector3d> &tangents) const;

	Field moisture; // 0..1
	Field valley; // metres the map's valleys cut
	Field drowned; // metres the large ones cut near the sea: inlets
	Field mouth; // deltas
	Field corridor; // green along rivers
	Field lake; // 0..1
	Field dry; // 0..1, salt flats and mud
	Field surface; // the height of the water in a river or lake, metres
	Field slope; // the ground's broad slope, capped

private:
	struct Segment {
		vector3d a, b, middle;
		float levelA, levelB;
		float halfWidth;
		float reach; // angle, from the middle
	};
	struct Piece {
		vector3d a, b;
		float levelA, levelB;
		float halfWidth;
	};
	// What one thread gathered for one band limit, and of that the pieces that can be the
	// nearest anywhere within a margin of a point
	struct Nearby {
		uint64_t owner = 0;
		uint32_t used = 0;
		double band = -1.0;
		vector3d centre = vector3d(0.0, 0.0, 0.0);
		double cosRadius = 2.0;
		std::vector<Piece> pieces;
		vector3d activeCentre = vector3d(0.0, 0.0, 0.0);
		double cosMargin = 2.0;
		double marginMetres = 0.0;
		double smallestHalfWidth = 0.0; // metres: what the samples of this band limit can't miss
		std::vector<const Piece *> active;
	};
	void Gather(const vector3d &centre, double radius, double spacing, std::vector<Piece> &out) const;
	void SplitLine(const Segment &s, double target, std::vector<vector3d> &line, std::vector<vector3d> &next) const;

	std::vector<Segment> m_segments;
	uint64_t m_id = 0;
	uint32_t m_seed = 0;
	double m_radius = 1.0;
	double m_seaLevel = -1e30;
};

// Runs a function for each row, on several threads: the processor's cores (up to 16), shared out between
// the terrains being built at the time (ScopedSharedCores)
void ParallelRows(int rows, const std::function<void(int)> &row);

// Held while a terrain is built alongside others (Terrain::Prebuild), so that each one's ParallelRows takes
// its share of the cores and not all of them
class ScopedSharedCores {
public:
	ScopedSharedCores();
	~ScopedSharedCores();
	ScopedSharedCores(const ScopedSharedCores &) = delete;
	ScopedSharedCores &operator=(const ScopedSharedCores &) = delete;
};

#endif // _TERRAINPHYSICALHYDRO_H
