// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Procedural surface patterns for the terrain shaders: waves on water, ripples
// on sand, clumps on vegetation, stones on rock, soft drifts on snow.
//
// The surface type is classified per pixel from the terrain albedo (the vertex
// colour from the colour fractal) using the body's vegetation and sand hues.
// Patterns are 3D noise in planet-fixed coordinates, so unlike the tiling
// detail textures they never repeat. Each pattern is evaluated at two
// power-of-two scales picked from the view distance and blended, so it keeps
// about the same on-screen size from orbit to low altitude.
//
// Needs: cloud-noise.glsl, cloud-uniforms.glsl (planet axes, time) and
// basesphere_uniforms.glsl.

// x: vegetation hue, y: sand hue (both 0..1), z: amount of life
uniform vec4 SurfaceMaterial;

// pattern cell size as a fraction of the view distance
#define SURFACE_CELL_PER_DIST 0.012
// smallest pattern cell in metres; below this float precision of the
// planet-fixed position runs out, and the detail textures take over
#define SURFACE_MIN_CELL 50.0

// pattern strength
#define SURFACE_WATER_CONTRAST 0.1
#define SURFACE_SAND_CONTRAST 0.14
#define SURFACE_VEGETATION_CONTRAST 0.22
#define SURFACE_ROCK_CONTRAST 0.28
#define SURFACE_SNOW_CONTRAST 0.05

vec3 surfaceRgbToHsv(vec3 c)
{
	vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
	vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
	float d = q.x - min(q.w, q.y);
	float e = 1.0e-10;
	return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

float surfaceHueDistance(float a, float b)
{
	float d = abs(a - b);
	return min(d, 1.0 - d);
}

// x: water, y: sand, z: vegetation, w: snow; rock is what is left
vec4 surfaceMaterialWeights(vec3 albedo, bool hasWater)
{
	vec3 hsv = surfaceRgbToHsv(clamp(albedo, 0.0, 1.0));

	// the colour fractals paint water with no red at all
	float water = hasWater ? smoothstep(0.06, 0.02, albedo.r) * smoothstep(0.03, 0.08, albedo.b) : 0.0;
	float snow = smoothstep(0.65, 0.85, hsv.z) * smoothstep(0.25, 0.1, hsv.y);
	float vegetation = step(0.02, SurfaceMaterial.z) *
		smoothstep(0.12, 0.06, surfaceHueDistance(hsv.x, SurfaceMaterial.x)) * smoothstep(0.12, 0.25, hsv.y);
	float sand = smoothstep(0.09, 0.04, surfaceHueDistance(hsv.x, SurfaceMaterial.y)) *
		smoothstep(0.18, 0.3, hsv.y) * smoothstep(0.4, 0.55, hsv.z);

	vec3 land = vec3(sand, vegetation, snow) * (1.0 - water);
	float landSum = land.x + land.y + land.z;
	if (landSum > 1.0 - water)
		land *= (1.0 - water) / landSum;
	return vec4(water, land);
}

// patterns around 1.0; q is the planet-fixed position in pattern cells
float surfaceWaterPattern(vec3 q, float t)
{
	// long swells: bands along one direction, bent by noise, drifting slowly
	float phase = dot(q, vec3(0.8, 0.2, 0.55)) * 3.0 + 2.5 * cloudGradNoise(q * 0.5) + t * 0.05;
	return 1.0 + SURFACE_WATER_CONTRAST * (sin(phase) * 0.7 + 0.3 * cloudGradNoise(q * 2.0));
}

float surfaceSandPattern(vec3 q)
{
	// dune ripples: sharp crests, soft troughs
	float phase = dot(q, vec3(0.3, 0.75, 0.6)) * 4.0 + 3.0 * cloudGradNoise(q * 0.7);
	float ripple = pow(0.5 + 0.5 * sin(phase), 3.0);
	return 1.0 + SURFACE_SAND_CONTRAST * ((ripple - 0.35) * 1.6 + 0.4 * cloudGradNoise(q * 3.0));
}

float surfaceVegetationPattern(vec3 q)
{
	// clumps of darker and lighter growth
	float clumps = cloudFbm(q * 1.5, 3, 1.0);
	return 1.0 + SURFACE_VEGETATION_CONTRAST * (clumps - 0.5) * 2.0;
}

float surfaceRockPattern(vec3 q)
{
	// stones: rounded blobs around cell points over mottled, darker ground
	float stones = smoothstep(0.6, 0.15, cloudWorley(q * 2.0));
	float mottling = cloudFbm(q * 0.6, 2, 1.0) - 0.5;
	return 1.0 + SURFACE_ROCK_CONTRAST * ((stones - 0.3) * 0.9 + 1.2 * mottling);
}

float surfaceSnowPattern(vec3 q)
{
	return 1.0 + SURFACE_SNOW_CONTRAST * cloudGradNoise(q);
}

float surfacePatternAt(vec3 q, vec4 w, float rock, float t)
{
	float m = 0.0;
	if (w.x > 0.01) m += w.x * surfaceWaterPattern(q, t);
	if (w.y > 0.01) m += w.y * surfaceSandPattern(q);
	if (w.z > 0.01) m += w.z * surfaceVegetationPattern(q);
	if (w.w > 0.01) m += w.w * surfaceSnowPattern(q);
	if (rock > 0.01) m += rock * surfaceRockPattern(q);
	// weights below the thresholds contribute a neutral 1.0
	float used = (w.x > 0.01 ? w.x : 0.0) + (w.y > 0.01 ? w.y : 0.0) + (w.z > 0.01 ? w.z : 0.0) +
		(w.w > 0.01 ? w.w : 0.0) + (rock > 0.01 ? rock : 0.0);
	return m + (1.0 - used);
}

// Pattern multiplier for the surface colour, around 1.0.
// eyepos is the view-space position in metres, albedo the unlit terrain colour.
float terrainSurfacePattern(vec3 eyepos, vec3 albedo, float distMetres, bool hasWater)
{
	vec4 w = surfaceMaterialWeights(albedo, hasWater);
	float rock = max(0.0, 1.0 - w.x - w.y - w.z - w.w);

	// planet-fixed position on the unit sphere
	vec3 P = eyepos * geosphereInvRadius - geosphereCenter;
	vec3 pf = vec3(dot(P, cloudAxisX.xyz), dot(P, cloudAxisY.xyz), dot(P, cloudAxisZ.xyz));

	float level = max(0.0, log2(distMetres * SURFACE_CELL_PER_DIST / SURFACE_MIN_CELL));
	float k = floor(level);
	float f = level - k;
	float cell0 = SURFACE_MIN_CELL * exp2(k);
	float t = cloudTime.x;

	float a = surfacePatternAt(pf * (geosphereRadius / cell0), w, rock, t);
	float b = surfacePatternAt(pf * (geosphereRadius / (cell0 * 2.0)), w, rock, t);
	return mix(a, b, f);
}
