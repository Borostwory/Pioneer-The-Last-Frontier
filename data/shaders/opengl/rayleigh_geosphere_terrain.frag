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

out vec4 frag_color;

uniform float PatchDetailFrequency;

#include "terrain-detail.glsl"
#include "terrain-surface.glsl"

void main(void)
{
	// detail texture contribution, at a constant on-screen scale at any distance
	vec4 detailMul = vec4(vec3(terrainDetail(texture0, texture1, texCoord0, PatchDetailFrequency, dist)), 1.0);

	// surface type patterns: water waves, sand ripples, vegetation clumps, stones
#ifdef TERRAIN_WITH_WATER
	const bool hasWater = true;
#else
	const bool hasWater = false;
#endif
	detailMul.rgb *= terrainSurfacePattern(varyingEyepos, albedoColor.rgb, dist, hasWater);

	// Use the detail value to multiply the final colour before lighting
	vec4 ambient = scene.ambient * vertexColor * 0.f;
	vec4 final = detailMul * vertexColor;

#ifdef ATMOSPHERE
	final = ambient + final;
#else
	// add extra brightness to atmosphere-less planetoids and dim stars
	final = ambient + final * 2.0;
#endif

	frag_color = final;
#ifdef TERRAIN_WITH_LAVA
	frag_color += vertexColor;
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

	frag_color = toSRGB(1 - exp(-frag_color));
}
