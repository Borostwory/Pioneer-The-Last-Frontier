// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainAsteroid.h"
#include "TerrainFeature.h"
#include "TerrainNoise.h"

using namespace TerrainNoise;
using namespace TerrainFeature;

// Rubble pile: rounded boulder humps and a few large craters

template <>
const char *TerrainHeightFractal<TerrainHeightAsteroid>::GetHeightFractalName() const { return "Asteroid"; }

template <>
TerrainHeightFractal<TerrainHeightAsteroid>::TerrainHeightFractal(const SystemBody *body) :
	Terrain(body)
{
	const double r = m_planetRadius;
	const double lobeWidth = m_rand.Double(1.2, 2.5) * r;
	SetFracDef(0, m_maxHeightInMeters, lobeWidth, lobeWidth / 8.0);
	SetFracDef(1, m_rand.Double(0.0, TerrainAsteroid::MAX_STRETCH) * m_maxHeightInMeters, r);
	m_quatDefs[1].SetAxisAngle(m_rand.Double(0.0, DEG2RAD(360.0)), GetVector3Random(m_rand));

	// humps
	SetFracDef(2, m_maxHeightInMeters * 0.1, m_rand.Double(0.15, 0.3) * r);
	// craters
	const double craterWidth = m_rand.Double(0.4, 0.8) * r;
	SetFracDef(3, m_maxHeightInMeters * 0.08, craterWidth, craterWidth / 8.0);
	m_quatDefs[3].SetAxisAngle(m_rand.Double(0.0, DEG2RAD(360.0)), GetVector3Random(m_rand));
}

template <>
void TerrainHeightFractal<TerrainHeightAsteroid>::GetBaseHeights(const vector3d *vP, double *heightsOut, const size_t count) const
{
	for (size_t i = 0; i < count; i++) {
		const vector3d &p = vP[i];
		double n = TerrainAsteroid::shape(m_fracdef[0], m_fracdef[1], m_quatDefs[1], p);
		n += 0.19 * (billow_octavenoise(m_fracdef[2], TerrainAsteroid::DETAIL_PERSISTENCE, p) - 0.29);
		n += crater_function(m_fracdef[3], m_quatDefs[3] * p);
		heightsOut[i] = Clamp(n, 0.0, 1.0) * m_maxHeight;
	}
}
