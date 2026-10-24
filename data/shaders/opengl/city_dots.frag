// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"

in vec3 dotColour;

out vec4 frag_color;

void main(void)
{
	// a soft round dot, brightest in the middle
	vec2 p = gl_PointCoord * 2.0 - 1.0;
	float r2 = dot(p, p);
	float fall = r2 < 1.0 ? (1.0 - r2) * (1.0 - r2) : 0.0;
	frag_color = vec4(dotColour * fall, 1.0);
}
