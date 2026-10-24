// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// A city's forcefield (CityOnPlanet::RenderForcefields): its field and its
// struts. As ForcefieldDataBlock in CityOnPlanet.cpp; positions in city space

layout(std140) uniform ForcefieldData {
	vec4 sphere;      // centre, radius
	vec4 openings[8]; // centre on the field, radius (0 = none)
	vec4 impacts[4];  // point on the field, age 0..1 (< 0 = none)
	vec4 impactReach; // m
	vec4 struts;      // radius (m), at least this much per metre away (a fraction of a pixel)
	vec4 colour;      // rgb, strut opacity
	vec4 fill;        // fill, the far side seen from outside, fade with distance
};

// Ripples running out from where it stopped ships, like a stone dropped in
// water: the height of the field along its normal (m), and its slope (the
// gradient of that height) in slope
float forcefieldRipple(in vec3 p, out vec3 slope)
{
	float h = 0.0;
	slope = vec3(0.0);
	for (int i = 0; i < 4; i++) {
		float t = impacts[i].w;
		if (t < 0.0)
			continue;
		float reach = impactReach[i];
		vec3 dv = p - impacts[i].xyz;
		float d = length(dv);
		float wavelength = reach * 0.25;
		float k = 6.2832 / wavelength;
		// behind the front, waves dying away towards the middle
		float x = t * reach - d;
		float env = smoothstep(-0.5 * wavelength, 0.0, x) * exp(-max(x, 0.0) / (1.5 * wavelength));
		float amp = reach * 0.03 * (1.0 - t) * (1.0 - t) / (1.0 + d / (0.3 * reach));
		h += amp * env * sin(k * x);
		slope -= dv / max(d, 0.001) * (amp * env * k * cos(k * x));
	}
	return h;
}

// 0 inside the openings round ships crossing it, 1 elsewhere; px is the size
// of a pixel there (m), lip the glow at their edges
float forcefieldCut(in vec3 p, in float px, out float lip)
{
	float cut = 1.0;
	lip = 0.0;
	for (int i = 0; i < 8; i++) {
		float r = openings[i].w;
		if (r <= 0.0)
			continue;
		float d = length(p - openings[i].xyz);
		cut *= smoothstep(r - px, r + px, d);
		lip += exp(-pow((d - r) / (0.03 * r + px), 2.0)) + 0.3 * exp(-pow((d - r * 1.15) / (0.02 * r + px), 2.0));
	}
	return cut;
}
