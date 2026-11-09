// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "basesphere_uniforms.glsl"
#include "rayleigh-lib.glsl"

uniform int NumShadows;
uniform sampler2D densityLUT;
uniform sampler2D scatterLUT;

// P8 of the OpenGL 4.6 plan: with the camera in the air, the sky's light from the table worked out this frame
// (sky_table.comp; GeoSphere::Render). x: 1 to read it
uniform vec4 SkyTable;
uniform sampler2D skyTable;

#include "sky-table.glsl"

in vec4 varyingEyepos;
in vec4 vertexColor;

out vec4 frag_color;

void main(void)
{
	vec3 eyenorm = normalize(varyingEyepos.xyz);

	vec3 specularHighlight;
	if (SkyTable.x > 0.5) {
		specularHighlight = textureLod(skyTable, skyTableUV(eyenorm), 0.0).rgb;
	} else {
		vec4 sky = skyLightAlong(eyenorm);
		// Invalid ray, skip shading this pixel
		// (can improve performance when spatially coherent)
		if (sky.a == 0.0) {
			frag_color = vec4(0.0);
			return;
		}
		specularHighlight = sky.rgb;
	}

	vec4 color = vec4(specularHighlight.rgb, 1.0) * 20;
	frag_color = toSRGB(1 - exp(-color));
}
