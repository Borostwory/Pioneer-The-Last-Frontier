// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainFarTexture.h"

#include "GameConfig.h"
#include "GeoPatchJobs.h"
#include "MathUtil.h"
#include "Pi.h"
#include "core/Log.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "profiler/Profiler.h"
#include "terrain/Terrain.h"
#include "terrain/TerrainNoise.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>

struct TerrainFarTexture::BakeState {
	RefCountedPtr<Terrain> terrain;
	int size = 0;
	// per cube face, rows of RGBA texels, written by the jobs (each its own rows)
	std::unique_ptr<uint8_t[]> colour[6];
	std::unique_ptr<uint8_t[]> normal[6];
	std::atomic<int> stripsDone { 0 };
};

namespace {
	// rows of a cube face per job: some 30-60 ms of work at 1024 texels
	static const int STRIP_ROWS = 32;
	// texels per terrain call, each band limited to its own texel size
	static const int SEGMENT = 64;
	static const float FADE_SECONDS = 1.5f;
	// largest normal offset stored, must match FAR_NORMAL_RANGE in terrain-far.glsl
	static const double NORMAL_RANGE = 0.5;

	// textures being baked (main thread only)
	static int s_baking = 0;
	static std::string s_lastBakeInfo;

	// strips queued or running at most, so the patch jobs keep most threads
	int MaxStripsInFlight()
	{
		static const int n = std::max(2, int(std::thread::hardware_concurrency()) / 3);
		return n;
	}

	// Planet-fixed direction through the point (s, t), 0 to 1, of a cube face,
	// in the OpenGL face order (+X, -X, +Y, -Y, +Z, -Z) and orientation, so
	// the shaders look the texture up with the direction itself. Points
	// outside 0..1 continue past the face edge.
	vector3d FaceDirection(const int face, const double s, const double t)
	{
		const double sc = 2.0 * s - 1.0, tc = 2.0 * t - 1.0;
		switch (face) {
		case 0: return vector3d(1.0, -tc, -sc).Normalized();
		case 1: return vector3d(-1.0, -tc, sc).Normalized();
		case 2: return vector3d(sc, 1.0, tc).Normalized();
		case 3: return vector3d(sc, -1.0, -tc).Normalized();
		case 4: return vector3d(sc, -tc, 1.0).Normalized();
		default: return vector3d(-sc, -tc, -1.0).Normalized();
		}
	}

	// highest noise frequency the texels around (s, t) can show, as
	// NyquistFrequency() in GeoPatchJobs.cpp does for the vertex grids
	double TexelMaxFrequency(const int face, const double s, const double t, const double step)
	{
		const vector3d p = FaceDirection(face, s, t);
		const double spacing = std::max((FaceDirection(face, s + step, t) - p).Length(), (FaceDirection(face, s, t + step) - p).Length());
		return 0.5 / spacing;
	}

	// 0.5 + 0.5 * sign(v) * sqrt(|v| / NORMAL_RANGE): fine steps for the gentle
	// slopes that most of a planet has
	inline uint8_t EncodeNormal(const double v)
	{
		const double s = std::sqrt(std::min(std::abs(v) / NORMAL_RANGE, 1.0));
		return static_cast<uint8_t>(Clamp(std::round(127.5 + (v < 0.0 ? -127.5 : 127.5) * s), 0.0, 255.0));
	}

	// Rows [row0, row0 + rows) of a face. The heights have a border of one
	// texel around them for the normals.
	void BakeStrip(TerrainFarTexture::BakeState &state, const int face, const int row0, const int rows)
	{
		const Terrain *terrain = state.terrain.Get();
		const int n = state.size;
		const double step = 1.0 / double(n);
		const int w = n + 2, h = rows + 2;

		std::vector<vector3d> dirs(size_t(w) * h);
		std::vector<double> heights(size_t(w) * h);
		for (int y = 0; y < h; y++) {
			const double t = (row0 - 1 + y + 0.5) * step;
			for (int x = 0; x < w; x++)
				dirs[size_t(y) * w + x] = FaceDirection(face, (x - 1 + 0.5) * step, t);
		}

		// heights, band limited to the texel size like the patches to their
		// vertex spacing (texels are up to 2.6x smaller in the face corners)
		for (int y = 0; y < h; y++) {
			const double t = (row0 - 1 + y + 0.5) * step;
			for (int x0 = 0; x0 < w; x0 += SEGMENT) {
				const int count = std::min(SEGMENT, w - x0);
				const double s = (x0 - 1 + 0.5 * count) * step;
				TerrainNoise::ScopedBandLimit bandLimit(TexelMaxFrequency(face, s, t, step));
				const size_t i = size_t(y) * w + x0;
				terrain->GetHeights(&dirs[i], &heights[i], count);
			}
		}

		auto position = [&](const size_t i) { return dirs[i] * (1.0 + heights[i]); };

		// normals from the neighbouring texels, then colours and altitudes as
		// the patches make them (setSurface in GeoPatchJobs.cpp)
		for (int y = 1; y <= rows; y++) {
			const int row = row0 + y - 1;
			const double t = (row + 0.5) * step;
			uint8_t *col = &state.colour[face][size_t(row) * n * 4];
			uint8_t *nrm = &state.normal[face][size_t(row) * n * 4];
			for (int x0 = 1; x0 <= n; x0 += SEGMENT) {
				const int end = std::min(x0 + SEGMENT, n + 1);
				const double s = (0.5 * (x0 + end) - 1.0) * step;
				TerrainNoise::ScopedBandLimit bandLimit(TexelMaxFrequency(face, s, t, step));
				for (int x = x0; x < end; x++) {
					const size_t i = size_t(y) * w + x;
					const vector3d &p = dirs[i];
					const double height = heights[i];
					vector3d normal = (position(i + 1) - position(i - 1)).Cross(position(i + w) - position(i - w)).Normalized();
					if (normal.Dot(p) < 0.0)
						normal = -normal;

					const vector3d colour = terrain->GetColor(p, height, normal);
					col[0] = static_cast<uint8_t>(Clamp(colour.x * 255.0, 0.0, 255.0));
					col[1] = static_cast<uint8_t>(Clamp(colour.y * 255.0, 0.0, 255.0));
					col[2] = static_cast<uint8_t>(Clamp(colour.z * 255.0, 0.0, 255.0));
					col[3] = EncodeAltitude(SurfaceAltitude(terrain, p, height, colour));
					col += 4;

					const vector3d offset = normal - p;
					nrm[0] = EncodeNormal(offset.x);
					nrm[1] = EncodeNormal(offset.y);
					nrm[2] = EncodeNormal(offset.z);
					nrm[3] = 255;
					nrm += 4;
				}
			}
		}
	}

	class FarStripJob : public Job {
	public:
		FarStripJob(std::shared_ptr<TerrainFarTexture::BakeState> state, const int face, const int row0, const int rows) :
			m_state(state),
			m_face(face),
			m_row0(row0),
			m_rows(rows)
		{}

		void OnRun() override
		{
			PROFILE_SCOPED()
			BakeStrip(*m_state, m_face, m_row0, m_rows);
			m_state->stripsDone.fetch_add(1, std::memory_order_release);
		}

		// picked up by TerrainFarTexture::Update on the main thread
		void OnFinish() override {}

	private:
		std::shared_ptr<TerrainFarTexture::BakeState> m_state;
		const int m_face, m_row0, m_rows;
	};
} // namespace

int TerrainFarTexture::ConfiguredSize()
{
	if (Pi::config->Int("PlanetFarTexture", 1) == 0)
		return 0;
	const int wanted = Clamp(Pi::config->Int("PlanetFarTextureSize", 1024), 256, 2048);
	int size = 256;
	while (size * 2 <= wanted)
		size *= 2;
	return size;
}

bool TerrainFarTexture::CanStart()
{
	return s_baking == 0;
}

const std::string &TerrainFarTexture::GetLastBakeInfo()
{
	return s_lastBakeInfo;
}

TerrainFarTexture::TerrainFarTexture(RefCountedPtr<Terrain> terrain, const std::string &bodyName, const double radius, const int size) :
	m_state(std::make_shared<BakeState>()),
	m_bodyName(bodyName),
	m_radius(radius),
	m_size(size),
	m_start(std::chrono::steady_clock::now())
{
	m_state->terrain = terrain;
	m_state->size = size;
	// left uninitialised: the jobs write every texel
	const size_t bytes = size_t(size) * size * 4;
	for (int face = 0; face < 6; face++) {
		m_state->colour[face].reset(new uint8_t[bytes]);
		m_state->normal[face].reset(new uint8_t[bytes]);
	}
	s_baking++;
}

TerrainFarTexture::~TerrainFarTexture()
{
	// destroying the handles cancels the strips still queued; running ones
	// keep the bake state alive until they finish
	m_jobs.clear();
	if (m_stage != Stage::Ready)
		s_baking--;
}

void TerrainFarTexture::Update(const float frameTime)
{
	PROFILE_SCOPED()
	const vector3f dataSize(float(m_size), float(m_size), 0.0f);
	switch (m_stage) {
	case Stage::Baking: {
		const int stripsPerFace = (m_size + STRIP_ROWS - 1) / STRIP_ROWS;
		const int total = 6 * stripsPerFace;
		const int done = m_state->stripsDone.load(std::memory_order_acquire);
		while (m_queued < total && m_queued - done < MaxStripsInFlight()) {
			const int face = m_queued / stripsPerFace;
			const int row0 = (m_queued % stripsPerFace) * STRIP_ROWS;
			m_jobs.push_back(Pi::GetAsyncJobQueue()->Queue(new FarStripJob(m_state, face, row0, std::min(STRIP_ROWS, m_size - row0))));
			m_queued++;
		}
		if (done < total)
			break;

		m_jobs.clear();
		const Graphics::TextureDescriptor desc(Graphics::TEXTURE_RGBA_8888, dataSize, Graphics::LINEAR_CLAMP, true, false, true, 0, Graphics::TEXTURE_CUBE_MAP);
		m_colour.Reset(Pi::renderer->CreateTexture(desc));
		m_normal.Reset(Pi::renderer->CreateTexture(desc));
		m_stage = Stage::Uploading;
	} break;

	case Stage::Uploading: {
		// one face a frame (4 MB at 1024 texels), colour and normal
		// alternating; the mipmaps with the last face of each
		const int face = m_uploaded / 2;
		const bool last = face == 5;
		if (m_uploaded % 2 == 0) {
			m_colour->UpdateCubeFace(face, m_state->colour[face].get(), dataSize, Graphics::TEXTURE_RGBA_8888, last);
			m_state->colour[face].reset();
		} else {
			m_normal->UpdateCubeFace(face, m_state->normal[face].get(), dataSize, Graphics::TEXTURE_RGBA_8888, last);
			m_state->normal[face].reset();
		}
		if (++m_uploaded < 12)
			break;

		m_state.reset();
		m_stage = Stage::Ready;
		s_baking--;
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
		s_lastBakeInfo = fmt::format("{}: {} texels per face in {:.1f} s", m_bodyName, m_size, seconds);
		Log::Verbose("Far terrain texture for {}: {} texels per face in {:.2f} s\n", m_bodyName, m_size, seconds);
	} break;

	case Stage::Ready:
		m_fade = std::min(1.0f, m_fade + frameTime / FADE_SECONDS);
		break;
	}
}
