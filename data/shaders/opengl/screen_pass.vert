// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// One triangle covering the screen for the screen passes (ScreenEffects.cpp): positions already in clip space, and
// where on the screen each pixel is, 0..1 (v_uv).

#include "attributes.glsl"

out vec2 v_uv;

void main(void)
{
	gl_Position = vec4(a_vertex.xy, 0.0, 1.0);
	v_uv = a_uv0.xy;
}
