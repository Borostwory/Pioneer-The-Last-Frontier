// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "forcefield.glsl"

// a_vertex: a point on the axis of a strut or the middle of a joint (city
// space), a_normal: out from there to its surface, a_uv0.x: its radius in
// struts' radii
out vec3 varyingPos;
out vec3 varyingEyePos;
out vec3 varyingEyeNormal;
out float varyingFarSide;
out float varyingWave;

void main(void)
{
	vec3 camPos = (uViewMatrixInverse * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	vec3 centre = a_vertex.xyz;
	vec3 up = normalize(centre - sphere.xyz);

	// lifted and lowered with the field by its ripples
	vec3 slope;
	centre += up * forcefieldRipple(centre, slope);
	varyingWave = length(slope);

	// never thinner than a fraction of a pixel, so they stay lines from afar
	float radius = max(struts.x, struts.y * length(camPos - centre)) * a_uv0.x;
	vec3 pos = centre + a_normal * radius;

	gl_Position = uViewProjectionMatrix * vec4(pos, 1.0);
	varyingPos = pos;
	varyingEyePos = (uViewMatrix * vec4(pos, 1.0)).xyz;
	varyingEyeNormal = normalize(normalMatrix() * a_normal);
	bool inside = length(camPos - sphere.xyz) < sphere.w;
	varyingFarSide = inside ? 0.0 : smoothstep(0.05, -0.05, dot(up, normalize(camPos - centre)));
}
