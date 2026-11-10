// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINLANDFORMS_H
#define _TERRAINLANDFORMS_H

#include "vector3.h"

#include <cstdint>

// The landforms of the GPU detail tiles (terrain-tile.glsl), made again for the CPU heights (P3 of
// docs/implementation_plans/opengl46-tessellation-compute_plan.md, option A of Q2). The same styles, sizes, densities
// and octave ladder: rolling hills, ridges, dunes, cracked slabs and craters. Made faster than a line-by-line copy
// (decided 2026-11-09), so the random placement differs from the tiles':
// - whole-number hashes of the cells (one per cell or corner gives all its numbers), not the tiles' fractional ones;
// - rolling hills and ridges of an octave share their corners' hashes;
// - cells counted from the planet's centre, so nothing repeats (the tiles wrap every 256 cells).
// Gives heights as well as slopes, consistent with each other: each style's height is the one whose slope the tiles use.
// Nothing uses it yet but the Landform Cost report (FrontierLog), which times it against the tiles' copy
// (TerrainTilesCPU); P3 puts it into the height function.
namespace TerrainLandforms {
	struct Params {
		double radius = 0.0; // metres
		double finestWave = 0.0; // metres: the wavelength of octave 0 here
		int finest = 0; // the index of octave 0 on the tiles' ladder (the salt)
		int count = 0; // octaves in all
		int band = 0; // the finest octaves, which hold all styles and the landform value
		float style[4] = {}; // rolling, ridged, dunes, craters
		double craterTop = 0.0; // metres; 0: no limit
		double landformTop = 0.0; // metres; 0: the band's only
		double blocks = 0.0;
	};

	struct Sample {
		double height = 0.0; // metres, about 0 on average
		vector3d slope = vector3d(0.0); // along the surface
		double form = 0.0; // the landform value, -1..1: high on crests and rims, low in hollows and crater floors
		// -1..1, for the colours: the landform value of every octave added, not only the band's, each about as strong,
		// and each counted only as far as the samples show it clearly (shadeFade); about 0 on average
		double shade = 0.0;
		int octaves = 0; // how many were added
	};

	// How an octave fades out as the samples grow too far apart to show it: in samples to its wavelength, none of it
	// up to `from`, all of it from `full` (full <= 0: no fading)
	struct Fade {
		double from = 0.0;
		double full = 0.0;
	};

	// at up (unit, planet-fixed), only the octaves of wavelengths of at least minWavelength metres (0: all), and of
	// at most maxWavelength metres (0: all). spacing: metres between the samples (0: none, so no fading); heightFade:
	// how much of each octave's height is added, shadeFade: how much it counts towards the shade
	Sample Evaluate(const Params &params, const vector3d &up, double minWavelength, double maxWavelength = 0.0, double spacing = 0.0,
		Fade heightFade = {}, Fade shadeFade = {});
} // namespace TerrainLandforms

#endif // _TERRAINLANDFORMS_H
