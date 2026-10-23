// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Glow around the bright point ahead of the ship in hyperspace: stands in for
// the bloom of a camera or eye looking at a source far brighter than the screen can show.

#include "attributes.glsl"
#include "lib.glsl"

layout(std140) uniform GlowData {
	vec4 u_glowAxis;  // xyz: direction of the glow in view space, w: angular size of a pixel (rad)
	vec4 u_glowColor; // rgb: flux spread into the glow (pixel units), w: quad radius (px)
	vec4 u_glowShape; // x: inner radius (px), y: inner fraction, z: outer radius (px), w: outer fraction
};

in vec3 v_dir;

out vec4 frag_color;

// Normalised (integrates to 1 over the plane) halo with core radius r0, in pixels
float halo(float r, float r0)
{
	float q = 1.0 + (r * r) / (r0 * r0);
	return 1.0 / (3.14159265 * r0 * r0 * q * q);
}

void main(void)
{
	float angle = 2.0 * asin(clamp(0.5 * length(normalize(v_dir) - u_glowAxis.xyz), 0.0, 1.0));
	float r = angle / u_glowAxis.w;

	float profile = u_glowShape.y * halo(r, u_glowShape.x) + u_glowShape.w * halo(r, u_glowShape.z);
	// fade out before the edge of the quad so it never shows
	profile *= 1.0 - smoothstep(0.6 * u_glowColor.w, u_glowColor.w, r);

	frag_color = vec4(toSRGB(1.0 - exp(-u_glowColor.rgb * profile)), 1.0);
}
