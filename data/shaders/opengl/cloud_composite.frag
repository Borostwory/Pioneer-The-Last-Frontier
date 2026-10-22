// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Depth-aware upsampling of the reduced-resolution cloud buffer over the
// scene (premultiplied blending). Cloud pixels whose scene distance matches
// this pixel's get the weight, so clouds do not bleed across the silhouettes
// of buildings, ships or mountains. Softness (cloudPassAtmos.w, 0-2) widens
// the Gaussian kernel to hide the reduced resolution.
// Cloud shadows (cloudPassFrame.w = 1): the scene under this pixel is
// darkened by the shadow buffer through the same blend: dst * (1 - a') with
// 1 - a' = (1 - a) * light.

#include "attributes.glsl"
#include "cloud-pass-uniforms.glsl"

uniform sampler2D cloudTex;
uniform sampler2D sceneDist;
uniform sampler2D shadowTex;

// share of the scene's light that cloud shadows can remove (the rest is sky
// light and haze)
#define CLOUD_SHADOW_STRENGTH 0.75

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

	if (cloudPassFrame.w > 0.5 && z < 1e20) {
		// depth-aware bilinear upsample of the half-resolution shadow buffer
		// (size cloudPassHistSize.zw): texels on the sky or on a different
		// surface (a nearer ridge, a ship) do not bleed across silhouettes
		vec2 shSize = cloudPassHistSize.zw;
		vec2 sp = gl_FragCoord.xy * shSize / cloudPassSize.zw - 0.5;
		vec2 sb = floor(sp);
		vec2 sf = sp - sb;
		float lsum = 0.0;
		float lw = 0.0;
		float lbest = 1.0;
		float lbestDiff = 1e30;
		for (int k = 0; k < 4; k++) {
			vec2 o = vec2(float(k & 1), float(k >> 1));
			ivec2 si = clamp(ivec2(sb + o), ivec2(0), ivec2(shSize) - 1);
			ivec2 fi = clamp(ivec2((vec2(si) + 0.5) * cloudPassSize.zw / shSize), ivec2(0), fullMax);
			float zi = texelFetch(sceneDist, fi, 0).r;
			float diff = abs(zi - z) / max(min(zi, z), 1.0);
			float l = texelFetch(shadowTex, si, 0).r;
			float w = (o.x > 0.5 ? sf.x : 1.0 - sf.x) * (o.y > 0.5 ? sf.y : 1.0 - sf.y) * exp(-16.0 * diff);
			lsum += w * l;
			lw += w;
			if (diff < lbestDiff) {
				lbestDiff = diff;
				lbest = l;
			}
		}
		float light = lw > 1e-4 ? lsum / lw : lbest;
		float m = mix(1.0, light, CLOUD_SHADOW_STRENGTH);
		frag_color.a = 1.0 - (1.0 - frag_color.a) * m;
	}
}
