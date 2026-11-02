// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainAsteroid.h"
#include "TerrainFeature.h"
#include "TerrainNoise.h"

using namespace TerrainNoise;
using namespace TerrainFeature;

// Fractured rock: sharp ridges, rougher in some regions than others

template <>
const char *TerrainHeightFractal<TerrainHeightAsteroid2>::GetHeightFractalName() const { return "Asteroid2"; }

template <>
TerrainHeightFractal<TerrainHeightAsteroid2>::TerrainHeightFractal(const SystemBody *body) :
	Terrain(body)
{
	const double r = m_planetRadius;
	const double lobeWidth = m_rand.Double(1.2, 2.5) * r;
	SetFracDef(0, m_maxHeightInMeters, lobeWidth, lobeWidth / 8.0);
	SetFracDef(1, m_rand.Double(0.0, TerrainAsteroid::MAX_STRETCH) * m_maxHeightInMeters, r);
	m_quatDefs[1].SetAxisAngle(m_rand.Double(0.0, DEG2RAD(360.0)), GetVector3Random(m_rand));

	// ridges
	SetFracDef(2, m_maxHeightInMeters * 0.1, m_rand.Double(0.15, 0.3) * r);
	// where the ridges are rough
	SetFracDef(3, m_maxHeightInMeters, m_rand.Double(0.4, 0.8) * r, 0.1 * r);
}

template <>
void TerrainHeightFractal<TerrainHeightAsteroid2>::GetBaseHeights(const vector3d *vP, double *heightsOut, const size_t count) const
{
	for (size_t i = 0; i < count; i++) {
		const vector3d &p = vP[i];
		double n = TerrainAsteroid::shape(m_fracdef[0], m_fracdef[1], m_quatDefs[1], p);
		const double roughness = 0.1 + 0.2 * octavenoise(m_fracdef[3], 0.5, p);
		n += roughness * (ridged_octavenoise(m_fracdef[2], 0.5, p) - 0.65);
		heightsOut[i] = Clamp(n, 0.0, 1.0) * m_maxHeight;
	}
}
