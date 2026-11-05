// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINPHYSICALPLATES_H
#define _TERRAINPHYSICALPLATES_H

#include "../vector3.h"

#include <cstdint>
#include <vector>

// Plates, ranges and rifts of the physical surface generator (planet lab:
// fields.py, catalogue, _plate_fields, _along_lines). Made when the body is built
// from its seed, read-only after that, and safe to read from any thread.
class TerrainPlates {
public:
	struct Bands {
		double continent = 0.0;
		double collision = 0.0; // continent against continent: a wide high range, well inland
		double coastRange = 0.0; // ocean under continent
		double arc = 0.0; // and the volcanoes a little further in
		double trench = 0.0;
		double islandArc = 0.0; // ocean under ocean
		double ridge = 0.0; // spreading: a mid-ocean ridge
		double ridgeRift = 0.0; // with a rift along it
		double rift = 0.0; // or a rift valley on a continent
	};
	struct Plateau {
		vector3d centre;
		double radius;
		double height; // share of the relief limit
	};

	void Build(uint32_t seed);
	bool Active() const { return !m_seeds.empty(); }

	// p on the unit sphere, warp: the body's warp noise there
	Bands At(const vector3d &p, double warp) const;
	// old worn ranges and rift valleys: the sum of share of the relief limit times how close p is
	double Ranges(const vector3d &p) const { return Along(m_ranges, p); }
	double Rifts(const vector3d &p) const { return Along(m_rifts, p); }
	const std::vector<Plateau> &Plateaus() const { return m_plateaus; }

private:
	struct Line {
		std::vector<vector3d> points;
		vector3d centre;
		double cosReach;
		double height;
		double width;
	};
	double Along(const std::vector<Line> &lines, const vector3d &p) const;

	int m_count = 0;
	std::vector<vector3d> m_seeds;
	std::vector<char> m_continental;
	// per pair of plates, m_count x m_count
	std::vector<char> m_converging, m_diverging;
	std::vector<float> m_amplitude, m_offset;
	std::vector<Line> m_ranges, m_rifts;
	std::vector<Plateau> m_plateaus;
};

#endif // _TERRAINPHYSICALPLATES_H
