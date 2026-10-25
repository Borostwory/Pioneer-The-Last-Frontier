// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _CITYLAYOUT_H
#define _CITYLAYOUT_H

#include "Random.h"

#include <cstdint>
#include <functional>
#include <vector>

// The plan of a city round a surface starport (CityOnPlanet): its shape,
// picked from the planet and the ground round the starport, its districts,
// main roads and streets, and what each cell of its grid holds. CityOnPlanet
// puts the buildings (the 3D models) on it, and the terrain shaders draw its
// roads (city-ground.glsl).
//
// Everything is in city space: metres in the starport's plane, x and z along
// its axes, the starport at the origin. The grid's cells are CELL wide, cell
// (ext, ext) centred on the starport, as CityOnPlanet has always had them.
//
// The shape:
// - planets where the air can't be breathed get domes: one over the city,
//   several (few people, low gravity, rough ground), or one over the floor
//   of a crater near the starport; the starport is always under one, and the
//   only roads outside them join two domes
// - elsewhere, from the ground round the starport: a strip along the coast,
//   islands, a line along a valley, a hillside; or from the planet: a planned
//   grid (billions of people, core worlds), towns strung along a highway
//   (few people, farming, frontier); else a sprawl. Never a circle in the
//   open air: a heart and a few lobes, stretched along one way, ragged
// Each shape scores from what fits, with a little of the starport's seed so
// alike worlds differ; the same starport always gets the same city.
//
// Roads: main roads found over the terrain (steep ground and crossings cost
// more, roads already built less, so they merge into trunks) between the
// districts, the starport and the country; streets on the cell edges, every
// few cells by the district, along the contours. No road runs into the sea.
class CityLayout {
public:
	enum class Shape : uint8_t {
		ROUND, // a sprawl of lobes, not a circle
		COASTAL,
		ISLANDS,
		VALLEY,
		HILLSIDE,
		GRID,
		HIGHWAY,
		DOME,
		DOME_CLUSTER,
		CRATER,
	};

	enum class District : uint8_t {
		NONE,
		DOWNTOWN,
		RESIDENTIAL,
		INDUSTRY,
		LOGISTICS, // warehouses by the starport
		FRINGE,	   // farms and outposts by the main roads outside the city
	};

	// the ground of a cell (city-ground.glsl)
	enum Lot : uint8_t {
		LOT_NONE,
		LOT_PAVED,
		LOT_YARD,
		LOT_APRON,
		LOT_PARKING,
	};

	enum RoadType : uint8_t {
		ROAD_LOCAL,
		ROAD_ARTERIAL,
		ROAD_CENTRE, // downtown streets, wider pavements
	};

	// what the planet and its system tell about the city
	struct Facts {
		double radius = 2000.0;		 // CityOnPlanet::CalcCityRadius, m
		double population = 0.0;	 // the planet's, billions
		double gravity = 1.0;		 // at the surface, g
		double water = 0.0;			 // SystemBody::GetVolatileLiquid
		double agricultural = 0.0;	 // SystemBody::GetAgricultural
		double industrial = 0.0;	 // StarSystem::GetIndustrial
		double humanProx = 0.0;		 // StarSystem::GetHumanProx
		bool needsDome = false;		 // CityOnPlanet::NeedsForcefield
		double domeHeightRatio = 0.4; // ForcefieldDef::heightRatio
		double domeClearance = 20.0;  // over the buildings, m
		uint32_t seed = 0;
		uint32_t portCells[2] = { 1, 1 }; // the starport's footprint, cells
	};

	// Heights of the ground above the planet's radius (m) at city-space
	// points (x, z pairs), and whether each is under the sea; detail narrower
	// than minFeature metres may be left out.
	using Ground = std::function<void(const std::vector<double> &xz, std::vector<double> &heights, std::vector<uint8_t> &sea, double minFeature)>;

	struct Segment {
		float x0, z0, x1, z1;
		float width; // of the carriageway, m
		float walk;	 // pavement on each side, m
		uint8_t type;
		uint32_t line; // the segments of one road share it
		float along;   // distance along the road at x0, z0, m
	};

	struct Dome {
		double x, z;   // centre, city space
		double radius; // at the ground, m
	};

	static constexpr double CELL = 50.0; // CityOnPlanet::CELLSIZE

	CityLayout(const Facts &facts, const Ground &ground);

	Shape GetShape() const { return m_shape; }
	static const char *ShapeName(Shape shape);

	// the grid
	int GetSize() const { return m_n; }
	double GetCorner() const { return m_x0; } // x and z of the grid's corner
	double CellX(int i) const { return m_x0 + (i + 0.5) * CELL; }
	double CellZ(int j) const { return m_x0 + (j + 0.5) * CELL; }
	bool InGrid(int i, int j) const { return i >= 0 && j >= 0 && i < m_n && j < m_n; }

	District GetDistrict(int i, int j) const { return m_cellDistrict[Idx(i, j)]; }
	// built up (any district but the fringe), dry, not cleared for a main road, not the starport
	bool IsBuildable(int i, int j) const;
	// outside the city, beside a main road: room for a farm or an outpost
	bool IsFringe(int i, int j) const;
	bool IsSea(int i, int j) const { return m_sea[Idx(i, j)]; }
	bool IsPort(int i, int j) const { return m_port[Idx(i, j)]; }
	// height range of the ground over the cell, m
	double CellSlope(int i, int j) const { return m_slope[Idx(i, j)]; }
	// sampled ground height above the planet's radius (m), between samples
	double GroundAt(double x, double z) const;
	// metres from the city's heart
	double DistanceToCentre(double x, double z) const;

	// Room to leave from each side of a cell for a street on that edge (its
	// half width, pavement and a margin, m; 0 without one): -x, +x, -z, +z
	void Setbacks(int i, int j, double out[4]) const;
	// whether a street runs between two neighbouring cells
	bool StreetBetween(int i0, int j0, int i1, int j1) const;
	// cells beside a main road: the road's direction (radians, as
	// matrix3x3::RotateY turns +x) and its distance from the cell's centre
	bool BesideMainRoad(int i, int j, double &yaw, double &distance) const;

	uint8_t GetLot(int i, int j) const { return m_lots[Idx(i, j)]; }
	void SetLot(int i, int j, Lot lot) { m_lots[Idx(i, j)] = lot; }
	const std::vector<uint8_t> &GetLots() const { return m_lots; }

	const std::vector<Segment> &GetSegments() const { return m_segments; }
	const std::vector<Dome> &GetDomes() const { return m_domes; }
	// the dome over (x, z), or -1
	int DomeAt(double x, double z) const;
	// how high a building at (x, z) may reach over the ground under its
	// dome's centre and stay clear of it (m); a large number outside domes
	double HeightCap(double x, double z) const;

	// Clearance of points (x, z pairs) from every road and its pavement:
	// metres from the nearest, negative on one
	double Clearance(const double *xz, size_t count) const;

	// The roads packed for city-ground.glsl, in rows TEXEL_WIDTH texels wide:
	// per lookup cell (LOOKUP_CELL square, two to a texel) the first index and
	// the count, then the segment indices four to a texel, then per segment
	// two texels: x0, z0, x1, z1 and width, pavement, line * 4 + type, along.
	// info: lookup cells per side, first index texel, first segment texel,
	// rows; rect: x, z of the lookup grid's corner, cell size, cells per side.
	void PackRoads(std::vector<float> &atlas, float info[4], float rect[4]) const;

	static constexpr uint32_t TEXEL_WIDTH = 1024;
	static constexpr double LOOKUP_CELL = 100.0;
	static constexpr uint32_t LOOKUP_MAX = 24; // segments drawn per lookup cell

private:
	size_t Idx(int i, int j) const { return size_t(j) * m_n + i; }

	struct Analysis;

	void SampleGround(const Ground &ground);
	void Analyse(Analysis &a) const;
	void ChooseShape(const Analysis &a);
	void Outline(const Analysis &a);
	void PlaceDistricts();
	void LayMainRoads(const Analysis &a);
	void LayStreets();
	void BuildLookup();

	// a road through the points, smoothed and cut into segments
	void AddRoad(const std::vector<double> &xz, RoadType type);
	void RoadDims(RoadType type, float &width, float &walk) const;
	// cheapest path between cells over the ground
	std::vector<int> FindPath(int from, int to, const std::vector<float> &cost, const std::vector<uint8_t> &used) const;

	Facts m_facts;
	Random m_rng;
	Shape m_shape = Shape::ROUND;
	int m_n = 0;
	int m_ext = 0;
	double m_x0 = 0.0;
	double m_cx = 0.0, m_cz = 0.0; // the city's heart
	double m_axis = 0.0;		   // its long axis (coast, valley, highway), radians

	// the ground, sampled every m_step metres from the grid's corner
	double m_step = 100.0;
	int m_m = 0;
	std::vector<double> m_coarse;
	std::vector<uint8_t> m_coarseSea;

	// per cell
	std::vector<double> m_height;
	std::vector<float> m_slope;
	std::vector<uint8_t> m_sea;
	std::vector<uint8_t> m_port;
	std::vector<uint8_t> m_developed;
	std::vector<uint8_t> m_roadCell;
	std::vector<District> m_cellDistrict;
	std::vector<int16_t> m_cellSeed;
	std::vector<uint8_t> m_lots;
	std::vector<float> m_density;
	// nearest main road: distance from the cell's centre and direction
	std::vector<float> m_mainDist;
	std::vector<float> m_mainYaw;

	// districts
	struct Seed {
		int i, j;
		District kind;
		int block;		   // street every this many cells
		int offsetX, offsetZ;
	};
	std::vector<Seed> m_seeds;
	int m_gate = -1;			// the starport's gate cell
	std::vector<int> m_towns;	// highway towns' cells
	double m_mainClear = 40.0;	// cells nearer a main road than this are left for it

	// street codes on the cell edges (0 none, else RoadType + 1):
	// m_vEdge[j * (n + 1) + k] between cells (k - 1, j) and (k, j),
	// m_hEdge[k * n + i] between cells (i, k - 1) and (i, k)
	std::vector<uint8_t> m_vEdge;
	std::vector<uint8_t> m_hEdge;

	std::vector<Segment> m_segments;
	uint32_t m_lines = 0;
	std::vector<Dome> m_domes;

	// segments by lookup cell, for Clearance
	int m_lookN = 0;
	std::vector<std::vector<uint32_t>> m_lookup;
};

#endif /* _CITYLAYOUT_H */
