// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINPHYSICALVOLCANOES_H
#define _TERRAINPHYSICALVOLCANOES_H

#include "PlanetPhysics.h"

#include "../vector3.h"

#include <cstdint>
#include <functional>
#include <vector>

class TerrainPlates;

// The volcanoes of the physical surface generator (planet lab: materials.py, vent_cones, _cone_fields;
// fields.py, the catalogue's shields, blocks and paterae). Cones and shields read heightmaps of simulated erosion
// (tools/volcano-tiles, data/terrain/volcano_*.bin), with where lava runs. All are in lists made when the body is built, each
// with a fixed number of draws, so a volcano is the same in every view: a view only decides which are large
// enough to show. They fade in between 1 and 2 samples to a radius. Read-only after Build, and safe to read
// from any thread.
//
// Two kinds of thing. What the interior built long ago (old shields, Io's mountain blocks and paterae) is part
// of the ground, which the map and all else keep to. Vents come from the surface's volcanicity and stand on
// that ground: they count as ground for the sea and snow, but mountains away from them don't change.
class TerrainVolcanoes {
public:
	enum Kind {
		CONE, // steep and concave, over sinking plates
		SHIELD, // wide and gentle, from hotspots and on worlds with no plates
		PIT // a low rim around a wide flat floor, on heat-pipe worlds like Io
	};

	// what the vents make of a point
	struct Sample {
		double height = 0.0; // metres
		double influence = 0.0; // 1 on a volcano, fading to 0 just outside it
		double slope = 0.0; // 1 on its slopes, which lava runs off; its foot is uneven
		double smooth = 0.0; // how much of the small relief a volcano's own shape replaces: most on a young one
		double field = 0.0; // the lava a volcano spreads around itself, in uneven lobes: 1 at the vent, to nothing at about 3 radii
		bool nearby = false; // within about 2 radii of a volcano, where no rivers or lakes lie
		double lava = 0.0, rock = 0.0, flow = 0.0, old = 0.0, ash = 0.0; // shares
		double floor = 0.0; // 1 on the level floor of a crater or pit
		double level = 0.0; // the height of that floor, metres
		int kind = -1; // the kind of volcano here, -1: none
		bool erupting = false;
	};
	// what the interior built at a point
	struct Construct {
		double height = 0.0; // metres
		double block = 0.0; // 0..1, on a mountain block
		double patera = 0.0; // 0..1, on a patera's floor
		double pateraLava = 0.0; // 0..1, the same, if lava lies in it
	};
	// a flow from a vent: a tongue, narrow near it and wider at the toe, bending on the way down
	struct Tongue {
		double direction, length, bend, width;
	};
	struct Vent {
		vector3d centre, axisA, axisB;
		double radius; // on the unit sphere
		double cosReach;
		double km; // its radius
		double height; // metres
		double rim; // of the radius, the edge of its crater
		double seed; // 0..1
		double ground; // metres, the map's ground under it: what the level of its crater's floor is fixed from
		int map; // which heightmap; -1: a pit, with a shape of its own
		double turnCos, turnSin; // how the map is turned
		double mirror; // 1 or -1
		Kind kind;
		bool erupting, recent;
		std::vector<Tongue> tongues;
	};
	struct Item {
		vector3d centre, axisA, axisB;
		double radius; // on the unit sphere
		double cosReach;
		double height; // metres; a patera's depth
		double seed; // 0..1
	};

	// before the ground is worked out, which includes these
	void BuildConstructs(const PlanetPhysics::Core &core, const PlanetPhysics::Interior &interior, uint32_t noiseSeed, double reliefLimit);
	// groundAt: the map's ground, in metres. plates: nullptr if the body has none. seaLevel: -1e30 if none.
	void BuildVents(const PlanetPhysics::Core &core, const PlanetPhysics::SurfaceInputs &inputs, const PlanetPhysics::Interior &interior,
		const PlanetPhysics::Surface &surface, double reliefLimit, double seaLevel, const TerrainPlates *plates,
		const std::function<double(const vector3d &)> &groundAt);

	// p on the unit sphere. Both follow the band limit of the calling thread.
	Sample At(const vector3d &p) const;
	Construct ConstructAt(const vector3d &p) const;

	const std::vector<Vent> &Vents() const { return m_vents; }
	const std::vector<Item> &Blocks() const { return m_blocks; }
	const std::vector<Item> &Paterae() const { return m_paterae; }
	// lava lakes around the erupting vents; their height is the lake's level
	const std::vector<Item> &Lakes() const { return m_lakes; }
	// the middles of the lava fields, with how far (on the unit sphere) each reaches; also on ice crusts,
	// which have no volcanoes as such but cold flows around these places
	const std::vector<vector3d> &FieldPlaces() const { return m_places; }
	double FieldSpread() const { return m_spread; }
	double Tallest() const { return m_tallest; }

private:
	// which items reach into each cell of a latitude-longitude grid
	struct Index {
		std::vector<std::vector<uint16_t>> cells;
		void Add(const vector3d &centre, double reach, uint16_t id);
		const std::vector<uint16_t> *At(const vector3d &p) const;
	};

	void AddVent(const Vent &vent, const vector3d &p, double spacing, Sample &out) const;
	// a cone or shield, from its heightmap; t0: the distance from its middle in radii; theta: the direction
	void AddMapped(const Vent &vent, const vector3d &p, double spacing, double fade, double t0, double theta, Sample &out) const;
	void BuildLakes(const PlanetPhysics::Core &core, const PlanetPhysics::Surface &surface, bool pits, double reliefLimit, double seaLevel, const std::function<double(const vector3d &)> &groundAt);

	std::vector<Item> m_blocks, m_paterae, m_shields, m_lakes;
	std::vector<Vent> m_vents;
	std::vector<vector3d> m_places;
	double m_spread = 0.2;
	Index m_blockIndex, m_pateraIndex, m_shieldIndex, m_ventIndex, m_lakeIndex;
	double m_tallest = 0.0;
	uint32_t m_noiseSeed = 0;
	double m_wear = 0.0;
	bool m_glowing = false;
	bool m_weather = false;
};

#endif // _TERRAINPHYSICALVOLCANOES_H
