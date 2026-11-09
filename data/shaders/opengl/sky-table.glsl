// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// The sky's light along a view direction, and the sky table (P8 of docs/implementation_plans/
// opengl46-tessellation-compute_plan.md): with the camera in the air, a compute shader (sky_table.comp) works the light
// out once per frame for a grid of directions around the camera, and the sky (rayleigh_geosphere_sky_full.frag) reads
// it instead of working it out at every pixel. The same sums either way.
//
// The grid: across, the direction round the camera's up (east, north: any two along the ground); down, from the zenith
// at the top to the horizon in the middle and straight down at the bottom, its rows closer together near the horizon,
// where the sky changes fastest (as in Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique", 2020).
//
// Needs: attributes.glsl, lib.glsl, basesphere_uniforms.glsl, rayleigh-lib.glsl, and the uniforms NumShadows,
// densityLUT and scatterLUT declared before it.

#define SKY_TABLE_WIDTH 384.0
#define SKY_TABLE_HEIGHT 256.0

// The sky's light along eyenorm (view space, unit length) from the camera, before the exposure, summed over the lights;
// alpha 0 for a ray that misses the air
vec4 skyLightAlong(vec3 eyenorm)
{
	vec2 atmosDist = raySphereIntersect(geosphereCenter, eyenorm, geosphereAtmosTopRad);
	// Invalid ray
	if (atmosDist.x == 0.0 && atmosDist.y == 0.0)
		return vec4(0.0);

	// a&b scaled so length of 1.0 means planet surface.
	vec3 a = atmosDist.x * eyenorm - geosphereCenter;
	vec3 b = atmosDist.y * eyenorm - geosphereCenter;

	vec3 specularHighlight = vec3(0.0);
	float AU = 149598000000.0;

#if (NUM_LIGHTS > 0)
	// coordinates, in planet radius
	vec4 planet = vec4(geosphereCenter, geosphereRadius);
	vec4 atmosphere = vec4(geosphereCenter, geosphereAtmosTopRad);
	for (int i = 0; i < NUM_LIGHTS; ++i) {
		vec3 lightDir = normalize(vec3(uLight[i].position));
		float uneclipsed = clamp(calcUneclipsedSky(eclipse, NumShadows, a, b, lightDir), 0.0, 1.0);

		vec3 lightPosAU = uLight[i].position.xyz / AU;
		float intensity = 1.f / dot(lightPosAU, lightPosAU); // magic to avoid calculating length and then squaring it
		// bodies without Earth's values: the gentler falloff their ground and haze get far from the
		// sun (rayleigh_geosphere_terrain.vert), so Titan's glow isn't left at 1% of Earth's
		if (!atmosEarthValues())
			intensity = inversesqrt(max(length(lightPosAU), 1e-3));
		specularHighlight += calculateAtmosphereColor(planet, atmosphere, toLinear(uLight[i].diffuse), lightDir, vec3(0.0), eyenorm, uneclipsed, densityLUT, scatterLUT) * intensity;
	}
#endif
	return vec4(specularHighlight, 1.0);
}

// the grid's frame round the camera
void skyTableFrame(out vec3 up, out vec3 east, out vec3 north)
{
	up = normalize(-geosphereCenter);
	vec3 across = abs(up.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	east = normalize(cross(across, up));
	north = cross(up, east);
}

// the angle from the zenith to the horizon, and from the horizon down to straight down, for the camera's height (planet
// radii, kept just above the ground)
void skyTableHorizon(out float zenithToHorizon, out float horizonToNadir)
{
	float r = max(length(geosphereCenter), 1.0001);
	horizonToNadir = acos(clamp(sqrt(r * r - 1.0) / r, -1.0, 1.0));
	zenithToHorizon = PI - horizonToNadir;
}

// where a direction (view space, unit length) is in the table, 0..1
vec2 skyTableUV(vec3 dir)
{
	vec3 up, east, north;
	skyTableFrame(up, east, north);
	float zenithToHorizon, horizonToNadir;
	skyTableHorizon(zenithToHorizon, horizonToNadir);
	float fromZenith = acos(clamp(dot(dir, up), -1.0, 1.0));
	float v;
	if (fromZenith < zenithToHorizon)
		v = 0.5 * (1.0 - sqrt(max(1.0 - fromZenith / zenithToHorizon, 0.0)));
	else
		v = 0.5 + 0.5 * sqrt(clamp((fromZenith - zenithToHorizon) / horizonToNadir, 0.0, 1.0));
	float u = atan(dot(dir, north), dot(dir, east)) / (2.0 * PI) + 0.5;
	// (between the first and last rows' centres, so the zenith and straight down don't blend across the wrap)
	return vec2(u, mix(0.5 / SKY_TABLE_HEIGHT, 1.0 - 0.5 / SKY_TABLE_HEIGHT, v));
}

// the direction (view space) at a place in the table, 0..1
vec3 skyTableDirection(vec2 uv)
{
	vec3 up, east, north;
	skyTableFrame(up, east, north);
	float zenithToHorizon, horizonToNadir;
	skyTableHorizon(zenithToHorizon, horizonToNadir);
	float v = clamp((uv.y - 0.5 / SKY_TABLE_HEIGHT) / (1.0 - 1.0 / SKY_TABLE_HEIGHT), 0.0, 1.0);
	float fromZenith;
	if (v < 0.5) {
		float c = 1.0 - 2.0 * v;
		fromZenith = zenithToHorizon * (1.0 - c * c);
	} else {
		float c = 2.0 * v - 1.0;
		fromZenith = zenithToHorizon + horizonToNadir * c * c;
	}
	float around = (uv.x - 0.5) * 2.0 * PI;
	return normalize(up * cos(fromZenith) + sin(fromZenith) * (east * cos(around) + north * sin(around)));
}
