// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINPHYSICAL_H
#define _TERRAINPHYSICAL_H

#include "PlanetPhysics.h"
#include "Terrain.h"
#include "TerrainPhysicalCraters.h"
#include "TerrainPhysicalHydro.h"
#include "TerrainPhysicalPlates.h"
#include "TerrainPhysicalValleys.h"

#include <array>
#include <vector>

// The physical surface generator (docs/implementation_plans/
// physical-surface-generator_plan.md), behind the TerrainGenerator config key.
// Everything depends only on the body and the position, never on the view, so
// a feature is the same in every patch. Read-only after construction.
//
// So far: the long wavelengths of the shape (L0), crustal units (L1), craters and
// basins (L2, L3), old flood plains (L4a), plates and continents (L7), the sea and
// its real depth, coasts, rain, drainage, valleys, main rivers and lakes (L8), ice caps
// (L10), climate and plants (Section 7), and the ground material's relief (L11). Side
// rivers, dunes, volcanoes and cracks follow.
class TerrainPhysical final : public Terrain {
public:
	explicit TerrainPhysical(const SystemBody *body);

	void GetBaseHeights(const vector3d *vP, double *heightsOut, const size_t count) const override;
	vector3d GetColor(const vector3d &p, double height, const vector3d &norm) const override;
	void GetSurface(const vector3d &p, double height, const vector3d &normal, vector3d &colourOut, uint8_t weightsOut[TerrainMaterialTable::SLOT_COUNT]) const override;
	double GetSurfaceAltitude(const vector3d &p, double height) const override;

	const char *GetHeightFractalName() const override { return "Physical"; }
	const char *GetColorFractalName() const override { return "Physical"; }

	float GetIceCrackStrength() const override { return float(m_interior.shellStrength); }
	bool GetTileStyle(float style[4]) const override;
	double GetTileCraterTop() const override { return TerrainCraters::SMALLEST_KM * 1000.0; }
	bool HasFixedDetail() const override { return true; }
	bool HasWaterAround(const vector3d &p, double radiusMetres) const override;
	std::vector<std::string> GetGeologyInfo() const override;
	std::vector<Feature> FindFeatures(const std::string &kind, const vector3d &around) const override;
	std::string DescribePoint(const vector3d &p) const override;

private:
	using Shares = std::array<double, TerrainMaterialTable::LAYER_COUNT>;

	// what is under a point, in metres
	struct Ground {
		double large = 0.0; // the shape, with plates, plains and tesserae
		double base = 0.0; // the shape with its large craters
		TerrainCraters::Sample craters;
		double small = 0.0;
		double warp = 0.0;
		double plains = 0.0;
		double unitClass = 0.0;
		double unitEdge = 1.0;
		double tessera = 0.0;
	};
	// everything worked out for a point, heights and water together
	struct Point {
		Ground g;
		TerrainHydro::Place place;
		double temperature = 288.0;
		double rain = 0.0;
		double coast = 0.0;
		double shore = 0.0; // the ground with its coast, which decides land or sea
		double height = 0.0; // the finished ground, metres
		bool underSea = false;
		double lakeField = 0.0;
		bool carved = false; // valleys are cut here
		double valley = 0.0; // 0 on the ridges, 1 on the valley floors
		bool hasRiver = false;
		TerrainHydro::River river;
		double sideEdge = 1e30; // metres from the edge of a side river, below 0 in it
		double sideWidth = 0.0; // metres
		double quiet = 1.0; // 0 along a side river or at a small lake: small relief stays out of its way
		double smallLakeRatio = 1e30; // the distance to the nearest small lake's middle, in its radii; below 1 in it
		double smallLakeRadius = 0.0; // metres
		vector3d smallLakePlace = vector3d(0.0, 0.0, 0.0);
		double smallLakeEdge = 1e30; // metres from its shore, below 0 in it
	};
	enum class MoonStyle {
		MOTTLED,
		BELTS,
		PATCHWORK,
		POLAR,
		STREAKS
	};

	Ground GroundAt(const vector3d &p) const;
	void Evaluate(const vector3d &p, Point &out) const;
	// the water's height in a small lake, the same in every view
	double SmallLakeLevel(const vector3d &place, double radius) const;
	double Temperature(const vector3d &p, double warp) const;
	double Coast(const vector3d &p, const Point &point, double plainGround) const;
	// cuts the valleys into point.g.base and returns the peaks along the ridges; calm: 0..1, how much of them
	double CutValleys(const vector3d &p, Point &point, double plainGround, double calm) const;
	void BuildShares();
	double OwnRelief(const vector3d &p, const Ground &g) const;
	double Keep(const Ground &g) const;
	double Warp(const vector3d &p) const;
	double Lag(const vector3d &p) const;
	double WaterEdge(const vector3d &p) const;
	double IceField(const vector3d &p, const Ground &g) const;
	double CrustIceField(const vector3d &p, const Ground &g) const;
	double DarkField(const vector3d &p, double warp) const;
	double DuneField(const vector3d &p, const Ground &g) const;
	double Thinning(const vector3d &p, const Ground &g) const;
	void Paint(const vector3d &p, const Point &point, Shares &shares) const;
	void Climate(const vector3d &p, const Point &point, const std::function<void(TerrainMaterialTable::Layer, double)> &paint) const;
	void Steep(const vector3d &p, const vector3d &normal, Shares &shares) const;
	double AltitudeOf(const Point &point, const Shares &shares) const;
	vector3d ColourFrom(const Point &point, const Shares &shares, double altitude) const;
	void BuildLevels();
	void BuildTones();
	void BuildMaterials();

	PlanetPhysics::Core m_core;
	PlanetPhysics::SurfaceInputs m_inputs;
	PlanetPhysics::Interior m_interior;
	PlanetPhysics::Surface m_state;
	TerrainCraters m_craters;
	TerrainPlates m_plates;
	TerrainHydro m_hydro;
	TerrainValleys m_valleys;
	TerrainHydro::Field m_ground; // the ground on a grid, which all else keeps to
	double m_valleyAmount = 0.0;
	double m_valleySea = 0.0; // what valleys count their depth from
	std::string m_sharesReadout;

	uint32_t m_noiseSeed;
	double m_reliefLimit; // metres, at least 1
	double m_seaLevel = -1e30; // metres; -1e30: none
	double m_datum = 0.0; // metres, height 0
	double m_floodLevel = -1e30; // metres; -1e30: no plains
	double m_iceLevel = 1e30, m_iceCoreLevel = 1e30;
	double m_darkLevel = 1e30, m_brightLevel = 1e30;
	double m_northFrost = 2.0, m_southFrost = 2.0;
	double m_sandLevel = 1e30, m_mudLevel = 1e30, m_saltLevel = 1e30, m_beltLevel = 1e30;
	double m_steepSlope = 1.0; // what counts as steep at the colours' scale, as a tangent
	double m_buildMilliseconds = 0.0;

	MoonStyle m_moonStyle = MoonStyle::MOTTLED;
	bool m_hemispheres = false;
	vector3d m_hemisphereAxis;

	static constexpr int UNIT_COUNT = 26;
	std::array<vector3d, UNIT_COUNT> m_unitSeeds;
	std::array<double, UNIT_COUNT> m_unitClass;

	std::array<vector3d, TerrainMaterialTable::LAYER_COUNT> m_tones;
	std::array<int, TerrainMaterialTable::LAYER_COUNT> m_slots;
	vector3d m_seaTone, m_lakeTone, m_riverTone, m_shallowTone;
	vector3d m_vegetation, m_tint;
	double m_bright = 1.0;
};

#endif // _TERRAINPHYSICAL_H
