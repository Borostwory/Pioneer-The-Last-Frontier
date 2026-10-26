// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "cloud-noise.glsl"

in vec4 vertexColor;
in vec3 varyingEyepos;
in vec3 varyingNormal;
in vec3 varyingSpherePos;

out vec4 frag_color;

// x: granule frequency, y: granule contrast, z: sunspot amount, w: baked texture weight (Star::VisualProfile)
uniform vec4 StarSurface;
// x, y, z: animation angles in radians (Star::GetAnimationAngles)
uniform vec4 StarTime;

// linear limb darkening coefficient, I(mu) = 1 - u * (1 - mu); the Sun is ~0.6 in the visible
const float LIMB_DARKENING = 0.6;
// how far the disc centre is pushed towards white
const float CORE_WHITENING = 0.35;
// overall disc brightness: above 1 the bright centre saturates, standing in for the missing bloom
const float SURFACE_BRIGHTNESS = 1.3;
// the limb is cooler, so it shifts towards red as well as darkening
const vec3 LIMB_WARMTH = vec3(1.0, 0.85, 0.7);
// sunspot umbrae are cooler too
const vec3 SPOT_COLOUR = vec3(1.0, 0.75, 0.55);
// how much the penumbra and umbra darken the surface, and how much of SPOT_COLOUR the umbra takes
const float SPOT_PENUMBRA = 0.12;
const float SPOT_UMBRA = 0.18;
const float SPOT_TINT = 0.5;
// the black-body colour is linear; encode it for display, or hot stars come out deep blue and cool ones deep red
const float DISPLAY_GAMMA = 2.2;
// sunspots stay below this |sin(latitude)|, like the Sun's activity belts
const float SPOT_LATITUDE = 0.6;

// rotation about the star's pole (y) and about x
mat3 rotateY(float a)
{
	float c = cos(a), s = sin(a);
	return mat3(c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c);
}

mat3 rotateX(float a)
{
	float c = cos(a), s = sin(a);
	return mat3(1.0, 0.0, 0.0, 0.0, c, s, 0.0, -s, c);
}

// bright cell centres and dark lanes, [0, 1]
float granules(vec3 q)
{
	return 1.0 - smoothstep(0.15, 0.95, cloudWorley(q));
}

void main(void)
{
	// unlit rendering - stars
	// material.emission holds the black-body star colour (brightest channel = 1),
	// vertexColor holds the baked surface noise from the TerrainColorStar* generators
	vec3 starColor = pow(max(material.emission.rgb, vec3(0.0)), vec3(1.0 / DISPLAY_GAMMA));
	vec3 p = normalize(varyingSpherePos);

	// convection cells, warped by a slowly turning noise field so they churn
	// (rotations wrap, so the animation has no seams)
	float frequency = StarSurface.x;
	vec3 q = p * frequency;
	vec3 wq = rotateY(StarTime.x) * p * (frequency * 0.5);
	vec3 warp = vec3(cloudGradNoise(wq),
		cloudGradNoise(wq + vec3(17.3, 5.1, 9.7)),
		cloudGradNoise(wq + vec3(41.7, 23.9, 3.3))) * 0.45;

	// fade the cells out before they get smaller than a few pixels and shimmer
	float cellsPerPixel = length(fwidth(q));
	float fade1 = 1.0 - smoothstep(0.35, 0.9, cellsPerPixel);
	float fade2 = 1.0 - smoothstep(0.35, 0.9, cellsPerPixel * 3.1);

	float g = 0.0;
	if (fade1 > 0.0)
		g += (granules(q + warp) - 0.6) * fade1;
	// finer cells for close views
	if (fade2 > 0.0)
		g += 0.5 * (granules(q * 3.1 + warp * 2.0) - 0.6) * fade2;
	float surface = 1.0 + StarSurface.y * g;

	// large-scale brightness flow
	surface *= mix(0.85, 1.1, cloudFbm(rotateX(StarTime.y) * p * 2.5, 4, 1.0));

	// sunspots: umbra and penumbra, drifting with the star's rotation
	float spot = 0.0;
	if (StarSurface.z > 0.0) {
		float spotNoise = cloudFbm(rotateY(StarTime.z) * p * 5.0, 5, 1.0);
		float threshold = 0.72 - 0.12 * StarSurface.z;
		float penumbra = smoothstep(threshold - 0.05, threshold, spotNoise);
		float umbra = smoothstep(threshold, threshold + 0.04, spotNoise);
		float belt = 1.0 - smoothstep(SPOT_LATITUDE - 0.15, SPOT_LATITUDE, abs(p.y));
		float strength = belt * min(1.0, StarSurface.z * 3.0);
		surface *= 1.0 - (SPOT_PENUMBRA * penumbra + SPOT_UMBRA * umbra) * strength;
		spot = umbra * strength * SPOT_TINT;
	}

	// the baked texture's (fixed) hues are ignored, only its brightness is used
	float baked = dot(vertexColor.rgb, vec3(0.2126, 0.7152, 0.0722));
	surface *= mix(1.0, mix(0.55, 1.15, baked), StarSurface.w);

	// cosine of the angle between the surface normal and the view direction:
	// 1 at the centre of the disc, 0 at the limb
	vec3 eyeDir = normalize(-varyingEyepos);
	float mu = clamp(dot(normalize(varyingNormal), eyeDir), 0.0, 1.0);

	// deeper, more saturated and warmer colour towards the limb
	vec3 tint = pow(starColor, vec3(mix(1.0, 2.0, 1.0 - mu)));
	tint *= mix(vec3(1.0), LIMB_WARMTH, 1.0 - mu);
	tint *= mix(vec3(1.0), SPOT_COLOUR, spot);

	// white-hot core at the centre of the disc, strongest over bright granules
	tint = mix(tint, vec3(1.0), CORE_WHITENING * mu * mu * clamp(surface, 0.0, 1.0));

	float limb = 1.0 - LIMB_DARKENING * (1.0 - mu);

	frag_color = vec4(tint * surface * SURFACE_BRIGHTNESS * limb, 1.0);
}
