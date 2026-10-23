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
// x: wavelength of the largest octave (m), y: its index on the octave ladder,
// z: number of octaves
uniform vec4 TileOctaves;
// xyz: patch centre in cells of the largest octave, modulo 256 (in double precision)
uniform vec4 TileOrigin;
// weights of the styles: rolling, ridged, dunes, craters
uniform vec4 TileStyle;

out vec4 frag_color;

void main(void)
{
	// texel position across the tile's interior, 0..1 (the border lies outside)
	vec2 uv = (gl_FragCoord.xy - TileSlot.xy - TileSlot.w) / TileSlot.z;

	// the patch point at these patch UVs (GeoPatch: u = 1 - x, v = y)
	float x = 1.0 - uv.x, y = uv.y;
	vec3 rel = TileCorner0.xyz + x * (1.0 - y) * (TileCorner1.xyz - TileCorner0.xyz) +
		x * y * (TileCorner2.xyz - TileCorner0.xyz) + (1.0 - x) * y * (TileCorner3.xyz - TileCorner0.xyz);
	vec3 up = normalize(TileCentre.xyz * TileCentre.w + rel);

	vec3 grad = vec3(0.0);
	float form = 0.0, formWeight = 0.0;
	vec3 accumulated = vec3(0.0);
	float lambda = TileOctaves.x;
	int count = int(TileOctaves.z);
	for (int j = 0; j < 4; ++j) {
		if (j >= count)
			break;
		float scale = exp2(float(j));
		vec3 q = mod(TileOrigin.xyz * scale, 256.0) + rel * (scale / lambda);
		float salt = TileOctaves.y - float(j);
		// the shortest octave is close to the texel size: weaker
		float w = (j == count - 1) ? 0.6 : 1.0;
		vec4 o = vec4(0.0);
		if (TileStyle.x > 0.0)
			o += TileStyle.x * tileRolling(q, salt, accumulated);
		if (TileStyle.y > 0.0)
			o += TileStyle.y * tileRidged(q, salt);
		if (TileStyle.z > 0.0)
			o += TileStyle.z * tileDunes(q, salt);
		if (TileStyle.w > 0.0)
			o += TileStyle.w * tileCraters(q, salt, 0.35);
		grad += w * o.xyz;
		form += w * o.w / scale;
		formWeight += w / scale;
	}

	// along the surface only
	vec3 slope = grad - up * dot(grad, up);
	frag_color = vec4(clamp(slope / TILE_SLOPE_RANGE, -1.0, 1.0) * 0.5 + 0.5, clamp(0.5 + 0.5 * form / max(formWeight, 1e-6), 0.0, 1.0));
}
