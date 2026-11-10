// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "GeoSphere.h"

#include "FrontierLog.h"

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
#include "Ship.h"
#include "Frame.h"
#include "SpaceStation.h"
#include "Star.h"
#include "TerrainSurfaceTiles.h"
#include "TerrainTiles.h"
#include "terrain/TerrainStones.h"
#include "RefCounted.h"
#include "surface/Excursion.h"
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
#include <cmath>
#include <deque>
#include <map>
#include <thread>
#include <tuple>

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
	Terrain::SetGenerator(Pi::config->Int("TerrainGenerator", 1));
}

//static
void GeoSphere::PrebuildPrograms(Graphics::Renderer *renderer)
{
	// (as SetUpMaterials makes them, and Material::SetTessellated the stones' version, under one sun: NUM_LIGHTS 1)
	const int scattering = Pi::config->Int("RealisticScattering");
	const std::string shader = (scattering == 1 || scattering == 2) ? "rayleigh_geosphere_terrain" : "geosphere_terrain";
	static const Graphics::EffectType effects[] = { Graphics::EFFECT_GEOSPHERE_TERRAIN, Graphics::EFFECT_GEOSPHERE_TERRAIN_WITH_WATER,
		Graphics::EFFECT_GEOSPHERE_TERRAIN_WITH_LAVA, Graphics::EFFECT_GEOSPHERE_TERRAIN_WITH_LAVA_AND_WATER };
	const bool stones = StonesEnabled() || TessellationEnabled();
	int count = 0;
	// (the plain ones first, which a load waits for; the stones' after, which it doesn't)
	for (int tessellated = 0; tessellated < (stones ? 2 : 1); tessellated++) {
		for (int air = 1; air >= 0; air--) {
			for (const Graphics::EffectType effect : effects) {
				Graphics::MaterialDescriptor desc;
				desc.effect = effect;
				desc.lighting = true;
				desc.dirLights = 1;
				desc.quality = Graphics::HAS_ECLIPSES | (air ? Graphics::HAS_ATMOSPHERE : 0);
				desc.tessellated = tessellated != 0;
				renderer->PrebuildProgram(shader, desc, GeoPatchContext::GetVertexFormat());
				count++;
			}
		}
	}
	Log::Info("Ground programs: {} asked for ahead; those not in the binary cache are built in the background", count);
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

// How a planet is drawn from high orbit (settings window, config PlanetOrbitSurface): the surface tiles alone, or the
// tiles with the detail distance kept up in high orbit, as fine as at mid altitude. Anything else in the config counts as
// the second.
enum OrbitSurface {
	ORBIT_TILES = 1,
	ORBIT_TILES_FULL_DETAIL = 2
};

static int GetOrbitSurface()
{
	return Pi::config->Int("PlanetOrbitSurface", ORBIT_TILES_FULL_DETAIL) == ORBIT_TILES ? ORBIT_TILES : ORBIT_TILES_FULL_DETAIL;
}

// The detail distance setting applies in full at mid altitude (about 200 to
// 2,000 km on Earth), where it shows most. Near the ground the terrain close
// by is already at full detail and the extra patches cost the most; in high
// orbit the surface tiles alone draw the detail (keepHighOrbit false, Planets
// from orbit: Surface tiles). Both get a reduced value.
// Altitude is in planet radii, so this scales with body size.
static double LodDistanceAtAltitude(const double altitude, const bool keepHighOrbit)
{
	auto smoothStep = [](const double edge0, const double edge1, const double x) {
		const double t = Clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	};
	const double peak = s_lodDistanceScale;
	const double reduced = std::min(peak, std::max(4.0, peak * 0.35));
	const double highOrbit = keepHighOrbit ? 1.0 : 1.0 - smoothStep(0.3, 1.0, altitude);
	const double weight = smoothStep(0.003, 0.03, altitude) * highOrbit;
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
	// P4d: with the stones on, standing on them as they're drawn
	const bool stones = StonesActive() && StonesContactEnabled() && m_hasTempCampos && m_terrain;
	GeoPatch::DrawnSurface surface;
	for (int i = 0; i < NUM_PATCHES; i++) {
		if (m_patches[i] && m_patches[i]->GetDrawnRadius(p, radius, stones ? &surface : nullptr)) {
			if (stones)
				radius += StoneLiftAt(p * radius, surface.normal, surface.slots, surface.seaHeight) / GetSystemBody()->GetRadius();
			return true;
		}
	}
	return false;
}

namespace {
	// the tallest of the body's materials' stones, as a share of the tile (MaterialStones.w)
	float DeepestStone(const Terrain *terrain)
	{
		const TerrainMaterialTable &table = terrain->GetMaterialTable();
		float deepest = 0.0f;
		for (int s = 0; s < table.SlotCount(); s++)
			deepest = std::max(deepest, TerrainStones::Depth(int(table.SlotLayer(s))));
		return deepest;
	}

	// the stones' constants, as the terrain material gets them
	TerrainStones::Params StoneParams(const Terrain *terrain, double pixelAngle)
	{
		TerrainStones::Params p;
		const TerrainMaterialTable &table = terrain->GetMaterialTable();
		for (int s = 0; s < 16; s++)
			p.slotLayer[s] = s < table.SlotCount() ? float(table.SlotLayer(s)) : -1.0f;
		p.usedSlots = table.SlotCount();
		p.rockSlot = table.RockSlot();
		p.beachSlot = table.BeachSlot();
		p.snowSlot = table.SlotOf(TerrainMaterialTable::SNOW);
		p.lavaSlot = table.LavaSlot();
		p.iceCracks = float(terrain->GetIceCrackStrength());
		p.fixedDetail = terrain->HasFixedDetail();
		p.nearLevel = float(GeoSphere::TextureFixedLevel());
		p.nearRange = float(GeoSphere::TextureFixedRange());
		p.nearOn = GeoSphere::TextureFixedEnabled();
		p.stonesOn = GeoSphere::StonesActive();
		p.stonesHeight = float(GeoSphere::StonesHeight());
		p.deepest = DeepestStone(terrain);
		p.pixelsPerRadian = float(1.0 / std::max(pixelAngle, 1e-9));
		p.aimPixels = float(GeoSphere::TessellationPixels());
		p.fullPixels = float(GeoSphere::StonesFullPixels());
		p.fadePixels = float(GeoSphere::StonesFadePixels());
		return p;
	}

	// the shaders' terrainAltitude()
	float StoneAltitude(float s)
	{
		return (s > 0.0f ? 1.0f : (s < 0.0f ? -1.0f : 0.0f)) * s * s * 8000.0f;
	}

	GeoSphere::StoneProbe s_stoneProbe;
	bool s_stoneProbeOn = false;
} // namespace

double GeoSphere::StoneLiftAt(const vector3d &point, const vector3d &normal, const float slots[16], float seaHeight) const
{
	const double radius = GetSystemBody()->GetRadius();
	TerrainStones::Point pt;
	pt.position = point * radius;
	pt.camera = m_tempCampos * radius;
	pt.normal = normal;
	std::copy(slots, slots + 16, pt.slots);
	pt.hasWater = (m_terrain->GetSurfaceEffects() & Terrain::EFFECT_WATER) != 0;
	pt.altitude = pt.hasWater ? StoneAltitude(seaHeight) : 1.0e4f;
	TerrainStones::Params params = StoneParams(m_terrain.Get(), m_pixelAngle);
	params.flatCount = std::min(int(m_stoneFlats.size()), int(TerrainStones::Params::MAX_FLATS));
	for (int i = 0; i < params.flatCount; i++) {
		params.flatCentre[i] = m_stoneFlats[i].centre;
		params.flatEnd[i] = m_stoneFlats[i].end;
		params.flatRadius[i] = m_stoneFlats[i].radius;
		params.flatMargin[i] = m_stoneFlats[i].margin;
	}
	double finest, perMetre;
	StoneMeshSpacing(finest, perMetre);
	params.meshFinest = float(finest);
	params.meshPerMetre = float(perMetre);
	params.maxCuts = 64.0f;
	return TerrainStones::Lift(params, pt);
}

void GeoSphere::CheckSeams() const
{
	std::vector<const GeoPatch *> drawn;
	for (int i = 0; i < NUM_PATCHES; i++)
		if (m_patches[i])
			m_patches[i]->CollectDrawn(drawn);
	struct Point {
		GeoPatch::EdgePoint e;
		int patch;
	};
	// (by direction, in cells a few millimetres wide on Earth: points that are one fall in one cell, but for those
	// on a cell's border, which is rare enough not to matter here)
	std::map<std::tuple<int64_t, int64_t, int64_t>, std::vector<Point>> cells;
	std::vector<GeoPatch::EdgePoint> points;
	for (int p = 0; p < int(drawn.size()); p++) {
		points.clear();
		drawn[p]->EdgePoints(points);
		for (const GeoPatch::EdgePoint &pt : points) {
			const auto key = std::make_tuple(int64_t(std::llround(pt.dir.x * 1e9)), int64_t(std::llround(pt.dir.y * 1e9)), int64_t(std::llround(pt.dir.z * 1e9)));
			cells[key].push_back({ pt, p });
		}
	}
	const double radius = GetSystemBody()->GetRadius();
	const vector3d camera = m_tempCampos;
	struct Seam {
		double step, distance;
		vector3d dir;
		int depthA, depthB;
	};
	std::vector<Seam> seams;
	// and where both keep their surface (near the camera): the normal and the material weights either side
	struct Surface {
		double distance, angle;
		int material, slot;
		vector3d dir;
		int depthA, depthB;
	};
	std::vector<Surface> surfaces;
	int shared = 0, sharedSurface = 0;
	for (const auto &cell : cells) {
		const std::vector<Point> &list = cell.second;
		for (size_t a = 0; a < list.size(); a++) {
			for (size_t b = a + 1; b < list.size(); b++) {
				if (list[a].patch == list[b].patch)
					continue;
				shared++;
				const GeoPatch::EdgePoint &ea = list[a].e, &eb = list[b].e;
				const double distance = (ea.dir * ea.radius - camera).Length() * radius;
				const double step = std::abs(ea.radius - eb.radius) * radius;
				if (step > 0.01)
					seams.push_back({ step, distance, ea.dir, drawn[list[a].patch]->GetDepth(), drawn[list[b].patch]->GetDepth() });
				if (ea.hasSurface && eb.hasSurface) {
					sharedSurface++;
					const double angle = std::acos(std::clamp(double(ea.normal.Dot(eb.normal)), -1.0, 1.0)) * 180.0 / M_PI;
					int material = 0, slot = -1;
					for (int s = 0; s < TerrainMaterialTable::SLOT_COUNT; s++) {
						const int d = std::abs(int(ea.materials[s]) - int(eb.materials[s]));
						if (d > material) {
							material = d;
							slot = s;
						}
					}
					if (angle > 0.5 || material > 4)
						surfaces.push_back({ distance, angle, material, slot, ea.dir, drawn[list[a].patch]->GetDepth(), drawn[list[b].patch]->GetDepth() });
				}
			}
		}
	}
	std::sort(seams.begin(), seams.end(), [](const Seam &a, const Seam &b) { return a.distance < b.distance; });
	char line[256];
	std::snprintf(line, sizeof(line), "SEAMS %s: %d drawn patches, %d edge points shared by two, %d at two heights (over 1 cm); the nearest of them:",
		GetSystemBody()->GetName().c_str(), int(drawn.size()), shared, int(seams.size()));
	FrontierLog::Note(line);
	for (size_t i = 0; i < seams.size() && i < 16; i++) {
		const Seam &s = seams[i];
		std::snprintf(line, sizeof(line), "    %.0f m from the camera, lat %.5f lon %.5f: %.3f m apart, levels %d and %d", s.distance,
			std::asin(std::clamp(s.dir.y, -1.0, 1.0)) * 180.0 / M_PI, std::atan2(s.dir.x, s.dir.z) * 180.0 / M_PI, s.step, s.depthA, s.depthB);
		FrontierLog::Note(line);
	}
	double worst = 0.0;
	for (const Seam &s : seams)
		worst = std::max(worst, s.step);
	std::snprintf(line, sizeof(line), "    the widest anywhere: %.3f m", worst);
	FrontierLog::Note(line);

	std::sort(surfaces.begin(), surfaces.end(), [](const Surface &a, const Surface &b) { return a.distance < b.distance; });
	double worstAngle = 0.0;
	int worstMaterial = 0;
	for (const Surface &s : surfaces) {
		worstAngle = std::max(worstAngle, s.angle);
		worstMaterial = std::max(worstMaterial, s.material);
	}
	std::snprintf(line, sizeof(line), "SEAMS' SURFACE: %d shared points with their surface kept, %d with different normals (over 0.5 degrees) or material weights (over 4 of 255); worst %.2f degrees, %d of 255; the nearest:",
		sharedSurface, int(surfaces.size()), worstAngle, worstMaterial);
	FrontierLog::Note(line);
	for (size_t i = 0; i < surfaces.size() && i < 16; i++) {
		const Surface &s = surfaces[i];
		std::snprintf(line, sizeof(line), "    %.0f m from the camera, lat %.5f lon %.5f: normals %.2f degrees apart, material slot %d weights %d of 255 apart, levels %d and %d",
			s.distance, std::asin(std::clamp(s.dir.y, -1.0, 1.0)) * 180.0 / M_PI, std::atan2(s.dir.x, s.dir.z) * 180.0 / M_PI, s.angle, s.slot, s.material, s.depthA, s.depthB);
		FrontierLog::Note(line);
	}
}

void GeoSphere::StoneMeshSpacing(double &finest, double &perMetre) const
{
	// (as GetDeepestVertexSpacing, at the depth the patches near the player reach; and as ReliefScale: a patch at a
	// distance d is about d / 1.4K wide within the terrain detail distance K, split into the context's vertex gaps)
	const int depth = std::min(int(GEOPATCH_MAX_DEPTH), int(m_maxDepth + m_extraDepth));
	finest = m_sbody->GetRadius() * 0.5 * M_PI / std::pow(2.0, double(depth)) * GeoPatchContext::GetFrac();
	perMetre = 1.0 / (1.41 * GetLodDistance() * double(s_patchContext->GetEdgeLen() - 3));
}

double GeoSphere::StonesFadePixels()
{
	return std::clamp(double(Pi::config->Float("TerrainStonesFadePixels", 4.0f)), 0.5, 16.0);
}

double GeoSphere::StonePixelsAt(double metres) const
{
	const double range = TextureFixedRange();
	const double t = std::clamp((metres - range) / range, 0.0, 1.0);
	const double fade = 1.0 - t * t * (3.0 - 2.0 * t);
	const double tallest = DeepestStone(m_terrain.Get()) * StonesHeight() * std::exp2(TextureFixedLevel()) * fade;
	return tallest / (std::max(metres, 1e-3) * std::max(m_pixelAngle, 1e-9));
}

double GeoSphere::StonesFullPixels()
{
	return std::clamp(double(Pi::config->Float("TerrainStonesFullPixels", 16.0f)), 1.0, 32.0);
}

void GeoSphere::UpdateStoneFlats(const vector3d &campos)
{
	m_stoneFlats.clear();
	if (!StonesActive())
		return;
	const double radius = GetSystemBody()->GetRadius();
	const vector3d camera = campos * radius;
	std::vector<StoneFlat> all;
	// the cities, as far out as their glow from afar reaches (their streets, lots, pads and gardens), the stones back
	// over 10 m beyond
	for (const GlowCity &city : m_glowCities)
		all.push_back({ city.pos, city.pos, city.radius, 10.0 });
	// The ships landed on this body (docked ones are on the cities' pads): the ground under the hull, along its length as
	// wide as it is, at its bottom, the stones back over 3 m beyond. (Until 2026-11-09 a circle as wide as the ship's whole
	// bounding sphere, and 10 m more: standing beside the ship there were no stones round you.)
	if (Pi::game && Pi::game->GetSpace()) {
		for (const Body *body : Pi::game->GetSpace()->GetBodies()) {
			if (!body->IsType(ObjectType::SHIP) || !static_cast<const Ship *>(body)->IsLanded())
				continue;
			const Frame *frame = Frame::GetFrame(body->GetFrame());
			if (!frame || !frame->IsRotFrame() || !frame->GetBody() || frame->GetBody()->GetSystemBody() != GetSystemBody())
				continue;
			const Aabb &box = static_cast<const Ship *>(body)->GetAabb();
			if (!(box.max.x > box.min.x && box.max.z > box.min.z)) {
				all.push_back({ body->GetPosition(), body->GetPosition(), body->GetPhysRadius(), 3.0 });
				continue;
			}
			// (in the ship's own axes, y up: the longer of x and z is its length)
			const vector3d half = (box.max - box.min) * 0.5;
			const bool alongZ = half.z >= half.x;
			const double width = alongZ ? half.x : half.z;
			const double along = std::max((alongZ ? half.z : half.x) - width, 0.0);
			const vector3d bottom = vector3d((box.min.x + box.max.x) * 0.5, box.min.y, (box.min.z + box.max.z) * 0.5);
			const vector3d axis = alongZ ? vector3d(0.0, 0.0, along) : vector3d(along, 0.0, 0.0);
			const matrix3x3d orient = body->GetOrient();
			const vector3d pos = body->GetPosition();
			all.push_back({ pos + orient * (bottom - axis), pos + orient * (bottom + axis), width, 3.0 });
		}
	}
	// the nearest to the camera that the stones could reach
	auto gap = [&](const StoneFlat &flat) {
		const vector3d ab = flat.end - flat.centre;
		const double t = std::clamp((camera - flat.centre).Dot(ab) / std::max(ab.LengthSqr(), 1e-6), 0.0, 1.0);
		return (camera - (flat.centre + ab * t)).Length() - flat.radius;
	};
	const double reach = StonesReach() + 10.0;
	std::sort(all.begin(), all.end(), [&](const StoneFlat &a, const StoneFlat &b) { return gap(a) < gap(b); });
	for (const StoneFlat &flat : all) {
		if (int(m_stoneFlats.size()) >= TerrainStones::Params::MAX_FLATS || gap(flat) > reach)
			break;
		m_stoneFlats.push_back(flat);
	}
}

void GeoSphere::SetStoneFlats(Graphics::Material *material, const matrix4x4d &trans) const
{
	static const size_t names[TerrainStones::Params::MAX_FLATS] = { "StoneFlat0"_hash, "StoneFlat1"_hash, "StoneFlat2"_hash, "StoneFlat3"_hash, "StoneFlat4"_hash, "StoneFlat5"_hash };
	static const size_t ends[TerrainStones::Params::MAX_FLATS] = { "StoneFlatEnd0"_hash, "StoneFlatEnd1"_hash, "StoneFlatEnd2"_hash, "StoneFlatEnd3"_hash, "StoneFlatEnd4"_hash, "StoneFlatEnd5"_hash };
	const double radius = GetSystemBody()->GetRadius();
	for (int i = 0; i < TerrainStones::Params::MAX_FLATS; i++) {
		if (i < int(m_stoneFlats.size())) {
			const StoneFlat &flat = m_stoneFlats[i];
			material->SetPushConstant(names[i], vector3f(trans * (flat.centre / radius)), float(flat.radius));
			material->SetPushConstant(ends[i], vector3f(trans * (flat.end / radius)), float(flat.margin));
		} else {
			material->SetPushConstant(names[i], vector3f(0.0f), 0.0f);
			material->SetPushConstant(ends[i], vector3f(0.0f), 0.0f);
		}
	}
}

bool GeoSphere::GetGroundMix(const vector3d &dir, GroundMix &mix) const
{
	mix = GroundMix();
	if (!m_terrain)
		return false;
	GeoPatch::DrawnSurface surface;
	double radius = 0.0;
	bool found = false;
	for (int i = 0; i < NUM_PATCHES && !found; i++)
		found = m_patches[i] && m_patches[i]->GetDrawnRadius(dir, radius, &surface);
	if (!found || !surface.found)
		return false;
	if ((m_terrain->GetSurfaceEffects() & Terrain::EFFECT_WATER) && StoneAltitude(surface.seaHeight) < 0.5f) {
		mix.water = 1.0f;
		return true;
	}
	const TerrainMaterialTable &table = m_terrain->GetMaterialTable();
	float total = 0.0f;
	for (int s = 0; s < table.SlotCount() && s < 16; s++) {
		const float w = surface.slots[s];
		switch (table.SlotLayer(s)) {
		case TerrainMaterialTable::SAND:
		case TerrainMaterialTable::REGOLITH:
		case TerrainMaterialTable::DUST:
		case TerrainMaterialTable::ASH:
		case TerrainMaterialTable::SOIL:
		case TerrainMaterialTable::SALT:
		case TerrainMaterialTable::CRACKED_MUD: mix.dust += w; break;
		case TerrainMaterialTable::SNOW:
		case TerrainMaterialTable::ICE:
		case TerrainMaterialTable::DIRTY_ICE: mix.ice += w; break;
		case TerrainMaterialTable::GRASS:
		case TerrainMaterialTable::FOREST:
		case TerrainMaterialTable::FOREST_FAR:
		case TerrainMaterialTable::TUNDRA: mix.plants += w; break;
		default: mix.rock += w; break;
		}
		total += w;
	}
	if (total <= 1e-3f)
		return false;
	mix.dust /= total;
	mix.rock /= total;
	mix.ice /= total;
	mix.plants /= total;
	return true;
}

static int s_groundSkip = 0;

// The functions of terrain-materials.glsl that MaterialSlotData stands in for, in the same single precision:
// materialHsv(), materialColour(), materialMeanRoughness(), materialRoughness(), materialBumpStrength()
namespace {
	using Layer = TerrainMaterialTable::Layer;

	vector3f MaterialHsv(float h, float s, float v)
	{
		auto channel = [h](float offset) {
			const float x = h * 6.0f + offset;
			const float m = x - 6.0f * std::floor(x / 6.0f);
			return std::min(std::max(std::abs(m - 3.0f) - 1.0f, 0.0f), 1.0f);
		};
		const vector3f rgb(channel(0.0f), channel(4.0f), channel(2.0f));
		return vector3f(1.0f + (rgb.x - 1.0f) * s, 1.0f + (rgb.y - 1.0f) * s, 1.0f + (rgb.z - 1.0f) * s) * v;
	}

	vector3f MaterialColour(int m, float vegHue, float sandHue)
	{
		switch (m) {
		case TerrainMaterialTable::ROCK: return vector3f(0.36f, 0.34f, 0.32f);
		case TerrainMaterialTable::GRAVEL: return vector3f(0.42f, 0.40f, 0.37f);
		case TerrainMaterialTable::SAND: return MaterialHsv(sandHue, 0.42f, 0.72f);
		case TerrainMaterialTable::SOIL: return MaterialHsv(sandHue, 0.40f, 0.42f);
		case TerrainMaterialTable::GRASS: return MaterialHsv(vegHue, 0.55f, 0.38f);
		case TerrainMaterialTable::FOREST: return MaterialHsv(vegHue, 0.60f, 0.20f);
		case TerrainMaterialTable::SNOW: return vector3f(0.90f, 0.92f, 0.95f);
		case TerrainMaterialTable::ICE: return vector3f(0.82f, 0.88f, 0.94f);
		case TerrainMaterialTable::BASALT: return vector3f(0.12f, 0.11f, 0.11f);
		case TerrainMaterialTable::LAVA: return vector3f(0.15f, 0.06f, 0.04f);
		case TerrainMaterialTable::ASH: return vector3f(0.18f, 0.17f, 0.16f);
		case TerrainMaterialTable::REGOLITH: return vector3f(0.45f, 0.44f, 0.42f);
		case TerrainMaterialTable::TUNDRA: return MaterialHsv(vegHue, 0.30f, 0.36f);
		case TerrainMaterialTable::DIRTY_ICE: return vector3f(0.33f, 0.31f, 0.28f);
		case TerrainMaterialTable::LAVA_FLOW: return vector3f(0.09f, 0.07f, 0.07f);
		case TerrainMaterialTable::CRACKED_MUD: return vector3f(0.55f, 0.46f, 0.35f);
		case TerrainMaterialTable::DUST: return MaterialHsv(sandHue, 0.35f, 0.55f);
		case TerrainMaterialTable::METAL: return vector3f(0.42f, 0.40f, 0.37f);
		default: return vector3f(0.85f, 0.83f, 0.78f);
		}
	}

	float MaterialMeanRoughness(int m, float iceCracks, bool fixedDetail)
	{
		switch (m) {
		case TerrainMaterialTable::ROCK: return 0.693f;
		case TerrainMaterialTable::GRAVEL: return 0.541f;
		case TerrainMaterialTable::SAND: return 0.652f;
		case TerrainMaterialTable::SOIL: return 0.675f;
		case TerrainMaterialTable::GRASS: return 0.263f;
		case TerrainMaterialTable::FOREST:
		case TerrainMaterialTable::FOREST_FAR: return 0.612f;
		case TerrainMaterialTable::SNOW: return 0.707f;
		case TerrainMaterialTable::ICE: {
			const float old = fixedDetail ? 0.6f * (1.0f - std::min(std::max(iceCracks / 0.5f, 0.0f), 1.0f)) : 0.0f;
			return 0.140f + (0.707f - 0.140f) * old;
		}
		case TerrainMaterialTable::BASALT: return 0.689f;
		case TerrainMaterialTable::LAVA: return 0.418f;
		case TerrainMaterialTable::ASH:
		case TerrainMaterialTable::DUST: return 0.967f;
		case TerrainMaterialTable::REGOLITH: return 0.593f;
		case TerrainMaterialTable::SALT: return 0.598f;
		case TerrainMaterialTable::TUNDRA: return 0.609f;
		case TerrainMaterialTable::DIRTY_ICE: return 0.937f;
		case TerrainMaterialTable::LAVA_FLOW: return 0.414f;
		case TerrainMaterialTable::CRACKED_MUD: return 0.756f;
		case TerrainMaterialTable::METAL: return 0.456f;
		default: return 0.7f;
		}
	}

	float MaterialReliefRoughness(int m, float iceCracks)
	{
		switch (m) {
		case TerrainMaterialTable::ROCK: return 0.17f;
		case TerrainMaterialTable::BASALT: return 0.12f;
		case TerrainMaterialTable::GRAVEL: return 0.11f;
		case TerrainMaterialTable::DIRTY_ICE: return 0.10f;
		case TerrainMaterialTable::FOREST: return 0.09f;
		case TerrainMaterialTable::SOIL:
		case TerrainMaterialTable::CRACKED_MUD: return 0.07f;
		case TerrainMaterialTable::REGOLITH:
		case TerrainMaterialTable::GRASS:
		case TerrainMaterialTable::TUNDRA: return 0.06f;
		case TerrainMaterialTable::ASH:
		case TerrainMaterialTable::LAVA_FLOW:
		case TerrainMaterialTable::METAL: return 0.05f;
		case TerrainMaterialTable::SAND:
		case TerrainMaterialTable::SNOW: return 0.04f;
		case TerrainMaterialTable::SALT: return 0.025f;
		case TerrainMaterialTable::ICE: {
			const float t = std::min(std::max(iceCracks / 0.5f, 0.0f), 1.0f);
			return 0.08f + (0.02f - 0.08f) * t;
		}
		default: return 0.02f;
		}
	}

	float MaterialBumpStrength(int m)
	{
		switch (m) {
		case TerrainMaterialTable::DUST: return 0.25f;
		case TerrainMaterialTable::LAVA:
		case TerrainMaterialTable::ICE: return 0.35f;
		case TerrainMaterialTable::SNOW:
		case TerrainMaterialTable::ASH: return 0.5f;
		case TerrainMaterialTable::SAND: return 0.25f;
		case TerrainMaterialTable::LAVA_FLOW: return 0.6f;
		default: return 1.0f;
		}
	}
} // namespace

void GeoSphere::FillMaterialSlotData()
{
	const TerrainMaterialTable &table = m_terrain->GetMaterialTable();
	const SurfaceProfile &profile = m_terrain->GetSurfaceProfile();
	const float vegHue = float(profile.vegetationHue / 360.0), sandHue = float(profile.SandHue() / 360.0);
	const float iceCracks = float(m_terrain->GetIceCrackStrength());
	const bool fixedDetail = m_terrain->HasFixedDetail();
	for (int s = 0; s < 16; s++) {
		const int layer = s < table.SlotCount() ? int(table.SlotLayer(s)) : -1;
		const vector3f colour = layer >= 0 ? MaterialColour(layer, vegHue, sandHue) : vector3f(0.0f);
		m_materialSlotData.colour[s][0] = colour.x;
		m_materialSlotData.colour[s][1] = colour.y;
		m_materialSlotData.colour[s][2] = colour.z;
		m_materialSlotData.colour[s][3] = layer >= 0 ? MaterialMeanRoughness(layer, iceCracks, fixedDetail) : 0.7f;
		m_materialSlotData.traits[s][0] = layer >= 0 ? MaterialReliefRoughness(layer, iceCracks) : 0.0f;
		m_materialSlotData.traits[s][1] = layer >= 0 ? MaterialBumpStrength(layer) : 1.0f;
		m_materialSlotData.traits[s][2] = 0.0f;
		m_materialSlotData.traits[s][3] = 0.0f;
	}
}


Graphics::Texture *GeoSphere::CellHashTexture(Graphics::Renderer *renderer)
{
	static RefCountedPtr<Graphics::Texture> s_texture;
	if (!s_texture) {
		// 25 x 2: x the cell (y * 5 + x), y the salt (materialCellHashTable)
		float texels[50];
		for (int salt = 0; salt < 2; salt++)
			for (int cell = 0; cell < 25; cell++)
				texels[salt * 25 + cell] = TerrainStones::CellHash(float(cell % 5), float(cell / 5), uint32_t(salt));
		const Graphics::TextureDescriptor desc(Graphics::TEXTURE_R32F, vector3f(25.0f, 2.0f, 0.0f), Graphics::NEAREST_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
		s_texture.Reset(renderer->CreateTexture(desc));
		s_texture->Update(texels, desc.dataSize, Graphics::TEXTURE_R32F);
	}
	return s_texture.Get();
}

int GeoSphere::GetGroundSkip()
{
	return s_groundSkip;
}

void GeoSphere::SetGroundSkip(int mask)
{
	s_groundSkip = mask;
}

bool GeoSphere::TextureSimpleFar()
{
	return Pi::config->Int("TerrainTextureSimpleFar", 0) != 0;
}

double GeoSphere::TextureSimpleFrom()
{
	return std::clamp(double(Pi::config->Float("TerrainTextureSimpleFrom", 1500.0f)), 100.0, 20000.0);
}

bool GeoSphere::TextureOneScale()
{
	return Pi::config->Int("TerrainTextureOneScale", 0) != 0;
}

bool GeoSphere::TurnTable()
{
	// (a new name, off: the table was TerrainTurnTable, on, until 2026-11-10)
	return Pi::config->Int("TerrainTurnTexture", 0) != 0;
}

double GeoSphere::TextureScaleBand()
{
	return std::clamp(double(Pi::config->Float("TerrainTextureScaleBand", 0.3f)), 0.02, 1.0);
}

bool GeoSphere::SkyTableEnabled()
{
	return Pi::config->Int("SkyTable", 1) != 0;
}

bool GeoSphere::StonesContactEnabled()
{
	return Pi::config->Int("TerrainStonesContact", 1) != 0;
}

void GeoSphere::SetStoneProbe(bool on)
{
	s_stoneProbeOn = on;
	if (!on)
		s_stoneProbe = StoneProbe();
}

const GeoSphere::StoneProbe &GeoSphere::GetStoneProbe()
{
	return s_stoneProbe;
}

void GeoSphere::RunStoneProbe(Graphics::Renderer *renderer, const matrix4x4d &trans, const vector3d &campos, PlanetClouds::CloudDataBlock &cloudData)
{
	// (only a body with fine ground under the camera finds any: the one you're on)
	if (!s_stoneProbeOn || !StonesActive())
		return;

	// last frame's answer from the graphics card (its commands have run by now)
	if (m_probePending && m_probeImage) {
		float gpu = 0.0f;
		if (renderer->ReadTexture(m_probeImage.Get(), &gpu, sizeof(gpu))) {
			s_stoneProbe.gpu = gpu;
			s_stoneProbe.valid = true;
		}
		m_probePending = false;
	}

	// the drawn ground under the camera
	const vector3d dir = campos.Normalized();
	GeoPatch::DrawnSurface surface;
	double ground = 0.0;
	bool found = false;
	for (int i = 0; i < NUM_PATCHES && !found; i++)
		found = m_patches[i] && m_patches[i]->GetDrawnRadius(dir, ground, &surface);
	if (!found)
		return;
	const vector3d point = dir * ground;
	const double radius = GetSystemBody()->GetRadius();
	const double cpu = StoneLiftAt(point, surface.normal, surface.slots, surface.seaHeight);

	if (!m_probeMaterial) {
		const Graphics::TextureDescriptor desc(Graphics::TEXTURE_R32F, vector3f(1.0f, 1.0f, 0.0f), Graphics::NEAREST_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
		m_probeImage.Reset(renderer->CreateTexture(desc));
		m_probeMaterial.Reset(renderer->CreateMaterial("stone_probe", Graphics::MaterialDescriptor(), Graphics::RenderStateDesc(), Graphics::VertexFormatDesc()));
		m_probeMaterial->SetTexture("probeOut"_hash, m_probeImage.Get());
	}
	Graphics::Material *m = m_probeMaterial.Get();
	const TerrainMaterialTable &table = m_terrain->GetMaterialTable();
	auto layer = [&](int slot) { return slot < table.SlotCount() ? float(table.SlotLayer(slot)) : -1.0f; };
	m->SetPushConstant("MaterialSlots0"_hash, vector3f(layer(0), layer(1), layer(2)), layer(3));
	m->SetPushConstant("MaterialSlots1"_hash, vector3f(layer(4), layer(5), layer(6)), layer(7));
	m->SetPushConstant("MaterialSlots2"_hash, vector3f(layer(8), layer(9), layer(10)), layer(11));
	m->SetPushConstant("MaterialSlots3"_hash, vector3f(layer(12), layer(13), layer(14)), layer(15));
	m->SetPushConstant("MaterialSpecial"_hash, vector3f(float(table.RockSlot()), float(table.BeachSlot()), float(table.SlotOf(TerrainMaterialTable::SNOW))), float(table.LavaSlot()));
	m->SetPushConstant("MaterialLook"_hash, vector3f(m_terrain->GetIceCrackStrength(), m_terrain->HasFixedDetail() ? 1.0f : 0.0f, 0.0f), float(table.SlotCount()));
	m->SetPushConstant("MaterialNear"_hash, vector3f(float(TextureFixedLevel()), float(TextureFixedRange()), TextureFixedEnabled() ? 1.0f : 0.0f), float(StonesFadePixels()));
	m->SetPushConstant("MaterialStones"_hash, vector3f(1.0f, float(StonesHeight()), 0.0f), DeepestStone(m_terrain.Get()));
	m->SetPushConstant("TessParams"_hash, vector3f(float(1.0 / std::max(m_pixelAngle, 1e-9)), float(TessellationPixels()), 64.0f), float(StonesFullPixels()));
	{
		double finest, perMetre;
		StoneMeshSpacing(finest, perMetre);
		m->SetPushConstant("StoneMesh"_hash, vector3f(float(finest), float(perMetre), 0.0f), 0.0f);
	}
	const vector3d camMetres = campos * radius;
	m->SetPushConstant("MaterialOrigin"_hash, FractionalPart(camMetres / (5.0 * 256.0)), 0.0f);
	m->SetPushConstant("MaterialOriginCoarse"_hash, FractionalPart(camMetres / (5.0 * 131072.0)), 0.0f);
	// the point, its up and the mesh's normal in view space, as the evaluation shaders have them
	m->SetPushConstant("ProbeEye"_hash, vector3f(trans * point), 0.0f);
	m->SetPushConstant("ProbeUp"_hash, vector3f(trans.ApplyRotationOnly(dir).Normalized()), 0.0f);
	m->SetPushConstant("ProbeNormal"_hash, vector3f(trans.ApplyRotationOnly(surface.normal).Normalized()), 0.0f);
	m->SetPushConstant("ProbeSlots0"_hash, vector3f(surface.slots[0], surface.slots[1], surface.slots[2]), surface.slots[3]);
	m->SetPushConstant("ProbeSlots1"_hash, vector3f(surface.slots[4], surface.slots[5], surface.slots[6]), surface.slots[7]);
	m->SetPushConstant("ProbeSlots2"_hash, vector3f(surface.slots[8], surface.slots[9], surface.slots[10]), surface.slots[11]);
	m->SetPushConstant("ProbeSlots3"_hash, vector3f(surface.slots[12], surface.slots[13], surface.slots[14]), surface.slots[15]);
	const bool water = (m_terrain->GetSurfaceEffects() & Terrain::EFFECT_WATER) != 0;
	m->SetPushConstant("ProbeGround"_hash, vector3f(water ? StoneAltitude(surface.seaHeight) : 1.0e4f, water ? 1.0f : 0.0f, 0.0f), 0.0f);
	m->SetTexture("terrainStoneHeights"_hash, TerrainStones::GetTexture(renderer));
	SetStoneFlats(m, trans);
	PlanetClouds::SetMaterialCloudData(m, cloudData, m_cloudHeightMap.get());
	renderer->DispatchCompute(m, 1, 1, 1);
	renderer->AddComputeBarrier(Graphics::Renderer::BARRIER_ALL);
	m_probePending = true;

	// (the main processor's answer and what it's of, kept for when the card's comes in)
	s_stoneProbe.cpu = cpu;
	s_stoneProbe.distance = (point - campos).Length() * radius;
	s_stoneProbe.meshData = surface.found;
	float w[16];
	std::copy(surface.slots, surface.slots + 16, w);
	int m1 = 0, m2 = 1;
	for (int i = 1; i < table.SlotCount(); ++i) {
		if (w[i] > w[m1]) {
			m2 = m1;
			m1 = i;
		} else if (i != m1 && w[i] > w[m2]) {
			m2 = i;
		}
	}
	s_stoneProbe.layer1 = table.SlotCount() > 0 ? int(table.SlotLayer(m1)) : -1;
	s_stoneProbe.layer2 = table.SlotCount() > 1 ? int(table.SlotLayer(m2)) : -1;
}

double GeoSphere::GetDeepestVertexSpacing() const
{
	// a face of the cube spans a quarter turn across its middle, halved at each level, split into the context's
	// vertex gaps (GeoPatch::GetVertexSpacing)
	return m_sbody->GetRadius() * 0.5 * M_PI / std::pow(2.0, double(m_maxDepth)) * GeoPatchContext::GetFrac();
}

double GeoSphere::GetLegacyDeepestSpacing() const
{
	const double circumference = 2.0 * M_PI * m_sbody->GetRadius();
	return circumference / double(s_patchContext->GetEdgeLen() * 8) / std::pow(2.0, double(m_maxDepth));
}

// Detail tile styles: the terrain's own, or from the height fractal (Terrain::GetLandformStyle, which the landforms in
// the heights use too)
void GeoSphere::SetUpTileStyle()
{
	m_terrain->GetLandformStyle(m_tileStyle);
}

bool GeoSphere::TessellationEnabled()
{
	return Pi::config->Int("TerrainTessellation", 0) != 0;
}

double GeoSphere::TessellationPixels()
{
	return std::clamp(double(Pi::config->Float("TerrainTessellationPixels", 20.0f)), 2.0, 64.0);
}

bool GeoSphere::TextureFixedEnabled()
{
	return Pi::config->Int("TerrainTextureFixed", 1) != 0;
}

double GeoSphere::TextureFixedLevel()
{
	return std::clamp(double(Pi::config->Float("TerrainTextureFixedLevel", 3.0f)), 0.0, 8.0);
}

double GeoSphere::TextureFixedRange()
{
	return std::clamp(double(Pi::config->Float("TerrainTextureFixedRange", 50.0f)), 2.0, 500.0);
}

bool GeoSphere::StonesEnabled()
{
	return Pi::config->Int("TerrainStones", 1) != 0;
}

double GeoSphere::StonesHeight()
{
	return std::clamp(double(Pi::config->Float("TerrainStonesHeight", 1.0f)), 0.0, 4.0);
}

bool GeoSphere::StoneShadowsEnabled()
{
	return Pi::config->Int("TerrainStoneShadows", 1) != 0;
}

void GeoSphere::NoteDrawnPatches()
{
	DrawnPatches d;
	d.tessellated = m_tessellatedThisFrame;
	const double radius = GetSystemBody()->GetRadius();
	double nearest = 1e300;
	for (const std::pair<double, GeoPatch *> &p : m_visiblePatches) {
		const int depth = p.second->GetDepth();
		if (depth > d.deepest || d.count == 0) {
			d.deepest = depth;
			d.finestSpacing = p.second->GetVertexSpacing() * radius;
		}
		if (p.first < nearest) {
			nearest = p.first;
			d.nearestDepth = depth;
			d.nearestSpacing = p.second->GetVertexSpacing() * radius;
		}
		d.count++;
	}
	m_drawnPatches = d;
}

Sint32 GeoSphere::GetMeantDepth() const
{
	const double circumference = 2.0 * M_PI * m_sbody->GetRadius();
	// calculate length of each edge segment (quad) times 4 due to that being the number around the sphere (1 per side, 4 sides for Root).
	double edgeMetres = circumference / double(s_patchContext->GetEdgeLen() * 8);
	// find out what depth we reach the desired resolution
	Sint32 depth = 0;
	while (edgeMetres > gs_targetPatchTriLength && depth < GEOPATCH_MAX_DEPTH) {
		edgeMetres *= 0.5;
		++depth;
	}
	return depth;
}

// The deepest level: twice the level meant for triangles of about 100 m, at most GEOPATCH_MAX_DEPTH. Until 2026-11-08
// the level was worked out twice without starting again from 0, which came to the same (and to four times the level,
// at most the cap, after a change of the planet detail setting, on the bodies below it); the ground's look was built on
// it (vertices 2-12 m apart near the ground on most bodies, 4.4 m on Mars), so it stays, now on purpose (P1 and Q5 in
// docs/implementation_plans/opengl46-tessellation-compute_plan.md). Bigger bodies stop at the cap, so their ground is
// coarser; only the smallest moons are far coarser (Phobos about 30 m).
void GeoSphere::CalculateMaxPatchDepth()
{
	m_maxDepth = std::min(2 * GetMeantDepth(), Sint32(GEOPATCH_MAX_DEPTH));
}

void GeoSphere::Update()
{
	// On foot or in the rover the ground near the player gets finer, down to about 25 m between vertices at most, in a
	// small radius (GeoPatch::LODUpdate). Heights are at full detail on these levels already. Only the smallest moons
	// have deepest patches coarser than that.
	m_extraDepth = 0;
	if (m_terrain->HasFixedDetail() && Pi::game && Pi::game->GetExcursion() && Pi::game->GetExcursion()->IsOffShip()) {
		for (double spacing = GetDeepestVertexSpacing(); spacing > 25.0 && m_extraDepth < 3; spacing *= 0.5)
			m_extraDepth++;
	}

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
			{
				FrontierLog::Timed timed(FrontierLog::PART_RESULTS);
				ProcessSplitResults();
			}
			FrontierLog::Timed timed(FrontierLog::PART_LOD);
			m_lodDistance = LodDistanceAtAltitude(m_tempCampos.Length() - 1.0, GetOrbitSurface() == ORBIT_TILES_FULL_DETAIL);
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

// the light on slopes from afar and the surface tiles' filter (FarTexture in terrain-far.glsl)
void GeoSphere::SetFarLook()
{
	m_surfaceMaterial->SetPushConstant("FarTexture"_hash, vector3f(float(m_terrain->GetFarReliefSlopes()), 0.0f, 0.0f),
		Pi::config->Int("TerrainTileBicubic", 1) != 0 ? 1.0f : 0.0f);
}

// The patch surface tiles (TerrainSurfaceTiles) for the visible patches,
// twice as fine as their vertices; each patch sets its own in RenderImmediate.
void GeoSphere::SetSurfaceTiles(const double pixelAngle)
{
	TerrainSurfaceTiles *tiles = nullptr;
	if (TerrainSurfaceTiles::Enabled() && GetSystemBody()->GetSuperType() != SystemBody::SUPERTYPE_STAR)
		tiles = TerrainSurfaceTiles::Get(2 * (s_patchContext->GetEdgeLen() - 3)); // twice the vertex gaps
	if (tiles) {
		tiles->Update(this, m_visiblePatches, pixelAngle);
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
	m_pixelAngle = pixelAngle;
	m_tessellatedThisFrame = 0;
	Graphics::Frustum frustum(modv, proj);
	m_tempFrustum = frustum;

	// no frustum test of entire geosphere, since Space::Render does this
	// for each body using its GetBoundingRadius() value

	//First draw - create materials (they do not change afterwards)
	if (!m_surfaceMaterial)
		SetUpMaterials();

	// The ground's program is built by the driver in the background (SetUpMaterials): building it took 3 to 4 s
	// after every change to its shaders, the game standing still all that time (2026-11-09). Within a radius of the
	// ground it's waited for, as there'd be nothing to stand on; further out the ground isn't drawn until it's ready
	// (its air still is), so a planet seen for the first time shows up a few seconds late. Near, the version with the
	// stones is started alongside, and the ground drawn without them until it's ready (Material::SetTessellated).
	const bool nearGround = campos.Length() < 2.0;
	if (nearGround && !m_stonesVersionStarted && (StonesActive() || TessellationEnabled())) {
		m_stonesVersionStarted = true;
		m_surfaceMaterial->SetTessellated(true);
		m_surfaceMaterial->SetTessellated(false);
	}
	bool groundReady = renderer->MaterialCompiled(m_surfaceMaterial.Get());
	if (!groundReady && nearGround)
		groundReady = renderer->WaitForMaterial(m_surfaceMaterial.Get());

	//Update material parameters
	SetMaterialParameters(trans, radius, shadows, m_atmosphereParameters);
	// P4a: how finely the tessellation control shader cuts each edge (x: pixels per radian, y: the edge on screen to
	// aim for in pixels, z: the most cuts per edge)
	m_surfaceMaterial->SetPushConstant("TessParams"_hash, vector3f(float(1.0 / std::max(pixelAngle, 1e-9)), float(TessellationPixels()), 64.0f), float(StonesFullPixels()));
	// P4b: the ground textures at one size near the camera (terrain-materials.glsl, materialMapping)
	m_surfaceMaterial->SetPushConstant("MaterialNear"_hash, vector3f(float(TextureFixedLevel()), float(TextureFixedRange()), TextureFixedEnabled() ? 1.0f : 0.0f), float(StonesFadePixels()));
	// P4d: the stones read their own copy of the textures' heights (TerrainStones), loaded the first time they're on
	if (StonesActive())
		m_surfaceMaterial->SetTexture("terrainStoneHeights"_hash, TerrainStones::GetTexture(renderer));
	// the debug window's ground cost breakdown
	m_surfaceMaterial->SetPushConstant("GroundSkip"_hash, s_groundSkip);
	// each slot's material, worked out once (FillMaterialSlotData); a buffer set each frame
	m_surfaceMaterial->SetBufferDynamic("MaterialSlotData"_hash, &m_materialSlotData);
	// the cheaper photo textures, as switched
	m_surfaceMaterial->SetPushConstant("TextureLod"_hash, vector3f(TextureSimpleFar() ? 1.0f : 0.0f, float(TextureSimpleFrom()), TextureOneScale() ? 1.0f : 0.0f), float(TextureScaleBand()));
	m_surfaceMaterial->SetPushConstant("TextureTurnTable"_hash, TurnTable() ? 1.0f : 0.0f);
	// P4c: the stones raised near the camera (terrain-materials.glsl, terrainStones)
	m_surfaceMaterial->SetPushConstant("MaterialStones"_hash, vector3f(StonesActive() ? 1.0f : 0.0f, float(StonesHeight()), StoneShadowsEnabled() ? 1.0f : 0.0f), DeepestStone(m_terrain.Get()));
	// the mesh's own vertex spacing, which the stones are smoothed to where it's cut as finely as it can be
	{
		double finest, perMetre;
		StoneMeshSpacing(finest, perMetre);
		m_surfaceMaterial->SetPushConstant("StoneMesh"_hash, vector3f(float(finest), float(perMetre), 0.0f), 0.0f);
	}
	// and the patches kept in view as far below the screen as the tallest stone could raise them (GeoPatch::IsPatchVisible)
	// (none with the debug window's comparison switch 21: as before 2026-11-09)
	m_stoneCullMargin = StonesActive() && !(s_groundSkip & (1 << 21)) ? DeepestStone(m_terrain.Get()) * StonesHeight() * std::exp2(TextureFixedLevel()) / GetSystemBody()->GetRadius() : 0.0;
	// and the viewport, for the edges' length on screen (terrain-tess-edges.glsl)
	m_surfaceMaterial->SetPushConstant("TessScreen"_hash, vector3f(0.5f * float(renderer->GetViewport().w), 0.5f * float(renderer->GetViewport().h), 0.0f), 0.0f);
	Graphics::CityGlowDataBlock cityGlow{};
	if (GetSystemBody()->GetSuperType() != SystemBody::SUPERTYPE_STAR)
		SetCityGlowData(campos, cityGlow);
	// P4: no stones in the cities or under landed ships
	UpdateStoneFlats(campos);
	SetStoneFlats(m_surfaceMaterial.Get(), trans);

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
		// (the old reckoning of their spacing, which the relief was tuned to: GetLegacyDeepestSpacing).
		const double vertexGaps = double(s_patchContext->GetEdgeLen() - 3); // edge length includes the skirt
		const double deepestSpacing = GetLegacyDeepestSpacing();
		const double spacingPerMetre = 1.0 / (1.41 * GetLodDistance() * vertexGaps);
		m_surfaceMaterial->SetPushConstant("ReliefScale"_hash,
			vector3f(float(spacingPerMetre), float(deepestSpacing / double(1 << m_extraDepth)), 0.0f), 0.0f);

		SetFarLook();
	}

	// cloud data: shadows in the terrain shader now, the clouds themselves in
	// the screen-space cloud pass after all bodies are drawn (PlanetClouds.h)
	{
		if (!m_cloudHeightMap && PlanetClouds::WantsHeightMap(GetSystemBody(), campos))
			m_cloudHeightMap.reset(new PlanetClouds::CloudHeightMap(m_terrain, GetSystemBody()->GetRadius()));

		PlanetClouds::CloudDataBlock cloudData;
		PlanetClouds::FillCloudData(cloudData, GetSystemBody(), trans, GetMaxFeatureHeight(), m_cloudHeightMap.get());
		PlanetClouds::SetMaterialCloudData(m_surfaceMaterial.Get(), cloudData, m_cloudHeightMap.get());
		// P4d's check, which needs the planet's axes from the cloud data
		RunStoneProbe(renderer, trans, campos, cloudData);
		PlanetClouds::QueueCloudPass(GetSystemBody(), cloudData, vector3f(trans * vector3d(0.0)) / radius,
			m_atmosphereParameters.atmosCol.ToColor4f(), m_cloudHeightMap.get(), cityGlow);
	}

	if (m_atmosphereMaterial.Valid() && m_atmosphereParameters.atmosDensity > 0.0) {
		// P8: with the camera in the air, the sky's light for a grid of directions round it, worked out once this
		// frame with the sky's own sums, for the sky to read instead of working it out at every pixel (from outside the
		// air, its edge round the planet is too sharp for the grid, and the sky works as before)
		const bool skyTable = m_skyTableMaterial && SkyTableEnabled() && campos.Length() < m_atmosphereParameters.atmosRadius;
		if (skyTable) {
			renderer->BeginGpuTimer("sky table");
			renderer->DispatchCompute(m_skyTableMaterial.Get(), 384 / 8, 256 / 8, 1);
			renderer->EndGpuTimer();
			renderer->AddComputeBarrier(Graphics::Renderer::BARRIER_TEXTURE_FETCH);
		}
		m_atmosphereMaterial->SetPushConstant("SkyTable"_hash, vector3f(skyTable ? 1.0f : 0.0f, 0.0f, 0.0f), 0.0f);

		// make atmosphere sphere slightly bigger than required so
		// that the edges of the pixel shader atmosphere jizz doesn't
		// show ugly polygonal angles
		renderer->BeginGpuTimer("atmosphere");
		DrawAtmosphereSurface(renderer, trans, campos,
			m_atmosphereParameters.atmosRadius * 1.02,
			m_atmosphereMaterial);
		renderer->EndGpuTimer();
	}

	// (the air is drawn; the ground once its program is ready, above)
	if (!groundReady)
		return;

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
		NoteDrawnPatches();
		m_visiblePatches.clear();
	} else {
		// gather the visible patches, bake the detail tiles they need, draw them
		for (int i = 0; i < NUM_PATCHES; i++) {
			m_patches[i]->GatherRenderablePatches(m_visiblePatches, renderer, campos, frustum);
		}
		// config TerrainDetailTiles (Terrain Debug window): 0 frees the atlas. Also while the landforms are real
		// shape (Terrain::SetLandformShape), so they aren't drawn twice.
		TerrainTiles *tiles = nullptr;
		if (Pi::config->Int("TerrainDetailTiles", 1) == 0 || Terrain::GetLandformShape() > 0.0)
			TerrainTiles::Destroy();
		else if (GetSystemBody()->GetSuperType() != SystemBody::SUPERTYPE_STAR)
			tiles = TerrainTiles::Get(renderer);
		if (tiles) {
			tiles->Update(renderer, this, m_visiblePatches, pixelAngle);
			m_surfaceMaterial->SetTexture("terrainTiles"_hash, tiles->GetAtlas());
		}
		SetSurfaceTiles(pixelAngle);
		// nearest first, so that the ground's costly shader isn't run for what nearer ground then covers
		// (config TerrainNearestFirst, Terrain Debug window, for A/B comparison)
		if (Pi::config->Int("TerrainNearestFirst", 1) != 0) {
			std::sort(m_visiblePatches.begin(), m_visiblePatches.end(), [](const std::pair<double, GeoPatch *> &a, const std::pair<double, GeoPatch *> &b) {
				return a.first < b.first;
			});
		}
		for (std::pair<double, GeoPatch *> &pPatch : m_visiblePatches) {
			pPatch.second->RenderImmediate(renderer, campos, modelView);
		}
		NoteDrawnPatches();
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
		if ((effect_flags & Terrain::EFFECT_LAVA) && (effect_flags & Terrain::EFFECT_WATER))
			surfDesc.effect = Graphics::EFFECT_GEOSPHERE_TERRAIN_WITH_LAVA_AND_WATER;
		else if (effect_flags & Terrain::EFFECT_LAVA)
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
		// built in the background, and waited for in Render only close to the ground
		Pi::renderer->SetBackgroundCompile(true);
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
		Pi::renderer->SetBackgroundCompile(false);
		m_stonesVersionStarted = false;

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
			"textures/terrain/salt.dds", "textures/terrain/tundra.dds", "textures/terrain/dirty_ice.dds", "textures/terrain/lava_flow.dds",
			"textures/terrain/cracked_mud.dds", "textures/terrain/dust.dds", "textures/terrain/metal.dds", "textures/terrain/forest_far.dds"
		};
		m_texMaterials.Reset(Graphics::TextureBuilder::Array(materialFiles, materialFiles.size()).GetOrCreateTexture(Pi::renderer, "terrain-materials"));
		m_surfaceMaterial->SetTexture("terrainMaterials"_hash, m_texMaterials.Get());
		// relief below the mesh (terrainReliefNormal() in terrain-materials.glsl)
		m_texRelief.Reset(Graphics::TextureBuilder::Normal("textures/terrain/relief.png").GetOrCreateTexture(Pi::renderer, "model"));
		m_surfaceMaterial->SetTexture("terrainRelief"_hash, m_texRelief.Get());
		// the turns' cell hashes, to look up (TerrainTurnTexture)
		m_surfaceMaterial->SetTexture("materialCellHashTex"_hash, CellHashTexture(Pi::renderer));
		SetUpTileStyle();

		// this body's material slots: texture layer per slot (-1 unused), and
		// the slots for bare steep ground, beaches and lava (-1 none)
		const TerrainMaterialTable &table = m_terrain->GetMaterialTable();
		auto layer = [&](int slot) { return slot < table.SlotCount() ? float(table.SlotLayer(slot)) : -1.0f; };
		m_surfaceMaterial->SetPushConstant("MaterialSlots0"_hash, vector3f(layer(0), layer(1), layer(2)), layer(3));
		m_surfaceMaterial->SetPushConstant("MaterialSlots1"_hash, vector3f(layer(4), layer(5), layer(6)), layer(7));
		m_surfaceMaterial->SetPushConstant("MaterialSlots2"_hash, vector3f(layer(8), layer(9), layer(10)), layer(11));
		m_surfaceMaterial->SetPushConstant("MaterialSlots3"_hash, vector3f(layer(12), layer(13), layer(14)), layer(15));
		// x: strength of the ice's cracks; y: detail fixed in space; z: slopes and hollows darker, in the light alone;
		// w: how many slots this body uses
		const bool occlusion = m_terrain->HasFixedDetail() || !GetSystemBody()->GetHeightMapFilename().empty();
		m_surfaceMaterial->SetPushConstant("MaterialLook"_hash, vector3f(m_terrain->GetIceCrackStrength(), m_terrain->HasFixedDetail() ? 1.0f : 0.0f, occlusion ? 1.0f : 0.0f), float(table.SlotCount()));
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
		FillMaterialSlotData();
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
	case 2: {
		m_atmosphereMaterial.Reset(Pi::renderer->CreateMaterial("rayleigh_geosphere_sky_full", skyDesc, rsd, atmosVtxFmt));
		// P8: its table round the camera (as large as sky-table.glsl's SKY_TABLE_WIDTH and _HEIGHT), and the compute
		// pass that fills it, with the sky's own lights and eclipses
		const Graphics::TextureDescriptor tableDesc(Graphics::TEXTURE_RGBA16F, vector3f(384.0f, 256.0f, 0.0f), Graphics::LINEAR_REPEAT, false, false, false, 0, Graphics::TEXTURE_2D);
		m_skyTable.Reset(Pi::renderer->CreateTexture(tableDesc));
		m_skyTableMaterial.Reset(Pi::renderer->CreateMaterial("sky_table", skyDesc, Graphics::RenderStateDesc(), Graphics::VertexFormatDesc()));
		m_skyTableMaterial->SetTexture("skyTableOut"_hash, m_skyTable.Get());
		m_atmosphereMaterial->SetTexture("skyTable"_hash, m_skyTable.Get());
		break;
	}
	default:
		m_atmosphereMaterial.Reset(Pi::renderer->CreateMaterial("geosphere_sky", skyDesc, rsd, atmosVtxFmt));
		break;
	}
}
