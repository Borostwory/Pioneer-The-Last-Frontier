// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// SMAA, step 3 of 3: each pixel blended with its neighbours by the weights of step 2, into the picture
// (ScreenEffects.cpp)

#include "attributes.glsl"

#define SMAA_INCLUDE_VS 0
#include "smaa-setup.glsl"

uniform sampler2D colourTex;
uniform sampler2D blendTex;

in vec2 v_uv;
in vec4 v_offset;
out vec4 frag_color;

void main(void)
{
	frag_color = SMAANeighborhoodBlendingPS(v_uv, v_offset, colourTex, blendTex);
}
