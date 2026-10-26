// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Spotlights of the ships near the camera that have their lights on
// (Ship::SetLightsOn, Camera::SetShipLights), lighting the ground and
// everything on it.
// Shaders using this declare in their .shaderdef:
//   Buffer ShipLightData binding=8

#define SHIP_MAX_LIGHTS 8

layout(std140) uniform ShipLightData {
	vec4 shipLightPos[SHIP_MAX_LIGHTS];	   // xyz: view-space position, w: range [m]
	vec4 shipLightDir[SHIP_MAX_LIGHTS];	   // xyz: view-space beam direction, w: cosine of the cone's outer edge
	vec4 shipLightColour[SHIP_MAX_LIGHTS]; // rgb: intensity (irradiance d metres down the beam: rgb / (d^2 + 1)), w: cosine of the cone's inner edge
	vec4 shipLightState;				   // x: number of lights
};

// the beam starts this far out of the lamp [m], so it leaves the hull it
// is mounted in dark
#define SHIP_LIGHT_NEAR 1.5

// Light of the ships' lamps reaching a surface at view-space position
// viewPos with view-space normal viewN: returns the irradiance (diffuse),
// and adds the Blinn-Phong highlights to specular when shininess > 0.
vec3 shipLightAt(vec3 viewPos, vec3 viewN, float shininess, inout vec3 specular)
{
	int count = int(shipLightState.x);
	if (count <= 0)
		return vec3(0.0);

	vec3 V = normalize(-viewPos);
	vec3 E = vec3(0.0);
	for (int i = 0; i < SHIP_MAX_LIGHTS; ++i) {
		if (i >= count)
			break;
		vec3 l = shipLightPos[i].xyz - viewPos;
		float d2 = dot(l, l);
		float r = shipLightPos[i].w;
		if (d2 >= r * r)
			continue;
		vec3 L = l * inversesqrt(max(d2, 1e-4));
		float cone = smoothstep(shipLightDir[i].w, shipLightColour[i].w, dot(-L, shipLightDir[i].xyz));
		if (cone <= 0.0)
			continue;
		float nDotL = dot(viewN, L);
		if (nDotL <= 0.0)
			continue;
		float nearFade = smoothstep(0.5 * SHIP_LIGHT_NEAR, SHIP_LIGHT_NEAR, sqrt(d2));
		// (as city lights: inverse square, smoothly cut off at the range)
		float x = d2 / (r * r);
		float w = clamp(1.0 - x * x, 0.0, 1.0);
		vec3 colour = shipLightColour[i].rgb * (cone * nearFade * w * w / (d2 + 1.0));
		E += colour * nDotL;
		if (shininess > 0.0)
			specular += colour * pow(max(dot(viewN, normalize(L + V)), 0.0), shininess);
	}
	return E;
}
