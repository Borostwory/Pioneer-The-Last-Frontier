// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAIN_SURFACEPROFILE_H
#define _TERRAIN_SURFACEPROFILE_H

#include "../vector3.h"

#include <cstdint>

class SystemBody;

// Surface parameters derived from a body's physical properties. The colour
// fractals use the palette; the climate values are for the biome shading.
// Uses no random numbers at construction, so terrain heights are unaffected.
struct SurfaceProfile {
	enum Volcanism {
		VOLCANISM_NONE,
		VOLCANISM_SILICATE,
		VOLCANISM_SULPHUR,
		VOLCANISM_CRYO
	};

	explicit SurfaceProfile(const SystemBody *body);

	// climate
	double starTempK = 5778.0;
	double equatorTempK = 288.0;
	double poleTempK = 288.0;
	double lapseRateKperM = 0.0; // 0 without an atmosphere
	double snowLineM = -1.0; // < 0 when there is no snow line
	double iceCapLatitude = 90.0; // degrees
	double moistureBase = 0.0; // 0..1

	// composition
	double ironOxide = 0.0; // 0..1, rusty red tones
	double albedoBase = 0.2;
	bool icyCrust = false;

	// life and volcanism
	double vegetationCover = 0.0; // 0..1
	double vegetationHue = 30.0; // degrees
	Volcanism volcanism = VOLCANISM_NONE;
	double lavaCover = 0.0; // 0..1
	double surfaceAge = 1.0; // 0 = freshly resurfaced, 1 = old

	// colour arrays used by the rocky colour fractals
	struct Palette {
		vector3d rock[8];
		vector3d darkrock[8];
		vector3d plant[8];
		vector3d darkplant[8];
		vector3d sand[8];
		vector3d darksand[8];
		vector3d dirt[8];
		vector3d darkdirt[8];
	};
	// colours vary per body, picked with a random generator seeded from bodySeed
	void MakePalette(uint32_t bodySeed, Palette &out) const;

	void Dump() const;
};

#endif /* _TERRAIN_SURFACEPROFILE_H */
