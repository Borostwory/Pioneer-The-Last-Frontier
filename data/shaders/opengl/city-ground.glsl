// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Streets and paved ground of the city nearest the camera (CityLayout.h,
// CityOnPlanet.h), drawn per pixel on the terrain: the asphalt, markings,
// junctions, crossings, kerbs and pavements of each road, the paving of the
// lots (plazas, yards, the spaceport's apron, car parks), and under the
// forcefield domes a living ground: meadows, lawns, shrubs and flowers, with
// the grass, moss and soil photo textures of terrain-materials.glsl (which
// must be included first).
// Shaders using this declare in their .shaderdef:
//   Texture sampler2D cityRoads
//   Texture sampler2D cityLots
//   Buffer CityGroundData binding=7

#define GROUND_DOME_MAX 8

layout(std140) uniform CityGroundData {
	mat4 groundViewToCity; // view space -> city space
	vec4 groundRoadRect;   // x, z of the road lookup grid's corner, cell size [m], cells per side
	vec4 groundLotRect;	   // x, z of the lot grid's corner, cell size [m], cells per side
	vec4 groundAtlas;	   // x: first index texel, y: first segment texel, z: strength (0: no city), w: number of domes
	vec4 groundDomes[GROUND_DOME_MAX]; // x, y, z of each dome's sphere's centre in city space, its radius [m]
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

// the gardens under the domes: grass gone dry in the sun and in its prime,
// shrubs, and the dark soil they grow in
#define GROUND_GRASS_DRY vec3(0.17, 0.22, 0.07)
#define GROUND_GRASS_LUSH vec3(0.06, 0.17, 0.04)
#define GROUND_SHRUB vec3(0.05, 0.11, 0.04)
#define GROUND_LOAM vec3(0.13, 0.095, 0.06)
// the band inside a dome's rim where the grass thins out to the bare ground
// (in from the dome's sphere) [m]
#define GROUND_GARDEN_RIM 60.0
// the bushes: one at most per cell this wide [m]
#define GROUND_BUSH_CELL 5.0
// the flowers: one at most per cell this wide [m], this wide in it [cells]
#define GROUND_FLOWER_CELL 0.35
#define GROUND_FLOWER_SIZE 0.14
// how much of a field in bloom they hide seen from afar, the grass between
// them hidden by the flowers in front
#define GROUND_FLOWER_FIELD 0.22

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

// how deep the city-space position pos is inside the domes' spheres [m],
// negative outside them all
float groundDomeDepth(vec3 pos)
{
	float depth = -1e9;
	for (int d = 0; d < GROUND_DOME_MAX; d++) {
		if (float(d) >= groundAtlas.w)
			break;
		depth = max(depth, groundDomes[d].w - length(pos - groundDomes[d].xyz));
	}
	return depth;
}

// a flower's colour, k 0..1: buttercups, daisies, violets, poppies, pinks,
// marigolds
vec3 groundFlower(float k)
{
	int i = int(k * 6.0);
	return i == 0 ? vec3(0.78, 0.62, 0.08) : i == 1 ? vec3(0.82, 0.82, 0.76) : i == 2 ? vec3(0.38, 0.18, 0.58) : i == 3 ? vec3(0.72, 0.09, 0.06) : i == 4 ? vec3(0.82, 0.42, 0.58) : vec3(0.85, 0.40, 0.05);
}

// the garden under the domes on the bare ground soil: meadows of dry and
// lush grass, worn patches of soil, clumps of shrubs and fields of flowers,
// with its cover of the pixel, nothing outside the domes. In a ragged band
// inside the rims the grass grows sparse and dusty, thinning out to the bare
// ground at the rims themselves. materials: the shares of grass, bushes and
// soil in it, for cityGardenDetail()
float groundGarden(vec3 pos, float px, vec3 soil, out vec3 colour, out vec3 materials)
{
	colour = vec3(0.0);
	materials = vec3(1.0, 0.0, 0.0);
	if (groundAtlas.w < 0.5)
		return 0.0;
	vec2 p = pos.xz;
	float thin = clamp(groundDomeDepth(pos) / GROUND_GARDEN_RIM, 0.0, 1.0);
	if (thin <= 0.0)
		return 0.0;
	// (0 at the rims whatever the noise)
	float ragged = 0.6 * groundFbm(p + 810.0, 25.0, 2, px) + 0.4 * groundFbm(p + 840.0, 2.0, 2, px);
	float cover = smoothstep(0.0, 1.0, 1.7 * thin - 0.7 + 0.7 * ragged);

	float meadow = smoothstep(0.3, 0.7, groundFbm(p + 410.0, 140.0, 3, px));
	vec3 grass = mix(GROUND_GRASS_DRY, GROUND_GRASS_LUSH, meadow);
	grass *= 0.8 + 0.4 * groundFbm(p + 470.0, 20.0, 3, px);
	// tussocks and tufts (the blades are the grass texture's)
	grass *= 0.8 + 0.4 * groundFbm(p + 490.0, 3.0, 2, px);
	grass *= 0.7 + 0.6 * groundFbm(p + 520.0, 0.3, 2, px);
	// worn to the soil here and there (averaged out where the patches get
	// smaller than pixels)
	float worn = 0.7 * mix(smoothstep(0.7, 0.78, groundFbm(p + 560.0, 5.0, 3, px)), 0.03, clamp(px / 5.0, 0.0, 1.0));
	grass = mix(grass, GROUND_LOAM * (0.8 + 0.4 * groundFbm(p + 580.0, 0.5, 2, px)), worn);
	// round bushes, at most one a cell, gathered in thickets, darker round
	// their edges
	float thickets = smoothstep(0.45, 0.7, groundFbm(p + 600.0, 30.0, 2, px));
	vec2 bq = p / GROUND_BUSH_CELL;
	vec2 bc = floor(bq);
	float br = 0.12 + 0.16 * groundHash(bc + 611.0);
	float bd = length(fract(bq) - br - (1.0 - 2.0 * br) * vec2(groundHash(bc + 613.0), groundHash(bc + 617.0)));
	float pxb = px / GROUND_BUSH_CELL;
	float bw = max(br, 0.5 * pxb);
	float shrub = clamp((bw - bd) / pxb + 0.5, 0.0, 1.0) * (br / bw) * (br / bw) * step(groundHash(bc + 619.0), thickets);
	// (averaged out where they get smaller than pixels)
	shrub = mix(shrub, 0.13 * thickets, clamp(2.0 * pxb - 1.0, 0.0, 1.0));
	vec3 leaves = GROUND_SHRUB * (0.4 + 1.2 * groundFbm(p + 650.0, 0.4, 3, px)) * (1.15 - 0.6 * clamp(bd / br, 0.0, 1.0) * clamp(bd / br, 0.0, 1.0));
	colour = mix(grass, leaves, shrub);

	// flowers where the meadows bloom, in drifts, each field mostly of one kind
	float bloom = smoothstep(0.5, 0.68, groundFbm(p + 700.0, 45.0, 2, px)) * (1.0 - shrub) * thin;
	float drifts = smoothstep(0.35, 0.6, groundFbm(p + 760.0, 3.0, 2, px));
	float kind = groundNoise(p / 70.0 + 13.0);
	vec2 q = p / GROUND_FLOWER_CELL;
	vec2 cell = floor(q);
	vec2 at = 0.2 + 0.6 * vec2(groundHash(cell + 17.0), groundHash(cell + 43.0));
	float pxc = px / GROUND_FLOWER_CELL;
	float w = max(GROUND_FLOWER_SIZE, 0.5 * pxc);
	float r = length(fract(q) - at);
	float petals = clamp((w - r) / pxc + 0.5, 0.0, 1.0) * (GROUND_FLOWER_SIZE / w) * (GROUND_FLOWER_SIZE / w);
	petals *= step(groundHash(cell + 71.0), bloom * drifts);
	vec3 flower = groundFlower(groundHash(cell + 97.0) < 0.8 ? kind : groundHash(cell + 89.0)) * (0.8 + 0.3 * groundHash(cell + 53.0));
	// its heart
	flower = mix(flower, vec3(0.55, 0.38, 0.06), clamp((0.3 * GROUND_FLOWER_SIZE - r) / pxc + 0.5, 0.0, 1.0));
	// where they get smaller than pixels: the field's tint
	float blur = clamp(1.5 * pxc - 0.5, 0.0, 1.0);
	petals = mix(petals, GROUND_FLOWER_FIELD * bloom * drifts, blur);
	colour = mix(colour, mix(flower, groundFlower(kind), blur), petals);

	// sparse and dusty with the bare ground's soil towards the rims
	float dusty = 0.6 * (1.0 - smoothstep(0.0, 1.0, thin));
	colour = mix(colour, soil, dusty);
	float bare = max(worn, dusty) * (1.0 - shrub);
	materials = vec3(1.0 - shrub - bare, shrub, bare);
	return cover;
}

// The photo textures' detail of the gardens (terrain-materials.glsl, which
// must come first): grass, moss on the bushes and soil, in the shares of
// garden.xyz (from cityGroundAt), fading out with distance as the terrain's
// own. Returns the detail (mean 1); bump: their height and its scale, as
// terrainMaterialDetail()'s
float cityGardenDetail(vec4 garden, MaterialMapping mapping, float distMetres, out vec2 bump)
{
	bump = vec2(0.5, 0.0);
	float contrast = 1.0 - smoothstep(MATERIAL_NEAR_DIST, MATERIAL_FAR_DIST, distMetres);
	if (garden.w <= 0.0 || contrast <= 0.0)
		return 1.0;
	float coarse = mapping.level + log2(MATERIAL_COARSE_SCALE);
	float detail = 0.0, height = 0.0, total = 0.0;
	for (int k = 0; k < 3; k++) {
		float w = garden[k];
		if (w < 0.02)
			continue;
		int layer = k == 0 ? MATERIAL_GRASS : k == 1 ? MATERIAL_FOREST : MATERIAL_SOIL;
		vec3 f = materialSample(layer, mapping, mapping.level);
		vec3 c = materialSample(layer, mapping, coarse);
		detail += w * f.y * c.y;
		height += w * f.z;
		total += w;
	}
	if (total <= 0.0)
		return 1.0;
	bump = vec2(height / total, contrast * MATERIAL_BUMP_HEIGHT * exp2(mapping.level));
	return 1.0 + (detail / total - 1.0) * contrast;
}

// the surface of the road at p (the one the pixel is deepest in), with its
// cover of the pixel
float groundRoad(vec2 p, float px, out vec3 surface)
{
	surface = vec3(0.0);
	ivec2 c = ivec2(floor((p - groundRoadRect.xy) / groundRoadRect.z));
	int N = int(groundRoadRect.w);
	if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, ivec2(N))))
		return 0.0;
	int cell = c.y * N + c.x;
	vec4 headers = groundTexel(cell / 2);
	vec2 header = (cell % 2) == 0 ? headers.xy : headers.zw;
	int first = int(groundAtlas.x) * 4 + int(header.x);
	int count = int(header.y);
	int segBase = int(groundAtlas.y);
	if (count == 0)
		return 0.0;

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
		return 0.0;

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
	return surfaceCover;
}

// a layer of colour above with its cover over the ground below: the colour
// of the two together, cover their cover
vec3 groundOver(vec3 below, inout float cover, vec3 above, float aboveCover)
{
	float total = 1.0 - (1.0 - cover) * (1.0 - aboveCover);
	vec3 colour = total > 0.0 ? (below * cover * (1.0 - aboveCover) + above * aboveCover) / total : above;
	cover = total;
	return colour;
}

// The city's ground at view-space position viewPos, on the terrain's own
// colour soil: its colour, how much of the pixel it covers (0: the
// terrain's own) and how much of the terrain's detail and relief it hides;
// garden: the shares of grass, bushes and soil in the gardens under the
// domes (xyz) and how much of the pixel they cover (w), whose detail
// replaces the terrain's (cityGardenDetail)
float cityGroundAt(vec3 viewPos, vec3 soil, out vec3 colour, out float smoothing, out vec4 garden)
{
	colour = vec3(0.0);
	smoothing = 0.0;
	garden = vec4(1.0, 0.0, 0.0, 0.0);
	vec3 pos = (groundViewToCity * vec4(viewPos, 1.0)).xyz;
	// pixel size, before any branch on the position
	vec2 p = pos.xz;
	float px = max(length(fwidth(p)), 0.02);
	if (groundAtlas.z <= 0.0 || abs(pos.y) > 3000.0)
		return 0.0;

	// the gardens under the domes, the lots' paving over them (dusted with
	// the soil round it, the gardens' own under the domes), and the roads
	vec3 materials;
	float cover = groundGarden(pos, px, soil, colour, materials);
	float gardenCover = cover;
	vec3 lotColour;
	float lot = groundLot(p, px, mix(soil, GROUND_LOAM, cover), lotColour);
	colour = groundOver(colour, cover, lotColour, lot);
	smoothing = lot * GROUND_LOT_SMOOTHING;
	vec3 roadColour;
	float road = groundRoad(p, px, roadColour);
	colour = groundOver(colour, cover, roadColour, road);
	smoothing = mix(smoothing, 1.0, road);

	float strength = clamp(groundAtlas.z, 0.0, 1.0);
	smoothing *= strength;
	garden = vec4(materials, gardenCover * (1.0 - lot) * (1.0 - road) * strength);
	return cover * strength;
}
