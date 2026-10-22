// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "PlanetClouds.h"

#include "Game.h"
#include "MathUtil.h"
#include "Pi.h"
#include "core/StringHash.h"
#include "galaxy/SystemBody.h"
#include "graphics/Material.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "terrain/Terrain.h"

#include <algorithm>
#include <cmath>

namespace {
	static const size_t s_cloudDataName = "CloudData"_hash;
	static const size_t s_cloudHeightTexName = "cloudHeightTex"_hash;

	// height map resolution, must match CLOUD_HEIGHTMAP_* in cloud-config.glsl
	static const int HEIGHTMAP_WIDTH = 512;
	static const int HEIGHTMAP_HEIGHT = 256;
	// terrain samples per texel edge (2x2 per texel, then a 3x3 max filter)
	static const int HEIGHTMAP_SUBSAMPLES = 2;
	// only bake when the camera is this close (planet radii from the centre)
	static const double HEIGHTMAP_BAKE_DISTANCE = 20.0;

	// time is split so the shader gets full float precision
	static const double CLOUD_TIME_WRAP = 1048576.0; // 2^20 s

	static RefCountedPtr<Graphics::Texture> s_fallbackTexture;

	Graphics::Texture *GetFallbackTexture()
	{
		if (!s_fallbackTexture) {
			const vector3f size(1.0f, 1.0f, 0.0f);
			const Graphics::TextureDescriptor desc(Graphics::TEXTURE_R32F, size, Graphics::NEAREST_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
			s_fallbackTexture.Reset(Pi::renderer->CreateTexture(desc));
			const float zero = 0.0f;
			s_fallbackTexture->Update(&zero, size, Graphics::TEXTURE_R32F);
		}
		return s_fallbackTexture.Get();
	}

	void SetAxis(float out[4], const vector3d &v, float w)
	{
		out[0] = float(v.x);
		out[1] = float(v.y);
		out[2] = float(v.z);
		out[3] = w;
	}

	void SetVec4(float out[4], double x, double y, double z, double w)
	{
		out[0] = float(x);
		out[1] = float(y);
		out[2] = float(z);
		out[3] = float(w);
	}

	// Samples the terrain on an equirectangular grid (lon = atan2(x, z),
	// lat = atan2(y, |xz|), matching Body::GetGroundPosition) and stores the
	// conservative (maximum) height of each texel in metres.
	class HeightMapBakeJob : public Job {
	public:
		HeightMapBakeJob(RefCountedPtr<Terrain> terrain, double radius, std::shared_ptr<PlanetClouds::CloudHeightMap::BakeResult> result) :
			m_terrain(terrain),
			m_radius(radius),
			m_result(result)
		{}

		void OnRun() override
		{
			const int sw = HEIGHTMAP_WIDTH * HEIGHTMAP_SUBSAMPLES;
			const int sh = HEIGHTMAP_HEIGHT * HEIGHTMAP_SUBSAMPLES;

			std::vector<float> texels(HEIGHTMAP_WIDTH * HEIGHTMAP_HEIGHT, 0.0f);
			std::vector<vector3d> points(sw);
			std::vector<double> heights(sw);

			for (int row = 0; row < sh; row++) {
				const double lat = ((row + 0.5) / sh) * M_PI - 0.5 * M_PI;
				const double clat = cos(lat), slat = sin(lat);
				for (int col = 0; col < sw; col++) {
					const double lon = ((col + 0.5) / sw) * 2.0 * M_PI - M_PI;
					points[col] = vector3d(clat * sin(lon), slat, clat * cos(lon));
				}
				m_terrain->GetHeights(points.data(), heights.data(), sw);

				float *texRow = &texels[(row / HEIGHTMAP_SUBSAMPLES) * HEIGHTMAP_WIDTH];
				for (int col = 0; col < sw; col++) {
					const float h = float(std::max(0.0, heights[col]) * m_radius);
					float &t = texRow[col / HEIGHTMAP_SUBSAMPLES];
					t = std::max(t, h);
				}
			}

			// 3x3 max filter so the floor stays conservative between texels
			// (longitude wraps, latitude clamps)
			std::vector<float> &out = m_result->heights;
			out.assign(texels.size(), 0.0f);
			for (int y = 0; y < HEIGHTMAP_HEIGHT; y++) {
				for (int x = 0; x < HEIGHTMAP_WIDTH; x++) {
					float m = 0.0f;
					for (int dy = -1; dy <= 1; dy++) {
						const int yy = Clamp(y + dy, 0, HEIGHTMAP_HEIGHT - 1);
						for (int dx = -1; dx <= 1; dx++) {
							const int xx = (x + dx + HEIGHTMAP_WIDTH) % HEIGHTMAP_WIDTH;
							m = std::max(m, texels[yy * HEIGHTMAP_WIDTH + xx]);
						}
					}
					out[y * HEIGHTMAP_WIDTH + x] = m;
				}
			}

			m_result->done.store(true, std::memory_order_release);
		}

		// results are picked up by CloudHeightMap::IsReady on the main thread
		void OnFinish() override {}

	private:
		RefCountedPtr<Terrain> m_terrain;
		double m_radius;
		std::shared_ptr<PlanetClouds::CloudHeightMap::BakeResult> m_result;
	};
} // namespace

namespace PlanetClouds {

	CloudHeightMap::CloudHeightMap(RefCountedPtr<Terrain> terrain, double radius) :
		m_result(std::make_shared<BakeResult>())
	{
		m_job = Pi::GetAsyncJobQueue()->Queue(new HeightMapBakeJob(terrain, radius, m_result));
	}

	CloudHeightMap::~CloudHeightMap()
	{
		// destroying m_job cancels the job if it is still queued; a running
		// job keeps m_result alive through its own shared_ptr
	}

	bool CloudHeightMap::IsReady()
	{
		if (!m_ready && m_result->done.load(std::memory_order_acquire)) {
			const vector3f size(float(HEIGHTMAP_WIDTH), float(HEIGHTMAP_HEIGHT), 0.0f);
			const Graphics::TextureDescriptor desc(Graphics::TEXTURE_R32F, size, Graphics::LINEAR_REPEAT, false, false, false, 0, Graphics::TEXTURE_2D);
			m_texture.Reset(Pi::renderer->CreateTexture(desc));
			m_texture->Update(m_result->heights.data(), size, Graphics::TEXTURE_R32F);
			m_result->heights.clear();
			m_result->heights.shrink_to_fit();
			m_ready = true;
		}
		return m_ready;
	}

	Graphics::Texture *CloudHeightMap::GetTexture()
	{
		return IsReady() ? m_texture.Get() : GetFallbackTexture();
	}

	bool WantsHeightMap(const SystemBody *sbody, const vector3d &campos)
	{
		return sbody->GetType() == SystemBody::TYPE_PLANET_TERRESTRIAL &&
			sbody->HasAtmosphere() &&
			campos.LengthSqr() < HEIGHTMAP_BAKE_DISTANCE * HEIGHTMAP_BAKE_DISTANCE;
	}

	void FillCloudData(CloudDataBlock &out, const SystemBody *sbody, const matrix4x4d &trans, double maxFeatureHeight, CloudHeightMap *heightMap)
	{
		// 1 = height texture ready, 0.5 = still baking (shader uses the max
		// terrain height), 0 = no map because the camera is far away
		const float heightState = heightMap ? (heightMap->IsReady() ? 1.0f : 0.5f) : 0.0f;

		// planet axes in view space: the rotation part of the planet model-view matrix
		SetAxis(out.axisX, trans.ApplyRotationOnly(vector3d(1.0, 0.0, 0.0)).NormalizedSafe(), heightState);
		SetAxis(out.axisY, trans.ApplyRotationOnly(vector3d(0.0, 1.0, 0.0)).NormalizedSafe(), 0.0f);
		SetAxis(out.axisZ, trans.ApplyRotationOnly(vector3d(0.0, 0.0, 1.0)).NormalizedSafe(), 0.0f);

		const double radius = sbody->GetRadius();
		SetVec4(out.atmos, sbody->GetAverageTemp(), sbody->GetAtmSurfacePressure(), sbody->GetAtmSurfaceDensity(), sbody->CalcSurfaceGravity());
		SetVec4(out.geom, radius, sbody->GetTropopause(), sbody->GetAtmRadius(), maxFeatureHeight * radius);
		SetVec4(out.comp, sbody->GetVolatileLiquid(), sbody->GetVolatileIces(), sbody->GetAtmosOxidizing(), sbody->GetVolcanicity());

		// the orbit period only matters for tidal locking to a star (a moon
		// locked to its planet still has a normal day-night cycle)
		const SystemBody *parent = sbody->GetParent();
		const bool orbitsStar = parent && parent->GetSuperType() == SystemBody::SUPERTYPE_STAR;
		const double orbitDays = orbitsStar ? sbody->GetOrbit().Period() / (60.0 * 60.0 * 24.0) : 0.0;
		const bool enabled = sbody->GetType() == SystemBody::TYPE_PLANET_TERRESTRIAL && Pi::game;
		const double accel = Pi::game ? Pi::game->GetTimeAccelRate() : 0.0;
		SetVec4(out.spin, sbody->GetRotationPeriodInDays(), orbitDays, accel, enabled ? 1.0 : 0.0);

		const double t = Pi::game ? Pi::game->GetTime() : 0.0;
		const Uint32 seed = sbody->GetSeed();
		SetVec4(out.time, std::fmod(t, CLOUD_TIME_WRAP), std::floor(t / CLOUD_TIME_WRAP), double(seed & 0xFFFF), double(seed >> 16));
	}

	void SetMaterialCloudData(Graphics::Material *mat, CloudDataBlock &data, CloudHeightMap *heightMap)
	{
		mat->SetBufferDynamic(s_cloudDataName, &data);
		mat->SetTexture(s_cloudHeightTexName, heightMap ? heightMap->GetTexture() : GetFallbackTexture());
	}

	void SetDisabledCloudData(Graphics::Material *mat)
	{
		CloudDataBlock data {};
		mat->SetBufferDynamic(s_cloudDataName, &data);
		mat->SetTexture(s_cloudHeightTexName, GetFallbackTexture());
	}

	void Uninit()
	{
		s_fallbackTexture.Reset();
	}

} // namespace PlanetClouds
