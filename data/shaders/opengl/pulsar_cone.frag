// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// The cones a pulsar's two beams sweep round (Star::RenderPulsar), lit by what the eye sees of the
// sweep: behind a slow beam a short fading trail, round a fast one a glowing hollow cone. The beam's
// light is spread over the part of the cone it crossed while the eye held the picture, so the faster
// it turns the fainter and more even the cone. Like any thin glowing shell, it is brightest where it
// is seen edge-on.

#include "attributes.glsl"

in vec2 conePos;
in vec3 eyePos;
in vec3 eyeNormal;

out vec4 frag_color;

// black-body star colour
uniform vec4 StarColor;
// x: the beams' angle round the spin axis now, y: how far round they swept while the eye held the
// picture (at most a full turn), z: how fast the trail fades behind the beam, w: a beam's width as
// an angle round the cone (all radians)
uniform vec4 PulsarCone;
// x: how bright the beam strip itself is drawn (pulsar_jet.frag), y: 1 when the beams lap the
// whole cone while the eye holds the picture
uniform vec4 PulsarBeam;

// (PI is defined for every shader, see graphics/opengl/Shader.cpp)
// where each beam fades out, in jet lengths from the star (as JET_END in pulsar_jet.frag)
const float JET_END = 0.8;
// overall brightness of the smear, against the beam strip's
const float CONE_GAIN = 1.2;
// edge-on brightening stops at this, so the cone's outline doesn't burn white
const float MIN_FACING = 0.12;
// as in star_halo.frag: display-encode the linear black-body colour
const float DISPLAY_GAMMA = 2.2;

void main(void)
{
	float s = conePos.x;
	float arc = PulsarCone.y;
	float fade = PulsarCone.z;
	float beamArc = PulsarCone.w;

	// how far behind the beams this point is, round the cone
	float behind = mod(PulsarCone.x - conePos.y, 2.0 * PI);

	// only the part the beams crossed is lit, the newest part brightest
	float inArc = PulsarBeam.y > 0.5 ? 1.0 : 1.0 - smoothstep(arc - 0.5 * beamArc, arc + 0.5 * beamArc, behind);
	float trailScale = fade > 1e-3 ? fade / (1.0 - exp(-fade)) : 1.0;
	float trail = exp(-fade * behind / arc) * trailScale * inArc;
	// the beam's light spread over the arc it swept
	float lit = min(trail * min(beamArc / arc, 1.0), 1.5);
	// the beam itself is drawn by the strip, so leave its place to the strip as much as the strip shows
	float head = 1.0 - smoothstep(0.5 * beamArc, 1.5 * beamArc, behind);
	lit *= 1.0 - PulsarBeam.x * head;
	if (lit <= 0.001)
		discard;

	// along each beam: bright at the star, fading outwards and gone by its end (as pulsar_jet.frag)
	float along = exp(-s * 1.6) * (1.0 - smoothstep(JET_END - 0.05, JET_END + 0.05, s));
	// a thin glowing shell is brighter where it's seen edge-on
	float facing = abs(dot(normalize(eyeNormal), normalize(eyePos)));
	float shell = 1.0 / max(facing, MIN_FACING);

	// white-blue at the root, blue along the beam, violet towards the tip (as pulsar_jet.frag)
	vec3 rootCol = vec3(0.9, 0.95, 1.0);
	vec3 midCol = vec3(0.45, 0.6, 1.0);
	vec3 tipCol = vec3(0.75, 0.55, 1.0);
	vec3 col = mix(mix(rootCol, midCol, smoothstep(0.0, 0.35, s)), tipCol, smoothstep(0.45, 0.85, s));

	// a light tint of the star's own colour
	vec3 starColor = pow(max(StarColor.rgb, vec3(0.0)), vec3(1.0 / DISPLAY_GAMMA));
	starColor /= max(max(starColor.r, starColor.g), max(starColor.b, 1e-3));
	col *= mix(vec3(1.0), starColor, 0.3);

	// a beam is a narrow bundle; at its root, where the cone's sides meet, keep the shell faint (the glow is there)
	float root = smoothstep(0.0, 0.04, s);
	frag_color = vec4(col * lit * along * shell * root * CONE_GAIN, 1.0);
}
