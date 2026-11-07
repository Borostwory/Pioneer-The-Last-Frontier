// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINPHYSICALCRATERS_H
#define _TERRAINPHYSICALCRATERS_H

#include "PlanetPhysics.h"

#include "../vector3.h"

#include <cstdint>
#include <string>
#include <vector>

// The craters of the physical surface generator (planet lab: fields.py,
// craters_splat, and materials.py, crop_catalogue). The largest are in a list
// made when the body is built; smaller ones sit on lattices fixed in space, one
// per size. A crater depends only on the body and its place: a view or patch
// only decides which are large enough to show, and they fade in between 4 and 8
// samples across. Read-only after Build, and safe to read from any thread.
class TerrainCraters {
public:
	// Craters smaller than this, in km, are left to the GPU detail tiles, which draw
	// only smaller ones (TerrainPhysical::GetTileCraterTop), so none is drawn twice.
	static constexpr double SMALLEST_KM = 0.4;

	struct Sample {
		double large = 0.0; // metres, craters larger than 5 % of the radius
		double small = 0.0; // metres, the rest
		double floor = 0.0; // 0..1, the floor of an old crater, where dust gathers
		double ejecta = 0.0; // 0..1, star-shaped debris of a young crater
		double rays = 0.0; // 0..1, long rays of the youngest
		// what large is made of: rims and the like, the hollows, and the deepest one hollow
		double largeUp = 0.0, largeDown = 0.0, largeDeepest = 0.0;
	};

	// brightRays: it has some large young craters, whose bright rays stand out from afar
	void Build(const PlanetPhysics::Core &core, const PlanetPhysics::Interior &interior, const PlanetPhysics::Surface &surface, uint32_t noiseSeed, bool brightRays = false);

	// p on the unit sphere. Follows the band limit of the calling thread.
	Sample At(const vector3d &p) const;
	// For the debug tools: the craters whose height differs between two nearby points, one line each, and At()'s
	// answer beside the sum of all those around, gathered anew
	std::vector<std::string> DescribeStep(const vector3d &a, const vector3d &b) const;

	size_t ListedCount() const { return m_listed.size(); }
	size_t SizeCount() const { return m_sizes.size(); }

private:
	struct Crater {
		vector3d centre;
		double diameter; // km
		double fresh; // 0..1
		double seed; // 0..1
		double oblique; // the direction of a low-angle impact, radians, or -1
		bool giant = false; // huge for the moon it's on: a tall rim and a tall peak
	};
	// a crater with what is worked out once, for one view of it
	struct Site {
		vector3d centre, axisA, axisB;
		double cosReach, sinReach, reachAngle, angle, reach, fade;
		double colourFade; // of its debris and rays, which show from further than its shape
		double diameter, fresh, seed, oblique, ratio;
		double wear, depth, wornDepth, rim;
		bool large;
		bool giant;
	};
	// the craters of one size, on a lattice in space
	struct Size {
		double diameter; // km, the largest of this size; the smallest is half
		double density; // per square km
		double cell; // km
		uint32_t seed;
	};
	// What one thread gathered for one band limit, and of that the craters that reach within a margin of a point
	struct Nearby {
		uint64_t owner = 0;
		uint32_t used = 0;
		double band = -1.0;
		vector3d centre = vector3d(0.0, 0.0, 0.0);
		double cosRadius = 2.0;
		std::vector<Site> sites;
		vector3d activeCentre = vector3d(0.0, 0.0, 0.0);
		double cosMargin = 2.0;
		double margin = 0.0;
		std::vector<const Site *> active;
	};

	void AddSite(const Crater &crater, double spacing, std::vector<Site> &out) const;
	void Gather(const vector3d &centre, double radius, double spacing, std::vector<Site> &out) const;
	void Accumulate(const Site &site, const vector3d &p, Sample &out) const;

	std::vector<Crater> m_listed;
	std::vector<Size> m_sizes;
	uint64_t m_id = 0;
	uint32_t m_seed = 0;
	double m_radiusKm = 1.0;
	double m_transition = 1.0; // km
	double m_reliefLimit = 1.0; // metres
	double m_wearLarge = 0.0, m_wearSmall = 0.0, m_gullies = 0.0;
	double m_flatten = 1.0;
	bool m_sponge = false; // holes crowded together, deep and sharp, as on Hyperion
	// on an ice crust: the diameter, km, past which large craters sag, and how deep they are there, metres; 0 on rock
	double m_icePeakKm = 0.0, m_icePeakDepth = 0.0;
};

#endif // _TERRAINPHYSICALCRATERS_H
