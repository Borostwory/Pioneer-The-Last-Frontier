// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// A black hole (Star::RenderBlackHoleLens), drawn over the whole screen once everything behind it is drawn.
// For each pixel, the ray of light that reaches the camera through it is followed backwards round the hole, along
// its true path in the hole's curved space (Schwarzschild). Rays that come too close fall in, and the pixel is
// black: the shadow. The rest leave for some point of the sky, which the pixel then shows: from the picture drawn so
// far if that point is on the screen, else from the snapshot of the sky all round. Far from the hole the bending
// comes from a formula instead of following the ray, and where it's less than a fraction of a pixel the pixel is
// left as it is.

#include "attributes.glsl"

uniform sampler2D sceneTex;
uniform samplerCube skyTex;
// xyz: the hole's direction from the camera (view space), w: its distance in Schwarzschild radii
uniform vec4 LensHole;
// x, y: the projection's scale; z: the angle one pixel spans (radians); w: 1 if sceneTex holds the picture
uniform vec4 LensProj;
// view space to the sky snapshot's axes, row by row
uniform vec4 SkyX;
uniform vec4 SkyY;
uniform vec4 SkyZ;

in vec2 v_uv;
out vec4 frag_color;

// beyond this impact parameter (Schwarzschild radii), the bending formula instead of following the ray
const float FAR_B = 60.0;
// the least bending drawn, in pixels
const float LEAST_BEND = 0.3;
// step along the ray, in radians of its angle round the hole, and the most steps (about two and a half turns:
// rays that circle the hole longer than that are taken to fall in)
const float STEP = 0.04;
const int MAX_STEPS = 400;
// how far past the screen's edge (a share of its half size) the picture fades into the sky snapshot
const float EDGE_FADE = 0.05;

// the orbit equation of light, with u = r_s / r against the angle round the hole: u'' = -u + 1.5 u^2
vec2 orbit(vec2 y)
{
	return vec2(y.y, -y.x + 1.5 * y.x * y.x);
}

void main(void)
{
	vec2 ndc = v_uv * 2.0 - 1.0;
	vec3 dir = normalize(vec3(ndc.x / LensProj.x, ndc.y / LensProj.y, -1.0));
	vec3 hole = LensHole.xyz;
	float D = LensHole.w;

	// inside the horizon there's nothing to see
	if (D <= 1.0) {
		frag_color = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}

	float cosT = dot(dir, hole);
	float sinT = length(cross(dir, hole));
	// the ray's impact parameter, from the angle a camera at rest at distance D sees it at
	float b = D * sinT / sqrt(1.0 - 1.0 / D);
	float ib = 1.0 / max(b, 1e-6);

	// far from the hole: the bending of light passing from infinity to infinity, as a series in 1 / b; a camera at
	// a finite distance only gets part of it, (1 + cos) / 2 to first order
	float bendFar = (2.0 * ib + 2.9452431 * ib * ib + 5.3333333 * ib * ib * ib) * 0.5 * (1.0 + cosT);
	bool isFar = b > FAR_B;
	if (isFar && bendFar < LEAST_BEND * LensProj.z)
		discard;

	// the plane the ray stays in: e1 from the hole out to the camera, e2 across it, on the ray's side
	vec3 e1 = -hole;
	vec3 across = dir - dot(dir, e1) * e1;
	float acrossLen = length(across);
	vec3 e2 = acrossLen > 1e-7 ? across / acrossLen :
		normalize(cross(e1, abs(e1.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
	float theta = atan(sinT, cosT);

	// the angle round the hole, from the camera, at which the ray (followed backwards) leaves for good
	float phiOut;
	if (isFar) {
		phiOut = PI - theta + bendFar;
	} else {
		float u = 1.0 / D;
		// du/dphi from the impact parameter; positive heading in towards the hole
		float w = sqrt(max(ib * ib - u * u * (1.0 - u), 0.0)) * (cosT >= 0.0 ? 1.0 : -1.0);
		float phi = 0.0;
		phiOut = -1.0;
		for (int i = 0; i < MAX_STEPS; i++) {
			// far out and heading away: the rest is a straight line
			if (w <= 0.0 && u < 0.02) {
				phiOut = phi + atan(u, -w);
				break;
			}
			vec2 y = vec2(u, w);
			vec2 k1 = orbit(y);
			vec2 k2 = orbit(y + 0.5 * STEP * k1);
			vec2 k3 = orbit(y + 0.5 * STEP * k2);
			vec2 k4 = orbit(y + STEP * k3);
			vec2 next = y + (STEP / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4);
			// fell in
			if (next.x >= 1.0)
				break;
			// out to infinity within this step
			if (next.x <= 0.0) {
				phiOut = phi + STEP * u / (u - next.x);
				break;
			}
			u = next.x;
			w = next.y;
			phi += STEP;
		}
		// the shadow
		if (phiOut < 0.0) {
			frag_color = vec4(0.0, 0.0, 0.0, 1.0);
			return;
		}
	}

	// where in the sky the light came from
	vec3 skyDir = cos(phiOut) * e1 + sin(phiOut) * e2;
	vec3 col = textureLod(skyTex, vec3(dot(SkyX.xyz, skyDir), dot(SkyY.xyz, skyDir), dot(SkyZ.xyz, skyDir)), 0.0).rgb;

	// on the screen: from the picture, which also has the planets and ships further away than the hole
	if (LensProj.w > 0.5 && skyDir.z < 0.0) {
		vec2 p = vec2(skyDir.x * LensProj.x, skyDir.y * LensProj.y) / -skyDir.z;
		float over = max(max(abs(p.x), abs(p.y)) - 1.0, 0.0);
		float onScreen = 1.0 - smoothstep(0.0, EDGE_FADE, over);
		if (onScreen > 0.0)
			col = mix(col, textureLod(sceneTex, clamp(p * 0.5 + 0.5, 0.0, 1.0), 0.0).rgb, onScreen);
	}

	frag_color = vec4(col, 1.0);
}
