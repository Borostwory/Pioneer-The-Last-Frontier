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

#ifdef TERRAIN_WITH_LAVA
in vec4 varyingEmission;
#endif

out vec4 frag_color;

uniform float PatchDetailFrequency;

void main(void)
{
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
	// the patch's surface tile where it has one, else the far texture where
	// the mesh is coarser than it. They are the surface the relief and the
	// bumps are added to below; baseScale: its detail as a fraction of the
	// vertex spacing, where the relief starts
	float facing = dot(surfaceUp, -eyenorm);
	float surfaceWeight = terrainTileWeight();
	float farWeight = terrainFarWeight(dist, facing) * (1.0 - surfaceWeight);
	vec4 albedo = vertexColor;
	float baseScale = 1.0;
#ifdef TERRAIN_WITH_WATER
	float seaHeight = surfaceHeight;
#endif
	if (FarTexture.y > 0.0) {
		FarSurface farSurface = terrainFarSample(surfaceUp);
		albedo.rgb = mix(albedo.rgb, farSurface.albedo, farWeight);
		tnorm = normalize(mix(tnorm, farSurface.normal, farWeight));
		baseScale = mix(baseScale, terrainFarTexelRatio(dist, facing), farWeight);
#ifdef TERRAIN_WITH_WATER
		seaHeight = mix(seaHeight, farSurface.height, farWeight);
#endif
	}
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
	vec3 materialDx = dFdx(varyingEyepos), materialDy = dFdy(varyingEyepos);
	MaterialMapping mapping = materialMapping(varyingEyepos, materialDx, materialDy, surfaceUp);
	vec4 tile = terrainTileSample(texCoord0);
	vec2 bump;
	vec4 surfaceMaterial = terrainMaterialDetail(materialWeights0, materialWeights1, albedo.rgb, mapping,
		dist, tnorm, surfaceUp, altitude, hasWater, tile.w, bump);
	vec4 detailMul = vec4(surfaceMaterial.rgb, 1.0);
	// (the patterns stand in for detail the baked surface has)
	float patternWeight = smoothstep(MATERIAL_NEAR_DIST, MATERIAL_FAR_DIST, dist) * (1.0 - max(farWeight, surfaceWeight));
	if (patternWeight > 0.0) {
		float rock;
		vec4 pw = terrainPatternWeights(materialWeights0, materialWeights1, rock);
		detailMul.rgb *= mix(1.0, terrainSurfacePatternFor(varyingEyepos, dist, pw, rock), patternWeight);
	}

	// the streets and paved ground of the nearest city, dusted with the
	// terrain's colour, and the gardens under its domes with their own
	// grass, moss and soil in place of the terrain's materials
	// (city-ground.glsl)
	vec3 cityPaving;
	float pavingSmoothing;
	vec4 garden;
	float paved = cityGroundAt(varyingEyepos, albedo.rgb, cityPaving, pavingSmoothing, garden);
	albedo.rgb = mix(albedo.rgb, cityPaving, paved);
	if (garden.w > 0.0) {
		vec2 gardenBump;
		float gardenDetail = cityGardenDetail(garden, mapping, dist, gardenBump);
		detailMul.rgb = mix(detailMul.rgb, vec3(gardenDetail), garden.w);
		bump = mix(bump, gardenBump, garden.w);
	}
	detailMul.rgb = mix(detailMul.rgb, vec3(1.0), pavingSmoothing);

	// the relief between the mesh and the pixel
	vec3 meshNormal = tnorm;
	tnorm = terrainReliefNormal(tnorm, surfaceUp, mapping, materialWeights0, materialWeights1, dist, tile, baseScale);
	// and the bumps of the material textures, but for the smooth paving
	tnorm = terrainBumpNormal(tnorm, materialDx, materialDy, bump);
	tnorm = normalize(mix(tnorm, meshNormal, pavingSmoothing));
	// the tracks of the rover's wheels and the walker's boots in soft ground
	// (surface-tracks.glsl)
	albedo.rgb *= surfaceTracksAt(varyingEyepos, surfaceUp, meshNormal, paved, tnorm);

#ifdef TERRAIN_WITH_WATER
	// water: sun glint per light here, the rest after the light loop
	float waterMask = terrainWaterMask(seaHeight);
	vec3 dpdx = dFdx(varyingEyepos), dpdy = dFdy(varyingEyepos);
	WaterSurface ws;
	if (waterMask > 0.0)
		ws = waterSurface(varyingEyepos, normalize(eyeposScaled - geosphereCenter), dpdx, dpdy);
	vec3 waterGlint = vec3(0.0);
#endif

#if (NUM_LIGHTS > 0)
	vec3 V = normalize(eyeposScaled - geosphereCenter);
	// shadows of ships, stations and buildings from the main star
	float sunLit = sceneShadow(varyingEyepos, normalize(varyingNormal));

	for (int i=0; i<NUM_LIGHTS; ++i) {
		vec3 L = normalize(uLight[i].position.xyz);
		float uneclipsed = clamp(calcUneclipsed(eclipse, NumShadows, V, L), 0.0, 1.0);
		if (i == 0)
			uneclipsed *= sunLit;
#if defined(ATMOSPHERE) && (CLOUD_TERRAIN_SHADOWS == 1)
		uneclipsed *= cloudShadowTerrain(clouds, eyeposScaled - geosphereCenter, L, max(dist * CLOUD_SHADOW_PIXEL_ANGLE, 1.0));
#endif
		CalcPlanetDiffuse(diff, uLight[i].diffuse, L, tnorm, uneclipsed);

#ifdef TERRAIN_WITH_WATER
		if (waterMask > 0.0)
			waterGlint += uLight[i].diffuse.rgb * uneclipsed * waterSpecular(ws, -eyenorm, L) * smoothstep(-0.02, 0.05, dot(V, L));
#endif
	}

	// Use the detail value to multiply the final colour before lighting
	vec4 ambient = scene.ambient * albedo;
	vec4 final = albedo * detailMul * diff;

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

	frag_color =
		material.emission +
#ifdef TERRAIN_WITH_LAVA
		varyingEmission + vec4(surfaceMaterial.a * MATERIAL_LAVA_GLOW, 0.0) +
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

	frag_color.rgb *= sceneShadowTint(varyingEyepos, normalize(varyingNormal));

#else // NUM_LIGHTS > 0 -- unlit rendering - stars
	//emission is used to boost colour of stars, which is a bit odd
	frag_color = material.emission + vertexColor;
#endif
}
