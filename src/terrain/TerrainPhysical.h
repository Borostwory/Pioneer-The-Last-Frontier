// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINPHYSICAL_H
#define _TERRAINPHYSICAL_H

#include "PlanetPhysics.h"
#include "Terrain.h"

#include <array>
#include <vector>

// The physical surface generator (docs/implementation_plans/
// physical-surface-generator_plan.md), behind the TerrainGenerator config key.
// Everything depends only on the body and the position, never on the view, so
// a feature is the same in every patch. Read-only after construction.
//
// For now: the long wavelengths of the shape (L0), crustal units as colour (L1),
// the ground material's own relief (L11), and the ground, ice caps and sea.
// Craters, plates, volcanoes and the rest follow, so bodies are nearly featureless.
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
	std::vector<std::string> GetGeologyInfo() const override;

private:
	// metres, with the mean surface near 0
	double LargeHeight(const vector3d &p) const;
	double OwnRelief(const vector3d &p) const;
	double Warp(const vector3d &p) const;
	double IceField(const vector3d &p, double largeHeight) const;
	double UnitClass(const vector3d &p) const;
	// the share of each layer at a point (ice, snow, the ground), adding up to 1
	void Shares(const vector3d &p, double largeHeight, double &ice, double &snow) const;
	vector3d ColourFrom(const vector3d &p, double ice, double snow, bool underWater) const;
	void BuildLevels();
	void BuildTones();

	PlanetPhysics::Core m_core;
	PlanetPhysics::SurfaceInputs m_inputs;
	PlanetPhysics::Interior m_interior;
	PlanetPhysics::Surface m_state;

	uint32_t m_noiseSeed;
	double m_reliefLimit; // metres, at least 1
	double m_seaLevel = -1e30; // metres; -1e30: none
	double m_datum = 0.0; // metres, height 0
	double m_iceLevel = 1e30, m_iceCoreLevel = 1e30;

	static constexpr int UNIT_COUNT = 26;
	std::array<vector3d, UNIT_COUNT> m_unitSeeds;
	std::array<double, UNIT_COUNT> m_unitClass;

	std::array<vector3d, TerrainMaterialTable::LAYER_COUNT> m_tones;
	vector3d m_seaTone;
};

#endif // _TERRAINPHYSICAL_H
