// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// The far terrain texture (TerrainFarTexture in the C++): the planet's
// colour, altitude above sea level and surface normals, baked into two cube
// maps from the same terrain functions as the patches. Used per pixel
// instead of the vertex colour and normal where the patches' vertices are
// further apart than its texels, so the planet stays sharp from orbit.
// Looked up with the planet-fixed direction of the pixel.
//
// Fragment shaders need cloud-uniforms.glsl (planet axes) first.

// x: distance in metres where the patches' vertex spacing reaches the texel
// size, the texture takes over fully at twice it; y: fade-in, 0 to 1 (0:
// no texture bound)
uniform vec4 FarTexture;

// largest normal offset stored (NORMAL_RANGE in TerrainFarTexture.cpp)
#define FAR_NORMAL_RANGE 0.5

// weight of the far texture at this distance from the camera (metres)
float terrainFarWeight(float distMetres)
{
	if (FarTexture.y <= 0.0)
		return 0.0;
	return FarTexture.y * smoothstep(FarTexture.x, 2.0 * FarTexture.x, distMetres);
}

#ifdef FRAGMENT_SHADER

// rgb: colour, a: altitude above sea level as in the vertex alpha
uniform samplerCube farColour;
// rgb: the normal minus the radial direction, planet-fixed, signed sqrt
uniform samplerCube farNormal;

struct FarSurface {
	vec3 albedo;   // as the vertex colour
	float height;  // encoded altitude as the vertex shaders pass it (2 * alpha - 1, terrain-water.glsl)
	vec3 normal;   // view space
};

// up: view-space direction from the planet centre to the pixel. Call it
// only where FarTexture.y > 0 (a uniform branch: it takes derivatives).
FarSurface terrainFarSample(vec3 up)
{
	mat3 toView = mat3(cloudAxisX.xyz, cloudAxisY.xyz, cloudAxisZ.xyz);
	vec3 dir = normalize(transpose(toView) * up);
	vec4 colour = texture(farColour, dir);
	vec3 e = texture(farNormal, dir).xyz * 2.0 - 1.0;
	vec3 offset = sign(e) * e * e * FAR_NORMAL_RANGE;

	FarSurface s;
	s.albedo = colour.rgb;
	s.height = colour.a * 2.0 - 1.0;
	s.normal = normalize(toView * (dir + offset));
	return s;
}

#endif
