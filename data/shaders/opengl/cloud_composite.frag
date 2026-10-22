// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Depth-aware upsampling of the reduced-resolution cloud buffer over the
// scene (premultiplied blending): of the four nearest cloud pixels, those
// whose scene distance matches this pixel's get the weight, so clouds do not
// bleed across the silhouettes of buildings, ships or mountains.

#include "attributes.glsl"
#include "cloud-pass-uniforms.glsl"

uniform sampler2D cloudTex;
uniform sampler2D sceneDist;

out vec4 frag_color;

void main(void)
{
	float div = cloudPassProj.w;
	float z = texelFetch(sceneDist, ivec2(gl_FragCoord.xy), 0).r;

	vec2 lowPos = gl_FragCoord.xy / div - 0.5;
	vec2 base = floor(lowPos);
	vec2 f = lowPos - base;
	ivec2 lowMax = ivec2(cloudPassSize.xy) - 1;
	ivec2 fullMax = ivec2(cloudPassSize.zw) - 1;

	vec4 sum = vec4(0.0);
	float wsum = 0.0;
	vec4 best = vec4(0.0);
	float bestDiff = 1e30;
	for (int j = 0; j < 2; j++) {
		for (int i = 0; i < 2; i++) {
			ivec2 lp = clamp(ivec2(base) + ivec2(i, j), ivec2(0), lowMax);
			ivec2 cp = clamp(ivec2((vec2(lp) + 0.5) * div), ivec2(0), fullMax);
			float zi = texelFetch(sceneDist, cp, 0).r;
			vec4 ci = texelFetch(cloudTex, lp, 0);
			float diff = abs(zi - z) / max(min(zi, z), 1.0);
			float wb = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
			float w = wb * exp(-8.0 * diff);
			sum += w * ci;
			wsum += w;
			if (diff < bestDiff) {
				bestDiff = diff;
				best = ci;
			}
		}
	}
	frag_color = wsum > 1e-3 ? sum / wsum : best;
}
