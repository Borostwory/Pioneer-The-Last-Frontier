// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Per-pixel land materials for the terrain shaders.
//
// The planet keeps its colours from the colour fractals (the vertex colour);
// this adds, per pixel:
// - material detail from photo textures (rock, gravel, sand, soil, grass,
//   forest, snow, ice, basalt, lava, ash, regolith, salt), greyscale so any
//   palette works (scripts/make_terrain_materials.py), the two strongest
//   materials of a pixel blended along their bumps so borders look natural;
// - materials the vertex colour cannot show: rock on steep slopes, snow
//   sliding off them, beaches along the shore. These shift the colour too;
// - the glow of lava, from the lava texture's emission.
//
// Each body has up to eight materials (its slots, TerrainMaterialTable in
// the C++); every vertex carries a weight per slot, from the colour fractal.
// Textures are mapped in planet-fixed coordinates, projected onto the cube
// face under the pixel (blended across the face edges), at power-of-two tile
// sizes picked from the pixel's footprint: fine grain and broad mottling
// keep the same size on screen at any distance, and a tile is larger than
// the screen, so it does not visibly repeat. They fade out with distance,
// where the procedural patterns of terrain-surface.glsl take over.
//
// Needs: cloud-uniforms.glsl (planet axes), terrain-surface.glsl.

uniform sampler2DArray terrainMaterials;
// xyz: camera position in planet-fixed axes in units of five 256 m tiles,
// fractional part; and the same in five 128 km tiles (GeoSphere::Render, in
// double precision)
uniform vec4 MaterialOrigin;
uniform vec4 MaterialOriginCoarse;
// texture layer of each of the body's eight material slots (-1: unused)
uniform vec4 MaterialSlots0;
uniform vec4 MaterialSlots1;
// slots for rock on steep slopes, beaches, snow and lava (-1: none)
uniform vec4 MaterialSpecial;

// texture layers (TerrainMaterialTable::Layer)
#define MATERIAL_ROCK 0
#define MATERIAL_GRAVEL 1
#define MATERIAL_SAND 2
#define MATERIAL_SOIL 3
#define MATERIAL_GRASS 4
#define MATERIAL_FOREST 5
#define MATERIAL_SNOW 6
#define MATERIAL_ICE 7
#define MATERIAL_BASALT 8
#define MATERIAL_LAVA 9
#define MATERIAL_ASH 10
#define MATERIAL_REGOLITH 11
#define MATERIAL_SALT 12

#define MATERIAL_SLOTS 8

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

// lava glow: average emission of the lava texture (used where it is not
// sampled), and the glow's colour and strength
#define MATERIAL_LAVA_MEAN_EMISSION 0.1
#define MATERIAL_LAVA_GLOW vec3(2.6, 0.75, 0.12)

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
	if (m == MATERIAL_ICE) return vec3(0.82, 0.88, 0.94);
	if (m == MATERIAL_BASALT) return vec3(0.12, 0.11, 0.11);
	if (m == MATERIAL_LAVA) return vec3(0.15, 0.06, 0.04);
	if (m == MATERIAL_ASH) return vec3(0.18, 0.17, 0.16);
	if (m == MATERIAL_REGOLITH) return vec3(0.45, 0.44, 0.42);
	return vec3(0.85, 0.83, 0.78);
}

// texture layer of a slot (-1: unused)
int materialSlotLayer(int slot)
{
	return int(slot < 4 ? MaterialSlots0[slot] : MaterialSlots1[slot - 4]);
}

// slot weights with this pixel's slope and altitude: steep ground becomes
// rock, snow slides off it, the shore becomes beach
void terrainSlotWeights(vec4 slots0, vec4 slots1, float slope, float altitude, bool hasWater, out float w[MATERIAL_SLOTS])
{
	for (int i = 0; i < 4; ++i) {
		w[i] = slots0[i];
		w[i + 4] = slots1[i];
	}
	int rockSlot = int(MaterialSpecial.x), beachSlot = int(MaterialSpecial.y);
	int snowSlot = int(MaterialSpecial.z), lavaSlot = int(MaterialSpecial.w);

	float steep = smoothstep(MATERIAL_ROCK_SLOPE_START, MATERIAL_ROCK_SLOPE_FULL, slope);
	if (rockSlot >= 0 && steep > 0.0) {
		float moved = 0.0;
		for (int i = 0; i < MATERIAL_SLOTS; ++i) {
			if (i == rockSlot || i == lavaSlot)
				continue;
			float k = (i == snowSlot) ? 0.7 * steep : steep;
			moved += k * w[i];
			w[i] *= 1.0 - k;
		}
		w[rockSlot] += moved;
	}

	if (hasWater && beachSlot >= 0) {
		float beach = smoothstep(MATERIAL_BEACH_TOP, MATERIAL_BEACH_FULL, altitude) * (1.0 - steep);
		if (beach > 0.0) {
			float moved = 0.0;
			for (int i = 0; i < MATERIAL_SLOTS; ++i) {
				if (i == beachSlot || i == rockSlot || i == snowSlot || i == lavaSlot)
					continue;
				moved += beach * w[i];
				w[i] *= 1.0 - beach;
			}
			w[beachSlot] += moved;
		}
	}
}

// pattern weights for the procedural surface patterns (terrain-surface.glsl)
// from the slots: x water (none, it is shaded separately), y sand, z
// vegetation, w snow; the rest is rock
vec4 terrainPatternWeights(vec4 slots0, vec4 slots1, out float rock)
{
	vec4 p = vec4(0.0);
	rock = 0.0;
	for (int i = 0; i < MATERIAL_SLOTS; ++i) {
		float w = i < 4 ? slots0[i] : slots1[i - 4];
		int m = materialSlotLayer(i);
		if (m == MATERIAL_SAND || m == MATERIAL_SALT || m == MATERIAL_ASH)
			p.y += w;
		else if (m == MATERIAL_GRASS || m == MATERIAL_FOREST)
			p.z += w;
		else if (m == MATERIAL_SNOW || m == MATERIAL_ICE)
			p.w += w;
		else if (m >= 0)
			rock += w;
	}
	return p;
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
// number of texture repeats. Emission (x), detail (y), height (z).
vec3 materialTileSample(int layer, MaterialMapping m, float level)
{
	float tile = exp2(level);
	vec3 origin = level <= MATERIAL_FINE_ORIGIN_LEVEL ?
		fract(MaterialOrigin.xyz * exp2(MATERIAL_FINE_ORIGIN_LEVEL - level)) :
		fract(MaterialOriginCoarse.xyz * exp2(MATERIAL_MAX_LEVEL - level));
	vec3 q = m.rel / tile + origin * 5.0;
	vec3 qx = m.relDx / tile, qy = m.relDy / tile;
	vec3 r = m.plane.x * textureGrad(terrainMaterials, vec3(materialPlaneUV(q, m.axes.x), float(layer)),
		materialPlaneUV(qx, m.axes.x), materialPlaneUV(qy, m.axes.x)).rga;
	if (m.plane.y > 0.0)
		r += m.plane.y * textureGrad(terrainMaterials, vec3(materialPlaneUV(q, m.axes.y), float(layer)),
			materialPlaneUV(qx, m.axes.y), materialPlaneUV(qy, m.axes.y)).rga;
	return r;
}

// emission (x), detail (y, mean 1) and height (z) of one material at a tile
// level, blended between the two power-of-two sizes around it
vec3 materialSample(int layer, MaterialMapping m, float level)
{
	level = clamp(level, 0.0, MATERIAL_MAX_LEVEL - 1.0);
	float k = floor(level);
	vec3 r = mix(materialTileSample(layer, m, k), materialTileSample(layer, m, k + 1.0), level - k);
	return vec3(r.x, r.y * 2.0, r.z);
}

// Colour multiplier for the terrain (rgb, around 1): material detail and the
// colour shifts from slope rock, snow and beaches; and the lava glow (a,
// times MATERIAL_LAVA_GLOW, added after lighting).
// slots0, slots1: the vertex's slot weights; albedo: unlit vertex colour;
// mapping: materialMapping() of the pixel; normal, up: view space;
// altitude: metres above sea level (only used with water)
vec4 terrainMaterialDetail(vec4 slots0, vec4 slots1, vec3 albedo, MaterialMapping mapping,
	float distMetres, vec3 normal, vec3 up, float altitude, bool hasWater)
{
	int lavaSlot = int(MaterialSpecial.w);
	float lavaWeight = lavaSlot < 0 ? 0.0 : (lavaSlot < 4 ? slots0[lavaSlot] : slots1[lavaSlot - 4]);
	float farGlow = lavaWeight * MATERIAL_LAVA_MEAN_EMISSION;

	float contrast = 1.0 - smoothstep(MATERIAL_NEAR_DIST, MATERIAL_FAR_DIST, distMetres);
	if (contrast <= 0.0)
		return vec4(1.0, 1.0, 1.0, farGlow);
	float slope = 1.0 - dot(normalize(normal), up);

	// weights as the vertex has them (smooth across the triangle), and with
	// this pixel's slope and altitude
	float w0[MATERIAL_SLOTS];
	float w[MATERIAL_SLOTS];
	terrainSlotWeights(slots0, slots1, 0.0, MATERIAL_BEACH_TOP, hasWater, w0);
	terrainSlotWeights(slots0, slots1, slope, altitude, hasWater, w);

	// the two strongest slots
	int m1 = 0, m2 = 1;
	for (int i = 1; i < MATERIAL_SLOTS; ++i) {
		if (w[i] > w[m1]) {
			m2 = m1;
			m1 = i;
		} else if (i != m1 && w[i] > w[m2]) {
			m2 = i;
		}
	}
	float landWeight = 0.0;
	for (int i = 0; i < MATERIAL_SLOTS; ++i)
		landWeight += w[i];
	if (landWeight < 1e-3)
		return vec4(1.0, 1.0, 1.0, farGlow);

	int l1 = materialSlotLayer(m1), l2 = materialSlotLayer(m2);
	float w1 = w[m1], w2 = l2 >= 0 ? w[m2] : 0.0;
	vec3 d1 = vec3(0.0, 1.0, 0.5), d2 = vec3(0.0, 1.0, 0.5);
	float coarse = mapping.level + log2(MATERIAL_COARSE_SCALE);
	if (l1 >= 0) {
		vec3 f1 = materialSample(l1, mapping, mapping.level);
		vec3 c1 = materialSample(l1, mapping, coarse);
		d1 = vec3(f1.x, f1.y * c1.y, f1.z);
	}
	if (w2 > 0.01) {
		vec3 f2 = materialSample(l2, mapping, mapping.level);
		vec3 c2 = materialSample(l2, mapping, coarse);
		d2 = vec3(f2.x, f2.y * c2.y, f2.z);
	}

	// blend the two along their bumps: the higher one shows through first
	// (only where the textures show; smooth weights further out)
	float total = w1 + w2;
	float s1 = w1 / max(total, 1e-6) + d1.z;
	float s2 = w2 / max(total, 1e-6) + d2.z;
	float top = max(s1, s2) - MATERIAL_BLEND_DEPTH;
	float b1 = max(s1 - top, 0.0), b2 = max(s2 - top, 0.0);
	float n1 = mix(w1, total * b1 / (b1 + b2), contrast);
	float n2 = mix(w2, total * b2 / (b1 + b2), contrast);

	// colour: shift by the materials gained and lost against the vertex
	// colour; near the camera only, so planets keep their look from afar
	vec3 shifted = albedo;
	for (int i = 0; i < MATERIAL_SLOTS; ++i) {
		int layer = materialSlotLayer(i);
		if (layer < 0)
			continue;
		float wi = (i == m1) ? n1 : (i == m2) ? n2 : w[i];
		shifted += contrast * (wi - w0[i]) * materialColour(layer);
		// up close, lava is a dark crust (the colour fractals paint it in
		// its glow colour) and only its cracks glow
		if (layer == MATERIAL_LAVA)
			shifted = mix(shifted, materialColour(MATERIAL_LAVA), contrast * min(wi, 1.0));
	}
	vec3 colourMul = clamp(max(shifted, 0.0) / max(albedo, vec3(0.03)), 0.0, 4.0);

	// detail: the blended materials, the rest of the land neutral
	float detail = (n1 * d1.y + n2 * d2.y + (landWeight - total)) / landWeight;
	detail = 1.0 + (detail - 1.0) * contrast;

	// lava glow: the texture's emission near the camera, its average further out
	float glow = (l1 == MATERIAL_LAVA ? n1 * d1.x : 0.0) + (l2 == MATERIAL_LAVA ? n2 * d2.x : 0.0);
	glow = mix(farGlow, glow, contrast);
	return vec4(colourMul * detail, glow);
}

// ---------------------------------------------------------------- relief

// Relief shading: slopes of the terrain detail between the mesh and the
// pixel, from a tileable slope texture (scripts/make_terrain_relief.py, three
// octaves of waves, 4 to 32 texels) mapped like the materials. Its longest
// waves are the mesh's shortest, twice the vertex spacing, estimated from
// the distance and the terrain detail distance (GeoSphere::Render); a
// second copy 8x smaller continues it down to
// the pixel near the ground (mipmaps fade it where pixels are larger). As
// strong as the materials are rough.
uniform sampler2D terrainRelief;
// x: vertex spacing per metre of distance, y: smallest vertex spacing in
// metres, of the deepest patches (GeoSphere::Render)
uniform vec4 ReliefScale;

#define RELIEF_TEXTURE_SIZE 256.0
// longest waves in the texture, in texels
#define RELIEF_LONGEST_WAVE 32.0
// strength fades to RELIEF_FAR_STRENGTH between these distances (metres)
#define RELIEF_NEAR_DIST 2.0e4
#define RELIEF_FAR_DIST 2.0e5
#define RELIEF_FAR_STRENGTH 0.4

// slope RMS of each material's relief below the mesh
float materialRoughness(int m)
{
	if (m == MATERIAL_ROCK || m == MATERIAL_BASALT) return 0.17;
	if (m == MATERIAL_LAVA) return 0.12;
	if (m == MATERIAL_GRAVEL || m == MATERIAL_ASH) return 0.11;
	if (m == MATERIAL_FOREST) return 0.09;
	if (m == MATERIAL_SOIL || m == MATERIAL_REGOLITH) return 0.07;
	if (m == MATERIAL_GRASS) return 0.06;
	if (m == MATERIAL_SAND || m == MATERIAL_SNOW) return 0.04;
	if (m == MATERIAL_ICE) return 0.03;
	return 0.025;
}

// planet-fixed slope from one cube-face projection
vec3 reliefPlaneSlope(vec3 q, vec3 qx, vec3 qy, int axis)
{
	vec2 s = (textureGrad(terrainRelief, materialPlaneUV(q, axis), materialPlaneUV(qx, axis), materialPlaneUV(qy, axis)).rg * 2.0 - 1.0) * 3.0;
	return axis == 0 ? vec3(0.0, s.x, s.y) : (axis == 1 ? vec3(s.y, 0.0, s.x) : vec3(s.x, s.y, 0.0));
}

vec3 reliefTileSlope(MaterialMapping m, float level)
{
	float tile = exp2(level);
	vec3 origin = level <= MATERIAL_FINE_ORIGIN_LEVEL ?
		fract(MaterialOrigin.xyz * exp2(MATERIAL_FINE_ORIGIN_LEVEL - level)) :
		fract(MaterialOriginCoarse.xyz * exp2(MATERIAL_MAX_LEVEL - level));
	vec3 q = m.rel / tile + origin * 5.0;
	vec3 qx = m.relDx / tile, qy = m.relDy / tile;
	vec3 s = m.plane.x * reliefPlaneSlope(q, qx, qy, m.axes.x);
	if (m.plane.y > 0.0)
		s += m.plane.y * reliefPlaneSlope(q, qx, qy, m.axes.y);
	return s;
}

// The surface normal with the relief added. normal, up: view space;
// m: materialMapping() of the pixel; slots0, slots1: the vertex's material weights
vec3 terrainReliefNormal(vec3 normal, vec3 up, MaterialMapping m, vec4 slots0, vec4 slots1, float distMetres)
{
	float roughness = 0.0;
	for (int i = 0; i < MATERIAL_SLOTS; ++i) {
		int layer = materialSlotLayer(i);
		if (layer >= 0)
			roughness += (i < 4 ? slots0[i] : slots1[i - 4]) * materialRoughness(layer);
	}
	roughness *= mix(1.0, RELIEF_FAR_STRENGTH, smoothstep(RELIEF_NEAR_DIST, RELIEF_FAR_DIST, distMetres));
	if (roughness < 0.005)
		return normal;

	// tile size putting the longest waves at twice the vertex spacing
	float spacing = max(distMetres * ReliefScale.x, ReliefScale.y);
	float level = clamp(log2(max(2.0 * spacing * RELIEF_TEXTURE_SIZE / RELIEF_LONGEST_WAVE, 1.0)), 0.0, MATERIAL_MAX_LEVEL - 1.0);
	float k = floor(level);
	vec3 slope = mix(reliefTileSlope(m, k), reliefTileSlope(m, k + 1.0), level - k);

	// the copy 8x smaller, where its waves are at least a few pixels long
	// (near the ground, where the mesh stops getting finer)
	float footprint = sqrt(length(m.relDx) * length(m.relDy));
	float finerTexel = exp2(level - 3.0) / RELIEF_TEXTURE_SIZE;
	float finer = 0.6 * smoothstep(0.5, 1.0, finerTexel / footprint);
	if (finer > 0.0) {
		float l = clamp(level - 3.0, 0.0, MATERIAL_MAX_LEVEL - 1.0);
		float kf = floor(l);
		slope += finer * mix(reliefTileSlope(m, kf), reliefTileSlope(m, kf + 1.0), l - kf);
	}
	slope *= inversesqrt(1.0 + finer * finer);

	// planet-fixed to view space, along the surface only
	slope = mat3(cloudAxisX.xyz, cloudAxisY.xyz, cloudAxisZ.xyz) * slope * roughness;
	slope -= up * dot(slope, up);
	return normalize(normal - slope);
}
