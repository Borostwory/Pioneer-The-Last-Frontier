// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainNoise.h"

using namespace TerrainNoise;

template <>
const char *TerrainColorFractal<TerrainColorAsteroid>::GetColorFractalName() const { return "Asteroid"; }

template <>
TerrainColorFractal<TerrainColorAsteroid>::TerrainColorFractal(const SystemBody *body) :
	Terrain(body)
{
	// regolith in all its shades, bright crater ejecta included; rock on steep slopes
	using M = TerrainMaterialTable;
	m_materials.Add(M::REGOLITH, m_greyrockColor, 8);
	m_materials.Add(M::REGOLITH, m_rockColor, 8);
	m_materials.Add(M::REGOLITH, { vector3d(1, 1, 1) });
	m_materials.Add(M::ROCK);
}

template <>
vector3d TerrainColorFractal<TerrainColorAsteroid>::GetColor(const vector3d &p, double height, const vector3d &norm) const
{
	const double h = m_invMaxHeight * height / 2;
	const double flatness = pow(p.Dot(norm), 6.0);
	const double equatorial_desert = 2.0 * terrain_colournoise_climate(0.5) * (1.0 - p.y * p.y);

	static const double bands[] = { 0.02 };
	return BlendBands(h, bands, 1, [&](const int band, const double n) -> vector3d {
		vector3d col;
		if (band == 0) {
			col = interpolate_color(equatorial_desert, m_greyrockColor[0], m_greyrockColor[2]);
			col = interpolate_color(n, col, m_rockColor[3]);
			return interpolate_color(flatness, m_greyrockColor[1], col);
		} else {
			col = interpolate_color(equatorial_desert, m_rockColor[0], m_greyrockColor[3]);
			col = interpolate_color(n, col, vector3d(1.5, 1.35, 1.3));
			return interpolate_color(flatness, m_rockColor[1], col);
		}
	});
}
