// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// The ground near the camera tessellated (P4a of docs/implementation_plans/opengl46-tessellation-compute_plan.md):
// each triangle of a patch is a patch of 3 vertices, cut finer where its edges are long on screen. What the vertex
// shader made for each corner is passed on; the evaluation shader blends it for the new points.

layout(vertices = 3) out;

// x: pixels per radian of the view, y: the edge on screen to aim for (pixels), z: the most cuts per edge
uniform vec4 TessParams;
// xy: half the viewport's width and height, in pixels
uniform vec4 TessScreen;

// (the view's matrices and the planet's centre, for what's off screen with the stones raised)
#include "attributes.glsl"
#include "lib.glsl"
#include "basesphere_uniforms.glsl"
#include "terrain-tess-edges.glsl"

in vec3 varyingEyepos[];
in vec3 varyingNormal[];
in vec4 vertexColor[];
in vec2 texCoord0[];
in float dist[];
in vec4 materialWeights0[];
in vec4 materialWeights1[];
in vec4 materialWeights2[];
in vec4 materialWeights3[];
#ifdef TERRAIN_WITH_WATER
in float surfaceHeight[];
#endif
#ifdef TERRAIN_WITH_LAVA
in vec4 varyingEmission[];
#endif

out vec3 tcEyepos[];
out vec3 tcNormal[];
out vec4 tcColor[];
out vec2 tcTexCoord0[];
out vec4 tcMaterials0[];
out vec4 tcMaterials1[];
out vec4 tcMaterials2[];
out vec4 tcMaterials3[];
#ifdef TERRAIN_WITH_WATER
out float tcSurfaceHeight[];
#endif
#ifdef TERRAIN_WITH_LAVA
out vec4 tcEmission[];
#endif

void main(void)
{
	// (outputs must be indexed by gl_InvocationID itself, not a copy of it)
	gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
	tcEyepos[gl_InvocationID] = varyingEyepos[gl_InvocationID];
	tcNormal[gl_InvocationID] = varyingNormal[gl_InvocationID];
	tcColor[gl_InvocationID] = vertexColor[gl_InvocationID];
	tcTexCoord0[gl_InvocationID] = texCoord0[gl_InvocationID];
	tcMaterials0[gl_InvocationID] = materialWeights0[gl_InvocationID];
	tcMaterials1[gl_InvocationID] = materialWeights1[gl_InvocationID];
	tcMaterials2[gl_InvocationID] = materialWeights2[gl_InvocationID];
	tcMaterials3[gl_InvocationID] = materialWeights3[gl_InvocationID];
#ifdef TERRAIN_WITH_WATER
	tcSurfaceHeight[gl_InvocationID] = surfaceHeight[gl_InvocationID];
#endif
#ifdef TERRAIN_WITH_LAVA
	tcEmission[gl_InvocationID] = varyingEmission[gl_InvocationID];
#endif
	if (gl_InvocationID == 0)
		terrainTessLevels(varyingEyepos[0], varyingEyepos[1], varyingEyepos[2], gl_in[0].gl_Position, gl_in[1].gl_Position, gl_in[2].gl_Position);
}
