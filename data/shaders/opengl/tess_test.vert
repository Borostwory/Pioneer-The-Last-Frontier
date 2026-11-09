// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt
// P6 of the OpenGL 4.6 plan: the tessellation test's vertices, passed through
in vec4 a_vertex;

void main(void)
{
	gl_Position = a_vertex;
}
