// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// SMAA, step 1 of 3: where the brightness jumps between neighbours, an edge (ScreenEffects.cpp). Pixels without one
// are left out, so the target is cleared first.

#include "attributes.glsl"

#define SMAA_INCLUDE_VS 0
#include "smaa-setup.glsl"

uniform sampler2D colourTex;

in vec2 v_uv;
in vec4 v_offset[3];
out vec4 frag_color;

void main(void)
{
	frag_color = vec4(SMAALumaEdgeDetectionPS(v_uv, v_offset, colourTex), 0.0, 0.0);
}
