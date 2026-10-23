// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "relativity.glsl"

uniform samplerCube texture0;

in vec3 v_texCoord;

out vec4 frag_color;

void main(void)
{
	vec3 axis = u_axisBeta.xyz;
	vec3 seenDir = normalize(mat3(uViewMatrix) * v_texCoord);

	// undo the aberration to find where this light came from in the rest frame;
	// the factor returned for the reversed motion is 1 / D
	float invDoppler;
	vec3 restDir = aberrate(seenDir, -axis, u_lorentz.x, u_lorentz.y, invDoppler);
	float doppler = 1.0 / invDoppler;

	// diffuse light: a blackbody of temperature T is seen as one of temperature T * D
	vec3 restColor = toLinear(texture(texture0, mat3(uViewMatrixInverse) * restDir).rgb * material.emission.rgb * 0.8);
	vec3 color = dopplerShift(restColor, estimateTemperature(restColor), doppler) * u_lorentz.z;

	// cosmic microwave background, a blackbody of u_cmb.y kelvin at rest. Inside the
	// central disc the hot spot can be smaller than a pixel, so use its integrated flux.
	float angle = 2.0 * asin(clamp(0.5 * length(seenDir - axis), 0.0, 1.0));
	if (angle < u_cmb.z)
		color += u_cmbCore.rgb;
	else
		color += planckRadiance(u_cmb.y * doppler) * whiteBalance() * u_cmb.x;

	frag_color = vec4(toSRGB(softClip(color)), 1.0);
}
