// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// P7 of the OpenGL 4.6 plan: the rover's dust on the graphics card, drawn as SfxManager::RenderAll drew TYPE_DUST with
// exhaust.vert: soft puffs facing the camera, growing and fading as they age, streaked along the way they fly. Each
// instance is one puff, read from the state the compute step (gpu_dust_step.comp) keeps; a dead one is left off screen.

#include "attributes.glsl"
#include "lib.glsl"

// (one per instance, unused: the instance's number says which puff)
layout (location = 6) in float a_instIndex;

uniform sampler2D dustState0; // position from the anchor (m, frame axes), age (s)
uniform sampler2D dustState1; // velocity (m/s), lifetime (s)
uniform sampler2D dustState2; // start size, end size (m), drag, gravity
uniform sampler2D dustState4; // colour 0..1, its alpha the opacity
uniform sampler2D dustState5; // x: streak (s), y: noise strength

// xyz: the anchor in view space; the frame's axes in view space
uniform vec4 DustView;
uniform vec4 DustAxisX;
uniform vec4 DustAxisY;
uniform vec4 DustAxisZ;
// x: how lit (SfxManager::RenderAll's illuminationFactor); y: the state's side, in puffs
uniform vec4 DustLight;

out vec2 v_uv;
out vec4 v_color;
out float v_noiseStrength;

vec3 normalizedSafe(vec3 v)
{
	float lenSq = dot(v, v);
	if (lenSq < 1e-18)
		return vec3(1.0, 0.0, 0.0);
	return v * inversesqrt(lenSq);
}

void main(void)
{
	vec2 uv = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
	v_uv = uv;
	int side = int(DustLight.y + 0.5);
	ivec2 t = ivec2(gl_InstanceID % side, gl_InstanceID / side);
	vec4 s0 = texelFetch(dustState0, t, 0);
	vec4 s1 = texelFetch(dustState1, t, 0);
	if (s0.w >= s1.w) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		v_color = vec4(0.0);
		v_noiseStrength = 0.0;
		return;
	}
	vec4 s2 = texelFetch(dustState2, t, 0);
	vec4 colour = texelFetch(dustState4, t, 0);
	vec4 s5 = texelFetch(dustState5, t, 0);

	mat3 toView = mat3(DustAxisX.xyz, DustAxisY.xyz, DustAxisZ.xyz);
	vec3 center = DustView.xyz + toView * s0.xyz;
	float ageNorm = clamp(s0.w / max(s1.w, 0.001), 0.0, 1.0);
	float size = s2.x + (s2.y - s2.x) * sqrt(ageNorm);
	colour.a *= pow(1.0 - ageNorm, 0.8);
	colour.rgb = clamp(colour.rgb * DustLight.x, 0.0, 1.0);
	vec3 jetVectorCam = toView * (s1.xyz * s5.x);
	float backboneCamZ = center.z;
	float stretchScale = s5.x > 0.0 ? 1.0 : 0.0;

	// (as exhaust.vert)
	float ux = mix(-1.0, 1.0, uv.x);
	float vy = mix(-1.0, 1.0, uv.y);
	vec3 viewDir = normalizedSafe(-center);
	vec3 vProj = jetVectorCam - viewDir * dot(jetVectorCam, viewDir);
	vec3 axisV = normalizedSafe(vProj);
	vec3 axisU = normalizedSafe(cross(viewDir, axisV));
	float speedMag = dot(jetVectorCam, axisV);
	float attenuation = 1.0;
	float prevZ = abs(backboneCamZ - jetVectorCam.z);
	float curZ = abs(backboneCamZ);
	if ((prevZ > 0.1) && (curZ > 0.1))
		attenuation = min(1.0, curZ / prevZ);
	float stretch = stretchScale * speedMag * attenuation;
	vec3 worldPos = center + axisU * (ux * size) + axisV * (vy * size);
	if (uv.y < 0.5)
		worldPos -= axisV * stretch;

	gl_Position = uViewProjectionMatrix * vec4(worldPos, 1.0);
	v_color = colour;
	v_noiseStrength = s5.y;
}
