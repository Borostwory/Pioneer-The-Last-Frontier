// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "forcefield.glsl"

in vec3 varyingPos;

out vec4 frag_color;

// The field between the struts: a faint film, rippling where it stopped a
// ship, open round ships crossing it
void main(void)
{
	vec3 camPos = (uViewMatrixInverse * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	bool inside = length(camPos - sphere.xyz) < sphere.w;
	float faces = (inside || gl_FrontFacing) ? 1.0 : fill.y;
	float px = length(fwidth(varyingPos)); // size of a pixel here, m

	// the ripples catch the light where they tilt the film towards or
	// away from the eye, and where they are steep
	vec3 slope;
	forcefieldRipple(varyingPos, slope);
	vec3 V = normalize(camPos - varyingPos);
	float ripple = abs(dot(slope, V)) * 1.5 + length(slope) * 0.4;

	vec3 rgb = colour.rgb * (fill.x + ripple * 0.8);
	float a = fill.x + ripple * 0.35;

	float lip;
	float cut = forcefieldCut(varyingPos, px, lip);
	rgb = rgb * cut + colour.rgb * lip;
	a = a * cut + lip * 0.5;

	// premultiplied: more colour than alpha adds light
	frag_color = vec4(min(rgb, vec3(1.0)), clamp(a, 0.0, 1.0)) * faces * fill.z;
}
