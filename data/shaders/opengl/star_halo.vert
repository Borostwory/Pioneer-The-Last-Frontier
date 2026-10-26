// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"

// position on the halo quad, -1..1 from its centre
out vec2 quadPos;

void main(void)
{
	gl_Position = matrixTransform();
	quadPos = a_uv0.xy;
}
