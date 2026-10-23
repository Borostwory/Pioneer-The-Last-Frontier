// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Per-pixel water for the terrain shaders: a coastline from the altitude the
// patch jobs store in the vertex colour's alpha, depth tint, wave normals,
// Fresnel reflection of the sky and a GGX sun glint.
//
// Waves come from a tileable slope texture (scripts/make_water_waves.py) in
// three layers of 2048 m, 128 m and 8 m tiles, each holding wavelengths of
// 1/2 to 1/32 of the tile: 1 km down to 25 cm. The texture stores slopes and
// squared slopes (LEAN mapping), so where its mipmaps average waves away their
// slope variance is still known and goes into the glint's roughness: a sharp
// sparkle close up, a broad sheen from orbit. The layers are mapped
// triplanar in planet-fixed axes, and each is sampled twice, the second time
// rotated, to hide the repetition of the tile.
//
// Needs: cloud-uniforms.glsl (planet axes) and basesphere_uniforms.glsl.

uniform sampler2D waterWaves;
// xyz: camera position in planet-fixed axes in units of five 2048 m tiles,
// fractional part (GeoSphere::Render, in double precision)
uniform vec4 WaterOrigin;
// xyz: drift of the three layers in tiles, modulo 5
uniform vec4 WaterDrift;

// altitude encoding, see EncodeAltitude() in GeoPatchJobs.cpp
#define WATER_ALTITUDE_RANGE 8000.0

#define WATER_LARGEST_TILE 2048.0
// slope variance (both axes) of one layer's four octaves of waves, and of the
// ripples below the smallest layer; 0.04 in all, a light breeze
#define WATER_LAYER_VARIANCE 0.0125
#define WATER_RIPPLE_VARIANCE 0.003

// signed altitude above sea level in metres, from the encoded vertex alpha
// (as interpolated: s = 2 * alpha - 1)
float terrainAltitude(float s)
{
	return sign(s) * s * s * WATER_ALTITUDE_RANGE;
}

// 0 on land, 1 on water, anti-aliased across the coastline
float terrainWaterMask(float s)
{
	float w = max(fwidth(s), 1e-4);
	return 1.0 - smoothstep(-w, w, s);
}

struct WaterSurface {
	vec3 N;	  // wave normal, view space
	float a2; // GGX roughness squared, from the waves too small for the pixel
};

// one texture sample: mean slope (xy) and slope variance (z), in units of
// the slope RMS
vec3 waterTexture(vec2 uv, vec2 dx, vec2 dy)
{
	vec4 t = textureGrad(waterWaves, uv, dx, dy);
	vec2 m = (t.rg * 2.0 - 1.0) * 3.0;
	vec2 m2 = t.ba * 9.0;
	return vec3(m, max(m2.x + m2.y - dot(m, m), 0.0));
}

// one layer, one triplanar plane: the tile, and the tile rotated by 53
// degrees, averaged (keeping the slope variance). The rotation is an integer
// matrix divided by 5, so with the origin and drift wrapping at multiples of
// 5 tiles the rotated copy tiles seamlessly too.
vec3 waterLayerSample(vec2 uv, vec2 dx, vec2 dy, float drift)
{
	const mat2 rot = mat2(0.6, 0.8, -0.8, 0.6);
	vec3 a = waterTexture(uv + vec2(drift, 0.0), dx, dy);
	vec3 b = waterTexture(rot * uv + vec2(drift + 0.37, 0.61), rot * dx, rot * dy);
	return vec3((a.xy + transpose(rot) * b.xy) * 0.70711, (a.z + b.z) * 0.5);
}

// eyepos: view-space position in metres; up: surface normal of the sea;
// dpdx, dpdy: screen-space derivatives of eyepos (take them outside branches)
WaterSurface waterSurface(vec3 eyepos, vec3 up, vec3 dpdx, vec3 dpdy)
{
	// planet-fixed axes, relative to the camera: precise near the camera,
	// where the small tiles are
	mat3 toPlanet = transpose(mat3(cloudAxisX.xyz, cloudAxisY.xyz, cloudAxisZ.xyz));
	vec3 rel = toPlanet * eyepos;
	vec3 relDx = toPlanet * dpdx;
	vec3 relDy = toPlanet * dpdy;
	vec3 tri = pow(abs(toPlanet * up), vec3(4.0));
	tri /= tri.x + tri.y + tri.z;

	vec3 slope = vec3(0.0);
	float variance = WATER_RIPPLE_VARIANCE;
	float tile = WATER_LARGEST_TILE;
	float scale = 1.0;
	float layerSigma = sqrt(WATER_LAYER_VARIANCE);
	for (int i = 0; i < 3; ++i) {
		vec3 q = rel / tile + fract(WaterOrigin.xyz * scale) * 5.0;
		vec3 qx = relDx / tile, qy = relDy / tile;
		vec3 a = waterLayerSample(q.yz, qx.yz, qy.yz, WaterDrift[i]);
		vec3 b = waterLayerSample(q.zx, qx.zx, qy.zx, WaterDrift[i]);
		vec3 c = waterLayerSample(q.xy, qx.xy, qy.xy, WaterDrift[i]);
		slope += layerSigma * (tri.x * vec3(0.0, a.x, a.y) + tri.y * vec3(b.y, 0.0, b.x) + tri.z * vec3(c.x, c.y, 0.0));
		variance += WATER_LAYER_VARIANCE * dot(tri, vec3(a.z, b.z, c.z));
		tile *= 1.0 / 16.0;
		scale *= 16.0;
	}

	// planet-fixed slope back to view space, minus the part along the normal
	slope = transpose(toPlanet) * slope;
	slope -= up * dot(slope, up);

	WaterSurface s;
	s.N = normalize(up - slope);
	s.a2 = variance;
	return s;
}

float waterFresnel(float cosTheta)
{
	return 0.02 + 0.98 * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

// GGX sun glint: radiance per unit of irradiance from the light
// V: towards the eye; L: towards the light
float waterSpecular(WaterSurface s, vec3 V, vec3 L)
{
	vec3 H = normalize(L + V);
	float NV = max(dot(s.N, V), 1e-3);
	float NL = max(dot(s.N, L), 0.0);
	float NH = max(dot(s.N, H), 0.0);
	float d = NH * NH * (s.a2 - 1.0) + 1.0;
	float D = s.a2 / (PI * d * d);
	float k = sqrt(s.a2) * 0.5;
	float G = NV / (NV * (1.0 - k) + k) * NL / (NL * (1.0 - k) + k);
	return D * G * waterFresnel(dot(V, H)) / (4.0 * NV);
}

// albedo of the water body: the terrain's water colour in deep water, lighter
// and greener over shallows
vec3 waterBody(vec3 albedo, float depth)
{
	vec3 deep = albedo * 0.35 + vec3(0.0, 0.006, 0.01);
	vec3 shallow = deep * 1.5 + vec3(0.03, 0.10, 0.08);
	return mix(shallow, deep, smoothstep(2.0, 60.0, depth));
}
