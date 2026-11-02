// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainAsteroid.h"
#include "TerrainFeature.h"
#include "TerrainNoise.h"

using namespace TerrainNoise;
using namespace TerrainFeature;

// Heavily cratered: smooth regolith, craters of every size and a few basins

template <>
const char *TerrainHeightFractal<TerrainHeightAsteroid3>::GetHeightFractalName() const { return "Asteroid3"; }

template <>
TerrainHeightFractal<TerrainHeightAsteroid3>::TerrainHeightFractal(const SystemBody *body) :
	Terrain(body)
{
	const double r = m_planetRadius;
	const double lobeWidth = m_rand.Double(1.2, 2.5) * r;
	SetFracDef(0, m_maxHeightInMeters, lobeWidth, lobeWidth / 8.0);
	SetFracDef(1, m_rand.Double(0.0, TerrainAsteroid::MAX_STRETCH) * m_maxHeightInMeters, r);
	m_quatDefs[1].SetAxisAngle(m_rand.Double(0.0, DEG2RAD(360.0)), GetVector3Random(m_rand));

	// gentle undulation
	SetFracDef(2, m_maxHeightInMeters * 0.05, m_rand.Double(0.2, 0.4) * r);
	// craters
	const double craterWidth = m_rand.Double(0.3, 0.6) * r;
	SetFracDef(3, m_maxHeightInMeters * 0.1, craterWidth, craterWidth / 32.0);
	// basins
	const double basinWidth = m_rand.Double(0.6, 1.0) * r;
	SetFracDef(4, m_maxHeightInMeters * 0.12, basinWidth, basinWidth / 4.0);
	for (size_t i = 3; i < 5; i++)
		m_quatDefs[i].SetAxisAngle(m_rand.Double(0.0, DEG2RAD(360.0)), GetVector3Random(m_rand));
}

template <>
void TerrainHeightFractal<TerrainHeightAsteroid3>::GetBaseHeights(const vector3d *vP, double *heightsOut, const size_t count) const
{
	for (size_t i = 0; i < count; i++) {
		const vector3d &p = vP[i];
		double n = TerrainAsteroid::shape(m_fracdef[0], m_fracdef[1], m_quatDefs[1], p);
		n += 0.25 * (octavenoise(m_fracdef[2], 0.5, p) - 0.5);
		n += crater_function(m_fracdef[3], m_quatDefs[3] * p);
		n += crater_function(m_fracdef[4], m_quatDefs[4] * p);
		heightsOut[i] = Clamp(n, 0.0, 1.0) * m_maxHeight;
	}
}
