// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAIN_ASTEROID_H
#define _TERRAIN_ASTEROID_H

#include "FracDef.h"
#include "TerrainNoise.h"
#include "../Quaternion.h"
#include "../vector3.h"

// Shared by the asteroid height fractals. Their heights are fractions of the
// relief, which is at most ASTEROID_RELIEF_PER_RADIUS of the radius
// (Terrain.cpp). A height field cannot make a real potato, so the body is a
// stretch along one axis plus a few broad lobes, with landforms of a few per
// cent of the radius on top: the slopes stay those of rock, not of spikes.
//
// Fracdef slots: 0 the lobes (feature width about the radius), 1 the
// stretch (amplitude only, the fraction of the relief), m_quatDefs[1] the
// stretch axis. The fractals use 2 and up for their own landforms.
namespace TerrainAsteroid {

	constexpr double LOBES_WEIGHT = 0.55;
	// the most the stretch takes of the relief
	constexpr double MAX_STRETCH = 0.4;
	// persistence of the landforms' octaves: at 0.5 each octave is as steep
	// as the one before, so the finest were as steep as the largest and the
	// surface looked crumpled; below 0.5 rock smooths out at small scales.
	// The fractals scale their landforms by 0.5 / DETAIL_PERSISTENCE so the
	// largest octave keeps its height.
	constexpr double DETAIL_PERSISTENCE = 0.4;

	// broad lobes in [0, 1]: octave noise only spreads about +-0.12 around
	// 0.5, so widen it and ease into the limits rather than cutting them flat
	inline double lobes(const fracdef_t &def, const vector3d &p)
	{
		const double n = Clamp(0.5 + 2.0 * (TerrainNoise::octavenoise(def, 0.5, p) - 0.5), 0.0, 1.0);
		return n * n * (3.0 - 2.0 * n);
	}

	// the body's overall shape, in [0, LOBES_WEIGHT + MAX_STRETCH]
	inline double shape(const fracdef_t &lobesDef, const fracdef_t &stretchDef, const Quaterniond &stretchAxis, const vector3d &p)
	{
		const double along = (stretchAxis * p).y;
		return stretchDef.amplitude * along * along + LOBES_WEIGHT * lobes(lobesDef, p);
	}

} // namespace TerrainAsteroid

#endif
