// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Procedural surface patterns for the terrain shaders: waves on water, ripples
// on sand, clumps on vegetation, stones on rock, soft drifts on snow, cracks
// in ice, a dark crust with brighter cracks on lava.
//
// The surface type weights come from the body's material slots
// (terrainPatternWeights() in terrain-materials.glsl). Patterns are 3D noise in planet-fixed coordinates, so unlike the tiling
// detail textures they never repeat. Each pattern is evaluated at two
// power-of-two scales picked from the view distance and blended, so it keeps
// about the same on-screen size from orbit to low altitude.
//
// Needs: cloud-noise.glsl, cloud-uniforms.glsl (planet axes, time) and
// basesphere_uniforms.glsl.

// x: vegetation hue, y: sand hue (both 0..1), z: amount of life; for the
// material colours of terrain-materials.glsl
uniform vec4 SurfaceMaterial;
// x: strength of the ice's cracks 0..1; y: 1 if the body's detail is fixed in space,
// so what is sized by the view stays out; w: how many material slots the body uses
uniform vec4 MaterialLook;

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
#define SURFACE_ICE_CONTRAST 0.3
#define SURFACE_LAVA_CONTRAST 0.45

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

// thin lines where bent noise crosses zero
float surfaceCracks(vec3 q, float width)
{
	return smoothstep(width, 0.0, abs(cloudGradNoise(q + 0.6 * cloudGradNoise(q * 0.37))));
}

float surfaceIcePattern(vec3 q)
{
	return 1.0 + SURFACE_ICE_CONTRAST * (0.1 * cloudGradNoise(q * 0.5) - MaterialLook.x * surfaceCracks(q * 0.8, 0.05));
}

float surfaceLavaPattern(vec3 q)
{
	return 1.0 + SURFACE_LAVA_CONTRAST * (surfaceCracks(q * 0.7, 0.08) * 1.4 - 0.25 + 0.3 * cloudGradNoise(q * 2.0));
}

float surfacePatternAt(vec3 q, vec4 w, float rock, float ice, float lava, float t)
{
	float m = 0.0;
	if (w.x > 0.01) m += w.x * surfaceWaterPattern(q, t);
	if (w.y > 0.01) m += w.y * surfaceSandPattern(q);
	if (w.z > 0.01) m += w.z * surfaceVegetationPattern(q);
	if (w.w > 0.01) m += w.w * surfaceSnowPattern(q);
	if (rock > 0.01) m += rock * surfaceRockPattern(q);
	if (ice > 0.01) m += ice * surfaceIcePattern(q);
	if (lava > 0.01) m += lava * surfaceLavaPattern(q);
	// weights below the thresholds contribute a neutral 1.0
	float used = (w.x > 0.01 ? w.x : 0.0) + (w.y > 0.01 ? w.y : 0.0) + (w.z > 0.01 ? w.z : 0.0) +
		(w.w > 0.01 ? w.w : 0.0) + (rock > 0.01 ? rock : 0.0) + (ice > 0.01 ? ice : 0.0) + (lava > 0.01 ? lava : 0.0);
	return m + (1.0 - used);
}

// Pattern multiplier for the surface colour, around 1.0, for given surface
// type weights (w: water, sand, vegetation, snow; rock, ice, lava).
// eyepos is the view-space position in metres.
float terrainSurfacePatternFor(vec3 eyepos, float distMetres, vec4 w, float rock, float ice, float lava)
{
	// planet-fixed position on the unit sphere
	vec3 P = eyepos * geosphereInvRadius - geosphereCenter;
	vec3 pf = vec3(dot(P, cloudAxisX.xyz), dot(P, cloudAxisY.xyz), dot(P, cloudAxisZ.xyz));

	float level = max(0.0, log2(distMetres * SURFACE_CELL_PER_DIST / SURFACE_MIN_CELL));
	float k = floor(level);
	float f = level - k;
	float cell0 = SURFACE_MIN_CELL * exp2(k);
	float t = cloudTime.x;

	float a = surfacePatternAt(pf * (geosphereRadius / cell0), w, rock, ice, lava, t);
	float b = surfacePatternAt(pf * (geosphereRadius / (cell0 * 2.0)), w, rock, ice, lava, t);
	return mix(a, b, f);
}
