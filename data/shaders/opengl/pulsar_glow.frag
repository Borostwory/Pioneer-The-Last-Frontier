// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// A pulsar's glow, on a quad facing the camera (Star::RenderPulsar): a blinding point,
// a soft blue glow, thin starburst rays and a flat flare line across it.

#include "attributes.glsl"
#include "cloud-noise.glsl"

// position on the quad, -1..1 from its centre; x runs across the screen
in vec2 quadPos;

out vec4 frag_color;

// black-body star colour
uniform vec4 StarColor;
// x: spin angle, y: slow whip angle (radians), z: seconds
uniform vec4 PulsarTime;

// as in star_halo.frag: display-encode the linear black-body colour
const float DISPLAY_GAMMA = 2.2;

void main(void)
{
	float r = length(quadPos);
	// how much of the quad one pixel covers, so the point and the line stay visible from afar
	float px = max(fwidth(r), 1e-6);
	if (r >= 1.0)
		discard;

	vec3 starColor = pow(max(StarColor.rgb, vec3(0.0)), vec3(1.0 / DISPLAY_GAMMA));
	starColor /= max(max(starColor.r, starColor.g), max(starColor.b, 1e-3));
	vec3 glowCol = mix(vec3(0.5, 0.65, 1.0), starColor, 0.3);

	// the blinding point, never smaller than a few pixels
	float coreR = max(0.012, 2.5 * px);
	float core = 2.5 * exp(-r / coreR);

	// soft glow round it
	float glow = 0.6 * exp(-r / 0.06) + 0.18 * pow(1.0 - r, 4.0);

	// starburst: thin rays of uneven length, slowly turning
	float a = atan(quadPos.y, quadPos.x) + PulsarTime.y * 0.02;
	float n = cloudFbm(vec3(cos(a) * 9.0, sin(a) * 9.0, 1.7), 3, 1.0);
	float ray = max(n - 0.4, 0.0) / 0.6;
	float rays = 0.8 * ray * ray * ray * exp(-r / (0.08 + 0.3 * n));

	// the flare line across the screen
	float lineW = max(0.004, 1.2 * px);
	float line = 0.9 * exp(-abs(quadPos.y) / lineW) * (1.0 - smoothstep(0.0, 1.0, abs(quadPos.x)));

	vec3 col = vec3(1.0) * core + glowCol * glow + mix(glowCol, vec3(1.0), 0.5) * (rays + line);

	// fade to nothing at the quad's edge
	float quadFade = 1.0 - smoothstep(0.85, 1.0, r);
	frag_color = vec4(col * quadFade, 1.0);
}
