// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "basesphere_uniforms.glsl"
#include "rayleigh-lib.glsl"

uniform int NumShadows;
uniform sampler2D scatterLUT;

#define CLOUD_USE_SCATTER_LUT
#include "cloud-config.glsl"
#include "cloud-uniforms.glsl"
#include "cloud-noise.glsl"
#include "cloud-derive.glsl"
#include "cloud-march.glsl"

in vec4 varyingEyepos;
in vec4 vertexColor;

out vec4 frag_color;

void main(void)
{
    vec4 color = vec4(vertexColor.rgb, 1.0) * 20;

	// premultiplied output, see geosphere_sky.frag
	vec3 atmos = toSRGB(1 - exp(-color.rgb));
	vec3 eyenorm = normalize(varyingEyepos.xyz);
	vec3 camPos = -geosphereCenter;
	CloudResult clouds = cloudSkyClouds(camPos, eyenorm, NumShadows);
	frag_color = vec4(cloudToneMap(clouds.inscatter) + clouds.transmittance * atmos, 1.0 - clouds.transmittance);
	frag_color = cloudSkyDebug(frag_color, camPos, eyenorm);
}
