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
	static const size_t s_cityGlowDataName = "CityGlowData"_hash;
	static const size_t s_cloudHeightTexName = "cloudHeightTex"_hash;
	static const size_t s_sceneDepthName = "sceneDepth"_hash;
	static const size_t s_sceneDistName = "sceneDist"_hash;
	static const size_t s_cloudTexName = "cloudTex"_hash;
	static const size_t s_marchTexName = "marchTex"_hash;
	static const size_t s_historyTexName = "historyTex"_hash;
	static const size_t s_shadowTexName = "shadowTex"_hash;
	static const size_t s_weatherXYName = "cloudWeatherXY"_hash;
	static const size_t s_weatherZName = "cloudWeatherZ"_hash;

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
	//   CloudResolution  0..5: effective cloud resolution 360, 540, 720, 1080, 1440 lines, or full
	//   CloudResolutionSpace  0..5: the same, used when the camera is well above the clouds
	//   CloudQuality     0..3: raymarch steps (CLOUD_QUALITY_STEPS)
	//   CloudWeatherMaps 0/1: bake the large-scale weather pattern of the nearest planet (UpdateWeatherMaps)
	//   CloudSpaceDetail 0..200 %: how soon the resolution rises toward the
	//                    space resolution with distance (scales RES_PIXELS_PER_CELL; 0 = never)
	//   CloudTemporal    0 off, 1 4x (each frame traces 1 pixel of every 2x2 block), 2 9x (3x3), 3 16x (4x4)
	//   CloudSoftness    0..2: upsampling kernel radius
	//   CloudShadows     0 off, 1 soft (cloud-field average), 2 detailed (per cloud)
	//   CloudSpeed       1..100: cloud motion and evolution relative to the real wind
	//   CloudDensityLow/Medium/High  0..300 %: cloud cover per layer (as 1 - (1 - c)^m)
	//   CloudSizeLow/Medium/High     25..1600 %: size of individual clouds per layer
	//   CloudTransparencyLow/Medium/High  25..1000 %: 200 = half the optical thickness
	//   CloudSystemSizeLow/Medium/High    25..400 %: size of cloud systems and the gaps between them
	//   CloudClusteringLow/Medium/High    0..100 %: scattered clouds .. solid decks, at the same cover
	static const int CLOUD_RESOLUTION_LINES[] = { 360, 540, 720, 1080, 1440, 0 };
	static const int CLOUD_QUALITY_STEPS[] = { 16, 24, 32, 48 };

	// Resolution per frame (ChooseFrameResolution). The cloud field has detail
	// down to its smallest cells; a history pixel needs to be no finer than
	// that detail seen from the camera: RES_PIXELS_PER_CELL pixels across the
	// smallest cell at the viewing distance. Near the ground cells look large,
	// so the normal ("Cloud resolution") setting is the coarsest used; far
	// above the clouds they shrink on screen and the resolution rises toward
	// the "from space" setting, the finest used. Inside and below the clouds
	// every pixel is raymarched (expensive) and the cells look large, so the
	// refinement fades in between REFINE_START and REFINE_END times the
	// cloud-top height (the tropopause, at least CLOUD_MIN_TOP_M). Changes are
	// smoothed over RES_SMOOTH_TIME. The buffers hold the finest resolution
	// and each frame draws into the part it needs, so nothing is reallocated.
	static const double RES_PIXELS_PER_CELL = 3.0;
	static const double REFINE_START = 1.0;
	static const double REFINE_END = 2.0;
	static const double CLOUD_MIN_TOP_M = 8000.0;
	static const double RES_SMOOTH_TIME = 0.5; // [s]

	// must match the shader (cloud-config.glsl, cloud-derive.glsl,
	// cloudCellU in cloud-march.glsl): cell sizes for a scale height of 8.4 km.
	// Cirrus fibres (700 m apart) are faint, so they count as 3x coarser.
	static const double CELL_LOW_M = 4000.0;
	static const double CELL_MID_M = 1500.0 / 2.2;
	static const double CIRRUS_DETAIL_M = 3.0 * 700.0;

	// When the shader still draws individual cells (cloudShapeDensity in
	// cloud-march.glsl): low and medium cells while their feature size spans
	// CLOUD_MIN_CELL_PIXELS pixels, cirrus fibres while the fibre crest spans
	// a pixel. Smaller ones become a uniform medium. Must match the shader.
	// Above the clouds (refinement weight 1) and with no cells visible as
	// such, the shadow buffer drops from half to quarter resolution: the
	// shadows are then the smooth average of the cloud field.
	static const double SHADER_FEATURE_M[3] = { 4000.0, 1500.0, 0.2 * 700.0 }; // CLOUD_CELL_LOW_M, CLOUD_CELL_MID_M, cirrus crest
	static const double SHADER_MIN_PIXELS[3] = { 4.0, 4.0, 1.0 };				 // CLOUD_MIN_CELL_PIXELS, cirrus octaves > 0

	// planets further away than this (planet radii) are not given a pass
	static const float CLOUD_PASS_MAX_DISTANCE = 2000.0f;

	// Must match the CloudPassData block in cloud-pass-uniforms.glsl
	struct CloudPassBlock {
		float center[4];	 // xyz: planet centre in view space [planet radii]
		float proj[4];		 // x: P[0], y: P[5] (1 / tan of the half fov), z: near plane [m], w: composite divisor
		float size[4];		 // xy: size of the buffer the composite reads, zw: full size [pixels]
		float atmos[4];		 // rgb: atmosphere colour, w: softness 0..2
		float march[4];		 // xy: march buffer size, z: full pixels per history pixel, w: sample footprint [full pixels]
		float frame[4];		 // xy: checkerboard offset in history pixels (0.5 = block centre), z: history valid (0/1), w: apply shadows (0/1)
		float prevCenter[4]; // previous frame: planet centre in view space [planet radii]
		float prevAxisX[4];	 // previous frame: planet axes in view space
		float prevAxisY[4];
		float prevAxisZ[4];
		float prevProj[4];	 // previous frame: x P[0], y P[5], zw: history size [pixels]
		float histSize[4];	 // xy: history buffer size, zw: shadow buffer size
	};
	static_assert(sizeof(CloudPassBlock) == 192, "CloudPassBlock must match the std140 CloudPassData block");

	// Temporal trace order within a block of history pixels: every pixel is
	// traced once per 4 (2x2) or 9 (3x3) frames, consecutive frames far apart
	// (ordered dither), so a partly refreshed block has no visible pattern.
	static const int CHECKER_OFFSETS_2[4][2] = { { 0, 0 }, { 1, 1 }, { 1, 0 }, { 0, 1 } };
	static const int CHECKER_OFFSETS_3[9][2] = { { 0, 0 }, { 1, 2 }, { 2, 1 }, { 2, 0 }, { 0, 2 }, { 1, 1 }, { 0, 1 }, { 1, 0 }, { 2, 2 } };
	static const int CHECKER_OFFSETS_4[16][2] = { { 0, 0 }, { 2, 2 }, { 2, 0 }, { 0, 2 }, { 1, 1 }, { 3, 3 }, { 3, 1 }, { 1, 3 },
		{ 1, 0 }, { 3, 2 }, { 3, 0 }, { 1, 2 }, { 0, 1 }, { 2, 3 }, { 2, 1 }, { 0, 3 } }; // 4x4 Bayer order

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
		Graphics::CityGlowDataBlock cityGlow;
	};

	struct PassResources {
		std::unique_ptr<Graphics::RenderTarget> distTarget;		  // full size, R32F view distance
		std::unique_ptr<Graphics::RenderTarget> marchTarget;	  // half the history size, freshly traced clouds
		std::unique_ptr<Graphics::RenderTarget> historyTarget[2]; // effective resolution, ping-pong
		std::unique_ptr<Graphics::RenderTarget> shadowTarget;	  // half the screen size, cloud shadow light factor
		RefCountedPtr<Graphics::Material> distMaterial;
		RefCountedPtr<Graphics::Material> passMaterial;
		RefCountedPtr<Graphics::Material> reprojectMaterial;
		RefCountedPtr<Graphics::Material> compositeMaterial;
		RefCountedPtr<Graphics::Material> shadowMaterial;
		std::unique_ptr<Graphics::VertexArray> triangle;
		int width = 0, height = 0;
		double allocDivisor = 0.0;		  // the buffers hold this resolution (full pixels per history pixel)
		int maxHistW = 0, maxHistH = 0, maxMarchW = 0, maxMarchH = 0; // march: for 2x2 blocks, the largest
		int shadowW = 0, shadowH = 0;
		// this frame: full pixels per history pixel, and the part of the
		// history buffers in use (the march size depends on the block size)
		double divisor = 1.0;
		int histW = 0, histH = 0;
		// this frame: camera well above the clouds, and whether any cells are
		// still drawn as cells (ChooseFrameResolution)
		bool orbit = false;
		bool cellsVisible = true;
		int shadowUsedW = 0, shadowUsedH = 0; // part of the shadow buffer in use
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
		int histW = 0, histH = 0;
		double cloudTime = 0.0;
	};

	static RefCountedPtr<Graphics::Texture> s_fallbackTexture;
	static std::unique_ptr<PassResources> s_pass;

	// Weather maps: the large-scale flow of the nearest planet's weather
	// (cyclones, zonal stretch, folding: cloudWeatherDomain), baked per layer
	// and cloud cycle by cloud_weather.frag and read by cloudWeatherCached
	// instead of being computed for every sample. Within a cycle the flow is
	// static in the advected frame, so a map is baked once per cycle (every
	// 32768 / 65536 s of cloud time). Each layer has two slots (cycle parity)
	// for the two cycles of its phases. Must match the shader:
	// CLOUD_WEATHER_MAP_W/H, the atlas layout in cloudWeatherUV, the layer
	// periods (cloudDerive) and the cycle numbers (cloudPhases).
	static const int WEATHER_MAP_W = 1024;
	static const int WEATHER_MAP_H = 512;
	static const double WEATHER_LAYER_PERIOD[3] = { 32768.0, 32768.0, 65536.0 };

	struct WeatherMaps {
		std::unique_ptr<Graphics::RenderTarget> xy; // RG32F atlas, 2 x 3 maps with 1-texel wrap padding
		std::unique_ptr<Graphics::RenderTarget> z;	// R32F atlas, same layout
		RefCountedPtr<Graphics::Material> bakeMaterial;
		const SystemBody *body = nullptr;
		float system[3] = { 0.0f, 0.0f, 0.0f };
		double held[3][2]; // cloud cycle in each layer's slot, -1: none
	};
	static std::unique_ptr<WeatherMaps> s_weather;
	static bool s_weatherUnsupported = false; // float render targets missing: always compute
	static std::vector<PassEntry> s_queue;
	static bool s_warnedMSAA = false;
	static TemporalState s_temporal;
	static Uint64 s_frameIndex = 0;

	// smoothed resolution (full pixels per history pixel) and the planet it is for
	static double s_resDivisor = 0.0;
	static const SystemBody *s_resBody = nullptr;

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
				// without the erosion cuts: they only lower the ground, so the
				// floor stays conservative, and they would cost 5x the time here
				m_terrain->GetBaseHeights(points.data(), heights.data(), sw);

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

	// full pixels per history pixel for a resolution setting (may be
	// fractional: 1440 lines on a 2160-line screen is 1.5)
	double ChooseDivisor(int height, bool space)
	{
		const int level = space ? Clamp(Pi::config->Int("CloudResolutionSpace", 5), 0, 5) : Clamp(Pi::config->Int("CloudResolution", 1), 0, 5);
		const int lines = CLOUD_RESOLUTION_LINES[level];
		if (lines <= 0)
			return 1.0;
		return Clamp(double(height) / lines, 1.0, 8.0);
	}

	void EnsurePassResources(Graphics::Renderer *r, int width, int height)
	{
		const double divisor = std::min(ChooseDivisor(height, false), ChooseDivisor(height, true));
		if (s_pass && s_pass->width == width && s_pass->height == height && s_pass->allocDivisor == divisor)
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
			s_pass->shadowMaterial.Reset(r->CreateMaterial("cloud_shadow", litDesc, rsd, vfmt));

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
		pass.allocDivisor = divisor;
		pass.maxHistW = int(std::ceil(width / divisor));
		pass.maxHistH = int(std::ceil(height / divisor));
		pass.maxMarchW = (pass.maxHistW + 1) / 2;
		pass.maxMarchH = (pass.maxHistH + 1) / 2;
		pass.shadowW = (width + 1) / 2;
		pass.shadowH = (height + 1) / 2;

		const Graphics::RenderTargetDesc distDesc(Uint16(width), Uint16(height), Graphics::TEXTURE_R32F, Graphics::TEXTURE_NONE, false, 0);
		pass.distTarget.reset(r->CreateRenderTarget(distDesc));

		const Graphics::RenderTargetDesc marchDesc(Uint16(pass.maxMarchW), Uint16(pass.maxMarchH), Graphics::TEXTURE_RGBA_8888, Graphics::TEXTURE_NONE, false, 0);
		pass.marchTarget.reset(r->CreateRenderTarget(marchDesc));

		const Graphics::RenderTargetDesc histDesc(Uint16(pass.maxHistW), Uint16(pass.maxHistH), Graphics::TEXTURE_RGBA_8888, Graphics::TEXTURE_NONE, false, 0);
		for (auto &t : pass.historyTarget)
			t.reset(r->CreateRenderTarget(histDesc));

		const Graphics::RenderTargetDesc shadowDesc(Uint16(pass.shadowW), Uint16(pass.shadowH), Graphics::TEXTURE_RGBA_8888, Graphics::TEXTURE_NONE, false, 0);
		pass.shadowTarget.reset(r->CreateRenderTarget(shadowDesc));
		s_temporal.valid = false;

		Log::Info("Cloud pass: up to {}x{} effective cloud resolution ({}x{} traced per frame) for {}x{} screen\n",
			pass.maxHistW, pass.maxHistH, pass.maxMarchW, pass.maxMarchH, width, height);
	}

	// Size of the smallest cloud cells of a planet [m]: they scale with the
	// atmosphere's scale height and the per-layer size settings. Layers set
	// to no cover are left out.
	double SmallestCloudCell(const PlanetClouds::CloudDataBlock &d)
	{
		const double T = std::max(double(d.atmos[0]), 1.0);
		const double g = std::max(double(d.atmos[3]), 1e-3);
		const double H = 8.3144621 * T / (0.02897 * g);
		const double sH = Clamp(H / 8400.0, 0.5, 4.0);
		const double cells[3] = { CELL_LOW_M, CELL_MID_M, CIRRUS_DETAIL_M };
		double smallest = 1e30;
		for (int i = 0; i < 3; i++)
			if (d.tuneDensity[i] > 0.0f)
				smallest = std::min(smallest, cells[i] * sH * std::max(double(d.tuneSize[i]), 0.05));
		return smallest < 1e29 ? smallest : CELL_LOW_M * sH;
	}

	// Like SmallestCloudCell, but only cells the shader still draws as cells
	// when a full-resolution pixel covers pixelM metres; 0 when none are.
	double SmallestVisibleCloudCell(const PlanetClouds::CloudDataBlock &d, double pixelM)
	{
		const double T = std::max(double(d.atmos[0]), 1.0);
		const double g = std::max(double(d.atmos[3]), 1e-3);
		const double H = 8.3144621 * T / (0.02897 * g);
		const double sH = Clamp(H / 8400.0, 0.5, 4.0);
		const double cells[3] = { CELL_LOW_M, CELL_MID_M, CIRRUS_DETAIL_M };
		double smallest = 0.0;
		for (int i = 0; i < 3; i++) {
			if (d.tuneDensity[i] <= 0.0f)
				continue;
			const double size = std::max(double(d.tuneSize[i]), 0.05) * sH;
			if (SHADER_FEATURE_M[i] * size < SHADER_MIN_PIXELS[i] * pixelM)
				continue;
			const double cell = cells[i] * size;
			if (smallest <= 0.0 || cell < smallest)
				smallest = cell;
		}
		return smallest;
	}

	// This frame's resolution for the nearest planet e (see RES_PIXELS_PER_CELL).
	// proj5: 1 / tan of the half vertical field of view.
	void ChooseFrameResolution(PassResources &pass, const PassEntry &e, double proj5)
	{
		const double divNear = ChooseDivisor(pass.height, false);
		const double divSpace = ChooseDivisor(pass.height, true);
		const double divFine = std::min(divNear, divSpace), divCoarse = std::max(divNear, divSpace);

		// camera relative to the planet [planet radii]; the camera looks down -Z
		const double a = std::max(double(e.cloudData.geom[0]), 1.0);
		const double top = std::max(double(e.cloudData.geom[1]), CLOUD_MIN_TOP_M);
		const vector3d c(e.center);
		const double dc = c.Length();
		const double alt = (dc - 1.0) * a;
		const double rTop = 1.0 + top / a;

		// viewing distance of the clouds: the geometric mean of the distance
		// straight down and along the view (at most to the horizon of the
		// cloud deck), so looking toward the horizon, where distant clouds are
		// small on screen, raises the resolution, but only moderately
		const double nadir = std::max(alt - top, 1.0);
		double dist = nadir;
		if (dc > rTop) {
			const double horizon = std::sqrt(dc * dc - rTop * rTop) * a;
			const double b = -c.z; // dot(view direction, centre)
			const double disc = b * b - (dc * dc - rTop * rTop);
			const double hit = (disc >= 0.0 && b > 0.0) ? (b - std::sqrt(disc)) * a : horizon;
			dist = std::sqrt(nadir * Clamp(hit, nadir, horizon));
		}

		// resolution at which the smallest cells span RES_PIXELS_PER_CELL pixels,
		// scaled by the "Cloud detail from space" setting (lower: later, cheaper).
		// All cells count, even ones drawn as a uniform medium: the edges of
		// the cloud fields still need the resolution (refining only for cells
		// drawn as cells made orbital views blurry).
		const double detail = Clamp(Pi::config->Int("CloudSpaceDetail", 50), 0, 200) / 100.0;
		const double pixelAngle = 2.0 / (proj5 * pass.height); // full-resolution pixel [rad]
		const double divDetail = detail > 0.0 ? SmallestCloudCell(e.cloudData) / (dist * RES_PIXELS_PER_CELL * detail * pixelAngle) : divCoarse;
		const double visibleCell = SmallestVisibleCloudCell(e.cloudData, dist * pixelAngle);

		// refinement only well above the clouds
		const double x = Clamp((alt - REFINE_START * top) / ((REFINE_END - REFINE_START) * top), 0.0, 1.0);
		const double w = x * x * (3.0 - 2.0 * x);
		pass.orbit = w > 0.999;
		pass.cellsVisible = visibleCell > 0.0;
		const double target = divNear + (Clamp(divDetail, divFine, divCoarse) - divNear) * w;

		// smooth over time; a new planet starts at its target
		if (s_resBody != e.body || s_resDivisor <= 0.0) {
			s_resDivisor = target;
			s_resBody = e.body;
		} else {
			const double dt = Clamp(double(Pi::GetFrameTime()), 0.0, 1.0);
			s_resDivisor += (target - s_resDivisor) * (1.0 - std::exp(-dt / RES_SMOOTH_TIME));
		}

		pass.divisor = std::max(s_resDivisor, pass.allocDivisor);
		pass.histW = std::min(int(std::ceil(pass.width / pass.divisor)), pass.maxHistW);
		pass.histH = std::min(int(std::ceil(pass.height / pass.divisor)), pass.maxHistH);
	}

	// bakes the weather map of layer li for cloud cycle cyc into its slot
	void BakeWeatherMap(Graphics::Renderer *r, const PassResources &pass, WeatherMaps &w, PassEntry &e, int li, int slot, double cyc)
	{
		CloudPassBlock bake {};
		SetVec4(bake.march, slot * (WEATHER_MAP_W + 2), li * WEATHER_MAP_H, 0.0, 0.0);
		for (int comp = 0; comp < 2; comp++) {
			SetVec4(bake.frame, li, cyc, 0.0, comp);
			r->SetRenderTarget(comp == 0 ? w.xy.get() : w.z.get());
			r->SetViewport({ slot * (WEATHER_MAP_W + 2), li * WEATHER_MAP_H, WEATHER_MAP_W + 2, WEATHER_MAP_H });
			w.bakeMaterial->SetBufferDynamic(s_cloudPassDataName, &bake);
			w.bakeMaterial->SetBufferDynamic(s_cloudDataName, &e.cloudData);
			r->DrawBuffer(pass.triangle.get(), w.bakeMaterial.Get());
		}
	}

	// Makes sure the weather maps hold the cycles of both phases of every
	// layer of planet e (baking what is missing) and tells its shaders which
	// cycles they hold. Changes the render target and viewport.
	void UpdateWeatherMaps(Graphics::Renderer *r, const PassResources &pass, PassEntry &e)
	{
		if (Pi::config->Int("CloudWeatherMaps", 1) == 0 || s_weatherUnsupported) {
			s_weather.reset();
			return;
		}
		if (!s_weather) {
			s_weather.reset(new WeatherMaps());
			const Uint16 aw = Uint16(2 * (WEATHER_MAP_W + 2)), ah = Uint16(3 * WEATHER_MAP_H);
			s_weather->xy.reset(r->CreateRenderTarget(Graphics::RenderTargetDesc(aw, ah, Graphics::TEXTURE_RG32F, Graphics::TEXTURE_NONE, false, 0)));
			s_weather->z.reset(r->CreateRenderTarget(Graphics::RenderTargetDesc(aw, ah, Graphics::TEXTURE_R32F, Graphics::TEXTURE_NONE, false, 0)));
			if (!s_weather->xy || !s_weather->z) {
				Log::Warning("Cloud weather maps need float render targets; computing the weather pattern instead.\n");
				s_weather.reset();
				s_weatherUnsupported = true;
				return;
			}

			Graphics::RenderStateDesc rsd;
			rsd.blendMode = Graphics::BLEND_SOLID;
			rsd.cullMode = Graphics::CULL_NONE;
			rsd.depthTest = false;
			rsd.depthWrite = false;
			const Graphics::VertexFormatDesc vfmt = Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0);
			s_weather->bakeMaterial.Reset(r->CreateMaterial("cloud_weather", Graphics::MaterialDescriptor(), rsd, vfmt));
			s_weather->body = nullptr;
		}
		WeatherMaps &w = *s_weather;

		// a different planet or weather-system size: every map is stale
		const PlanetClouds::CloudDataBlock &d = e.cloudData;
		if (w.body != e.body || !std::equal(w.system, w.system + 3, d.tuneSystem)) {
			w.body = e.body;
			std::copy(d.tuneSystem, d.tuneSystem + 3, w.system);
			for (auto &layer : w.held)
				layer[0] = layer[1] = -1.0;
		}

		// the cycles of phase A and B, computed like cloudPhases from the
		// same (float) cloud time, so they match the shader exactly
		for (int li = 0; li < 3; li++) {
			const double period = WEATHER_LAYER_PERIOD[li];
			const double base = double(d.time[1]) * (1048576.0 / period);
			for (int k = 0; k < 2; k++) {
				const double cyc = base + std::floor(double(d.time[0]) / period + 0.5 * k);
				const int slot = int(std::fmod(cyc, 2.0));
				if (w.held[li][slot] != cyc) {
					BakeWeatherMap(r, pass, w, e, li, slot, cyc);
					w.held[li][slot] = cyc;
				}
			}
		}
		SetVec4(e.cloudData.weatherCyc0, w.held[0][0], w.held[0][1], w.held[1][0], w.held[1][1]);
		SetVec4(e.cloudData.weatherCyc1, w.held[2][0], w.held[2][1], 1.0, 0.0);
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

		SetVec4(out.tuneDensity, LayerPercent("CloudDensityLow", 0, 300), LayerPercent("CloudDensityMedium", 0, 300), LayerPercent("CloudDensityHigh", 0, 300),
			Clamp(Pi::config->Int("CloudShadows", 2), 0, 2));
		SetVec4(out.tuneSize, LayerPercent("CloudSizeLow", 25, 1600), LayerPercent("CloudSizeMedium", 25, 1600), LayerPercent("CloudSizeHigh", 25, 1600),
			CLOUD_QUALITY_STEPS[Clamp(Pi::config->Int("CloudQuality", 2), 0, 3)]);
		SetVec4(out.tuneTransparency, LayerPercent("CloudTransparencyLow", 25, 1000), LayerPercent("CloudTransparencyMedium", 25, 1000), LayerPercent("CloudTransparencyHigh", 25, 1000), 0.0);
		SetVec4(out.tuneSystem, LayerPercent("CloudSystemSizeLow", 25, 400), LayerPercent("CloudSystemSizeMedium", 25, 400), LayerPercent("CloudSystemSizeHigh", 25, 400), 0.0);
		SetVec4(out.tuneCluster, Clamp(Pi::config->Int("CloudClusteringLow", 75), 0, 100) / 100.0,
			Clamp(Pi::config->Int("CloudClusteringMedium", 50), 0, 100) / 100.0,
			Clamp(Pi::config->Int("CloudClusteringHigh", 50), 0, 100) / 100.0, 0.0);
		// no weather maps until RenderCloudPasses bakes them for the nearest planet
		SetVec4(out.weatherCyc0, -1.0, -1.0, -1.0, -1.0);
		SetVec4(out.weatherCyc1, -1.0, -1.0, 0.0, 0.0);
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

	void QueueCloudPass(const SystemBody *sbody, const CloudDataBlock &data, const vector3f &center, const Color4f &atmosColor, CloudHeightMap *heightMap,
		const Graphics::CityGlowDataBlock &cityGlow)
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
		e.cityGlow = cityGlow;
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

		// resolution from what the nearest planet's clouds look like on screen
		{
			const PassEntry *nearest = &s_queue[0];
			for (const PassEntry &e : s_queue)
				if (e.center.Length() < nearest->center.Length())
					nearest = &e;
			ChooseFrameResolution(pass, *nearest, r->GetProjection()[5]);
		}
		const bool shadows = Pi::config->Int("CloudShadows", 2) > 0;
		const bool smoothShadows = pass.orbit && !pass.cellsVisible;
		pass.shadowUsedW = smoothShadows ? (width + 3) / 4 : pass.shadowW;
		pass.shadowUsedH = smoothShadows ? (height + 3) / 4 : pass.shadowH;

		// restores the render target, viewport and matrices on scope exit
		Graphics::Renderer::StateTicket ticket(r);

		// weather maps for the nearest planet
		{
			size_t nearest = 0;
			for (size_t i = 1; i < s_queue.size(); i++)
				if (s_queue[i].center.Length() < s_queue[nearest].center.Length())
					nearest = i;
			r->BeginGpuTimer("clouds: weather maps");
			UpdateWeatherMaps(r, pass, s_queue[nearest]);
			r->EndGpuTimer();
		}
		Graphics::Texture *weatherXY = s_weather ? s_weather->xy->GetColorTexture() : GetFallbackTexture();
		Graphics::Texture *weatherZ = s_weather ? s_weather->z->GetColorTexture() : GetFallbackTexture();

		// infinite reverse-Z projection: depth = near / view distance
		const matrix4x4f proj = r->GetProjection();

		CloudPassBlock passData {};
		const int softness = Clamp(Pi::config->Int("CloudSoftness", 1), 0, 2);
		SetVec4(passData.histSize, pass.histW, pass.histH, pass.shadowUsedW, pass.shadowUsedH);

		// the nearest planet (largest on screen) gets temporal reconstruction,
		// tracing 1 pixel of every 2x2 (4x) or 3x3 (9x) block per frame
		const int temporalMode = Clamp(Pi::config->Int("CloudTemporal", 2), 0, 3);
		const int temporalBlock = temporalMode + 1; // 2x2, 3x3 or 4x4
		int temporalEntry = -1;
		if (temporalMode != 0) {
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
		r->BeginGpuTimer("clouds: scene distance");
		r->SetRenderTarget(pass.distTarget.get());
		r->SetViewport({ 0, 0, width, height });
		pass.distMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
		pass.distMaterial->SetTexture(s_sceneDepthName, mainTarget->GetDepthTexture());
		r->DrawBuffer(pass.triangle.get(), pass.distMaterial.Get());
		r->EndGpuTimer();

		for (size_t i = 0; i < s_queue.size(); i++) {
			PassEntry &e = s_queue[i];
			const bool temporal = int(i) == temporalEntry;
			// history pixels per march pixel (each way); without history the
			// centre of every 2x2 block is traced
			const int block = temporal ? temporalBlock : 2;
			const int marchW = (pass.histW + block - 1) / block;
			const int marchH = (pass.histH + block - 1) / block;

			SetVec4(passData.center, e.center.x, e.center.y, e.center.z, block);
			// softening hides upscaling; a buffer at screen resolution needs none
			const double compositeDivisor = temporal ? pass.divisor : 2.0 * pass.divisor;
			SetVec4(passData.atmos, e.atmosColor.r, e.atmosColor.g, e.atmosColor.b, compositeDivisor > 1.01 ? softness : 0);

			if (temporal) {
				// trace one pixel of every block, rotating over 4 or 9 frames
				const int *off = block == 4 ? CHECKER_OFFSETS_4[s_frameIndex % 16] : (block == 3 ? CHECKER_OFFSETS_3[s_frameIndex % 9] : CHECKER_OFFSETS_2[s_frameIndex % 4]);
				const bool historyValid = s_temporal.valid && s_temporal.body == e.body &&
					s_temporal.lastFrame + 1 == s_frameIndex &&
					(e.center - s_temporal.center).Length() < HISTORY_MAX_CENTER_JUMP &&
					std::abs(s_cloudTime - s_temporal.cloudTime) < HISTORY_MAX_CLOCK_JUMP;
				SetVec4(passData.frame, off[0], off[1], historyValid ? 1.0 : 0.0, shadows ? 1.0 : 0.0);
				SetVec4(passData.march, marchW, marchH, pass.divisor, pass.divisor);
				SetVec4(passData.prevCenter, s_temporal.center.x, s_temporal.center.y, s_temporal.center.z, 0.0);
				std::copy(s_temporal.axisX, s_temporal.axisX + 4, passData.prevAxisX);
				std::copy(s_temporal.axisY, s_temporal.axisY + 4, passData.prevAxisY);
				std::copy(s_temporal.axisZ, s_temporal.axisZ + 4, passData.prevAxisZ);
				SetVec4(passData.prevProj, s_temporal.proj[0], s_temporal.proj[1], s_temporal.histW, s_temporal.histH);
			} else {
				// no history: trace the centre of every 2x2 block
				SetVec4(passData.frame, 0.5, 0.5, 0.0, shadows ? 1.0 : 0.0);
				SetVec4(passData.march, marchW, marchH, pass.divisor, 2.0 * pass.divisor);
			}

			// 2. trace clouds into the march buffer
			r->BeginGpuTimer("clouds: trace");
			r->SetRenderTarget(pass.marchTarget.get());
			r->SetViewport({ 0, 0, marchW, marchH });
			r->ClearScreen(Color(0, 0, 0, 0), false);
			pass.passMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
			pass.passMaterial->SetBufferDynamic(s_cloudDataName, &e.cloudData);
			pass.passMaterial->SetTexture(s_sceneDistName, pass.distTarget->GetColorTexture());
			pass.passMaterial->SetTexture(s_cloudHeightTexName, e.heightTex.Get());
			pass.passMaterial->SetTexture(s_weatherXYName, weatherXY);
			pass.passMaterial->SetTexture(s_weatherZName, weatherZ);
			r->DrawBuffer(pass.triangle.get(), pass.passMaterial.Get());
			r->EndGpuTimer();

			// 2b. cloud shadows on the scene (applied by the composite)
			if (shadows) {
				r->BeginGpuTimer("clouds: shadow");
				r->SetRenderTarget(pass.shadowTarget.get());
				r->SetViewport({ 0, 0, pass.shadowUsedW, pass.shadowUsedH });
				pass.shadowMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
				pass.shadowMaterial->SetBufferDynamic(s_cloudDataName, &e.cloudData);
				pass.shadowMaterial->SetBufferDynamic(s_cityGlowDataName, &e.cityGlow);
				pass.shadowMaterial->SetTexture(s_sceneDistName, pass.distTarget->GetColorTexture());
				pass.shadowMaterial->SetTexture(s_cloudHeightTexName, e.heightTex.Get());
				pass.shadowMaterial->SetTexture(s_weatherXYName, weatherXY);
				pass.shadowMaterial->SetTexture(s_weatherZName, weatherZ);
				r->DrawBuffer(pass.triangle.get(), pass.shadowMaterial.Get());
				r->EndGpuTimer();
			}

			Graphics::Texture *cloudTex = pass.marchTarget->GetColorTexture();
			if (temporal) {
				// 3. reconstruct the full history from the fresh pixels and the
				//    reprojected previous frame
				const int next = 1 - s_temporal.current;
				r->BeginGpuTimer("clouds: reproject");
				r->SetRenderTarget(pass.historyTarget[next].get());
				r->SetViewport({ 0, 0, pass.histW, pass.histH });
				pass.reprojectMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
				pass.reprojectMaterial->SetBufferDynamic(s_cloudDataName, &e.cloudData);
				pass.reprojectMaterial->SetTexture(s_marchTexName, pass.marchTarget->GetColorTexture());
				pass.reprojectMaterial->SetTexture(s_historyTexName, pass.historyTarget[s_temporal.current]->GetColorTexture());
				pass.reprojectMaterial->SetTexture(s_sceneDistName, pass.distTarget->GetColorTexture());
				r->DrawBuffer(pass.triangle.get(), pass.reprojectMaterial.Get());
				r->EndGpuTimer();
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
				s_temporal.histW = pass.histW;
				s_temporal.histH = pass.histH;
				s_temporal.cloudTime = s_cloudTime;

				SetVec4(passData.proj, proj[0], proj[5], proj[14], pass.divisor);
				SetVec4(passData.size, pass.histW, pass.histH, width, height);
			} else {
				SetVec4(passData.proj, proj[0], proj[5], proj[14], 2.0 * pass.divisor);
				SetVec4(passData.size, marchW, marchH, width, height);
			}

			// 4. depth-aware upsample, blended over the scene
			r->BeginGpuTimer("clouds: composite");
			r->SetRenderTarget(mainTarget);
			r->SetViewport({ 0, 0, width, height });
			pass.compositeMaterial->SetBufferDynamic(s_cloudPassDataName, &passData);
			pass.compositeMaterial->SetTexture(s_cloudTexName, cloudTex);
			pass.compositeMaterial->SetTexture(s_sceneDistName, pass.distTarget->GetColorTexture());
			pass.compositeMaterial->SetTexture(s_shadowTexName, pass.shadowTarget->GetColorTexture());
			r->DrawBuffer(pass.triangle.get(), pass.compositeMaterial.Get());
			r->EndGpuTimer();
		}

		s_queue.clear();
	}

	void Uninit()
	{
		s_queue.clear();
		s_pass.reset();
		s_weather.reset();
		s_fallbackTexture.Reset();
	}

} // namespace PlanetClouds
