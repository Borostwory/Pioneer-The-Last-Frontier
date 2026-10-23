// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainNoise.h"

using namespace TerrainNoise;

template <>
const char *TerrainColorFractal<TerrainColorRock2>::GetColorFractalName() const { return "Rock2"; }

template <>
TerrainColorFractal<TerrainColorRock2>::TerrainColorFractal(const SystemBody *body) :
	Terrain(body)
{
}

template <>
vector3d TerrainColorFractal<TerrainColorRock2>::GetColor(const vector3d &p, double height, const vector3d &norm) const
{
	const double h = m_invMaxHeight * height / 2;
	if (h <= 0) return m_darkrockColor[0];
	const double flatness = pow(p.Dot(norm), 6.0);
	const vector3d color_cliffs = m_rockColor[0];
	const double equatorial_desert = (2.0 - m_icyness) * terrain_colournoise_climate(0.05) * (1.0 - p.y * p.y);
	// Below is to do with variable colours for different heights, it gives a nice effect.
	const vector3d base = interpolate_color(equatorial_desert, m_rockColor[2], m_darkrockColor[4]);
	static const double bands[] = { 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1 };
	return BlendBands(h, bands, 9, [&](const int band, double n) -> vector3d {
		vector3d col;
		switch (band) {
		case 0:
			n -= 0.9;
			n *= 10.0;
			col = interpolate_color(n, m_rockColor[5], base);
			break;
		case 1:
			n -= 0.8;
			n *= 10.0;
			col = interpolate_color(n, base, m_rockColor[5]);
			break;
		case 2:
			n -= 0.7;
			n *= 10.0;
			col = interpolate_color(n, m_rockColor[4], base);
			break;
		case 3:
			n -= 0.6;
			n *= 10.0;
			col = interpolate_color(n, m_rockColor[0], m_rockColor[4]);
			break;
		case 4:
			n -= 0.5;
			n *= 10.0;
			col = interpolate_color(n, base, m_rockColor[0]);
			break;
		case 5:
			n -= 0.4;
			n *= 10.0;
			col = interpolate_color(n, m_darkrockColor[3], base);
			break;
		case 6:
			n -= 0.3;
			n *= 10.0;
			col = interpolate_color(n, base, m_darkrockColor[3]);
			break;
		case 7:
			n -= 0.2;
			n *= 10.0;
			col = interpolate_color(n, m_rockColor[1], base);
			break;
		case 8:
			n -= 0.1;
			n *= 10.0;
			col = interpolate_color(n, base, m_rockColor[1]);
			break;
		default:
			n *= 10.0;
			col = interpolate_color(n, m_darkrockColor[0], base);
			break;
		}
		return interpolate_color(flatness, color_cliffs, col);
	});
}
