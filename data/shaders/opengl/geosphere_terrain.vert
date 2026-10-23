// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"
#include "basesphere_uniforms.glsl"

out vec3 varyingEyepos;
out vec3 varyingNormal;
out vec4 vertexColor;

out vec2 texCoord0;
out float dist;

// the body's material weights (terrain-materials.glsl)
layout(location = 10) in vec4 a_materials0;
layout(location = 11) in vec4 a_materials1;
out vec4 materialWeights0;
out vec4 materialWeights1;

#ifdef TERRAIN_WITH_WATER
// encoded altitude above sea level, for the per-pixel water (terrain-water.glsl)
out float surfaceHeight;
#endif

#ifdef TERRAIN_WITH_LAVA
out vec4 varyingEmission;
#endif

void main(void)
{
	gl_Position = matrixTransform();
	// the colour's alpha holds the altitude above sea level
	vertexColor = vec4(a_color.rgb, 1.0);
	materialWeights0 = a_materials0;
	materialWeights1 = a_materials1;
#ifdef TERRAIN_WITH_WATER
	surfaceHeight = a_color.a * 2.0 - 1.0;
#endif
	varyingEyepos = vec3(uViewMatrix * a_vertex);
	varyingNormal = normalize(normalMatrix() * a_normal);

	texCoord0 = a_uv0.xy;
	dist = length(varyingEyepos);

#ifdef TERRAIN_WITH_LAVA
	// lava glows per pixel (terrain-materials.glsl)
	varyingEmission = material.emission;
#endif
}
