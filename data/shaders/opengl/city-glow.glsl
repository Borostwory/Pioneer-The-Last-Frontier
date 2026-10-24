// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Lights of the starport cities on the night side, seen from afar
// (GeoSphere::SetCityGlowData), as large as their buildings will be
// (CityOnPlanet::GetCitySummary). Close to a city its real lights take over
// (city-lights.glsl), so the CPU fades its glow out. Used by the terrain, and
// by the cloud shadows, which leave the city's light alone.
//
// Needs: cloud-noise.glsl and cloud-uniforms.glsl (planet axes). The
// .shaderdef declares:
//   Buffer CityGlowData binding=6

#define CITY_GLOW_MAX 16

layout(std140) uniform CityGlowData {
	vec4 cityGlowParams;					 // y: number of cities, z: brightness
	vec4 cityGlowCity[CITY_GLOW_MAX];		 // planet-fixed direction to the city, its radius [m]
	vec4 cityGlowFade[CITY_GLOW_MAX / 4];	 // brightness of each city, four to a vector
};

// the lights are fully on below this daylight on level ground and off above
// that (CITY_LIGHTS_* in Camera.cpp)
#define CITY_LIGHTS_DARK 0.03
#define CITY_LIGHTS_BRIGHT 0.12

#define CITY_GLOW_COLOUR vec3(1.0, 0.7, 0.42)
// brightness of a city's centre
#define CITY_GLOW_CORE 0.1

// how far the city lights are on (0..1) at a daylight on level ground
float cityLightsNight(float daylight)
{
	return 1.0 - smoothstep(CITY_LIGHTS_DARK, CITY_LIGHTS_BRIGHT, daylight);
}

float cityGlowCityFade(int i)
{
	vec4 f = cityGlowFade[i / 4];
	int c = i % 4;
	return c == 0 ? f.x : c == 1 ? f.y : c == 2 ? f.z : f.w;
}

// Light the cities would emit at a surface point in the dark: up is the
// view-space direction up there, radius the planet's [m], footprint the
// pixel's size there [m].
vec3 cityGlowEmission(vec3 up, float radius, float footprint)
{
	if (cityGlowParams.z <= 0.0)
		return vec3(0.0);

	vec3 pf = vec3(dot(up, cloudAxisX.xyz), dot(up, cloudAxisY.xyz), dot(up, cloudAxisZ.xyz));
	vec3 p = pf * radius;

	float glow = 0.0;
	int numCities = int(cityGlowParams.y);
	for (int i = 0; i < CITY_GLOW_MAX; ++i) {
		if (i >= numCities)
			break;
		vec4 city = cityGlowCity[i];
		float x = length(pf - city.xyz) * radius / city.w;
		float fade = cityGlowCityFade(i);
		if (x > 1.0 || fade <= 0.0)
			continue;
		// a bright centre broken up into districts
		float districts = mix(0.5 + 0.5 * smoothstep(-0.4, 0.5, cloudGradNoise(p / 700.0)), 0.75, smoothstep(100.0, 400.0, footprint));
		glow += fade * CITY_GLOW_CORE * (1.0 - smoothstep(0.2, 1.0, x)) * districts;
	}

	return CITY_GLOW_COLOUR * (glow * cityGlowParams.z);
}

// Light emitted by the cities at a surface point, where it is dark: daylight
// is the starlight reaching level ground there (luminance, after the
// atmosphere and eclipses).
vec3 cityGlowAt(vec3 up, float radius, float footprint, float daylight)
{
	float night = cityLightsNight(daylight);
	return night > 0.0 ? night * cityGlowEmission(up, radius, footprint) : vec3(0.0);
}
