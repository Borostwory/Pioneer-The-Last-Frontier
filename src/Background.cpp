// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Background.h"

#include "FileSystem.h"
#include "FloatComparison.h"
#include "Game.h"
#include "GameConfig.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Player.h"
#include "Space.h"
#include "StringF.h"

#include "core/TaskGraph.h"

#include "galaxy/GalaxyGenerator.h"
#include "galaxy/StarSystem.h"

#include "graphics/Graphics.h"
#include "graphics/RenderState.h"
#include "graphics/TextureBuilder.h"
#include "graphics/Types.h"
#include "graphics/VertexArray.h"
#include "graphics/VertexBuffer.h"
#include "perlin.h"
#include "profiler/Profiler.h"
#include "utils.h"

#include <SDL_stdinc.h>
#include <iostream>
#include <sstream>
#include <numeric>

using namespace Graphics;

namespace {
	constexpr Uint32 BG_STAR_MAX = 500000;
	constexpr Uint32 BG_STAR_MIN = 1;
	constexpr Sint32 BG_STAR_RADIUS_MAX = 500;
	static RefCountedPtr<Graphics::Texture> s_defaultCubeMap;

	/*
	 * Hyperspace is shown as a 1 g relativistic voyage along the ship's nose,
	 * compressed into the length of the transit: accelerate for PEAK_SHIP_YEARS of
	 * ship time, flip, decelerate. Stars and sky are aberrated and Doppler shifted
	 * in data/shaders/opengl/relativity.glsl; the cosmic microwave background (CMB)
	 * turns into a white-hot point ahead.
	 */
	constexpr double PEAK_SHIP_YEARS = 11.0;
	constexpr double G_OVER_C = 1.0326; // 1 g in units of c per year
	constexpr double CMB_TEMPERATURE = 2.725;
	// starlight of the sky at rest: magnitude -3.1 as 550 nm flux, W m^-2 nm^-1
	constexpr double REST_STARLIGHT_FLUX = 3.63e-11 * 17.38;
	// the same starlight in the starfield's pixel units: ties the CMB to the stars
	constexpr double REST_STARLIGHT_PIXELS = 5000.0;
	// typical star, for estimating how bright the sky gets
	constexpr double STAR_TEMPERATURE = 5800.0;
	// light (pixel units) below which the bright point does not glow
	constexpr double GLOW_THRESHOLD = 20.0;
	// a star sprite grows by at most this factor to show light it cannot show as brightness
	constexpr float MAX_SPRITE_GROWTH = 8.0f;
	constexpr double CHANNEL_WAVELENGTH[3] = { 610e-9, 550e-9, 465e-9 };
	constexpr double HC_OVER_K = 1.438777e-2; // m K

	static RefCountedPtr<Graphics::Material> s_starfieldRelativistic;
	static RefCountedPtr<Graphics::Material> s_skyboxRelativistic;

	// Matches the uniform block RelativityData in relativity.glsl
	struct RelativityData {
		vector3f axis;
		float beta;
		float gamma;
		float oneMinusBeta;
		float exposure;
		float pixelAngle;
		float cmbScale;
		float cmbTemperature;
		float coreRadius;
		float maxSpriteGrowth;
		vector3f cmbCore;
		float unused0;
		vector3f glow;
		float unused1;
	};
	static_assert(sizeof(RelativityData) == 80, "RelativityData must match the std140 layout of the shader");

	// Planck spectral radiance B_lambda(T), in W / (m^2 sr nm)
	static double PlanckRadiance(double lambda, double T)
	{
		const double x = HC_OVER_K / (lambda * T);
		return (T <= 0.0 || x > 700.0) ? 0.0 : 1.191042972e-16 / std::pow(lambda, 5.0) / std::expm1(x) * 1e-9;
	}

	// Integrate f(D) dD / D over ln(D) in [uMin, uMax]
	template <typename F>
	static auto IntegrateLogD(double uMin, double uMax, F f)
	{
		constexpr int STEPS = 256;
		const double du = (uMax - uMin) / STEPS;
		auto sum = f(std::exp(uMin)) * 0.5;
		for (int i = 1; i <= STEPS; i++)
			sum += f(std::exp(uMin + i * du)) * (i == STEPS ? 0.5 : 1.0);
		return sum * du;
	}

	// Flux per channel (W m^-2 nm^-1) of the CMB inside a cone around the direction of travel.
	// Seen from the ship, light from angle theta has Doppler factor D = 1 / (gamma (1 - beta cos(theta)))
	// and dOmega = 2 pi dD / (gamma beta D^2).
	static vector3d CmbFlux(double rapidity, double halfAngle)
	{
		const double s = std::sin(0.5 * halfAngle);
		const double oneMinusCos = 2.0 * s * s;
		if (rapidity < 1e-6)
			return vector3d(0.0); // invisible at rest
		const double beta = std::tanh(rapidity), gamma = std::cosh(rapidity);
		const double oneMinusBeta = 1.0 - beta;
		const double dEdge = 1.0 / (gamma * (oneMinusBeta + beta * oneMinusCos));
		return IntegrateLogD(std::log(dEdge), rapidity, [](double D) {
			return vector3d(PlanckRadiance(CHANNEL_WAVELENGTH[0], CMB_TEMPERATURE * D),
					   PlanckRadiance(CHANNEL_WAVELENGTH[1], CMB_TEMPERATURE * D),
					   PlanckRadiance(CHANNEL_WAVELENGTH[2], CMB_TEMPERATURE * D)) / D;
		}) * (2.0 * M_PI / std::sinh(rapidity));
	}

	// Starlight of a sky of STAR_TEMPERATURE stars relative to rest. A star seen with Doppler
	// factor D = gamma (1 + beta cos(theta)) is a blackbody at T D, dimmed by 1 / D^2.
	static double StarlightFactor(double rapidity)
	{
		if (rapidity < 1e-6)
			return 1.0;
		const double x0 = HC_OVER_K / (CHANNEL_WAVELENGTH[1] * STAR_TEMPERATURE);
		return IntegrateLogD(-rapidity, rapidity, [x0](double D) {
			return x0 / D > 700.0 ? 0.0 : std::expm1(x0) / std::expm1(x0 / D) / D;
		}) / (2.0 * std::sinh(rapidity));
	}

	// Physical channel flux * WhiteBalance() = display value; a 5800 K body looks white
	static vector3d WhiteBalance()
	{
		vector3d w;
		for (int c = 0; c < 3; c++)
			w[c] = PlanckRadiance(CHANNEL_WAVELENGTH[1], 5800.0) / PlanckRadiance(CHANNEL_WAVELENGTH[c], 5800.0);
		return w;
	}

	// What the ship sees in hyperspace. axisView: direction of travel in view space.
	// Returns false when not in hyperspace.
	static bool GetRelativityData(Graphics::Renderer *renderer, const vector3f &axisView, RelativityData &data)
	{
		if (!Pi::game || !Pi::game->IsHyperspace() || Pi::game->GetHyperspaceDuration() <= 0.0)
			return false;

		const double progress = Clamp((Pi::game->GetHyperspaceProgress() + Pi::GetGameTickAlpha() * Pi::game->GetTimeStep()) / Pi::game->GetHyperspaceDuration(), 0.0, 1.0);
		const double rapidity = G_OVER_C * PEAK_SHIP_YEARS * (progress < 0.5 ? 2.0 * progress : 2.0 - 2.0 * progress);
		const double e = std::exp(-2.0 * rapidity);

		// angular size of a pixel at the centre of the view, and a disc of the same area
		const double pixelAngle = 2.0 / (std::abs(renderer->GetProjection()[5]) * std::max(1, int(renderer->GetViewport().h)));
		const double coreRadius = pixelAngle / std::sqrt(M_PI);

		// physical flux -> pixel values
		const vector3d toPixels = WhiteBalance() * (REST_STARLIGHT_PIXELS / REST_STARLIGHT_FLUX);
		const vector3d cmbTotal = CmbFlux(rapidity, M_PI);
		const vector3d cmbCore = CmbFlux(rapidity, coreRadius);

		// exposure keeps the total light at what the sky gives at rest, never brighter
		const double exposure = 1.0 / std::max(1.0, StarlightFactor(rapidity) + cmbTotal.y / REST_STARLIGHT_FLUX);

		vector3d glow(cmbTotal.x * toPixels.x, cmbTotal.y * toPixels.y, cmbTotal.z * toPixels.z);
		glow *= exposure;
		const double glowLuma = glow.Dot(vector3d(0.2126, 0.7152, 0.0722));
		glow *= glowLuma > GLOW_THRESHOLD ? 1.0 - GLOW_THRESHOLD / glowLuma : 0.0;

		data = {};
		data.axis = axisView.Normalized();
		data.beta = float(std::tanh(rapidity));
		data.gamma = float(std::cosh(rapidity));
		data.oneMinusBeta = float(2.0 * e / (1.0 + e));
		data.exposure = float(exposure);
		data.pixelAngle = float(pixelAngle);
		data.cmbScale = float(pixelAngle * pixelAngle * toPixels.y * exposure);
		data.cmbTemperature = float(CMB_TEMPERATURE);
		data.coreRadius = float(coreRadius);
		data.maxSpriteGrowth = MAX_SPRITE_GROWTH;
		data.cmbCore = vector3f(vector3d(cmbCore.x * toPixels.x, cmbCore.y * toPixels.y, cmbCore.z * toPixels.z) * exposure);
		data.glow = vector3f(glow);
		return true;
	}

	// direction the ship travels in hyperspace: along its nose, in background coordinates
	static vector3f HyperspaceTravelDir()
	{
		return vector3f(-Pi::player->GetOrient().VectorZ());
	}

	static Uint32 GetNumSkyboxes()
	{
		char filename[1024];
		snprintf(filename, sizeof(filename), "textures/skybox");
		std::vector<FileSystem::FileInfo> fileList;
		FileSystem::gameDataFiles.ReadDirectory(filename, fileList);

		const char *itemMask = "ub";

		Uint32 num_matching = 0;
		for (std::vector<FileSystem::FileInfo>::const_iterator it = fileList.begin(), itEnd = fileList.end(); it != itEnd; ++it) {
			if (starts_with((*it).GetName(), itemMask)) {
				++num_matching;
			}
		}
		return num_matching;
	}
} // namespace

namespace Background {

#pragma pack(push, 4)
	struct MilkyWayVert {
		vector3f pos;
		Color4ub col;
	};

	struct StarVert {
		vector3f pos;
		Color4ub col;
	};

	struct SkyboxVert {
		vector3f pos;
		vector2f uv;
	};
#pragma pack(pop)

	void BackgroundElement::SetIntensity(float intensity)
	{
		m_material->emissive = Color(intensity * 255, intensity * 255, intensity * 255);
	}

	UniverseBox::UniverseBox(Graphics::Renderer *renderer)
	{
		m_renderer = renderer;
		Init();
	}

	UniverseBox::~UniverseBox()
	{
	}

	void UniverseBox::Init()
	{
		// Load default cubemap
		if (!s_defaultCubeMap.Valid()) {
			TextureBuilder texture_builder = TextureBuilder::Cube("textures/skybox/skybox.dds");
			s_defaultCubeMap.Reset(texture_builder.GetOrCreateTexture(m_renderer, std::string("cube")));
		}

		// Create skybox geometry
		std::unique_ptr<Graphics::VertexArray> box(new VertexArray(ATTRIB_POSITION | ATTRIB_UV0, 36));
		const float vp = 1000.0f;
		// Top +Y
		box->Add(vector3f(-vp, vp, vp), vector2f(0.0f, 0.0f));
		box->Add(vector3f(-vp, vp, -vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(vp, vp, vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(vp, vp, vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(-vp, vp, -vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(vp, vp, -vp), vector2f(1.0f, 1.0f));
		// Bottom -Y
		box->Add(vector3f(-vp, -vp, -vp), vector2f(0.0f, 0.0f));
		box->Add(vector3f(-vp, -vp, vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(vp, -vp, -vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(vp, -vp, -vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(-vp, -vp, vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(vp, -vp, vp), vector2f(1.0f, 1.0f));
		// Front -Z
		box->Add(vector3f(-vp, vp, -vp), vector2f(0.0f, 0.0f));
		box->Add(vector3f(-vp, -vp, -vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(vp, vp, -vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(vp, vp, -vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(-vp, -vp, -vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(vp, -vp, -vp), vector2f(1.0f, 1.0f));
		// Back +Z
		box->Add(vector3f(vp, vp, vp), vector2f(0.0f, 0.0f));
		box->Add(vector3f(vp, -vp, vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(-vp, vp, vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(-vp, vp, vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(vp, -vp, vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(-vp, -vp, vp), vector2f(1.0f, 1.0f));
		// Right +X
		box->Add(vector3f(vp, vp, -vp), vector2f(0.0f, 0.0f));
		box->Add(vector3f(vp, -vp, -vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(vp, vp, vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(vp, vp, vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(vp, -vp, -vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(vp, -vp, vp), vector2f(1.0f, 1.0f));
		// Left -X
		box->Add(vector3f(-vp, vp, vp), vector2f(0.0f, 0.0f));
		box->Add(vector3f(-vp, -vp, vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(-vp, vp, -vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(-vp, vp, -vp), vector2f(1.0f, 0.0f));
		box->Add(vector3f(-vp, -vp, vp), vector2f(0.0f, 1.0f));
		box->Add(vector3f(-vp, -vp, -vp), vector2f(1.0f, 1.0f));

		//create buffer and upload data
		Graphics::VertexFormatDesc fmt = Graphics::VertexFormatDesc::FromAttribSet(box->GetAttributeSet());
		Graphics::VertexBuffer *vertexBuf = m_renderer->CreateVertexBuffer(Graphics::BUFFER_USAGE_STATIC, box->GetNumVerts(), fmt.bindings[0].stride);

		box->Populate(vertexBuf);

		Graphics::MaterialDescriptor desc;
		Graphics::RenderStateDesc stateDesc;
		stateDesc.depthTest = false;
		stateDesc.depthWrite = false;

		m_material.Reset(m_renderer->CreateMaterial("skybox", desc, stateDesc, fmt));
		m_material->diffuse = Color4f(0.8, 0.8, 0.8, 1.0);

		if (!s_skyboxRelativistic.Valid()) {
			s_skyboxRelativistic.Reset(m_renderer->CreateMaterial("skybox_relativistic", desc, stateDesc, fmt));
			s_skyboxRelativistic->emissive = Color::WHITE;
		}

		m_universeBox.reset(m_renderer->CreateMeshObject(fmt, vertexBuf));
		m_numCubemaps = GetNumSkyboxes();

		SetIntensity(1.0f);
	}

	void UniverseBox::Draw()
	{
		if (!m_cubemap.Valid())
			return;

		RelativityData data;
		const vector3f dir = Pi::game ? HyperspaceTravelDir() : vector3f(0.0f);
		if (GetRelativityData(m_renderer, m_renderer->GetTransform().ApplyRotationOnly(dir), data)) {
			s_skyboxRelativistic->SetTexture("texture0"_hash, m_cubemap.Get());
			s_skyboxRelativistic->SetBufferDynamic("RelativityData"_hash, &data);
			m_renderer->DrawMesh(m_universeBox.get(), s_skyboxRelativistic.Get());
		} else
			m_renderer->DrawMesh(m_universeBox.get(), m_material.Get());
	}

	void UniverseBox::LoadCubeMap(Random &rand)
	{
		if (m_numCubemaps > 0) {
			const int new_ubox_index = rand.Int32(1, m_numCubemaps);
			if (new_ubox_index > 0) {
				// Load new one
				const std::string os = stringf("textures/skybox/ub%0{d}.dds", (new_ubox_index - 1));
				TextureBuilder texture_builder = TextureBuilder::Cube(os.c_str());
				m_cubemap.Reset(texture_builder.GetOrCreateTexture(m_renderer, std::string("cube")));
			}
		} else {
			// use default cubemap
			m_cubemap.Reset(s_defaultCubeMap.Get());
		}

		m_material->SetTexture("texture0"_hash, m_cubemap.Get());
	}

	Starfield::Starfield(Graphics::Renderer *renderer)
	{
		m_renderer = renderer;
		Init();
	}

	void Starfield::Init()
	{
		PROFILE_SCOPED()

		m_pointSprites.reset(new Graphics::Drawables::PointSprites);

		// Create material to be used with starfield points
		Graphics::MaterialDescriptor desc;

		Graphics::RenderStateDesc stateDesc;
		stateDesc.depthTest = false;
		stateDesc.depthWrite = false;
		stateDesc.blendMode = Graphics::BLEND_ALPHA;
		stateDesc.primitiveType = Graphics::POINTS;

		m_material.Reset(m_renderer->CreateMaterial("starfield", desc, stateDesc, m_pointSprites->GetVertexFormat()));
		Graphics::Texture *texture = Graphics::TextureBuilder::Billboard("textures/star_point.png").GetOrCreateTexture(m_renderer, "billboard");
		m_material->SetTexture("texture0"_hash, texture);
		m_material->emissive = Color::WHITE;

		// Create material for the stars seen at relativistic speed in hyperspace.
		// Additive, so that stars crowding into the same pixels add up their light.
		if (!s_starfieldRelativistic.Valid()) {
			Graphics::RenderStateDesc stateDescRelativistic = stateDesc;
			stateDescRelativistic.blendMode = Graphics::BLEND_ALPHA_ONE;
			s_starfieldRelativistic.Reset(m_renderer->CreateMaterial("starfield_relativistic", desc, stateDescRelativistic, m_pointSprites->GetVertexFormat()));
			s_starfieldRelativistic->SetTexture("texture0"_hash, texture);
			s_starfieldRelativistic->emissive = Color::WHITE;
		}

		IniConfig cfg;
		cfg.Read(FileSystem::gameDataFiles, "configs/Starfield.ini");
		// NB: limit the ranges of all values loaded from the file
		m_rMin = Clamp(cfg.Float("rMin", 0.6), 0.2f, 1.0f);
		m_rMax = Clamp(cfg.Float("rMax", 1.0), 0.2f, 1.0f);
		m_gMin = Clamp(cfg.Float("gMin", 0.6), 0.2f, 1.0f);
		m_gMax = Clamp(cfg.Float("gMax", 1.0), 0.2f, 1.0f);
		m_bMin = Clamp(cfg.Float("bMin", 0.6), 0.2f, 1.0f);
		m_bMax = Clamp(cfg.Float("bMax", 1.0), 0.2f, 1.0f);
	}

	struct StarInfo {
		std::vector<vector3f> pos;
		std::vector<Color> color;
		std::vector<float> brightness;
	};

	struct StarQueryInfo {
		const SystemPath *systemPath;
		int32_t sectorMin;
		int32_t sectorMax;
		int32_t visibleRadiusSqr;
		Color colorMin;
		Color colorMax;
		float brightnessFactor;
	};

	class SampleStarsTask : public Task {
	public:
		SampleStarsTask(RefCountedPtr<Galaxy> galaxy, const StarQueryInfo &info, int32_t starsLimit, StarInfo &stars, double &medianBrightness, TaskRange range) :
			Task(range),
			galaxy(galaxy),
			info(info),
			starsLimit(starsLimit),
			stars(stars),
			medianBrightness(medianBrightness)
		{
			stars.pos.reserve(starsLimit);
			stars.color.reserve(starsLimit);
			stars.brightness.reserve(starsLimit);
		}

		void OnExecute(TaskRange range) override
		{
			PROFILE_SCOPED()
			const SystemPath *systemPath = info.systemPath;

			int32_t minZ = info.sectorMin + int32_t(range.begin);
			int32_t maxZ = info.sectorMin + int32_t(range.end);

			// fill star array
			for (Sint32 x = info.sectorMin; x <= info.sectorMax; ++x) {
				for (Sint32 y = info.sectorMin; y <= info.sectorMax; ++y) {
					for (Sint32 z = minZ; z <= maxZ; ++z) {
						SystemPath sys(systemPath->sectorX + x, systemPath->sectorY + y, systemPath->sectorZ + z);

						if (SystemPath::SectorDistanceSqr(sys, *systemPath) * Sector::SIZE * Sector::SIZE >= info.visibleRadiusSqr)
							continue; // early out

						// TODO: we're generating these sectors manually and not caching for two reasons:
						// - Sectors are culled from the cache as soon as they don't have any external references
						//   (so this isn't a performance regression from prior versions of this code)
						// - SectorCache isn't thread-safe, as slave caches modify the master cache when you
						//   generate sectors. SectorCache in general isn't designed for use on multiple threads.
						// Resolving the above issues in future versions of the galaxy code should allow this
						//   generation step to pay performance dividends for later code that needs to use the sectors.
						RefCountedPtr<const Sector> sec = galaxy->GetGenerator()->Generate<Sector, SectorCache>(galaxy, sys, nullptr);

						// add as many systems as we can
						const size_t numSystems = std::min(starsLimit - stars.pos.size(), sec->m_systems.size());
						for (size_t systemIndex = 0; systemIndex < numSystems; systemIndex++) {
							const Sector::System *ss = &(sec->m_systems[systemIndex]);

							const vector3f distance = Sector::SIZE * vector3f(systemPath->sectorX, systemPath->sectorY, systemPath->sectorZ) - ss->GetFullPosition();
							if (distance.LengthSqr() >= info.visibleRadiusSqr)
								continue; // too far

							// add the colors and luminosities of all stars in a system together
							float luminositySystemSum = 0.0f;
							vector3f colorSystemSum(0.0f, 0.0f, 0.0f);
							for (size_t i = 0; i < ss->GetNumStars(); ++i) {
								luminositySystemSum += StarSystem::starLuminosities[ss->GetStarType(i)];
								Color col = StarSystem::starRealColors[ss->GetStarType(i)];
								colorSystemSum += vector3f(col.r, col.g, col.b) * StarSystem::starLuminosities[ss->GetStarType(i)];
							}

							if (!is_zero_exact(luminositySystemSum))
								colorSystemSum /= luminositySystemSum;

							Color col(colorSystemSum.x, colorSystemSum.y, colorSystemSum.z);
							col.r = Clamp(col.r, info.colorMin.r, info.colorMax.r);
							col.g = Clamp(col.g, info.colorMin.g, info.colorMax.g);
							col.b = Clamp(col.b, info.colorMin.b, info.colorMax.b);
							//const Color col(Color::PINK); // debug pink

							// use a logarithmic scala for brightness since this looks more natural to the human eye
							float brightness = log(luminositySystemSum / (4 * M_PI * distance.Length() * distance.Length()));

							// handle zero-brightness systems (no stars)
							brightness = is_zero_exact(luminositySystemSum) ? 0.0 : brightness;

							stars.pos.push_back(distance.Normalized() * 1000.0f);
							stars.color.push_back(col);
							stars.brightness.push_back(brightness);
						}

						// Don't process any more sectors if we've generated our quota of stars.
						if (stars.pos.size() >= starsLimit)
							break;
					}
				}
			}

			// find the median brightness of all visible stars
			const size_t numStars = stars.pos.size();
			std::vector<uint32_t> sortedBrightnessIndex;
			sortedBrightnessIndex.reserve(numStars);

			for (uint32_t i = 0; i < numStars; ++i) {
				sortedBrightnessIndex.push_back(i);
			}

			std::sort(sortedBrightnessIndex.begin(), sortedBrightnessIndex.end(), [&](const uint32_t a, const uint32_t b) {
				return stars.brightness[a] > stars.brightness[b];
			});

			constexpr float medianPosition = 0.7;
			if (numStars > 0) {
				medianBrightness = stars.brightness[sortedBrightnessIndex[Clamp<uint32_t>(medianPosition * numStars, 0, numStars - 1)]];
			}
		}

		RefCountedPtr<Galaxy> galaxy;
		const StarQueryInfo info;
		const size_t starsLimit;
		StarInfo &stars;
		double &medianBrightness;
	};

	class SortStarsTask : public Task {
	public:
		SortStarsTask(const StarQueryInfo &info, StarInfo &stars, double medianBrightness) :
			info(info),
			stars(stars),
			medianBrightness(medianBrightness)
		{}

		// Do the brightness sort on worker threads using the worker's subset
		// of stars rather than on the main thread with all stars.
		void OnExecute(TaskRange) override
		{
			PROFILE_SCOPED()
			const size_t numStars = stars.pos.size();

			for (size_t i = 0; i < numStars; ++i) {
				// dividing through the median helps bringing the logarithmic brightnesses to a scala that is easier to work with
				float brightness = stars.brightness[i] / medianBrightness;
				// the exponentiation helps to emphasize very bright stars
				constexpr float brightnessPower = 7.5;
				brightness = std::pow(brightness, brightnessPower);

				// 0.0f filters out NaNs from previous operations
				// Brightness now stores the size value used by the shader
				stars.brightness[i] = std::max(0.0f, brightness * info.brightnessFactor);

				// convert temporarily to floats to prevent narrowing errors
				Color4f colorF = stars.color[i].ToColor4f();

				// find a color scaling factor that doesn't make a colored star look white
				float colorMax = std::max({ colorF.r, colorF.g, colorF.b });
				float scaledColorMax = colorMax * brightness;
				const float colorFactor = std::min(scaledColorMax, 255.0f) / colorMax;

				colorF = colorF *= colorFactor;
				colorF.a = 1.0f;

				stars.color[i] = Color4ub(colorF);
			}
		}

		const StarQueryInfo info;
		StarInfo &stars;
		const double medianBrightness;
	};

	// https://en.wikipedia.org/wiki/Spherical_segment
	static double spherical_segment_volume(double h, double r1_sq, double r2_sq)
	{
		return M_PI * h / 6.0 * (3 * r1_sq + 3 * r2_sq + h * h);
	}

	static double spherical_circle_radius_sq(double h, double R)
	{
		// distance to sphere center
		double l = std::abs(R - h);
		return R * R - l * l;
	}

	static double task_spherical_segment_volume(uint32_t begin, uint32_t end, double r)
	{
		double h = end - begin;
		double r1_sq = spherical_circle_radius_sq(begin, r);
		double r2_sq = spherical_circle_radius_sq(end, r);
		return spherical_segment_volume(h, r1_sq, r2_sq);
	}

	void Starfield::Fill(Random &rand, const SystemPath *const systemPath, RefCountedPtr<Galaxy> galaxy)
	{
		PROFILE_SCOPED()

		const Uint32 NUM_BG_STARS = MathUtil::mix(BG_STAR_MIN, BG_STAR_MAX, Pi::GetAmountBackgroundStars());
		m_animMesh.reset();

		// user doesn't want to see stars
		if (NUM_BG_STARS == BG_STAR_MIN) return;

		const float brightnessApparentSizeFactor = Pi::GetStarFieldStarSizeFactor() / 7.0;
		// dividing by 7 to make sure that 100% star size isn't too big to clash with UI elements

		assert(sizeof(StarVert) == 16);

		StarInfo stars;
		stars.pos.reserve(NUM_BG_STARS);
		stars.color.reserve(NUM_BG_STARS);
		stars.brightness.reserve(NUM_BG_STARS);

		//fill the array
		Uint32 num = 0;
		if (systemPath && galaxy.Valid()) {
			PROFILE_SCOPED_DESC("Pick Stars from Galaxy")

			TaskGraph *graph = Pi::GetApp()->GetTaskGraph();

			// judging by the current sector generator, maximum average number
			// of stars in a sector is 6
			// It’s easy to express what the radius of a ball should be so that
			// at such a density it would contain approximately NUM_BG_STARS stars:
			const double density = 6.0;
			const double maxBall = pow(3.0 / 4.0 / M_PI * (double)NUM_BG_STARS / density, 1.0 / 3.0);
			const int32_t visibleRadius = std::min<int32_t>(BG_STAR_RADIUS_MAX, maxBall * Sector::SIZE);

			StarQueryInfo info;
			info.systemPath = systemPath;
			info.sectorMin = -(visibleRadius / Sector::SIZE); // lyrs_radius / sector_size_in_lyrs
			info.sectorMax = visibleRadius / Sector::SIZE;	  // lyrs_radius / sector_size_in_lyrs
			info.visibleRadiusSqr = (visibleRadius * visibleRadius);
			info.colorMin = Color((Uint8)(m_rMin * 255), (Uint8)(m_gMin * 255), (Uint8)(m_rMin * 255));
			info.colorMax = Color((Uint8)(m_rMax * 255), (Uint8)(m_gMax * 255), (Uint8)(m_rMax * 255));
			info.brightnessFactor = brightnessApparentSizeFactor;

			// We want the main thread to participate in this work as well,
			// but don't split the number of stars too much that we have visible brightness "patches"
			// also we want a piece of at least size 1
			const uint32_t numTasks = std::min({ graph->GetNumWorkerThreads() + 1, 8U, uint32_t(info.sectorMax - info.sectorMin) });

			TaskSet *sampleStarsTaskSet = new TaskSet();

			int32_t starsLeft = NUM_BG_STARS;
			const double realRadius = info.sectorMax;
			const double realDensity = NUM_BG_STARS / (M_PI / 0.75 * realRadius * realRadius * realRadius);

			std::vector<StarInfo> taskStars(numTasks);
			std::vector<double> taskMedians(numTasks);

			// Split the visible area of the galaxy up into separate tasks
			uint32_t current = 0;
			// divide the ball more evenly into tasks, when the number of tasks
			// is comparable to the diameter of the ball (in sectors)
			float range_step = (info.sectorMax - info.sectorMin) / (float)numTasks;

			for (size_t i = 0; i < numTasks; i++) {
				int32_t starsLimit;
				uint32_t end = std::max(uint32_t(range_step * (i + 1)), current + 1);
				if (i + 1 == numTasks) {
					end = (info.sectorMax - info.sectorMin);
					starsLimit = starsLeft;
				} else {
					starsLimit = realDensity * task_spherical_segment_volume(current, end, realRadius);
					starsLeft -= starsLimit;
				}

				// in the task the loop runs from current to end inclusive
				sampleStarsTaskSet->AddTask(new SampleStarsTask(galaxy, info, starsLimit, taskStars[i], taskMedians[i], { current, end - 1 }));
				current = end;
			}

			// We can't make progress until all stars are gathered, so run the
			// star collection on the 'main' thread as well.
			auto sampleHandle = graph->QueueTaskSet(sampleStarsTaskSet);
			graph->WaitForTaskSet(sampleHandle);

			double medianBrightness = std::reduce(taskMedians.begin(), taskMedians.end()) / taskMedians.size();

			TaskSet *sortStarsTaskSet = new TaskSet();
			for (size_t i = 0; i < numTasks; i++) {
				sortStarsTaskSet->AddTask(new SortStarsTask(info, taskStars[i], medianBrightness));
			}
			auto sortHandle = graph->QueueTaskSet(sortStarsTaskSet);
			graph->WaitForTaskSet(sortHandle);

			for(auto &item : taskStars) {
				stars.pos.insert(stars.pos.end(), item.pos.begin(), item.pos.end());
				stars.color.insert(stars.color.end(), item.color.begin(), item.color.end());
				stars.brightness.insert(stars.brightness.end(), item.brightness.begin(), item.brightness.end());
			}
		}
		num = stars.pos.size();
		Output("Stars picked from galaxy: %d\n", stars.pos.size());

		stars.pos.resize(NUM_BG_STARS);
		stars.color.resize(NUM_BG_STARS);
		stars.brightness.resize(NUM_BG_STARS);

		PROFILE_START_DESC("Generate Random Stars")
		// fill out the remaining target count with generated points and also fill hyperspace stars
		Output("Generating %d random stars\n", NUM_BG_STARS - num);
		for (Uint32 i = num; i < NUM_BG_STARS; i++) {
			const double size = rand.Double(0.2, 0.9);
			const Uint8 colScale = size * 255;

			const Color col(
				rand.Double(m_rMin, m_rMax) * colScale,
				rand.Double(m_gMin, m_gMax) * colScale,
				rand.Double(m_bMin, m_bMax) * colScale,
				255);

			// this is proper random distribution on a sphere's surface
			const float theta = float(rand.Double(0.0, 2.0 * M_PI));
			const float u = float(rand.Double(-1.0, 1.0));

			// squeeze the starfield a bit to get more density near horizon using matrix3x3f::Scale
			const auto star = matrix3x3f::Scale(1.0, 1.0, 0.4) * (vector3f(sqrt(1.0f - u * u) * cos(theta), u, sqrt(1.0f - u * u) * sin(theta)).Normalized() * 1000.0f);

			stars.pos[i] = star;
			stars.color[i] = col;
			stars.brightness[i] = size;
			num++;
		}
		PROFILE_STOP()

		Output("Final stars number: %d\n", num);

		m_pointSprites->SetData(NUM_BG_STARS, std::move(stars.pos), std::move(stars.color), std::move(stars.brightness));
	}

	void Starfield::Draw()
	{
		PROFILE_SCOPED()
		// XXX would be nice to get rid of the Pi:: stuff here
		// the starfield is Z-up: bring the direction of travel from Y-up space into it
		const vector3f dir = Pi::game ? HyperspaceTravelDir() : vector3f(0.0f);
		RelativityData data;
		if (GetRelativityData(m_renderer, m_renderer->GetTransform().ApplyRotationOnly(vector3f(dir.z, dir.x, dir.y)), data)) {
			s_starfieldRelativistic->SetBufferDynamic("RelativityData"_hash, &data);
			m_pointSprites->Draw(m_renderer, s_starfieldRelativistic.Get());
		} else
			m_pointSprites->Draw(m_renderer, m_material.Get());
	}

	MilkyWay::MilkyWay(Graphics::Renderer *renderer)
	{
		m_renderer = renderer;

		//build milky way model in two strips (about 256 verts)
		std::unique_ptr<Graphics::VertexArray> bottom(new VertexArray(ATTRIB_POSITION | ATTRIB_DIFFUSE));
		std::unique_ptr<Graphics::VertexArray> top(new VertexArray(ATTRIB_POSITION | ATTRIB_DIFFUSE));

		const Color dark(Color::BLANK);
		const Color bright(13, 13, 13, 13);

		//bottom
		float theta;
		for (theta = 0.0; theta < 2.f * float(M_PI); theta += 0.1f) {
			bottom->Add(
				vector3f(100.0f * sin(theta), float(-40.0 - 30.0 * noise(vector3d(sin(theta), 1.0, cos(theta)))), 100.0f * cos(theta)),
				dark);
			bottom->Add(
				vector3f(100.0f * sin(theta), float(5.0 * noise(vector3d(sin(theta), 0.0, cos(theta)))), 100.0f * cos(theta)),
				bright);
		}
		theta = 2.f * float(M_PI);
		bottom->Add(
			vector3f(100.0f * sin(theta), float(-40.0 - 30.0 * noise(vector3d(sin(theta), 1.0, cos(theta)))), 100.0f * cos(theta)),
			dark);
		bottom->Add(
			vector3f(100.0f * sin(theta), float(5.0 * noise(vector3d(sin(theta), 0.0, cos(theta)))), 100.0f * cos(theta)),
			bright);
		//top
		for (theta = 0; theta < 2.f * float(M_PI); theta += 0.1f) {
			top->Add(
				vector3f(100.0f * sin(theta), float(5.0 * noise(vector3d(sin(theta), 0.0, cos(theta)))), 100.0f * cos(theta)),
				bright);
			top->Add(
				vector3f(100.0f * sin(theta), float(40.0 + 30.0 * noise(vector3d(sin(theta), -1.0, cos(theta)))), 100.0f * cos(theta)),
				dark);
		}
		theta = 2.f * float(M_PI);
		top->Add(
			vector3f(100.0f * sin(theta), float(5.0 * noise(vector3d(sin(theta), 0.0, cos(theta)))), 100.0f * cos(theta)),
			bright);
		top->Add(
			vector3f(100.0f * sin(theta), float(40.0 + 30.0 * noise(vector3d(sin(theta), -1.0, cos(theta)))), 100.0f * cos(theta)),
			dark);

		Graphics::VertexFormatDesc fmt = VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_DIFFUSE);
		//two strips in one buffer, but seems to work ok without degenerate triangles
		Graphics::VertexBuffer *vtxBuffer = renderer->CreateVertexBuffer(Graphics::BUFFER_USAGE_STATIC, bottom->GetNumVerts() + top->GetNumVerts(), fmt.bindings[0].stride);

		uint8_t *vtxPtr = vtxBuffer->Map<uint8_t>(Graphics::BUFFER_MAP_WRITE);
		size_t topSize = top->GetNumVerts() * fmt.bindings[0].stride;
		size_t bottomSize = bottom->GetNumVerts() * fmt.bindings[0].stride;

		top->PopulateRange(fmt, vtxPtr, topSize);
		bottom->PopulateRange(fmt, vtxPtr + topSize, bottomSize);

		vtxBuffer->Unmap();

		Graphics::MaterialDescriptor desc;
		Graphics::RenderStateDesc stateDesc;
		stateDesc.depthTest = false;
		stateDesc.depthWrite = false;
		stateDesc.primitiveType = Graphics::TRIANGLE_STRIP;

		m_material.Reset(m_renderer->CreateMaterial("starfield", desc, stateDesc, fmt));
		m_material->emissive = Color::WHITE;

		m_meshObject.reset(m_renderer->CreateMeshObject(fmt, vtxBuffer));
	}

	void MilkyWay::Draw()
	{
		assert(m_meshObject);
		assert(m_material);
		m_renderer->DrawMesh(m_meshObject.get(), m_material.Get());
	}

	Container::Container(Graphics::Renderer *renderer, Random &rand) :
		m_renderer(renderer),
		m_milkyWay(renderer),
		m_starField(renderer),
		m_universeBox(renderer),
		m_drawFlags(DRAW_SKYBOX | DRAW_STARS)
	{
		m_universeBox.LoadCubeMap(rand);
	}

	void Container::Draw(const matrix4x4d &transform)
	{
		PROFILE_SCOPED()
		m_renderer->SetTransform(matrix4x4f(transform));
		if (DRAW_SKYBOX & m_drawFlags) {
			m_universeBox.Draw();
		}
		if (DRAW_MILKY & m_drawFlags) {
			m_milkyWay.Draw();
		}
		if (DRAW_STARS & m_drawFlags) {
			auto Zup_to_Yup = matrix4x4f::FromRowMajor({
				0, 1, 0, 0,
				0, 0, 1, 0,
				1, 0, 0, 0,
				0, 0, 0, 1
			});
			m_renderer->SetTransform(matrix4x4f(transform) * Zup_to_Yup);
			m_starField.Draw();
		}
	}

	void Container::SetIntensity(float intensity)
	{
		PROFILE_SCOPED()
		intensity = Clamp(intensity, 0.0f, 1.0f);
		m_universeBox.SetIntensity(intensity);
		m_starField.SetIntensity(intensity);
		m_milkyWay.SetIntensity(intensity);
	}

	void Container::SetDrawFlags(const Uint32 flags)
	{
		m_drawFlags = flags;
	}

} //namespace Background
