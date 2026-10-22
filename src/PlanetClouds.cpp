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
	static const size_t s_marchTexName = "marchTex"_hash;
	static const size_t s_historyTexName = "historyTex"_hash;

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
	//   CloudResolution  0..4: effective cloud resolution 360, 540, 720, 1080 lines, or full
	//   CloudTemporal    0/1: temporal reconstruction (each frame traces 1 of 4 pixels)
	//   CloudSoftness    0..2: upsampling kernel radius
	//   CloudSpeed       1..100: cloud motion and evolution relative to the real wind
	//   CloudDensityLow/Medium/High  0..300 %: cloud cover per layer (as 1 - (1 - c)^m)
	//   CloudSizeLow/Medium/High     25..1600 %: size of individual clouds per layer
	//   CloudTransparencyLow/Medium/High  25..1000 %: 200 = half the optical thickness
	//   CloudSystemSizeLow/Medium/High    25..400 %: size of cloud systems and the gaps between them
	//   CloudClusteringLow/Medium/High    0..100 %: scattered clouds .. solid decks, at the same cover
	static const int CLOUD_RESOLUTION_LINES[] = { 360, 540, 720, 1080, 0 };

	// planets further away than this (planet radii) are not given a pass
	static const float CLOUD_PASS_MAX_DISTANCE = 2000.0f;

	// Must match the CloudPassData block in cloud-pass-uniforms.glsl
	struct CloudPassBlock {
		float center[4];	 // xyz: planet centre in view space [planet radii]
		float proj[4];		 // x: P[0], y: P[5] (1 / tan of the half fov), z: near plane [m], w: composite divisor
		float size[4];		 // xy: size of the buffer the composite reads, zw: full size [pixels]
		float atmos[4];		 // rgb: atmosphere colour, w: softness 0..2
		float march[4];		 // xy: march buffer size, z: full pixels per history pixel, w: sample footprint [full pixels]
		float frame[4];		 // xy: checkerboard offset in history pixels (0.5 = block centre), z: history valid (0/1)
		float prevCenter[4]; // previous frame: planet centre in view space [planet radii]
		float prevAxisX[4];	 // previous frame: planet axes in view space
		float prevAxisY[4];
		float prevAxisZ[4];
		float prevProj[4];	 // previous frame: x P[0], y P[5]
		float histSize[4];	 // xy: history buffer size
	};
	static_assert(sizeof(CloudPassBlock) == 192, "CloudPassBlock must match the std140 CloudPassData block");

	// 2x2 checkerboard order: every history pixel is traced once per 4 frames
	static const int CHECKER_OFFSETS[4][2] = { { 0, 0 }, { 1, 1 }, { 1, 0 }, { 0, 1 } };

	// temporal history is dropped when, between two frames, the planet centre
	// moves more than this (planet radii) or the cloud clock jumps more than this (s)
	static const double HISTORY_MAX_CENTER_JUMP = 0.02;
	static const double HISTORY_MAX_CLOCK_JUMP = 30.0;

	struct PassEntry {
		const SystemBody *body = nullptr;
		PlanetClouds::CloudDataBlock cloudData;
		vector3f center;
		Color4f atmosColor;
		RefCountedPtr<Graphics::Texture> heightTex;
	};

	struct PassResources {
		std::unique_ptr<Graphics::RenderTarget> distTarget;		  // full size, R32F view distance
		std::unique_ptr<Graphics::RenderTarget> marchTarget;	  // half the history size, freshly traced clouds
		std::unique_ptr<Graphics::RenderTarget> historyTarget[2]; // effective resolution, ping-pong
		RefCountedPtr<Graphics::Material> distMaterial;
		RefCountedPtr<Graphics::Material> passMaterial;
		RefCountedPtr<Graphics::Material> reprojectMaterial;
		RefCountedPtr<Graphics::Material> compositeMaterial;
		std::unique_ptr<Graphics::VertexArray> triangle;
		int width = 0, height = 0, divisor = 0;
		int histW = 0, histH = 0, marchW = 0, marchH = 0;
	};

	// history of the one planet that gets temporal reconstruction
	struct TemporalState {
		const SystemBody *body = nullptr;
		Uint64 lastFrame = 0;
		int current = 0; // history target holding the last frame
		bool valid = false;
		vector3f center;
		float axisX[4], axisY[4], axisZ[4];
		float proj[2];
		double cloudTime = 0.0;
	};

	static RefCountedPtr<Graphics::Texture> s_fallbackTexture;
	static std::unique_ptr<PassResources> s_pass;
	static std::vector<PassEntry> s_queue;
	static bool s_warnedMSAA = false;
	static TemporalState s_temporal;
	static Uint64 s_frameIndex = 0;

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

			s_pass->reprojectMaterial.Reset(r->CreateMaterial("cloud_reproject", plainDesc, rsd, vfmt));

			Graphics::RenderStateDesc blendRsd = rsd;
			blendRsd.blendMode = Graphics::BLEND_ALPHA_PREMULT;
			s_pass->compositeMaterial.Reset(r->CreateMaterial("cloud_composite", plainDesc, blendRsd, vfmt));

			// one triangle covering the screen (clip-space positions)
			s_pass->triangle.reset(new Graphics::VertexArray(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0, 3));
			s_pass->triangle->Add(vector3f(-1.0f, -1.0f, 0.0f), vector2f(0.0f, 0.0f));
			s_pass->triangle->Add(vector3f(3.0f, -1.0f, 0.0f), vector2f(2.0f, 0.0f));
			s_pass->triangle->Add(vector3f(-1.0f, 3.0f, 0.0f), vector2f(0.0f, 2.0f));
		}

		PassResources &pass = *s_pass;
		pass.width = width;
		pass.height = height;
		pass.divisor = divisor;
		pass.histW = (width + divisor - 1) / divisor;
		pass.histH = (height + divisor - 1) / divisor;
		pass.marchW = (pass.histW + 1) / 2;
		pass.marchH = (pass.histH + 1) / 2;

		const Graphics::RenderTargetDesc distDesc(Uint16(width), Uint16(height), Graphics::TEXTURE_R32F, Graphics::TEXTURE_NONE, false, 0);
		pass.distTarget.reset(r->CreateRenderTarget(distDesc));

		const Graphics::RenderTargetDesc marchDesc(Uint16(pass.marchW), Uint16(pass.marchH), Graphics::TEXTURE_RGBA_8888, Graphics::TEXTURE_NONE, false, 0);
		pass.marchTarget.reset(r->CreateRenderTarget(marchDesc));

		const Graphics::RenderTargetDesc histDesc(Uint16(pass.histW), Uint16(pass.histH), Graphics::TEXTURE_RGBA_8888, Graphics::TEXTURE_NONE, false, 0);
		for (auto &t : pass.historyTarget)
			t.reset(r->CreateRenderTarget(histDesc));
		s_temporal.valid = false;

		Log::Info("Cloud pass: {}x{} effective cloud resolution ({}x{} traced per frame) for {}x{} screen\n",
			pass.histW, pass.histH, pass.marchW, pass.marchH, width, height);
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
		SetVec4(out.tuneSize, LayerPercent("CloudSizeLow", 25, 1600), LayerPercent("CloudSizeMedium", 25, 1600), LayerPercent("CloudSizeHigh", 25, 1600), 0.0);
		SetVec4(out.tuneTransparency, LayerPercent("CloudTransparencyLow", 25, 1000), LayerPercent("CloudTransparencyMedium", 25, 1000), LayerPercent("CloudTransparencyHigh", 25, 1000), 0.0);
		SetVec4(out.tuneSystem, LayerPercent("CloudSystemSizeLow", 25, 400), LayerPercent("CloudSystemSizeMedium", 25, 400), LayerPercent("CloudSystemSizeHigh", 25, 400), 0.0);
		SetVec4(out.tuneCluster, Clamp(Pi::config->Int("CloudClusteringLow", 75), 0, 100) / 100.0,
			Clamp(Pi::config->Int("CloudClusteringMedium", 50), 0, 100) / 100.0,
			Clamp(Pi::config->Int("CloudClusteringHigh", 50), 0, 100) / 100.0, 0.0);
	}

	void SetMaterialCloudData(Graphics::Material *mat, CloudDataBlock &data, CloudHeightMap *heightMap)
	{
		mat->SetBufferDynamic(s_cloudDataName, &data);
		mat->SetTexture(s_cloudHeightTexName, heightMap ? heightMap->GetTexture() : GetFallbackTexture());
	}

	void BeginFrame()
	{
		s_frameIndex++;
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
		e.body = sbody;
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

		// restores the render target, viewport and matrices on scope exit
		Graphics::Renderer::StateTicket ticket(r);

		// infinite reverse-Z projection: depth = near / view distance
		const matrix4x4f proj = r->GetProjection();

		CloudPassBlock passData {};
		const int softness = Clamp(Pi::config->Int("CloudSoftness", 1), 0, 2);
		SetVec4(passData.histSize, pass.histW, pass.histH, 0.0, 0.0);

		// the nearest planet (largest on screen) gets temporal reconstruction
		int temporalEntry = -1;
		if (Pi::config->Int("CloudTemporal", 1) != 0) {
			float best = 1e30f;
			for (size_t i = 0; i < s_queue.size(); i++) {
				if (s_queue[i].center.Length() < best) {
					best = s_queue[i].center.Length();
					temporalEntry = int(i);
				}
			}
		}

		// 1. linear view distance of the scene, so the passes below never
		//    sample the depth buffer they are drawing into
		SetVec4(passData.proj, proj[0], proj[5], proj[14], pass.divisor);
		SetVec4(passData.size, pass.histW, pass.histH, width, height);
		r->SetRenderTarget(pass.distTarget.get());
		r->SetViewport({ 0, 0, width, height });
		pass.distMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
		pass.distMaterial->SetTexture(s_sceneDepthName, mainTarget->GetDepthTexture());
		r->DrawBuffer(pass.triangle.get(), pass.distMaterial.Get());

		for (size_t i = 0; i < s_queue.size(); i++) {
			PassEntry &e = s_queue[i];
			const bool temporal = int(i) == temporalEntry;

			SetVec4(passData.center, e.center.x, e.center.y, e.center.z, 0.0);
			SetVec4(passData.atmos, e.atmosColor.r, e.atmosColor.g, e.atmosColor.b, softness);

			if (temporal) {
				// trace one pixel of every 2x2 block, rotating over 4 frames
				const int *off = CHECKER_OFFSETS[s_frameIndex % 4];
				const bool historyValid = s_temporal.valid && s_temporal.body == e.body &&
					s_temporal.lastFrame + 1 == s_frameIndex &&
					(e.center - s_temporal.center).Length() < HISTORY_MAX_CENTER_JUMP &&
					std::abs(s_cloudTime - s_temporal.cloudTime) < HISTORY_MAX_CLOCK_JUMP;
				SetVec4(passData.frame, off[0], off[1], historyValid ? 1.0 : 0.0, 0.0);
				SetVec4(passData.march, pass.marchW, pass.marchH, pass.divisor, pass.divisor);
				SetVec4(passData.prevCenter, s_temporal.center.x, s_temporal.center.y, s_temporal.center.z, 0.0);
				std::copy(s_temporal.axisX, s_temporal.axisX + 4, passData.prevAxisX);
				std::copy(s_temporal.axisY, s_temporal.axisY + 4, passData.prevAxisY);
				std::copy(s_temporal.axisZ, s_temporal.axisZ + 4, passData.prevAxisZ);
				SetVec4(passData.prevProj, s_temporal.proj[0], s_temporal.proj[1], 0.0, 0.0);
			} else {
				// no history: trace the centre of every 2x2 block
				SetVec4(passData.frame, 0.5, 0.5, 0.0, 0.0);
				SetVec4(passData.march, pass.marchW, pass.marchH, pass.divisor, 2.0 * pass.divisor);
			}

			// 2. trace clouds into the march buffer
			r->SetRenderTarget(pass.marchTarget.get());
			r->SetViewport({ 0, 0, pass.marchW, pass.marchH });
			r->ClearScreen(Color(0, 0, 0, 0), false);
			pass.passMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
			pass.passMaterial->SetBufferDynamic(s_cloudDataName, &e.cloudData);
			pass.passMaterial->SetTexture(s_sceneDistName, pass.distTarget->GetColorTexture());
			pass.passMaterial->SetTexture(s_cloudHeightTexName, e.heightTex.Get());
			r->DrawBuffer(pass.triangle.get(), pass.passMaterial.Get());

			Graphics::Texture *cloudTex = pass.marchTarget->GetColorTexture();
			if (temporal) {
				// 3. reconstruct the full history from the fresh pixels and the
				//    reprojected previous frame
				const int next = 1 - s_temporal.current;
				r->SetRenderTarget(pass.historyTarget[next].get());
				r->SetViewport({ 0, 0, pass.histW, pass.histH });
				pass.reprojectMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
				pass.reprojectMaterial->SetBufferDynamic(s_cloudDataName, &e.cloudData);
				pass.reprojectMaterial->SetTexture(s_marchTexName, pass.marchTarget->GetColorTexture());
				pass.reprojectMaterial->SetTexture(s_historyTexName, pass.historyTarget[s_temporal.current]->GetColorTexture());
				pass.reprojectMaterial->SetTexture(s_sceneDistName, pass.distTarget->GetColorTexture());
				r->DrawBuffer(pass.triangle.get(), pass.reprojectMaterial.Get());
				cloudTex = pass.historyTarget[next]->GetColorTexture();

				s_temporal.current = next;
				s_temporal.valid = true;
				s_temporal.body = e.body;
				s_temporal.lastFrame = s_frameIndex;
				s_temporal.center = e.center;
				std::copy(e.cloudData.axisX, e.cloudData.axisX + 4, s_temporal.axisX);
				std::copy(e.cloudData.axisY, e.cloudData.axisY + 4, s_temporal.axisY);
				std::copy(e.cloudData.axisZ, e.cloudData.axisZ + 4, s_temporal.axisZ);
				s_temporal.proj[0] = proj[0];
				s_temporal.proj[1] = proj[5];
				s_temporal.cloudTime = s_cloudTime;

				SetVec4(passData.proj, proj[0], proj[5], proj[14], pass.divisor);
				SetVec4(passData.size, pass.histW, pass.histH, width, height);
			} else {
				SetVec4(passData.proj, proj[0], proj[5], proj[14], 2.0 * pass.divisor);
				SetVec4(passData.size, pass.marchW, pass.marchH, width, height);
			}

			// 4. depth-aware upsample, blended over the scene
			r->SetRenderTarget(mainTarget);
			r->SetViewport({ 0, 0, width, height });
			pass.compositeMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
			pass.compositeMaterial->SetTexture(s_cloudTexName, cloudTex);
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
