// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINSTONES_H
#define _TERRAINSTONES_H

#include "RefCounted.h"
#include "vector3.h"

#include <cstdint>
#include <vector>

namespace Graphics {
	class Renderer;
	class Texture;
} // namespace Graphics

// The stones raised near the camera (P4c of docs/implementation_plans/opengl46-tessellation-compute_plan.md), worked
// out on the main processor the way the terrain's tessellation evaluation shaders work them out (terrainStones() in
// terrain-materials.glsl), so feet and wheels stand on them (P4d). Line for line the same, in single precision, reading
// the same copy of the textures' heights: an uncompressed one, from their 1024-pixel size down, which the graphics card
// gets too (GetTexture), since the card may unpack the compressed textures slightly differently. The turns of the
// textures come from terrain-material-rotation.glsl itself, so the material tool stays the one place they're set.
namespace TerrainStones {
	// what the terrain material's constants hold for the stones (GeoSphere: MaterialSlots, MaterialSpecial,
	// MaterialLook, MaterialNear, MaterialStones, TessParams)
	struct Params {
		float slotLayer[16] = {}; // texture layer of each slot (-1 unused)
		int usedSlots = 0;
		int rockSlot = -1, beachSlot = -1, snowSlot = -1, lavaSlot = -1;
		float iceCracks = 0.0f; // MaterialLook.x
		bool fixedDetail = false; // MaterialLook.y
		float nearLevel = 0.0f, nearRange = 0.0f; // MaterialNear.xy
		bool nearOn = false; // MaterialNear.z
		bool stonesOn = false; // MaterialStones.x
		float stonesHeight = 0.0f; // MaterialStones.y
		float deepest = 0.0f; // MaterialStones.w: the tallest of the body's materials' stones (Depth())
		float pixelsPerRadian = 1.0f, aimPixels = 1.0f; // TessParams.xy
		float fullPixels = 8.0f; // TessParams.w: the stones' height on screen (pixels) below which the cuts thin out
		float fadePixels = 2.0f; // MaterialNear.w: the stones' height on screen (pixels) from which they fade out
		// the circles kept free of stones (StoneFlat0..5): planet-fixed centres in metres, radii in metres
		static constexpr int MAX_FLATS = 6;
		int flatCount = 0;
		vector3d flatCentre[MAX_FLATS];
		double flatRadius[MAX_FLATS] = {};
	};

	// a point of the drawn ground, as the evaluation shaders get it
	struct Point {
		vector3d position; // planet-fixed, metres from the centre
		vector3d camera; // the camera's, likewise
		vector3d normal; // the mesh's, planet-fixed, unit length
		float slots[16] = {}; // the mesh's slot weights there, 0..1
		float altitude = 1.0e4f; // above sea level, metres
		bool hasWater = false;
	};

	// how far the ground stands up there, in metres (0 where nothing is raised). Loads the heights the first time.
	double Lift(const Params &params, const Point &point);

	// the heights for the graphics card (terrainStoneHeights in the terrain shaders), made the first time
	Graphics::Texture *GetTexture(Graphics::Renderer *renderer);

	// how high a material's texture raises the ground: its full height range, as a share of the tile
	// (materialStoneDepth() in terrain-materials.glsl)
	float Depth(int layer);

	// materialCellHash() of terrain-materials.glsl: a cell's hash, 0..1, for the turns of the textures
	float CellHash(float cx, float cy, uint32_t salt);

	// whether the heights loaded (they are read the first time they're needed)
	bool Loaded();
} // namespace TerrainStones

#endif
