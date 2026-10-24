// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAIN_EROSION_H
#define _TERRAIN_EROSION_H

#include "../vector2.h"
#include "../vector3.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

class SystemBody;
class Terrain;

// River valleys cut into the mountains of planets with liquid water.
//
// The valleys come from a pre-simulated eroded landscape
// (data/heightmaps/erosion_valleys.bin, scripts/make_erosion_valleys.py),
// tiled over the planet on the six cube faces. Erosion only lowers the
// terrain: crests keep the height fractal's height, valley floors are cut
// down by up to CUT_DEPTH_M, never by more than 60% of the height.
//
// Where it cuts depends on the large-scale terrain (features under ~20 km
// left out): the full cut where it is rugged, half the cut on high ground,
// nothing on low, flat land and under water, nor around surface starports.
//
// Erosion is part of Terrain::GetHeights, so patches and collision agree.
class TerrainErosion {
public:
	// nullptr when the body gets no erosion: no liquid water, no atmosphere,
	// too cold or hot, switched off (TerrainErosion=0) or the data is missing
	static std::unique_ptr<TerrainErosion> Create(const SystemBody *body, double planetRadius);

	// Lowers heightsOut (unit-sphere units, from Terrain::GetBaseHeights at
	// the points vP on the unit sphere) in place. Detail follows the current
	// TerrainNoise band limit.
	void Apply(const Terrain &terrain, const vector3d *vP, double *heightsOut, size_t count) const;

	// keeps the terrain around p (unit sphere) uncut, like around the
	// starports known at creation: for starports moved to flatter ground
	void AddCity(const vector3d &p);

	void Dump() const;

	~TerrainErosion();

private:
	struct Valleys;

	TerrainErosion(std::shared_ptr<const Valleys> valleys, double planetRadius, double strength, unsigned int seed);

	// valley depth at p, 0 (crest) to 1 (valley floor), blended over the cube
	// faces; lod 0 is the full resolution, each step halves it
	double ValleyAt(const vector3d &p, double lod) const;
	// 0 near a surface starport (no cut), 1 far from all of them
	double CityFade(const std::vector<vector3d> &cities, const vector3d &p) const;

	std::shared_ptr<const Valleys> m_valleys;
	double m_radius; // metres
	double m_strength; // 0..1, from the amount of liquid
	vector2d m_faceOffset[6]; // in tiles, per body
	// surface starports, on the unit sphere; replaced as a whole when one is
	// added, so height jobs on other threads keep a consistent copy
	std::shared_ptr<const std::vector<vector3d>> m_cities;
	mutable std::mutex m_citiesLock;
	std::shared_ptr<const std::vector<vector3d>> Cities() const;
};

#endif /* _TERRAIN_EROSION_H */
