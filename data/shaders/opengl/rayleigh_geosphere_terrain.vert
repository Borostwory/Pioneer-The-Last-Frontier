// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "basesphere_uniforms.glsl"
#include "rayleigh-lib.glsl"
#include "terrain-far.glsl"

uniform int NumShadows;
uniform sampler2D densityLUT;
uniform sampler2D scatterLUT;

out vec3 varyingEyepos;
out vec3 varyingNormal;
out vec4 vertexColor;
// unlit terrain colour, for the surface type patterns
out vec4 albedoColor;

out vec2 texCoord0;
out float dist;

// the body's material weights (terrain-materials.glsl)
layout(location = 10) in vec4 a_materials0;
layout(location = 11) in vec4 a_materials1;
out vec4 materialWeights0;
out vec4 materialWeights1;

#if (NUM_LIGHTS > 0)
// for the per-pixel lighting: the sunlight reaching the surface from each
// light, and the transmittance and in-scattering of the view ray
out vec3 lightAtSurface[NUM_LIGHTS];
out vec3 viewTransmittance;
out vec3 viewInscatter;
#endif

#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
// for the per-pixel water (terrain-water.glsl): the encoded altitude and
// the sky light a flat sea reflects
out float surfaceHeight;
out vec3 waterSkyLight;
#endif

void main(void)
{
	gl_Position = matrixTransform();
	// the colour's alpha holds the altitude above sea level
	albedoColor = vec4(a_color.rgb, 1.0);
	vertexColor = vec4(a_color.rgb, 1.0);
	materialWeights0 = a_materials0;
	materialWeights1 = a_materials1;
	varyingEyepos = vec3(uViewMatrix * a_vertex);
	varyingNormal = normalize(normalMatrix() * a_normal);

	texCoord0 = a_uv0.xy;
	dist = length(varyingEyepos);

	vec3 eyeposScaled = varyingEyepos * geosphereInvRadius;
	vec3 eyenorm = normalize(varyingEyepos);
	vec3 tnorm = normalize(varyingNormal);
	vec4 diff = vec4(0.0);

#if (NUM_LIGHTS > 0)
	vec3 P = eyeposScaled - geosphereCenter;
	vec3 V = normalize(P);

	float AU = 149598000000.0;

	// coordinates, in planet radius
	vec4 planet = vec4(geosphereCenter, geosphereRadius);
	vec4 atmosphere = vec4(geosphereCenter, geosphereAtmosTopRad);

	vec4 terrainColor = vec4(0.f);
	vec4 atmosphereColor = vec4(0.f);
	viewTransmittance = vec3(1.0);
	viewInscatter = vec3(0.0);

#ifdef TERRAIN_WITH_WATER
	surfaceHeight = a_color.a * 2.0 - 1.0;
	waterSkyLight = vec3(0.0);
	// The sky a flat sea reflects, seen from just above the surface. Only
	// below about 1000 m, where pixels can be water, and where the far
	// texture (terrain-far.glsl) draws the coasts instead of the vertices.
	bool nearWater = surfaceHeight < 0.35 || terrainFarWeight(dist) > 0.0;
	vec3 seaUp = normalize(P);
	vec3 skyDir = reflect(eyenorm, seaUp);
	skyDir = normalize(skyDir + seaUp * max(0.0, 0.02 - dot(skyDir, seaUp)));
	vec3 skyStart = seaUp * (geosphereRadius + 20.0);
#endif

	for (int i=0; i<NUM_LIGHTS; ++i) {
		vec3 terrainDiffIn = vec3(0.f);
		vec3 terrainDiffOut = vec3(0.f);
		vec3 atmosphereDiff = vec3(0.f);
		vec3 waterSpecular = vec3(0.f);

		vec3 L = normalize(uLight[i].position.xyz);
		float uneclipsed = clamp(calcUneclipsed(eclipse, NumShadows, V, L), 0.0, 1.0);
		CalcPlanetDiffuse(diff, uLight[i].diffuse, L, tnorm, uneclipsed);

		vec3 lightPosAU = uLight[i].position.xyz / AU;
		// Pioneer uses a LDR pipeline without automatic exposure control, so
		// inverse-square falloff would make most distant planets impossible to
		// see.
		float intensity = 1.f / length(lightPosAU);

		vec4 lightColor = toLinear(uLight[i].diffuse);

		// sunlight reaching the surface
		vec3 sunLight = lightColor.xyz * uneclipsed * intensity;
#ifdef ATMOSPHERE
		// Atmospheric outscattering along the ray between the light source to the point on terrain
		sunLight *= calculateRayTransmittance(P, L, scatterLUT);
#endif
		lightAtSurface[i] = sunLight;
		// start with diffuse terrain color
		vec3 terrain = vertexColor.xyz * sunLight * max(0.f, dot(L, tnorm));
		lightColor *= intensity;

		// TODO: this should compute inscattering + A/B transmittance and pass
		// as a varying to fragment shader so we can run Blinn-Phong lighting
		// per-pixel.
		//
		// There are two multiplications by optical transmittance, one for the
		// sun->surface ray and once for the surface->view ray; the optical
		// transmittance and inscattering can be computed for each and passed to
		// the fragment shader.

#ifdef TERRAIN_WITH_WATER
#ifdef ATMOSPHERE
		// the sky colour, as the sky shader computes it (x20 exposure there),
		// with fewer samples: it varies slowly along the ray
		if (nearWater) {
			_iSamples = 4;
			waterSkyLight += 20.0 * calculateIncidentLight(L, skyDir, skyStart, lightColor, uneclipsed, densityLUT, scatterLUT);
			_iSamples = 16;
		}
#endif
#endif // TERRAIN_WITH_WATER

		// lava glows per pixel (terrain-materials.glsl)
#ifdef TERRAIN_WITH_LAVA
		terrain += material.emission.xyz;
#endif // TERRAIN_WITH_LAVA

#ifdef ATMOSPHERE
		// some light is again lost in atmosphere

		vec2 opticalDistance = vec2(0);
		vec3 inscattering = calculateRayInscattering(opticalDistance, P, -eyenorm, dist * geosphereInvRadius, L, lightColor.xyz, densityLUT, scatterLUT);

		// Apply the optical transmittance of the terrain -> view vector to our accumulated light
		terrain *= calcTransmittance(opticalDistance);
		// And add the (pre-damped) inscattering over the same length. We multiply by 4 to fake multiple scattering.
		terrain += inscattering * 4;
		viewTransmittance = calcTransmittance(opticalDistance);
		viewInscatter += inscattering * 4;

#endif // ATMOSPHERE

		terrainColor.xyz += terrain;
	}

	vertexColor = terrainColor;
#endif // (NUM_LIGHTS > 0)
}
