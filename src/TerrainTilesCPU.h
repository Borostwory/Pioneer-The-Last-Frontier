// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINTILESCPU_H
#define _TERRAINTILESCPU_H

#include "terrain/TerrainLandforms.h"
#include "vector3.h"

class GeoSphere;

// A copy on the CPU of the landforms of the GPU detail tiles (terrain-tile.glsl, terrain_tile.frag), in double
// precision, for timing only: what moving them into the CPU heights would cost (option A of Q2 in
// docs/implementation_plans/opengl46-tessellation-compute_plan.md). Nothing draws it and nothing else uses it. It does
// the same noise work per octave as the shader, so its time per point is what the heights would cost; its results are
// not the shader's to the last bit (the shader hashes in 32-bit floats).
namespace TerrainTilesCPU {
	// the same as the landforms made anew for the CPU heights (P3) take
	using Params = TerrainLandforms::Params;

	// as the tile of a patch whose vertices are vertexSpacing metres apart holds them, near the ground (TerrainTiles::
	// BandIndex with the band set by the spacing)
	Params ParamsFor(const GeoSphere *sphere, double vertexSpacing);

	struct Result {
		vector3d slope; // of the landforms, along the surface
		double form = 0.0; // the landform value, -1..1
		int octaves = 0; // how many were added
	};

	// at up (unit, planet-fixed), only the octaves of wavelengths of at least minWavelength metres (0: all).
	// allCraterCells: the craters as before P2, looking at all 27 cells around, to check that the faster way gives the
	// same result
	Result Evaluate(const Params &params, const vector3d &up, double minWavelength, bool allCraterCells = false);
} // namespace TerrainTilesCPU

#endif // _TERRAINTILESCPU_H
