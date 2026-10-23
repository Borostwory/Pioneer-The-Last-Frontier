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

// Terrain detail tiles: per patch, the GPU bakes the landforms between the
// mesh and a quarter of its vertex spacing (data/shaders/opengl/terrain_tile.frag)
// into a slot of a shared atlas, once, and the terrain shaders sample it for
// relief and materials (terrain-materials.glsl). Only patches whose vertices
// are a few pixels apart get one, nearest first; the rest keep the generic
// relief. Mesh and collision are unchanged.
class TerrainTiles {
public:
	// texels across a patch, the border around it (for filtering across the
	// patch edges), and the atlas size
	static constexpr int TILE_SIZE = 128;
	static constexpr int TILE_BORDER = 1;
	static constexpr int ATLAS_SIZE = 4096;
	// at most this many tiles baked per frame
	static constexpr int BAKES_PER_FRAME = 24;
	// patches get a tile once their vertices are this many pixels apart
	static constexpr double MIN_VERTEX_PIXELS = 4.0;

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
	void Bake(Graphics::Renderer *r, GeoSphere *sphere, GeoPatch *patch, int slot);

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
