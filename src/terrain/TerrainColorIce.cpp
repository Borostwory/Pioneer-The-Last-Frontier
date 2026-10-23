// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainNoise.h"

using namespace TerrainNoise;

template <>
const char *TerrainColorFractal<TerrainColorIce>::GetColorFractalName() const { return "Ice"; }

template <>
TerrainColorFractal<TerrainColorIce>::TerrainColorFractal(const SystemBody *body) :
	Terrain(body)
{
	// the colours of GetColor(): snow fields, darker ice crust, rock cliffs
	using M = TerrainMaterialTable;
	m_materials.Add(M::SNOW, { vector3d(.96, .95, .94), vector3d(1, 1, 1) });
	m_materials.Add(M::ICE, m_darkrockColor, 8);
	m_materials.Add(M::ROCK, m_rockColor, 8);
}

template <>
vector3d TerrainColorFractal<TerrainColorIce>::GetColor(const vector3d &p, double height, const vector3d &norm) const
{
	const double h = m_invMaxHeight * height;

	if (h <= 0.0) return vector3d(0.96, 0.96, 0.96);

	const double flatness = pow(p.Dot(norm), 24.0);
	const double equatorial_desert = (2.0 - m_icyness) * terrain_colournoise_climate(0.5) * (1.0 - p.y * p.y);
	const double equatorial_region_1 = billow_octavenoise(m_colorFracdef[4], 0.5, p) * p.y * p.y;
	const double equatorial_region_2 = ridged_octavenoise(m_colorFracdef[5], 0.5, p) * p.x * p.x;
	// cliff colours
	vector3d color_cliffs;
	// adds some variation
	color_cliffs = interpolate_color(equatorial_region_1, m_rockColor[3], m_rockColor[0]);
	color_cliffs = interpolate_color(equatorial_region_2, color_cliffs, m_rockColor[2]);
	// main colours
	vector3d base;
	// start by interpolating between noise values for variation
	base = interpolate_color(equatorial_region_1, m_darkrockColor[0], vector3d(1, 1, 1));
	base = interpolate_color(equatorial_region_2, m_darkrockColor[1], base);
	base = interpolate_color(equatorial_desert, base, vector3d(.96, .95, .94));
	// scale by different colours depending on height for more variation
	static const double bands[] = { .666, .333 };
	return BlendBands(h, bands, 2, [&](const int band, double n) -> vector3d {
		vector3d col;
		switch (band) {
		case 0:
			n -= 0.666;
			n *= 3.0;
			col = interpolate_color(n, vector3d(.96, .95, .94), base);
			break;
		case 1:
			n -= 0.333;
			n *= 3.0;
			col = interpolate_color(n, base, vector3d(.96, .95, .94));
			break;
		default:
			n *= 3.0;
			col = interpolate_color(n, vector3d(.96, .95, .94), base);
			break;
		}
		return interpolate_color(flatness, color_cliffs, col);
	});
}
