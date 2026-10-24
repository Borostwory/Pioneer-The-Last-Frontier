// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Night lights of the city nearest the camera (CityOnPlanet.h): street lamps,
// building and spaceport lights, as point lights in city space (the
// station's frame, metres, y up), lighting the ground and everything on it.
// - near the camera every light reaching a surface is evaluated, from a grid
//   over the city listing the lights of each cell (cityLightGrid)
// - further out, a baked map of the light reaching the ground (cityLightMap)
//   lights the ground, and walls and undersides less
// Shaders using this declare in their .shaderdef:
//   Texture sampler2D cityLightMap
//   Texture sampler2D cityLightGrid
//   Buffer CityLightData binding=5

layout(std140) uniform CityLightData {
	mat4 cityViewToCity; // view space -> city space
	vec4 cityMapRect;	 // xy: x, z of the map's corner in city space, z: 1 / map size, w: grid cell size [m]
	vec4 cityGrid;		 // xy: grid size [cells], z: first index texel, w: first light texel
	vec4 cityState;		 // x: strength (0: no city lights), y: distance [m] out to which lights are evaluated one by one, z: height range [m]
};

uniform sampler2D cityLightMap;
uniform sampler2D cityLightGrid;

// The grid texture (CityOnPlanet::BuildLightGrid) holds, row by row:
// - per cell, two to a texel: its first index (from the first index texel)
//   and number of lights
// - the light indices, four to a texel
// - per light, two texels: position and radius, colour and height above the ground
#define CITY_ATLAS_WIDTH 1024
#define CITY_CELL_MAX_LIGHTS 16

// the map's light on surfaces: walls get this share of the ground's light,
// and it fades over this height above the ground [m]
#define CITY_MAP_WALL 0.5
#define CITY_MAP_HEIGHT 25.0
// light bounced off the lit ground onto surfaces facing down
#define CITY_GROUND_BOUNCE 0.2
#define CITY_BOUNCE_HEIGHT 150.0
// a little of the light around reaching the walls, near and far: the glow
// of the lit streets, windows and the lamps further away, fading over this
// height above the ground [m]
#define CITY_WALL_FILL 0.35
#define CITY_WALL_FILL_HEIGHT 300.0
// the models' lighting is written out as it is, while the terrain's is
// tone mapped (1 - exp(-x)) and sRGB encoded, which brightens dim light:
// the city's light on models is raised to match
#define CITY_MODEL_GAIN 1.6
// share of the city's light models lose where the star lights them
#define CITY_SUNLIT_DIMMING 0.7
// the city's light on the terrain falls on paved ground of this lightness
// (the ground's own, kept within it), so it shows alike on dark grass and
// soil and on bright rock and ice
#define CITY_GROUND_ALBEDO_MIN 0.25
#define CITY_GROUND_ALBEDO_MAX 0.35

float cityComponent(vec4 v, int c)
{
	return c == 0 ? v.x : c == 1 ? v.y : c == 2 ? v.z : v.w;
}

vec4 cityAtlasTexel(int texel)
{
	return texelFetch(cityLightGrid, ivec2(texel % CITY_ATLAS_WIDTH, texel / CITY_ATLAS_WIDTH), 0);
}

// one of the values stored four to a texel
float cityAtlasValue(int i)
{
	return cityComponent(cityAtlasTexel(i / 4), i % 4);
}

// share of a light of radius r reaching distance^2 d2, as the C++ bakes it:
// inverse square, smoothly cut off at the radius
float cityLightFalloff(float d2, float r)
{
	float x = d2 / (r * r);
	float w = clamp(1.0 - x * x, 0.0, 1.0);
	return w * w / (d2 + 1.0);
}

// the lights of the grid cell around pos, one by one (city space)
void cityGridLights(vec3 pos, vec3 n, vec3 V, float shininess, inout vec3 diffuse, inout vec3 specular)
{
	ivec2 cell = ivec2(floor((pos.xz - cityMapRect.xy) / cityMapRect.w));
	ivec2 size = ivec2(cityGrid.xy);
	if (any(lessThan(cell, ivec2(0))) || any(greaterThanEqual(cell, size)))
		return;

	int c = cell.y * size.x + cell.x;
	vec4 headers = cityAtlasTexel(c / 2);
	vec2 header = (c % 2) == 0 ? headers.xy : headers.zw;
	int first = int(cityGrid.z) * 4 + int(header.x);
	int count = int(header.y);
	int lightBase = int(cityGrid.w);
	for (int k = 0; k < CITY_CELL_MAX_LIGHTS; ++k) {
		if (k >= count)
			break;
		int light = lightBase + 2 * int(cityAtlasValue(first + k));
		vec4 a = cityAtlasTexel(light);
		vec3 l = a.xyz - pos;
		float d2 = dot(l, l);
		if (d2 >= a.w * a.w)
			continue;
		vec3 colour = cityAtlasTexel(light + 1).rgb * cityLightFalloff(d2, a.w);
		vec3 L = l * inversesqrt(max(d2, 1e-4));
		diffuse += colour * max(dot(n, L), 0.0);
		if (shininess > 0.0)
			specular += colour * pow(max(dot(n, normalize(L + V)), 0.0), shininess);
	}
}

// Light of the city reaching a surface at view-space position viewPos with
// view-space normal viewN: returns the irradiance (diffuse), and adds the
// Blinn-Phong highlights of the nearby lights to specular when shininess > 0.
vec3 cityLightAt(vec3 viewPos, vec3 viewN, float shininess, inout vec3 specular)
{
	if (cityState.x <= 0.0)
		return vec3(0.0);

	vec3 pos = (cityViewToCity * vec4(viewPos, 1.0)).xyz;
	vec2 uv = (pos.xz - cityMapRect.xy) * cityMapRect.z;
	// before any branch on the position: the map's filtering needs them
	vec2 uvDx = dFdx(uv), uvDy = dFdy(uv);
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || abs(pos.y) > cityState.z)
		return vec3(0.0);

	vec3 n = mat3(cityViewToCity) * viewN;
	float nearShare = 1.0 - smoothstep(0.6 * cityState.y, cityState.y, length(viewPos));

	vec3 E = vec3(0.0);
	if (nearShare > 0.0) {
		vec3 V = mat3(cityViewToCity) * normalize(-viewPos);
		vec3 d = vec3(0.0), s = vec3(0.0);
		cityGridLights(pos, n, V, shininess, d, s);
		E += nearShare * d;
		specular += nearShare * cityState.x * s;
	}

	vec4 m = textureGrad(cityLightMap, uv, uvDx, uvDy);
	// the map holds the light on the ground and, weighted by it, the ground's height
	float lum = dot(m.rgb, vec3(0.3, 0.59, 0.11));
	float h = lum > 1e-6 ? max(pos.y - m.a / lum, 0.0) : 0.0;
	if (nearShare < 1.0)
		E += (1.0 - nearShare) * m.rgb * exp(-h / CITY_MAP_HEIGHT) * mix(CITY_MAP_WALL, 1.0, max(n.y, 0.0));
	E += m.rgb * CITY_GROUND_BOUNCE * max(-n.y, 0.0) * exp(-h / CITY_BOUNCE_HEIGHT);
	E += m.rgb * CITY_WALL_FILL * (1.0 - abs(n.y)) * exp(-h / CITY_WALL_FILL_HEIGHT);

	return E * cityState.x;
}
