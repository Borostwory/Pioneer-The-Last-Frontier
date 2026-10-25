// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINSURFACETILES_H
#define _TERRAINSURFACETILES_H

#include "JobQueue.h"
#include "RefCounted.h"
#include "vector3.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

class GeoPatch;
class GeoSphere;

namespace Graphics {
	class Texture;
}

// Patch surface tiles: the colour, altitude above sea level and surface
// normals of a patch at twice its vertex density, baked on the CPU from the
// terrain functions (BakeSurfaceTexels, as the far texture) into a slot of a
// shared atlas, for the terrain shaders to use per pixel instead of the
// vertex colours and normals (terrain-far.glsl). The tiles follow the patch
// quadtree, so they get finer as the camera comes closer, on every body, as
// the mesh does: the far texture's look carries on down to the ground.
//
// Only patches whose vertices are a few pixels apart on screen get one, and
// only where the tile is finer than the planet's far texture (from afar that
// is sharper); nearest first, a few jobs at a time, and fewer while patch
// jobs are waiting. Tiles fade in once uploaded. Mesh and collision are
// unchanged.
class TerrainSurfaceTiles {
public:
	static constexpr int ATLAS_SIZE = 4096;
	// texels around a tile for filtering across the patch edges
	static constexpr int TILE_BORDER = 1;
	// patches get a tile once their vertices are this many pixels apart
	static constexpr double MIN_VERTEX_PIXELS = 2.0;
	// at most this many finished tiles uploaded per frame
	static constexpr int UPLOADS_PER_FRAME = 48;

	// switched on (config TerrainSurfaceTiles)
	static bool Enabled();
	// the shared atlas for tiles of tileSize texels across a patch (twice the
	// patch's vertex gaps), created on first use or when the size changes
	static TerrainSurfaceTiles *Get(int tileSize);
	// the atlas if it exists, nullptr otherwise
	static TerrainSurfaceTiles *Instance();
	static void Destroy();
	// frees a patch's tile (safe after Destroy)
	static void ReleaseSlot(int slot);

	// Gives tiles to the visible patches of a planet that need one and
	// uploads finished ones. visible: (squared distance in planet radii,
	// patch). pixelAngle: radians per pixel. farTexel: texel size of the
	// planet's far texture on the unit sphere, 0 if it has none.
	void Update(GeoSphere *sphere, const std::vector<std::pair<double, GeoPatch *>> &visible, double pixelAngle, double farTexel);

	Graphics::Texture *GetColour() const { return m_colour.Get(); }
	Graphics::Texture *GetNormal() const { return m_normal.Get(); }
	// atlas texture coordinates of a slot's interior: origin (xy), size (z)
	vector3f SlotUV(int slot) const;
	// 0 until the slot's tile is uploaded, then rising to 1
	float SlotFade(int slot) const;

	// tiles in use and being baked, for the Terrain Debug window
	int GetTileCount() const;
	int GetBakingCount() const { return int(m_pending.size()); }

	~TerrainSurfaceTiles();

	// filled by a job, see TerrainSurfaceTiles.cpp
	struct Bake;

private:
	explicit TerrainSurfaceTiles(int tileSize);
	int Allocate(GeoPatch *patch);
	void Free(int slot);
	void UploadFinished();

	struct Pending {
		std::shared_ptr<Bake> bake;
		Job::Handle job;
		int slot;
	};

	int m_tileSize;
	int m_slotStride;
	int m_slotsPerRow;
	RefCountedPtr<Graphics::Texture> m_colour;
	RefCountedPtr<Graphics::Texture> m_normal;
	// per slot: the patch (nullptr if free), when it was last visible (real
	// time: several planets update the atlas each frame), when its tile
	// arrived, and whether it has
	std::vector<GeoPatch *> m_owners;
	std::vector<std::chrono::steady_clock::time_point> m_lastSeen;
	std::vector<std::chrono::steady_clock::time_point> m_uploaded;
	std::vector<bool> m_ready;
	std::vector<int> m_free;
	std::vector<Pending> m_pending;
};

#endif // _TERRAINSURFACETILES_H
