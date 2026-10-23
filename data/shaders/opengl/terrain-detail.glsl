// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Multi-scale detail texturing for the terrain shaders.
//
// Each detail layer is sampled at two power-of-two scales picked from the
// screen-space size of the patch UVs, and blended between them, so its texels
// stay about the same size on screen from orbit down to the ground. Scales are
// limited so a patch always holds a whole number of texture repeats, which
// keeps patch edges seamless.

// mean brightness of high.dds and low.dds (measured), so the detail varies the
// surface around its colour instead of darkening it
#define DETAIL_HI_MEAN 0.675
#define DETAIL_LO_MEAN 0.504
#define DETAIL_HI_SIZE 512.0
#define DETAIL_LO_SIZE 1024.0

// on-screen size of a detail texel in pixels: fine grain from the high
// texture, broad mottling from the low texture
#define DETAIL_HI_PIXELS_PER_TEXEL 2.0
#define DETAIL_LO_PIXELS_PER_TEXEL 16.0

// detail contrast near the camera and far away (distances in metres)
#define DETAIL_CONTRAST_NEAR 1.0
#define DETAIL_CONTRAST_FAR 0.6
#define DETAIL_CONTRAST_NEAR_DIST 2.0e3
#define DETAIL_CONTRAST_FAR_DIST 2.0e5

// finest scale: 16 texture repeats per deepest patch
#define DETAIL_MIN_LEVEL -4.0

// One detail layer, normalised to a mean of 1.
// uv is in units of the deepest patch width; maxLevel is the coarsest scale
// level (log2 of the patch's detail frequency) that keeps patch edges seamless.
float terrainDetailLayer(sampler2D tex, vec2 uv, vec2 duvdx, vec2 duvdy, float texSize, float pixelsPerTexel, float mean, float maxLevel)
{
	// uv units covered by one pixel
	float pixelSize = max(max(abs(duvdx.x), abs(duvdx.y)), max(abs(duvdy.x), abs(duvdy.y)));
	// level k samples uv * 2^-k, giving texels of 1 / (pixelSize * texSize * 2^-k) pixels
	float level = log2(max(pixelSize * texSize * pixelsPerTexel, 1e-8));
	level = clamp(level, DETAIL_MIN_LEVEL, maxLevel - 1.0);
	float k = floor(level);
	float f = level - k;

	// explicit gradients: k can differ between neighbouring pixels
	float s0 = exp2(-k);
	float s1 = s0 * 0.5;
	float a = textureGrad(tex, uv * s0, duvdx * s0, duvdy * s0).r;
	float b = textureGrad(tex, uv * s1, duvdx * s1, duvdy * s1).r;
	return mix(a, b, f) / mean;
}

// Detail multiplier for the surface colour, around 1.0.
float terrainDetail(sampler2D hiTex, sampler2D loTex, vec2 patchUV, float patchDetailFrequency, float distMetres)
{
	vec2 uv = patchUV * patchDetailFrequency;
	vec2 duvdx = dFdx(uv);
	vec2 duvdy = dFdy(uv);
	float maxLevel = log2(patchDetailFrequency);

	float hi = terrainDetailLayer(hiTex, uv, duvdx, duvdy, DETAIL_HI_SIZE, DETAIL_HI_PIXELS_PER_TEXEL, DETAIL_HI_MEAN, maxLevel);
	float lo = terrainDetailLayer(loTex, uv, duvdx, duvdy, DETAIL_LO_SIZE, DETAIL_LO_PIXELS_PER_TEXEL, DETAIL_LO_MEAN, maxLevel);

	float contrast = mix(DETAIL_CONTRAST_NEAR, DETAIL_CONTRAST_FAR,
		smoothstep(DETAIL_CONTRAST_NEAR_DIST, DETAIL_CONTRAST_FAR_DIST, distMetres));
	return clamp(1.0 + (hi * lo - 1.0) * contrast, 0.2, 2.0);
}
