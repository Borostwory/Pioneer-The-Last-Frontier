// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// The blend between detail levels, for the terrain vertex shaders (needs attributes.glsl first). Each vertex of a
// patch also holds its parent patch's surface there, as the parent's own triangles draw it (GeoPatch::UpdateVBOs):
// the offset to it, its normal and its colour. Along each edge that meets a coarser patch, a strip of the patch takes
// it on, all of it at the edge, where the patch then matches its coarser neighbour, so there's no line between them;
// the rest keeps its own detail at any distance. A patch just made fades in from its parent's look, so new detail
// grows in rather than popping in. (GeoPatch::SetMorphConstants sets the uniforms for each patch drawn.) The fragment
// shaders take it for the uniforms alone, before terrain-far.glsl, which reads them.

// 1 for each edge of the patch that meets a coarser patch: x at the patch coordinate x = 0, y at x = 1, z at y = 0,
// w at y = 1
uniform vec4 PatchMorph;
// x: the strip's width, a share of the patch; y: how much of its parent's look a patch just made still has, 1 falling
// to 0 over its first moments; z: 1 to blend; w: 1 to tint the ground by the blend (the debug window)
uniform vec4 PatchMorphRange;

#ifdef VERTEX_SHADER
// the parent's surface (GeoPatchContext::VBOVertex): its position less the vertex's and its normal at locations 4 and 5,
// which the terrain doesn't use otherwise (a_uv1, a_tangent in attributes.glsl), and its colour
#define a_morphOffset (a_uv1.xyz)
#define a_morphNormal a_tangent
layout(location = 14) in vec4 a_morphColor;
// what GeoPatch adds to a skirt vertex's uv (SKIRT_UV_MARK)
#define TERRAIN_MORPH_SKIRT_MARK 2.0

// 0..1: how much of the parent's surface this vertex takes on
float terrainMorph()
{
	if (PatchMorphRange.z < 0.5)
		return 0.0;
	// the patch coordinates (GeoPatch: uv = (1 - x, y)), a skirt's as the ground's beside it
	vec2 uv = a_uv0.x > 0.5 * TERRAIN_MORPH_SKIRT_MARK + 0.5 ? a_uv0.xy - TERRAIN_MORPH_SKIRT_MARK : a_uv0.xy;
	vec2 c = clamp(vec2(1.0 - uv.x, uv.y), 0.0, 1.0);
	vec4 toEdge = vec4(c.x, 1.0 - c.x, c.y, 1.0 - c.y);
	vec4 strip = PatchMorph * (1.0 - smoothstep(vec4(0.0), vec4(PatchMorphRange.x), toEdge));
	return max(max(max(strip.x, strip.y), max(strip.z, strip.w)), PatchMorphRange.y);
}

// the vertex blended by morph: its position (w 1), normal (not normalised) and colour
vec4 terrainMorphPosition(float morph)
{
	return vec4(a_vertex.xyz + a_morphOffset * morph, 1.0);
}

vec3 terrainMorphNormal(float morph)
{
	return mix(a_normal, a_morphNormal, morph);
}

vec4 terrainMorphColor(float morph)
{
	return mix(a_color, a_morphColor, morph);
}
#endif // VERTEX_SHADER
