// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "basesphere_uniforms.glsl"

#define WATER_SHINE 16.0

uniform sampler2D texture0;
uniform sampler2D texture1;
in vec2 texCoord0;

uniform int NumShadows;

#include "cloud-config.glsl"
#include "cloud-uniforms.glsl"
#include "cloud-noise.glsl"
#include "cloud-derive.glsl"
#include "cloud-march.glsl"
#include "terrain-detail.glsl"
#include "terrain-surface.glsl"

#ifdef TERRAIN_WITH_WATER
in float surfaceHeight;
#include "terrain-water.glsl"
#endif

#include "terrain-materials.glsl"
#include "terrain-morph.glsl"
#include "terrain-far.glsl"
#include "shadows.glsl"
#include "city-ground.glsl"
#include "ship-lights.glsl"
#include "surface-tracks.glsl"

in float dist;

in vec3 varyingEyepos;
in vec3 varyingNormal;
in vec4 vertexColor;
in vec4 materialWeights0;
in vec4 materialWeights1;
in vec4 materialWeights2;
in vec4 materialWeights3;

#ifdef TERRAIN_WITH_LAVA
in vec4 varyingEmission;
#endif
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

void main(void)
{
#ifdef GROUND_HOLES
	if (inGroundHole(varyingEyepos, skirtWeight > 0.0))
		discard;
#endif
	vec3 eyeposScaled = varyingEyepos * geosphereInvRadius;
	vec3 eyenorm = normalize(varyingEyepos);
	vec3 tnorm = normalize(varyingNormal);
	vec4 diff = vec4(0.0);

	float surfaceDist = dist * geosphereInvRadius;

#if defined(ATMOSPHERE) && (CLOUD_TERRAIN_SHADOWS == 1)
	// cloud shadows; the clouds themselves are drawn by the cloud pass
	CloudPlanet clouds = cloudDerive();
#endif

	vec3 surfaceUp = normalize(eyeposScaled - geosphereCenter);

	// colour, altitude and normal baked from the terrain (terrain-far.glsl):
	// the patch's surface tile where it has one. They are the surface the
	// relief and the bumps are added to below; baseScale: its detail as a
	// fraction of the vertex spacing, where the relief starts
	float surfaceWeight = GROUND_SKIP(2) ? 0.0 : terrainTileWeight();
	vec4 albedo = vertexColor;
	float baseScale = 1.0;
#ifdef TERRAIN_WITH_WATER
	float seaHeight = surfaceHeight;
#endif
	if (surfaceWeight > 0.0) {
		FarSurface surfaceTile = terrainTileSurface(texCoord0, surfaceUp);
		albedo.rgb = mix(albedo.rgb, surfaceTile.albedo, surfaceWeight);
		tnorm = normalize(mix(tnorm, surfaceTile.normal, surfaceWeight));
		baseScale = mix(baseScale, 0.5, surfaceWeight);
#ifdef TERRAIN_WITH_WATER
		seaHeight = mix(seaHeight, surfaceTile.height, surfaceWeight);
#endif
	}

#ifdef TERRAIN_WITH_WATER
	const bool hasWater = true;
	float altitude = terrainAltitude(seaHeight);
#else
	const bool hasWater = false;
	float altitude = 1.0e4;
#endif

	// land materials (photo texture detail, slope rock, beaches) near the
	// camera, procedural surface patterns further out
	// (on the flat ground under the raised stones, as the evaluation shader read them: so the textures stay on the
	// stones, and the stones' light comes from their slopes)
#ifdef TESSELLATED
	vec3 groundPos = varyingEyepos - surfaceUp * stoneLift;
#else
	vec3 groundPos = varyingEyepos;
#endif
	vec3 materialDx = dFdx(groundPos), materialDy = dFdy(groundPos);
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
		surfaceMaterial = terrainMaterialDetail(materialSlots, albedo.rgb, mapping,
			dist, tnorm, surfaceUp, altitude, hasWater, tile.w, bump, surfaceRoughness);
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
		paved = cityGroundAt(varyingEyepos, albedo.rgb, cityPaving, pavingSmoothing, garden);
	albedo.rgb = mix(albedo.rgb, cityPaving, paved);
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
	// the gardens, lots and roads cover the terrain's ice, metal and dust: none of their shine, shadow tint or
	// backscatter (the gardens on metal Phobos shone as bare metal; 2026-11-11)
	lightWeights *= 1.0 - paved;

	// the relief between the mesh and the pixel
	vec3 meshNormal = tnorm;
	if (!GROUND_SKIP(1))
		tnorm = terrainReliefNormal(tnorm, surfaceUp, mapping, materialSlots, dist, tile, baseScale);
	// and the bumps of the material textures, and the raised stones' slope (P4c), but for the smooth paving
	tnorm = terrainBumpNormal(tnorm, materialDx, materialDy, bump);
#ifdef TESSELLATED
	tnorm = normalize(tnorm - (stoneSlope - surfaceUp * dot(stoneSlope, surfaceUp)));
#endif
	tnorm = normalize(mix(tnorm, meshNormal, pavingSmoothing));
	tnorm = terrainFarRelief(tnorm, surfaceUp, dist);
	// the tracks of the rover's wheels and the walker's boots in soft ground
	// (surface-tracks.glsl)
	if (!GROUND_SKIP(6))
		albedo.rgb *= surfaceTracksAt(varyingEyepos, surfaceUp, meshNormal, paved, tnorm);

#ifdef TERRAIN_WITH_WATER
	// water: sun glint per light here, the rest after the light loop
	float waterMask = GROUND_SKIP(4) ? 0.0 : terrainWaterMask(seaHeight);
	vec3 dpdx = dFdx(varyingEyepos), dpdy = dFdy(varyingEyepos);
	WaterSurface ws;
	if (waterMask > 0.0)
		ws = waterSurface(varyingEyepos, normalize(eyeposScaled - geosphereCenter), dpdx, dpdy);
	vec3 waterGlint = vec3(0.0);
#endif

#if (NUM_LIGHTS > 0)
	vec3 V = normalize(eyeposScaled - geosphereCenter);
	vec3 materialGlint = vec3(0.0);
	float backscatter = 1.0;
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

	for (int i=0; i<NUM_LIGHTS; ++i) {
		vec3 L = normalize(uLight[i].position.xyz);
		float uneclipsed = clamp(calcUneclipsed(eclipse, NumShadows, V, L), 0.0, 1.0);
		if (i == 0)
			uneclipsed *= sunLit;
#if defined(ATMOSPHERE) && (CLOUD_TERRAIN_SHADOWS == 1)
		uneclipsed *= cloudShadowTerrain(clouds, eyeposScaled - geosphereCenter, L, max(dist * CLOUD_SHADOW_PIXEL_ANGLE, 1.0));
#endif
		CalcPlanetDiffuse(diff, uLight[i].diffuse, L, tnorm, uneclipsed);
		if (!GROUND_SKIP(7)) {
			materialGlint += uLight[i].diffuse.rgb * uneclipsed * materialGloss(lightWeights, surfaceRoughness, tnorm, -eyenorm, L, dist);
			backscatter = max(backscatter, materialBackscatter(lightWeights, -eyenorm, L));
		}

#ifdef TERRAIN_WITH_WATER
		if (waterMask > 0.0)
			waterGlint += uLight[i].diffuse.rgb * uneclipsed * waterSpecular(ws, -eyenorm, L) * smoothstep(-0.02, 0.05, dot(V, L));
#endif
	}

	// Use the detail value to multiply the final colour before lighting
	vec4 ambient = scene.ambient * albedo;
	vec4 final = albedo * detailMul * diff;
	final.rgb *= backscatter * materialShadowTint(lightWeights, diff.rgb) * terrainOcclusion(tnorm, surfaceUp, tile);
	final.rgb += materialGlint;

#ifdef TERRAIN_WITH_WATER
	vec3 waterReflect = vec3(0.0);
	if (waterMask > 0.0) {
		float F = waterFresnel(dot(ws.N, -eyenorm));
		float depth = max(-terrainAltitude(seaHeight), 0.0);
#ifdef ATMOSPHERE
		// the sky, approximated by the atmosphere colour at the light level
		waterReflect = F * atmosColor.rgb * (diff.r + diff.g + diff.b) / 3.0;
#endif
		final.rgb = mix(final.rgb, waterBody(albedo.rgb, depth) * diff.rgb * (1.0 - F) + waterReflect, waterMask);
	}
#endif

#ifdef ATMOSPHERE
	float ldprod=0.0;
	float fogFactor=0.0;
	CalcPlanetFogFactor(ldprod, fogFactor, eyenorm, eyeposScaled - geosphereCenter, surfaceDist);

	//calculate sunset tone red when passing through more atmosphere, clamp everything.
	float atmpower = (diff.r+diff.g+diff.b)/3.0;
	vec4 sunset = vec4(0.8,clamp(pow(atmpower,0.8),0.0,1.0),clamp(pow(atmpower,1.2),0.0,1.0),1.0);

	final = fogFactor * (ambient + final);
#else
	// add extra brightness to atmosphere-less planetoids and dim stars
	final = ambient + final * 2.0;
#endif

#ifdef TERRAIN_WITH_LAVA
	// lava glows, in shadow and at night too; not across the water
	float lavaGlowMask = 1.0;
#ifdef TERRAIN_WITH_WATER
	lavaGlowMask = materialGlowOverWater(waterMask, altitude);
#endif
#endif

	frag_color =
		material.emission +
#ifdef TERRAIN_WITH_LAVA
		lavaGlowMask * (varyingEmission + vec4(surfaceMaterial.a * MATERIAL_LAVA_GLOW, 0.0)) +
#endif
		final;

#ifdef ATMOSPHERE
	frag_color +=
		(1.0-fogFactor) * (diff*atmosColor) +
#ifdef TERRAIN_WITH_WATER
		  vec4(waterGlint * waterMask, 0.0) * sunset +
#endif
		  (0.02-clamp(fogFactor,0.0,0.01))*diff*ldprod*sunset +	      //increase fog scatter
		  (pow((1.0-pow(fogFactor,0.75)),256.0)*0.4*diff*atmosColor)*sunset;  //distant fog.
#endif //ATMOSPHERE

	// the lamps of nearby ships (ship-lights.glsl), on the ground's own
	// colours, through the tone curve the other terrain shader has
	vec3 shipSpecular = vec3(0.0);
	vec3 shipLight = shipLightAt(varyingEyepos, tnorm, 0.0, shipSpecular);
	frag_color.rgb += toSRGB(1.0 - exp(-albedo.rgb * detailMul.rgb * shipLight));

	if (skirtWeight <= 0.0)
		frag_color.rgb *= sceneShadowTint(varyingEyepos, normalize(varyingNormal));
	frag_color.rgb = terrainMorphTint(frag_color.rgb);

#else // NUM_LIGHTS > 0 -- unlit rendering - stars
	//emission is used to boost colour of stars, which is a bit odd
	frag_color = material.emission + vertexColor;
#endif
}
