// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "relativity.glsl"

uniform samplerCube texture0;
uniform samplerCube snapshot;

in vec3 v_texCoord;

out vec4 frag_color;

// Halo with core radius r0 (pixels) that integrates to 1 over the screen
float halo(float r, float r0)
{
	float q = 1.0 + (r * r) / (r0 * r0);
	return 1.0 / (3.14159265 * r0 * r0 * q * q);
}

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

	// the view all around at the moment of the jump (planets, stations...), captured in
	// view space, is warped the same way and fades out as the ship leaves it behind
	if (u_snapshot.x > 0.0) {
		vec3 snap = toLinear(texture(snapshot, restDir).rgb);
		snap = dopplerShift(snap, estimateTemperature(snap), doppler) * u_lorentz.z;
		color = mix(color, snap, u_snapshot.x);
	}

	// cosmic microwave background, a blackbody of u_cmb.y kelvin at rest. Inside the
	// central disc the hot spot can be smaller than a pixel, so use its integrated flux.
	float angle = 2.0 * asin(clamp(0.5 * length(seenDir - axis), 0.0, 1.0));
	if (angle < u_cmb.z)
		color += u_cmbCore.rgb;
	else
		color += planckRadiance(u_cmb.y * doppler) * whiteBalance() * u_cmb.x;

	// glow around the bright point, standing in for the bloom of an eye or camera
	float r = angle / u_lorentz.w;
	color += u_glow.rgb * (0.10 * halo(r, 2.5) + 0.04 * halo(r, 25.0));

	frag_color = vec4(toSRGB(softClip(color)), 1.0);
}
