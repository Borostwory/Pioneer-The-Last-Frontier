// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// SMAA, step 2 of 3: from the shape of each edge (its length and how it ends, read from the lookup textures), how
// much each pixel along it blends with its neighbours (ScreenEffects.cpp)

#include "attributes.glsl"

#define SMAA_INCLUDE_VS 0
#include "smaa-setup.glsl"

uniform sampler2D edgesTex;
uniform sampler2D areaTex;
uniform sampler2D searchTex;

in vec2 v_uv;
in vec2 v_pixcoord;
in vec4 v_offset[3];
out vec4 frag_color;

void main(void)
{
	frag_color = SMAABlendingWeightCalculationPS(v_uv, v_pixcoord, v_offset, edgesTex, areaTex, searchTex, vec4(0.0));
}
