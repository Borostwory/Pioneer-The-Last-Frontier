// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Converts the scene depth buffer to linear view distance for the cloud passes.
// Pioneer uses an infinite reverse-Z projection: depth = near / view distance,
// and the cleared (empty) background has depth 0.

#include "attributes.glsl"
#include "cloud-pass-uniforms.glsl"

uniform sampler2D sceneDepth;

out vec4 frag_color;

void main(void)
{
	float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
	float viewZ = depth > 0.0 ? cloudPassProj.z / depth : 1e30;
	frag_color = vec4(viewZ, 0.0, 0.0, 1.0);
}
