// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// How finely the terrain's tessellation control shaders cut a triangle (P4a of the OpenGL 4.6 plan). Needs the
// uniforms TessParams (x: pixels per radian of the view, y: the edge on screen to aim for in pixels, z: the most cuts)
// and TessScreen (xy: half the viewport's width and height, in pixels), and attributes.glsl (the view's matrices) and
// basesphere_uniforms.glsl (the planet's centre) included before it.

// With the stones raised (P4c), only edges within their reach are cut: nothing further out changes shape, so cutting it
// would cost for nothing. MaterialStones.x: 1 on; MaterialNear: z 1 on, y the distance the stones fade out from (gone
// by twice that).
uniform vec4 MaterialStones;
uniform vec4 MaterialNear;
// the ground cost breakdown's switches (terrain-surface.glsl); 21: what's off screen by the flat ground alone
uniform int GroundSkip;

#include "terrain-stone-aim.glsl"

// the distance from the camera to the nearest point of an edge (eye space)
float terrainTessEdgeDistance(vec3 a, vec3 b)
{
	vec3 d = b - a;
	float t = clamp(-dot(a, d) / max(dot(d, d), 1e-12), 0.0, 1.0);
	return length(a + t * d);
}

// An edge's cuts from its own two ends alone, the same either way round: the patch on the other side of the edge cuts
// it the same, so no cracks open between them. By the pixels it spans on screen (its ends projected), so an edge seen
// at a grazing angle, squashed on screen, isn't cut into slivers thinner than a pixel, which the GPU shades very
// wastefully (2026-11-09: a floor of a quarter of its length across the view still cost 2-3 times the terrain's GPU
// time). Behind the camera, by its length across the view. 1 (not cut) for an edge no longer than the aim. With the
// stones on, the aim at the edge's nearest point grows as the stones there shrink on screen (terrainStoneAim()).
float terrainTessEdge(vec3 a, vec3 b, vec4 ca, vec4 cb)
{
	float aim = max(TessParams.y, 1.0);
	if (MaterialStones.x > 0.5 && MaterialNear.z > 0.5) {
		float near = terrainTessEdgeDistance(a, b);
		if (near > 2.0 * MaterialNear.y || terrainStonePixels(near) < 0.5 * STONE_FADE_PIXELS)
			return 1.0;
		aim = terrainStoneAim(near);
	}
	float pixels;
	if (ca.w > 1e-3 && cb.w > 1e-3) {
		vec2 sa = ca.xy / ca.w * TessScreen.xy;
		vec2 sb = cb.xy / cb.w * TessScreen.xy;
		pixels = length(sa - sb);
	} else {
		float d = max(length(0.5 * (a + b)), 1e-3);
		pixels = length(a - b) / d * TessParams.x;
	}
	return clamp(pixels / aim, 1.0, TessParams.z);
}

// the sides of the view (clip space) that three points all lie outside: bits 1 right, 2 left, 4 top, 8 bottom, 16
// behind the camera
int terrainTessOutsideSides(vec4 c0, vec4 c1, vec4 c2)
{
	int sides = 0;
	if (c0.x > c0.w && c1.x > c1.w && c2.x > c2.w)
		sides |= 1;
	if (c0.x < -c0.w && c1.x < -c1.w && c2.x < -c2.w)
		sides |= 2;
	if (c0.y > c0.w && c1.y > c1.w && c2.y > c2.w)
		sides |= 4;
	if (c0.y < -c0.w && c1.y < -c1.w && c2.y < -c2.w)
		sides |= 8;
	if (c0.w <= 0.0 && c1.w <= 0.0 && c2.w <= 0.0)
		sides |= 16;
	return sides;
}

// the tallest the stones could raise the ground here (metres; terrainStonePixels() without its fading), 0 with them off
float terrainTessTallestStone()
{
	return (MaterialStones.x > 0.5 && MaterialNear.z > 0.5) ? MaterialStones.w * MaterialStones.y * exp2(MaterialNear.x) : 0.0;
}

// a corner raised h metres straight up, in clip space, as the evaluation shaders raise the stones (eyepos: view space)
vec4 terrainTessRaised(vec3 eyepos, vec4 clip, float h)
{
	vec3 up = normalize(eyepos * geosphereInvRadius - geosphereCenter);
	vec3 patchLift = transpose(mat3(uViewMatrix)) * (up * h) / dot(uViewMatrix[0].xyz, uViewMatrix[0].xyz);
	return clip + uViewProjectionMatrix * vec4(patchLift, 0.0);
}

// Whether a triangle can't be seen, so it can be dropped before it's cut: its corners all outside one side of the view,
// and, with the stones on, still outside that side raised as high as they could raise them. (Until 2026-11-09 the flat
// corners alone: ground just below the bottom of the screen, which the stones raise into view, was dropped, leaving a
// hole that showed the sky and flickered as the walker's head bobbed.) p: the corners in eye space, c: in clip space.
bool terrainTessOffscreen(vec3 p0, vec3 p1, vec3 p2, vec4 c0, vec4 c1, vec4 c2)
{
	int sides = terrainTessOutsideSides(c0, c1, c2);
	float h = (GroundSkip & (1 << 21)) != 0 ? 0.0 : terrainTessTallestStone();
	if (sides == 0 || h <= 0.0)
		return sides != 0;
	return (sides & terrainTessOutsideSides(terrainTessRaised(p0, c0, h), terrainTessRaised(p1, c1, h), terrainTessRaised(p2, c2, h))) != 0;
}

// gl_TessLevelOuter[k] is the edge across from corner k; the inside as finely as the finest edge. p: the corners in
// eye space, c: in clip space.
void terrainTessLevels(vec3 p0, vec3 p1, vec3 p2, vec4 c0, vec4 c1, vec4 c2)
{
	if (terrainTessOffscreen(p0, p1, p2, c0, c1, c2)) {
		// (no output: the triangle can't be seen)
		gl_TessLevelOuter[0] = 0.0;
		gl_TessLevelOuter[1] = 0.0;
		gl_TessLevelOuter[2] = 0.0;
		gl_TessLevelInner[0] = 0.0;
		return;
	}
	float e0 = terrainTessEdge(p1, p2, c1, c2);
	float e1 = terrainTessEdge(p2, p0, c2, c0);
	float e2 = terrainTessEdge(p0, p1, c0, c1);
	gl_TessLevelOuter[0] = e0;
	gl_TessLevelOuter[1] = e1;
	gl_TessLevelOuter[2] = e2;
	gl_TessLevelInner[0] = max(e0, max(e1, e2));
}
