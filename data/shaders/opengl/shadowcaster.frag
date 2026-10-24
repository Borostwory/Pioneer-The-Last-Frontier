// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"

// the shadow map has no color attachment; only the depth is written
out vec4 frag_color;

void main(void)
{
	frag_color = vec4(1.0);
}
