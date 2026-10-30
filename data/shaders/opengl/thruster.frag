// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"

uniform sampler2D texture0; //glow halo

in vec2 texCoord0;
in vec3 face_norm;
in float is_glow;
in float power;
in float flicker;
in float phase;
in float alpha;

out vec4 frag_color;

const float TAU = 6.2831853;

// Low power: loose, turbulent yellow/orange flame
const vec3 LOW_HOT = vec3(1.0, 0.82, 0.38);
const vec3 LOW_COOL = vec3(1.0, 0.38, 0.06);
// Half power: white-hot cone inside an orange sheath
const vec3 MID_CORE = vec3(1.0, 0.9, 0.85);
const vec3 MID_SHEATH = vec3(1.0, 0.42, 0.18);
// Full power: blue jet (material diffuse) with shock diamonds
const vec3 DIAMOND_NEAR = vec3(1.0, 0.6, 0.55);
const vec3 DIAMOND_FAR = vec3(1.0, 0.8, 0.4);
const float DIAMOND_COUNT = 7.0;

float map_percentage(float val, float base_percent)
{
	return base_percent + val * (1.0 - base_percent);
}

// 1 on the axis, fading to 0 at radius R (soft from inner*R outwards)
float envelope(float r, float R, float inner)
{
	return 1.0 - smoothstep(R * inner, R, r);
}

vec3 lowFlame(float t, float r)
{
	// periodic in phase so the 0..1 wrap is seamless
	float turb = sin(t * 11.0 - TAU * 2.0 * phase + r * 4.0) * sin(t * 7.0 - TAU * phase - r * 6.0 + 1.3);
	float R = (0.45 + 0.45 * t) * (1.0 + 0.15 * turb);
	float env = pow(1.0 - clamp(r / R, 0.0, 1.0), 1.2);
	float fade = pow(1.0 - t, 1.5);
	float lumps = 0.75 + 0.25 * turb;
	vec3 col = mix(LOW_HOT, LOW_COOL, clamp(t * 1.5 + r * 0.6, 0.0, 1.0));
	return col * env * fade * lumps * 1.8;
}

vec3 midFlame(float t, float r)
{
	// bright cone tapering from the nozzle to a point
	const float coneLen = 0.7;
	float Rc = 0.4 * (1.0 - t / coneLen);
	float core = (Rc > 0.0) ? envelope(r, Rc, 0.15) * (0.6 + 0.4 * Rc / 0.4) : 0.0;
	float sheath = envelope(r, 0.5 + 0.1 * t, 0.2) * pow(1.0 - t, 1.2);
	return MID_CORE * core * 1.2 + MID_SHEATH * sheath * 0.7;
}

vec3 highFlame(float t, float r, vec3 tint)
{
	// long, slightly converging blue column
	float Rb = 0.38 - 0.08 * t;
	vec3 jet = tint * envelope(r, Rb, 0.0) * pow(1.0 - t, 0.8) * 1.1;

	// shock diamonds: a train of soft oval hot spots along the axis
	float c = t * DIAMOND_COUNT - 0.35 + (flicker - 0.5) * 0.04;
	float id = floor(c);
	float f = fract(c);
	float Rd = 0.28 * (1.0 - 0.5 * t);
	float q = length(vec2(r / Rd, (f - 0.5) / 0.35));
	float diamond = (1.0 - smoothstep(0.2, 1.0, q)) * step(0.0, c) * (1.0 - t);
	vec3 dcol = mix(DIAMOND_NEAR, DIAMOND_FAR, clamp(id / DIAMOND_COUNT * 1.5, 0.0, 1.0));

	return jet + dcol * diamond * 1.1;
}

void main(void)
{
	float wLow = 1.0 - smoothstep(0.2, 0.5, power);
	float wHigh = smoothstep(0.6, 0.9, power);
	float wMid = max(0.0, 1.0 - wLow - wHigh);

	vec3 tint = material.diffuse.rgb;
	float brightness = map_percentage(power, 0.5) * map_percentage(flicker, 0.95);

	if (is_glow > 0.5) {
		// glow cards seen when looking down the nozzle, coloured to match the flame
		vec4 tex = texture(texture0, texCoord0);
		vec3 glowCol = wLow * vec3(1.0, 0.6, 0.2) + wMid * vec3(1.0, 0.85, 0.7) + wHigh * mix(tint, vec3(1.0), 0.35);
		frag_color = vec4(tex.rgb * glowCol * brightness, tex.a * alpha);
		return;
	}

	// t: 0 at the nozzle, 1 at the flame tip; r: 0 on the axis, 1 at the plane edge
	float t = 1.0 - texCoord0.y;
	float r = abs(texCoord0.x * 2.0 - 1.0);

	vec3 light = vec3(0.0);
	if (wLow > 0.0) light += wLow * lowFlame(t, r);
	if (wMid > 0.0) light += wMid * midFlame(t, r);
	if (wHigh > 0.0) light += wHigh * highFlame(t, r, tint);

	vec4 color = vec4(light * brightness, alpha);

	// Thruster volumetrics: fade thruster planes out to 50% intensity
	// as they approach edge-on to the camera. Still appears 3d with
	// less strong artifacts from each individual plane.
	color.a *= 0.5 + 0.5 * abs(dot(face_norm, vec3(0, 0, -1)));

	frag_color = color;
}
