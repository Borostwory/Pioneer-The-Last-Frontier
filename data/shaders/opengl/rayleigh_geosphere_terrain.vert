// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "basesphere_uniforms.glsl"
#include "terrain-morph.glsl"
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
// 1 on the patch's skirt, 0 on its ground (GeoPatch marks the skirt's uv): the stones taper off down the skirt, and
// it's lit as the ground beside it
out float skirtWeight;
// what GeoPatch adds to a skirt vertex's uv (SKIRT_UV_MARK)
#define TERRAIN_SKIRT_UV_MARK 2.0
// how much of the parent patch's surface the ground takes on here (terrain-morph.glsl), for its surface tile
out float patchMorph;

// the body's material weights (terrain-materials.glsl)
layout(location = 10) in vec4 a_materials0;
layout(location = 11) in vec4 a_materials1;
layout(location = 12) in vec4 a_materials2;
layout(location = 13) in vec4 a_materials3;
out vec4 materialWeights0;
out vec4 materialWeights1;
out vec4 materialWeights2;
out vec4 materialWeights3;

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

// y: 1 on the physical generator's bodies
uniform vec4 MaterialLook;
// the debug window's ground cost breakdown (GROUND_SKIP in terrain-surface.glsl); bit 8: the air's light along the view
// and the sky on the water, worked out at each corner
uniform int GroundSkip;

void main(void)
{
	// blended towards the parent patch's surface (terrain-morph.glsl)
	float morph = terrainMorph();
	vec4 position = terrainMorphPosition(morph);
	vec4 colour = terrainMorphColor(morph);
	patchMorph = morph;
	gl_Position = uViewProjectionMatrix * position;
	// the colour's alpha holds the altitude above sea level
	albedoColor = vec4(colour.rgb, 1.0);
	vertexColor = vec4(colour.rgb, 1.0);
	materialWeights0 = a_materials0;
	materialWeights1 = a_materials1;
	materialWeights2 = a_materials2;
	materialWeights3 = a_materials3;
	varyingEyepos = vec3(uViewMatrix * position);
	varyingNormal = normalize(normalMatrix() * terrainMorphNormal(morph));

	bool skirt = a_uv0.x > 0.5 * TERRAIN_SKIRT_UV_MARK + 0.5;
	texCoord0 = skirt ? a_uv0.xy - TERRAIN_SKIRT_UV_MARK : a_uv0.xy;
	skirtWeight = skirt ? 1.0 : 0.0;
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
	surfaceHeight = colour.a * 2.0 - 1.0;
	waterSkyLight = vec3(0.0);
	// The sky a flat sea reflects, seen from just above the surface. Only
	// below about 1000 m, where pixels can be water.
	bool nearWater = surfaceHeight < 0.35;
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
		// (the physical generator's colours are the ground's real ones, so its bodies get a gentler
		// falloff: close-in worlds don't burn out, and an ice moon of Jupiter still reads as ice)
		float sunIntensity = MaterialLook.y > 0.5 ? sqrt(intensity) : intensity;
		vec3 sunLight = lightColor.xyz * uneclipsed * sunIntensity;
#ifdef ATMOSPHERE
		// Atmospheric outscattering along the ray between the light source to the point on terrain
		sunLight *= pow(calculateRayTransmittance(P, L, scatterLUT), vec3(airShare()));
#endif
		lightAtSurface[i] = sunLight;
		// start with diffuse terrain color
		vec3 terrain = vertexColor.xyz * sunLight * max(0.f, dot(L, tnorm));
		// The air's glow gets the falloff the ground gets, so far from the sun a thick haze
		// still hides the ground (Titan). Bodies with Earth's values keep the old falloff.
		lightColor *= atmosEarthValues() ? intensity : sunIntensity;

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
		if (nearWater && (GroundSkip & 256) == 0) {
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
		vec3 inscattering = vec3(0.0);
		if ((GroundSkip & 256) == 0)
			inscattering = calculateRayInscattering(opticalDistance, P, -eyenorm, dist * geosphereInvRadius, L, lightColor.xyz, densityLUT, scatterLUT);

		// Apply the optical transmittance of the terrain -> view vector to our accumulated light
		vec3 viewLoss = pow(calcTransmittance(opticalDistance), vec3(airShare()));
		terrain *= viewLoss;
		// And add the (pre-damped) inscattering over the same length. We multiply by 4 to fake multiple scattering.
		terrain += inscattering * 4;
		viewTransmittance = viewLoss;
		viewInscatter += inscattering * 4;

		// A thick haze seen from above (Titan's smog, atmosHazeExtinction.w): the light that
		// bounces many times inside it comes back out as if off a pale ground of the haze's own
		// colour, which the single bounce above can't give. Its brightness is a thick layer's:
		// (1 - g) / (1 + g) with g = sqrt(1 - share of the dimming that is scattering), lit
		// like the ground (sun height), a little darker towards the limb, and only as much as
		// the haze along the view hides what is behind it.
		if (!atmosEarthValues() && atmosHazeExtinction.w > 0.0) {
			vec3 tauHaze = ATMOS_BETA_M0 * atmosHazeExtinction.rgb * opticalDistance.y;
			vec3 scatterShare = atmosHazeScatter.rgb / max(atmosHazeExtinction.rgb, vec3(1e-9));
			vec3 gam = sqrt(max(1.0 - scatterShare, vec3(0.0)));
			vec3 layerAlbedo = (1.0 - gam) / (1.0 + gam);
			vec3 up = normalize(P);
			float muS = max(dot(up, L), 0.0);
			float muV = max(dot(up, -eyenorm), 0.0);
			vec3 glow = atmosHazeExtinction.w * layerAlbedo * lightColor.xyz * uneclipsed * muS * (0.6 + 0.4 * muV) *
				(1.0 - exp(-tauHaze));
			// in place of the single bounce where the haze hides the view (atmosHazeCover)
			vec3 swap = glow - inscattering * 4.0 * atmosHazeCover(opticalDistance.y);
			terrain += swap;
			viewInscatter += swap;
		}

#endif // ATMOSPHERE

		terrainColor.xyz += terrain;
	}

	vertexColor = terrainColor;
#endif // (NUM_LIGHTS > 0)
}
