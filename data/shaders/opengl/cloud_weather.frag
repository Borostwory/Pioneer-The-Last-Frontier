// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Bakes one weather map of the atlas read by cloudWeatherCached
// (cloud-march.glsl): for each direction of an equirectangular grid, the
// displacement of the weather-noise domain by the large-scale flow
// (cloudWeatherDomain minus cloudWeatherBase). The flow is static in the
// advected frame within a cloud cycle, so a map is baked once per layer and
// cycle (PlanetClouds::UpdateWeatherMaps).
//  cloudPassFrame: x layer, y cloud cycle, w component (0: xy, 1: z)
//  cloudPassMarch.xy: origin of the map in the atlas [texels]

#include "attributes.glsl"
#include "lib.glsl"
#include "cloud-pass-uniforms.glsl"

#define atmosColor cloudPassAtmos

#include "cloud-config.glsl"
#include "cloud-uniforms.glsl"
#include "cloud-noise.glsl"
#include "cloud-derive.glsl"
#include "cloud-march.glsl"

out vec4 frag_color;

void main(void)
{
	vec2 local = floor(gl_FragCoord.xy) - cloudPassMarch.xy;
	float W = float(CLOUD_WEATHER_MAP_W), H = float(CLOUD_WEATHER_MAP_H);
	// column 0 and W + 1 are the wrap padding (they continue past +-180)
	float lon = ((local.x - 0.5) / W) * 2.0 * PI - PI;
	float lat = ((local.y + 0.5) / H) * PI - 0.5 * PI;
	vec3 p = vec3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));

	CloudPlanet cp = cloudDerive();
	int li = int(cloudPassFrame.x + 0.5);
	float wsize = cloudWeatherSize(cp, li);
	vec3 off = cloudPhaseOffset(cloudPassFrame.y, float(li));
	vec3 d = cloudWeatherDomain(cp, p, wsize, off) - cloudWeatherBase(cp, p, wsize, off);
	frag_color = cloudPassFrame.w < 0.5 ? vec4(d.xy, 0.0, 1.0) : vec4(d.z, 0.0, 0.0, 1.0);
}
