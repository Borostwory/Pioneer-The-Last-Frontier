// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// How finely the ground is cut for the stones at a distance (P4c of the OpenGL 4.6 plan), shared by the tessellation
// control shaders (terrain-tess-edges.glsl), the stones themselves (terrain-materials.glsl) and the main processor's
// copy (TerrainStones.cpp, which must change with it). Needs the uniforms TessParams (x: pixels per radian of the
// view, y: the edge on screen to aim for, pixels), MaterialNear and MaterialStones.
//
// Gradually coarser with distance, by how tall the stones stand on screen: where even the tallest stone here stands at
// least TessParams.w pixels (the debug window's slider, 8 to start), the cuts aim for TessParams.y; below that, the aim grows as the stones shrink, so the
// cuts stay as far apart against the stones as up close; from MaterialNear.w pixels (the debug window's slider, 2 to
// start) down to half that the stones fade out, and below it nothing is cut or raised.

// the stones' height on screen (pixels) from which they fade out, and below half of which they're gone
#define STONE_FADE_PIXELS max(MaterialNear.w, 0.1)

// how many pixels tall the tallest stone could stand on screen at a distance (metres), with the fading out by distance;
// MaterialStones.w: the tallest of this body's materials' stones, as a share of the tile (materialStoneDepth())
float terrainStonePixels(float dist)
{
	float tallest = MaterialStones.w * MaterialStones.y * exp2(MaterialNear.x) * (1.0 - smoothstep(MaterialNear.y, 2.0 * MaterialNear.y, dist));
	return tallest * TessParams.x / max(dist, 1e-3);
}

// the edge on screen the cuts aim for at a distance, in pixels
float terrainStoneAim(float dist)
{
	return max(TessParams.y, 1.0) * max(1.0, max(TessParams.w, 1.0) / max(terrainStonePixels(dist), 1e-3));
}
