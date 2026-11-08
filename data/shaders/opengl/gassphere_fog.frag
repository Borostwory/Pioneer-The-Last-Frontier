// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// The fog inside a gas giant's clouds (GasGiant::Render). A gas giant has no ground: going
// down into its cloud tops the view fills with the clouds' own colour, lit as the cloud tops
// there are (gassphere_base.frag), and further down the light fades to black.

#include "attributes.glsl"
#include "lib.glsl"

uniform samplerCube texture0; // the cloud tops (the planet's surface texture)

uniform vec4 FogUp;		// xyz: up at the camera, in view space
uniform vec4 FogCloud;	// xyz: the camera's direction from the planet's centre, in the planet's frame (the texture's)
uniform vec4 FogAmount; // x: how much of the view the fog covers, 0..1; y: how much light gets down this deep, 0..1

out vec4 frag_color;

void main(void)
{
	vec3 up = normalize(FogUp.xyz);

	// lit as gassphere_base.frag lights the cloud tops (CalcPlanetDiffuse, without its eclipses)
	vec3 diff = vec3(0.0);
#if (NUM_LIGHTS > 0)
	for (int i = 0; i < NUM_LIGHTS; ++i) {
		vec3 L = normalize(uLight[i].position.xyz);
		float nDotVP = max(0.0, dot(up, L));
		float nnDotVP = max(0.0, dot(up, -L));
		float clampedCosine = nDotVP + 0.5 * clamp(1.0 - nnDotVP * 4.0, 0.0, 1.0) / float(NUM_LIGHTS);
		diff += uLight[i].diffuse.rgb * 0.5 * clampedCosine;
	}
#endif

	vec3 cloud = texture(texture0, normalize(FogCloud.xyz)).rgb;
	vec3 colour = cloud * (scene.ambient.rgb + diff) * FogAmount.y;

	// premultiplied: the fog covers what is behind it by its amount
	float amount = clamp(FogAmount.x, 0.0, 1.0);
	frag_color = vec4(colour * amount, amount);
}
