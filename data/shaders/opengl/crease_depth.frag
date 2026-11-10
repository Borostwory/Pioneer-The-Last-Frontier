// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Crease shading, step 1 (ScreenEffects.cpp): the scene's depth buffer as each pixel's distance in front of the camera
// (m), so the steps after never read the depth buffer of the picture they draw into. The game's projection is an
// infinite reverse-Z one: depth = near plane / distance, and the empty background has depth 0 (as cloud_depth.frag).

#include "attributes.glsl"

uniform sampler2D sceneDepth;
// x: the projection's P[0], y: its P[5], z: the near plane (m)
uniform vec4 CreaseProj;

out vec4 frag_color;

void main(void)
{
	float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
	frag_color = vec4(depth > 0.0 ? CreaseProj.z / depth : 1e30, 0.0, 0.0, 1.0);
}
