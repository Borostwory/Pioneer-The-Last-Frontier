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
// 1 on the patch's skirt, 0 on its ground (GeoPatch marks the skirt's uv): the stones taper off down the skirt, and
// it's lit as the ground beside it
out float skirtWeight;
// what GeoPatch adds to a skirt vertex's uv (SKIRT_UV_MARK)
#define TERRAIN_SKIRT_UV_MARK 2.0

// the body's material weights (terrain-materials.glsl)
layout(location = 10) in vec4 a_materials0;
layout(location = 11) in vec4 a_materials1;
layout(location = 12) in vec4 a_materials2;
layout(location = 13) in vec4 a_materials3;
out vec4 materialWeights0;
out vec4 materialWeights1;
out vec4 materialWeights2;
out vec4 materialWeights3;

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
	materialWeights2 = a_materials2;
	materialWeights3 = a_materials3;
#ifdef TERRAIN_WITH_WATER
	surfaceHeight = a_color.a * 2.0 - 1.0;
#endif
	varyingEyepos = vec3(uViewMatrix * a_vertex);
	varyingNormal = normalize(normalMatrix() * a_normal);

	bool skirt = a_uv0.x > 0.5 * TERRAIN_SKIRT_UV_MARK + 0.5;
	texCoord0 = skirt ? a_uv0.xy - TERRAIN_SKIRT_UV_MARK : a_uv0.xy;
	skirtWeight = skirt ? 1.0 : 0.0;
	dist = length(varyingEyepos);

#ifdef TERRAIN_WITH_LAVA
	// lava glows per pixel (terrain-materials.glsl)
	varyingEmission = material.emission;
#endif
}
