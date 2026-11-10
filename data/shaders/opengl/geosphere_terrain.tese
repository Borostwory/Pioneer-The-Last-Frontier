// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// The ground near the camera tessellated (P4a of docs/implementation_plans/opengl46-tessellation-compute_plan.md):
// each new point lies on its triangle, and gets what the vertex shader made for the corners, blended by where it is;
// then it's raised by the stones of the textures, with their slope and shade for the pixel shader (P4c, P4e,
// terrainStones()).

layout(triangles, fractional_odd_spacing, ccw) in;

#include "attributes.glsl"
#include "lib.glsl"
#include "basesphere_uniforms.glsl"
#include "cloud-config.glsl"
#include "cloud-uniforms.glsl"
#include "cloud-noise.glsl"
#include "terrain-surface.glsl"
#include "terrain-materials.glsl"

#define TESS_BLEND(v) (gl_TessCoord.x * v[0] + gl_TessCoord.y * v[1] + gl_TessCoord.z * v[2])

in vec3 tcEyepos[];
in vec3 tcNormal[];
in vec4 tcColor[];
in vec2 tcTexCoord0[];
in float tcSkirt[];
in float tcMorph[];
in vec4 tcMaterials0[];
in vec4 tcMaterials1[];
in vec4 tcMaterials2[];
in vec4 tcMaterials3[];
#ifdef TERRAIN_WITH_WATER
in float tcSurfaceHeight[];
#endif
#ifdef TERRAIN_WITH_LAVA
in vec4 tcEmission[];
#endif

// as geosphere_terrain.vert gives them to geosphere_terrain.frag
out vec3 varyingEyepos;
out vec3 varyingNormal;
out vec4 vertexColor;
out vec2 texCoord0;
out float dist;
out vec4 materialWeights0;
out vec4 materialWeights1;
out vec4 materialWeights2;
out vec4 materialWeights3;
#ifdef TERRAIN_WITH_WATER
out float surfaceHeight;
#endif
#ifdef TERRAIN_WITH_LAVA
out vec4 varyingEmission;
#endif
// how far the point was raised, the raised shape's slope and how much main light gets past the stones around
out float stoneLift;
out vec3 stoneSlope;
out float stoneLit;
// the patch's skirt here (the vertex shader's skirtWeight), for the pixel shader
out float skirtWeight;
// how much of the parent patch's surface the ground takes on here (terrain-morph.glsl)
out float patchMorph;

void main(void)
{
	gl_Position = gl_TessCoord.x * gl_in[0].gl_Position + gl_TessCoord.y * gl_in[1].gl_Position + gl_TessCoord.z * gl_in[2].gl_Position;
	varyingEyepos = TESS_BLEND(tcEyepos);
	varyingNormal = TESS_BLEND(tcNormal);
	vertexColor = TESS_BLEND(tcColor);
	texCoord0 = TESS_BLEND(tcTexCoord0);
	dist = length(varyingEyepos);
	materialWeights0 = TESS_BLEND(tcMaterials0);
	materialWeights1 = TESS_BLEND(tcMaterials1);
	materialWeights2 = TESS_BLEND(tcMaterials2);
	materialWeights3 = TESS_BLEND(tcMaterials3);
#ifdef TERRAIN_WITH_WATER
	surfaceHeight = TESS_BLEND(tcSurfaceHeight);
#endif
#ifdef TERRAIN_WITH_LAVA
	varyingEmission = TESS_BLEND(tcEmission);
#endif

	// the stones: straight up from the flat ground, on screen too (a direction, so the view's offset doesn't enter)
	vec3 up = normalize(varyingEyepos * geosphereInvRadius - geosphereCenter);
	vec4 slots[MATERIAL_SLOT_GROUPS] = vec4[MATERIAL_SLOT_GROUPS](materialWeights0, materialWeights1, materialWeights2, materialWeights3);
#ifdef TERRAIN_WITH_WATER
	terrainStones(slots, varyingEyepos, normalize(varyingNormal), up, terrainAltitude(surfaceHeight), true, stoneLift, stoneSlope, stoneLit);
#else
	terrainStones(slots, varyingEyepos, normalize(varyingNormal), up, 1.0e4, false, stoneLift, stoneSlope, stoneLit);
#endif
	// (down a patch's skirt the stones taper off to nothing at its bottom, so it hangs below the ground beside rather
	// than standing up through it; 2026-11-09)
	skirtWeight = TESS_BLEND(tcSkirt);
	patchMorph = TESS_BLEND(tcMorph);
	stoneLift *= 1.0 - skirtWeight;
	vec3 lift = up * stoneLift;
	varyingEyepos += lift;
	// (back to the patch's own units by the view matrix's turn, transposed, and its scale squared: the patches are
	// drawn scaled, and uViewMatrixInverse is made for a turn and a shift alone, so it doesn't undo the scale)
	vec3 patchLift = transpose(mat3(uViewMatrix)) * lift / dot(uViewMatrix[0].xyz, uViewMatrix[0].xyz);
	gl_Position += uViewProjectionMatrix * vec4(patchLift, 0.0);
	dist = length(varyingEyepos);
}
