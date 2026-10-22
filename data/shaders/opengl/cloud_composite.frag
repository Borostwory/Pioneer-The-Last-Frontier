// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Depth-aware upsampling of the reduced-resolution cloud buffer over the
// scene (premultiplied blending). Cloud pixels whose scene distance matches
// this pixel's get the weight, so clouds do not bleed across the silhouettes
// of buildings, ships or mountains. Softness (cloudPassAtmos.w, 0-2) widens
// the Gaussian kernel to hide the reduced resolution.

#include "attributes.glsl"
#include "cloud-pass-uniforms.glsl"

uniform sampler2D cloudTex;
uniform sampler2D sceneDist;

out vec4 frag_color;

void main(void)
{
	float div = cloudPassProj.w;
	int radius = int(clamp(cloudPassAtmos.w, 0.0, 2.0) + 0.5);
	float sigma = 0.45 + 0.6 * float(radius);

	float z = texelFetch(sceneDist, ivec2(gl_FragCoord.xy), 0).r;

	vec2 lowPos = gl_FragCoord.xy / div - 0.5;
	vec2 base = floor(lowPos);
	ivec2 lowMax = ivec2(cloudPassSize.xy) - 1;
	ivec2 fullMax = ivec2(cloudPassSize.zw) - 1;

	vec4 sum = vec4(0.0);
	float wsum = 0.0;
	vec4 best = vec4(0.0);
	float bestDiff = 1e30;
	for (int j = -2; j <= 3; j++) {
		if (j < -radius || j > radius + 1) continue;
		for (int i = -2; i <= 3; i++) {
			if (i < -radius || i > radius + 1) continue;
			vec2 tc = base + vec2(float(i), float(j));
			ivec2 lp = clamp(ivec2(tc), ivec2(0), lowMax);
			ivec2 cp = clamp(ivec2((vec2(lp) + 0.5) * div), ivec2(0), fullMax);
			float zi = texelFetch(sceneDist, cp, 0).r;
			vec4 ci = texelFetch(cloudTex, lp, 0);
			float diff = abs(zi - z) / max(min(zi, z), 1.0);
			vec2 dxy = tc - lowPos;
			float ws = radius == 0 ? max(1.0 - abs(dxy.x), 0.0) * max(1.0 - abs(dxy.y), 0.0)
								   : exp(-dot(dxy, dxy) / (2.0 * sigma * sigma));
			float w = ws * exp(-8.0 * diff);
			sum += w * ci;
			wsum += w;
			if (diff < bestDiff) {
				bestDiff = diff;
				best = ci;
			}
		}
	}
	frag_color = wsum > 1e-4 ? sum / wsum : best;
}
