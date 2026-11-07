// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINSURFACETILES_H
#define _TERRAINSURFACETILES_H

#include "JobQueue.h"
#include "RefCounted.h"
#include "vector3.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

class GeoPatch;
class GeoSphere;
class Terrain;

namespace Graphics {
	class Texture;
}

// The surface texels of the patch surface tiles, as the patches make their
// vertices (setSurface in GeoPatchJobs.cpp): for the grid points (x, y),
// x0 <= x < x1, y0 <= y < y1, of a grid of unit-sphere directions with their
// heights (gridWidth points per row, with the neighbours of those points in it):
// - colour (RGBA8): the colour and the altitude above sea level
//   (EncodeAltitude). The colours see the slope of colourNormals
//   (ColourSlopeNormals, colourPitch per row, from the entry of (x0, y0)),
//   or the grid's own if that is nullptr;
// - normal (RGB10_A2): the surface normal minus the direction, each as
//   0.5 + 0.5 * sign(v) * sqrt(|v| / 1.0) (FAR_NORMAL_RANGE in terrain-far.glsl).
// rowPitch texels per output row, from the texel of (x0, y0). Uses the band
// limit set by the caller (TerrainNoise::ScopedBandLimit).
void BakeSurfaceTexels(const Terrain *terrain, const vector3d *dirs, const double *heights, int gridWidth,
	int x0, int x1, int y0, int y1, const vector3d *colourNormals, int colourPitch,
	uint8_t *colour, uint32_t *normal, size_t rowPitch);

// Patch surface tiles: the colour, altitude above sea level and surface
// normals of a patch at twice its vertex density, baked on the CPU from the
// terrain functions (BakeSurfaceTexels) into a slot of a shared atlas, for
// the terrain shaders to use per pixel instead of the vertex colours and
// normals (terrain-far.glsl). The tiles follow the patch quadtree, so they
// get finer as the camera comes closer, on every body, as the mesh does.
//
// Only patches whose vertices are a few pixels apart on screen get one, and
// not those deep under the sea; nearest first, on the threads the patch jobs
// leave free. Tiles fade in once uploaded. Mesh and collision are unchanged.
class TerrainSurfaceTiles {
public:
	static constexpr int ATLAS_SIZE = 4096;
	// texels around a tile for filtering across the patch edges: the bicubic
	// filter (terrain-far.glsl) reaches two texels from where it reads
	static constexpr int TILE_BORDER = 2;
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
	// patch). pixelAngle: radians per pixel.
	void Update(GeoSphere *sphere, const std::vector<std::pair<double, GeoPatch *>> &visible, double pixelAngle);

	Graphics::Texture *GetColour() const { return m_colour.Get(); }
	Graphics::Texture *GetNormal() const { return m_normal.Get(); }
	// atlas texture coordinates of a slot's interior: origin (xy), size (z)
	vector3f SlotUV(int slot) const;
	// 0 until the slot's tile is uploaded, then rising to 1
	float SlotFade(int slot) const;

	// tiles in use and being baked, for the Terrain Debug window
	int GetTileCount() const;
	int GetBakingCount() const { return int(m_pending.size()); }
	// visible patches still waiting for a bake to start, for the log
	int GetWaitingCount() const;

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
	int m_waiting = 0;
	std::chrono::steady_clock::time_point m_waitingSeen;
};

#endif // _TERRAINSURFACETILES_H
