// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINPHYSICAL_H
#define _TERRAINPHYSICAL_H

#include "PlanetPhysics.h"
#include "Terrain.h"
#include "TerrainPhysicalCraters.h"

#include <array>
#include <vector>

// The physical surface generator (docs/implementation_plans/
// physical-surface-generator_plan.md), behind the TerrainGenerator config key.
// Everything depends only on the body and the position, never on the view, so
// a feature is the same in every patch. Read-only after construction.
//
// So far: the long wavelengths of the shape (L0), crustal units (L1), craters and
// basins (L2, L3), old flood plains (L4a), the ground material's relief (L11), and
// what lies on the ground of airless and icy worlds. Plates, water, volcanoes,
// cracks and life follow in later phases.
class TerrainPhysical final : public Terrain {
public:
	explicit TerrainPhysical(const SystemBody *body);

	void GetBaseHeights(const vector3d *vP, double *heightsOut, const size_t count) const override;
	vector3d GetColor(const vector3d &p, double height, const vector3d &norm) const override;
	void GetSurface(const vector3d &p, double height, const vector3d &normal, vector3d &colourOut, uint8_t weightsOut[TerrainMaterialTable::SLOT_COUNT]) const override;

	const char *GetHeightFractalName() const override { return "Physical"; }
	const char *GetColorFractalName() const override { return "Physical"; }

	float GetIceCrackStrength() const override { return float(m_interior.shellStrength); }
	bool GetTileStyle(float style[4]) const override;
	double GetTileCraterTop() const override { return TerrainCraters::SMALLEST_KM * 1000.0; }
	bool HasFixedDetail() const override { return true; }
	std::vector<std::string> GetGeologyInfo() const override;

private:
	using Shares = std::array<double, TerrainMaterialTable::LAYER_COUNT>;

	// what is under a point, in metres
	struct Ground {
		double large = 0.0; // the shape, with plains and tesserae
		double base = 0.0; // the shape with its large craters
		TerrainCraters::Sample craters;
		double small = 0.0;
		double warp = 0.0;
		double plains = 0.0;
		double unitClass = 0.0;
		double unitEdge = 1.0;
		double tessera = 0.0;
	};
	enum class MoonStyle {
		MOTTLED,
		BELTS,
		PATCHWORK,
		POLAR,
		STREAKS
	};

	Ground GroundAt(const vector3d &p) const;
	double OwnRelief(const vector3d &p) const;
	double Keep(const Ground &g) const;
	double Warp(const vector3d &p) const;
	double Lag(const vector3d &p) const;
	double IceField(const vector3d &p, const Ground &g) const;
	double CrustIceField(const vector3d &p, const Ground &g) const;
	double DarkField(const vector3d &p, double warp) const;
	double Thinning(const vector3d &p, const Ground &g) const;
	void Paint(const vector3d &p, const Ground &g, Shares &shares) const;
	vector3d ColourFrom(const Ground &g, const Shares &shares, bool underWater) const;
	void BuildLevels();
	void BuildTones();
	void BuildMaterials();

	PlanetPhysics::Core m_core;
	PlanetPhysics::SurfaceInputs m_inputs;
	PlanetPhysics::Interior m_interior;
	PlanetPhysics::Surface m_state;
	TerrainCraters m_craters;

	uint32_t m_noiseSeed;
	double m_reliefLimit; // metres, at least 1
	double m_seaLevel = -1e30; // metres; -1e30: none
	double m_datum = 0.0; // metres, height 0
	double m_floodLevel = -1e30; // metres; -1e30: no plains
	double m_iceLevel = 1e30, m_iceCoreLevel = 1e30;
	double m_darkLevel = 1e30, m_brightLevel = 1e30;
	double m_northFrost = 2.0, m_southFrost = 2.0;

	MoonStyle m_moonStyle = MoonStyle::MOTTLED;
	bool m_hemispheres = false;
	vector3d m_hemisphereAxis;

	static constexpr int UNIT_COUNT = 26;
	std::array<vector3d, UNIT_COUNT> m_unitSeeds;
	std::array<double, UNIT_COUNT> m_unitClass;

	std::array<vector3d, TerrainMaterialTable::LAYER_COUNT> m_tones;
	std::array<int, TerrainMaterialTable::LAYER_COUNT> m_slots;
	vector3d m_seaTone;
};

#endif // _TERRAINPHYSICAL_H
