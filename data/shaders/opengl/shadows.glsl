// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Sun shadows of ships, stations, buildings and terrain (see SceneShadows.h).
// Three shadow-map cascades, from sharp and small to coarse and large.
// Shaders using this declare in their .shaderdef:
//   Texture sampler2D shadowMapNear
//   Texture sampler2D shadowMapMid
//   Texture sampler2D shadowMapFar
//   Buffer ShadowData binding=4
// and dim the direct light of uLight[0] by sceneShadow().

layout(std140) uniform ShadowData {
	mat4 shadowMatrix[3];  // view space -> shadow map coordinates (xy 0..1, z depth)
	vec4 shadowCascade[3]; // x: texel size (m), y: depth per metre
	vec4 shadowState;      // x: 1 while the shadow maps are valid, y: share of the sunlight a shadow removes, z: 1 to tint by cascade
};

uniform sampler2DShadow shadowMapNear;
uniform sampler2DShadow shadowMapMid;
uniform sampler2DShadow shadowMapFar;

// Cascade coordinates of a view-space position. Against surfaces shadowing
// themselves, the lookup moves off the surface along its normal, and toward
// the sun by the depth the surface spans across the filtered texels; both
// grow where the light grazes the surface.
vec3 shadowCoord(int cascade, vec3 pos, vec3 normal, float cosLight)
{
	float texel = shadowCascade[cascade].x;
	float sinLight = sqrt(1.0 - cosLight * cosLight);
	vec3 c = (shadowMatrix[cascade] * vec4(pos + normal * (texel * (0.5 + sinLight)), 1.0)).xyz;
	c.z += texel * (0.2 + 0.5 * min(sinLight / cosLight, 10.0)) * shadowCascade[cascade].y;
	return c;
}

// whether the cascade's map covers c: its area, less a margin, and its depth
// range (below it nothing was drawn, above it the casters were clipped)
bool inCascade(vec3 c, float margin)
{
	return all(greaterThan(c, vec3(margin, margin, 0.0))) && all(lessThan(c, vec3(1.0 - margin, 1.0 - margin, 1.0)));
}

#if (NUM_LIGHTS > 0)
// the finest cascade covering a position (3: none) and its coordinates
int shadowCascadeAt(vec3 pos, vec3 normal, out vec3 c)
{
	float cosLight = clamp(dot(normal, normalize(uLight[0].position.xyz)), 0.05, 1.0);
	c = shadowCoord(0, pos, normal, cosLight);
	if (inCascade(c, 0.02))
		return 0;
	c = shadowCoord(1, pos, normal, cosLight);
	if (inCascade(c, 0.02))
		return 1;
	c = shadowCoord(2, pos, normal, cosLight);
	if (inCascade(c, 0.0))
		return 2;
	return 3;
}

// share of uLight[0] reaching the view-space position pos with geometric normal normal
float sceneShadow(vec3 pos, vec3 normal)
{
	if (shadowState.x < 0.5)
		return 1.0;

	vec3 c;
	float lit = 1.0;
	int cascade = shadowCascadeAt(pos, normal, c);
	if (cascade == 0) {
		lit = texture(shadowMapNear, c);
	} else if (cascade == 1) {
		lit = texture(shadowMapMid, c);
	} else if (cascade == 2) {
		// the far map fades out toward its edge
		vec2 edge = abs(c.xy * 2.0 - 1.0);
		lit = mix(texture(shadowMapFar, c), 1.0, smoothstep(0.8, 1.0, max(edge.x, edge.y)));
	}

	// the sky still lights what the sun doesn't reach
	return mix(1.0, lit, shadowState.y);
}

// debugging (config ShadowDebug=1): tints by the cascade used, near red, mid green, far blue
vec3 sceneShadowTint(vec3 pos, vec3 normal)
{
	if (shadowState.x < 0.5 || shadowState.z < 0.5)
		return vec3(1.0);
	vec3 c;
	int cascade = shadowCascadeAt(pos, normal, c);
	return cascade == 0 ? vec3(1.0, 0.4, 0.4) : cascade == 1 ? vec3(0.4, 1.0, 0.4) : cascade == 2 ? vec3(0.4, 0.4, 1.0) : vec3(1.0);
}
#endif
