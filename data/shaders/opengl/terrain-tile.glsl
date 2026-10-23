// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Terrain detail tiles: the landforms between a patch's mesh and a quarter
// of its vertex spacing, baked once per patch on the GPU (TerrainTiles in
// the C++, terrain_tile.frag) into an atlas, and sampled by the terrain
// shaders for relief and materials.
//
// Detail comes in octaves of planet-fixed noise with a fixed ladder of
// wavelengths, so neighbouring tiles agree. Four styles, mixed per planet:
// rolling (eroded fBm), ridged, dunes and craters.
//
// A tile texel holds the slope of the detail (planet-fixed, along the
// surface) and a landform value: high on crests, ridges and crater rims,
// low in hollows and crater floors.

// slopes are stored divided by this, mapped from [-1, 1] to [0, 1]
#define TILE_SLOPE_RANGE 1.5

// gradient noise periodic over 256 cells, with analytic derivatives:
// (value, d/dx, d/dy, d/dz); salt picks an independent noise
vec3 tileGrad(vec3 c, float salt)
{
	vec3 p = mod(c, 256.0) + salt * vec3(17.0, 59.0, 113.0);
	p = fract(p * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yxz + 33.33);
	return fract((p.xxy + p.yxx) * p.zyx) * 2.0 - 1.0;
}

vec4 tileNoised(vec3 x, float salt)
{
	vec3 i = floor(x);
	vec3 f = x - i;
	vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	vec3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);

	vec3 ga = tileGrad(i + vec3(0, 0, 0), salt);
	vec3 gb = tileGrad(i + vec3(1, 0, 0), salt);
	vec3 gc = tileGrad(i + vec3(0, 1, 0), salt);
	vec3 gd = tileGrad(i + vec3(1, 1, 0), salt);
	vec3 ge = tileGrad(i + vec3(0, 0, 1), salt);
	vec3 gf = tileGrad(i + vec3(1, 0, 1), salt);
	vec3 gg = tileGrad(i + vec3(0, 1, 1), salt);
	vec3 gh = tileGrad(i + vec3(1, 1, 1), salt);

	float va = dot(ga, f - vec3(0, 0, 0));
	float vb = dot(gb, f - vec3(1, 0, 0));
	float vc = dot(gc, f - vec3(0, 1, 0));
	float vd = dot(gd, f - vec3(1, 1, 0));
	float ve = dot(ge, f - vec3(0, 0, 1));
	float vf = dot(gf, f - vec3(1, 0, 1));
	float vg = dot(gg, f - vec3(0, 1, 1));
	float vh = dot(gh, f - vec3(1, 1, 1));

	float v = va + u.x * (vb - va) + u.y * (vc - va) + u.z * (ve - va) +
		u.x * u.y * (va - vb - vc + vd) + u.y * u.z * (va - vc - ve + vg) +
		u.z * u.x * (va - vb - ve + vf) + (-va + vb + vc - vd + ve - vf - vg + vh) * u.x * u.y * u.z;
	vec3 d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.z * (ge - ga) +
		u.x * u.y * (ga - gb - gc + gd) + u.y * u.z * (ga - gc - ge + gg) +
		u.z * u.x * (ga - gb - ge + gf) + (-ga + gb + gc - gd + ge - gf - gg + gh) * u.x * u.y * u.z +
		du * (vec3(vb, vc, ve) - va +
			u.yzx * vec3(va - vb - vc + vd, va - vc - ve + vg, va - vb - ve + vf) +
			u.zxy * vec3(va - vb - ve + vf, va - vb - vc + vd, va - vc - ve + vg) +
			u.yzx * u.zxy * (-va + vb + vc - vd + ve - vf - vg + vh));
	return vec4(v, d);
}

vec3 tileHash33(vec3 c, float salt)
{
	return tileGrad(c, salt + 7.0) * 0.5 + 0.5;
}

// One octave of each style at q (in cells of the octave). Returns the
// gradient of the detail height in cells per cell (= slope, as heights are
// in cells too) in xyz, and the landform value (-1..1) in w.

vec4 tileRolling(vec3 q, float salt, inout vec3 accumulated)
{
	vec4 n = tileNoised(q, salt);
	// eroded: damped where the larger octaves are steep
	accumulated += n.yzw;
	float damp = 1.0 / (1.0 + 0.6 * dot(accumulated, accumulated));
	return vec4(n.yzw * 0.45 * damp, n.x * 1.4 * damp);
}

vec4 tileRidged(vec3 q, float salt)
{
	vec4 n = tileNoised(q, salt + 31.0);
	float r = 1.0 - min(abs(n.x) * 1.6, 1.0);
	// ridge height r^2: sharp crests, wide valleys
	vec3 g = 2.0 * r * (-sign(n.x) * 1.6) * n.yzw;
	return vec4(g * 0.09, r * r * 2.0 - 1.0);
}

vec4 tileDunes(vec3 q, float salt)
{
	// crests along the contours of a smooth noise: sinuous, roughly parallel
	vec4 n = tileNoised(q * 0.5, salt + 61.0);
	float phase = 6.2831853 * 2.5 * n.x;
	vec3 dphase = 6.2831853 * 2.5 * 0.5 * n.yzw;
	// asymmetric profile: steep lee side
	float s = 0.5 + 0.5 * sin(phase);
	float h = s * s;
	vec3 g = 2.0 * s * 0.5 * cos(phase) * dphase;
	return vec4(g * 0.075, h * 2.0 - 1.0);
}

vec4 tileCraters(vec3 q, float salt, float density)
{
	vec3 cell = floor(q);
	vec3 grad = vec3(0.0);
	float form = 0.0;
	for (int z = -1; z <= 1; ++z)
		for (int y = -1; y <= 1; ++y)
			for (int x = -1; x <= 1; ++x) {
				vec3 c = cell + vec3(x, y, z);
				vec3 h = tileHash33(c, salt + 97.0);
				if (h.x > density)
					continue;
				vec3 centre = c + 0.25 + 0.5 * tileHash33(c, salt + 131.0);
				float radius = 0.18 + 0.3 * h.y;
				vec3 d = q - centre;
				float dist = length(d);
				float x01 = dist / radius;
				if (x01 > 1.6 || dist < 1e-5)
					continue;
				// bowl inside, raised rim around the edge, fading outside
				float depth = 0.22 * radius, rim = 0.08 * radius;
				float bowl = x01 < 1.0 ? depth * (x01 * x01 - 1.0) : 0.0;
				float dbowl = x01 < 1.0 ? depth * 2.0 * x01 : 0.0;
				float e = (x01 - 1.0) / 0.25;
				float rimH = rim * exp(-e * e);
				float drim = rimH * (-2.0 * e / 0.25);
				// height in cells, its derivative along the radius per cell
				float dh = (dbowl + drim) / radius;
				grad += dh * d / dist;
				form += (bowl + rimH) / (0.1 * radius);
			}
	return vec4(grad * 1.2, clamp(form, -1.0, 1.0));
}
