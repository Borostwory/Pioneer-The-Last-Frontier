// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// SMAA, step 2 of 3: how much each pixel on an edge blends with its neighbours (ScreenEffects.cpp)

#include "attributes.glsl"

#define SMAA_INCLUDE_PS 0
#include "smaa-setup.glsl"

out vec2 v_uv;
out vec2 v_pixcoord;
out vec4 v_offset[3];

void main(void)
{
	gl_Position = vec4(a_vertex.xy, 0.0, 1.0);
	v_uv = a_uv0.xy;
	SMAABlendingWeightCalculationVS(v_uv, v_pixcoord, v_offset);
}
