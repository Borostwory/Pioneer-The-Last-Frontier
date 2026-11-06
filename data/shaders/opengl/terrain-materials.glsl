// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Per-pixel land materials for the terrain shaders.
//
// The planet keeps its colours from the colour fractals (the vertex colour);
// this adds, per pixel:
// - material detail from photo textures (rock, gravel, sand, soil, grass,
//   forest, snow, ice, basalt, lava, ash, regolith, salt, tundra, dirty ice,
//   cooled lava flows, cracked mud, dust, metal), greyscale so any
//   palette works (scripts/make_terrain_materials.py), the two strongest
//   materials of a pixel blended along their bumps so borders look natural;
// - materials the vertex colour cannot show: bare ground on steep slopes, snow
//   sliding off them, beaches along the shore. These shift the colour too;
// - the glow of lava, from the lava texture's emission;
// - how the materials take the light: gloss on ice, lava and metal, blue
//   shadows on ice, backscatter on airless regolith and dust.
//
// Each body has up to sixteen materials (its slots, TerrainMaterialTable in
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
// texture layer of each of the body's sixteen material slots (-1: unused)
uniform vec4 MaterialSlots0;
uniform vec4 MaterialSlots1;
uniform vec4 MaterialSlots2;
uniform vec4 MaterialSlots3;
// slots for bare steep ground, beaches, snow and lava (-1: none)
uniform vec4 MaterialSpecial;

// the atlas of terrain detail tiles (TerrainTiles in the C++, terrain-tile.glsl)
uniform sampler2D terrainTiles;
// xy: atlas coordinates of this patch's tile (or of its part of an
// ancestor's, until its own is baked), z: its size; w: 1 if the patch has one
uniform vec4 PatchTile;
// as TILE_SLOPE_RANGE in terrain-tile.glsl
#define TERRAIN_TILE_SLOPE_RANGE 1.5

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
#define MATERIAL_TUNDRA 13
#define MATERIAL_DIRTY_ICE 14
#define MATERIAL_LAVA_FLOW 15
#define MATERIAL_CRACKED_MUD 16
#define MATERIAL_DUST 17
#define MATERIAL_METAL 18
#define MATERIAL_FOREST_FAR 19

#define MATERIAL_SLOTS 16
// the weights come as this many vec4 (materialWeights0.. in the shaders)
#define MATERIAL_SLOT_GROUPS 4
// slots this body uses; the rest are empty
#define MATERIAL_USED_SLOTS int(MaterialLook.w)

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

// detail fixed in space (MaterialLook.y): textures stay fine ground grain, with a tile
// no larger than this level, so no pattern in a photo grows with the distance;
// the generic relief stops at the far distance
#define MATERIAL_FIXED_MAX_LEVEL 6.0
#define MATERIAL_FIXED_RELIEF_NEAR 500.0
#define MATERIAL_FIXED_RELIEF_FAR 3000.0

// how far along their bumps two materials blend (in height units 0..1)
#define MATERIAL_BLEND_DEPTH 0.2

// bump lighting: height of the textures' bumps as a fraction of the fine
// tile size
#define MATERIAL_BUMP_HEIGHT 0.008

// lava glow: average emission of the lava texture (used where it is not
// sampled), and the glow's colour and strength
#define MATERIAL_LAVA_MEAN_EMISSION 0.1
#define MATERIAL_LAVA_GLOW vec3(4.0, 1.15, 0.18)
// glow stops just short of the sea: altitude (metres) where it starts and is full
#define MATERIAL_GLOW_SHORE_LOW 2.0
#define MATERIAL_GLOW_SHORE_HIGH 30.0

// FOREST turns from its forest floor to its canopy between these tile levels
// (from some hundreds of metres up to a few km)
#define MATERIAL_FOREST_NEAR_LEVEL 7.0
#define MATERIAL_FOREST_FAR_LEVEL 10.0

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
	if (m == MATERIAL_TUNDRA) return materialHsv(vegHue, 0.30, 0.36);
	if (m == MATERIAL_DIRTY_ICE) return vec3(0.33, 0.31, 0.28);
	if (m == MATERIAL_LAVA_FLOW) return vec3(0.09, 0.07, 0.07);
	if (m == MATERIAL_CRACKED_MUD) return vec3(0.55, 0.46, 0.35);
	if (m == MATERIAL_DUST) return materialHsv(sandHue, 0.35, 0.55);
	if (m == MATERIAL_METAL) return vec3(0.42, 0.40, 0.37);
	return vec3(0.85, 0.83, 0.78);
}

// The patch's detail tile at patch coordinates uv: the slope of the
// landforms below the mesh (xyz, planet-fixed) and the landform value (w:
// high on crests, ridges and crater rims, low in hollows; 0.5 without a tile).
vec4 terrainTileSample(vec2 uv)
{
	if (PatchTile.w <= 0.0)
		return vec4(0.0, 0.0, 0.0, 0.5);
	vec4 t = textureLod(terrainTiles, PatchTile.xy + clamp(uv, 0.0, 1.0) * PatchTile.z, 0.0);
	return vec4((t.rgb * 2.0 - 1.0) * TERRAIN_TILE_SLOPE_RANGE, t.a);
}

// How much of the sky the ground at a pixel sees, 0..1, whatever the sun is doing, so its shape shows at noon too: slopes
// and hollows are darker. Only where detail is fixed in space, and the less the more air lights them from all round.
float terrainOcclusion(vec3 normal, vec3 up, vec4 tile)
{
	if (MaterialLook.y < 0.5)
		return 1.0;
	float level = clamp(dot(normal, up), 0.0, 1.0);
	float slopes = level * level * level;
	float hollows = mix(0.74, 1.08, smoothstep(0.25, 0.6, tile.w));
	return mix(1.0, slopes * hollows, 1.0 - 0.7 * airShare());
}

// texture layer of a slot (-1: unused)
int materialSlotLayer(int slot)
{
	vec4 group = slot < 8 ? (slot < 4 ? MaterialSlots0 : MaterialSlots1) : (slot < 12 ? MaterialSlots2 : MaterialSlots3);
	return int(group[slot & 3]);
}

// a vertex's weight of a slot
float materialSlotWeight(vec4 slots[MATERIAL_SLOT_GROUPS], int slot)
{
	return slots[slot >> 2][slot & 3];
}

// slot weights with this pixel's slope and altitude: steep ground becomes
// the body's bare ground, snow slides off it, the shore becomes beach
void terrainSlotWeights(vec4 slots[MATERIAL_SLOT_GROUPS], float slope, float altitude, bool hasWater, out float w[MATERIAL_SLOTS])
{
	for (int i = 0; i < MATERIAL_SLOTS; ++i)
		w[i] = materialSlotWeight(slots, i);
	int rockSlot = int(MaterialSpecial.x), beachSlot = int(MaterialSpecial.y);
	int snowSlot = int(MaterialSpecial.z), lavaSlot = int(MaterialSpecial.w);

	float steep = smoothstep(MATERIAL_ROCK_SLOPE_START, MATERIAL_ROCK_SLOPE_FULL, slope);
	if (rockSlot >= 0 && steep > 0.0) {
		float moved = 0.0;
		for (int i = 0; i < MATERIAL_USED_SLOTS; ++i) {
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
			for (int i = 0; i < MATERIAL_USED_SLOTS; ++i) {
				int layer = materialSlotLayer(i);
				// (lava's crust and ash stay black down to the water)
				if (i == beachSlot || i == rockSlot || i == snowSlot || i == lavaSlot || layer == MATERIAL_ASH || layer == MATERIAL_LAVA_FLOW)
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
// vegetation, w snow; ice and lava have their own; the rest is rock
vec4 terrainPatternWeights(vec4 slots[MATERIAL_SLOT_GROUPS], out float rock, out float ice, out float lava)
{
	vec4 p = vec4(0.0);
	rock = 0.0;
	ice = 0.0;
	lava = 0.0;
	for (int i = 0; i < MATERIAL_USED_SLOTS; ++i) {
		float w = materialSlotWeight(slots, i);
		int m = materialSlotLayer(i);
		if (m == MATERIAL_SAND || m == MATERIAL_SALT || m == MATERIAL_ASH || m == MATERIAL_DUST)
			p.y += w;
		else if (m == MATERIAL_GRASS || m == MATERIAL_FOREST || m == MATERIAL_TUNDRA)
			p.z += w;
		else if (m == MATERIAL_SNOW)
			p.w += w;
		else if (m == MATERIAL_ICE)
			ice += w;
		else if (m == MATERIAL_LAVA)
			lava += w;
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
	if (MaterialLook.y > 0.5)
		m.level = min(m.level, MATERIAL_FIXED_MAX_LEVEL);
	return m;
}

// position on the cube-face plane of one axis
vec2 materialPlaneUV(vec3 q, int axis)
{
	return axis == 0 ? q.yz : (axis == 1 ? q.zx : q.xy);
}

// where a pixel is in tiles of 2^level metres; the origin wraps at 5 tiles, a whole number of texture repeats
vec3 materialTileCoord(MaterialMapping m, float level)
{
	vec3 origin = level <= MATERIAL_FINE_ORIGIN_LEVEL ?
		fract(MaterialOrigin.xyz * exp2(MATERIAL_FINE_ORIGIN_LEVEL - level)) :
		fract(MaterialOriginCoarse.xyz * exp2(MATERIAL_MAX_LEVEL - level));
	return m.rel / exp2(level) + origin * 5.0;
}

// smooth noise 0..1 repeating every 5 cells, so it goes on across the origin's wrap
float materialRegionNoise(vec2 p, float salt)
{
	vec2 c = floor(p), t = p - c;
	t = t * t * (3.0 - 2.0 * t);
	vec2 next = mod(c + 1.0, 5.0);
	c = mod(c, 5.0);
	vec4 h = fract(sin(vec4(dot(c, vec2(12.9898, 78.233)), dot(vec2(next.x, c.y), vec2(12.9898, 78.233)),
		dot(vec2(c.x, next.y), vec2(12.9898, 78.233)), dot(next, vec2(12.9898, 78.233))) + salt) * 43758.5453);
	return mix(mix(h.x, h.y, t.x), mix(h.z, h.w, t.x), t.y);
}

// The turns of the tile. These are the ones that take a step of 5 tiles to a whole number of them (the 3, 4, 5 triangle),
// so turning doesn't show where the origin wraps.
const vec2 MATERIAL_TURNS[8] = vec2[8](vec2(1.0, 0.0), vec2(0.6, 0.8), vec2(0.0, 1.0), vec2(-0.8, 0.6), vec2(-1.0, 0.0), vec2(-0.6, -0.8), vec2(0.0, -1.0), vec2(0.8, -0.6));

// Reads the tile turned and shifted one way or the next, by a field fixed in space, blended along their bumps: no two
// places alike, and no grid. uvFar: this place in tiles 32 times the size, for the field's long repeats.
vec3 materialVariedRead(int layer, vec2 uv, vec2 dx, vec2 dy, vec2 uvFar)
{
	float field = 8.0 * fract(materialRegionNoise(uv, 0.0) + 3.0 * materialRegionNoise(uvFar, 5.7));
	float index = floor(field);
	vec2 a = MATERIAL_TURNS[int(index)], b = MATERIAL_TURNS[int(mod(index + 1.0, 8.0))];
	vec2 offsetA = fract(vec2(0.7548777, 0.5698403) * index), offsetB = fract(vec2(0.7548777, 0.5698403) * (index + 1.0));
	vec3 ra = textureGrad(terrainMaterials, vec3(vec2(a.x * uv.x - a.y * uv.y, a.y * uv.x + a.x * uv.y) + offsetA, float(layer)),
		vec2(a.x * dx.x - a.y * dx.y, a.y * dx.x + a.x * dx.y), vec2(a.x * dy.x - a.y * dy.y, a.y * dy.x + a.x * dy.y)).rga;
	vec3 rb = textureGrad(terrainMaterials, vec3(vec2(b.x * uv.x - b.y * uv.y, b.y * uv.x + b.x * uv.y) + offsetB, float(layer)),
		vec2(b.x * dx.x - b.y * dx.y, b.y * dx.x + b.x * dx.y), vec2(b.x * dy.x - b.y * dy.y, b.y * dy.x + b.x * dy.y)).rga;
	return mix(ra, rb, smoothstep(0.25, 0.75, field - index + 0.8 * (rb.z - ra.z)));
}

// one tile size of 2^level metres. Emission (x), detail (y), height (z).
vec3 materialTileSample(int layer, MaterialMapping m, float level)
{
	float tile = exp2(level);
	vec3 q = materialTileCoord(m, level);
	vec3 qx = m.relDx / tile, qy = m.relDy / tile;
	// (detail fixed in space: varied, everywhere else as it was)
	if (MaterialLook.y > 0.5) {
		vec3 qFar = materialTileCoord(m, min(level + 5.0, MATERIAL_MAX_LEVEL));
		vec3 varied = m.plane.x * materialVariedRead(layer, materialPlaneUV(q, m.axes.x), materialPlaneUV(qx, m.axes.x), materialPlaneUV(qy, m.axes.x), materialPlaneUV(qFar, m.axes.x));
		if (m.plane.y > 0.0)
			varied += m.plane.y * materialVariedRead(layer, materialPlaneUV(q, m.axes.y), materialPlaneUV(qx, m.axes.y), materialPlaneUV(qy, m.axes.y), materialPlaneUV(qFar, m.axes.y));
		return varied;
	}
	vec3 r = m.plane.x * textureGrad(terrainMaterials, vec3(materialPlaneUV(q, m.axes.x), float(layer)),
		materialPlaneUV(qx, m.axes.x), materialPlaneUV(qy, m.axes.x)).rga;
	if (m.plane.y > 0.0)
		r += m.plane.y * textureGrad(terrainMaterials, vec3(materialPlaneUV(q, m.axes.y), float(layer)),
			materialPlaneUV(qx, m.axes.y), materialPlaneUV(qy, m.axes.y)).rga;
	return r;
}

// Soft, uneven patches of brighter and duller glow for lava seen from afar, kilometres across, from the lava
// texture itself, averaged a long way down; around 1.
float materialLavaPatches(MaterialMapping m)
{
	float level = 13.0;
	vec3 origin = fract(MaterialOriginCoarse.xyz * exp2(MATERIAL_MAX_LEVEL - level));
	vec3 q = m.rel / exp2(level) + origin * 5.0;
	float e = textureGrad(terrainMaterials, vec3(materialPlaneUV(q, m.axes.x), float(MATERIAL_LAVA)), vec2(0.03, 0.0), vec2(0.0, 0.03)).r;
	return clamp(1.0 + (e - 0.078) * 12.0, 0.25, 2.0);
}

// emission (x), detail (y, mean 1) and height (z) of one material at a tile
// level, blended between the two power-of-two sizes around it
vec3 materialSample(int layer, MaterialMapping m, float level)
{
	level = clamp(level, 0.0, MATERIAL_MAX_LEVEL - 1.0);
	float k = floor(level);
	vec3 r = mix(materialTileSample(layer, m, k), materialTileSample(layer, m, k + 1.0), level - k);
	// forest: its floor on foot, its canopy from the air
	if (layer == MATERIAL_FOREST) {
		float canopy = smoothstep(MATERIAL_FOREST_NEAR_LEVEL, MATERIAL_FOREST_FAR_LEVEL, m.level);
		if (canopy > 0.0)
			r = mix(r, mix(materialTileSample(MATERIAL_FOREST_FAR, m, k), materialTileSample(MATERIAL_FOREST_FAR, m, k + 1.0), level - k), canopy);
	}
	r.y *= 2.0;
	// the ice photo has its cracks built in: as strong as this body's are
	if (layer == MATERIAL_ICE) {
		r.y = 1.0 + (r.y - 1.0) * MaterialLook.x;
		r.z = 0.5 + (r.z - 0.5) * MaterialLook.x;
	}
	return r;
}

// how far each material's bumps stand out
float materialBumpStrength(int m)
{
	if (m == MATERIAL_DUST) return 0.25;
	if (m == MATERIAL_LAVA || m == MATERIAL_ICE) return 0.35;
	if (m == MATERIAL_SNOW || m == MATERIAL_ASH) return 0.5;
	if (m == MATERIAL_SAND) return 0.25;
	if (m == MATERIAL_LAVA_FLOW) return 0.6;
	return 1.0;
}

// Colour multiplier for the terrain (rgb, around 1): material detail and the
// colour shifts from steep ground, snow and beaches; and the lava glow (a,
// times MATERIAL_LAVA_GLOW, added after lighting).
// slots: the vertex's slot weights; albedo: unlit vertex colour;
// mapping: materialMapping() of the pixel; normal, up: view space;
// altitude: metres above sea level (only used with water); landform: from
// the detail tile (0.5 without one): bare rock on crests and rims.
// bump: the blended texture height (x, 0..1) and its scale in metres (y),
// for terrainBumpNormal()
vec4 terrainMaterialDetail(vec4 slots[MATERIAL_SLOT_GROUPS], vec3 albedo, MaterialMapping mapping,
	float distMetres, vec3 normal, vec3 up, float altitude, bool hasWater, float landform, out vec2 bump)
{
	bump = vec2(0.5, 0.0);
	int lavaSlot = int(MaterialSpecial.w);
	float lavaWeight = lavaSlot < 0 ? 0.0 : materialSlotWeight(slots, lavaSlot);
	float farGlow = lavaWeight * MATERIAL_LAVA_MEAN_EMISSION;

	float contrast = 1.0 - smoothstep(MATERIAL_NEAR_DIST, MATERIAL_FAR_DIST, distMetres);
	if (contrast <= 0.0)
		return vec4(1.0, 1.0, 1.0, farGlow);
	float slope = 1.0 - dot(normalize(normal), up);

	// weights as the vertex has them (smooth across the triangle), and with
	// this pixel's slope and altitude
	float w0[MATERIAL_SLOTS];
	float w[MATERIAL_SLOTS];
	terrainSlotWeights(slots, 0.0, MATERIAL_BEACH_TOP, hasWater, w0);
	terrainSlotWeights(slots, slope, altitude, hasWater, w);
	int usedSlots = MATERIAL_USED_SLOTS;
	int rockSlot = int(MaterialSpecial.x);
	float crest = 0.6 * smoothstep(0.7, 0.9, landform);
	if (rockSlot >= 0 && crest > 0.0) {
		float moved = 0.0;
		for (int i = 0; i < usedSlots; ++i) {
			if (i == rockSlot || i == int(MaterialSpecial.w))
				continue;
			moved += crest * w[i];
			w[i] *= 1.0 - crest;
		}
		w[rockSlot] += moved;
	}

	// the two strongest slots
	int m1 = 0, m2 = 1;
	for (int i = 1; i < usedSlots; ++i) {
		if (w[i] > w[m1]) {
			m2 = m1;
			m1 = i;
		} else if (i != m1 && w[i] > w[m2]) {
			m2 = i;
		}
	}
	float landWeight = 0.0;
	for (int i = 0; i < usedSlots; ++i)
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
	for (int i = 0; i < usedSlots; ++i) {
		int layer = materialSlotLayer(i);
		if (layer < 0)
			continue;
		float wi = (i == m1) ? n1 : (i == m2) ? n2 : w[i];
		// (with detail fixed in space, the vertex colours are the materials' own; these
		// generic colours would turn floors tan and the walls of dark moons white)
		if (MaterialLook.y < 0.5)
			shifted += contrast * (wi - w0[i]) * materialColour(layer);
		// up close, lava is a dark crust (the colour fractals paint it in
		// its glow colour) and only its cracks glow
		if (layer == MATERIAL_LAVA)
			shifted = mix(shifted, materialColour(MATERIAL_LAVA), contrast * min(wi, 1.0));
	}
	vec3 colourMul = clamp(max(shifted, 0.0) / max(albedo, vec3(0.03)), 0.0, 4.0);

	// detail: the blended materials, the rest of the land neutral
	float detail = (n1 * d1.y + n2 * d2.y + (landWeight - total)) / landWeight;

	// the bumps, the rest of the land flat
	bump.x = (n1 * d1.z + n2 * d2.z + 0.5 * (landWeight - total)) / landWeight;
	float bumps = (n1 * (l1 >= 0 ? materialBumpStrength(l1) : 1.0) + n2 * (l2 >= 0 ? materialBumpStrength(l2) : 1.0) + (landWeight - total)) / landWeight;
	bump.y = contrast * MATERIAL_BUMP_HEIGHT * exp2(mapping.level) * bumps;
	detail = 1.0 + (detail - 1.0) * contrast;

	// lava glow: the texture's emission near the camera, its average further out
	float glow = (l1 == MATERIAL_LAVA ? n1 * d1.x : 0.0) + (l2 == MATERIAL_LAVA ? n2 * d2.x : 0.0);
	if (MaterialLook.y > 0.5) {
		// (detail fixed in space has a small tile, whose bright cracks repeat as a grid if seen from any distance:
		// the cracks only up close, and uneven patches beyond that)
		float patches = lavaWeight > 0.0 ? mix(1.0, materialLavaPatches(mapping), contrast) : 1.0;
		glow = mix(farGlow * patches, glow, 1.0 - smoothstep(800.0, 3000.0, distMetres));
	} else {
		glow = mix(farGlow, glow, contrast);
	}
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
	if (m == MATERIAL_ROCK) return 0.17;
	if (m == MATERIAL_BASALT) return 0.12;
	if (m == MATERIAL_GRAVEL) return 0.11;
	if (m == MATERIAL_DIRTY_ICE) return 0.10;
	if (m == MATERIAL_FOREST) return 0.09;
	if (m == MATERIAL_SOIL || m == MATERIAL_CRACKED_MUD) return 0.07;
	if (m == MATERIAL_REGOLITH || m == MATERIAL_GRASS || m == MATERIAL_TUNDRA) return 0.06;
	if (m == MATERIAL_ASH || m == MATERIAL_LAVA_FLOW || m == MATERIAL_METAL) return 0.05;
	if (m == MATERIAL_SAND || m == MATERIAL_SNOW) return 0.04;
	if (m == MATERIAL_SALT) return 0.025;
	return 0.02; // lava, ice and dust are the smoothest
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
// m: materialMapping() of the pixel; slots: the vertex's material
// weights; tile: terrainTileSample(), which replaces the generic relief's
// longer waves where the patch has a tile; baseScale: the detail of the
// surface below (the vertices, or the baked surface of terrain-far.glsl) as
// a fraction of the vertex spacing: the relief starts below that
vec3 terrainReliefNormal(vec3 normal, vec3 up, MaterialMapping m, vec4 slots[MATERIAL_SLOT_GROUPS], float distMetres, vec4 tile, float baseScale)
{
	float roughness = 0.0;
	for (int i = 0; i < MATERIAL_USED_SLOTS; ++i) {
		int layer = materialSlotLayer(i);
		if (layer >= 0)
			roughness += materialSlotWeight(slots, i) * materialRoughness(layer);
	}
	roughness *= mix(1.0, RELIEF_FAR_STRENGTH, smoothstep(RELIEF_NEAR_DIST, RELIEF_FAR_DIST, distMetres));
	if (roughness < 0.005)
		return normal;

	// tile size putting the longest waves at twice the spacing of the
	// surface below
	float spacing = max(distMetres * ReliefScale.x, ReliefScale.y) * baseScale;
	float level = clamp(log2(max(2.0 * spacing * RELIEF_TEXTURE_SIZE / RELIEF_LONGEST_WAVE, 1.0)), 0.0, MATERIAL_MAX_LEVEL - 1.0);
	float k = floor(level);
	vec3 slope;
	if (PatchTile.w > 0.0) {
		// the detail tile: the planet's own landforms, as strong as the
		// materials are rough (and visible on smooth ones). Kept whatever the
		// surface below: fading them where it is the far texture left patches
		// without a surface tile smooth next to those with one
		slope = tile.xyz * clamp(roughness / 0.1, 0.5, 1.6) / roughness;
	} else if (MaterialLook.y > 0.5) {
		slope = vec3(0.0);
	} else {
		slope = mix(reliefTileSlope(m, k), reliefTileSlope(m, k + 1.0), level - k);
	}

	// the copy 8x smaller, where its waves are at least a few pixels long
	// (near the ground, where the mesh stops getting finer)
	float footprint = sqrt(length(m.relDx) * length(m.relDy));
	float finerTexel = exp2(level - 3.0) / RELIEF_TEXTURE_SIZE;
	float finer = 0.6 * smoothstep(0.5, 1.0, finerTexel / footprint);
	if (MaterialLook.y > 0.5)
		finer *= 1.0 - smoothstep(MATERIAL_FIXED_RELIEF_NEAR, MATERIAL_FIXED_RELIEF_FAR, distMetres);
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

// ---------------------------------------------------------------- how materials take the light

// x: ice, y: lava, z: metal, w: regolith and dust, from the vertex's weights
vec4 materialLightWeights(vec4 slots[MATERIAL_SLOT_GROUPS])
{
	vec4 look = vec4(0.0);
	for (int i = 0; i < MATERIAL_USED_SLOTS; ++i) {
		int m = materialSlotLayer(i);
		float w = materialSlotWeight(slots, i);
		if (m == MATERIAL_ICE)
			look.x += w;
		else if (m == MATERIAL_LAVA)
			look.y += w;
		else if (m == MATERIAL_METAL)
			look.z += w;
		else if (m == MATERIAL_REGOLITH || m == MATERIAL_DUST)
			look.w += w;
	}
	return look;
}

// sun glint on ice, lava and metal, weaker from afar so that a whole moon
// doesn't carry one highlight. N, V, L: view space, unit length
float materialGloss(vec4 look, vec3 N, vec3 V, vec3 L, float distMetres)
{
	float weight = look.x + look.y + look.z;
	if (weight < 0.01)
		return 0.0;
	float NdotL = dot(N, L);
	if (NdotL <= 0.0)
		return 0.0;
	float strength = 0.35 * look.x + 0.5 * look.y + 0.6 * look.z;
	float shine = (80.0 * look.x + 40.0 * look.y + 60.0 * look.z) / weight;
	float distant = mix(1.0, 0.3, smoothstep(RELIEF_NEAR_DIST, RELIEF_FAR_DIST, distMetres));
	return strength * distant * pow(max(dot(N, normalize(V + L)), 0.0), shine) * smoothstep(0.0, 0.1, NdotL);
}

// powder with no air brightens with the sun behind the viewer; a multiplier for the light
float materialBackscatter(vec4 look, vec3 V, vec3 L)
{
#ifdef ATMOSPHERE
	return 1.0;
#else
	return 1.0 + 0.15 * look.w * pow(max(dot(V, L), 0.0), 8.0);
#endif
}

// ice is slightly blue where it lies in shadow; light: the light it gets
vec3 materialShadowTint(vec4 look, vec3 light)
{
	float shadow = 1.0 - clamp(dot(light, vec3(0.3, 0.59, 0.11)), 0.0, 1.0);
	return mix(vec3(1.0), vec3(0.85, 0.93, 1.1), look.x * shadow);
}

// lava's glow stops over water and just short of it
float materialGlowOverWater(float waterMask, float altitude)
{
	return (1.0 - waterMask) * smoothstep(MATERIAL_GLOW_SHORE_LOW, MATERIAL_GLOW_SHORE_HIGH, altitude);
}

// ---------------------------------------------------------------- bump lighting

// The normal with the material textures' bumps (bump from
// terrainMaterialDetail()), from the screen-space derivatives of their
// height (surface gradient bump mapping): nearly free, the heights are
// already sampled. Call it outside branches. normal: view space; dpdx,
// dpdy: screen derivatives of the view-space position.
vec3 terrainBumpNormal(vec3 normal, vec3 dpdx, vec3 dpdy, vec2 bump)
{
	// derivatives of the height alone: the tile size changes with distance
	vec2 dh = vec2(dFdx(bump.x), dFdy(bump.x)) * bump.y;
	vec3 r1 = cross(dpdy, normal);
	vec3 r2 = cross(normal, dpdx);
	float det = dot(dpdx, r1);
	if (bump.y <= 0.0 || abs(det) < 1e-20)
		return normal;
	vec3 grad = sign(det) * (dh.x * r1 + dh.y * r2);
	return normalize(abs(det) * normal - grad);
}
