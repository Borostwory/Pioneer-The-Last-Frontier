// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"

// quad given directly in view space
out vec3 v_dir;

void main(void)
{
	gl_Position = uViewProjectionMatrix * a_vertex;
	v_dir = a_vertex.xyz;
}
