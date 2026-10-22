// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Full-screen triangle for the cloud passes: positions are already in clip space.

#include "attributes.glsl"

void main(void)
{
	gl_Position = vec4(a_vertex.xy, 0.0, 1.0);
}
