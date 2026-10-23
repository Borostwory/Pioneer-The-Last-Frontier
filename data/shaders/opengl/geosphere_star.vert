// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"

out vec4 vertexColor;

void main(void)
{
	gl_Position = matrixTransform();
	// the colour's alpha holds the altitude above sea level
	vertexColor = vec4(a_color.rgb, 1.0);
}
