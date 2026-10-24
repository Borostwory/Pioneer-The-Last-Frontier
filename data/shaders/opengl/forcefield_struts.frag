// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "forcefield.glsl"

in vec3 varyingPos;
in vec3 varyingEyePos;
in vec3 varyingEyeNormal;
in float varyingFarSide;
in float varyingWave;

out vec4 frag_color;

// Struts and joints as tubes of light: glowing, lit by the stars, brighter
// round their edges and where the ripples run along them
void main(void)
{
	float px = length(fwidth(varyingPos));
	float lip;
	float cut = forcefieldCut(varyingPos, px, lip);
	if (cut <= 0.0)
		discard;

	vec3 N = normalize(varyingEyeNormal);
	vec3 V = normalize(-varyingEyePos);
	vec3 lit = vec3(0.0);
	vec3 shine = vec3(0.0);
#if (NUM_LIGHTS > 0)
	for (int i = 0; i < NUM_LIGHTS; ++i) {
		vec3 L = normalize(uLight[i].position.xyz);
		lit += uLight[i].diffuse.rgb * max(dot(N, L), 0.0);
		shine += uLight[i].specular.rgb * pow(max(dot(N, normalize(L + V)), 0.0), 40.0);
	}
#endif
	float rim = pow(1.0 - abs(dot(N, V)), 2.0);

	vec3 rgb = colour.rgb * (0.7 + 0.4 * lit + 0.4 * rim + varyingWave) + shine * 0.4;
	float a = colour.a * (0.75 + 0.25 * rim) * cut * mix(1.0, fill.y, varyingFarSide) * fill.z;

	// premultiplied
	frag_color = vec4(min(rgb, vec3(1.0)) * a, a);
}
