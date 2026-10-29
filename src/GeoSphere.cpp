// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "GeoSphere.h"

#include "Aabb.h"
#include "CityOnPlanet.h"

#include "GameConfig.h"
#include "GeoPatch.h"
#include "GeoPatchContext.h"
#include "GeoPatchJobs.h"
#include "Game.h"
#include "MathUtil.h"
#include "Pi.h"
#include "PlanetClouds.h"
#include "Space.h"
#include "SpaceStation.h"
#include "Star.h"
#include "TerrainFarTexture.h"
#include "TerrainSurfaceTiles.h"
#include "TerrainTiles.h"
#include "RefCounted.h"
#include "galaxy/AtmosphereParameters.h"
#include "galaxy/StarSystem.h"
#include "graphics/Drawables.h"
#include "graphics/Frustum.h"
#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/RenderState.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "graphics/TextureBuilder.h"
#include "graphics/Types.h"
#include "graphics/VertexArray.h"
#include "perlin.h"
#include "utils.h"
#include "vcacheopt/vcacheopt.h"
#include <algorithm>
#include <deque>
#include <thread>

RefCountedPtr<GeoPatchContext> GeoSphere::s_patchContext;

// must be odd numbers
static const int detail_edgeLen[5] = {
	//7, 15, 25, 35, 55 -- old non power-of-2+1 values
	// some detail settings duplicated intentionally
	// in real terms provides only 3 settings
	// however this value is still used for gas giants
	// with 5 distinct settings elsewhere
	9, 17, 17, 33, 33
};

static const double gs_targetPatchTriLength(100.0);
static std::vector<GeoSphere *> s_allGeospheres;
static Uint32 s_debugFlags = GeoSphere::DebugFlags::DEBUG_NONE;

// patches split once the camera is closer than this many patch widths
static double s_lodDistanceScale = 8.0;
// 0..1: how much patches seen edge-on split later (see GeoPatch::LODUpdate)
static double s_lodForeshortening = 1.0;

// fractional part, for the planet-fixed texture origins the terrain shaders
// cannot compute precisely themselves
static float FractionalPart(const double v)
{
	return float(v - std::floor(v));
}

static vector3f FractionalPart(const vector3d &v)
{
	return vector3f(FractionalPart(v.x), FractionalPart(v.y), FractionalPart(v.z));
}

static double ReadLodForeshortening()
{
	return Clamp(Pi::config->Int("PlanetLodForeshortening", 100), 0, 100) / 100.0;
}

void GeoSphere::InitGeoSphere()
{
	s_patchContext.Reset(new GeoPatchContext(detail_edgeLen[Pi::detail.planets > 4 ? 4 : Pi::detail.planets]));
	s_lodDistanceScale = Clamp(double(Pi::config->Float("PlanetLodDistance", 15.0f)), 1.0, 30.0);
	s_lodForeshortening = ReadLodForeshortening();
}

//static
double GeoSphere::GetLodForeshortening()
{
	return s_lodForeshortening;
}

//static
double GeoSphere::GetLodDistanceScale()
{
	return s_lodDistanceScale;
}

//static
void GeoSphere::SetLodDistanceScale(const double scale)
{
	s_lodDistanceScale = Clamp(scale, 1.0, 30.0);
}

// The detail distance setting applies in full at mid altitude (about 200 to
// 2,000 km on Earth), where it shows most. Near the ground the terrain close
// by is already at full detail and the extra patches cost the most; in high
// orbit the far texture (TerrainFarTexture) draws the detail instead of the
// mesh. Both get a reduced value. Altitude is in planet radii, so this scales
// with body size.
static double LodDistanceAtAltitude(const double altitude)
{
	auto smoothStep = [](const double edge0, const double edge1, const double x) {
		const double t = Clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	};
	const double peak = s_lodDistanceScale;
	const double reduced = std::min(peak, std::max(4.0, peak * 0.35));
	const double weight = smoothStep(0.003, 0.03, altitude) * (1.0 - smoothStep(0.3, 1.0, altitude));
	return reduced + (peak - reduced) * weight;
}

void GeoSphere::UninitGeoSphere()
{
	TerrainTiles::Destroy();
	TerrainSurfaceTiles::Destroy();
	assert(s_patchContext.Unique());
	s_patchContext.Reset();
	PlanetClouds::Uninit();
}

static void print_info(const SystemBody *sbody, const Terrain *terrain)
{
	Log::Verbose(
		"Geosphere Init for {}:\n" \
		"\theight fractal: {}\n" \
		"\tcolour fractal: {}\n" \
		"\tseed: {}\n",
		sbody->GetName(), terrain->GetHeightFractalName(), terrain->GetColorFractalName(), sbody->GetSeed());
}

// static
void GeoSphere::UpdateAllGeoSpheres()
{
	PROFILE_SCOPED()
	// terrain detail settings; read every frame so the sliders apply live
	s_lodDistanceScale = Clamp(double(Pi::config->Float("PlanetLodDistance", 15.0f)), 1.0, 30.0);
	s_lodForeshortening = ReadLodForeshortening();
	// switched off in the Terrain Debug window: freed here, between frames,
	// not while other planets' draws using the atlas are queued
	if (!TerrainSurfaceTiles::Enabled())
		TerrainSurfaceTiles::Destroy();
	for (std::vector<GeoSphere *>::iterator i = s_allGeospheres.begin(); i != s_allGeospheres.end(); ++i) {
		(*i)->Update();
	}
}

// static
void GeoSphere::OnChangeGeoSphereDetailLevel()
{
	// the tiles hold the old terrain, perhaps at another size
	TerrainSurfaceTiles::Destroy();
	s_patchContext.Reset(new GeoPatchContext(detail_edgeLen[Pi::detail.planets > 4 ? 4 : Pi::detail.planets]));

	// reinit the geosphere terrain data
	for (std::vector<GeoSphere *>::iterator i = s_allGeospheres.begin(); i != s_allGeospheres.end(); ++i) {
		// clearout anything we don't need
		(*i)->Reset();

		// reinit the terrain with the new settings
		(*i)->m_terrain.Reset(Terrain::InstanceTerrain((*i)->GetSystemBody()));
		print_info((*i)->GetSystemBody(), (*i)->m_terrain.Get());

		// Reload the surface and atmosphere material (scattering option)
		if ((*i)->m_surfaceMaterial.Valid()) {
			(*i)->SetUpMaterials();
		}
	}
}

//static
bool GeoSphere::OnAddQuadSplitResult(const SystemPath &path, SQuadSplitResult *res)
{
	// Find the correct GeoSphere via it's system path, and give it the split result
	for (std::vector<GeoSphere *>::iterator i = s_allGeospheres.begin(), iEnd = s_allGeospheres.end(); i != iEnd; ++i) {
		if (path == (*i)->GetSystemBody()->GetPath()) {
			(*i)->AddQuadSplitResult(res);
			return true;
		}
	}
	// GeoSphere not found to return the data to, cancel and delete it instead
	if (res) {
		res->OnCancel();
		delete res;
	}
	return false;
}

//static
bool GeoSphere::OnAddSingleSplitResult(const SystemPath &path, SSingleSplitResult *res)
{
	// Find the correct GeoSphere via it's system path, and give it the split result
	for (std::vector<GeoSphere *>::iterator i = s_allGeospheres.begin(), iEnd = s_allGeospheres.end(); i != iEnd; ++i) {
		if (path == (*i)->GetSystemBody()->GetPath()) {
			(*i)->AddSingleSplitResult(res);
			return true;
		}
	}
	// GeoSphere not found to return the data to, cancel and delete it instead
	if (res) {
		res->OnCancel();
		delete res;
	}
	return false;
}

//static
void GeoSphere::SetDebugFlags(Uint32 flags)
{
	s_debugFlags = flags;
}

//static
Uint32 GeoSphere::GetDebugFlags()
{
	return s_debugFlags;
}

void GeoSphere::Reset()
{
	{
		std::deque<SSingleSplitResult *>::iterator iter = mSingleSplitResults.begin();
		while (iter != mSingleSplitResults.end()) {
			// finally pass SplitResults
			SSingleSplitResult *psr = (*iter);
			assert(psr);

			psr->OnCancel();

			// tidyup
			delete psr;

			// Next!
			++iter;
		}
		mSingleSplitResults.clear();
	}

	{
		std::deque<SQuadSplitResult *>::iterator iter = mQuadSplitResults.begin();
		while (iter != mQuadSplitResults.end()) {
			// finally pass SplitResults
			SQuadSplitResult *psr = (*iter);
			assert(psr);

			psr->OnCancel();

			// tidyup
			delete psr;

			// Next!
			++iter;
		}
		mQuadSplitResults.clear();
	}

	for (int p = 0; p < NUM_PATCHES; p++) {
		// delete patches
		if (m_patches[p]) {
			m_patches[p].reset();
		}
	}

	// baked from the terrain the patches had
	m_farTexture.reset();
	m_farTextureIdle = 0.0;

	CalculateMaxPatchDepth();

	m_visiblePatches.reserve(1024);

	m_initStage = eBuildFirstPatches;
}

GeoSphere::GeoSphere(const SystemBody *body) :
	BaseSphere(body),
	m_hasTempCampos(false),
	m_tempCampos(0.0),
	m_tempFrustum(800, 600, 0.5, 1.0, 1000.0),
	m_initStage(eBuildFirstPatches),
	m_maxDepth(0)
{
	print_info(body, m_terrain.Get());

	s_allGeospheres.emplace_back(this);

	CalculateMaxPatchDepth();

	m_visiblePatches.reserve(1024);

	if (Pi::config->Int("SortGeoPatches") == 0) {
		SetDebugFlags(GetDebugFlags() & ~DebugFlags::DEBUG_SORTGEOPATCHES);
	} else {
		SetDebugFlags(GetDebugFlags() | DebugFlags::DEBUG_SORTGEOPATCHES);
	}

	//SetUpMaterials is not called until first Render since light count is zero :)
}

GeoSphere::~GeoSphere()
{
	// update thread should not be able to access us now, so we can safely continue to delete
	assert(std::count(s_allGeospheres.begin(), s_allGeospheres.end(), this) == 1);
	s_allGeospheres.erase(std::find(s_allGeospheres.begin(), s_allGeospheres.end(), this));
}

// Results are always kept. They used to be dropped once 128 were waiting,
// which left the requesting patch flagged as waiting for a job forever: it
// never split again and blocked its parent from merging, so it stayed as a
// coarse flat patch. Higher terrain detail distances finish far more jobs
// per frame and hit that limit.
bool GeoSphere::AddQuadSplitResult(SQuadSplitResult *res)
{
	assert(res);
	mQuadSplitResults.emplace_back(res);
	return true;
}

bool GeoSphere::AddSingleSplitResult(SSingleSplitResult *res)
{
	assert(res);
	mSingleSplitResults.emplace_back(res);
	return true;
}

void GeoSphere::ProcessSplitResults()
{
	PROFILE_SCOPED()
	// now handle the single split results that define the base level of the quad tree
	{
		std::deque<SSingleSplitResult *>::iterator iter = mSingleSplitResults.begin();
		while (iter != mSingleSplitResults.end()) {
			// finally pass SplitResults
			SSingleSplitResult *psr = (*iter);
			assert(psr);

			const int32_t faceIdx = psr->face();
			if (m_patches[faceIdx]) {
				m_patches[faceIdx]->ReceiveHeightmap(psr);
			} else {
				psr->OnCancel();
			}

			// tidyup
			delete psr;

			// Next!
			++iter;
		}
		mSingleSplitResults.clear();
	}

	// now handle the quad split results
	{
		std::deque<SQuadSplitResult *>::iterator iter = mQuadSplitResults.begin();
		while (iter != mQuadSplitResults.end()) {
			// finally pass SplitResults
			SQuadSplitResult *psr = (*iter);
			assert(psr);

			const int32_t faceIdx = psr->face();
			if (m_patches[faceIdx]) {
				m_patches[faceIdx]->ReceiveHeightmaps(psr);
			} else {
				psr->OnCancel();
			}

			// tidyup
			delete psr;

			// Next!
			++iter;
		}
		mQuadSplitResults.clear();
	}
}

void GeoSphere::BuildFirstPatches()
{
	PROFILE_SCOPED()
	assert(!m_patches[0]);
	if (m_patches[0])
		return;

	CalculateMaxPatchDepth();

	// generate root face patches of the cube/sphere
	static const vector3d p1 = (vector3d(1, 1, 1)).Normalized();
	static const vector3d p2 = (vector3d(-1, 1, 1)).Normalized();
	static const vector3d p3 = (vector3d(-1, -1, 1)).Normalized();
	static const vector3d p4 = (vector3d(1, -1, 1)).Normalized();
	static const vector3d p5 = (vector3d(1, 1, -1)).Normalized();
	static const vector3d p6 = (vector3d(-1, 1, -1)).Normalized();
	static const vector3d p7 = (vector3d(-1, -1, -1)).Normalized();
	static const vector3d p8 = (vector3d(1, -1, -1)).Normalized();

	const uint64_t maxShiftDepth = GeoPatchID::MAX_SHIFT_DEPTH;

	m_patches[0].reset(new GeoPatch(s_patchContext, this, p1, p2, p3, p4, 0, (0ULL << maxShiftDepth)));
	m_patches[1].reset(new GeoPatch(s_patchContext, this, p4, p3, p7, p8, 0, (1ULL << maxShiftDepth)));
	m_patches[2].reset(new GeoPatch(s_patchContext, this, p1, p4, p8, p5, 0, (2ULL << maxShiftDepth)));
	m_patches[3].reset(new GeoPatch(s_patchContext, this, p2, p1, p5, p6, 0, (3ULL << maxShiftDepth)));
	m_patches[4].reset(new GeoPatch(s_patchContext, this, p3, p2, p6, p7, 0, (4ULL << maxShiftDepth)));
	m_patches[5].reset(new GeoPatch(s_patchContext, this, p8, p7, p6, p5, 0, (5ULL << maxShiftDepth)));

	for (int i = 0; i < NUM_PATCHES; i++) {
		m_patches[i]->RequestSinglePatch();
	}

	m_initStage = eRequestedFirstPatches;
}

bool GeoSphere::GetDrawnRadius(const vector3d &p, double &radius) const
{
	for (int i = 0; i < NUM_PATCHES; i++) {
		if (m_patches[i] && m_patches[i]->GetDrawnRadius(p, radius))
			return true;
	}
	return false;
}

double GeoSphere::GetDeepestVertexSpacing() const
{
	// as CalculateMaxPatchDepth() works it out
	const double circumference = 2.0 * M_PI * m_sbody->GetRadius();
	return circumference / double(s_patchContext->GetEdgeLen() * 8) / std::pow(2.0, double(m_maxDepth));
}

// Detail tile styles from the height fractal: craters on airless and
// cratered bodies, dunes on dune terrains, ridges on mountains and rugged
// terrain, rolling eroded hills elsewhere.
void GeoSphere::SetUpTileStyle()
{
	const std::string name = m_terrain->GetHeightFractalName();
	auto has = [&](const char *s) { return name.find(s) != std::string::npos; };
	float rolling = 1.0f, ridged = 0.0f, dunes = 0.0f, craters = 0.0f;
	if (has("Dunes")) {
		rolling = 0.4f;
		dunes = 0.8f;
	} else if (has("Mountains") || has("Rugged") || has("Ridged") || has("Barren") || has("Mapped")) {
		rolling = 0.5f;
		ridged = 0.6f;
	}
	const bool airless = GetSystemBody()->GetVolatileGas() < 0.05;
	if (has("Crater") || has("Asteroid") || airless) {
		craters = 1.0f;
		rolling *= 0.4f;
		ridged *= 0.4f;
		dunes = 0.0f;
	}
	m_tileStyle[0] = rolling;
	m_tileStyle[1] = ridged;
	m_tileStyle[2] = dunes;
	m_tileStyle[3] = craters;
}

void GeoSphere::CalculateMaxPatchDepth()
{
	const double circumference = 2.0 * M_PI * m_sbody->GetRadius();
	// calculate length of each edge segment (quad) times 4 due to that being the number around the sphere (1 per side, 4 sides for Root).
	double edgeMetres = circumference / double(s_patchContext->GetEdgeLen() * 8);
	// find out what depth we reach the desired resolution
	while (edgeMetres > gs_targetPatchTriLength && m_maxDepth < GEOPATCH_MAX_DEPTH) {
		edgeMetres *= 0.5;
		++m_maxDepth;
	}
}

void GeoSphere::Update()
{
	// keeps a bake going while the planet is out of view (it fades in as drawn)
	if (m_farTexture)
		m_farTexture->Update(0.0f);

	switch (m_initStage) {
	case eBuildFirstPatches:
		BuildFirstPatches();
		break;
	case eRequestedFirstPatches: {
		ProcessSplitResults();
		uint8_t numValidPatches = 0;
		for (int i = 0; i < NUM_PATCHES; i++) {
			if (m_patches[i]->HasHeightData()) {
				++numValidPatches;
			}
		}
		m_initStage = (NUM_PATCHES == numValidPatches) ? eReceivedFirstPatches : eRequestedFirstPatches;
	} break;
	case eReceivedFirstPatches: {
		for (int i = 0; i < NUM_PATCHES; i++) {
			m_patches[i]->SetNeedToUpdateVBOs();
		}
		m_initStage = eDefaultUpdateState;
	} break;
	case eDefaultUpdateState:
		if (m_hasTempCampos) {
			ProcessSplitResults();
			m_lodDistance = LodDistanceAtAltitude(m_tempCampos.Length() - 1.0);
			for (int i = 0; i < NUM_PATCHES; i++) {
				m_patches[i]->LODUpdate(m_tempCampos, m_tempFrustum);
			}
			ProcessQuadSplitRequests();
		}
		break;
	}
}

void GeoSphere::AddQuadSplitCandidate(double distance, GeoPatch *patch)
{
	mQuadSplitCandidates.emplace_back(distance, patch);
}

// Submits split jobs nearest first, keeping only a few per worker thread in
// flight. Previously every candidate was queued at once: at high terrain
// detail distances that flooded the job queue (which blocks the main thread
// when full) with work for patches the camera had often moved away from by
// the time it ran. Candidates not submitted now are re-evaluated on the next
// update.
void GeoSphere::ProcessQuadSplitRequests()
{
	static const int s_maxJobsInFlight = std::max(8, 3 * int(std::thread::hardware_concurrency()));

	const int slots = s_maxJobsInFlight - GetQuadPatchJobsInFlight();
	if (slots > 0 && !mQuadSplitCandidates.empty()) {
		const size_t count = std::min(mQuadSplitCandidates.size(), size_t(slots));
		std::partial_sort(mQuadSplitCandidates.begin(), mQuadSplitCandidates.begin() + count, mQuadSplitCandidates.end(),
			[](const std::pair<double, GeoPatch *> &a, const std::pair<double, GeoPatch *> &b) { return a.first < b.first; });
		for (size_t i = 0; i < count; i++) {
			mQuadSplitCandidates[i].second->RequestQuadSplit();
		}
	}
	mQuadSplitCandidates.clear();
}

// The night lights seen from afar: the starport cities, faded out where the
// camera is close enough to see their own lights. campos: the camera,
// planet-fixed, in planet radii. Sets them on the surface material, and
// fills data for the cloud shadows.
void GeoSphere::SetCityGlowData(const vector3d &campos, Graphics::CityGlowDataBlock &data)
{
	using Graphics::CityGlowDataBlock;
	if (!m_glowCitiesFound && Pi::game && Pi::game->GetSpace()) {
		m_glowCitiesFound = true;
		for (const SystemBody *kid : GetSystemBody()->GetChildren()) {
			if (kid->GetType() != SystemBody::TYPE_STARPORT_SURFACE)
				continue;
			const Body *body = Pi::game->GetSpace()->FindBodyForPath(&kid->GetPath());
			if (!body || !body->IsType(ObjectType::SPACESTATION))
				continue;
			const SpaceStation *station = static_cast<const SpaceStation *>(body);
			double cityRadius;
			if (CityOnPlanet::GetCitySummary(GetSystemBody(), station, cityRadius))
				m_glowCities.push_back({ station, station->GetPosition(), cityRadius });
		}
	}

	data = CityGlowDataBlock{};
	data.params[1] = float(std::min<size_t>(m_glowCities.size(), CityGlowDataBlock::MAX_CITIES));
	data.params[2] = Pi::config->Int("CityGlow") / 100.0f;

	const double radius = GetSystemBody()->GetRadius();
	for (size_t i = 0; i < m_glowCities.size() && i < size_t(CityGlowDataBlock::MAX_CITIES); i++) {
		const GlowCity &city = m_glowCities[i];
		const vector3d dir = city.pos.Normalized();
		data.cities[i][0] = float(dir.x);
		data.cities[i][1] = float(dir.y);
		data.cities[i][2] = float(dir.z);
		data.cities[i][3] = float(city.radius);

		const CityOnPlanet *lights = city.station->GetAdjacentCity();
		const double distance = (campos * radius - city.pos).Length();
		data.fade[i] = (lights && lights->HasLights()) ? float(1.0 - CityOnPlanet::LightsFade(distance)) : 1.0f;
	}

	m_surfaceMaterial->SetBufferDynamic("CityGlowData"_hash, &data);
}

// The far terrain texture: baked once the planet is large on screen, freed
// once it has been small for a while; drawn (terrain-far.glsl) where the
// patches' vertices are further apart than its texels.
void GeoSphere::SetFarTexture(const vector3d &campos, const double pixelAngle, const double spacingPerMetre)
{
	const int size = TerrainFarTexture::ConfiguredSize();
	const bool star = GetSystemBody()->GetSuperType() == SystemBody::SUPERTYPE_STAR;
	if (size == 0 || star || (m_farTexture && m_farTexture->GetSize() != size))
		m_farTexture.reset();

	// apparent size in pixels
	const double camDist = std::max(campos.Length(), 1.0);
	const double discPixels = 2.0 * std::asin(1.0 / camDist) / pixelAngle;
	if (size > 0 && !star && discPixels >= TerrainFarTexture::START_PIXELS) {
		m_farTextureIdle = 0.0;
		if (!m_farTexture && TerrainFarTexture::CanStart())
			m_farTexture.reset(new TerrainFarTexture(m_terrain, GetSystemBody()->GetName(), GetSystemBody()->GetRadius(), size));
	} else if (m_farTexture && discPixels < TerrainFarTexture::RELEASE_PIXELS) {
		m_farTextureIdle += Pi::GetFrameTime();
		if (m_farTextureIdle > TerrainFarTexture::RELEASE_SECONDS)
			m_farTexture.reset();
	} else {
		m_farTextureIdle = 0.0;
	}

	// the distance where the vertex spacing reaches the texel size: the
	// texture fades in from there to twice it
	float fade = 0.0f;
	double handover = 0.0;
	if (m_farTexture) {
		m_farTexture->Update(Pi::GetFrameTime());
		if (m_farTexture->IsReady()) {
			handover = m_farTexture->GetTexelMetres() / spacingPerMetre;
			// none of it shows when all the ground in sight (to the horizon,
			// and mountains beyond it) is nearer, nor in the patch depth view;
			// patches seen edge-on hand over up to half as far (terrain-far.glsl)
			const double top = 1.0 + GetMaxFeatureHeight();
			const double farthest = (std::sqrt(camDist * camDist - 1.0) + std::sqrt(top * top - 1.0)) * GetSystemBody()->GetRadius();
			if (farthest > handover * (1.0 - 0.5 * GetLodForeshortening()) && !(s_debugFlags & DebugFlags::DEBUG_PATCHDEPTH))
				fade = m_farTexture->GetFade();
		}
	}
	m_farTexelDrawn = fade > 0.0f ? m_farTexture->GetTexelMetres() / GetSystemBody()->GetRadius() : 0.0;
	m_farFadeDrawn = fade;
	// never leave the material with textures that may be freed
	m_surfaceMaterial->SetTexture("farColour"_hash, fade > 0.0f ? m_farTexture->GetColour() : nullptr);
	m_surfaceMaterial->SetTexture("farNormal"_hash, fade > 0.0f ? m_farTexture->GetNormal() : nullptr);
	m_surfaceMaterial->SetPushConstant("FarTexture"_hash, vector3f(float(handover), fade, float(GetLodForeshortening())), 0.0f);
}

// The patch surface tiles (TerrainSurfaceTiles) for the visible patches,
// twice as fine as their vertices; each patch sets its own in RenderImmediate.
void GeoSphere::SetSurfaceTiles(const double pixelAngle)
{
	TerrainSurfaceTiles *tiles = nullptr;
	if (TerrainSurfaceTiles::Enabled() && GetSystemBody()->GetSuperType() != SystemBody::SUPERTYPE_STAR)
		tiles = TerrainSurfaceTiles::Get(2 * (s_patchContext->GetEdgeLen() - 3)); // twice the vertex gaps
	if (tiles) {
		// only where finer than the far texture, which is sharper from afar
		const double farTexel = m_farTexture ? m_farTexture->GetTexelMetres() / GetSystemBody()->GetRadius() : 0.0;
		tiles->Update(this, m_visiblePatches, pixelAngle, farTexel);
	}
	m_surfaceMaterial->SetTexture("surfaceTileColour"_hash, tiles ? tiles->GetColour() : nullptr);
	m_surfaceMaterial->SetTexture("surfaceTileNormal"_hash, tiles ? tiles->GetNormal() : nullptr);
}

void GeoSphere::Render(Graphics::Renderer *renderer, const matrix4x4d &modelView, vector3d campos, const float radius, const std::vector<Camera::Shadow> &shadows)
{
	PROFILE_SCOPED()
	// store this for later usage in the update method.
	m_tempCampos = campos;
	m_hasTempCampos = true;

	if (m_initStage < eDefaultUpdateState)
		return;

	matrix4x4d trans = modelView;
	trans.Translate(-campos.x, -campos.y, -campos.z);
	renderer->SetTransform(matrix4x4f(trans)); //need to set this for the following line to work
	matrix4x4d modv = matrix4x4d(renderer->GetTransform());
	matrix4x4d proj = matrix4x4d(renderer->GetProjection());
	// radians per pixel, from the projection and the viewport height
	const double pixelAngle = 2.0 / (std::max(proj[5], 1e-6) * std::max(renderer->GetViewport().h, 1));
	Graphics::Frustum frustum(modv, proj);
	m_tempFrustum = frustum;

	// no frustum test of entire geosphere, since Space::Render does this
	// for each body using its GetBoundingRadius() value

	//First draw - create materials (they do not change afterwards)
	if (!m_surfaceMaterial)
		SetUpMaterials();

	//Update material parameters
	SetMaterialParameters(trans, radius, shadows, m_atmosphereParameters);
	Graphics::CityGlowDataBlock cityGlow{};
	if (GetSystemBody()->GetSuperType() != SystemBody::SUPERTYPE_STAR)
		SetCityGlowData(campos, cityGlow);

	if (m_terrain->GetSurfaceEffects() & Terrain::EFFECT_WATER) {
		// The water waves (terrain-water.glsl) are a tileable texture in three
		// layers of 2048 m, 128 m and 8 m tiles around the camera, and wraps
		// at 5 tiles. Pass the camera's planet-fixed position in units of 5 of
		// the largest tiles and each layer's drift in its own tiles, both
		// wrapped: the shader cannot get them precisely from the full values.
		const double largestTile = 2048.0;
		const vector3d tiles = campos * (GetSystemBody()->GetRadius() / (5.0 * largestTile));
		m_surfaceMaterial->SetPushConstant("WaterOrigin"_hash, FractionalPart(tiles), 0.0f);

		// each layer drifts at the deep water phase speed sqrt(g * wavelength / 2pi)
		// of its typical waves, 1/8 of the tile
		const double t = Pi::game ? Pi::game->GetTime() : 0.0;
		float drift[3];
		double tile = largestTile;
		for (float &d : drift) {
			d = 5.0f * FractionalPart(t * std::sqrt(9.81 * (tile / 8.0) / (2.0 * M_PI)) / (5.0 * tile));
			tile /= 16.0;
		}
		m_surfaceMaterial->SetPushConstant("WaterDrift"_hash, vector3f(drift[0], drift[1], drift[2]), 0.0f);
	}

	{
		// Land material textures (terrain-materials.glsl) are planet-fixed
		// tiles of 1 m to 128 km, wrapping at 5 tiles. Pass the camera's
		// planet-fixed position in units of five 256 m and five 128 km tiles.
		const vector3d camMetres = campos * GetSystemBody()->GetRadius();
		m_surfaceMaterial->SetPushConstant("MaterialOrigin"_hash, FractionalPart(camMetres / (5.0 * 256.0)), 0.0f);
		m_surfaceMaterial->SetPushConstant("MaterialOriginCoarse"_hash, FractionalPart(camMetres / (5.0 * 131072.0)), 0.0f);

		// Relief shading adds the detail below the mesh, so it needs the
		// vertex spacing: patches split within the terrain detail distance K
		// patch widths, so a patch at distance d is d/K to d/2K wide (about
		// d / 1.4K), split into the context's vertex gaps.
		// The deepest patches stop it from getting finer near the ground
		// (as CalculateMaxPatchDepth() works it out).
		const double vertexGaps = double(s_patchContext->GetEdgeLen() - 3); // edge length includes the skirt
		const double deepestSpacing = 2.0 * M_PI * GetSystemBody()->GetRadius() / double(s_patchContext->GetEdgeLen() * 8) / std::pow(2.0, double(m_maxDepth));
		const double spacingPerMetre = 1.0 / (1.41 * GetLodDistance() * vertexGaps);
		m_surfaceMaterial->SetPushConstant("ReliefScale"_hash,
			vector3f(float(spacingPerMetre), float(deepestSpacing), 0.0f), 0.0f);

		SetFarTexture(campos, pixelAngle, spacingPerMetre);
	}

	// cloud data: shadows in the terrain shader now, the clouds themselves in
	// the screen-space cloud pass after all bodies are drawn (PlanetClouds.h)
	{
		if (!m_cloudHeightMap && PlanetClouds::WantsHeightMap(GetSystemBody(), campos))
			m_cloudHeightMap.reset(new PlanetClouds::CloudHeightMap(m_terrain, GetSystemBody()->GetRadius()));

		PlanetClouds::CloudDataBlock cloudData;
		PlanetClouds::FillCloudData(cloudData, GetSystemBody(), trans, GetMaxFeatureHeight(), m_cloudHeightMap.get());
		PlanetClouds::SetMaterialCloudData(m_surfaceMaterial.Get(), cloudData, m_cloudHeightMap.get());
		PlanetClouds::QueueCloudPass(GetSystemBody(), cloudData, vector3f(trans * vector3d(0.0)) / radius,
			m_atmosphereParameters.atmosCol.ToColor4f(), m_cloudHeightMap.get(), cityGlow);
	}

	if (m_atmosphereMaterial.Valid() && m_atmosphereParameters.atmosDensity > 0.0) {
		// make atmosphere sphere slightly bigger than required so
		// that the edges of the pixel shader atmosphere jizz doesn't
		// show ugly polygonal angles
		renderer->BeginGpuTimer("atmosphere");
		DrawAtmosphereSurface(renderer, trans, campos,
			m_atmosphereParameters.atmosRadius * 1.02,
			m_atmosphereMaterial);
		renderer->EndGpuTimer();
	}

	Color ambient;
	Color &emission = m_surfaceMaterial->emissive;

	// save old global ambient
	const Color oldAmbient = renderer->GetAmbientColor();

	if ((GetSystemBody()->GetSuperType() == SystemBody::SUPERTYPE_STAR) || (GetSystemBody()->GetType() == SystemBody::TYPE_BROWN_DWARF)) {
		// stars should emit light and terrain should be visible from distance
		ambient.r = ambient.g = ambient.b = 51;
		ambient.a = 255;
		emission = GetSystemBody()->GetStarColor();
		emission.a = 255;

		// per-type surface look and its animation (geosphere_star.frag)
		const Star::VisualProfile vp = Star::GetVisualProfile(GetSystemBody());
		m_surfaceMaterial->SetPushConstant("StarSurface"_hash,
			vector3f(vp.granuleFrequency, vp.granuleContrast, vp.spotAmount), vp.bakedWeight);
		m_surfaceMaterial->SetPushConstant("StarTime"_hash, Star::GetAnimationAngles(), 0.0f);
	} else {
		// give planet some ambient lighting if the viewer is close to it
		double camdist = 0.1 / campos.LengthSqr();
		// why the fuck is this returning 0.1 when we are sat on the planet??
		// JJ: Because campos is relative to a unit-radius planet - 1.0 at the surface
		// XXX oh well, it is the value we want anyway...
		ambient.r = ambient.g = ambient.b = camdist * 255;
		ambient.a = 255;
	}

	renderer->SetAmbientColor(ambient);

	renderer->SetTransform(matrix4x4f(modelView));

	renderer->BeginGpuTimer("terrain");
	if (s_debugFlags & GeoSphere::DebugFlags::DEBUG_WIREFRAME)
		renderer->SetWireFrameMode(true);

	if (s_debugFlags & GeoSphere::DebugFlags::DEBUG_SORTGEOPATCHES) {
		// Gather the patches that could be rendered
		for (int i = 0; i < NUM_PATCHES; i++) {
			m_patches[i]->GatherRenderablePatches(m_visiblePatches, renderer, campos, frustum);
		}

		// distance sort the patches
		std::sort(m_visiblePatches.begin(), m_visiblePatches.end(), [&, campos](const std::pair<double, GeoPatch *> &a, const std::pair<double, GeoPatch *> &b) {
			return (a.first) < (b.first);
		});
		SetSurfaceTiles(pixelAngle);

		// cull occluded patches somehow?
		// create frustum from corner points, something vertical, and the campos??? Cull anything within that frustum?

		// render the sorted patches
		for (std::pair<double, GeoPatch *> &pPatch : m_visiblePatches) {
			pPatch.second->RenderImmediate(renderer, campos, modelView);
		}

		// must clear this after each render otherwise it just accumulates every patch ever drawn!
		m_visiblePatches.clear();
	} else {
		// gather the visible patches, bake the detail tiles they need, draw them
		for (int i = 0; i < NUM_PATCHES; i++) {
			m_patches[i]->GatherRenderablePatches(m_visiblePatches, renderer, campos, frustum);
		}
		// config TerrainDetailTiles (Terrain Debug window): 0 frees the atlas
		TerrainTiles *tiles = nullptr;
		if (Pi::config->Int("TerrainDetailTiles", 1) == 0)
			TerrainTiles::Destroy();
		else if (GetSystemBody()->GetSuperType() != SystemBody::SUPERTYPE_STAR)
			tiles = TerrainTiles::Get(renderer);
		if (tiles) {
			tiles->Update(renderer, this, m_visiblePatches, pixelAngle);
			m_surfaceMaterial->SetTexture("terrainTiles"_hash, tiles->GetAtlas());
		}
		SetSurfaceTiles(pixelAngle);
		for (std::pair<double, GeoPatch *> &pPatch : m_visiblePatches) {
			pPatch.second->RenderImmediate(renderer, campos, modelView);
		}
		m_visiblePatches.clear();
	}

	renderer->EndGpuTimer();
	if (s_debugFlags & GeoSphere::DebugFlags::DEBUG_WIREFRAME)
		renderer->SetWireFrameMode(false);

	renderer->SetAmbientColor(oldAmbient);

	renderer->GetStats().AddToStatCount(Graphics::Stats::STAT_PLANETS, 1);
}

void GeoSphere::RenderShadowCaster(Graphics::Renderer *renderer, const matrix4x4d &unitToLight, double radius, const Aabb &lightBox)
{
	PROFILE_SCOPED()
	if (!m_shadowMaterial) {
		Graphics::RenderStateDesc rsd;
		rsd.cullMode = Graphics::CULL_NONE;
		m_shadowMaterial.Reset(renderer->CreateMaterial("shadowcaster", Graphics::MaterialDescriptor(), rsd, GeoPatchContext::GetVertexFormat()));
	}
	for (int i = 0; i < NUM_PATCHES; i++)
		if (m_patches[i])
			m_patches[i]->RenderShadowCaster(renderer, unitToLight, radius, lightBox, m_shadowMaterial.Get());
}

void GeoSphere::SetUpMaterials()
{
	// XXX: this has to be synced with the vertex format used in GeoPatch
	auto vtxFormat = GeoPatchContext::GetVertexFormat();

	GetSystemBody()->CalcAtmosphereParams(m_atmosphereParameters);
	// normal star has a different setup path than geosphere terrain does
	if (GetSystemBody()->GetSuperType() == SystemBody::SUPERTYPE_STAR) {
		Graphics::MaterialDescriptor surfDesc;
		surfDesc.lighting = false;
		Graphics::RenderStateDesc rsd;
		m_surfaceMaterial.Reset(Pi::renderer->CreateMaterial("geosphere_star", surfDesc, rsd, vtxFormat));
	} else {
		// Request material for this star or planet, with or without
		// atmosphere. Separate material for surface and sky.
		Graphics::MaterialDescriptor surfDesc;
		const Uint32 effect_flags = m_terrain->GetSurfaceEffects();
		if (effect_flags & Terrain::EFFECT_LAVA)
			surfDesc.effect = Graphics::EFFECT_GEOSPHERE_TERRAIN_WITH_LAVA;
		else if (effect_flags & Terrain::EFFECT_WATER)
			surfDesc.effect = Graphics::EFFECT_GEOSPHERE_TERRAIN_WITH_WATER;
		else
			surfDesc.effect = Graphics::EFFECT_GEOSPHERE_TERRAIN;

		if ((GetSystemBody()->GetType() == SystemBody::TYPE_BROWN_DWARF) ||
			(GetSystemBody()->GetType() == SystemBody::TYPE_STAR_M)) {
			//dim star (emits and receives light)
			surfDesc.lighting = true;
			// emits light, so atmosphere should not affect visible light
			surfDesc.quality &= ~Graphics::HAS_ATMOSPHERE;
		} else {
			//planetoid with or without atmosphere
			surfDesc.lighting = true;
			if (m_atmosphereParameters.atmosDensity > 0.0) {
				surfDesc.quality |= Graphics::HAS_ATMOSPHERE;
			}
		}

		//solid blendmode
		Graphics::RenderStateDesc rsd;
		surfDesc.quality |= Graphics::HAS_ECLIPSES;
		const int scattering = Pi::config->Int("RealisticScattering");
		switch (scattering) {
		case 1:
			/* fallthrough */
		case 2:
			m_surfaceMaterial.Reset(Pi::renderer->CreateMaterial("rayleigh_geosphere_terrain", surfDesc, rsd, vtxFormat));
			break;
		default:
			m_surfaceMaterial.Reset(Pi::renderer->CreateMaterial("geosphere_terrain", surfDesc, rsd, vtxFormat));
			break;
		}

		m_texHi.Reset(Graphics::TextureBuilder::Model("textures/high.dds").GetOrCreateTexture(Pi::renderer, "model"));
		m_texLo.Reset(Graphics::TextureBuilder::Model("textures/low.dds").GetOrCreateTexture(Pi::renderer, "model"));
		m_surfaceMaterial->SetTexture("texture0"_hash, m_texHi.Get());
		m_surfaceMaterial->SetTexture("texture1"_hash, m_texLo.Get());

		// land material detail (terrain-materials.glsl); the layer order matches
		// the MATERIAL_* indices there
		// (TerrainMaterialTable::Layer)
		static const std::vector<std::string> materialFiles = {
			"textures/terrain/rock.dds", "textures/terrain/gravel.dds", "textures/terrain/sand.dds", "textures/terrain/soil.dds",
			"textures/terrain/grass.dds", "textures/terrain/forest.dds", "textures/terrain/snow.dds", "textures/terrain/ice.dds",
			"textures/terrain/basalt.dds", "textures/terrain/lava.dds", "textures/terrain/ash.dds", "textures/terrain/regolith.dds",
			"textures/terrain/salt.dds"
		};
		m_texMaterials.Reset(Graphics::TextureBuilder::Array(materialFiles, materialFiles.size()).GetOrCreateTexture(Pi::renderer, "terrain-materials"));
		m_surfaceMaterial->SetTexture("terrainMaterials"_hash, m_texMaterials.Get());
		// relief below the mesh (terrainReliefNormal() in terrain-materials.glsl)
		m_texRelief.Reset(Graphics::TextureBuilder::Normal("textures/terrain/relief.png").GetOrCreateTexture(Pi::renderer, "model"));
		m_surfaceMaterial->SetTexture("terrainRelief"_hash, m_texRelief.Get());
		SetUpTileStyle();

		// this body's material slots: texture layer per slot (-1 unused), and
		// the slots for steep rock, beaches and lava (-1 none)
		const TerrainMaterialTable &table = m_terrain->GetMaterialTable();
		auto layer = [&](int slot) { return slot < table.SlotCount() ? float(table.SlotLayer(slot)) : -1.0f; };
		m_surfaceMaterial->SetPushConstant("MaterialSlots0"_hash, vector3f(layer(0), layer(1), layer(2)), layer(3));
		m_surfaceMaterial->SetPushConstant("MaterialSlots1"_hash, vector3f(layer(4), layer(5), layer(6)), layer(7));
		m_surfaceMaterial->SetPushConstant("MaterialSpecial"_hash,
			vector3f(float(table.RockSlot()), float(table.BeachSlot()), float(table.SlotOf(TerrainMaterialTable::SNOW))), float(table.LavaSlot()));

		// wave slopes for the water shading (terrain-water.glsl)
		if (m_terrain->GetSurfaceEffects() & Terrain::EFFECT_WATER) {
			m_texWater.Reset(Graphics::TextureBuilder::Normal("textures/water_waves.png").GetOrCreateTexture(Pi::renderer, "model"));
			m_surfaceMaterial->SetTexture("waterWaves"_hash, m_texWater.Get());
		}

		// surface type patterns (terrain-surface.glsl) classify the terrain
		// colour using the palette's vegetation and sand hues
		const SurfaceProfile &profile = m_terrain->GetSurfaceProfile();
		m_surfaceMaterial->SetPushConstant("SurfaceMaterial"_hash,
			vector3f(float(profile.vegetationHue / 360.0), float(profile.SandHue() / 360.0), float(profile.vegetationCover)), 0.0f);
	}

	CreateAtmosphereMaterial();
}

void GeoSphere::CreateAtmosphereMaterial()
{
	Graphics::RenderStateDesc rsd;
	// atmosphere is blended over the background
	rsd.blendMode = Graphics::BLEND_ALPHA_ONE;
	rsd.cullMode = Graphics::CULL_FRONT;
	rsd.depthWrite = false;

	Graphics::MaterialDescriptor skyDesc;
	skyDesc.effect = Graphics::EFFECT_GEOSPHERE_SKY;
	skyDesc.lighting = true;
	skyDesc.quality |= Graphics::HAS_ECLIPSES;

	Graphics::VertexFormatDesc atmosVtxFmt = m_atmos->GetVertexFormat();

	const int scattering = Pi::config->Int("RealisticScattering");
	switch (scattering) {
	case 1:
		m_atmosphereMaterial.Reset(Pi::renderer->CreateMaterial("rayleigh_geosphere_sky_fast", skyDesc, rsd, atmosVtxFmt));
		break;
	case 2:
		m_atmosphereMaterial.Reset(Pi::renderer->CreateMaterial("rayleigh_geosphere_sky_full", skyDesc, rsd, atmosVtxFmt));
		break;
	default:
		m_atmosphereMaterial.Reset(Pi::renderer->CreateMaterial("geosphere_sky", skyDesc, rsd, atmosVtxFmt));
		break;
	}
}
