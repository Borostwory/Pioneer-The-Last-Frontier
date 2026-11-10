// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Crease shading, step 2 (ScreenEffects.cpp): how much of the sky round each point the things next to it hide, at
// half the screen's size. 1: none hidden, 0: all of it.
//
// The way of Jimenez, Wu, Pesce and Jarabo, "Practical Real-Time Strategies for Accurate Indirect Occlusion" (GTAO,
// 2016), as Intel's XeGTAO writes it out: the half circle of sky over the pixel is cut into a few upright slices round
// the view. In each, the highest the surroundings rise on either side is searched for along the screen, out to the
// reach; the sky left between those two horizons, over the pixel's own slope, is what lights it, summed exactly for
// the slice. Things further off than the reach count less and less, then not at all. The slices are turned a
// different way in each pixel of a 4 x 4 block, and the steps along them start a little further in or out, which the
// blur (crease_blur.frag) then evens out.

#include "attributes.glsl"
#include "crease-common.glsl"

out vec4 frag_color;

#define CREASE_SLICES 3
#define CREASE_STEPS 4
#define CREASE_PI 3.14159265
#define CREASE_HALF_PI 1.57079633
// the reach's far share over which things fade from counting fully to not at all (XeGTAO's falloff range)
#define CREASE_FALLOFF 0.615
// the steps crowd toward the pixel: step i of n lies (i / n) to this power of the way out
#define CREASE_STEP_POWER 2.0
// the shade fades out between these distances from the camera (m): it is for where things meet, close by
#define CREASE_FADE_FROM 200.0
#define CREASE_FADE_TO 800.0

// a 4 x 4 Bayer pattern (neighbours far apart), for the slices' turn and, read the other way, the steps' start
const int CREASE_BAYER[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);

void main(void)
{
	ivec2 h = ivec2(gl_FragCoord.xy);
	ivec2 c = h * 2;
	float dist = creaseDist(c);
	if (dist >= CREASE_SKY || dist > CREASE_FADE_TO) {
		frag_color = vec4(1.0);
		return;
	}
	vec3 P = creaseViewPos(c, dist);

	// the surface's slope from the pixels either side, each way from the side nearer in distance (so an outline
	// doesn't tilt it)
	vec3 pR = creaseViewPos(c + ivec2(1, 0), creaseDist(c + ivec2(1, 0)));
	vec3 pL = creaseViewPos(c - ivec2(1, 0), creaseDist(c - ivec2(1, 0)));
	vec3 pU = creaseViewPos(c + ivec2(0, 1), creaseDist(c + ivec2(0, 1)));
	vec3 pD = creaseViewPos(c - ivec2(0, 1), creaseDist(c - ivec2(0, 1)));
	vec3 dx = abs(pR.z - P.z) < abs(P.z - pL.z) ? pR - P : P - pL;
	vec3 dy = abs(pU.z - P.z) < abs(P.z - pD.z) ? pU - P : P - pD;
	vec3 normal = cross(dx, dy);
	if (dot(normal, normal) < 1e-30) {
		frag_color = vec4(1.0);
		return;
	}
	normal = normalize(normal);
	vec3 viewVec = normalize(-P);
	if (dot(normal, viewVec) < 0.0)
		normal = -normal;

	// the reach on screen (px), no more than the most allowed, and in metres what that leaves
	float reach = CreaseParams.x;
	float reachPx = reach * CreaseProj.x * 0.5 * CreaseScreen.x / dist;
	if (reachPx < 1.0) {
		frag_color = vec4(1.0);
		return;
	}
	float spanPx = min(reachPx, CreaseParams.z);
	reach *= spanPx / reachPx;
	float falloffRange = CREASE_FALLOFF * reach;
	float falloffMul = -1.0 / falloffRange;
	float falloffAdd = (reach - falloffRange) / falloffRange + 1.0;

	int cell = (h.x & 3) + 4 * (h.y & 3);
	float sliceNoise = (float(CREASE_BAYER[cell]) + 0.5) / 16.0;
	float stepNoise = (float(CREASE_BAYER[(h.y & 3) + 4 * (h.x & 3)]) + 0.5) / 16.0;

	ivec2 size = ivec2(CreaseScreen.xy);
	float visibility = 0.0;
	for (int slice = 0; slice < CREASE_SLICES; slice++) {
		float phi = (float(slice) + sliceNoise) * CREASE_PI / float(CREASE_SLICES);
		vec2 omega = vec2(cos(phi), sin(phi));

		// the slice's plane through the view, and the pixel's slope within it (n: the angle of its normal from the
		// view, within the slice)
		vec3 directionVec = vec3(omega, 0.0);
		vec3 orthoDirectionVec = directionVec - dot(directionVec, viewVec) * viewVec;
		vec3 axisVec = normalize(cross(orthoDirectionVec, viewVec));
		vec3 projectedNormal = normal - axisVec * dot(normal, axisVec);
		float projectedNormalLength = length(projectedNormal);
		if (projectedNormalLength < 1e-6)
			continue;
		float signNorm = sign(dot(orthoDirectionVec, projectedNormal));
		float cosNorm = clamp(dot(projectedNormal, viewVec) / projectedNormalLength, 0.0, 1.0);
		float n = signNorm * acos(cosNorm);

		// the horizons start flat with the pixel's own surface, and rise to what stands above it
		float lowHorizonCos0 = cos(n + CREASE_HALF_PI);
		float lowHorizonCos1 = cos(n - CREASE_HALF_PI);
		float horizonCos0 = lowHorizonCos0;
		float horizonCos1 = lowHorizonCos1;
		for (int i = 0; i < CREASE_STEPS; i++) {
			float along = pow((float(i) + stepNoise) / float(CREASE_STEPS), CREASE_STEP_POWER);
			vec2 offset = omega * (1.0 + along * (spanPx - 1.0));
			ivec2 q0 = clamp(c + ivec2(round(offset)), ivec2(0), size - 1);
			ivec2 q1 = clamp(c - ivec2(round(offset)), ivec2(0), size - 1);
			vec3 delta0 = creaseViewPos(q0, creaseDist(q0)) - P;
			vec3 delta1 = creaseViewPos(q1, creaseDist(q1)) - P;
			float len0 = length(delta0);
			float len1 = length(delta1);
			float weight0 = clamp(len0 * falloffMul + falloffAdd, 0.0, 1.0);
			float weight1 = clamp(len1 * falloffMul + falloffAdd, 0.0, 1.0);
			float shc0 = len0 > 1e-6 ? dot(delta0 / len0, viewVec) : lowHorizonCos0;
			float shc1 = len1 > 1e-6 ? dot(delta1 / len1, viewVec) : lowHorizonCos1;
			horizonCos0 = max(horizonCos0, mix(lowHorizonCos0, shc0, weight0));
			horizonCos1 = max(horizonCos1, mix(lowHorizonCos1, shc1, weight1));
		}

		// the sky between the two horizons, over the slope, summed exactly (cosine weighted)
		float h0 = -acos(clamp(horizonCos1, -1.0, 1.0));
		float h1 = acos(clamp(horizonCos0, -1.0, 1.0));
		h0 = n + clamp(h0 - n, -CREASE_HALF_PI, CREASE_HALF_PI);
		h1 = n + clamp(h1 - n, -CREASE_HALF_PI, CREASE_HALF_PI);
		float iarc0 = (cosNorm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
		float iarc1 = (cosNorm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
		visibility += projectedNormalLength * (iarc0 + iarc1);
	}
	visibility = clamp(visibility / float(CREASE_SLICES), 0.0, 1.0);

	// strength: the visible sky to this power (1: as worked out, more: darker shade)
	float open = pow(visibility, CreaseParams.y);
	// (fading in as the reach grows from one pixel on screen to four, so far things don't come and go, and out with the
	// distance)
	open = mix(1.0, open, smoothstep(1.0, 4.0, reachPx) * (1.0 - smoothstep(CREASE_FADE_FROM, CREASE_FADE_TO, dist)));
	frag_color = vec4(open, 0.0, 0.0, 1.0);
}
