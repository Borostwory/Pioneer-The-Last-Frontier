// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Lights of the starport cities on the night side, seen from afar
// (GeoSphere::SetCityGlowData), as large as their buildings will be
// (CityOnPlanet::GetCitySummary). Close to a city its real lights take over
// (city-lights.glsl), so the CPU fades its glow out.
//
// Needs: cloud-noise.glsl, cloud-uniforms.glsl (planet axes),
// basesphere_uniforms.glsl and the terrain shaders' lightAtSurface, and
// NUM_LIGHTS > 0. The .shaderdef declares:
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

float cityGlowCityFade(int i)
{
	vec4 f = cityGlowFade[i / 4];
	int c = i % 4;
	return c == 0 ? f.x : c == 1 ? f.y : c == 2 ? f.z : f.w;
}

// Light emitted by the cities at a surface point: surfaceUp is the view-space
// direction up there, footprint the pixel's size there [m].
vec3 cityGlowAt(vec3 surfaceUp, float footprint)
{
	if (cityGlowParams.z <= 0.0)
		return vec3(0.0);

	// only where it is dark: the starlight reaching level ground, after the
	// atmosphere and eclipses (as Camera::SetCityLights() works it out)
	vec3 sunlight = vec3(0.0);
	for (int i = 0; i < NUM_LIGHTS; ++i)
		sunlight += lightAtSurface[i] * max(dot(surfaceUp, normalize(uLight[i].position.xyz)), 0.0);
	float night = 1.0 - smoothstep(CITY_LIGHTS_DARK, CITY_LIGHTS_BRIGHT, dot(sunlight, vec3(0.3, 0.59, 0.11)));
	if (night <= 0.0)
		return vec3(0.0);

	vec3 pf = vec3(dot(surfaceUp, cloudAxisX.xyz), dot(surfaceUp, cloudAxisY.xyz), dot(surfaceUp, cloudAxisZ.xyz));
	vec3 p = pf * geosphereRadius;

	float glow = 0.0;
	int numCities = int(cityGlowParams.y);
	for (int i = 0; i < CITY_GLOW_MAX; ++i) {
		if (i >= numCities)
			break;
		vec4 city = cityGlowCity[i];
		float x = length(pf - city.xyz) * geosphereRadius / city.w;
		float fade = cityGlowCityFade(i);
		if (x > 1.0 || fade <= 0.0)
			continue;
		// a bright centre broken up into districts
		float districts = mix(0.5 + 0.5 * smoothstep(-0.4, 0.5, cloudGradNoise(p / 700.0)), 0.75, smoothstep(100.0, 400.0, footprint));
		glow += fade * CITY_GLOW_CORE * (1.0 - smoothstep(0.2, 1.0, x)) * districts;
	}

	return CITY_GLOW_COLOUR * (glow * night * cityGlowParams.z);
}
