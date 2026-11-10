// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Crease shading, step 4 (ScreenEffects.cpp): the evened shade (crease_blur.frag) laid over the picture, from the
// two by two half-size pixels round each screen pixel, those about as far away as it counting. Drawn alpha blended
// with black: the picture times the shade.

#include "attributes.glsl"
#include "crease-common.glsl"

uniform sampler2D creaseTex;

out vec4 frag_color;

void main(void)
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	float dist = creaseDist(p);
	if (dist >= CREASE_SKY)
		discard;

	// the half-size pixels each side of this one, and how far between them it is
	ivec2 halfSize = ivec2(CreaseScreen.zw);
	ivec2 base = (p - ivec2(1)) / 2;
	vec2 f = clamp((vec2(p) - 2.0 * vec2(base)) * 0.5, 0.0, 1.0);
	float sum = 0.0, weights = 0.0;
	for (int y = 0; y <= 1; y++) {
		for (int x = 0; x <= 1; x++) {
			ivec2 q = clamp(base + ivec2(x, y), ivec2(0), halfSize - 1);
			float closeness = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
			float w = closeness * creaseDepthWeight(dist, creaseDist(q * 2)) + 1e-4 * closeness;
			sum += w * texelFetch(creaseTex, q, 0).r;
			weights += w;
		}
	}
	float open = weights > 0.0 ? sum / weights : 1.0;
	if (open >= 0.999)
		discard;
	frag_color = vec4(0.0, 0.0, 0.0, 1.0 - open);
}
