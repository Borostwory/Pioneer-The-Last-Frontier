// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainNoise.h"

using namespace TerrainNoise;

template <>
const char *TerrainColorFractal<TerrainColorEarthLike>::GetColorFractalName() const { return "EarthLike"; }

template <>
TerrainColorFractal<TerrainColorEarthLike>::TerrainColorFractal(const SystemBody *body) :
	Terrain(body)
{
	// crappy water
	//double height = m_maxHeightInMeters*0.5;
	//SetFracDef(3, m_maxHeightInMeters, 1e8, 50.0);
	//SetFracDef(2, m_maxHeightInMeters, 10, 10.0);
	m_surfaceEffects |= Terrain::EFFECT_WATER;
	AddPaletteMaterials();
}

template <>
vector3d TerrainColorFractal<TerrainColorEarthLike>::GetColor(const vector3d &p, double height, const vector3d &norm) const
{
	const double h = m_invMaxHeight * height;
	const double flatness = pow(p.Dot(norm), 8.0);

	const double equatorial_desert = (2.0 - m_icyness) * terrain_colournoise_climate(0.5) * (1.0 - p.y * p.y);

	// ice on flat mountain tops and polar ice-caps, faded in at the edges
	const double polar = (m_icyness * 0.5) + (fabs(p.y * p.y * p.y * 0.38));
	const double mountainIce = 0.6 / Clamp(h * m_icyness + polar, 0.1, 1.0);
	const double ice = std::max(SmoothStep(mountainIce * 0.9, mountainIce * 1.1, flatness), SmoothStep(0.57, 0.63, polar));
	auto iceColor = [&]() {
		const vector3d col = interpolate_color(terrain_colournoise_rock, m_darkrockColor[5], m_rockColor[5]);
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
			const vector3d col = interpolate_color(equatorial_desert, vector3d(0, 0, 0.15), vector3d(0, 0, 0.25));
			return interpolate_color(n, col, vector3d(0, 0.8, 0.6));
		}

		const double flat = pow(p.Dot(norm), 16.0);
		// More sensitive height detection for application of colours
		static const double bands[] = { 0.5, 0.25, 0.05, 0.01, 0.005 };
		return BlendBands(h, bands, 5, [&](const int band, double n) -> vector3d {
			vector3d color_cliffs, col, tex1, tex2;
			switch (band) {
			case 0:
				n -= 0.5;
				n *= 2.0;
				color_cliffs = interpolate_color(n, m_darkrockColor[2], m_rockColor[4]);
				col = interpolate_color(equatorial_desert, m_rockColor[2], m_rockColor[4]);
				col = interpolate_color(n, col, m_darkrockColor[6]);
				tex1 = interpolate_color(terrain_colournoise_rock, col, color_cliffs);
				tex2 = interpolate_color(terrain_colournoise_sand, col, m_darkdirtColor[3]);
				return interpolate_color(flat, tex1, tex2);
			case 1:
				n -= 0.25;
				n *= 4.0;
				color_cliffs = interpolate_color(n, m_rockColor[3], m_darkplantColor[4]);
				col = interpolate_color(equatorial_desert, m_darkrockColor[3], m_darksandColor[1]);
				col = interpolate_color(n, col, m_rockColor[2]);
				tex1 = interpolate_color(terrain_colournoise_rock, col, color_cliffs);
				tex2 = interpolate_color(terrain_colournoise_sand, col, m_darkdirtColor[3]);
				return interpolate_color(flat, tex1, tex2);
			case 2:
				n -= 0.05;
				n *= 5.0;
				color_cliffs = interpolate_color(equatorial_desert, m_darkrockColor[5], m_darksandColor[7]);
				col = interpolate_color(equatorial_desert, m_darkplantColor[2], m_sandColor[2]);
				col = interpolate_color(n, col, m_darkrockColor[3]);
				tex1 = interpolate_color(terrain_colournoise_rock, col, color_cliffs);
				tex2 = interpolate_color(terrain_colournoise_forest, col, color_cliffs);
				return interpolate_color(flat, tex1, tex2);
			case 3:
				n -= 0.01;
				n *= 25.0;
				color_cliffs = m_darkdirtColor[7];
				col = interpolate_color(equatorial_desert, m_plantColor[1], m_plantColor[0]);
				col = interpolate_color(n, col, m_darkplantColor[2]);
				tex1 = interpolate_color(terrain_colournoise_rock, col, color_cliffs);
				tex2 = interpolate_color(terrain_colournoise_grass, color_cliffs, col);
				return interpolate_color(flat, tex1, tex2);
			case 4:
				n -= 0.005;
				n *= 200.0;
				color_cliffs = m_dirtColor[2];
				col = interpolate_color(equatorial_desert, m_darkplantColor[0], m_sandColor[1]);
				col = interpolate_color(n, col, m_plantColor[0]);
				tex1 = interpolate_color(terrain_colournoise_rock, col, color_cliffs);
				tex2 = interpolate_color(terrain_colournoise_grass, color_cliffs, col);
				return interpolate_color(flat, tex1, tex2);
			default:
				n *= 200.0;
				color_cliffs = m_darksandColor[0];
				col = interpolate_color(equatorial_desert, m_sandColor[0], m_sandColor[1]);
				col = interpolate_color(n, col, m_darkplantColor[0]);
				tex1 = interpolate_color(terrain_colournoise_rock, col, color_cliffs);
				tex2 = interpolate_color(terrain_colournoise_sand, col, color_cliffs);
				return interpolate_color(flat, tex1, tex2);
			}
		});
	};

	const vector3d surface = surfaceColor();
	return (ice > 0.0) ? interpolate_color(ice, surface, iceColor()) : surface;
}
