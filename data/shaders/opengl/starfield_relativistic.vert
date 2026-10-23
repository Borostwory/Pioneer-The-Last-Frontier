// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "relativity.glsl"

out vec4 v_color;

void main(void)
{
	// work in view space, where the direction of travel is given
	vec3 restDir = normalize(mat3(uViewMatrix) * a_vertex.xyz);

	float doppler;
	vec3 seenDir = aberrate(restDir, u_axisBeta.xyz, u_lorentz.x, u_lorentz.y, doppler);
	gl_Position = uViewProjectionMatrix * (uViewMatrixInverse * vec4(seenDir * 1000.0, 1.0));

	// a point source keeps its blackbody spectrum at T * D, and aberration
	// shrinks the solid angle it covers by D^2
	vec3 restColor = toLinear(a_color.rgb);
	float T = estimateTemperature(restColor);
	vec3 color = dopplerShift(restColor, T, doppler) / (doppler * doppler) * u_lorentz.z;

	// a sprite cannot get brighter than white: spread the extra light over a larger sprite
	float peak = max(max(color.r, color.g), color.b);
	float growth = clamp(sqrt(peak), 1.0, u_cmb.w);
	gl_PointSize = (1.0 + pow(a_normal.z, 3.0)) * growth;
	v_color = vec4(toSRGB(softClip(color / (growth * growth))), a_color.a);
}
