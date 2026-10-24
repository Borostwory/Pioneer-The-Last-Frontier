// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "basesphere_uniforms.glsl"

#define WATER_SHINE 16.0

#include "cloud-config.glsl"
#include "cloud-uniforms.glsl"
#include "cloud-noise.glsl"
#include "cloud-derive.glsl"
#include "cloud-march.glsl"

uniform sampler2D texture0;
uniform sampler2D texture1;
in vec2 texCoord0;

in float dist;

in vec3 varyingEyepos;
in vec3 varyingNormal;
in vec4 vertexColor;
in vec4 albedoColor;
in vec4 materialWeights0;
in vec4 materialWeights1;

out vec4 frag_color;

uniform float PatchDetailFrequency;

#include "terrain-detail.glsl"
#include "terrain-surface.glsl"

#if (NUM_LIGHTS > 0)
in vec3 lightAtSurface[NUM_LIGHTS];
in vec3 viewTransmittance;
in vec3 viewInscatter;
#endif

#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
in float surfaceHeight;
in vec3 waterSkyLight;
#include "terrain-water.glsl"
#endif

#include "terrain-materials.glsl"
#include "shadows.glsl"
#include "city-lights.glsl"
#if (NUM_LIGHTS > 0)
#include "city-glow.glsl"
#endif

void main(void)
{
#ifdef TERRAIN_WITH_WATER
	const bool hasWater = true;
#else
	const bool hasWater = false;
#endif

	// land materials (photo texture detail, slope rock, beaches) near the
	// camera, procedural surface patterns further out
	vec3 materialDx = dFdx(varyingEyepos), materialDy = dFdy(varyingEyepos);
	vec3 surfaceUp = normalize(varyingEyepos * geosphereInvRadius - geosphereCenter);
#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
	float altitude = terrainAltitude(surfaceHeight);
#else
	float altitude = 1.0e4;
#endif
	MaterialMapping mapping = materialMapping(varyingEyepos, materialDx, materialDy, surfaceUp);
	vec4 tile = terrainTileSample(texCoord0);
	vec2 bump;
	vec4 surfaceMaterial = terrainMaterialDetail(materialWeights0, materialWeights1, albedoColor.rgb, mapping,
		dist, varyingNormal, surfaceUp, altitude, hasWater, tile.w, bump);
	vec4 detailMul = vec4(surfaceMaterial.rgb, 1.0);
	float patternWeight = smoothstep(MATERIAL_NEAR_DIST, MATERIAL_FAR_DIST, dist);
	if (patternWeight > 0.0) {
		float rock;
		vec4 pw = terrainPatternWeights(materialWeights0, materialWeights1, rock);
		detailMul.rgb *= mix(1.0, terrainSurfacePatternFor(varyingEyepos, dist, pw, rock), patternWeight);
	}

#if (NUM_LIGHTS > 0)
	// lit per pixel, with the relief between the mesh and the pixel
	vec3 N = terrainReliefNormal(normalize(varyingNormal), surfaceUp, mapping, materialWeights0, materialWeights1, dist, tile);
	// and the bumps of the material textures
	N = terrainBumpNormal(N, materialDx, materialDy, bump);
	// shadows of ships, stations and buildings from the main star
	float sunLit = sceneShadow(varyingEyepos, normalize(varyingNormal));
	vec3 light = vec3(0.0);
	for (int i = 0; i < NUM_LIGHTS; ++i)
		light += lightAtSurface[i] * max(dot(N, normalize(uLight[i].position.xyz)), 0.0) * (i == 0 ? sunLit : 1.0);
	// night lights of the nearest city
	vec3 citySpecular = vec3(0.0);
	vec3 cityLight = cityLightAt(varyingEyepos, N, 0.0, citySpecular);
	vec3 land = albedoColor.rgb * detailMul.rgb * (light + cityLight);
	// and of the settlements seen from afar
	land += cityGlowAt(surfaceUp, max(length(materialDx), length(materialDy)));
#ifdef TERRAIN_WITH_LAVA
	land += material.emission.xyz * float(NUM_LIGHTS);
#endif
	land = land * viewTransmittance + viewInscatter;
#ifndef ATMOSPHERE
	// add extra brightness to atmosphere-less planetoids and dim stars
	land *= 2.0;
#endif
	frag_color = vec4(land, 0.0);
#else
	frag_color = detailMul * vertexColor;
#endif

#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
	// water: sky reflection, sun glint and the water body, per pixel
	float waterMask = terrainWaterMask(surfaceHeight);
	if (waterMask > 0.0) {
		vec3 up = surfaceUp;
		vec3 V = -normalize(varyingEyepos);
		WaterSurface ws = waterSurface(varyingEyepos, up, materialDx, materialDy);
		float F = waterFresnel(dot(ws.N, V));
		vec3 bodyLight = 0.3 * waterSkyLight + cityLight;
		vec3 water = F * waterSkyLight;
		for (int i = 0; i < NUM_LIGHTS; ++i) {
			vec3 L = normalize(uLight[i].position.xyz);
			vec3 lightIn = lightAtSurface[i] * (i == 0 ? sunLit : 1.0);
			// (no glint from tilted waves once the sun has set)
			water += lightIn * waterSpecular(ws, V, L) * smoothstep(-0.02, 0.05, dot(up, L));
			bodyLight += lightIn * max(dot(up, L), 0.0);
		}
		water += waterBody(albedoColor.rgb, max(-terrainAltitude(surfaceHeight), 0.0)) * bodyLight * (1.0 - F);
		water = water * viewTransmittance + viewInscatter;
		frag_color.rgb = mix(frag_color.rgb, water, waterMask);
	}
#endif


#ifdef ATMOSPHERE
#if (CLOUD_TERRAIN_SHADOWS == 1) && (NUM_LIGHTS > 0)
	// cloud shadows; the clouds themselves are drawn by the cloud pass
	CloudPlanet clouds = cloudDerive();
	// lighting is per-vertex in this shader; dim the direct-light share only
	vec3 surfPos = varyingEyepos * geosphereInvRadius - geosphereCenter;
	float shadow = cloudShadowTerrain(clouds, surfPos, normalize(uLight[0].position.xyz), max(dist * CLOUD_SHADOW_PIXEL_ANGLE, 1.0));
	frag_color.rgb *= mix(1.0, shadow, 0.75);
#endif
#endif

#ifdef TERRAIN_WITH_LAVA
	// lava glows, in shadow and at night too
	frag_color.rgb += surfaceMaterial.a * MATERIAL_LAVA_GLOW;
#endif

	frag_color = toSRGB(1 - exp(-frag_color));
#if (NUM_LIGHTS > 0)
	frag_color.rgb *= sceneShadowTint(varyingEyepos, normalize(varyingNormal));
#endif
}
