// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Crease shading (ScreenEffects.cpp): what its steps share. The scene's distance in front of the camera is in
// sceneDist (crease_depth.frag), one texel a screen pixel; the shading itself is worked out at half the screen's
// size, each of its pixels standing for the screen pixel at twice its place.

uniform sampler2D sceneDist;
// x: the projection's P[0], y: its P[5], z: the near plane (m)
uniform vec4 CreaseProj;
// xy: the screen's size (px), zw: the half size the shading is worked out at (px)
uniform vec4 CreaseScreen;
// x: how far round each point the shade reaches (m); y: how strong; z: the most that reach may span on screen (px)
uniform vec4 CreaseParams;

// what the empty background's distance reads as (crease_depth.frag)
#define CREASE_SKY 1e29

// a screen pixel's distance in front of the camera (m), the screen's edge carried on outside it
float creaseDist(ivec2 p)
{
	ivec2 size = ivec2(CreaseScreen.xy);
	return texelFetch(sceneDist, clamp(p, ivec2(0), size - 1), 0).r;
}

// where that pixel is in view space (m), at that distance
vec3 creaseViewPos(ivec2 p, float dist)
{
	vec2 ndc = (vec2(p) + 0.5) / CreaseScreen.xy * 2.0 - 1.0;
	return vec3(ndc.x / CreaseProj.x, ndc.y / CreaseProj.y, -1.0) * dist;
}

// how much a neighbour at distance other counts beside a pixel at dist: nothing once 5 % of the way further or nearer
// (and 2 cm), so the shade doesn't bleed across the outline of a ship against the ground far behind it, while ground
// seen at a low angle, whose distance changes a per cent or so from pixel to pixel, still evens out
float creaseDepthWeight(float dist, float other)
{
	return max(0.0, 1.0 - abs(other - dist) / (0.05 * dist + 0.02));
}
