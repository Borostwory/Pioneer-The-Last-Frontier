// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// The ground near the camera tessellated, with the air (P4a of docs/implementation_plans/opengl46-tessellation-compute_plan.md):
// as geosphere_terrain.tese, with what rayleigh_geosphere_terrain.vert makes for the light and the air, blended; then
// raised by the stones of the textures, with their slope and shade for the pixel shader (P4c, P4e, terrainStones()).

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
in vec4 tcAlbedo[];
in vec2 tcTexCoord0[];
in float tcSkirt[];
in float tcMorph[];
in vec4 tcMaterials0[];
in vec4 tcMaterials1[];
in vec4 tcMaterials2[];
in vec4 tcMaterials3[];
#if (NUM_LIGHTS > 0)
in vec3 tcLightAtSurface[][NUM_LIGHTS];
in vec3 tcViewTransmittance[];
in vec3 tcViewInscatter[];
#endif
#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
in float tcSurfaceHeight[];
in vec3 tcWaterSkyLight[];
#endif

// as rayleigh_geosphere_terrain.vert gives them to rayleigh_geosphere_terrain.frag
out vec3 varyingEyepos;
out vec3 varyingNormal;
out vec4 vertexColor;
out vec4 albedoColor;
out vec2 texCoord0;
out float dist;
out vec4 materialWeights0;
out vec4 materialWeights1;
out vec4 materialWeights2;
out vec4 materialWeights3;
#if (NUM_LIGHTS > 0)
out vec3 lightAtSurface[NUM_LIGHTS];
out vec3 viewTransmittance;
out vec3 viewInscatter;
#endif
#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
out float surfaceHeight;
out vec3 waterSkyLight;
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
	albedoColor = TESS_BLEND(tcAlbedo);
	texCoord0 = TESS_BLEND(tcTexCoord0);
	dist = length(varyingEyepos);
	materialWeights0 = TESS_BLEND(tcMaterials0);
	materialWeights1 = TESS_BLEND(tcMaterials1);
	materialWeights2 = TESS_BLEND(tcMaterials2);
	materialWeights3 = TESS_BLEND(tcMaterials3);
#if (NUM_LIGHTS > 0)
	for (int l = 0; l < NUM_LIGHTS; ++l)
		lightAtSurface[l] = gl_TessCoord.x * tcLightAtSurface[0][l] + gl_TessCoord.y * tcLightAtSurface[1][l] + gl_TessCoord.z * tcLightAtSurface[2][l];
	viewTransmittance = TESS_BLEND(tcViewTransmittance);
	viewInscatter = TESS_BLEND(tcViewInscatter);
#endif
#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
	surfaceHeight = TESS_BLEND(tcSurfaceHeight);
	waterSkyLight = TESS_BLEND(tcWaterSkyLight);
#endif

	// the stones: straight up from the flat ground, on screen too (a direction, so the view's offset doesn't enter)
	vec3 up = normalize(varyingEyepos * geosphereInvRadius - geosphereCenter);
	vec4 slots[MATERIAL_SLOT_GROUPS] = vec4[MATERIAL_SLOT_GROUPS](materialWeights0, materialWeights1, materialWeights2, materialWeights3);
#ifdef TERRAIN_WITH_WATER
	const bool hasWater = true;
#else
	const bool hasWater = false;
#endif
#if defined(TERRAIN_WITH_WATER) && (NUM_LIGHTS > 0)
	float altitude = terrainAltitude(surfaceHeight);
#else
	float altitude = 1.0e4;
#endif
	terrainStones(slots, varyingEyepos, normalize(varyingNormal), up, altitude, hasWater, stoneLift, stoneSlope, stoneLit);
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
