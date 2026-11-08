// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Bakes one terrain detail tile into its slot of the atlas (terrain-tile.glsl,
// TerrainTiles in the C++).

#include "terrain-tile.glsl"

// x, y: the slot's first pixel in the atlas; z: tile size without the border; w: border
uniform vec4 TileSlot;
// the patch's corners v0..v3 relative to its centre, planet-fixed, metres
uniform vec4 TileCorner0;
uniform vec4 TileCorner1;
uniform vec4 TileCorner2;
uniform vec4 TileCorner3;
// xyz: direction to the patch centre (planet-fixed), w: planet radius in metres
uniform vec4 TileCentre;
// x: wavelength of the finest octave (m), y: its index on the octave ladder,
// z: number of octaves of the band (the finest ones: all styles and the
// landform value), w: number of octaves in all (craters, up to TILE_MAX_OCTAVES)
uniform vec4 TileOctaves;
// xyz: patch centre in cells of octave min(4g + 3, count - 1) for g = 0..3,
// modulo 256 (in double precision); octave i takes the origin of group i / 4
uniform vec4 TileOrigin0;
uniform vec4 TileOrigin1;
uniform vec4 TileOrigin2;
uniform vec4 TileOrigin3;
// weights of the styles: rolling, ridged, dunes, craters (how many of them, at most 1)
uniform vec4 TileStyle;
// the largest wavelength the tile may hold, in metres; 0: no limit. The mesh draws everything larger,
// and the band follows the distance, so without this the ground's texture keeps its size on screen
uniform float TileCraterTop;
// metres: landforms of every size up to this, fixed in space, instead of the band's (0)
uniform float TileLandformTop;
// weight of the cracked slabs and blocks (Terrain::GetTileBlocks)
uniform float TileBlocks;

// as TerrainTiles::MAX_OCTAVES
#define TILE_MAX_OCTAVES 16
// craters stack at most this deep: inside as many larger craters' bowls,
// smaller ones fade out, so crater floors are smoother than the ground
// around them instead of holding craters within craters without end
#define CRATER_MAX_NEST 2.0

out vec4 frag_color;

// the patch centre in cells of octave i, modulo 256: from its group's
// origin, at most 3 octaves coarser (exact powers of two, mod 256 stays valid)
vec3 tileOctaveOrigin(int i, int count)
{
	int g = i / 4;
	int coarsest = min(4 * g + 3, count - 1);
	vec3 o = g == 0 ? TileOrigin0.xyz : (g == 1 ? TileOrigin1.xyz : (g == 2 ? TileOrigin2.xyz : TileOrigin3.xyz));
	return mod(o * exp2(float(coarsest - i)), 256.0);
}

void main(void)
{
	// texel position across the tile's interior, 0..1 (the border lies outside)
	vec2 uv = (gl_FragCoord.xy - TileSlot.xy - TileSlot.w) / TileSlot.z;

	// the patch point at these patch UVs (GeoPatch: u = 1 - x, v = y)
	float x = 1.0 - uv.x, y = uv.y;
	vec3 rel = TileCorner0.xyz + x * (1.0 - y) * (TileCorner1.xyz - TileCorner0.xyz) +
		x * y * (TileCorner2.xyz - TileCorner0.xyz) + (1.0 - x) * y * (TileCorner3.xyz - TileCorner0.xyz);
	vec3 up = normalize(TileCentre.xyz * TileCentre.w + rel);

	// octaves from the coarsest down (rolling erodes by the slopes above it).
	// Craters take every octave up to their largest size, the same in every
	// tile, so patches splitting or the camera moving only add or drop the
	// finest; the other styles and the landform value only the band
	vec3 grad = vec3(0.0);
	float form = 0.0, formWeight = 0.0;
	vec3 accumulated = vec3(0.0);
	// how many larger craters' bowls the point lies in
	float nest = 0.0;
	int band = int(TileOctaves.z);
	int count = int(TileOctaves.w);
	// (the physical generator's are varied, thick in some regions and thin in others; others' as they were)
	bool variedCraters = TileCraterTop > 0.0;
	float craterDensity = 0.35 * min(TileStyle.w, 1.0);
	if (variedCraters)
		craterDensity *= mix(0.05, 0.95, tileCraterRegion(up));
	for (int i = TILE_MAX_OCTAVES - 1; i >= 0; --i) {
		if (i >= count)
			continue;
		float lambda = TileOctaves.x * exp2(float(i));
		vec3 q = tileOctaveOrigin(i, count) + rel / lambda;
		float salt = TileOctaves.y + float(i);
		// the shortest octave is close to the texel size: weaker
		float w = (i == 0) ? 0.6 : 1.0;
		vec4 o = vec4(0.0);
		if (TileLandformTop > 0.0 ? lambda <= TileLandformTop * 1.01 : (i < band && (TileCraterTop <= 0.0 || lambda <= TileCraterTop * 1.01))) {
			if (TileStyle.x > 0.0)
				o += TileStyle.x * tileRolling(q, salt, accumulated);
			if (TileStyle.y > 0.0)
				o += TileStyle.y * tileRidged(q, salt);
			if (TileStyle.z > 0.0)
				o += TileStyle.z * tileDunes(q, salt);
			if (TileBlocks > 0.0)
				o += TileBlocks * tileBlocks(q, salt);
		}
		if (TileStyle.w > 0.0 && (TileCraterTop <= 0.0 || lambda <= TileCraterTop * 1.01)) {
			// faded craters do not count towards the nesting
			float allowed = 1.0 - smoothstep(CRATER_MAX_NEST - 0.75, CRATER_MAX_NEST, nest);
			if (allowed > 0.0) {
				float inside;
				o += allowed * tileCraters(q, salt, craterDensity, variedCraters, inside);
				nest += allowed * inside;
			}
		}
		grad += w * o.xyz;
		if (i < band) {
			// weighted towards the band's longest octave
			float scale = exp2(float(band - 1 - i));
			form += w * o.w / scale;
			formWeight += w / scale;
		}
	}

	// along the surface only
	vec3 slope = grad - up * dot(grad, up);
	frag_color = vec4(clamp(slope / TILE_SLOPE_RANGE, -1.0, 1.0) * 0.5 + 0.5, clamp(0.5 + 0.5 * form / max(formWeight, 1e-6), 0.0, 1.0));
}
