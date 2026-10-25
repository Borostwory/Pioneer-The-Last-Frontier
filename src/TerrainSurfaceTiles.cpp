// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainSurfaceTiles.h"

#include "GameConfig.h"
#include "GeoPatch.h"
#include "GeoPatchJobs.h"
#include "GeoSphere.h"
#include "MathUtil.h"
#include "Pi.h"
#include "TerrainFarTexture.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "profiler/Profiler.h"
#include "terrain/Terrain.h"
#include "terrain/TerrainNoise.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

struct TerrainSurfaceTiles::Bake {
	RefCountedPtr<Terrain> terrain;
	vector3d corners[4];
	int tileSize = 0;
	double texelSpacing = 0.0; // on the unit sphere
	// (tileSize + 2 * TILE_BORDER)^2 texels (RGBA8, RGB10_A2), as in the atlas slot
	std::vector<uint8_t> colour;
	std::vector<uint32_t> normal;
	std::atomic<bool> done { false };
};

namespace {
	std::unique_ptr<TerrainSurfaceTiles> s_tiles;
	static const float FADE_SECONDS = 0.5f;
	// a full atlas takes the tiles of patches out of view this long
	static const std::chrono::milliseconds EVICT_UNSEEN(250);

	// bakes queued or running at most, so the patch jobs keep most threads
	int MaxBakesInFlight()
	{
		static const int n = std::max(2, int(std::thread::hardware_concurrency()) / 3);
		return n;
	}

	// a point of the patch, as GetSpherePoint in GeoPatchJobs.cpp places it:
	// patch surface coordinates 0 to 1, continuing past the edges
	inline vector3d PatchPoint(const vector3d *v, const double x, const double y)
	{
		return (v[0] + x * (1.0 - y) * (v[1] - v[0]) + x * y * (v[2] - v[0]) + (1.0 - x) * y * (v[3] - v[0])).Normalized();
	}

	class SurfaceTileJob : public Job {
	public:
		explicit SurfaceTileJob(std::shared_ptr<TerrainSurfaceTiles::Bake> bake) :
			m_bake(bake)
		{}

		void OnRun() override
		{
			PROFILE_SCOPED()
			TerrainSurfaceTiles::Bake &b = *m_bake;
			// the slot's texels (the tile and its border), and one more row of
			// heights around them for the normals
			const int out = b.tileSize + 2 * TerrainSurfaceTiles::TILE_BORDER;
			const int grid = out + 2;
			const int first = TerrainSurfaceTiles::TILE_BORDER + 1; // grid index of the tile's first texel
			std::vector<vector3d> dirs(size_t(grid) * grid);
			std::vector<double> heights(dirs.size());
			for (int y = 0; y < grid; y++) {
				const double v = (y - first + 0.5) / b.tileSize;
				for (int x = 0; x < grid; x++) {
					// the vertices' uv runs against the patch x (GeoPatch::UpdateVBOs)
					const double u = (x - first + 0.5) / b.tileSize;
					dirs[size_t(y) * grid + x] = PatchPoint(b.corners, 1.0 - u, v);
				}
			}

			// band limited to the texel size, as the patches to their vertex spacing
			TerrainNoise::ScopedBandLimit bandLimit(0.5 / b.texelSpacing);
			b.terrain->GetHeights(dirs.data(), heights.data(), dirs.size());
			// the slope the colours see (COLOUR_SLOPE_SCALE), where the tile is
			// finer than that
			std::vector<vector3d> colourNormals;
			const bool fixedSlope = ColourSlopeNormals(b.terrain.Get(), dirs.data(), grid, 1, grid - 1, 1, grid - 1, colourNormals);
			b.colour.resize(size_t(out) * out * 4);
			b.normal.resize(size_t(out) * out);
			BakeSurfaceTexels(b.terrain.Get(), dirs.data(), heights.data(), grid, 1, grid - 1, 1, grid - 1,
				fixedSlope ? colourNormals.data() : nullptr, out, b.colour.data(), b.normal.data(), size_t(out));
			b.done.store(true, std::memory_order_release);
		}

		// picked up by TerrainSurfaceTiles::Update on the main thread
		void OnFinish() override {}

	private:
		std::shared_ptr<TerrainSurfaceTiles::Bake> m_bake;
	};
} // namespace

bool TerrainSurfaceTiles::Enabled()
{
	return Pi::config->Int("TerrainSurfaceTiles", 1) != 0;
}

TerrainSurfaceTiles *TerrainSurfaceTiles::Get(const int tileSize)
{
	if (s_tiles && s_tiles->m_tileSize != tileSize)
		Destroy();
	if (!s_tiles)
		s_tiles.reset(new TerrainSurfaceTiles(tileSize));
	return s_tiles.get();
}

TerrainSurfaceTiles *TerrainSurfaceTiles::Instance()
{
	return s_tiles.get();
}

void TerrainSurfaceTiles::Destroy()
{
	s_tiles.reset();
}

void TerrainSurfaceTiles::ReleaseSlot(const int slot)
{
	if (!s_tiles || slot < 0 || slot >= int(s_tiles->m_owners.size()) || !s_tiles->m_owners[slot])
		return;
	s_tiles->Free(slot);
}

TerrainSurfaceTiles::TerrainSurfaceTiles(const int tileSize) :
	m_tileSize(tileSize),
	m_slotStride(tileSize + 2 * TILE_BORDER),
	m_slotsPerRow(ATLAS_SIZE / (tileSize + 2 * TILE_BORDER))
{
	const vector3f size(float(ATLAS_SIZE), float(ATLAS_SIZE), 0.0f);
	const Graphics::TextureDescriptor desc(Graphics::TEXTURE_RGBA_8888, size, Graphics::LINEAR_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
	const Graphics::TextureDescriptor normalDesc(Graphics::TEXTURE_RGB10_A2, size, Graphics::LINEAR_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
	m_colour.Reset(Pi::renderer->CreateTexture(desc));
	m_normal.Reset(Pi::renderer->CreateTexture(normalDesc));

	const int slots = m_slotsPerRow * m_slotsPerRow;
	m_owners.assign(slots, nullptr);
	m_lastSeen.assign(slots, std::chrono::steady_clock::time_point());
	m_uploaded.assign(slots, std::chrono::steady_clock::time_point());
	m_ready.assign(slots, false);
	m_free.reserve(slots);
	for (int i = slots - 1; i >= 0; i--)
		m_free.push_back(i);
}

TerrainSurfaceTiles::~TerrainSurfaceTiles()
{
	// destroying the handles cancels the queued bakes; running ones keep
	// their data alive until they finish
	m_pending.clear();
	for (GeoPatch *owner : m_owners) {
		if (owner)
			owner->SetSurfaceSlot(-1);
	}
}

vector3f TerrainSurfaceTiles::SlotUV(const int slot) const
{
	const float x = float((slot % m_slotsPerRow) * m_slotStride + TILE_BORDER);
	const float y = float((slot / m_slotsPerRow) * m_slotStride + TILE_BORDER);
	return vector3f(x, y, float(m_tileSize)) / float(ATLAS_SIZE);
}

float TerrainSurfaceTiles::SlotFade(const int slot) const
{
	if (slot < 0 || slot >= int(m_ready.size()) || !m_ready[slot])
		return 0.0f;
	const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_uploaded[slot]).count();
	return std::min(seconds / FADE_SECONDS, 1.0f);
}

int TerrainSurfaceTiles::GetTileCount() const
{
	return int(std::count(m_ready.begin(), m_ready.end(), true));
}

int TerrainSurfaceTiles::Allocate(GeoPatch *patch)
{
	if (m_free.empty()) {
		// take the tile of the patch seen longest ago, if it is out of view
		int slot = -1;
		std::chrono::steady_clock::time_point oldest = std::chrono::steady_clock::now() - EVICT_UNSEEN;
		for (int i = 0; i < int(m_owners.size()); i++) {
			if (m_owners[i] && m_lastSeen[i] < oldest) {
				oldest = m_lastSeen[i];
				slot = i;
			}
		}
		if (slot < 0)
			return -1;
		m_owners[slot]->SetSurfaceSlot(-1);
		Free(slot);
	}
	const int slot = m_free.back();
	m_free.pop_back();
	m_owners[slot] = patch;
	m_lastSeen[slot] = std::chrono::steady_clock::now();
	m_ready[slot] = false;
	patch->SetSurfaceSlot(slot);
	return slot;
}

void TerrainSurfaceTiles::Free(const int slot)
{
	m_owners[slot] = nullptr;
	m_ready[slot] = false;
	// its bake, if any, is dropped (cancelled while still queued)
	m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), [slot](const Pending &p) { return p.slot == slot; }), m_pending.end());
	m_free.push_back(slot);
}

void TerrainSurfaceTiles::UploadFinished()
{
	const int out = m_tileSize + 2 * TILE_BORDER;
	const vector3f size(float(out), float(out), 0.0f);
	int uploads = 0;
	for (auto it = m_pending.begin(); it != m_pending.end() && uploads < UPLOADS_PER_FRAME;) {
		if (!it->bake->done.load(std::memory_order_acquire)) {
			++it;
			continue;
		}
		const int slot = it->slot;
		const vector2f pos(float((slot % m_slotsPerRow) * m_slotStride), float((slot / m_slotsPerRow) * m_slotStride));
		m_colour->Update(it->bake->colour.data(), pos, size, Graphics::TEXTURE_RGBA_8888);
		m_normal->Update(it->bake->normal.data(), pos, size, Graphics::TEXTURE_RGB10_A2);
		m_ready[slot] = true;
		m_uploaded[slot] = std::chrono::steady_clock::now();
		it = m_pending.erase(it);
		uploads++;
	}
}

void TerrainSurfaceTiles::Update(GeoSphere *sphere, const std::vector<std::pair<double, GeoPatch *>> &visible, const double pixelAngle, const double farTexel)
{
	PROFILE_SCOPED()
	UploadFinished();
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

	// patches whose vertices are far enough apart on screen for the tile to
	// show more, where the tile (half the vertex spacing) is finer than the
	// far texture
	std::vector<std::pair<double, GeoPatch *>> wanted;
	for (const auto &v : visible) {
		GeoPatch *patch = v.second;
		if (patch->GetSurfaceSlot() >= 0) {
			m_lastSeen[patch->GetSurfaceSlot()] = now;
			continue;
		}
		const double spacing = patch->GetVertexSpacing();
		const double dist = std::max(std::sqrt(v.first), 1e-9);
		if (spacing / (dist * pixelAngle) >= MIN_VERTEX_PIXELS && (farTexel <= 0.0 || 0.5 * spacing < farTexel))
			wanted.push_back(v);
	}

	// nearest first, a few at a time; one at a time while the patch jobs
	// fill the threads, so the mesh comes first
	int room = MaxBakesInFlight() - int(m_pending.size());
	if (GetQuadPatchJobsInFlight() >= int(std::thread::hardware_concurrency()))
		room = std::min(room, 1 - int(m_pending.size()));
	if (wanted.empty() || room <= 0)
		return;
	const size_t count = std::min(wanted.size(), size_t(room));
	std::partial_sort(wanted.begin(), wanted.begin() + count, wanted.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

	for (size_t i = 0; i < count; i++) {
		GeoPatch *patch = wanted[i].second;
		const int slot = Allocate(patch);
		if (slot < 0)
			break;
		std::shared_ptr<Bake> bake = std::make_shared<Bake>();
		bake->terrain.Reset(sphere->GetTerrain());
		for (int c = 0; c < 4; c++)
			bake->corners[c] = patch->GetCorner(c);
		bake->tileSize = m_tileSize;
		// the tile has twice the patch's vertex gaps (GeoSphere::SetSurfaceTiles)
		bake->texelSpacing = 0.5 * patch->GetVertexSpacing();
		m_pending.push_back({ bake, Pi::GetAsyncJobQueue()->Queue(new SurfaceTileJob(bake)), slot });
	}
}
