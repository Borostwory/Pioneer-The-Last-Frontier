// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainNoise.h"

using namespace TerrainNoise;

template <>
const char *TerrainColorFractal<TerrainColorVolcanic>::GetColorFractalName() const { return "Volcanic"; }

template <>
TerrainColorFractal<TerrainColorVolcanic>::TerrainColorFractal(const SystemBody *body) :
	Terrain(body)
{
	// percent chance of there being exposed lava based on planets volcanicity
	if (m_rand.Double(1.0) > (1.0 - m_volcanic)) {
		m_surfaceEffects |= Terrain::EFFECT_LAVA;
	}

	SetFracDef(0, m_maxHeightInMeters, m_rand.Double(1e6, 1e7));

	// the colours of GetColor(): dark basalt, red-orange lava, brown ash,
	// yellow sulphur deposits (as sand), rock cliffs
	using M = TerrainMaterialTable;
	m_materials.Add(M::BASALT, { vector3d(.1, 0, 0), vector3d(.2, .1, 0), vector3d(.1, .05, 0), vector3d(.15, .07, 0), vector3d(0, 0, 0) });
	m_materials.Add(M::LAVA, { vector3d(.9, .3, 0), vector3d(.75, .2, 0), vector3d(1, .45, 0) });
	m_materials.Add(M::ASH, { vector3d(.3, .2, 0), vector3d(.3, .1, 0) });
	m_materials.Add(M::SAND, { vector3d(1, 1, 0), vector3d(.75, .6, 0), vector3d(.9, .75, 0) });
	m_materials.Add(M::ROCK, m_rockColor, 8);
}

template <>
vector3d TerrainColorFractal<TerrainColorVolcanic>::GetColor(const vector3d &p, double height, const vector3d &norm) const
{
	const double h = m_invMaxHeight * height;
	const double flatness = pow(p.Dot(norm), 6.0);
	const vector3d color_cliffs = m_rockColor[2];
	const double equatorial_desert = terrain_colournoise_climate(0.5) * 1.0 * (1.0 - p.y * p.y);

	static const double bands[] = { 0.4, 0.2, 0.1 };
	return BlendBands(h, bands, 3, [&](const int band, const double n) -> vector3d {
		vector3d col;
		switch (band) {
		case 0:
			col = interpolate_color(equatorial_desert, vector3d(.3, .2, 0), vector3d(.3, .1, .0));
			col = interpolate_color(n, col, vector3d(.1, .0, .0));
			break;
		case 1:
			col = interpolate_color(equatorial_desert, vector3d(1.2, 1, 0), vector3d(.9, .3, .0));
			col = interpolate_color(n, col, vector3d(-1.1, -1, .0));
			break;
		case 2:
			col = interpolate_color(equatorial_desert, vector3d(.2, .1, 0), vector3d(.1, .05, .0));
			col = interpolate_color(n, col, vector3d(2.5, 2, .0));
			break;
		default:
			col = interpolate_color(equatorial_desert, vector3d(.75, .6, 0), vector3d(.75, .2, .0));
			col = interpolate_color(n, col, vector3d(-2, -2.2, .0));
			break;
		}
		return interpolate_color(flatness, color_cliffs, col);
	});
}
