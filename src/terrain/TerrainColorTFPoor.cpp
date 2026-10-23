// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainNoise.h"

using namespace TerrainNoise;

template <>
const char *TerrainColorFractal<TerrainColorTFPoor>::GetColorFractalName() const { return "TFPoor"; }

template <>
TerrainColorFractal<TerrainColorTFPoor>::TerrainColorFractal(const SystemBody *body) :
	Terrain(body)
{
	m_surfaceEffects |= Terrain::EFFECT_WATER;
}

template <>
vector3d TerrainColorFractal<TerrainColorTFPoor>::GetColor(const vector3d &p, double height, const vector3d &norm) const
{
	const double h = m_invMaxHeight * height;
	const double flatness = pow(p.Dot(norm), 8.0);

	const double equatorial_desert = (2.0 - m_icyness) * terrain_colournoise_climate(0.5) *
		1.0 * (2.0 - m_icyness) * (1.0 - p.y * p.y);

	// ice on mountains and poles, faded in at the edges
	const double ice = SmoothStep(0.97, 1.03, fabs(m_icyness * p.y) + m_icyness * h);
	auto iceColor = [&]() {
		const vector3d col = interpolate_color(terrain_colournoise_rock2, m_darkrockColor[5], vector3d(.9, .9, .9));
		return interpolate_color(flatness, col, vector3d(1, 1, 1));
	};
	if (ice >= 1.0)
		return iceColor();

	auto surfaceColor = [&]() -> vector3d {
		// water
		if (h <= 0) {
			// This is for fake ocean depth by the coast.
			const double continents = ridged_octavenoise(m_colorFracdef[3], 0.55, p) * (1.0 - m_sealevel) - ((m_sealevel * 0.1) - 0.1);
			// Oooh, pretty coastal regions with shading based on underwater depth.
			double n = h + continents;
			n *= n * 10.0;
			return interpolate_color(n, vector3d(0, 0.0, 0.1), vector3d(0, 0.5, 0.5));
		}

		// More sensitive height detection for application of colours
		static const double bands[] = { 0.5, 0.25, 0.05, 0.01, 0.005 };
		return BlendBands(h, bands, 5, [&](const int band, double n) -> vector3d {
			vector3d color_cliffs = m_darkrockColor[5];
			vector3d col, tex1, tex2;
			switch (band) {
			case 0:
				n -= 0.5;
				n *= 2.0;
				col = interpolate_color(equatorial_desert, m_rockColor[2], m_rockColor[6]);
				col = interpolate_color(n, col, m_darkrockColor[6]);
				tex1 = interpolate_color(terrain_colournoise_rock, col, color_cliffs);
				tex2 = interpolate_color(terrain_colournoise_rock2, col, color_cliffs);
				return interpolate_color(flatness, tex1, tex2);
			case 1:
				n -= 0.25;
				n *= 4.0;
				color_cliffs = m_rockColor[3];
				col = interpolate_color(equatorial_desert, m_darkrockColor[4], m_darksandColor[6]);
				col = interpolate_color(n, col, m_rockColor[2]);
				tex1 = interpolate_color(terrain_colournoise_rock, col, color_cliffs);
				tex2 = interpolate_color(terrain_colournoise_mud, col, color_cliffs);
				return interpolate_color(flatness, tex1, tex2);
			case 2:
				n -= 0.05;
				n *= 5.0;
				col = interpolate_color(equatorial_desert, m_darkrockColor[5], m_darksandColor[7]);
				color_cliffs = col;
				col = interpolate_color(equatorial_desert, m_darksandColor[2], m_sandColor[2]);
				col = interpolate_color(n, col, m_darkrockColor[3]);
				tex1 = interpolate_color(terrain_colournoise_mud, col, color_cliffs);
				tex2 = interpolate_color(terrain_colournoise_grass, col, color_cliffs);
				return interpolate_color(flatness, tex1, tex2);
			case 3:
				n -= 0.01;
				n *= 25.0;
				color_cliffs = m_darkplantColor[0];
				col = interpolate_color(equatorial_desert, m_sandColor[1], m_sandColor[0]);
				col = interpolate_color(n, col, m_darksandColor[2]);
				tex1 = interpolate_color(terrain_colournoise_grass, col, color_cliffs);
				tex2 = interpolate_color(terrain_colournoise_grass2, col, color_cliffs);
				return interpolate_color(flatness, tex1, tex2);
			case 4:
				n -= 0.005;
				n *= 200.0;
				color_cliffs = m_plantColor[0];
				col = interpolate_color(equatorial_desert, m_darkplantColor[0], m_sandColor[1]);
				col = interpolate_color(n, col, m_plantColor[0]);
				tex1 = interpolate_color(terrain_colournoise_sand2, col, color_cliffs);
				tex2 = interpolate_color(terrain_colournoise_grass, col, color_cliffs);
				return interpolate_color(flatness, tex1, tex2);
			default:
				n *= 200.0;
				color_cliffs = m_darksandColor[0];
				col = interpolate_color(equatorial_desert, m_sandColor[0], m_sandColor[1]);
				col = interpolate_color(n, col, m_darkplantColor[0]);
				tex1 = interpolate_color(terrain_colournoise_sand, col, color_cliffs);
				return interpolate_color(flatness, tex1, col);
			}
		});
	};

	vector3d col = surfaceColor();
	//we don't want water on the poles if there are ice-caps
	const double polar = SmoothStep(0.64, 0.70, fabs(m_icyness * p.y));
	if (polar > 0.0) {
		vector3d polarColor = interpolate_color(equatorial_desert, m_sandColor[2], m_darksandColor[5]);
		polarColor = interpolate_color(flatness, polarColor, vector3d(1, 1, 1));
		col = interpolate_color(polar, col, polarColor);
	}
	return (ice > 0.0) ? interpolate_color(ice, col, iceColor()) : col;
}
