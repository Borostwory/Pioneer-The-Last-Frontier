// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Crease shading, step 3 (ScreenEffects.cpp): the shade evened out over 4 x 4 of its half-size pixels, the block its
// slices turn over (crease_ao.frag), each counting only if it's about as far away as this one.

#include "attributes.glsl"
#include "crease-common.glsl"

uniform sampler2D creaseTex;

out vec4 frag_color;

void main(void)
{
	ivec2 h = ivec2(gl_FragCoord.xy);
	float dist = creaseDist(h * 2);
	if (dist >= CREASE_SKY) {
		frag_color = vec4(1.0);
		return;
	}
	ivec2 halfSize = ivec2(CreaseScreen.zw);
	float sum = 0.0, weights = 0.0;
	for (int y = -1; y <= 2; y++) {
		for (int x = -1; x <= 2; x++) {
			ivec2 q = clamp(h + ivec2(x, y), ivec2(0), halfSize - 1);
			float w = creaseDepthWeight(dist, creaseDist(q * 2));
			sum += w * texelFetch(creaseTex, q, 0).r;
			weights += w;
		}
	}
	frag_color = vec4(weights > 0.0 ? sum / weights : 1.0, 0.0, 0.0, 1.0);
}
