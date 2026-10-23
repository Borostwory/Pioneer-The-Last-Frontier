// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Temporal reconstruction of the cloud history buffer. Each frame the march
// pass traces one pixel of every 2x2 or 3x3 history block (cloudPassCenter.w).
// This pass writes every history pixel:
//  - pixels traced this frame take the fresh sample;
//  - the others reproject last frame's history through the planet-fixed
//    frame (planet rotation and camera motion), at the distance of the
//    nearest cloud layer along the ray, clamped to the range of the fresh
//    neighbouring samples so moving or changing clouds do not leave ghosts;
//  - without valid history (first frame, cuts, jumps) a spatial estimate
//    from the fresh samples is used.

#include "attributes.glsl"
#include "lib.glsl"
#include "cloud-pass-uniforms.glsl"

#define atmosColor cloudPassAtmos

#include "cloud-config.glsl"
#include "cloud-uniforms.glsl"
#include "cloud-noise.glsl"
#include "cloud-derive.glsl"
#include "cloud-march.glsl"

uniform sampler2D marchTex;
uniform sampler2D historyTex;
uniform sampler2D sceneDist;

out vec4 frag_color;

void main(void)
{
	ivec2 hp = ivec2(gl_FragCoord.xy);
	ivec2 off = ivec2(cloudPassFrame.xy);
	int blk = max(int(cloudPassCenter.w + 0.5), 1); // history pixels per march pixel
	ivec2 mp = hp / blk;
	ivec2 mMax = ivec2(cloudPassMarch.xy) - 1;

	vec4 fresh = texelFetch(marchTex, min(mp, mMax), 0);
	if (all(equal(hp - mp * blk, off))) {
		frag_color = fresh;
		return;
	}

	// range of the fresh samples around this pixel (for the history clamp)
	vec4 nmin = vec4(1.0);
	vec4 nmax = vec4(0.0);
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			vec4 c = texelFetch(marchTex, clamp(mp + ivec2(i, j), ivec2(0), mMax), 0);
			nmin = min(nmin, c);
			nmax = max(nmax, c);
		}
	}

	// spatial estimate: bilinear between the fresh samples, which sit at
	// history pixel block * m + offset. The buffers are larger than the part in use
	// (the resolution varies with altitude), so texture coordinates use the
	// texture size and stay inside the used part.
	vec2 sp = clamp((vec2(hp) - vec2(off)) / float(blk), vec2(0.0), cloudPassMarch.xy - 1.0);
	vec4 spatial = texture(marchTex, (sp + 0.5) / vec2(textureSize(marchTex, 0)));

	if (cloudPassFrame.z < 0.5) {
		frag_color = spatial;
		return;
	}

	// view ray of this history pixel
	vec2 fullPos = (vec2(hp) + 0.5) * cloudPassMarch.z;
	vec2 ndc = fullPos / cloudPassSize.zw * 2.0 - 1.0;
	vec3 dirV = vec3(ndc.x / cloudPassProj.x, ndc.y / cloudPassProj.y, -1.0);
	vec3 eyenorm = normalize(dirV);
	float a = max(cloudGeom.x, 1.0);
	vec3 camPos = -cloudPassCenter.xyz;

	// reprojection distance: the nearest cloud layer crossing, or the scene
	float viewZ = texelFetch(sceneDist, ivec2(min(fullPos, cloudPassSize.zw - 1.0)), 0).r;
	float tRep = min(viewZ * length(dirV) / a, 1e6);
	CloudPlanet cp = cloudDerive();
	for (int li = 0; li < 3; li++) {
		if (cp.L[li].active < 0.5) continue;
		vec2 h = cloudSphereHit(camPos, eyenorm, 1.0 + 0.5 * (cp.L[li].base + cp.L[li].top) / a);
		if (h.y > 0.0)
			tRep = min(tRep, h.x > 0.0 ? h.x : h.y);
	}

	// this point in the planet-fixed frame, then in last frame's view
	vec3 pf = cloudToPf(eyenorm * tRep + camPos);
	vec3 pPrev = cloudPassPrevCenter.xyz + pf.x * cloudPassPrevAxisX.xyz + pf.y * cloudPassPrevAxisY.xyz + pf.z * cloudPassPrevAxisZ.xyz;
	if (pPrev.z >= 0.0) {
		frag_color = spatial;
		return;
	}
	vec2 uvPrev = vec2(pPrev.x * cloudPassPrevProj.x, pPrev.y * cloudPassPrevProj.y) / -pPrev.z * 0.5 + 0.5;
	if (any(lessThan(uvPrev, vec2(0.0))) || any(greaterThan(uvPrev, vec2(1.0)))) {
		frag_color = spatial;
		return;
	}

	// last frame's history used cloudPassPrevProj.zw pixels of the buffer
	vec2 prevSize = cloudPassPrevProj.zw;
	vec2 hpPrev = clamp(uvPrev * prevSize, vec2(0.5), prevSize - 0.5);
	vec4 history = texture(historyTex, hpPrev / vec2(textureSize(historyTex, 0)));
	frag_color = clamp(history, nmin - CLOUD_HISTORY_CLAMP_MARGIN, nmax + CLOUD_HISTORY_CLAMP_MARGIN);
}
