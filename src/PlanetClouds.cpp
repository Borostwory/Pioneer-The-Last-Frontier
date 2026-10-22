// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "PlanetClouds.h"

#include "Game.h"
#include "GameConfig.h"
#include "MathUtil.h"
#include "Pi.h"
#include "core/Log.h"
#include "core/StringHash.h"
#include "galaxy/SystemBody.h"
#include "graphics/Material.h"
#include "graphics/RenderState.h"
#include "graphics/RenderTarget.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "graphics/Types.h"
#include "graphics/VertexArray.h"
#include "graphics/VertexBuffer.h"
#include "terrain/Terrain.h"

#include <algorithm>
#include <cmath>

namespace {
	static const size_t s_cloudDataName = "CloudData"_hash;
	static const size_t s_cloudPassDataName = "CloudPassData"_hash;
	static const size_t s_cloudHeightTexName = "cloudHeightTex"_hash;
	static const size_t s_sceneDepthName = "sceneDepth"_hash;
	static const size_t s_sceneDistName = "sceneDist"_hash;
	static const size_t s_cloudTexName = "cloudTex"_hash;

	// height map resolution, must match the clamp in cloudTerrainHeight (cloud-march.glsl)
	static const int HEIGHTMAP_WIDTH = 512;
	static const int HEIGHTMAP_HEIGHT = 256;
	// terrain samples per texel edge (2x2 per texel, then a 3x3 max filter)
	static const int HEIGHTMAP_SUBSAMPLES = 2;
	// only bake when the camera is this close (planet radii from the centre)
	static const double HEIGHTMAP_BAKE_DISTANCE = 20.0;

	// time is split so the shader gets full float precision
	static const double CLOUD_TIME_WRAP = 1048576.0; // 2^20 s

	// the cloud clock never follows game time faster than this multiple of
	// real time, so high time acceleration does not make the clouds flicker
	static const double CLOUD_MAX_TIME_RATE = 100.0;

	// Settings (config.ini, also in the video settings window):
	//   CloudsEnabled    0/1
	//   CloudResolution  0..4: cloud buffer height 360, 540, 720, 1080 lines, or full
	//   CloudSoftness    0..2: upsampling kernel radius
	//   CloudSpeed       1..100: cloud motion and evolution relative to the real wind
	//   CloudDensityLow/Medium/High  0..300 %: amount of cloud per layer
	//   CloudSizeLow/Medium/High     25..400 %: size of individual clouds per layer
	static const int CLOUD_RESOLUTION_LINES[] = { 360, 540, 720, 1080, 0 };

	// planets further away than this (planet radii) are not given a pass
	static const float CLOUD_PASS_MAX_DISTANCE = 2000.0f;

	// Must match the CloudPassData block in cloud_pass.frag / cloud_composite.frag
	struct CloudPassBlock {
		float center[4]; // xyz: planet centre in view space [planet radii]
		float proj[4];	 // x: P[0], y: P[5] (1 / tan of the half fov), z: near plane [m], w: resolution divisor
		float size[4];	 // xy: cloud buffer size, zw: full size [pixels]
		float atmos[4];	 // rgb: atmosphere colour, w: softness 0..2
	};
	static_assert(sizeof(CloudPassBlock) == 64, "CloudPassBlock must match the std140 CloudPassData block");

	struct PassEntry {
		PlanetClouds::CloudDataBlock cloudData;
		vector3f center;
		Color4f atmosColor;
		RefCountedPtr<Graphics::Texture> heightTex;
	};

	struct PassResources {
		std::unique_ptr<Graphics::RenderTarget> distTarget;	 // full size, R32F view distance
		std::unique_ptr<Graphics::RenderTarget> cloudTarget; // reduced size, premultiplied clouds
		RefCountedPtr<Graphics::Material> distMaterial;
		RefCountedPtr<Graphics::Material> passMaterial;
		RefCountedPtr<Graphics::Material> compositeMaterial;
		std::unique_ptr<Graphics::VertexArray> triangle;
		int width = 0, height = 0, divisor = 0;
	};

	static RefCountedPtr<Graphics::Texture> s_fallbackTexture;
	static std::unique_ptr<PassResources> s_pass;
	static std::vector<PassEntry> s_queue;
	static bool s_warnedMSAA = false;

	// cloud clock
	static const Game *s_clockGame = nullptr;
	static double s_cloudTime = 0.0;
	static double s_lastGameTime = 0.0;

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

	// Advances the cloud clock with game time (at most CLOUD_MAX_TIME_RATE
	// times real time), multiplied by the CloudSpeed setting. A new game, a
	// loaded game or a large time jump (hyperspace) re-synchronises it with
	// game time.
	void AdvanceCloudClock()
	{
		if (!Pi::game) {
			s_clockGame = nullptr;
			return;
		}
		const double t = Pi::game->GetTime();
		if (s_clockGame != Pi::game) {
			s_clockGame = Pi::game;
			s_cloudTime = t;
			s_lastGameTime = t;
			return;
		}
		const double dGame = t - s_lastGameTime;
		s_lastGameTime = t;
		if (dGame < 0.0 || dGame > 86400.0) {
			s_cloudTime = t;
			return;
		}
		const double maxStep = CLOUD_MAX_TIME_RATE * std::max(double(Pi::GetFrameTime()), 0.0);
		const double speed = Clamp(Pi::config->Int("CloudSpeed", 10), 1, 100);
		s_cloudTime += std::min(dGame, maxStep) * speed;
	}

	// a per-layer percentage setting as a multiplier
	double LayerPercent(const char *key, int lo, int hi)
	{
		return Clamp(Pi::config->Int(key, 100), lo, hi) / 100.0;
	}

	bool CloudsEnabled()
	{
		return Pi::config->Int("CloudsEnabled", 1) != 0;
	}

	int ChooseDivisor(int height)
	{
		const int level = Clamp(Pi::config->Int("CloudResolution", 1), 0, 4);
		const int lines = CLOUD_RESOLUTION_LINES[level];
		if (lines <= 0)
			return 1;
		const int div = int(std::lround(double(height) / lines));
		return Clamp(div, 1, 8);
	}

	void EnsurePassResources(Graphics::Renderer *r, int width, int height)
	{
		const int divisor = ChooseDivisor(height);
		if (s_pass && s_pass->width == width && s_pass->height == height && s_pass->divisor == divisor)
			return;

		if (!s_pass) {
			s_pass.reset(new PassResources());

			const Graphics::VertexFormatDesc vfmt = Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0);

			Graphics::RenderStateDesc rsd;
			rsd.blendMode = Graphics::BLEND_SOLID;
			rsd.cullMode = Graphics::CULL_NONE;
			rsd.depthTest = false;
			rsd.depthWrite = false;

			Graphics::MaterialDescriptor plainDesc;
			s_pass->distMaterial.Reset(r->CreateMaterial("cloud_depth", plainDesc, rsd, vfmt));

			Graphics::MaterialDescriptor litDesc;
			litDesc.lighting = true;
			s_pass->passMaterial.Reset(r->CreateMaterial("cloud_pass", litDesc, rsd, vfmt));

			Graphics::RenderStateDesc blendRsd = rsd;
			blendRsd.blendMode = Graphics::BLEND_ALPHA_PREMULT;
			s_pass->compositeMaterial.Reset(r->CreateMaterial("cloud_composite", plainDesc, blendRsd, vfmt));

			// one triangle covering the screen (clip-space positions)
			s_pass->triangle.reset(new Graphics::VertexArray(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0, 3));
			s_pass->triangle->Add(vector3f(-1.0f, -1.0f, 0.0f), vector2f(0.0f, 0.0f));
			s_pass->triangle->Add(vector3f(3.0f, -1.0f, 0.0f), vector2f(2.0f, 0.0f));
			s_pass->triangle->Add(vector3f(-1.0f, 3.0f, 0.0f), vector2f(0.0f, 2.0f));
		}

		s_pass->width = width;
		s_pass->height = height;
		s_pass->divisor = divisor;

		const Graphics::RenderTargetDesc distDesc(Uint16(width), Uint16(height), Graphics::TEXTURE_R32F, Graphics::TEXTURE_NONE, false, 0);
		s_pass->distTarget.reset(r->CreateRenderTarget(distDesc));

		const int cw = (width + divisor - 1) / divisor;
		const int ch = (height + divisor - 1) / divisor;
		const Graphics::RenderTargetDesc cloudDesc(Uint16(cw), Uint16(ch), Graphics::TEXTURE_RGBA_8888, Graphics::TEXTURE_NONE, false, 0);
		s_pass->cloudTarget.reset(r->CreateRenderTarget(cloudDesc));

		Log::Info("Cloud pass: {}x{} cloud buffer for {}x{} screen (divisor {})\n", cw, ch, width, height, divisor);
	}
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
		const bool enabled = sbody->GetType() == SystemBody::TYPE_PLANET_TERRESTRIAL && Pi::game && CloudsEnabled();
		const double accel = Pi::game ? Pi::game->GetTimeAccelRate() : 0.0;
		SetVec4(out.spin, sbody->GetRotationPeriodInDays(), orbitDays, accel, enabled ? 1.0 : 0.0);

		const double t = s_clockGame ? s_cloudTime : (Pi::game ? Pi::game->GetTime() : 0.0);
		const Uint32 seed = sbody->GetSeed();
		SetVec4(out.time, std::fmod(t, CLOUD_TIME_WRAP), std::floor(t / CLOUD_TIME_WRAP), double(seed & 0xFFFF), double(seed >> 16));

		SetVec4(out.tuneDensity, LayerPercent("CloudDensityLow", 0, 300), LayerPercent("CloudDensityMedium", 0, 300), LayerPercent("CloudDensityHigh", 0, 300), 0.0);
		SetVec4(out.tuneSize, LayerPercent("CloudSizeLow", 25, 400), LayerPercent("CloudSizeMedium", 25, 400), LayerPercent("CloudSizeHigh", 25, 400), 0.0);
	}

	void SetMaterialCloudData(Graphics::Material *mat, CloudDataBlock &data, CloudHeightMap *heightMap)
	{
		mat->SetBufferDynamic(s_cloudDataName, &data);
		mat->SetTexture(s_cloudHeightTexName, heightMap ? heightMap->GetTexture() : GetFallbackTexture());
	}

	void BeginFrame()
	{
		s_queue.clear();
		AdvanceCloudClock();
	}

	void QueueCloudPass(const SystemBody *sbody, const CloudDataBlock &data, const vector3f &center, const Color4f &atmosColor, CloudHeightMap *heightMap)
	{
		// only planets that can have clouds; the shaders decide the rest
		if (sbody->GetType() != SystemBody::TYPE_PLANET_TERRESTRIAL || !sbody->HasAtmosphere() || !Pi::game || !CloudsEnabled())
			return;
		if (center.Length() > CLOUD_PASS_MAX_DISTANCE || s_queue.size() >= 8)
			return;

		PassEntry e;
		e.cloudData = data;
		e.center = center;
		e.atmosColor = atmosColor;
		e.heightTex.Reset(heightMap ? heightMap->GetTexture() : GetFallbackTexture());
		s_queue.push_back(e);
	}

	void RenderCloudPasses(Graphics::Renderer *r)
	{
		if (s_queue.empty())
			return;

		Graphics::RenderTarget *mainTarget = r->GetRenderTarget();
		if (!mainTarget || !mainTarget->GetDepthTexture()) {
			s_queue.clear();
			return;
		}
		const Graphics::RenderTargetDesc &mainDesc = mainTarget->GetDesc();
		if (mainDesc.numSamples > 0) {
			// a multisampled depth texture cannot be read by the cloud shaders
			if (!s_warnedMSAA) {
				Log::Warning("Volumetric clouds are disabled while multisample anti-aliasing is enabled.\n");
				s_warnedMSAA = true;
			}
			s_queue.clear();
			return;
		}

		const int width = mainDesc.width;
		const int height = mainDesc.height;
		EnsurePassResources(r, width, height);
		PassResources &pass = *s_pass;
		const int cw = pass.cloudTarget->GetDesc().width;
		const int ch = pass.cloudTarget->GetDesc().height;

		// restores the render target, viewport and matrices on scope exit
		Graphics::Renderer::StateTicket ticket(r);

		// infinite reverse-Z projection: depth = near / view distance
		const matrix4x4f proj = r->GetProjection();

		CloudPassBlock passData;
		const int softness = Clamp(Pi::config->Int("CloudSoftness", 1), 0, 2);
		SetVec4(passData.proj, proj[0], proj[5], proj[14], pass.divisor);
		SetVec4(passData.size, cw, ch, width, height);

		// 1. linear view distance of the scene, so the passes below never
		//    sample the depth buffer they are drawing into
		r->SetRenderTarget(pass.distTarget.get());
		r->SetViewport({ 0, 0, width, height });
		pass.distMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
		pass.distMaterial->SetTexture(s_sceneDepthName, mainTarget->GetDepthTexture());
		r->DrawBuffer(pass.triangle.get(), pass.distMaterial.Get());

		for (PassEntry &e : s_queue) {
			SetVec4(passData.center, e.center.x, e.center.y, e.center.z, 0.0);
			SetVec4(passData.atmos, e.atmosColor.r, e.atmosColor.g, e.atmosColor.b, softness);

			// 2. clouds at reduced resolution
			r->SetRenderTarget(pass.cloudTarget.get());
			r->SetViewport({ 0, 0, cw, ch });
			r->ClearScreen(Color(0, 0, 0, 0), false);
			pass.passMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
			pass.passMaterial->SetBufferDynamic(s_cloudDataName, &e.cloudData);
			pass.passMaterial->SetTexture(s_sceneDistName, pass.distTarget->GetColorTexture());
			pass.passMaterial->SetTexture(s_cloudHeightTexName, e.heightTex.Get());
			r->DrawBuffer(pass.triangle.get(), pass.passMaterial.Get());

			// 3. depth-aware upsample, blended over the scene
			r->SetRenderTarget(mainTarget);
			r->SetViewport({ 0, 0, width, height });
			pass.compositeMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
			pass.compositeMaterial->SetTexture(s_cloudTexName, pass.cloudTarget->GetColorTexture());
			pass.compositeMaterial->SetTexture(s_sceneDistName, pass.distTarget->GetColorTexture());
			r->DrawBuffer(pass.triangle.get(), pass.compositeMaterial.Get());
		}

		s_queue.clear();
	}

	void Uninit()
	{
		s_queue.clear();
		s_pass.reset();
		s_fallbackTexture.Reset();
	}

} // namespace PlanetClouds
