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
in vec4 materialWeights2;
in vec4 materialWeights3;
#ifdef TESSELLATED
// how far the evaluation shader raised the ground here, the raised shape's slope, and how much main light gets past
// the stones around (terrainStones(), P4c, P4e)
in float stoneLift;
in vec3 stoneSlope;
in float stoneLit;
#endif
// 1 on the patch's skirt (the vertex shader's), 0 on its ground
in float skirtWeight;

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
#include "terrain-far.glsl"
#include "shadows.glsl"
#include "city-lights.glsl"
#include "city-glow.glsl"
#include "city-ground.glsl"
#include "ship-lights.glsl"
#include "surface-tracks.glsl"

// the glint of the ships' lamps on water
#define SHIP_WATER_SHINE 128.0

void main(void)
{
#ifdef TERRAIN_WITH_WATER
	const bool hasWater = true;
#else
	const bool hasWater = false;
#endif

	// land materials (photo texture detail, slope rock, beaches) near the
	// camera, procedural surface patterns further out
	vec3 surfaceUp = normalize(varyingEyepos * geosphereInvRadius - geosphereCenter);
	// (on the flat ground under the raised stones, as the evaluation shader read them: so the textures stay on the
	// stones, and the stones' light comes from their slopes)
#ifdef TESSELLATED
	vec3 groundPos = varyingEyepos - surfaceUp * stoneLift;
#else
	vec3 groundPos = varyingEyepos;
#endif
	vec3 materialDx = dFdx(groundPos), materialDy = dFdy(groundPos);

	// colour, altitude and normal baked from the terrain (terrain-far.glsl):
	// the patch's surface tile where it has one. They are the surface the
	// relief and the bumps are added to below; baseScale: its detail as a
	// fraction of the vertex spacing, where the relief starts
	float surfaceWeight = GROUND_SKIP(2) ? 0.0 : terrainTileWeight();
	vec3 albedo = albedoColor.rgb;
	vec3 baseNormal = normalize(varyingNormal);
	float baseScale = 1.0;
#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
	float seaHeight = surfaceHeight;
#endif
	if (surfaceWeight > 0.0) {
		FarSurface surfaceTile = terrainTileSurface(texCoord0, surfaceUp);
		albedo = mix(albedo, surfaceTile.albedo, surfaceWeight);
		baseNormal = normalize(mix(baseNormal, surfaceTile.normal, surfaceWeight));
		baseScale = mix(baseScale, 0.5, surfaceWeight);
#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
		seaHeight = mix(seaHeight, surfaceTile.height, surfaceWeight);
#endif
	}

#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
	float altitude = terrainAltitude(seaHeight);
#else
	float altitude = 1.0e4;
#endif
	MaterialMapping mapping = materialMapping(groundPos, materialDx, materialDy, surfaceUp);
	vec4 tile = terrainTileSample(texCoord0);
	vec2 bump;
	float surfaceRoughness;
	vec4 materialSlots[MATERIAL_SLOT_GROUPS] = vec4[MATERIAL_SLOT_GROUPS](materialWeights0, materialWeights1, materialWeights2, materialWeights3);
	vec4 lightWeights = materialLightWeights(materialSlots);
	vec4 surfaceMaterial = vec4(1.0, 1.0, 1.0, 0.0);
	bump = vec2(0.5, 0.0);
	surfaceRoughness = 0.7;
	if (!GROUND_SKIP(0))
		surfaceMaterial = terrainMaterialDetail(materialSlots, albedo, mapping,
			dist, baseNormal, surfaceUp, altitude, hasWater, tile.w, bump, surfaceRoughness);
	vec4 detailMul = vec4(surfaceMaterial.rgb, 1.0);
	// (the patterns stand in for detail the baked surface has)
	float patternWeight = smoothstep(MATERIAL_NEAR_DIST, MATERIAL_FAR_DIST, dist) * (1.0 - surfaceWeight) * (1.0 - MaterialLook.y);
	if (patternWeight > 0.0) {
		float rock, ice, lava;
		vec4 pw = terrainPatternWeights(materialSlots, rock, ice, lava);
		detailMul.rgb *= mix(1.0, terrainSurfacePatternFor(varyingEyepos, dist, pw, rock, ice, lava), patternWeight);
	}

	// the streets and paved ground of the nearest city, dusted with the
	// terrain's colour, and the gardens under its domes with their own
	// grass, moss and soil in place of the terrain's materials
	// (city-ground.glsl)
	vec3 cityPaving = vec3(0.0);
	float pavingSmoothing = 0.0;
	vec4 garden = vec4(1.0, 0.0, 0.0, 0.0);
	float paved = 0.0;
	if (!GROUND_SKIP(3))
		paved = cityGroundAt(varyingEyepos, albedo, cityPaving, pavingSmoothing, garden);
	albedo = mix(albedo, cityPaving, paved);
	if (garden.w > 0.0) {
		vec2 gardenBump;
		float gardenRoughness;
		float gardenDetail = cityGardenDetail(garden, mapping, dist, gardenBump, gardenRoughness);
		detailMul.rgb = mix(detailMul.rgb, vec3(gardenDetail), garden.w);
		bump = mix(bump, gardenBump, garden.w);
		surfaceRoughness = mix(surfaceRoughness, gardenRoughness, garden.w);
	}
	surfaceRoughness = mix(surfaceRoughness, MATERIAL_PAVING_ROUGHNESS, paved);
	detailMul.rgb = mix(detailMul.rgb, vec3(1.0), pavingSmoothing);

	// the lamps of nearby ships on the land and the water, added after the
	// cloud shadows (ship-lights.glsl)
	vec3 shipLight = vec3(0.0);
	vec3 shipLand = vec3(0.0);

#if (NUM_LIGHTS > 0)
	// lit per pixel, with the relief between the mesh and the pixel
	vec3 N = GROUND_SKIP(1) ? baseNormal : terrainReliefNormal(baseNormal, surfaceUp, mapping, materialSlots, dist, tile, baseScale);
	// and the bumps of the material textures, and the raised stones' slope (P4c), but for the smooth paving
	N = terrainBumpNormal(N, materialDx, materialDy, bump);
#ifdef TESSELLATED
	N = normalize(N - (stoneSlope - surfaceUp * dot(stoneSlope, surfaceUp)));
#endif
	N = normalize(mix(N, baseNormal, pavingSmoothing));
	N = terrainFarRelief(N, surfaceUp, dist);
	// the tracks of the rover's wheels and the walker's boots in soft ground
	// (surface-tracks.glsl)
	if (!GROUND_SKIP(6))
		albedo *= surfaceTracksAt(varyingEyepos, surfaceUp, baseNormal, paved, N);
	// shadows of ships, stations and buildings from the main star
	float sunLit = GROUND_SKIP(5) ? 1.0 : sceneShadow(varyingEyepos, normalize(varyingNormal));
#ifdef TESSELLATED
	// and of the raised stones (P4e)
	sunLit *= stoneLit;
#endif
	// A patch's skirt shows only through a gap where two patches don't quite meet: lit as the ground beside it, whose
	// normal it has, not in the shadow of the ground above it, black on a body without air: a dark line along the
	// gap, zigzag where a finer patch meets a coarser one (2026-11-10)
	if (skirtWeight > 0.0)
		sunLit = 1.0;
	vec3 light = vec3(0.0);
	vec3 materialGlint = vec3(0.0);
	float backscatter = 1.0;
	vec3 viewDir = -normalize(varyingEyepos);
	for (int i = 0; i < NUM_LIGHTS; ++i) {
		vec3 sunDir = normalize(uLight[i].position.xyz);
		vec3 lightIn = lightAtSurface[i] * (i == 0 ? sunLit : 1.0);
		light += lightIn * max(dot(N, sunDir), 0.0);
		if (!GROUND_SKIP(7)) {
			materialGlint += lightIn * materialGloss(lightWeights, surfaceRoughness, N, viewDir, sunDir, dist);
			backscatter = max(backscatter, materialBackscatter(lightWeights, viewDir, sunDir));
		}
	}
	vec3 land = albedo * detailMul.rgb * light * backscatter * materialShadowTint(lightWeights, light) * terrainOcclusion(N, surfaceUp, tile) + materialGlint;
#ifdef TERRAIN_WITH_LAVA
	land += material.emission.xyz * float(NUM_LIGHTS);
#endif
#ifdef ATMOSPHERE
	// a trace of air shouldn't leave a moon half as bright as an airless one
	land *= mix(2.0, 1.0, airShare());
#endif
	land = land * viewTransmittance + viewInscatter;
#ifndef ATMOSPHERE
	// add extra brightness to atmosphere-less planetoids and dim stars
	land *= 2.0;
#endif

	// night lights of the nearest city (after the brightening for dim stars,
	// which is not theirs), on paved ground (CITY_GROUND_ALBEDO_*)
	vec3 citySpecular = vec3(0.0);
	vec3 cityLight = GROUND_SKIP(3) || GROUND_SKIP(19) ? vec3(0.0) : cityLightAt(varyingEyepos, N, 0.0, citySpecular);
	vec3 cityLand = clamp(albedo * detailMul.rgb, vec3(CITY_GROUND_ALBEDO_MIN), vec3(CITY_GROUND_ALBEDO_MAX)) * cityLight;
	// and of the cities seen from afar, where the starlight on level ground is dim
	vec3 daylight = vec3(0.0);
	for (int i = 0; i < NUM_LIGHTS; ++i)
		daylight += lightAtSurface[i] * max(dot(surfaceUp, normalize(uLight[i].position.xyz)), 0.0);
	if (!GROUND_SKIP(3))
		cityLand += cityGlowAt(surfaceUp, geosphereRadius, max(length(materialDx), length(materialDy)), dot(daylight, vec3(0.3, 0.59, 0.11)));
	land += cityLand * viewTransmittance;
	frag_color = vec4(land, 0.0);

	// the lamps of nearby ships, on the ground's own colours
	vec3 shipSpecular = vec3(0.0);
	if (!GROUND_SKIP(6))
		shipLight = shipLightAt(varyingEyepos, N, 0.0, shipSpecular);
	shipLand = albedo * detailMul.rgb * shipLight * viewTransmittance;
#else
	frag_color = detailMul * vertexColor;
#endif

#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
	// water: sky reflection, sun glint and the water body, per pixel
	float waterMask = GROUND_SKIP(4) ? 0.0 : terrainWaterMask(seaHeight);
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
		vec3 body = waterBody(albedo, max(-terrainAltitude(seaHeight), 0.0));
		water += body * bodyLight * (1.0 - F);
		water = water * viewTransmittance + viewInscatter;
		frag_color.rgb = mix(frag_color.rgb, water, waterMask);

		// the ships' lamps: their glint and the water body they light
		vec3 shipGlint = vec3(0.0);
		if (shipLightState.x > 0.0)
			shipLightAt(varyingEyepos, ws.N, SHIP_WATER_SHINE, shipGlint);
		shipLand = mix(shipLand, (F * shipGlint + body * shipLight * (1.0 - F)) * viewTransmittance, waterMask);
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

	frag_color.rgb += shipLand;

#ifdef TERRAIN_WITH_LAVA
	// lava glows, in shadow and at night too; not across the water
	float lavaGlowMask = 1.0;
#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
	lavaGlowMask = materialGlowOverWater(waterMask, altitude);
#endif
	frag_color.rgb += lavaGlowMask * surfaceMaterial.a * MATERIAL_LAVA_GLOW;
#endif

	frag_color = toSRGB(1 - exp(-frag_color));
#if (NUM_LIGHTS > 0)
	if (!GROUND_SKIP(5) && skirtWeight <= 0.0)
		frag_color.rgb *= sceneShadowTint(varyingEyepos, normalize(varyingNormal));
#endif
}
