// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "cloud-noise.glsl"

in vec2 quadPos;

out vec4 frag_color;

// black-body star colour
uniform vec4 StarColor;
// x: corona size (star radii), y: streaks, z: prominences, w: Wolf-Rayet envelope (Star::VisualProfile)
uniform vec4 StarHalo;
// x: radius of the star's limb as a fraction of the quad's radius
uniform vec4 StarHaloGeom;
// x, y, z: animation angles in radians (Star::GetAnimationAngles)
uniform vec4 StarTime;

// glow over the whole quad, so the star still shines from afar
const float WIDE_GLOW = 0.3;
// scale height of the prominences, in star radii
const float PROMINENCE_HEIGHT = 0.12;
// scale height of the Wolf-Rayet wind envelope, in star radii
const float ENVELOPE_HEIGHT = 6.0;
// as in geosphere_star.frag: display-encode the linear black-body colour
const float DISPLAY_GAMMA = 2.2;
// the halo is drawn over the star: how far its glow reaches in across the limb (star radii),
// and how strongly it washes over the whole disc, softening the fine detail
const float BLOOM_INWARD = 0.12;
const float BLOOM_VEIL = 0.12;

vec2 rotate2(vec2 v, float a)
{
	float c = cos(a), s = sin(a);
	return vec2(c * v.x - s * v.y, s * v.x + c * v.y);
}

void main(void)
{
	vec3 starColor = pow(max(StarColor.rgb, vec3(0.0)), vec3(1.0 / DISPLAY_GAMMA));

	float r = length(quadPos);
	if (r >= 1.0)
		discard;
	vec2 dir = r > 1e-5 ? quadPos / r : vec2(0.0, 1.0);

	// height above the limb in star radii, negative over the disc
	float heightRaw = r / StarHaloGeom.x - 1.0;
	float h = max(heightRaw, 0.0);
	float outside = step(0.0, heightRaw);
	// how many star radii one pixel covers, to drop detail too fine to show
	float pixelHeight = fwidth(heightRaw);

	// fade to nothing at the quad's edge
	float quadFade = 1.0 - smoothstep(0.85, 1.0, r);

	// corona: exponential falloff, broken into slowly turning radial streaks;
	// over the disc it becomes bloom fading inwards from the limb
	float corona = heightRaw >= 0.0 ? exp(-h / max(StarHalo.x, 1e-3)) : exp(heightRaw / BLOOM_INWARD);
	float radial = log(1.0 + h) * 2.0;
	float s1 = cloudFbm(vec3(rotate2(dir, StarTime.x * 0.5) * 4.0, radial), 4, 1.0);
	float s2 = cloudFbm(vec3(rotate2(dir, -StarTime.y) * 7.0, radial * 1.5 + 5.0), 3, 1.0);
	float streaks = mix(1.0, 0.2 + 3.2 * s1 * s2, StarHalo.y * smoothstep(0.0, 0.3, h));
	// whiter just above the limb
	vec3 coronaColor = mix(starColor, vec3(1.0), 0.35 * exp(-h / 0.08));
	vec3 col = coronaColor * corona * streaks * 0.9;

	col += starColor * WIDE_GLOW * (1.0 - r) * (1.0 - r);
	col += starColor * BLOOM_VEIL * (1.0 - outside);

	// prominences: flame-like tongues at a few sites around the limb
	if (StarHalo.z > 0.0 && heightRaw > 0.0 && h < 0.6) {
		float site = cloudFbm(vec3(rotate2(dir, StarTime.z * 4.0) * 2.0, 7.3), 3, 1.0);
		float mask = smoothstep(0.52, 0.66, site);
		// two layers turning opposite ways, so the flames change shape as they move
		float flames = 0.5 * (cloudFbm(vec3(rotate2(dir, StarTime.x) * 18.0, h * 10.0), 4, 1.0)
			+ cloudFbm(vec3(rotate2(dir, -StarTime.x * 1.3) * 18.0, h * 10.0 + 3.1), 4, 1.0));
		// taller flames need stronger noise, so they taper upwards
		float tongue = smoothstep(0.5 + h * 1.5, 0.62 + h * 1.5, flames);
		float visible = 1.0 - smoothstep(0.02, 0.06, pixelHeight);
		float prominence = StarHalo.z * mask * tongue * exp(-h / PROMINENCE_HEIGHT) * visible;
		col += pow(starColor, vec3(1.8)) * 1.6 * prominence;
	}

	// Wolf-Rayet stars: a thick, turbulent, deeply coloured wind envelope
	if (StarHalo.w > 0.0) {
		float turbulence = cloudFbm(vec3(rotate2(dir, StarTime.y) * 3.0, log(1.0 + h) * 1.5), 5, 1.0);
		// thinner where it is seen in front of the disc
		float envelope = StarHalo.w * exp(-h / ENVELOPE_HEIGHT) * mix(0.4, 1.6, turbulence) * mix(0.5, 1.0, outside);
		col += pow(starColor, vec3(2.5)) * 1.3 * envelope;
	}

	frag_color = vec4(col * quadFade, 1.0);
}
