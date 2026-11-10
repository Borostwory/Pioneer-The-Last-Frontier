// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// SMAA, step 1 of 3: finding the edges (ScreenEffects.cpp)

#include "attributes.glsl"

#define SMAA_INCLUDE_PS 0
#include "smaa-setup.glsl"

out vec2 v_uv;
out vec4 v_offset[3];

void main(void)
{
	gl_Position = vec4(a_vertex.xy, 0.0, 1.0);
	v_uv = a_uv0.xy;
	SMAAEdgeDetectionVS(v_uv, v_offset);
}
