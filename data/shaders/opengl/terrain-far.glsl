// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// The terrain surface baked from the terrain functions, used per pixel
// instead of the vertex colours and normals:
// - the far texture (TerrainFarTexture in the C++): the whole planet's
//   colour, altitude above sea level and normals in two cube maps, where the
//   patches' vertices are further apart than its texels (from orbit);
// - the patch surface tiles (TerrainSurfaceTiles): the same for each patch
//   at twice its vertex density, in an atlas, where they are finer than the
//   far texture. They follow the patches, so they get finer up close.
//
// Fragment shaders need cloud-uniforms.glsl (planet axes) first.

// x: distance in metres where the patches' vertex spacing reaches the far
// texture's texel size, the texture takes over fully at twice it; y:
// fade-in, 0 to 1 (0: no texture bound); z: how much later patches seen
// edge-on split (GeoSphere::GetLodForeshortening); w: 1 to read the surface
// tiles with the bicubic filter
uniform vec4 FarTexture;

// largest normal offset stored (NORMAL_RANGE in TerrainFarTexture.cpp)
#define FAR_NORMAL_RANGE 0.5

// facing: cosine of the angle between the view and the ground's vertical.
// Patches seen edge-on split later, so their vertices are further apart (as
// GeoPatch::LODUpdate): the distance of a patch seen face-on with the same
// vertex spacing is this much shorter.
float terrainEdgeOnShrink(float facing)
{
	return 1.0 + (sqrt(clamp(facing, 0.25, 1.0)) - 1.0) * FarTexture.z;
}

// weight of the far texture at this distance from the camera (metres)
float terrainFarWeight(float distMetres, float facing)
{
	if (FarTexture.y <= 0.0)
		return 0.0;
	return FarTexture.y * smoothstep(FarTexture.x, 2.0 * FarTexture.x, distMetres / terrainEdgeOnShrink(facing));
}

// the far texture's texel size as a fraction of the patches' estimated
// vertex spacing at this distance (at most 1)
float terrainFarTexelRatio(float distMetres, float facing)
{
	return min(1.0, FarTexture.x * terrainEdgeOnShrink(facing) / max(distMetres, 1.0));
}

#ifdef FRAGMENT_SHADER

// Seen from afar, the slopes of a whole planet are too gentle to catch the light, and the land reads as flat
// paint. Where detail is fixed in space, the lighting takes them this many times as steep from afar. It changes
// only the light, not the ground, and it fades evenly with each factor of distance, gone by the time the ground is near.
#define FAR_RELIEF_SLOPES 4.0
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
	float slopes = mix(1.0, FAR_RELIEF_SLOPES, smoothstep(0.0, 1.0, log(max(distMetres, 1.0) / FAR_RELIEF_NEAR) / log(FAR_RELIEF_FAR / FAR_RELIEF_NEAR)));
	vec3 side = normal - up * along;
	float tangent = length(side) / max(along, 1.0e-3);
	float steep = tangent * slopes;
	if (steep > FAR_RELIEF_KNEE)
		steep = FAR_RELIEF_KNEE + (FAR_RELIEF_MOST - FAR_RELIEF_KNEE) * (1.0 - exp(-(steep - FAR_RELIEF_KNEE) / (FAR_RELIEF_MOST - FAR_RELIEF_KNEE)));
	steep = max(steep, tangent);
	return normalize(up * along + side * (steep / max(tangent, 1.0e-4)));
}

// rgb: colour, a: altitude above sea level as in the vertex alpha
uniform samplerCube farColour;
// rgb: the normal minus the radial direction, planet-fixed, signed sqrt
uniform samplerCube farNormal;

// the patch surface tiles, laid out as farColour and farNormal
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

// up: view-space direction from the planet centre to the pixel. Call it
// only where FarTexture.y > 0 (a uniform branch: it takes derivatives).
FarSurface terrainFarSample(vec3 up)
{
	mat3 toView = mat3(cloudAxisX.xyz, cloudAxisY.xyz, cloudAxisZ.xyz);
	vec3 dir = normalize(transpose(toView) * up);
	return terrainDecodeSurface(texture(farColour, dir), texture(farNormal, dir).xyz, dir, toView);
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

// weight of this patch's surface tile
float terrainTileWeight()
{
	return PatchSurface.w;
}

// uv: the patch coordinates (texCoord0); up as for terrainFarSample. Call it
// only where PatchSurface.w > 0.
FarSurface terrainTileSurface(vec2 uv, vec3 up)
{
	mat3 toView = mat3(cloudAxisX.xyz, cloudAxisY.xyz, cloudAxisZ.xyz);
	vec3 dir = normalize(transpose(toView) * up);
	vec2 st = PatchSurface.xy + clamp(uv, 0.0, 1.0) * PatchSurface.z;
	if (FarTexture.w > 0.5)
		return terrainDecodeSurface(terrainTileCubic(surfaceTileColour, st), terrainTileCubic(surfaceTileNormal, st).xyz, dir, toView);
	return terrainDecodeSurface(textureLod(surfaceTileColour, st, 0.0), textureLod(surfaceTileNormal, st, 0.0).xyz, dir, toView);
}

#endif
