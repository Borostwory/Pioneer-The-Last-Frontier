// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt
// P6 of the OpenGL 4.6 plan: each triangle cut into 4 along its edges
layout(vertices = 3) out;

void main(void)
{
	gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
	if (gl_InvocationID == 0) {
		gl_TessLevelOuter[0] = 4.0;
		gl_TessLevelOuter[1] = 4.0;
		gl_TessLevelOuter[2] = 4.0;
		gl_TessLevelInner[0] = 4.0;
	}
}
