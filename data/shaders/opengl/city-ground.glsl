// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Streets and paved ground of the city nearest the camera (CityLayout.h,
// CityOnPlanet.h), drawn per pixel on the terrain: the asphalt, markings,
// junctions, crossings, kerbs and pavements of each road, and the paving of
// the lots (plazas, yards, the spaceport's apron, car parks).
// Shaders using this declare in their .shaderdef:
//   Texture sampler2D cityRoads
//   Texture sampler2D cityLots
//   Buffer CityGroundData binding=7

layout(std140) uniform CityGroundData {
	mat4 groundViewToCity; // view space -> city space
	vec4 groundRoadRect;   // x, z of the road lookup grid's corner, cell size [m], cells per side
	vec4 groundLotRect;	   // x, z of the lot grid's corner, cell size [m], cells per side
	vec4 groundAtlas;	   // x: first index texel, y: first segment texel, z: strength (0: no city), w: unused
};

uniform sampler2D cityRoads;
uniform sampler2D cityLots;

// The road texture (CityLayout::PackRoads) holds, row by row:
// - per lookup cell, two to a texel: its first index and number of segments
// - the segment indices, four to a texel
// - per segment, two texels: x0, z0, x1, z1 and width, pavement, line * 4 + type, along
#define GROUND_ATLAS_WIDTH 1024
#define GROUND_CELL_MAX 24
#define ROAD_LOCAL 0
#define ROAD_ARTERIAL 1
#define ROAD_CENTRE 2

#define GROUND_ASPHALT vec3(0.075, 0.075, 0.08)
#define GROUND_PAVEMENT vec3(0.40, 0.39, 0.37)
#define GROUND_CONCRETE vec3(0.45, 0.43, 0.40)
#define GROUND_WHITE vec3(0.78, 0.78, 0.76)
#define GROUND_YELLOW vec3(0.75, 0.58, 0.12)
// the lots' concrete slabs, staggered row by row [m]
#define GROUND_SLAB 4.0
// share of the terrain's detail and relief the lots' paving hides (the
// roads hide all of it)
#define GROUND_LOT_SMOOTHING 0.6

float groundHash(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.zyx + 31.32);
	return fract((p3.x + p3.y) * p3.z);
}

// value noise, 0..1
float groundNoise(vec2 p)
{
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(groundHash(i), groundHash(i + vec2(1.0, 0.0)), u.x),
		mix(groundHash(i + vec2(0.0, 1.0)), groundHash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// octaves of it from features scale [m] wide down, each fading to its mean
// where it gets smaller than the pixel, px wide
float groundFbm(vec2 p, float scale, int octaves, float px)
{
	float s = 0.0, amp = 0.5, total = 0.0;
	for (int o = 0; o < octaves; o++) {
		float fade = clamp(1.5 - px / scale, 0.0, 1.0);
		s += amp * mix(0.5, groundNoise(p / scale + float(o) * vec2(17.3, -9.1)), fade);
		total += amp;
		amp *= 0.5;
		scale *= 0.5;
	}
	return s / total;
}

vec4 groundTexel(int texel)
{
	return texelFetch(cityRoads, ivec2(texel % GROUND_ATLAS_WIDTH, texel / GROUND_ATLAS_WIDTH), 0);
}

float groundValue(int i)
{
	vec4 t = groundTexel(i / 4);
	int c = i % 4;
	return c == 0 ? t.x : c == 1 ? t.y : c == 2 ? t.z : t.w;
}

// share of a pixel px wide covered by a line hw half wide, x from its middle
float groundLine(float x, float hw, float px)
{
	float w = max(hw, 0.5 * px);
	return clamp((w - abs(x)) / px + 0.5, 0.0, 1.0) * (hw / w);
}

// dashes along a line, averaged out where they get smaller than pixels
float groundDashes(float along, float period, float duty, float px)
{
	return mix(step(fract(along / period), duty), duty, clamp(px * 3.0 / period, 0.0, 1.0));
}

int groundLotAt(ivec2 c)
{
	int n = int(groundLotRect.w);
	if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, ivec2(n))))
		return 0;
	return int(texelFetch(cityLots, c, 0).r * 255.0 + 0.5);
}

// the paving of the cell's lot, weathered and dusted with the soil round
// it, with its cover of the pixel
float groundLot(vec2 p, float px, vec3 soil, out vec3 colour)
{
	vec2 g = (p - groundLotRect.xy) / groundLotRect.z;
	ivec2 c = ivec2(floor(g));
	int k = groundLotAt(c);
	colour = vec3(0.0);
	if (k == 0)
		return 0.0;
	// inset where the next cell is not the same
	float S = groundLotRect.z;
	vec2 f = fract(g) * S;
	float l = groundLotAt(c + ivec2(-1, 0)) == k ? -9.0 : 3.0;
	float r = groundLotAt(c + ivec2(1, 0)) == k ? -9.0 : 3.0;
	float b = groundLotAt(c + ivec2(0, -1)) == k ? -9.0 : 3.0;
	float t = groundLotAt(c + ivec2(0, 1)) == k ? -9.0 : 3.0;
	float inside = min(min(f.x - l, S - f.x - r), min(f.y - b, S - f.y - t));
	// dust: patches of it, and drifts along the edges
	float drifts = groundFbm(p + 210.0, 35.0, 3, px);
	float edge = 1.0 - smoothstep(0.0, 9.0, inside);
	if (k == 1) { // plazas, paved plots: concrete slabs, stained
		colour = GROUND_CONCRETE * (0.92 + 0.14 * groundFbm(p + 220.0, 7.0, 3, px));
		float row = floor(p.y / GROUND_SLAB);
		float u = p.x / GROUND_SLAB + 0.5 * row;
		colour *= 0.93 + 0.14 * groundHash(vec2(floor(u), row) + 31.0);
		float joint = max(groundLine(abs(fract(u) - 0.5) - 0.5, 0.012, px / GROUND_SLAB),
			groundLine(abs(fract(p.y / GROUND_SLAB) - 0.5) - 0.5, 0.012, px / GROUND_SLAB));
		colour *= 1.0 - 0.3 * joint;
		colour *= 1.0 - 0.25 * smoothstep(0.6, 0.78, groundFbm(p + 330.0, 5.0, 3, px));
		colour = mix(colour, soil * 0.97, clamp(0.28 + 0.3 * drifts + 0.45 * edge, 0.0, 0.9));
	} else if (k == 2) { // yards: gravel
		colour = mix(soil * 0.72, vec3(0.31, 0.30, 0.28), 0.45) * (0.85 + 0.3 * groundFbm(p + 230.0, 1.2, 2, px));
		colour = mix(colour, soil, clamp(0.15 + 0.3 * drifts + 0.4 * edge, 0.0, 0.8));
	} else if (k == 3) { // the spaceport's apron
		float n = groundHash(floor(p / 3.0) + 7.0);
		colour = vec3(0.46, 0.46, 0.45) * (0.96 + 0.04 * n);
		colour = mix(colour, GROUND_YELLOW, groundLine(fract(p.x / 25.0) - 0.5, 0.008, px / 25.0));
		colour = mix(colour, soil, 0.1 + 0.2 * drifts);
	} else { // car parks: worn markings
		colour = GROUND_ASPHALT * (0.9 + 0.2 * groundFbm(p + 230.0, 1.2, 2, px));
		colour = mix(colour, soil * 0.75, clamp(0.22 + 0.25 * drifts + 0.3 * edge, 0.0, 0.8));
		float stall = groundLine(fract(p.x / 2.7) - 0.5, 0.03, px / 2.7) * step(abs(fract(p.y / 16.0) - 0.5), 0.3);
		stall *= 0.4 + 0.6 * groundFbm(p + 360.0, 3.0, 2, px);
		colour = mix(colour, vec3(0.6, 0.58, 0.55), stall);
	}
	return clamp(inside / px + 0.5, 0.0, 1.0);
}

// The city's ground at view-space position viewPos, on the terrain's own
// colour soil: its colour, how much of the pixel it covers (0: the
// terrain's own) and how much of the terrain's detail and relief it hides
float cityGroundAt(vec3 viewPos, vec3 soil, out vec3 colour, out float smoothing)
{
	colour = vec3(0.0);
	smoothing = 0.0;
	vec3 pos = (groundViewToCity * vec4(viewPos, 1.0)).xyz;
	// pixel size, before any branch on the position
	vec2 p = pos.xz;
	float px = max(length(fwidth(p)), 0.02);
	if (groundAtlas.z <= 0.0 || abs(pos.y) > 3000.0)
		return 0.0;

	vec3 lotColour;
	float cover = groundLot(p, px, soil, lotColour) * clamp(groundAtlas.z, 0.0, 1.0);
	colour = lotColour;
	smoothing = cover * GROUND_LOT_SMOOTHING;

	ivec2 c = ivec2(floor((p - groundRoadRect.xy) / groundRoadRect.z));
	int N = int(groundRoadRect.w);
	if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, ivec2(N))))
		return cover;
	int cell = c.y * N + c.x;
	vec4 headers = groundTexel(cell / 2);
	vec2 header = (cell % 2) == 0 ? headers.xy : headers.zw;
	int first = int(groundAtlas.x) * 4 + int(header.x);
	int count = int(header.y);
	int segBase = int(groundAtlas.y);
	if (count == 0)
		return cover;

	// the road whose surface the pixel is deepest in (or nearest to)
	float bestSd = 1e9, lat = 0.0, along = 0.0, rhalf = 1.0, walk = 0.0;
	int type = 0;
	float line = -1.0;
	for (int k = 0; k < GROUND_CELL_MAX; k++) {
		if (k >= count)
			break;
		int s = segBase + 2 * int(groundValue(first + k));
		vec4 a = groundTexel(s);
		vec4 b = groundTexel(s + 1);
		vec2 ab = a.zw - a.xy;
		float L = max(length(ab), 1e-3);
		vec2 dir = ab / L;
		float t = clamp(dot(p - a.xy, dir), 0.0, L);
		vec2 rel = p - (a.xy + dir * t);
		float sd = length(rel) - 0.5 * b.x;
		if (sd < bestSd) {
			bestSd = sd;
			lat = dot(rel, vec2(-dir.y, dir.x));
			along = b.w + t;
			rhalf = 0.5 * b.x;
			walk = b.y;
			type = int(mod(b.z, 4.0) + 0.5);
			line = floor(b.z / 4.0);
		}
	}
	if (bestSd > walk + px)
		return cover;

	// how far the nearest other road's surface is: junctions and crossings
	float otherSd = 1e9;
	for (int k = 0; k < GROUND_CELL_MAX; k++) {
		if (k >= count)
			break;
		int s = segBase + 2 * int(groundValue(first + k));
		vec4 b = groundTexel(s + 1);
		if (abs(floor(b.z / 4.0) - line) < 0.5)
			continue;
		vec4 a = groundTexel(s);
		vec2 ab = a.zw - a.xy;
		float L = max(length(ab), 1e-3);
		vec2 dir = ab / L;
		float t = clamp(dot(p - a.xy, dir), 0.0, L);
		otherSd = min(otherSd, length(p - (a.xy + dir * t)) - 0.5 * b.x);
	}

	float n = groundHash(floor(p / 2.0) + 3.0);
	vec3 surface;
	float surfaceCover;
	if (bestSd > 0.0) {
		// pavement, its kerb
		surfaceCover = clamp((walk - bestSd) / px + 0.5, 0.0, 1.0);
		surface = GROUND_PAVEMENT * (0.93 + 0.07 * n);
		surface *= 1.0 - 0.15 * groundLine(fract(along / 1.8) - 0.5, 0.03, px / 1.8);
		surface = mix(surface, GROUND_PAVEMENT * 0.7, groundLine(bestSd - 0.15, 0.15, px));
	} else {
		surfaceCover = clamp(-bestSd / px + 0.5, 0.0, 1.0);
		surface = GROUND_ASPHALT * (0.88 + 0.24 * n);
		// worn wheel tracks
		float lanes = type == ROAD_ARTERIAL ? 4.0 : 2.0;
		float laneW = 2.0 * rhalf / lanes;
		float inLane = fract((lat + rhalf) / laneW);
		surface *= 1.0 + 0.12 * (groundLine(inLane - 0.3, 0.06, px / laneW) + groundLine(inLane - 0.7, 0.06, px / laneW));
		if (otherSd >= 0.0) {
			float white = 0.0, yellow = 0.0;
			if (type == ROAD_ARTERIAL) {
				yellow = max(groundLine(lat - 0.25, 0.08, px), groundLine(lat + 0.25, 0.08, px));
				white = max(groundLine(abs(lat) - rhalf * 0.5, 0.07, px) * groundDashes(along, 12.0, 0.3, px),
					groundLine(abs(lat) - (rhalf - 0.6), 0.08, px));
			} else {
				white = groundLine(lat, 0.07, px) * groundDashes(along, 9.0, 0.35, px);
			}
			// zebra crossings and stop lines before the junctions
			if (otherSd > 1.5 && otherSd < 5.5) {
				float zebra = mix(step(fract(lat / 1.2), 0.5), 0.5, clamp(px * 2.0 / 1.2, 0.0, 1.0));
				white = max(white, zebra * step(abs(lat), rhalf - 0.4));
				yellow = 0.0;
			} else if (otherSd > 6.2 && otherSd < 6.7 && lat > 0.4) {
				white = 1.0;
			}
			surface = mix(surface, GROUND_WHITE, white);
			surface = mix(surface, GROUND_YELLOW, yellow);
		}
	}
	colour = cover > 0.0 ? mix(lotColour, surface, surfaceCover) : surface;
	float strength = clamp(groundAtlas.z, 0.0, 1.0);
	smoothing = max(smoothing, surfaceCover * strength);
	return max(cover, surfaceCover) * strength;
}
