// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainAsteroid.h"
#include "TerrainFeature.h"
#include "TerrainNoise.h"

using namespace TerrainNoise;
using namespace TerrainFeature;

// Grooved: narrow grooves across a rough surface, like Phobos

template <>
const char *TerrainHeightFractal<TerrainHeightAsteroid4>::GetHeightFractalName() const { return "Asteroid4"; }

template <>
TerrainHeightFractal<TerrainHeightAsteroid4>::TerrainHeightFractal(const SystemBody *body) :
	Terrain(body)
{
	const double r = m_planetRadius;
	const double lobeWidth = m_rand.Double(1.2, 2.5) * r;
	SetFracDef(0, m_maxHeightInMeters, lobeWidth, lobeWidth / 8.0);
	SetFracDef(1, m_rand.Double(0.0, TerrainAsteroid::MAX_STRETCH) * m_maxHeightInMeters, r);
	m_quatDefs[1].SetAxisAngle(m_rand.Double(0.0, DEG2RAD(360.0)), GetVector3Random(m_rand));

	// grooves
	SetFracDef(2, m_maxHeightInMeters * 0.08, m_rand.Double(0.2, 0.4) * r);
	// rough texture
	SetFracDef(3, m_maxHeightInMeters * 0.05, m_rand.Double(0.1, 0.2) * r);
}

template <>
void TerrainHeightFractal<TerrainHeightAsteroid4>::GetBaseHeights(const vector3d *vP, double *heightsOut, const size_t count) const
{
	for (size_t i = 0; i < count; i++) {
		const vector3d &p = vP[i];
		double n = TerrainAsteroid::shape(m_fracdef[0], m_fracdef[1], m_quatDefs[1], p);
		// sqrt(|noise|) drops steeply where the noise crosses zero: the grooves
		n += 0.06 * (voronoiscam_octavenoise(m_fracdef[2], 0.5, p) - 1.3);
		n += 0.1 * (ridged_octavenoise(m_fracdef[3], 0.5, p) - 0.65);
		heightsOut[i] = Clamp(n, 0.0, 1.0) * m_maxHeight;
	}
}
