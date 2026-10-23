// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Per-pixel land materials for the terrain shaders.
//
// The planet keeps its colours from the colour fractals (the vertex colour);
// this adds, per pixel:
// - material detail from photo textures (rock, gravel, sand, soil, grass,
//   forest, snow, ice), greyscale so any palette works
//   (scripts/make_terrain_materials.py), the two strongest materials of a
//   pixel blended along their bumps so borders look natural;
// - materials the vertex colour cannot show: rock on steep slopes, snow
//   sliding off them, beaches along the shore. These shift the colour too.
//
// Materials are picked from the vertex colour (surfaceMaterialWeights() in
// terrain-surface.glsl) plus slope and altitude. Textures are mapped in
// planet-fixed coordinates, projected onto the cube face under the pixel
// (blended across the face edges), at power-of-two tile sizes picked from
// the pixel's footprint: fine grain and broad mottling keep the same size on
// screen at any distance, and a tile is larger than the screen, so it does
// not visibly repeat. They fade out with distance, where the procedural
// patterns of terrain-surface.glsl take over.
//
// Needs: cloud-uniforms.glsl (planet axes), terrain-surface.glsl.

uniform sampler2DArray terrainMaterials;
// xyz: camera position in planet-fixed axes in units of five 256 m tiles,
// fractional part; and the same in five 128 km tiles (GeoSphere::Render, in
// double precision)
uniform vec4 MaterialOrigin;
uniform vec4 MaterialOriginCoarse;

#define MATERIAL_ROCK 0
#define MATERIAL_GRAVEL 1
#define MATERIAL_SAND 2
#define MATERIAL_SOIL 3
#define MATERIAL_GRASS 4
#define MATERIAL_FOREST 5
#define MATERIAL_SNOW 6
#define MATERIAL_ICE 7
#define MATERIAL_COUNT 8

#define MATERIAL_TEXTURE_SIZE 1024.0
// on-screen size of a texel in pixels: grain, and mottling from the same
// texture 8x larger
#define MATERIAL_FINE_PIXELS_PER_TEXEL 2.5
#define MATERIAL_COARSE_SCALE 8.0
// tile sizes are 2^level metres: 1 m to 128 km; MaterialOrigin covers levels
// up to MATERIAL_FINE_ORIGIN_LEVEL, MaterialOriginCoarse the rest
#define MATERIAL_MAX_LEVEL 17.0
#define MATERIAL_FINE_ORIGIN_LEVEL 8.0

// textures at full strength up to this distance (metres), gone beyond the far one
#define MATERIAL_NEAR_DIST 1.5e4
#define MATERIAL_FAR_DIST 5.0e4

// slope (1 - cos of the angle to the vertical) where rock takes over
#define MATERIAL_ROCK_SLOPE_START 0.12
#define MATERIAL_ROCK_SLOPE_FULL 0.30
// beaches: altitude above sea level (metres) where sand starts and is full
#define MATERIAL_BEACH_TOP 8.0
#define MATERIAL_BEACH_FULL 2.0

// how far along their bumps two materials blend (in height units 0..1)
#define MATERIAL_BLEND_DEPTH 0.2

vec3 materialHsv(float h, float s, float v)
{
	vec3 rgb = clamp(abs(mod(h * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
	return v * mix(vec3(1.0), rgb, s);
}

// typical colour of each material on this planet, for the colour shifts
vec3 materialColour(int m)
{
	float vegHue = SurfaceMaterial.x;
	float sandHue = SurfaceMaterial.y;
	if (m == MATERIAL_ROCK) return vec3(0.36, 0.34, 0.32);
	if (m == MATERIAL_GRAVEL) return vec3(0.42, 0.40, 0.37);
	if (m == MATERIAL_SAND) return materialHsv(sandHue, 0.42, 0.72);
	if (m == MATERIAL_SOIL) return materialHsv(sandHue, 0.40, 0.42);
	if (m == MATERIAL_GRASS) return materialHsv(vegHue, 0.55, 0.38);
	if (m == MATERIAL_FOREST) return materialHsv(vegHue, 0.60, 0.20);
	if (m == MATERIAL_SNOW) return vec3(0.90, 0.92, 0.95);
	return vec3(0.82, 0.88, 0.94);
}

// material weights from the vertex colour, the vertex normal and the
// altitude; they add up to 1 minus the water weight
void terrainMaterialWeightsFor(vec3 albedo, float slope, float altitude, bool hasWater, out float w[MATERIAL_COUNT])
{
	vec4 s = surfaceMaterialWeights(albedo, hasWater); // water, sand, vegetation, snow
	float rock = max(0.0, 1.0 - s.x - s.y - s.z - s.w);
	vec3 hsv = surfaceRgbToHsv(clamp(albedo, 0.0, 1.0));

	float forest = s.z * (1.0 - smoothstep(0.2, 0.36, hsv.z));
	float grass = s.z - forest;
	float ice = s.w * smoothstep(0.02, 0.08, albedo.b - albedo.r);
	float snow = s.w - ice;
	// grey rock is bare rock, brownish rock soil, between them gravel
	float bare = rock * smoothstep(0.22, 0.10, hsv.y);
	float soil = (rock - bare) * smoothstep(0.18, 0.35, hsv.y);
	float gravel = rock - bare - soil;
	float sand = s.y;

	// steep ground is bare rock; snow slides off it
	float steep = smoothstep(MATERIAL_ROCK_SLOPE_START, MATERIAL_ROCK_SLOPE_FULL, slope);
	float toRock = steep * (sand + soil + gravel + grass + forest + 0.7 * snow);
	sand *= 1.0 - steep;
	soil *= 1.0 - steep;
	gravel *= 1.0 - steep;
	grass *= 1.0 - steep;
	forest *= 1.0 - steep;
	snow *= 1.0 - 0.7 * steep;
	bare += toRock;

	// beaches along the shore
	if (hasWater) {
		float beach = smoothstep(MATERIAL_BEACH_TOP, MATERIAL_BEACH_FULL, altitude) * (1.0 - steep);
		float toSand = beach * (soil + gravel + grass + forest);
		soil *= 1.0 - beach;
		gravel *= 1.0 - beach;
		grass *= 1.0 - beach;
		forest *= 1.0 - beach;
		sand += toSand;
	}

	w[MATERIAL_ROCK] = bare;
	w[MATERIAL_GRAVEL] = gravel;
	w[MATERIAL_SAND] = sand;
	w[MATERIAL_SOIL] = soil;
	w[MATERIAL_GRASS] = grass;
	w[MATERIAL_FOREST] = forest;
	w[MATERIAL_SNOW] = snow;
	w[MATERIAL_ICE] = ice;
}

// planet-fixed mapping of one pixel, shared by all material samples
struct MaterialMapping {
	vec3 rel, relDx, relDy; // position relative to the camera and its screen derivatives, planet-fixed, metres
	vec2 plane;				// weights of the two cube-face projections used
	ivec2 axes;				// their axes (0: x, 1: y, 2: z)
	float level;			// log2 of the fine tile size in metres
};

MaterialMapping materialMapping(vec3 eyepos, vec3 dpdx, vec3 dpdy, vec3 up)
{
	mat3 toPlanet = transpose(mat3(cloudAxisX.xyz, cloudAxisY.xyz, cloudAxisZ.xyz));
	MaterialMapping m;
	m.rel = toPlanet * eyepos;
	m.relDx = toPlanet * dpdx;
	m.relDy = toPlanet * dpdy;

	// the cube face under the pixel, and the next one near the face edges
	vec3 a = abs(toPlanet * up);
	int first = (a.x > a.y && a.x > a.z) ? 0 : (a.y > a.z ? 1 : 2);
	vec3 rest = a;
	rest[first] = -1.0;
	int second = (rest.x > rest.y && rest.x > rest.z) ? 0 : (rest.y > rest.z ? 1 : 2);
	float edge = smoothstep(0.85, 1.0, a[second] / a[first]);
	m.axes = ivec2(first, second);
	m.plane = vec2(1.0 - 0.5 * edge, 0.5 * edge);

	// tile size from the pixel footprint (geometric mean, so grazing views do
	// not blow the tiles up)
	float footprint = sqrt(length(m.relDx) * length(m.relDy));
	m.level = log2(max(footprint * MATERIAL_TEXTURE_SIZE * MATERIAL_FINE_PIXELS_PER_TEXEL, 1.0));
	return m;
}

// position on the cube-face plane of one axis
vec2 materialPlaneUV(vec3 q, int axis)
{
	return axis == 0 ? q.yz : (axis == 1 ? q.zx : q.xy);
}

// one tile size of 2^level metres; the origin wraps at 5 tiles, a whole
// number of texture repeats
vec2 materialTileSample(int material, MaterialMapping m, float level)
{
	float tile = exp2(level);
	vec3 origin = level <= MATERIAL_FINE_ORIGIN_LEVEL ?
		fract(MaterialOrigin.xyz * exp2(MATERIAL_FINE_ORIGIN_LEVEL - level)) :
		fract(MaterialOriginCoarse.xyz * exp2(MATERIAL_MAX_LEVEL - level));
	vec3 q = m.rel / tile + origin * 5.0;
	vec3 qx = m.relDx / tile, qy = m.relDy / tile;
	vec2 r = m.plane.x * textureGrad(terrainMaterials, vec3(materialPlaneUV(q, m.axes.x), float(material)),
		materialPlaneUV(qx, m.axes.x), materialPlaneUV(qy, m.axes.x)).ga;
	if (m.plane.y > 0.0)
		r += m.plane.y * textureGrad(terrainMaterials, vec3(materialPlaneUV(q, m.axes.y), float(material)),
			materialPlaneUV(qx, m.axes.y), materialPlaneUV(qy, m.axes.y)).ga;
	return r;
}

// detail (x, mean 1) and height (y) of one material at a tile level,
// blended between the two power-of-two sizes around it
vec2 materialSample(int material, MaterialMapping m, float level)
{
	level = clamp(level, 0.0, MATERIAL_MAX_LEVEL - 1.0);
	float k = floor(level);
	vec2 r = mix(materialTileSample(material, m, k), materialTileSample(material, m, k + 1.0), level - k);
	return vec2(r.x * 2.0, r.y);
}

// Colour multiplier for the terrain (around 1): material detail and the
// colour shifts from slope rock, snow and beaches.
// albedo: unlit vertex colour; eyepos: view-space position in metres;
// dpdx, dpdy: its screen derivatives (take them outside branches); normal,
// up: view space; altitude: metres above sea level (only used with water)
vec3 terrainMaterialDetail(vec3 albedo, vec3 eyepos, vec3 dpdx, vec3 dpdy,
	float distMetres, vec3 normal, vec3 up, float altitude, bool hasWater)
{
	float contrast = 1.0 - smoothstep(MATERIAL_NEAR_DIST, MATERIAL_FAR_DIST, distMetres);
	if (contrast <= 0.0)
		return vec3(1.0);
	float slope = 1.0 - dot(normalize(normal), up);

	// weights as the vertex colour has them (smooth across the triangle), and
	// with this pixel's slope and altitude
	float w0[MATERIAL_COUNT];
	float w[MATERIAL_COUNT];
	terrainMaterialWeightsFor(albedo, 0.0, MATERIAL_BEACH_TOP, hasWater, w0);
	terrainMaterialWeightsFor(albedo, slope, altitude, hasWater, w);

	// the two strongest materials
	int m1 = 0, m2 = 1;
	for (int i = 1; i < MATERIAL_COUNT; ++i) {
		if (w[i] > w[m1]) {
			m2 = m1;
			m1 = i;
		} else if (i != m1 && w[i] > w[m2]) {
			m2 = i;
		}
	}
	float landWeight = 0.0;
	for (int i = 0; i < MATERIAL_COUNT; ++i)
		landWeight += w[i];
	if (landWeight < 1e-3)
		return vec3(1.0);

	float w1 = w[m1], w2 = w[m2];
	vec2 d1, d2 = vec2(1.0, 0.5);
	{
		MaterialMapping mapping = materialMapping(eyepos, dpdx, dpdy, up);
		float coarse = mapping.level + log2(MATERIAL_COARSE_SCALE);
		vec2 f1 = materialSample(m1, mapping, mapping.level);
		vec2 c1 = materialSample(m1, mapping, coarse);
		d1 = vec2(f1.x * c1.x, f1.y);
		if (w2 > 0.01) {
			vec2 f2 = materialSample(m2, mapping, mapping.level);
			vec2 c2 = materialSample(m2, mapping, coarse);
			d2 = vec2(f2.x * c2.x, f2.y);
		}
	}

	// blend the two along their bumps: the higher one shows through first
	// (only where the textures show; smooth weights further out)
	float total = w1 + w2;
	float s1 = w1 / max(total, 1e-6) + d1.y;
	float s2 = w2 / max(total, 1e-6) + d2.y;
	float top = max(s1, s2) - MATERIAL_BLEND_DEPTH;
	float b1 = max(s1 - top, 0.0), b2 = max(s2 - top, 0.0);
	float n1 = mix(w1, total * b1 / (b1 + b2), contrast);
	float n2 = mix(w2, total * b2 / (b1 + b2), contrast);

	// colour: shift by the materials gained and lost against the vertex
	// colour; near the camera only, so planets keep their look from afar
	vec3 shifted = albedo;
	for (int i = 0; i < MATERIAL_COUNT; ++i) {
		float wi = (i == m1) ? n1 : (i == m2) ? n2 : w[i];
		shifted += contrast * (wi - w0[i]) * materialColour(i);
	}
	vec3 colourMul = clamp(max(shifted, 0.0) / max(albedo, vec3(0.03)), 0.0, 4.0);

	// detail: the blended materials, the rest of the land neutral
	float detail = (n1 * d1.x + n2 * d2.x + (landWeight - total)) / landWeight;
	detail = 1.0 + (detail - 1.0) * contrast;
	return colourMul * detail;
}
