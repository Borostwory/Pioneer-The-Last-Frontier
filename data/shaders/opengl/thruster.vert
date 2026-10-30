// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// #extension GL_ARB_gpu_shader5 : enable

#include "attributes.glsl"
#include "lib.glsl"

out vec2 texCoord0;
out vec3 face_norm;
out float is_glow;
out float power;
out float flicker;
out float phase;
out float alpha;

float map_percentage( float val, float base_percent)
{
   return base_percent + val * (1.0 - base_percent);
}

void main(void)
{
	// emission.r is the thruster power setting
	power = material.emission.r;
	// emission.g is a looping animation phase (0..1)
	phase = material.emission.g;
	// emission.a is the random flicker value
	float random = material.emission.a;
	flicker = random;

	// the glow cards face along the thrust axis, the flame planes contain it
	is_glow = step(0.5, abs(a_normal.z));

	// flicker should only change the size a small amount each frame
	float flicker_w = map_percentage(random, 0.95);
	float flicker_len = map_percentage(1.0 - random, 0.90);

	// low power: short and spread out, full power: long and tight
	// (the fragment shader draws the actual flame shape inside these planes)
	float flame_width = mix(1.2, 0.9, smoothstep(0.0, 1.0, power)) * flicker_w;
	float flame_length = mix(0.3, 1.0, smoothstep(0.0, 1.0, power)) * flicker_len;
	// flame_length only effects the z axis which is the thrust direction
	vec4 scale = vec4(flame_width, flame_width, flame_length, 1.0);

	gl_Position = uViewProjectionMatrix * (a_vertex * scale);

	face_norm = normalize(normalMatrix() * a_normal);

	texCoord0 = a_uv0.xy;
	// diffuse.a is the view-direction crossfade between flame planes and glow cards
	alpha = material.diffuse.a;
}
