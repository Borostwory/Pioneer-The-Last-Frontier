// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// A pulsar's two beams, painted on one strip that runs along the magnetic axis through the star
// (Star::RenderPulsar), which sweeps round the spin axis once a turn. Two strands of glowing gas
// are wound round each other and turn with the star's real spin; the beam fades out towards its end.
// When the beams sweep faster than the eye can follow, the strip fades and pulsar_cone.frag shows
// the smear instead.

#include "attributes.glsl"
#include "cloud-noise.glsl"

// position on the strip, -1..1: x across the jets, y along the spin axis (0 at the star, +-1 at the tips)
in vec2 quadPos;

out vec4 frag_color;

// black-body star colour
uniform vec4 StarColor;
// x: spin angle (radians), y: brightness (lower when the beam smears into a cone), z: seconds, for the
// outflow, w: the strip's half width in jet lengths
uniform vec4 PulsarTime;

// (PI is defined for every shader, see graphics/opengl/Shader.cpp)
// how fast the strands wind round each other, in radians per jet length
const float TWIST = 26.0;
// where each jet fades out, in jet lengths from the star
const float JET_END = 0.8;
// how fast the wisps stream outwards, in noise units per second
const float FLOW_SPEED = 0.6;
// as in star_halo.frag: display-encode the linear black-body colour
const float DISPLAY_GAMMA = 2.2;

void main(void)
{
	if (PulsarTime.y <= 0.001)
		discard;

	float s = abs(quadPos.y);
	float side = quadPos.y >= 0.0 ? 1.0 : -1.0;
	// across the jet, in jet lengths
	float x = quadPos.x * PulsarTime.w;
	// how many jet lengths one pixel covers, so thin strands widen instead of flickering from afar
	float px = max(fwidth(x), 1e-6);
	float flow = PulsarTime.z * FLOW_SPEED;

	float xc = x;

	// two strands wound round each other, widening outwards; the near half of each turn is brighter
	float amp = 0.004 + 0.05 * s;
	float strandW = 0.002 + 0.012 * s;
	float drawW = max(strandW, 1.5 * px);
	float strands = 0.0;
	for (int k = 0; k < 2; k++) {
		float ph = TWIST * s - PulsarTime.x + float(k) * PI + side * 0.9;
		float d = (xc - amp * sin(ph)) / drawW;
		strands += exp(-d * d) * (0.6 + 0.4 * cos(ph)) * (strandW / drawW);
	}
	// a fainter sheath of gas round the strands
	float sheathW = max(amp + 2.0 * strandW, 1.5 * px);
	float sheath = 0.35 * exp(-(xc * xc) / (sheathW * sheathW));

	// wisps streaming outwards
	float wisps = cloudFbm(vec3(xc * 55.0, s * 12.0 - flow, side * 7.3), 4, 1.0);
	float jet = (strands + sheath) * mix(0.35, 1.6, wisps);
	// bright at the star, fading along the jet and gone by its end
	jet *= exp(-s * 1.6) * (1.0 - smoothstep(JET_END - 0.05, JET_END + 0.05, s));

	// white-blue at the root, blue along the jet, violet towards the tip
	vec3 rootCol = vec3(0.9, 0.95, 1.0);
	vec3 midCol = vec3(0.45, 0.6, 1.0);
	vec3 tipCol = vec3(0.75, 0.55, 1.0);
	vec3 jetCol = mix(mix(rootCol, midCol, smoothstep(0.0, 0.35, s)), tipCol, smoothstep(0.45, 0.85, s));
	vec3 col = jetCol * jet;

	// a light tint of the star's own colour
	vec3 starColor = pow(max(StarColor.rgb, vec3(0.0)), vec3(1.0 / DISPLAY_GAMMA));
	starColor /= max(max(starColor.r, starColor.g), max(starColor.b, 1e-3));
	col *= mix(vec3(1.0), starColor, 0.3);

	// fade out at the strip's long edges
	float edge = 1.0 - smoothstep(0.85, 1.0, abs(quadPos.x));
	frag_color = vec4(col * edge * 1.2 * PulsarTime.y, 1.0);
}
