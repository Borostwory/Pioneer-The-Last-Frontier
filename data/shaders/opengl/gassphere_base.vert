// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "basesphere_uniforms.glsl"
#include "rayleigh-lib.glsl"

uniform sampler2D densityLUT;
uniform sampler2D scatterLUT;

out vec3 varyingEyepos;
out vec3 varyingNormal;
out vec3 varyingTexCoord0;

// the air between the cloud tops and the viewer, as rayleigh_geosphere_terrain.vert has it for
// the ground (for bodies with Earth's values the old fog in the fragment shader is kept)
out vec3 viewTransmittance;
out vec3 viewInscatter;

void main(void)
{
	gl_Position = matrixTransform();
	varyingEyepos = vec3(uViewMatrix * a_vertex);
	varyingNormal = normalize(normalMatrix() * a_normal);
	varyingTexCoord0 = a_normal.xyz;

	viewTransmittance = vec3(1.0);
	viewInscatter = vec3(0.0);
#if (NUM_LIGHTS > 0)
	if (!atmosEarthValues()) {
		vec3 eyeposScaled = varyingEyepos * geosphereInvRadius;
		vec3 P = eyeposScaled - geosphereCenter;
		vec3 eyenorm = normalize(varyingEyepos);
		float dist = length(varyingEyepos);
		float AU = 149598000000.0;
		// The cloud tops get the sky's own exposure (rayleigh_geosphere_sky_full.frag: x20, times
		// atmosEdgeScale), not the ground's x4. A gas giant has no ground, and towards the horizon
		// both look through the same thick air, so with the same exposure the clouds fade into
		// the sky with no edge. From high above this is x4, as before.
		float exposure = 20.0 * atmosEdgeScale(-geosphereCenter * geosphereRadius);
		for (int i = 0; i < NUM_LIGHTS; ++i) {
			vec3 L = normalize(uLight[i].position.xyz);
			// the falloff the sky and the ground of such bodies get (rayleigh_geosphere_sky_full.frag)
			float intensity = inversesqrt(max(length(uLight[i].position.xyz / AU), 1e-3));
			vec3 lightColor = toLinear(uLight[i].diffuse).xyz * intensity;
			vec2 opticalDistance = vec2(0.0);
			vec3 inscattering = calculateRayInscattering(opticalDistance, P, -eyenorm, dist * geosphereInvRadius, L, lightColor, densityLUT, scatterLUT);
			viewTransmittance = calcTransmittance(opticalDistance);
			viewInscatter += inscattering * exposure;
		}
	}
#endif
}
