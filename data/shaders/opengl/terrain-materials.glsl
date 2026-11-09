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
//   materials of a pixel blended along their bumps so borders look natural,
//   each with its own roughness map;
// - materials the vertex colour cannot show: bare ground on steep slopes, snow
//   sliding off them, beaches along the shore. These shift the colour too;
// - the glow of lava, from the lava texture's emission;
// - how the materials take the light: shine from the roughness maps, blue
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
// the same textures' height alone, uncompressed, from their 1024-pixel size down (TerrainStones in the C++), which the
// stones read (P4d), so the main processor can read exactly the same
uniform sampler2DArray terrainStoneHeights;
#define STONE_HEIGHTS_SIZE 1024.0
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
// (Every loop over the slots runs to MATERIAL_SLOTS, a number fixed when the shader is compiled, and stops at the used
// ones; and no slot is read by a number only known while running. Then the compiler can unroll the loops and keep the
// weights in registers. A loop up to MATERIAL_USED_SLOTS, or w[rockSlot], makes it keep the weight arrays in memory
// instead, which is slow: the photo textures cost 2.6 ms of New Hope's 4.9 on 2026-11-09, but their texture reads
// little of that.)

// Each slot's material, as the functions below give it, worked out once for the body on the main processor
// (GeoSphere::FillMaterialSlotData, which must change with them), so the ground reads them instead of working them out
// at every pixel (the colour shift alone cost 0.74 ms on New Hope on 2026-11-09). Colour: rgb materialColour(), a
// materialMeanRoughness(); traits: x materialRoughness(), y materialBumpStrength(). An unused slot: 0.7 and 1.0, as
// the functions' callers took for no material.
// cellHashes: materialCellHash() of the 5 x 5 cells (x across, y down) of salts 0 and 1, four to a vector, for the
// turns' field (materialRegionNoiseTable) to look up instead of working out
layout(std140) uniform MaterialSlotData {
	vec4 materialSlotColour[MATERIAL_SLOTS];
	vec4 materialSlotTraits[MATERIAL_SLOTS];
	vec4 materialCellHashes[13];
};

// Cheaper photo textures, each switched in the debug window (GeoSphere: TerrainTextureSimpleFar, TerrainTextureOneScale,
// TerrainTurnTable). TextureLod x: 1 to read them simply far off, y: from how far (m), full by half as far again;
// z: 1 for one texture size at a time, w: the share of the way between two sizes over which they blend (1: all of it,
// as before). TextureTurnTable: 1 to look the turns' cell hashes up in the table instead of working them out.
uniform vec4 TextureLod;
uniform float TextureTurnTable;

// (as scripts/make_terrain_materials.py makes them)
#define MATERIAL_TEXTURE_SIZE 4096.0
// on-screen size of a texel in pixels: grain, and mottling from the same
// texture 8x larger. Below 1 the texture is finer than the screen, and the
// mipmaps show it as sharp as the pixels allow. 0.625 keeps the tiles the
// size the 1K textures had at 2.5, so the grain looks the same size, only
// sharper.
#define MATERIAL_FINE_PIXELS_PER_TEXEL 0.625
#define MATERIAL_COARSE_LEVELS 3.0
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

// the ice photo's grain and bumps at least this strong, with no crust activity at all
#define MATERIAL_ICE_LEAST 0.4
// Old ice, with little crust activity (crack strength below MATERIAL_OLD_ICE_UNTIL), takes up to this share of the snow
// photo's grain and bumps, which have no cracks: frosty, granular ground, not a flat sheet with faint cracks
#define MATERIAL_OLD_ICE_UNTIL 0.5
#define MATERIAL_OLD_ICE_SNOW 0.6

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

// shine: the share of light a material reflects as a mirror would, head on (Schlick's F0): ice, metal, and all the
// rest; and the least roughness used, for the sharpest highlight allowed, so small bumps don't sparkle and flicker
#define MATERIAL_F0_ICE 0.018
#define MATERIAL_F0_METAL 0.5
#define MATERIAL_F0_GROUND 0.04
#define MATERIAL_ROUGHNESS_MIN 0.25
// how much rougher the ground looks from afar, where its small bumps have faded out (added at RELIEF_FAR_DIST)
#define MATERIAL_FAR_ROUGHENING 0.15
// roughness of city paving
#define MATERIAL_PAVING_ROUGHNESS 0.6

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
// and hollows are darker. On the physical generator and height maps (MaterialLook.z), and the less the more air lights them from all round.
float terrainOcclusion(vec3 normal, vec3 up, vec4 tile)
{
	if (MaterialLook.z < 0.5)
		return 1.0;
	float level = clamp(dot(normal, up), 0.0, 1.0);
	float slopes = level * level * level;
	float hollows = mix(0.74, 1.08, smoothstep(0.25, 0.6, tile.w));
	return mix(1.0, slopes * hollows, 1.0 - 0.7 * airShare());
}

// Each texture's mean roughness (its blue channel; printed by scripts/make_terrain_materials.py), for where the
// texture isn't read. Forest is its canopy, as it is seen from afar.
float materialMeanRoughness(int m)
{
	if (m == MATERIAL_ROCK) return 0.693;
	if (m == MATERIAL_GRAVEL) return 0.541;
	if (m == MATERIAL_SAND) return 0.652;
	if (m == MATERIAL_SOIL) return 0.675;
	if (m == MATERIAL_GRASS) return 0.263;
	if (m == MATERIAL_FOREST || m == MATERIAL_FOREST_FAR) return 0.612;
	if (m == MATERIAL_SNOW) return 0.707;
	// (old ice takes in the snow photo, as in materialSample())
	if (m == MATERIAL_ICE) return mix(0.140, 0.707, MaterialLook.y > 0.5 ? MATERIAL_OLD_ICE_SNOW * (1.0 - clamp(MaterialLook.x / MATERIAL_OLD_ICE_UNTIL, 0.0, 1.0)) : 0.0);
	if (m == MATERIAL_BASALT) return 0.689;
	if (m == MATERIAL_LAVA) return 0.418;
	if (m == MATERIAL_ASH || m == MATERIAL_DUST) return 0.967;
	if (m == MATERIAL_REGOLITH) return 0.593;
	if (m == MATERIAL_SALT) return 0.598;
	if (m == MATERIAL_TUNDRA) return 0.609;
	if (m == MATERIAL_DIRTY_ICE) return 0.937;
	if (m == MATERIAL_LAVA_FLOW) return 0.414;
	if (m == MATERIAL_CRACKED_MUD) return 0.756;
	if (m == MATERIAL_METAL) return 0.456;
	return 0.7;
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

// roughness from the vertex's materials alone, at their means
float materialVertexRoughness(vec4 slots[MATERIAL_SLOT_GROUPS])
{
	float r = 0.0, total = 0.0;
	int usedSlots = MATERIAL_USED_SLOTS;
	for (int i = 0; i < MATERIAL_SLOTS; ++i) {
		if (i >= usedSlots)
			break;
		int layer = materialSlotLayer(i);
		if (layer < 0)
			continue;
		float w = materialSlotWeight(slots, i);
		r += w * materialSlotColour[i].a;
		total += w;
	}
	return total > 1e-3 ? r / total : 0.7;
}

// slot weights with this pixel's slope and altitude: steep ground becomes
// the body's bare ground, snow slides off it, the shore becomes beach
void terrainSlotWeights(vec4 slots[MATERIAL_SLOT_GROUPS], float slope, float altitude, bool hasWater, out float w[MATERIAL_SLOTS])
{
	for (int i = 0; i < MATERIAL_SLOTS; ++i)
		w[i] = materialSlotWeight(slots, i);
	int rockSlot = int(MaterialSpecial.x), beachSlot = int(MaterialSpecial.y);
	int snowSlot = int(MaterialSpecial.z), lavaSlot = int(MaterialSpecial.w);

	int usedSlots = MATERIAL_USED_SLOTS;

	float steep = smoothstep(MATERIAL_ROCK_SLOPE_START, MATERIAL_ROCK_SLOPE_FULL, slope);
	if (rockSlot >= 0 && steep > 0.0) {
		float moved = 0.0;
		for (int i = 0; i < MATERIAL_SLOTS; ++i) {
			if (i >= usedSlots)
				break;
			if (i == rockSlot || i == lavaSlot)
				continue;
			float k = (i == snowSlot) ? 0.7 * steep : steep;
			moved += k * w[i];
			w[i] *= 1.0 - k;
		}
		for (int i = 0; i < MATERIAL_SLOTS; ++i) {
			if (i == rockSlot)
				w[i] += moved;
		}
	}

	if (hasWater && beachSlot >= 0) {
		float beach = smoothstep(MATERIAL_BEACH_TOP, MATERIAL_BEACH_FULL, altitude) * (1.0 - steep);
		if (beach > 0.0) {
			float moved = 0.0;
			for (int i = 0; i < MATERIAL_SLOTS; ++i) {
				if (i >= usedSlots)
					break;
				int layer = materialSlotLayer(i);
				// (lava's crust and ash stay black down to the water)
				if (i == beachSlot || i == rockSlot || i == snowSlot || i == lavaSlot || layer == MATERIAL_ASH || layer == MATERIAL_LAVA_FLOW)
					continue;
				moved += beach * w[i];
				w[i] *= 1.0 - beach;
			}
			for (int i = 0; i < MATERIAL_SLOTS; ++i) {
				if (i == beachSlot)
					w[i] += moved;
			}
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
	int usedSlots = MATERIAL_USED_SLOTS;
	for (int i = 0; i < MATERIAL_SLOTS; ++i) {
		if (i >= usedSlots)
			break;
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

// the cube face under a point, and the next one near the face edges; upPlanet: straight up, planet-fixed
void materialPlanes(inout MaterialMapping m, vec3 upPlanet)
{
	vec3 a = abs(upPlanet);
	int first = (a.x > a.y && a.x > a.z) ? 0 : (a.y > a.z ? 1 : 2);
	vec3 rest = a;
	rest[first] = -1.0;
	int second = (rest.x > rest.y && rest.x > rest.z) ? 0 : (rest.y > rest.z ? 1 : 2);
	float edge = smoothstep(0.85, 1.0, a[second] / a[first]);
	m.axes = ivec2(first, second);
	m.plane = vec2(1.0 - 0.5 * edge, 0.5 * edge);
}

MaterialMapping materialMapping(vec3 eyepos, vec3 dpdx, vec3 dpdy, vec3 up)
{
	mat3 toPlanet = transpose(mat3(cloudAxisX.xyz, cloudAxisY.xyz, cloudAxisZ.xyz));
	MaterialMapping m;
	m.rel = toPlanet * eyepos;
	m.relDx = toPlanet * dpdx;
	m.relDy = toPlanet * dpdy;

	materialPlanes(m, toPlanet * up);

	// tile size from the pixel footprint (geometric mean, so grazing views do
	// not blow the tiles up)
	float footprint = sqrt(length(m.relDx) * length(m.relDy));
	m.level = log2(max(footprint * MATERIAL_TEXTURE_SIZE * MATERIAL_FINE_PIXELS_PER_TEXEL, 1.0));
	// P4b: near the camera, one size fixed on the ground, so what the textures show (gravel, stones, cracks) keeps its
	// size as the camera moves; beyond, back to the size by distance, never smaller than the near one
	if (MaterialNear.z > 0.5) {
		float far = smoothstep(MaterialNear.y, 2.0 * MaterialNear.y, length(m.rel));
		m.level = mix(MaterialNear.x, max(MaterialNear.x, m.level), far);
	}
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

// A cell's hash, 0..1, in whole numbers: the main processor gets exactly the same (TerrainStones.cpp), which a sin() hash
// doesn't give, the graphics card working sin() out less exactly (P4d of the OpenGL 4.6 plan, 2026-11-09)
float materialCellHash(vec2 c, uint salt)
{
	uvec3 v = uvec3(uint(int(c.x)), uint(int(c.y)), salt);
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	return float(v.x >> 8u) * (1.0 / 16777216.0);
}

// smooth noise 0..1 repeating every 5 cells, so it goes on across the origin's wrap
float materialRegionNoise(vec2 p, uint salt)
{
	vec2 c = floor(p), t = p - c;
	t = t * t * (3.0 - 2.0 * t);
	vec2 next = mod(c + 1.0, 5.0);
	c = mod(c, 5.0);
	vec4 h = vec4(materialCellHash(c, salt), materialCellHash(vec2(next.x, c.y), salt),
		materialCellHash(vec2(c.x, next.y), salt), materialCellHash(next, salt));
	return mix(mix(h.x, h.y, t.x), mix(h.z, h.w, t.x), t.y);
}

// as materialRegionNoise(), the cells' hashes looked up (the same values: GeoSphere::FillMaterialSlotData works them
// out with TerrainStones::CellHash); c is already in 0..4
float materialCellHashTable(vec2 c, uint salt)
{
	int i = int(salt) * 25 + int(c.y) * 5 + int(c.x);
	return materialCellHashes[i >> 2][i & 3];
}

float materialRegionNoiseTable(vec2 p, uint salt)
{
	vec2 c = floor(p), t = p - c;
	t = t * t * (3.0 - 2.0 * t);
	vec2 next = mod(c + 1.0, 5.0);
	c = mod(c, 5.0);
	vec4 h = vec4(materialCellHashTable(c, salt), materialCellHashTable(vec2(next.x, c.y), salt),
		materialCellHashTable(vec2(c.x, next.y), salt), materialCellHashTable(next, salt));
	return mix(mix(h.x, h.y, t.x), mix(h.z, h.w, t.x), t.y);
}

// One read of the photo textures, as textureGrad(); for the ground cost breakdown (GROUND_SKIP 12), without the
// sharper filtering at grazing angles: one level of detail for the larger of the two footprints
vec4 materialRead(vec3 uvLayer, vec2 dx, vec2 dy)
{
	if (GROUND_SKIP(12))
		return textureLod(terrainMaterials, uvLayer, log2(max(max(length(dx), length(dy)) * MATERIAL_TEXTURE_SIZE, 1e-6)));
	return textureGrad(terrainMaterials, uvLayer, dx, dy);
}

// The turns of the tile. These are the ones that take a step of 5 tiles to a whole number of them (the 3, 4, 5 triangle),
// so turning doesn't show where the origin wraps.
const vec2 MATERIAL_TURNS[8] = vec2[8](vec2(1.0, 0.0), vec2(0.6, 0.8), vec2(0.0, 1.0), vec2(-0.8, 0.6), vec2(-1.0, 0.0), vec2(-0.6, -0.8), vec2(0.0, -1.0), vec2(0.8, -0.6));

// each texture's turns, patch size and blend
#include "terrain-material-rotation.glsl"

// Reads the tile turned and shifted one way or the next, by a field fixed in space, blended along their bumps: no two
// places alike, and no grid. uvPatch, uvFar: this place in cells of a patch, and 32 times that, for the field's long
// repeats. turnStep: 1 for all eight turns, 2 for right angles only. blend: how far they blend into each other.
// Emission (x), detail (y), height (z), roughness (w), as materialTileSample().
vec4 materialVariedRead(int layer, vec2 uv, vec2 dx, vec2 dy, vec2 uvPatch, vec2 uvFar, int turnStep, float blend)
{
	float count = float(8 / turnStep);
	float field = TextureTurnTable > 0.5 ?
		count * fract(materialRegionNoiseTable(uvPatch, 0u) + 3.0 * materialRegionNoiseTable(uvFar, 1u)) :
		count * fract(materialRegionNoise(uvPatch, 0u) + 3.0 * materialRegionNoise(uvFar, 1u));
	float index = floor(field);
	vec2 a = MATERIAL_TURNS[int(index) * turnStep], b = MATERIAL_TURNS[int(mod(index + 1.0, count)) * turnStep];
	vec2 offsetA = fract(vec2(0.7548777, 0.5698403) * index), offsetB = fract(vec2(0.7548777, 0.5698403) * (index + 1.0));
	vec4 ra = materialRead(vec3(vec2(a.x * uv.x - a.y * uv.y, a.y * uv.x + a.x * uv.y) + offsetA, float(layer)),
		vec2(a.x * dx.x - a.y * dx.y, a.y * dx.x + a.x * dx.y), vec2(a.x * dy.x - a.y * dy.y, a.y * dy.x + a.x * dy.y)).rgab;
	vec4 rb = materialRead(vec3(vec2(b.x * uv.x - b.y * uv.y, b.y * uv.x + b.x * uv.y) + offsetB, float(layer)),
		vec2(b.x * dx.x - b.y * dx.y, b.y * dx.x + b.x * dx.y), vec2(b.x * dy.x - b.y * dy.y, b.y * dy.x + b.x * dy.y)).rgab;
	return mix(ra, rb, smoothstep(0.5 - blend, 0.5 + blend, field - index + 0.8 * (rb.z - ra.z)));
}

// one tile size of 2^level metres. Emission (x), detail (y), height (z), roughness (w: 0 a mirror, 1 fully matte).
vec4 materialTileSample(int layer, MaterialMapping m, float level)
{
	float tile = exp2(level);
	vec3 q = materialTileCoord(m, level);
	vec3 qx = m.relDx / tile, qy = m.relDy / tile;
	// (detail fixed in space: as set for this texture; everywhere else straight)
	vec3 rotation = MaterialLook.y > 0.5 ? materialRotation(layer) : vec3(0.0);
	if (MaterialLook.y > 0.5 && rotation.x > 0.5 && !GROUND_SKIP(9)) {
		// a patch of 2^y tiles, in the coordinates of that size, so it goes on across their wrap; half a tile is twice
		// these, which still does
		vec3 qPatch = rotation.y < -0.5 ? q * 2.0 : (rotation.y < 0.5 ? q : materialTileCoord(m, min(level + rotation.y, MATERIAL_MAX_LEVEL)));
		vec3 qFar = materialTileCoord(m, min(level + max(rotation.y, 0.0) + 5.0, MATERIAL_MAX_LEVEL));
		int turnStep = rotation.x < 1.5 ? 2 : 1;
		vec4 varied = m.plane.x * materialVariedRead(layer, materialPlaneUV(q, m.axes.x), materialPlaneUV(qx, m.axes.x), materialPlaneUV(qy, m.axes.x), materialPlaneUV(qPatch, m.axes.x), materialPlaneUV(qFar, m.axes.x), turnStep, rotation.z);
		if (m.plane.y > 0.0)
			varied += m.plane.y * materialVariedRead(layer, materialPlaneUV(q, m.axes.y), materialPlaneUV(qx, m.axes.y), materialPlaneUV(qy, m.axes.y), materialPlaneUV(qPatch, m.axes.y), materialPlaneUV(qFar, m.axes.y), turnStep, rotation.z);
		return varied;
	}
	vec4 r = m.plane.x * materialRead(vec3(materialPlaneUV(q, m.axes.x), float(layer)),
		materialPlaneUV(qx, m.axes.x), materialPlaneUV(qy, m.axes.x)).rgab;
	if (m.plane.y > 0.0)
		r += m.plane.y * materialRead(vec3(materialPlaneUV(q, m.axes.y), float(layer)),
			materialPlaneUV(qx, m.axes.y), materialPlaneUV(qy, m.axes.y)).rgab;
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

// emission (x), detail (y, mean 1), height (z) and roughness (w) of one material at a tile
// level, blended between the two power-of-two sizes around it
// one texture between the two power-of-two sizes around its level k + f (0 <= f < 1), as far between them as f; with
// one size at a time (TextureLod.z), they blend only over the middle share TextureLod.w of the way, and only the one is
// read either side of it
vec4 materialTwoSizes(int layer, MaterialMapping m, float k, float f)
{
	if (TextureLod.z > 0.5)
		f = smoothstep(0.5 - 0.5 * TextureLod.w, 0.5 + 0.5 * TextureLod.w, f);
	if (f <= 0.0)
		return materialTileSample(layer, m, k);
	if (f >= 1.0)
		return materialTileSample(layer, m, k + 1.0);
	return mix(materialTileSample(layer, m, k), materialTileSample(layer, m, k + 1.0), f);
}

vec4 materialSample(int layer, MaterialMapping m, float level)
{
	level = clamp(level, 0.0, MATERIAL_MAX_LEVEL - 1.0);
	float k = floor(level);
	// (level is often a whole number, as MATERIAL_FIXED_MAX_LEVEL holds it at one over most of the screen: then the next
	// size would count for nothing, so it isn't read)
	vec4 r = materialTwoSizes(layer, m, k, level - k);
	// forest: its floor on foot, its canopy from the air
	if (layer == MATERIAL_FOREST) {
		float canopy = smoothstep(MATERIAL_FOREST_NEAR_LEVEL, MATERIAL_FOREST_FAR_LEVEL, m.level);
		if (canopy > 0.0)
			r = mix(r, materialTwoSizes(MATERIAL_FOREST_FAR, m, k, level - k), canopy);
	}
	// (on old ice, the snow photo at the same sizes, read only there)
	float oldIce = layer == MATERIAL_ICE && MaterialLook.y > 0.5 ? MATERIAL_OLD_ICE_SNOW * (1.0 - clamp(MaterialLook.x / MATERIAL_OLD_ICE_UNTIL, 0.0, 1.0)) : 0.0;
	vec4 snow = vec4(0.0, 0.5, 0.5, materialMeanRoughness(MATERIAL_SNOW));
	if (oldIce > 0.0)
		snow = materialTwoSizes(MATERIAL_SNOW, m, k, level - k);
	r.y *= 2.0;
	// the ice photo has its cracks built in: as strong as this body's are, but never gone, or an old ice moon's ground
	// is flat and plain up close
	if (layer == MATERIAL_ICE) {
		float strength = max(MaterialLook.x, MATERIAL_ICE_LEAST);
		r.y = 1.0 + (r.y - 1.0) * strength;
		r.z = 0.5 + (r.z - 0.5) * strength;
		r = mix(r, vec4(snow.x, 2.0 * snow.y, snow.z, snow.w), oldIce);
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
// for terrainBumpNormal(); roughness: for materialGloss(), from the textures
// near the camera, the materials' means further out
vec4 terrainMaterialDetail(vec4 slots[MATERIAL_SLOT_GROUPS], vec3 albedo, MaterialMapping mapping,
	float distMetres, vec3 normal, vec3 up, float altitude, bool hasWater, float landform, out vec2 bump,
	out float roughness)
{
	bump = vec2(0.5, 0.0);
	roughness = materialVertexRoughness(slots);
	int lavaSlot = int(MaterialSpecial.w);
	float lavaWeight = 0.0;
	for (int i = 0; i < MATERIAL_SLOTS; ++i) {
		if (i == lavaSlot)
			lavaWeight = materialSlotWeight(slots, i);
	}
	float farGlow = lavaWeight * MATERIAL_LAVA_MEAN_EMISSION;

	float contrast = 1.0 - smoothstep(MATERIAL_NEAR_DIST, MATERIAL_FAR_DIST, distMetres);
	if (contrast <= 0.0)
		return vec4(1.0, 1.0, 1.0, farGlow);
	float slope = 1.0 - dot(normalize(normal), up);

	// weights as the vertex has them (smooth across the triangle), and with
	// this pixel's slope and altitude
	float w0[MATERIAL_SLOTS];
	float w[MATERIAL_SLOTS];
	if (GROUND_SKIP(13)) {
		// (the breakdown: the vertex's weights as they are, without the slope and shore)
		for (int i = 0; i < MATERIAL_SLOTS; ++i) {
			w0[i] = materialSlotWeight(slots, i);
			w[i] = w0[i];
		}
	} else {
		// (as the vertex has them: terrainSlotWeights() with no slope and at the beaches' top changes nothing, so a copy)
		for (int i = 0; i < MATERIAL_SLOTS; ++i)
			w0[i] = materialSlotWeight(slots, i);
		terrainSlotWeights(slots, slope, altitude, hasWater, w);
	}
	int usedSlots = MATERIAL_USED_SLOTS;
	int rockSlot = int(MaterialSpecial.x);
	float crest = 0.6 * smoothstep(0.7, 0.9, landform);
	if (rockSlot >= 0 && crest > 0.0) {
		float moved = 0.0;
		for (int i = 0; i < MATERIAL_SLOTS; ++i) {
			if (i >= usedSlots)
				break;
			if (i == rockSlot || i == int(MaterialSpecial.w))
				continue;
			moved += crest * w[i];
			w[i] *= 1.0 - crest;
		}
		for (int i = 0; i < MATERIAL_SLOTS; ++i) {
			if (i == rockSlot)
				w[i] += moved;
		}
	}

	// the two strongest slots (their weights kept as they're found, not read by their number after)
	int m1 = 0, m2 = 1;
	float v1 = w[0], v2 = w[1];
	for (int i = 1; i < MATERIAL_SLOTS; ++i) {
		if (i >= usedSlots)
			break;
		if (w[i] > v1) {
			m2 = m1;
			v2 = v1;
			m1 = i;
			v1 = w[i];
		} else if (i != m1 && w[i] > v2) {
			m2 = i;
			v2 = w[i];
		}
	}
	float landWeight = 0.0;
	for (int i = 0; i < MATERIAL_SLOTS; ++i) {
		if (i >= usedSlots)
			break;
		landWeight += w[i];
	}
	if (landWeight < 1e-3)
		return vec4(1.0, 1.0, 1.0, farGlow);

	int l1 = materialSlotLayer(m1), l2 = materialSlotLayer(m2);
	float w1 = v1, w2 = l2 >= 0 ? v2 : 0.0;
	vec4 d1 = vec4(0.0, 1.0, 0.5, materialSlotColour[m1].a), d2 = vec4(0.0, 1.0, 0.5, materialSlotColour[m2].a);
	float coarse = mapping.level + MATERIAL_COARSE_LEVELS;
	// The second material only where it shows: over a tenth of the land here (below that it hardly ever comes through
	// the blend along the bumps), and with the first one's coarse size. Its two reads cost 1.2 ms at Cydonia on
	// 2026-11-09; this saved 0.48 ms and you saw no difference.
	// Far off, simply (TextureLod.xy): the first material's fine size alone, at the one size nearest, for both
	// materials (their colours still each their own); fading in from TextureLod.y to half as far again
	float simple = TextureLod.x > 0.5 ? smoothstep(TextureLod.y, 1.5 * TextureLod.y, distMetres) : 0.0;
	vec4 c1 = vec4(1.0);
	if (simple < 1.0) {
		if (l1 >= 0) {
			vec4 f1 = GROUND_SKIP(15) ? vec4(0.0, 0.5, 0.5, 0.7) : materialSample(l1, mapping, mapping.level);
			c1 = GROUND_SKIP(10) ? vec4(1.0) : materialSample(l1, mapping, coarse);
			d1 = vec4(f1.x, f1.y * c1.y, f1.z, f1.w);
		}
		if (w2 > 0.1 && !GROUND_SKIP(11)) {
			vec4 f2 = materialSample(l2, mapping, mapping.level);
			d2 = vec4(f2.x, f2.y * c1.y, f2.z, f2.w);
		}
	}
	if (simple > 0.0 && l1 >= 0) {
		vec4 s1 = materialSample(l1, mapping, floor(mapping.level + 0.5));
		d1 = mix(d1, s1, simple);
		d2 = mix(d2, s1, simple);
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
	// (and the roughness of the land not in the two strongest, at their means)
	float restRoughness = 0.0;
	for (int i = 0; i < MATERIAL_SLOTS; ++i) {
		if (i >= usedSlots || GROUND_SKIP(14))
			break;
		int layer = materialSlotLayer(i);
		float wi = (i == m1) ? n1 : (i == m2) ? n2 : w[i];
		// (a slot with no weight here, before or after, adds nothing)
		if (layer < 0 || (wi == 0.0 && w0[i] == 0.0))
			continue;
		if (i != m1 && i != m2)
			restRoughness += w[i] * materialSlotColour[i].a;
		// (with detail fixed in space, the vertex colours are the materials' own; these
		// generic colours would turn floors tan and the walls of dark moons white)
		if (MaterialLook.y < 0.5)
			shifted += contrast * (wi - w0[i]) * materialSlotColour[i].rgb;
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
	float bumps = (n1 * materialSlotTraits[m1].y + n2 * materialSlotTraits[m2].y + (landWeight - total)) / landWeight;
	bump.y = contrast * MATERIAL_BUMP_HEIGHT * exp2(mapping.level) * bumps;
	detail = 1.0 + (detail - 1.0) * contrast;

	// roughness: the blended materials' textures, fading to the vertex's means as the textures do
	roughness = mix(roughness, (n1 * d1.w + n2 * d2.w + restRoughness) / landWeight, contrast);

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

// ---------------------------------------------------------------- stones

// P4c of docs/implementation_plans/opengl46-tessellation-compute_plan.md: the ground near the camera raised by the
// textures' own height, so stones and lumps stand up out of it.

// how high each material's texture raises the ground: its height channel's full range (0..1), as a share of the tile
// it covers. Stones and boulders the most, fine-grained ground hardly at all
float materialStoneDepth(int m)
{
	if (m == MATERIAL_ROCK) return 0.10;
	if (m == MATERIAL_BASALT || m == MATERIAL_LAVA_FLOW) return 0.08;
	if (m == MATERIAL_GRAVEL) return 0.05;
	if (m == MATERIAL_DIRTY_ICE || m == MATERIAL_LAVA) return 0.04;
	if (m == MATERIAL_REGOLITH) return 0.03;
	if (m == MATERIAL_SOIL || m == MATERIAL_CRACKED_MUD || m == MATERIAL_TUNDRA || m == MATERIAL_FOREST) return 0.025;
	if (m == MATERIAL_ICE || m == MATERIAL_SNOW || m == MATERIAL_ASH || m == MATERIAL_GRASS) return 0.015;
	if (m == MATERIAL_SAND || m == MATERIAL_DUST || m == MATERIAL_SALT) return 0.008;
	return 0.0; // metal
}

// steps toward the sun when looking for stones that shade a point
#define STONE_SHADOW_STEPS 8

// how finely the ground is cut for the stones at a distance
#include "terrain-stone-aim.glsl"

// how far beyond a circle kept free of stones they are back in full (metres)
#define STONE_FLAT_MARGIN 10.0

float terrainStoneFlatOne(vec3 p, vec4 circle)
{
	return circle.w <= 0.0 ? 1.0 : smoothstep(circle.w, circle.w + STONE_FLAT_MARGIN, length(p - circle.xyz));
}

// 0 inside the circles kept free of stones (cities, landed ships), 1 well clear of them; p: view space
float terrainStoneFlat(vec3 p)
{
	return terrainStoneFlatOne(p, StoneFlat0) * terrainStoneFlatOne(p, StoneFlat1) * terrainStoneFlatOne(p, StoneFlat2) * terrainStoneFlatOne(p, StoneFlat3) * terrainStoneFlatOne(p, StoneFlat4) * terrainStoneFlatOne(p, StoneFlat5);
}

// The stone heights (terrainStoneHeights) at tile coordinates uv, smoothed to lod (in the heights' own pixels). Read at a
// lod given, not from the screen, so the main processor can work out the same (TerrainStones::Sample).
float stoneRead(int layer, vec2 uv, float lod)
{
	return textureLod(terrainStoneHeights, vec3(uv, float(layer)), lod).r;
}

// as materialVariedRead(), the heights alone
float stoneVariedRead(int layer, vec2 uv, vec2 uvPatch, vec2 uvFar, int turnStep, float blend, float lod)
{
	float count = float(8 / turnStep);
	float field = count * fract(materialRegionNoise(uvPatch, 0u) + 3.0 * materialRegionNoise(uvFar, 1u));
	float index = floor(field);
	vec2 a = MATERIAL_TURNS[int(index) * turnStep], b = MATERIAL_TURNS[int(mod(index + 1.0, count)) * turnStep];
	vec2 offsetA = fract(vec2(0.7548777, 0.5698403) * index), offsetB = fract(vec2(0.7548777, 0.5698403) * (index + 1.0));
	float ra = stoneRead(layer, vec2(a.x * uv.x - a.y * uv.y, a.y * uv.x + a.x * uv.y) + offsetA, lod);
	float rb = stoneRead(layer, vec2(b.x * uv.x - b.y * uv.y, b.y * uv.x + b.x * uv.y) + offsetB, lod);
	return mix(ra, rb, smoothstep(0.5 - blend, 0.5 + blend, field - index + 0.8 * (rb - ra)));
}

// as materialTileSample(), the heights alone, smoothed to spacing (metres)
float stoneTileHeight(int layer, MaterialMapping m, float level, float spacing)
{
	float tile = exp2(level);
	vec3 q = materialTileCoord(m, level);
	float lod = max(log2(max(spacing / tile * STONE_HEIGHTS_SIZE, 1e-6)), 0.0);
	vec3 rotation = materialRotation(layer);
	if (MaterialLook.y > 0.5 && rotation.x > 0.5) {
		vec3 qPatch = rotation.y < -0.5 ? q * 2.0 : (rotation.y < 0.5 ? q : materialTileCoord(m, min(level + rotation.y, MATERIAL_MAX_LEVEL)));
		vec3 qFar = materialTileCoord(m, min(level + max(rotation.y, 0.0) + 5.0, MATERIAL_MAX_LEVEL));
		int turnStep = rotation.x < 1.5 ? 2 : 1;
		float h = m.plane.x * stoneVariedRead(layer, materialPlaneUV(q, m.axes.x), materialPlaneUV(qPatch, m.axes.x), materialPlaneUV(qFar, m.axes.x), turnStep, rotation.z, lod);
		if (m.plane.y > 0.0)
			h += m.plane.y * stoneVariedRead(layer, materialPlaneUV(q, m.axes.y), materialPlaneUV(qPatch, m.axes.y), materialPlaneUV(qFar, m.axes.y), turnStep, rotation.z, lod);
		return h;
	}
	float h = m.plane.x * stoneRead(layer, materialPlaneUV(q, m.axes.x), lod);
	if (m.plane.y > 0.0)
		h += m.plane.y * stoneRead(layer, materialPlaneUV(q, m.axes.y), lod);
	return h;
}

// as materialSample(), the heights alone: blended between the two power-of-two sizes around the level, and the ice's
// as strong as its cracks
float stoneSample(int layer, MaterialMapping m, float level, float spacing)
{
	level = clamp(level, 0.0, MATERIAL_MAX_LEVEL - 1.0);
	float k = floor(level);
	float h = stoneTileHeight(layer, m, k, spacing);
	if (level > k)
		h = mix(h, stoneTileHeight(layer, m, k + 1.0, spacing), level - k);
	if (layer == MATERIAL_ICE)
		h = 0.5 + (h - 0.5) * max(MaterialLook.x, MATERIAL_ICE_LEAST);
	return h;
}

// The two materials whose stones stand at a point, and how much of each (as terrainMaterialDetail() picks them), kept
// for reading the height there and around it
struct StonePick {
	int l1, l2;		 // their texture layers (l2 -1: none)
	float w1, w2;	 // their weights
	float k1, k2;	 // their stone heights, times the land's share and metres: height = k1 * h1 + k2 * h2, after the blend
	float total;	 // w1 + w2
};

// the pick at a point; false where nothing is raised. eyepos: the point on the flat ground, view space; normal (of the
// mesh), up: view space, unit length; scale: metres the full height of a texture stands at here, fading out with the
// one size, over the shore and on water
bool terrainStonePick(vec4 slots[MATERIAL_SLOT_GROUPS], vec3 eyepos, vec3 normal, vec3 up, float altitude, bool hasWater, out StonePick p)
{
	p.l1 = -1;
	p.l2 = -1;
	p.w1 = p.w2 = p.k1 = p.k2 = p.total = 0.0;
	if (MaterialStones.x < 0.5 || MaterialNear.z < 0.5)
		return false;
	float near = 1.0 - smoothstep(MaterialNear.y, 2.0 * MaterialNear.y, length(eyepos));
	// (and where even the tallest would stand too small on screen, they fade out: nothing is cut there)
	near *= smoothstep(0.5 * STONE_FADE_PIXELS, STONE_FADE_PIXELS, terrainStonePixels(length(eyepos)));
	// (none in the cities or under landed ships)
	near *= terrainStoneFlat(eyepos);
	if (hasWater)
		near *= smoothstep(0.5, 2.0, altitude);
	if (near <= 0.0)
		return false;

	float w[MATERIAL_SLOTS];
	terrainSlotWeights(slots, 1.0 - dot(normal, up), altitude, hasWater, w);
	int usedSlots = MATERIAL_USED_SLOTS;
	int m1 = 0, m2 = 1;
	float v1 = w[0], v2 = w[1];
	for (int i = 1; i < MATERIAL_SLOTS; ++i) {
		if (i >= usedSlots)
			break;
		if (w[i] > v1) {
			m2 = m1;
			v2 = v1;
			m1 = i;
			v1 = w[i];
		} else if (i != m1 && w[i] > v2) {
			m2 = i;
			v2 = w[i];
		}
	}
	float landWeight = 0.0;
	for (int i = 0; i < MATERIAL_SLOTS; ++i) {
		if (i >= usedSlots)
			break;
		landWeight += w[i];
	}
	p.l1 = materialSlotLayer(m1);
	p.l2 = usedSlots > 1 ? materialSlotLayer(m2) : -1;
	if (landWeight < 1e-3 || p.l1 < 0)
		return false;
	p.w1 = v1;
	p.w2 = p.l2 >= 0 ? v2 : 0.0;
	p.total = p.w1 + p.w2;
	float scale = near * MaterialStones.y * exp2(MaterialNear.x) / landWeight;
	p.k1 = scale * materialStoneDepth(p.l1);
	p.k2 = p.l2 >= 0 ? scale * materialStoneDepth(p.l2) : 0.0;
	return p.k1 > 0.0 || p.k2 > 0.0;
}

// how far the ground stands up, in metres, where the mapping m is (terrainStoneMapping()): the textures' height at the
// one size held near the camera (MaterialNear.x), smoothed to spacing (metres), the two materials blended along their
// bumps as the colours are
float terrainStoneAt(StonePick p, MaterialMapping m, float spacing)
{
	float h1 = stoneSample(p.l1, m, m.level, spacing), h2 = 0.5;
	if (p.w2 > 0.01)
		h2 = stoneSample(p.l2, m, m.level, spacing);
	float s1 = p.w1 / max(p.total, 1e-6) + h1;
	float s2 = p.w2 / max(p.total, 1e-6) + h2;
	float top = max(s1, s2) - MATERIAL_BLEND_DEPTH;
	float b1 = max(s1 - top, 0.0), b2 = max(s2 - top, 0.0);
	float n1 = p.total * b1 / (b1 + b2), n2 = p.total * b2 / (b1 + b2);
	return n1 * h1 * p.k1 + n2 * h2 * p.k2;
}

// A point's mapping, as materialMapping() makes a pixel's (no screen derivatives: the stones are read at a lod worked
// out from the spacing). Move m.rel (planet-fixed metres) to read elsewhere.
MaterialMapping terrainStoneMapping(vec3 eyepos, vec3 up, mat3 toPlanet)
{
	MaterialMapping m;
	m.rel = toPlanet * eyepos;
	materialPlanes(m, toPlanet * up);
	m.relDx = vec3(0.0);
	m.relDy = vec3(0.0);
	m.level = MaterialNear.x;
	return m;
}

// The stones at a point of the ground near the camera (P4c, P4e of docs/implementation_plans/
// opengl46-tessellation-compute_plan.md), for the tessellation evaluation shaders, so the work is done once per cut
// point, not per pixel:
// - lift: how far the point is raised, in metres (only up, so the ground never sinks below the flat ground that casts
//   the shadow maps), smoothed to how far apart the cut points are there, so the shape doesn't shimmer;
// - slope: the raised shape's slope (view space, along the ground), for the light: the pixel shader tilts its normal by
//   it;
// - lit: how much of the main light gets past the stones around (1 all, 0 none), from a few steps toward it over the
//   same heights (MaterialStones.z on).
// eyepos: the point on the flat ground, view space; normal (of the mesh), up: view space, unit length
void terrainStones(vec4 slots[MATERIAL_SLOT_GROUPS], vec3 eyepos, vec3 normal, vec3 up, float altitude, bool hasWater,
	out float lift, out vec3 slope, out float lit)
{
	lift = 0.0;
	slope = vec3(0.0);
	lit = 1.0;
	StonePick p;
	if (!terrainStonePick(slots, eyepos, normal, up, altitude, hasWater, p))
		return;
	mat3 toPlanet = transpose(mat3(cloudAxisX.xyz, cloudAxisY.xyz, cloudAxisZ.xyz));
	// (the cut points' spacing there: the edge on screen aimed for, at this distance)
	float spacing = max(length(eyepos) * terrainStoneAim(length(eyepos)) / max(TessParams.x, 1.0), 1e-3);
	MaterialMapping m = terrainStoneMapping(eyepos, up, toPlanet);
	lift = terrainStoneAt(p, m, spacing);

	// the slope, from the heights half a spacing away along two directions on the ground
	vec3 t1 = normalize(cross(up, abs(up.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
	vec3 t2 = cross(up, t1);
	float e = 0.5 * spacing;
	MaterialMapping mx = m, my = m;
	mx.rel += toPlanet * (t1 * e);
	my.rel += toPlanet * (t2 * e);
	slope = ((terrainStoneAt(p, mx, spacing) - lift) * t1 + (terrainStoneAt(p, my, spacing) - lift) * t2) / e;

#if (NUM_LIGHTS > 0)
	// the shade: toward the main light along the ground, the line to it rising as the light stands; anything standing
	// above that line shades the point. Out as far as the highest stone here could reach the line, at most half a
	// tile; the steps closer together near the point, where the small stones are
	// (only where the stones stand a pixel or more on screen, fading in up to three: smaller, their shade can't be seen)
	float shadePixels = terrainStonePixels(length(eyepos));
	if (MaterialStones.z > 0.5 && shadePixels > 1.0) {
		vec3 L = normalize(uLight[0].position.xyz);
		float sunUp = dot(L, up);
		vec3 along = L - up * sunUp;
		float alongLen = length(along);
		if (sunUp > 0.0 && alongLen > 1e-4) {
			vec3 toward = toPlanet * (along / alongLen);
			float rise = sunUp / alongLen;
			float highest = max(p.k1, p.k2) * p.total;
			float reach = min((highest - lift) / rise, 0.5 * exp2(m.level));
			if (reach > e) {
				float soft = 0.1 * highest + 1e-3;
				float shade = 0.0;
				for (int i = 1; i <= STONE_SHADOW_STEPS; ++i) {
					float f = float(i) / float(STONE_SHADOW_STEPS);
					float s = reach * f * f;
					MaterialMapping ms = m;
					ms.rel += toward * s;
					shade = max(shade, clamp((terrainStoneAt(p, ms, spacing) - lift - s * rise) / soft, 0.0, 1.0));
				}
				lit = 1.0 - shade * smoothstep(1.0, 3.0, shadePixels);
			}
		}
	}
#endif
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
	// (old ice, with little crust activity, is about as rough as dirty ice; an active shell's stays smooth)
	if (m == MATERIAL_ICE) return mix(0.08, 0.02, clamp(MaterialLook.x / MATERIAL_OLD_ICE_UNTIL, 0.0, 1.0));
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
	int usedSlots = MATERIAL_USED_SLOTS;
	for (int i = 0; i < MATERIAL_SLOTS; ++i) {
		if (i >= usedSlots)
			break;
		float w = materialSlotWeight(slots, i);
		if (w > 0.0)
			roughness += w * materialSlotTraits[i].x;
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
		// surface below, so patches without a surface tile aren't smooth next
		// to those with one
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
	int usedSlots = MATERIAL_USED_SLOTS;
	for (int i = 0; i < MATERIAL_SLOTS; ++i) {
		if (i >= usedSlots)
			break;
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

// The ground's shine, from its roughness (terrainMaterialDetail(): 0 a mirror, 1 fully matte): a sharp, bright
// highlight on smooth ground, a broad, faint one on rough ground, the same light in all (GGX, with Smith-Schlick
// shadowing and Schlick's Fresnel, scaled as the terrain's diffuse light, which leaves out pi). Weaker from afar, so
// that a whole moon doesn't carry one highlight, and rougher, as the bumps below the pixels fade out. look:
// materialLightWeights(), for what reflects how much head on; N, V, L: view space, unit length. Times the light's
// colour, added after lighting.
float materialGloss(vec4 look, float roughness, vec3 N, vec3 V, vec3 L, float distMetres)
{
	float NdotL = dot(N, L);
	if (NdotL <= 0.0)
		return 0.0;
	float farShare = smoothstep(RELIEF_NEAR_DIST, RELIEF_FAR_DIST, distMetres);
	float r = clamp(roughness + MATERIAL_FAR_ROUGHENING * farShare, MATERIAL_ROUGHNESS_MIN, 1.0);
	float a = r * r, a2 = a * a;
	vec3 H = normalize(V + L);
	float NdotH = max(dot(N, H), 0.0), NdotV = max(dot(N, V), 1e-3);
	float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
	float k = 0.5 * a;
	float visibility = 0.25 / ((NdotL * (1.0 - k) + k) * (NdotV * (1.0 - k) + k));
	float ice = clamp(look.x, 0.0, 1.0), metal = clamp(look.z, 0.0, 1.0 - ice);
	float f0 = MATERIAL_F0_GROUND * (1.0 - ice - metal) + MATERIAL_F0_ICE * ice + MATERIAL_F0_METAL * metal;
	// (rough ground rises less toward grazing light, its bumps shadowing each other: Lagarde's form)
	float fresnel = f0 + (max(1.0 - r, f0) - f0) * pow(1.0 - clamp(dot(V, H), 0.0, 1.0), 5.0);
	float distant = mix(1.0, 0.3, farShare);
	return distant * a2 / (d * d) * visibility * fresnel * NdotL * smoothstep(0.0, 0.1, NdotL);
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

#ifdef FRAGMENT_SHADER
// The normal with the material textures' bumps (bump from
// terrainMaterialDetail()), from the screen-space derivatives of their
// height (surface gradient bump mapping): nearly free, the heights are
// already sampled. Call it outside branches. normal: view space; dpdx,
// dpdy: screen derivatives of the view-space position, on the flat ground.
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
#endif
