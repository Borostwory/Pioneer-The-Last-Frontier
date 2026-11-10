// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// The terrain surface baked from the terrain functions, used per pixel
// instead of the vertex colours and normals: the patch surface tiles
// (TerrainSurfaceTiles in the C++), each patch's colour, altitude above sea
// level and normals at twice its vertex density, in an atlas. They follow the
// patches, so they get finer up close. And the light on slopes seen from afar.
//
// Fragment shaders need cloud-uniforms.glsl (planet axes) first.

// x: how many times as steep the light takes slopes from afar (terrainFarRelief);
// y, z: unused; w: 1 to read the surface tiles with the bicubic filter (GeoSphere::SetFarLook)
uniform vec4 FarTexture;

// largest normal offset stored (NORMAL_RANGE in TerrainSurfaceTiles.cpp)
#define FAR_NORMAL_RANGE 1.0

#ifdef FRAGMENT_SHADER

// Seen from afar, the slopes of a whole planet are too gentle to catch the light, and the land reads as flat
// paint. Where detail is fixed in space, the lighting takes them FarTexture.x times as steep from afar. It changes
// only the light, not the ground, and it fades evenly with each factor of distance, gone by the time the ground is near.
#define FAR_RELIEF_NEAR 3.0e3
#define FAR_RELIEF_FAR 3.0e5
// slopes light as steep as this at most (a tangent), or a cliff or a rift's wall would go black; gentle ones aren't touched
#define FAR_RELIEF_KNEE 0.5
#define FAR_RELIEF_MOST 0.85

vec3 terrainFarRelief(vec3 normal, vec3 up, float distMetres)
{
	if (MaterialLook.y < 0.5)
		return normal;
	float along = dot(normal, up);
	float slopes = mix(1.0, FarTexture.x, smoothstep(0.0, 1.0, log(max(distMetres, 1.0) / FAR_RELIEF_NEAR) / log(FAR_RELIEF_FAR / FAR_RELIEF_NEAR)));
	vec3 side = normal - up * along;
	float tangent = length(side) / max(along, 1.0e-3);
	float steep = tangent * slopes;
	if (steep > FAR_RELIEF_KNEE)
		steep = FAR_RELIEF_KNEE + (FAR_RELIEF_MOST - FAR_RELIEF_KNEE) * (1.0 - exp(-(steep - FAR_RELIEF_KNEE) / (FAR_RELIEF_MOST - FAR_RELIEF_KNEE)));
	steep = max(steep, tangent);
	return normalize(up * along + side * (steep / max(tangent, 1.0e-4)));
}

// the patch surface tiles: rgb colour, a altitude above sea level as in the vertex alpha; rgb the normal minus the
// radial direction, planet-fixed, signed sqrt
uniform sampler2D surfaceTileColour;
uniform sampler2D surfaceTileNormal;
// xy: atlas texture coordinates of this patch's tile, z: its size; w: its
// weight, 0 to 1 (0: no tile; GeoPatch::RenderImmediate)
uniform vec4 PatchSurface;

struct FarSurface {
	vec3 albedo;   // as the vertex colour
	float height;  // encoded altitude as the vertex shaders pass it (2 * alpha - 1, terrain-water.glsl)
	vec3 normal;   // view space
};

// from the texels; dir: planet-fixed direction of the pixel
FarSurface terrainDecodeSurface(vec4 colour, vec3 normal, vec3 dir, mat3 toView)
{
	vec3 e = normal * 2.0 - 1.0;
	vec3 offset = sign(e) * e * e * FAR_NORMAL_RANGE;

	FarSurface s;
	s.albedo = colour.rgb;
	s.height = colour.a * 2.0 - 1.0;
	s.normal = normalize(toView * (dir + offset));
	return s;
}

// A Catmull-Rom curve through the texels, in five reads of the linear filter: sharper than
// the linear filter alone, from the same texels. It reads up to two texels away, so the
// tiles have a border that wide (TILE_BORDER in TerrainSurfaceTiles.h).
vec4 terrainTileCubic(sampler2D atlas, vec2 st)
{
	vec2 size = vec2(textureSize(atlas, 0));
	vec2 position = st * size;
	vec2 centre = floor(position - 0.5) + 0.5;
	vec2 f = position - centre;
	vec2 f2 = f * f;
	vec2 f3 = f2 * f;
	vec2 w0 = -0.5 * f3 + f2 - 0.5 * f;
	vec2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
	vec2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
	vec2 w3 = 0.5 * f3 - 0.5 * f2;
	vec2 w12 = w1 + w2;
	vec2 middle = (centre + w2 / w12) / size;
	vec2 before = (centre - 1.0) / size;
	vec2 after = (centre + 2.0) / size;
	float up = w12.x * w0.y;
	float left = w0.x * w12.y;
	float mid = w12.x * w12.y;
	float right = w3.x * w12.y;
	float down = w12.x * w3.y;
	vec4 sum = textureLod(atlas, vec2(middle.x, before.y), 0.0) * up
		+ textureLod(atlas, vec2(before.x, middle.y), 0.0) * left
		+ textureLod(atlas, middle, 0.0) * mid
		+ textureLod(atlas, vec2(after.x, middle.y), 0.0) * right
		+ textureLod(atlas, vec2(middle.x, after.y), 0.0) * down;
	return clamp(sum / (up + left + mid + right + down), 0.0, 1.0);
}

// The parent patch's surface tile, the part of it this patch covers: atlas texture coordinates (origin, size), and
// its weight, 0 without one (GeoPatch::RenderImmediate). The patch takes it on as it takes on its parent's shape
// (patchMorph, terrain-morph.glsl), so where it meets a coarser neighbour it shows the same tile that one does.
uniform vec4 PatchSurfaceParent;
// how much of the parent's surface the ground takes on here (the vertex shader's)
in float patchMorph;

// the share of this patch's own tile and of its parent's in the surface
float terrainTileOwnShare()
{
	return PatchSurface.w * (1.0 - patchMorph);
}

float terrainTileParentShare()
{
	return PatchSurfaceParent.w * patchMorph;
}

// weight of the surface tiles, the patch's own and its parent's: how much of the surface they make, against the
// vertices' colours and normals (themselves blended towards the parent's)
float terrainTileWeight()
{
	return terrainTileOwnShare() + terrainTileParentShare();
}

// a tile at atlas texture coordinates tile (origin xy, size z)
FarSurface terrainTileSurfaceIn(vec3 tile, vec2 uv, vec3 dir, mat3 toView)
{
	vec2 st = tile.xy + clamp(uv, 0.0, 1.0) * tile.z;
	if (FarTexture.w > 0.5)
		return terrainDecodeSurface(terrainTileCubic(surfaceTileColour, st), terrainTileCubic(surfaceTileNormal, st).xyz, dir, toView);
	return terrainDecodeSurface(textureLod(surfaceTileColour, st, 0.0), textureLod(surfaceTileNormal, st, 0.0).xyz, dir, toView);
}

// uv: the patch coordinates (texCoord0); up: view-space direction from the planet centre to the pixel. The patch's
// own tile and its parent's, each by its share. Call it only where terrainTileWeight() > 0.
FarSurface terrainTileSurface(vec2 uv, vec3 up)
{
	mat3 toView = mat3(cloudAxisX.xyz, cloudAxisY.xyz, cloudAxisZ.xyz);
	vec3 dir = normalize(transpose(toView) * up);
	float own = terrainTileOwnShare(), parent = terrainTileParentShare();
	if (parent <= 0.0)
		return terrainTileSurfaceIn(PatchSurface.xyz, uv, dir, toView);
	FarSurface p = terrainTileSurfaceIn(PatchSurfaceParent.xyz, uv, dir, toView);
	if (own <= 0.0)
		return p;
	FarSurface s = terrainTileSurfaceIn(PatchSurface.xyz, uv, dir, toView);
	float t = parent / (own + parent);
	s.albedo = mix(s.albedo, p.albedo, t);
	s.height = mix(s.height, p.height, t);
	s.normal = normalize(mix(s.normal, p.normal, t));
	return s;
}

// the debug window's tint showing the blend between detail levels: red where the ground is all its parent's
vec3 terrainMorphTint(vec3 colour)
{
	if (PatchMorphRange.w < 0.5)
		return colour;
	return mix(colour, vec3(1.0, 0.15, 0.05) * max(dot(colour, vec3(0.3, 0.6, 0.1)), 0.15), 0.7 * patchMorph);
}

#endif
