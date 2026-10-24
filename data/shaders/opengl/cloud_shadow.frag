// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Cloud shadows on the ground, in screen space at reduced resolution: each
// pixel finds the scene point (terrain, cities, ships) from the scene
// distance and computes the light reaching it through the clouds. The
// composite pass darkens the scene with it. Pixels that are not on or near
// this planet's surface (sky, space, other bodies) get 1.
// Output: r = light factor 0..1.

#include "attributes.glsl"
#include "lib.glsl"
#include "cloud-pass-uniforms.glsl"

#define atmosColor cloudPassAtmos

#include "cloud-config.glsl"
#include "cloud-uniforms.glsl"
#include "cloud-noise.glsl"
#include "cloud-derive.glsl"
#include "cloud-march.glsl"
#include "city-lights.glsl"
#include "city-glow.glsl"

uniform sampler2D sceneDist;

// the city glow's light as if the ground it seems to light had this albedo
#define CITY_GLOW_ALBEDO 0.25

#if (NUM_LIGHTS > 0)
// Starlight on level ground (luminance), dimmed by the distance to the star
// and the air above, as Camera::CityDarkness() works it out.
float cityDaylight(vec3 up)
{
	float airMass = cloudAtmos.z / 1.225;
	float daylight = 0.0;
	for (int i = 0; i < NUM_LIGHTS; ++i) {
		vec3 toStar = uLight[i].position.xyz;
		float sunUp = dot(up, normalize(toStar));
		if (sunUp <= 0.0)
			continue;
		float star = dot(uLight[i].diffuse.rgb, vec3(0.3, 0.59, 0.11)) * 149598000000.0 / length(toStar);
		daylight += star * sunUp * exp(-0.22 * airMass / max(sunUp, 0.05));
	}
	return daylight;
}
#endif

out vec4 frag_color;

void main(void)
{
	frag_color = vec4(1.0);
#if (NUM_LIGHTS > 0)
	// shadow buffer size: cloudPassHistSize.zw
	vec2 fullPos = (floor(gl_FragCoord.xy) + 0.5) * cloudPassSize.zw / cloudPassHistSize.zw;
	ivec2 ip = ivec2(min(fullPos, cloudPassSize.zw - 1.0));
	float viewZ = texelFetch(sceneDist, ip, 0).r;
	if (viewZ > 1e20) return;

	vec2 ndc = fullPos / cloudPassSize.zw * 2.0 - 1.0;
	vec3 dirV = vec3(ndc.x / cloudPassProj.x, ndc.y / cloudPassProj.y, -1.0);
	float a = max(cloudGeom.x, 1.0);
	float dist = viewZ * length(dirV); // [m]
	vec3 pos = -cloudPassCenter.xyz + normalize(dirV) * (dist / a);

	// only on (or just above) this planet's surface
	float alt = (length(pos) - 1.0) * a;
	if (alt < -0.01 * a || alt > cloudGeom.w + 3000.0) return;

	vec3 L = normalize(uLight[0].position.xyz);
	float sunUp = dot(normalize(pos), L);
	if (sunUp < -0.05) return; // night side: nothing to shadow (and city lights stay lit)

	CloudPlanet cp = cloudDerive();
	float pixelAngle = 2.0 / cloudPassProj.y / cloudPassHistSize.w;
	float footprint = max(dist * pixelAngle, 1.0);
	float s = cloudShadowTerrain(cp, pos, L, footprint);
	s = mix(1.0, s, smoothstep(-0.05, 0.05, sunUp));

	// the clouds take away sunlight only: not the city lights, which light
	// the ground under them too (the composite darkens the whole scene)
	vec3 up = normalize(pos);
	float sun = cityDaylight(up);
	vec3 citySpecular = vec3(0.0);
	vec3 viewPos = normalize(dirV) * dist;
	float city = dot(cityLightAt(viewPos, up, 0.0, citySpecular), vec3(0.3, 0.59, 0.11));
	city += dot(cityGlowAt(up, a, footprint, sun), vec3(0.3, 0.59, 0.11)) / CITY_GLOW_ALBEDO;
	frag_color = vec4(mix(1.0, s, sun / max(sun + city, 1e-6)));
#endif
}
