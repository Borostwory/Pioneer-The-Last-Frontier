// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINTILES_H
#define _TERRAINTILES_H

#include "RefCounted.h"
#include "vector3.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

class GeoPatch;
class GeoSphere;

namespace Graphics {
	class Material;
	class Renderer;
	class RenderTarget;
	class Texture;
	class VertexArray;
} // namespace Graphics

// Terrain detail tiles: per patch, the GPU bakes landforms (craters, ridges,
// dunes, rolling hills; data/shaders/opengl/terrain_tile.frag) into a slot
// of a shared atlas, once, and the terrain shaders sample it for relief and
// materials (terrain-materials.glsl). The largest landforms are twice the
// vertex spacing, and at least LANDFORM_PIXELS on screen: they used to
// follow the vertex spacing alone, and only patches whose vertices were
// 4 px apart got a tile, so at high terrain detail distances (vertices about
// 3 px apart) the landforms vanished and bodies like the Moon looked smooth,
// or showed them on some patches only. Nearest first; patches too small on
// screen keep the generic relief. Mesh and collision are unchanged.
class TerrainTiles {
public:
	// texels across a patch, the border around it (for filtering across the
	// patch edges), and the atlas size
	static constexpr int TILE_SIZE = 128;
	static constexpr int TILE_BORDER = 1;
	// 47 x 47 slots (144 MB): a tile for every visible patch at high terrain
	// detail distances
	static constexpr int ATLAS_SIZE = 6144;
	// at most this many tiles baked per frame
	static constexpr int BAKES_PER_FRAME = 24;
	// patches get a tile once they are this many pixels across
	static constexpr double MIN_PATCH_PIXELS = 16.0;
	// the largest landforms are at least this many pixels long, the
	// smallest a quarter of that
	static constexpr double LANDFORM_PIXELS = 32.0;

	// the shared atlas, created on first use; nullptr if it cannot be
	static TerrainTiles *Get(Graphics::Renderer *r);
	static void Destroy();
	// frees a patch's tile (safe after Destroy)
	static void ReleaseSlot(int slot);

	// Gives tiles to the visible patches that need one, nearest first,
	// baking at most BAKES_PER_FRAME; when the atlas is full, takes the tiles
	// of patches not seen for a few frames. visible: (squared distance in
	// planet radii, patch). pixelAngle: radians per pixel. Changes the render
	// target and viewport.
	void Update(Graphics::Renderer *r, GeoSphere *sphere, const std::vector<std::pair<double, GeoPatch *>> &visible, double pixelAngle);

	Graphics::Texture *GetAtlas() const;
	// atlas texture coordinates of a slot's interior: origin (xy) and size (z)
	vector3f SlotUV(int slot) const;

private:
	explicit TerrainTiles(Graphics::Renderer *r);
	int Allocate(GeoPatch *patch);
	// footprint: metres per pixel at the patch
	void Bake(Graphics::Renderer *r, GeoSphere *sphere, GeoPatch *patch, int slot, double footprint);

	static constexpr int SLOT_STRIDE = TILE_SIZE + 2 * TILE_BORDER;
	static constexpr int SLOTS_PER_ROW = ATLAS_SIZE / SLOT_STRIDE;

	std::unique_ptr<Graphics::RenderTarget> m_target;
	RefCountedPtr<Graphics::Material> m_material;
	std::unique_ptr<Graphics::VertexArray> m_triangle;
	std::vector<GeoPatch *> m_owners; // per slot, nullptr if free
	std::vector<uint32_t> m_lastSeen; // per slot, frame its patch was last visible
	std::vector<int> m_free;
	uint32_t m_frame = 0;
};

#endif // _TERRAINTILES_H
