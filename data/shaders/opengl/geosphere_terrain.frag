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

in float dist;

in vec3 varyingEyepos;
in vec3 varyingNormal;
in vec4 vertexColor;

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

	// detail texture contribution, at a constant on-screen scale at any distance
	vec4 detailMul = vec4(vec3(terrainDetail(texture0, texture1, texCoord0, PatchDetailFrequency, dist)), 1.0);

	// surface type patterns: water waves, sand ripples, vegetation clumps, stones
#ifdef TERRAIN_WITH_WATER
	const bool hasWater = true;
#else
	const bool hasWater = false;
#endif
	detailMul.rgb *= terrainSurfacePattern(varyingEyepos, vertexColor.rgb, dist, hasWater);

#ifdef TERRAIN_WITH_WATER
	float specularReflection=0.0;
#endif

#if (NUM_LIGHTS > 0)
	vec3 V = normalize(eyeposScaled - geosphereCenter);

	for (int i=0; i<NUM_LIGHTS; ++i) {
		vec3 L = normalize(uLight[i].position.xyz);
		float uneclipsed = clamp(calcUneclipsed(eclipse, NumShadows, V, L), 0.0, 1.0);
#if defined(ATMOSPHERE) && (CLOUD_TERRAIN_SHADOWS == 1)
		uneclipsed *= cloudShadowTerrain(clouds, eyeposScaled - geosphereCenter, L, max(dist * CLOUD_SHADOW_PIXEL_ANGLE, 1.0));
#endif
		CalcPlanetDiffuse(diff, uLight[i].diffuse, L, tnorm, uneclipsed);

#ifdef TERRAIN_WITH_WATER
		//water only for specular
	    if (vertexColor.b > 0.05 && vertexColor.r < 0.05) {
			CalcPlanetSpec(specularReflection, uLight[i], L, tnorm, eyenorm, WATER_SHINE);
		}
#endif
	}

	// Use the detail value to multiply the final colour before lighting
	vec4 ambient = scene.ambient * vertexColor;
	vec4 final = vertexColor * detailMul * diff;

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
		varyingEmission +
#endif
		final;

#ifdef ATMOSPHERE
	frag_color +=
		(1.0-fogFactor) * (diff*atmosColor) +
#ifdef TERRAIN_WITH_WATER
		  diff * specularReflection * sunset +
#endif
		  (0.02-clamp(fogFactor,0.0,0.01))*diff*ldprod*sunset +	      //increase fog scatter
		  (pow((1.0-pow(fogFactor,0.75)),256.0)*0.4*diff*atmosColor)*sunset;  //distant fog.
#endif //ATMOSPHERE

#else // NUM_LIGHTS > 0 -- unlit rendering - stars
	//emission is used to boost colour of stars, which is a bit odd
	frag_color = material.emission + vertexColor;
#endif
}
