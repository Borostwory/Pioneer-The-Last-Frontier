// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainNoise.h"

using namespace TerrainNoise;

template <>
const char *TerrainColorFractal<TerrainColorTFGood>::GetColorFractalName() const { return "TFGood"; }

template <>
TerrainColorFractal<TerrainColorTFGood>::TerrainColorFractal(const SystemBody *body) :
	Terrain(body)
{
	m_surfaceEffects |= Terrain::EFFECT_WATER;
}

template <>
vector3d TerrainColorFractal<TerrainColorTFGood>::GetColor(const vector3d &p, double height, const vector3d &norm) const
{
	const double h = m_invMaxHeight * height;
	const double flatness = pow(p.Dot(norm), 8.0);
	// ice on mountains and poles, faded in at the edges
	const double ice = SmoothStep(0.97, 1.03, fabs(m_icyness * p.y) + m_icyness * h);
	const vector3d iceColor = interpolate_color(flatness, m_rockColor[5], vector3d(1, 1, 1));
	if (ice >= 1.0)
		return iceColor;

	const double equatorial_desert = (2.0 - m_icyness) * terrain_colournoise_climate(0.5) *
		1.0 * (2.0 - m_icyness) * (1.0 - p.y * p.y);

	auto surfaceColor = [&]() -> vector3d {
		// water
		if (h <= 0) {
			// This is for fake ocean depth by the coast.
			const double continents = octavenoise(m_colorFracdef[3], 0.7 * ridged_octavenoise(m_colorFracdef[8], 0.58, p), p) - m_sealevel * 0.6;
			// Oooh, pretty coastal regions with shading based on underwater depth.
			double n = h + continents - (m_fracdef[0].amplitude * m_sealevel * 0.49);
			n *= 10.0;
			n = (n > 0.3 ? 0.3 - (n * n * n - 0.027) : n);
			const vector3d col = interpolate_color(equatorial_desert, vector3d(0, 0, 0.15), vector3d(0, 0, 0.25));
			return interpolate_color(n, col, vector3d(0, 0.8, 0.6));
		}

		// More sensitive height detection for application of colours
		static const double bands[] = { 0.5, 0.25, 0.05, 0.01, 0.005 };
		return BlendBands(h, bands, 5, [&](const int band, const double n) -> vector3d {
			vector3d color_cliffs, col;
			switch (band) {
			case 0:
				color_cliffs = m_rockColor[5];
				col = interpolate_color(equatorial_desert, m_rockColor[2], m_rockColor[4]);
				col = interpolate_color(n, col, m_darkrockColor[6]);
				break;
			case 1:
				color_cliffs = m_darkrockColor[1];
				col = interpolate_color(equatorial_desert, m_darkrockColor[5], m_darkrockColor[7]);
				col = interpolate_color(n, col, m_rockColor[1]);
				break;
			case 2:
				col = interpolate_color(equatorial_desert, m_darkrockColor[5], m_darkrockColor[7]);
				color_cliffs = col;
				col = interpolate_color(equatorial_desert, vector3d(.45, .43, .2), vector3d(.4, .43, .2));
				col = interpolate_color(n, col, vector3d(-1.66, -2.3, -1.75));
				break;
			case 3:
				color_cliffs = vector3d(0.2, 0.28, 0.2);
				col = interpolate_color(equatorial_desert, vector3d(.15, .5, -.1), vector3d(.2, .6, -.1));
				col = interpolate_color(n, col, vector3d(5, -5, 5));
				break;
			case 4:
				color_cliffs = vector3d(0.25, 0.28, 0.2);
				col = interpolate_color(equatorial_desert, vector3d(.45, .6, 0), vector3d(.5, .6, .0));
				col = interpolate_color(n, col, vector3d(-10, -10, 0));
				break;
			default:
				color_cliffs = vector3d(0.3, 0.1, 0.0);
				col = interpolate_color(equatorial_desert, vector3d(.35, .3, 0), vector3d(.4, .3, .0));
				col = interpolate_color(n, col, vector3d(0, 20, 0));
				break;
			}
			return interpolate_color(flatness, color_cliffs, col);
		});
	};

	vector3d col = surfaceColor();
	//we don't want water on the poles if there are ice-caps
	const double polar = SmoothStep(0.72, 0.78, fabs(m_icyness * p.y));
	if (polar > 0.0) {
		vector3d polarColor = interpolate_color(equatorial_desert, vector3d(0.42, 0.46, 0), vector3d(0.5, 0.3, 0));
		polarColor = interpolate_color(flatness, polarColor, vector3d(1, 1, 1));
		col = interpolate_color(polar, col, polarColor);
	}
	return (ice > 0.0) ? interpolate_color(ice, col, iceColor) : col;
}
