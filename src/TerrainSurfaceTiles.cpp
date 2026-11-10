// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainSurfaceTiles.h"

#include "FrontierLog.h"
#include "GameConfig.h"
#include "GeoPatch.h"
#include "GeoPatchJobs.h"
#include "GeoSphere.h"
#include "MathUtil.h"
#include "Pi.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "profiler/Profiler.h"
#include "terrain/Terrain.h"
#include "terrain/TerrainNoise.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {
	// GetBakeStats
	std::atomic<uint64_t> s_bakes(0), s_bakeNanos(0), s_heightsNanos(0), s_slopesNanos(0), s_texelsNanos(0);
	// a fill of fewer tiles than this isn't written to the log: tiles keep coming in a few at a time as the camera moves
	constexpr int FILL_LOG_TILES = 50;

	inline long long NowNanos()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	inline void AddNanos(std::atomic<uint64_t> &to, const long long nanos)
	{
		to.fetch_add(uint64_t(std::max(nanos, 0LL)), std::memory_order_relaxed);
	}

	// largest normal offset stored, must match FAR_NORMAL_RANGE in terrain-far.glsl: 1 holds slopes up to vertical (at
	// 0.5, slopes past about 30 degrees were flattened, and steep crater walls lost their shading on the surface tiles)
	constexpr double NORMAL_RANGE = 1.0;

	// 0.5 + 0.5 * sign(v) * sqrt(|v| / NORMAL_RANGE) in 10 bits: fine steps for
	// the gentle slopes that most of a planet has (8 bits showed as contour
	// lines in the shading when magnified)
	inline uint32_t EncodeNormal(const double v)
	{
		const double s = std::sqrt(std::min(std::abs(v) / NORMAL_RANGE, 1.0));
		return static_cast<uint32_t>(Clamp(std::round(511.5 + (v < 0.0 ? -511.5 : 511.5) * s), 0.0, 1023.0));
	}
} // namespace

void BakeSurfaceTexels(const Terrain *terrain, const vector3d *dirs, const double *heights, const int gridWidth,
	const int x0, const int x1, const int y0, const int y1, const vector3d *colourNormals, const int colourPitch,
	uint8_t *colour, uint32_t *normal, const size_t rowPitch)
{
	auto position = [&](const size_t i) { return dirs[i] * (1.0 + heights[i]); };
	for (int y = y0; y < y1; y++) {
		uint8_t *col = colour + size_t(y - y0) * rowPitch * 4;
		uint32_t *nrm = normal + size_t(y - y0) * rowPitch;
		for (int x = x0; x < x1; x++) {
			const size_t i = size_t(y) * gridWidth + x;
			const vector3d &p = dirs[i];
			const double height = heights[i];
			vector3d n = (position(i + 1) - position(i - 1)).Cross(position(i + gridWidth) - position(i - gridWidth)).Normalized();
			if (n.Dot(p) < 0.0)
				n = -n;

			const vector3d &slope = colourNormals ? colourNormals[size_t(y - y0) * colourPitch + (x - x0)] : n;
			const vector3d c = terrain->GetColor(p, height, slope);
			col[0] = static_cast<uint8_t>(Clamp(c.x * 255.0, 0.0, 255.0));
			col[1] = static_cast<uint8_t>(Clamp(c.y * 255.0, 0.0, 255.0));
			col[2] = static_cast<uint8_t>(Clamp(c.z * 255.0, 0.0, 255.0));
			col[3] = EncodeAltitude(SurfaceAltitude(terrain, p, height, c));
			col += 4;

			const vector3d offset = n - p;
			*(nrm++) = EncodeNormal(offset.x) | (EncodeNormal(offset.y) << 10) | (EncodeNormal(offset.z) << 20) | (3u << 30);
		}
	}
}

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

	// bakes queued or running at most: the threads the patch jobs leave free,
	// and always a few, but one at a time while they fill the threads, so the
	// mesh comes first
	int MaxBakesInFlight()
	{
		static const int threads = int(std::thread::hardware_concurrency());
		const int patchJobs = GetQuadPatchJobsInFlight();
		if (patchJobs >= threads)
			return 1;
		return std::max({ 2, threads / 3, threads - 1 - patchJobs });
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
			const long long start = NowNanos();
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
			const long long heightsStart = NowNanos();
			b.terrain->GetHeights(dirs.data(), heights.data(), dirs.size());
			const long long slopesStart = NowNanos();
			// the slope the colours see (COLOUR_SLOPE_SCALE), where the tile is
			// finer than that
			std::vector<vector3d> colourNormals;
			const bool fixedSlope = ColourSlopeNormals(b.terrain.Get(), dirs.data(), grid, 1, grid - 1, 1, grid - 1, colourNormals);
			const long long texelsStart = NowNanos();
			b.colour.resize(size_t(out) * out * 4);
			b.normal.resize(size_t(out) * out);
			BakeSurfaceTexels(b.terrain.Get(), dirs.data(), heights.data(), grid, 1, grid - 1, 1, grid - 1,
				fixedSlope ? colourNormals.data() : nullptr, out, b.colour.data(), b.normal.data(), size_t(out));
			const long long end = NowNanos();
			AddNanos(s_heightsNanos, slopesStart - heightsStart);
			AddNanos(s_slopesNanos, texelsStart - slopesStart);
			AddNanos(s_texelsNanos, end - texelsStart);
			AddNanos(s_bakeNanos, end - start);
			s_bakes.fetch_add(1, std::memory_order_relaxed);
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

int TerrainSurfaceTiles::GetWaitingCount() const
{
	// (each planet in view reports in turn: a planet with none waiting mustn't hide the one that has)
	return std::chrono::steady_clock::now() - m_waitingSeen < std::chrono::milliseconds(100) ? m_waiting : 0;
}

TerrainSurfaceTiles::BakeStats TerrainSurfaceTiles::GetBakeStats()
{
	BakeStats stats;
	stats.bakes = s_bakes;
	stats.nanos = s_bakeNanos;
	stats.heightsNanos = s_heightsNanos;
	stats.slopesNanos = s_slopesNanos;
	stats.texelsNanos = s_texelsNanos;
	return stats;
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
		m_lastUpload = m_uploaded[slot];
		it = m_pending.erase(it);
		uploads++;
	}
	if (m_filling)
		m_fillUploads += uploads;
}

void TerrainSurfaceTiles::Update(GeoSphere *sphere, const std::vector<std::pair<double, GeoPatch *>> &visible, const double pixelAngle)
{
	PROFILE_SCOPED()
	UploadFinished();
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

	// patches whose vertices are far enough apart on screen for the tile to
	// show more
	std::vector<std::pair<double, GeoPatch *>> wanted;
	for (const auto &v : visible) {
		GeoPatch *patch = v.second;
		// the parent's tile stays while its children are drawn: they blend towards it with distance (GeoPatch::
		// SetMorphConstants), and show it again when they merge back
		if (const GeoPatch *parent = patch->GetParent()) {
			if (parent->GetSurfaceSlot() >= 0 && parent->GetSurfaceSlot() < int(m_lastSeen.size()))
				m_lastSeen[parent->GetSurfaceSlot()] = now;
		}
		if (patch->GetSurfaceSlot() >= 0) {
			m_lastSeen[patch->GetSurfaceSlot()] = now;
			continue;
		}
		const double spacing = patch->GetVertexSpacing();
		const double dist = std::max(std::sqrt(v.first), 1e-9);
		if (spacing / (dist * pixelAngle) >= MIN_VERTEX_PIXELS && !patch->IsDeepUnderSea())
			wanted.push_back(v);
	}
	if (!wanted.empty()) {
		m_waiting = int(wanted.size());
		m_waitingSeen = now;
		if (!m_filling) {
			m_filling = true;
			m_fillStart = now;
			m_fillUploads = 0;
			m_fillBakes = GetBakeStats();
		}
	} else if (m_filling && m_pending.empty() && GetWaitingCount() == 0) {
		// over: from the first tile wanted to the last uploaded
		m_filling = false;
		if (m_fillUploads >= FILL_LOG_TILES) {
			const BakeStats stats = GetBakeStats();
			const uint64_t bakes = stats.bakes - m_fillBakes.bakes;
			const double perBake = bakes ? double(stats.nanos - m_fillBakes.nanos) / double(bakes) / 1e6 : 0.0;
			const double bakeNanos = double(std::max<uint64_t>(stats.nanos - m_fillBakes.nanos, 1));
			const double seconds = std::chrono::duration<double>(std::max(m_waitingSeen, m_lastUpload) - m_fillStart).count();
			char line[320];
			std::snprintf(line, sizeof(line), "surface tiles filled: %d tiles in %.2f s, %llu bakes at %.1f ms of thread time (heights %.0f%%, colour slopes %.0f%%, texels %.0f%%), %d texels across",
				m_fillUploads, seconds, (unsigned long long)bakes, perBake,
				100.0 * double(stats.heightsNanos - m_fillBakes.heightsNanos) / bakeNanos,
				100.0 * double(stats.slopesNanos - m_fillBakes.slopesNanos) / bakeNanos,
				100.0 * double(stats.texelsNanos - m_fillBakes.texelsNanos) / bakeNanos, m_tileSize);
			FrontierLog::Note(line);
		}
	}

	// nearest first
	const int room = MaxBakesInFlight() - int(m_pending.size());
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
