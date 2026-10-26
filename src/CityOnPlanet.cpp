// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "CityOnPlanet.h"

#include "Camera.h"
#include "CityLayout.h"
#include "FileSystem.h"
#include "Frame.h"
#include "Game.h"
#include "JsonUtils.h"
#include "MathUtil.h"
#include "ModelCache.h"
#include "Pi.h"
#include "Planet.h"
#include "Player.h"
#include "Ship.h"
#include "Space.h"
#include "SpaceStation.h"

#include "collider/Geom.h"
#include "galaxy/StarSystem.h"
#include "collider/GeomTree.h"
#include "core/Log.h"
#include "core/StringHash.h"

#include "graphics/Frustum.h"
#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/RenderState.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "graphics/Types.h"
#include "graphics/VertexArray.h"
#include "graphics/VertexBuffer.h"
#include "lua/LuaEvent.h"
#include "scenegraph/Animation.h"
#include "scenegraph/CollisionGeometry.h"
#include "scenegraph/Model.h"
#include "scenegraph/ModelSkin.h"
#include "sound/Sound.h"

#include "matrix3x3.h"
#include "matrix4x4.h"

#include "utils.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <tuple>

// Night lights (city-lights.glsl). Brightness is the light reaching a
// surface square on at 1 m, before the inverse square falloff.
// street lamps; cells without a ground height (no building) for them
static constexpr float NO_CELL_HEIGHT = -1e30f;
static constexpr float LAMP_HEIGHT = 8.0f;
static constexpr float LAMP_RADIUS = 40.0f;
static constexpr float LAMP_BRIGHTNESS = 80.0f;
// along the streets, and both sides of the main roads (m)
static constexpr double LAMP_SPACING = 38.0;
static constexpr double MAIN_LAMP_SPACING = 32.0;
// lights at the foot of the buildings' facades
static constexpr float FACADE_HEIGHT = 4.0f;
static constexpr float FACADE_OFFSET = 4.0f;
// floodlights around the spaceport
static constexpr float FLOOD_HEIGHT = 18.0f;
static constexpr float FLOOD_RADIUS = 90.0f;
static constexpr float FLOOD_BRIGHTNESS = 400.0f;
// floodlights over each landing pad: four around its centre, this far out
// along each axis and this high above it (m)
static constexpr float PAD_LIGHT_OFFSET = 35.0f;
static constexpr float PAD_LIGHT_HEIGHT = 30.0f;
static constexpr float PAD_LIGHT_RADIUS = 110.0f;
static constexpr float PAD_LIGHT_BRIGHTNESS = 700.0f;
static constexpr float FLOOD_SPACING = 60.0f;
// the light map's texels are at least this size (m), and at most this many across
static constexpr float LIGHT_MAP_TEXEL = 16.0f;
static constexpr uint32_t LIGHT_MAP_MAX = 1024;
// the light grid: cell size (m), lights per cell and texture width (as in city-lights.glsl)
static constexpr float LIGHT_GRID_CELL = 40.0f;
static constexpr uint32_t LIGHT_CELL_MAX = 16;
static constexpr uint32_t LIGHT_ATLAS_WIDTH = 1024;
// indices are stored as floats, exact below 2^24
static constexpr uint32_t LIGHT_MAX_INDICES = 1 << 24;
// lights are evaluated one by one out to this distance from the camera (m),
// and light what is within this height of the station (m)
static constexpr float LIGHT_NEAR_DISTANCE = 2500.0f;
static constexpr float LIGHT_HEIGHT_RANGE = 3000.0f;
// the city's own lights take over from its glow from afar between these camera distances (m)
static constexpr double LIGHTS_FADE_FAR = 90000.0;
static constexpr double LIGHTS_FADE_NEAR = 60000.0;

// share of a light of radius r reaching distance^2 d2: inverse square, smoothly
// cut off at the radius (cityLightFalloff() in city-lights.glsl)
static inline float LightFalloff(float d2, float r)
{
	const float x = d2 / (r * r);
	const float w = Clamp(1.0f - x * x, 0.0f, 1.0f);
	return w * w / (d2 + 1.0f);
}

static inline float Luminance(const vector3f &c)
{
	return 0.3f * c.x + 0.59f * c.y + 0.11f * c.z;
}

// Forcefield (forcefield.glsl). Its rim is sampled for the lowest ground under
// it, and its skirt reaches this far lower (m)
static constexpr int FORCEFIELD_RIM_SAMPLES = 48;
static constexpr double FORCEFIELD_SKIRT = 40.0;
// grown in steps until every building is this far inside it (m)
static constexpr double FORCEFIELD_CLEARANCE = 20.0;
static constexpr double FORCEFIELD_FIT_GROWTH = 1.02;
static constexpr int FORCEFIELD_FIT_STEPS = 60;
// struts: tubes this share of the sphere's radius thick (times the flavour's
// strutWidth), within these bounds (m), cut in segments at most this long (m)
// and at most this many, so the ripples can bend them; joints this much
// thicker; never thinner than this many pixels across (at 1080 lines)
static constexpr double FORCEFIELD_STRUT_RADIUS = 0.0012;
static constexpr double FORCEFIELD_STRUT_RADIUS_MIN = 2.0;
static constexpr double FORCEFIELD_STRUT_RADIUS_MAX = 12.0;
static constexpr double FORCEFIELD_STRUT_SEGMENT = 20.0;
static constexpr int FORCEFIELD_STRUT_MAX_SEGMENTS = 48;
static constexpr int FORCEFIELD_STRUT_SIDES = 6;
static constexpr float FORCEFIELD_JOINT_SCALE = 1.8f;
static constexpr float FORCEFIELD_STRUT_MIN_PX = 1.4f;
static constexpr float FORCEFIELD_FILL = 0.012f;
// the far side seen from outside, dimmer so the near struts read
static constexpr float FORCEFIELD_BACK_FACES = 0.7f;
// faded out before cities stop being drawn (SpaceStation.cpp), camera distance (m)
static constexpr double FORCEFIELD_FADE_NEAR = 60000.0;
static constexpr double FORCEFIELD_FADE_FAR = 95000.0;
// openings round crossing ships: the ship's clip radius times this, within
// these bounds (m), open this long (s) before it reaches the field
static constexpr size_t FORCEFIELD_MAX_OPENINGS = 8;
static constexpr double FORCEFIELD_OPENING_SCALE = 2.5;
static constexpr double FORCEFIELD_OPENING_MIN = 30.0;
static constexpr double FORCEFIELD_OPENING_MAX = 400.0;
static constexpr double FORCEFIELD_OPENING_LEAD = 0.5;
static constexpr double FORCEFIELD_OPENING_RANGE = 3000.0; // looked for this far outside it (m)
// the player may come back in without clearance this long after going out (s);
// otherwise kept this many of its clip radii outside, and bounced off like this
static constexpr double FORCEFIELD_RETURN_TIME = 10.0;
static constexpr double FORCEFIELD_SHIP_SHELL = 1.0;
static constexpr double FORCEFIELD_BOUNCE = 0.35;
static constexpr double FORCEFIELD_IMPACT_SPEED = 2.0; // m/s, slower makes no wave
static constexpr float FORCEFIELD_OPEN_TIME = 0.3f;
static constexpr float FORCEFIELD_CLOSE_TIME = 1.0f;
// ripples where it stops a ship: reach the ship's clip radius times this,
// within these bounds (m), over this long (s, as its sound); no new ones this
// soon within the reach of the last
static constexpr size_t FORCEFIELD_MAX_IMPACTS = 4;
static constexpr double FORCEFIELD_IMPACT_SCALE = 12.0;
static constexpr double FORCEFIELD_IMPACT_MIN = 300.0;
static constexpr double FORCEFIELD_IMPACT_MAX = 1200.0;
static constexpr float FORCEFIELD_IMPACT_TIME = 2.0f;
static constexpr float FORCEFIELD_IMPACT_REPEAT = 0.4f;

// ground this little above the planet's radius (m) is sea, on worlds with one
static constexpr double SEA_LEVEL_MARGIN = 0.5;
// room a building keeps from every road and its pavement (m)
static constexpr double BUILDING_CLEARANCE = 0.5;
// and from the sides of its plot without a street (m)
static constexpr double PLOT_MARGIN = 2.0;

namespace {
	// as ForcefieldData in forcefield.glsl
	struct ForcefieldDataBlock {
		vector3f sphereCentre;
		float sphereRadius;
		struct {
			vector3f centre;
			float radius;
		} openings[FORCEFIELD_MAX_OPENINGS];
		struct {
			vector3f point;
			float age;
		} impacts[FORCEFIELD_MAX_IMPACTS];
		float impactReach[FORCEFIELD_MAX_IMPACTS];
		float strutRadius, strutRadiusPerMetre, unused0, unused1; // m, at least this per metre away
		vector3f colour;
		float strutOpacity;
		float fill, backFaces, fade, unused2;
	};
	static_assert(sizeof(ForcefieldDataBlock) == 17 * 16, "");

	static const size_t s_forcefieldDataName = "ForcefieldData"_hash;
} // namespace

std::vector<CityOnPlanet::CityFlavourType> CityOnPlanet::s_cityFlavours;
std::unique_ptr<Graphics::Material> CityOnPlanet::s_debugMat;
std::vector<CityOnPlanet::QueuedForcefield> CityOnPlanet::s_forcefieldQueue;

void CityOnPlanet::AddStaticGeomsToCollisionSpace()
{
	PROFILE_SCOPED()

	size_t numBuildingTypes = m_cityType->buildingTypes.size();

	// reset data structures
	m_enabledBuildings.clear();
	m_buildingCounts.resize(numBuildingTypes);
	for (Uint32 i = 0; i < numBuildingTypes; i++) {
		m_buildingCounts[i] = 0;
	}

	// Generate the new building list
	int skipMask;
	switch (Pi::detail.cities) {
	case 0: skipMask = 0xf; break;
	case 1: skipMask = 0x7; break;
	case 2: skipMask = 0x3; break;
	case 3: skipMask = 0x1; break;
	default:
		skipMask = 0;
		break;
	}
	Uint32 numVisibleBuildings = 0;
	for (unsigned int i = 0; i < m_buildings.size(); i++) {
		if (!(i & skipMask)) {
			++numVisibleBuildings;
		}
	}

	// we know how many building we'll be adding, reserve space up front
	m_enabledBuildings.reserve(numVisibleBuildings);
	for (unsigned int i = 0; i < m_buildings.size(); i++) {
		if (i & skipMask) {
		} else {
			Frame *f = Frame::GetFrame(m_frame);
			f->AddStaticGeom(m_buildings[i].geom);
			m_enabledBuildings.push_back(m_buildings[i]);
			// Update building types
			++(m_buildingCounts[m_buildings[i].instIndex]);
		}
	}

	// reset the reset flag
	m_detailLevel = Pi::detail.cities;
}

void CityOnPlanet::RemoveStaticGeomsFromCollisionSpace()
{
	m_enabledBuildings.clear();
	for (unsigned int i = 0; i < m_buildings.size(); i++) {
		Frame *f = Frame::GetFrame(m_frame);
		f->RemoveStaticGeom(m_buildings[i].geom);
	}
}

void CityOnPlanet::PrecalcInstanceTransforms(const matrix4x4d &stationOrient)
{
	PROFILE_SCOPED();

	m_instanceTransforms.clear();

	const matrix3x3f orient(stationOrient.GetOrient());

	// Cache building instance transforms
	// TODO: this could easily be represented as a 4x3 matrix or dual quaternion for significantly less bandwidth
	// That requires an "instance buffer" that isn't a 4x4 matrix per instance though... :(
	for (const auto &building : m_enabledBuildings) {
		m_instanceTransforms.emplace_back(orient * matrix3x3f::RotateY(building.yaw), building.pos);
	}
}

void CityOnPlanet::GetModelSize(const Aabb &aabb, uint8_t size[2])
{
	vector3d aabbSize = aabb.max - aabb.min;

	size[0] = std::ceil(aabbSize.x / double(CELLSIZE));
	size[1] = std::ceil(aabbSize.z / double(CELLSIZE));
}

void CityOnPlanet::LoadBuildingType(std::string_view key, const Json &buildingDef, BuildingType &out)
{
	std::string modelPath = buildingDef.value("model", "");

	if (modelPath.empty()) {
		modelPath = key;
	}

	out.model = Pi::FindModel(modelPath);

	if (!out.model->GetCollisionMesh().Get()) {
		out.model->CreateCollisionMesh();
	}

	const Aabb aabb = out.model->GetCollisionMesh()->GetAabb();
	GetModelSize(aabb, out.cellSize);
	out.minX = float(aabb.min.x);
	out.maxX = float(aabb.max.x);
	out.minZ = float(aabb.min.z);
	out.maxZ = float(aabb.max.z);
	out.height = float(aabb.max.y);

	out.cellSize[0] = buildingDef.value("size-x", out.cellSize[0]);
	out.cellSize[1] = buildingDef.value("size-y", out.cellSize[1]);

	if (out.cellSize[0] > CELLMAX || out.cellSize[1] > CELLMAX) {
		Log::Warning("\tCity building {} has {}x{}c footprint (at {} m/c), will be truncated to {}x{}c.",
			key, out.cellSize[0], out.cellSize[1], CELLSIZE, CELLMAX, CELLMAX);

		out.cellSize[0] = std::min(out.cellSize[0], uint8_t(CELLMAX));
		out.cellSize[1] = std::min(out.cellSize[1], uint8_t(CELLMAX));
	}

	out.rarityAirless = buildingDef.value("airless-rarity", 1.0);
	// configs use "atmospheric-rarity"; "atmo-rarity" is accepted for compatibility
	out.rarityAtmo = buildingDef.value("atmospheric-rarity", buildingDef.value("atmo-rarity", 1.0));

	std::string kind = buildingDef.value("kind", "");

	out.buildingKind = SectorKind::None;
	if (kind == "storage")
		out.buildingKind = SectorKind::Storage;
	else if (kind == "industry")
		out.buildingKind = SectorKind::Industry;
	else if (kind == "monument")
		out.buildingKind = SectorKind::Monument;
	else if (kind == "habitat")
		out.buildingKind = SectorKind::Habitat;
	else if (kind == "frontier")
		out.buildingKind = SectorKind::Frontier;

	// night lights at the foot of the facades: by kind, unless set by
	// "light-colour": [r, g, b] (0..1) and "light-brightness" (0 for none)
	switch (out.buildingKind) {
	case SectorKind::Industry:
		out.lightColour = vector3f(1.0f, 0.62f, 0.3f);
		out.lightIntensity = 48.0f;
		break;
	case SectorKind::Storage:
		out.lightColour = vector3f(0.82f, 0.9f, 1.0f);
		out.lightIntensity = 20.0f;
		break;
	case SectorKind::Monument:
		out.lightColour = vector3f(0.95f, 0.95f, 1.0f);
		out.lightIntensity = 70.0f;
		break;
	case SectorKind::Habitat:
		out.lightColour = vector3f(1.0f, 0.82f, 0.6f);
		out.lightIntensity = 35.0f;
		break;
	default:
		out.lightColour = vector3f(1.0f, 0.78f, 0.55f);
		out.lightIntensity = 25.0f;
		break;
	}
	const auto lightColour = buildingDef.find("light-colour");
	if (lightColour != buildingDef.end() && lightColour->is_array() && lightColour->size() == 3)
		out.lightColour = vector3f((*lightColour)[0].get<float>(), (*lightColour)[1].get<float>(), (*lightColour)[2].get<float>());
	out.lightIntensity = buildingDef.value("light-brightness", out.lightIntensity);

	std::string idleName = buildingDef.value("idle", "");
	if (!idleName.empty()) {
		out.idleAnimation = out.model->FindAnimation(idleName);

		if (out.idleAnimation == nullptr) {
			Log::Warning("\tIdle animation {} specified for building {} not found!", idleName, key);
		}
	} else {
		out.idleAnimation = out.model->FindAnimation("idle");
	}

	Log::Verbose("\tLoaded city building {} ({}x{}c, atm: {}, arl: {}, kind: {}, idleAnim: {})",
		key, out.cellSize[0], out.cellSize[1], out.rarityAtmo, out.rarityAirless, int(out.buildingKind), out.idleAnimation != nullptr);
}

void CityOnPlanet::LoadCityFlavour(const FileSystem::FileInfo &file)
{
	Json fileData = JsonUtils::LoadJsonDataFile(file.GetPath(), true);
	if (!fileData.is_object()) {
		Log::Info("CityOnPlanet: Could not load city definition file '{}' as a valid JSON file.", file.GetPath());
		return;
	}

	Log::Info("Loading city definition file '{}'", file.GetPath());

	const Json &sizeDef = fileData["size"];
	if (!sizeDef.is_array()) {
		Log::Warning("\tMissing city size definition array! Should be \"size\": [ ... ]");
		return;
	}

	const Json &buildingDefs = fileData["buildings"];
	if (!buildingDefs.is_object()) {
		Log::Warning("\tMissing city buildings object! Should be \"buildings\": { ... }");
		return;
	}

	CityFlavourType cityType = {};

	uint32_t numSizeDefs = 0;
	for (const Json &item : sizeDef) {
		CityRadiusDef radius = {};
		numSizeDefs++;

		radius.population = item.value("population", 0.0);
		radius.baseSize = item.value("baseSize", 0.0);
		radius.atmoSize = item.value("atmoSize", 0.0);
		radius.randomSize = item.value("randomSize", 0.0);
		radius.density = item.value("density", 1.0);

		if (!cityType.sizeDefs.empty() && radius.population > cityType.sizeDefs.back().population) {
			Log::Warning("\tSize definition {} has higher population threshold than previous ({}b vs. {}b).\n" \
				"Size defs should be in descending order.",
				numSizeDefs, radius.population, cityType.sizeDefs.back().population);
		}

		cityType.sizeDefs.emplace_back(std::move(radius));
	}

	std::sort(cityType.sizeDefs.begin(), cityType.sizeDefs.end(), [](auto &a, auto &b) -> bool {
		return a.population > b.population;
	});

	Log::Verbose("\tLoaded {} city size definitions", numSizeDefs);

	uint32_t numBuildingDefs = 0;

	for (const auto &iter : buildingDefs.items()) {
		BuildingType building = {};
		numBuildingDefs++;

		try {
			LoadBuildingType(iter.key(), iter.value(), building);
		} catch (std::exception &e) {
			Log::Warning("Couldn't parse building def {}; error: {}", numBuildingDefs, e.what());
			continue;
		}

		cityType.buildingTypes.emplace_back(std::move(building));
	}

	Log::Verbose("\tLoaded {} building definitions", numBuildingDefs);

	// optional, defaults as ForcefieldDef
	const Json &ffDef = fileData["forcefield"];
	if (ffDef.is_object()) {
		ForcefieldDef &ff = cityType.forcefield;
		ff.minOxidizing = ffDef.value("minOxidizing", ff.minOxidizing);
		ff.minDensity = ffDef.value("minDensity", ff.minDensity);
		ff.maxDensity = ffDef.value("maxDensity", ff.maxDensity);
		ff.minTemperature = ffDef.value("minTemperature", ff.minTemperature);
		ff.maxTemperature = ffDef.value("maxTemperature", ff.maxTemperature);
		ff.radiusScale = ffDef.value("radiusScale", ff.radiusScale);
		ff.heightRatio = std::max(0.05f, ffDef.value("heightRatio", ff.heightRatio));
		ff.frequency = std::max(1, ffDef.value("frequency", ff.frequency));
		const auto colour = ffDef.find("colour");
		if (colour != ffDef.end() && colour->is_array() && colour->size() == 3)
			ff.colour = vector3f((*colour)[0].get<float>(), (*colour)[1].get<float>(), (*colour)[2].get<float>());
		ff.opacity = Clamp(ffDef.value("opacity", ff.opacity), 0.0f, 1.0f);
		ff.strutWidth = std::max(0.1f, ffDef.value("strutWidth", ff.strutWidth));
		ff.lethalSpeed = std::max(0.0f, ffDef.value("lethalSpeed", ff.lethalSpeed));
	}

	s_cityFlavours.emplace_back(std::move(cityType));

	Log::Info("\tCreated city definition flavour #{}", s_cityFlavours.size());
}

void CityOnPlanet::Init()
{
	PROFILE_SCOPED()

	// Load all city definition configs
	FileSystem::FileEnumerator iter(FileSystem::gameDataFiles, "configs/buildings/");
	for (; !iter.Finished(); iter.Next()) {
		const FileSystem::FileInfo &file = iter.Current();
		std::string name = file.GetName();

		if (!ends_with_ci(name, ".json")) {
			continue;
		}

		try {
			LoadCityFlavour(file);
		} catch (std::exception &e) {
			Log::Warning("CityOnPlanet: failed to parse city definition '{}': {}", name, e.what());
		}
	}

	if (s_cityFlavours.empty()) {
		Log::Error("CityOnPlanet: failed to load any city definitions, no cities will be generated!");
	}

	Graphics::RenderStateDesc rsd = {};
	rsd.depthTest = false;
	rsd.depthWrite = false;
	rsd.primitiveType = Graphics::LINE_SINGLE;

	auto fmt = Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_DIFFUSE);
	s_debugMat.reset(Pi::renderer->CreateMaterial("vtxColor", Graphics::MaterialDescriptor(), rsd, fmt));
}

void CityOnPlanet::Uninit()
{
	s_cityFlavours.clear();
	s_debugMat.reset();
}

// FIXME: this is a horrible idea as it directly mutates models in the ModelCache
// and forces all instances of a building to have the exact same color.
// Color data should be supplied via an instance buffer and managed as part of a building instance.
//static
void CityOnPlanet::SetCityModelPatterns(const SystemPath &path)
{
	PROFILE_SCOPED()
	// FIXME: should use system seed + body seed instead of path index
	Uint32 _init[5] = { path.systemIndex, Uint32(path.sectorX), Uint32(path.sectorY), Uint32(path.sectorZ), UNIVERSE_SEED };
	Random rand(_init, 5);

	SceneGraph::ModelSkin skin;
	for (auto &cityFlavour : s_cityFlavours) {
		for (auto &buildingType : cityFlavour.buildingTypes) {

			SceneGraph::Model *model = buildingType.model;

			if (!model->SupportsPatterns())
				continue;

			skin.SetRandomColors(rand);
			skin.Apply(model);

			model->SetPattern(rand.Int32(model->GetNumPatterns()));
		}
	}
}

CityOnPlanet::~CityOnPlanet()
{
	// frame may be null (already removed from
	for (unsigned int i = 0; i < m_buildings.size(); i++) {
		Frame *f = Frame::GetFrame(m_frame);
		f->RemoveStaticGeom(m_buildings[i].geom);
		delete m_buildings[i].geom;
	}
	for (const auto &field : m_forcefields)
		if (field->geom)
			Frame::GetFrame(m_frame)->RemoveStaticGeom(field->geom.get());
}

CityOnPlanet::CityOnPlanet(Planet *planet, SpaceStation *station, const Uint32 seed)
{
	// beware, these are not used in this function, but are used in subroutines!
	m_body = planet->GetSystemBody();
	m_planet = planet;
	m_frame = planet->GetFrame();

	m_rand.seed(seed);

	if (s_cityFlavours.empty()) {
		return; // no buildings available to generate, we already logged an error about this during startup
	}

	// TODO: allow specifying city flavors based on various parameters (faction, world type, set from custom system def, etc.)
	// (GetCitySummary() must pick the same)
	m_cityType = &s_cityFlavours[m_rand.Int32(s_cityFlavours.size())];

	CalcCityRadius(*m_cityType, m_body, m_rand, m_cityRadius, m_cityDensity);

	m_stationOrient = station->GetOrient();

	// TODO: run in a task thread
	Generate(station);

	AddStaticGeomsToCollisionSpace();

	PrecalcInstanceTransforms(station->GetOrient());

	BuildLighting();

	if (NeedsForcefield(m_body, m_cityType->forcefield))
		BuildForcefields(station);

	BuildGround();
}

//static
bool CityOnPlanet::GetCitySummary(const SystemBody *planet, const SpaceStation *station, double &radius)
{
	if (s_cityFlavours.empty())
		return false;

	// as the constructor and Generate() work it out
	Random rand(station->GetSystemBody()->GetSeed());
	const CityFlavourType &cityType = s_cityFlavours[rand.Int32(s_cityFlavours.size())];
	double density;
	CalcCityRadius(cityType, planet, rand, radius, density);

	uint8_t stationSize[2] = {};
	GetModelSize(station->GetAabb(), stationSize);
	radius += std::max<uint32_t>(stationSize[0] / 2, stationSize[1] / 2) * CELLSIZE;
	return true;
}

//static
double CityOnPlanet::LightsFade(double distance)
{
	const double t = Clamp((LIGHTS_FADE_FAR - distance) / (LIGHTS_FADE_FAR - LIGHTS_FADE_NEAR), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

void CityOnPlanet::Generate(SpaceStation *station)
{
	PROFILE_SCOPED()
	Profiler::Clock _genTimer;
	_genTimer.Start();

	// Retrieve the station parameters
	uint8_t stationSize[2] = {};
	GetModelSize(station->GetAabb(), stationSize);
	const uint32_t stationSizeMax = std::max<uint32_t>(stationSize[0] / 2, stationSize[1] / 2);

	// update the allowed radius to ensure the spaceport isn't larger than the entire city
	m_cityRadius += stationSizeMax * CELLSIZE;

	const vector3d stationOrigin = station->GetPosition();
	const matrix3x3d orient = m_stationOrient;
	const double bodyRadius = m_body->GetRadius();
	const bool hasSea = m_planet->HasSea();

	// city space (the station's plane, its axes, metres) to a direction from the planet's centre
	auto dirOf = [&](double x, double z) {
		return (stationOrigin + orient * vector3d(x, 0.0, z)).Normalized();
	};

	// The plan: its shape, districts, main roads and streets
	// ==========================================

	CityLayout::Facts facts;
	facts.radius = m_cityRadius;
	facts.population = m_body->GetPopulation();
	facts.gravity = m_body->CalcSurfaceGravity() / 9.81;
	facts.water = m_body->GetVolatileLiquid();
	facts.agricultural = m_body->GetAgricultural();
	if (const StarSystem *system = m_body->GetStarSystem()) {
		facts.industrial = system->GetIndustrial().ToDouble();
		facts.humanProx = system->GetHumanProx().ToDouble();
	}
	facts.needsDome = NeedsForcefield(m_body, m_cityType->forcefield);
	facts.domeHeightRatio = m_cityType->forcefield.heightRatio;
	facts.domeClearance = FORCEFIELD_CLEARANCE;
	facts.seed = m_rand.Int32();
	facts.portCells[0] = stationSize[0];
	facts.portCells[1] = stationSize[1];

	auto ground = [&](const std::vector<double> &xz, std::vector<double> &heights, std::vector<uint8_t> &sea, double minFeature) {
		const size_t count = xz.size() / 2;
		std::vector<vector3d> dirs(count);
		for (size_t k = 0; k < count; k++)
			dirs[k] = dirOf(xz[2 * k], xz[2 * k + 1]);
		heights.resize(count);
		m_planet->GetTerrainHeights(dirs.data(), heights.data(), count, minFeature);
		sea.resize(count);
		for (size_t k = 0; k < count; k++) {
			heights[k] -= bodyRadius;
			sea[k] = hasSea && heights[k] < SEA_LEVEL_MARGIN;
		}
	};
	m_layout.reset(new CityLayout(facts, ground));
	CityLayout &layout = *m_layout;

	Log::Verbose("Generating City for spacestation {}", station->GetSystemBody()->GetName());
	Log::Verbose("\tpopulation: {} radius {}, shape: {}, {} domes, {} road segments",
		m_body->GetPopulation(), m_cityRadius, CityLayout::ShapeName(layout.GetShape()), layout.GetDomes().size(), layout.GetSegments().size());

	// The buildings: the city's models on plots of its cells, clear of every road
	// ==========================================

	using District = CityLayout::District;
	const bool hasAtmo = m_body->HasAtmosphere();
	const int n = layout.GetSize();
	std::vector<uint8_t> taken(size_t(n) * n, 0);

	// how likely a type is in a district: its rarity and its kind; tall ones in the heart
	auto weightOf = [&](const BuildingType &type, District district, double heart) {
		double w = hasAtmo ? type.rarityAtmo : type.rarityAirless;
		const SectorKind kind = type.buildingKind;
		switch (district) {
		case District::DOWNTOWN:
			w *= (kind == SectorKind::Monument || kind == SectorKind::Habitat) ? 1.0 : kind == SectorKind::None ? 0.4 : 0.05;
			w *= 1.0 + 2.0 * heart * std::min(type.height / 120.0, 1.5);
			break;
		case District::RESIDENTIAL:
			w *= kind == SectorKind::Habitat ? 1.0 : kind == SectorKind::None ? 0.5 : kind == SectorKind::Frontier ? 0.3 : kind == SectorKind::Monument ? 0.1 : 0.05;
			w /= 1.0 + type.height / 90.0;
			break;
		case District::INDUSTRY:
			w *= kind == SectorKind::Industry ? 1.0 : kind == SectorKind::Storage ? 0.6 : kind == SectorKind::None ? 0.3 : 0.0;
			break;
		case District::LOGISTICS:
			w *= kind == SectorKind::Storage ? 1.0 : kind == SectorKind::Industry ? 0.6 : kind == SectorKind::None ? 0.3 : 0.0;
			break;
		case District::FRINGE:
			w *= kind == SectorKind::Frontier ? 1.0 : kind == SectorKind::Storage ? 0.6 : kind == SectorKind::Industry ? 0.4 : 0.0;
			if (type.height > 60.0f)
				w = 0.0;
			break;
		default:
			w = 0.0;
			break;
		}
		return std::max(w, 0.0);
	};
	// how much of each district is built on
	auto fillOf = [](District district) {
		switch (district) {
		case District::DOWNTOWN: return 0.95;
		case District::RESIDENTIAL: return 0.85;
		case District::INDUSTRY: return 0.75;
		case District::LOGISTICS: return 0.85;
		default: return 0.25;
		}
	};

	struct Placed {
		uint32_t type;
		double x, z, yaw;
	};
	std::vector<Placed> placed;

	// a type on the plot of w x d cells from (i, j): inside the plot less the
	// streets round it, clear of every road, under its dome
	auto tryPlot = [&](uint32_t typeIndex, int i, int j, int w, int d, District district, Placed &out) {
		for (int b = 0; b < d; b++) {
			for (int a = 0; a < w; a++) {
				const int ci = i + a, cj = j + b;
				if (!layout.InGrid(ci, cj) || taken[size_t(cj) * n + ci])
					return false;
				if (district == District::FRINGE ? !layout.IsFringe(ci, cj) : (!layout.IsBuildable(ci, cj) || layout.GetDistrict(ci, cj) != district))
					return false;
				if ((a > 0 && layout.StreetBetween(ci - 1, cj, ci, cj)) || (b > 0 && layout.StreetBetween(ci, cj - 1, ci, cj)))
					return false;
			}
		}
		const double half = 0.5 * CELLSIZE;
		double x0 = layout.CellX(i) - half, x1 = layout.CellX(i + w - 1) + half;
		double z0 = layout.CellZ(j) - half, z1 = layout.CellZ(j + d - 1) + half;
		double inset[4] = { PLOT_MARGIN, PLOT_MARGIN, PLOT_MARGIN, PLOT_MARGIN };
		double back[4];
		for (int b = 0; b < d; b++) {
			layout.Setbacks(i, j + b, back);
			inset[0] = std::max(inset[0], back[0]);
			layout.Setbacks(i + w - 1, j + b, back);
			inset[1] = std::max(inset[1], back[1]);
		}
		for (int a = 0; a < w; a++) {
			layout.Setbacks(i + a, j, back);
			inset[2] = std::max(inset[2], back[2]);
			layout.Setbacks(i + a, j + d - 1, back);
			inset[3] = std::max(inset[3], back[3]);
		}
		x0 += inset[0];
		x1 -= inset[1];
		z0 += inset[2];
		z1 -= inset[3];
		if (x1 <= x0 || z1 <= z0)
			return false;

		const BuildingType &type = m_cityType->buildingTypes[typeIndex];
		// facing the main road beside it, else along the streets, turned at random
		double yaw0 = 0.0, roadDistance = 0.0;
		layout.BesideMainRoad(i, j, yaw0, roadDistance);
		const int first = m_rand.Int32(4);
		for (int r = 0; r < 4; r++) {
			const double yaw = yaw0 + 0.5 * M_PI * ((first + r) % 4);
			const double c = std::cos(yaw), sn = std::sin(yaw);
			// the footprint turned, as matrix3x3::RotateY turns it
			double lo[2] = { 1e9, 1e9 }, hi[2] = { -1e9, -1e9 };
			for (int k = 0; k < 4; k++) {
				const double lx = (k & 1) ? type.maxX : type.minX, lz = (k & 2) ? type.maxZ : type.minZ;
				const double rx = c * lx - sn * lz, rz = sn * lx + c * lz;
				lo[0] = std::min(lo[0], rx);
				hi[0] = std::max(hi[0], rx);
				lo[1] = std::min(lo[1], rz);
				hi[1] = std::max(hi[1], rz);
			}
			if (hi[0] - lo[0] > x1 - x0 || hi[1] - lo[1] > z1 - z0)
				continue;
			// in the middle of the plot
			const double px = 0.5 * (x0 + x1) - 0.5 * (lo[0] + hi[0]);
			const double pz = 0.5 * (z0 + z1) - 0.5 * (lo[1] + hi[1]);
			double pts[2 * 25];
			for (int k = 0; k < 25; k++) {
				const double lx = type.minX + (type.maxX - type.minX) * (k % 5) / 4.0;
				const double lz = type.minZ + (type.maxZ - type.minZ) * (k / 5) / 4.0;
				pts[2 * k] = px + c * lx - sn * lz;
				pts[2 * k + 1] = pz + sn * lx + c * lz;
			}
			if (layout.Clearance(pts, 25) < BUILDING_CLEARANCE || type.height > layout.HeightCap(px, pz))
				continue;
			out = { typeIndex, px, pz, yaw };
			return true;
		}
		return false;
	};

	static const int plots[][2] = { { 1, 1 }, { 2, 1 }, { 1, 2 }, { 2, 2 }, { 3, 2 }, { 2, 3 }, { 3, 3 } };
	std::vector<double> weights(m_cityType->buildingTypes.size());
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			if (taken[size_t(j) * n + i])
				continue;
			const bool fringe = layout.IsFringe(i, j);
			if (!fringe && !layout.IsBuildable(i, j))
				continue;
			const District district = fringe ? District::FRINGE : layout.GetDistrict(i, j);
			if (m_rand.Double() > fillOf(district))
				continue;

			const double heart = std::exp(-std::pow(layout.DistanceToCentre(layout.CellX(i), layout.CellZ(j)) / 1500.0, 2.0));
			double total = 0.0;
			for (size_t t = 0; t < weights.size(); t++)
				total += weights[t] = weightOf(m_cityType->buildingTypes[t], district, heart);
			if (total <= 0.0)
				continue;

			bool done = false;
			for (int pick = 0; pick < 6 && !done; pick++) {
				double roll = m_rand.Double() * total;
				uint32_t t = 0;
				while (t + 1 < weights.size() && roll >= weights[t]) {
					roll -= weights[t];
					t++;
				}
				Placed p;
				for (const auto &plot : plots) {
					if (!tryPlot(t, i, j, plot[0], plot[1], district, p))
						continue;
					const CityLayout::Lot lot = district == District::INDUSTRY ? CityLayout::LOT_YARD :
						district == District::FRINGE								  ? CityLayout::LOT_NONE :
																						CityLayout::LOT_PAVED;
					for (int b = 0; b < plot[1]; b++) {
						for (int a = 0; a < plot[0]; a++) {
							taken[size_t(j + b) * n + i + a] = 1;
							layout.SetLot(i + a, j + b, lot);
						}
					}
					placed.push_back(p);
					done = true;
					break;
				}
			}
			// inside the blocks, away from the streets: car parks
			if (!done && !fringe && district != District::INDUSTRY) {
				double back[4];
				layout.Setbacks(i, j, back);
				if (!back[0] && !back[1] && !back[2] && !back[3] && m_rand.Double() < 0.5)
					layout.SetLot(i, j, CityLayout::LOT_PARKING);
			}
		}
	}

	// on the ground: the lowest of the footprint's corners and middle
	std::vector<vector3d> dirs;
	dirs.reserve(placed.size() * 5);
	for (const Placed &p : placed) {
		const BuildingType &type = m_cityType->buildingTypes[p.type];
		const double c = std::cos(p.yaw), sn = std::sin(p.yaw);
		dirs.push_back(dirOf(p.x, p.z));
		for (int k = 0; k < 4; k++) {
			const double lx = (k & 1) ? type.maxX : type.minX, lz = (k & 2) ? type.maxZ : type.minZ;
			dirs.push_back(dirOf(p.x + c * lx - sn * lz, p.z + sn * lx + c * lz));
		}
	}
	std::vector<double> heights(dirs.size());
	if (!dirs.empty())
		m_planet->GetTerrainHeights(dirs.data(), heights.data(), dirs.size());

	const matrix4x4d stationOrient(station->GetOrient());
	m_buildings.clear();
	m_buildings.reserve(placed.size());
	for (size_t k = 0; k < placed.size(); k++) {
		const Placed &p = placed[k];
		const double height = *std::min_element(heights.begin() + 5 * k, heights.begin() + 5 * k + 5);
		const vector3d pos = dirs[5 * k] * height;
		const CollMesh *cmesh = m_cityType->buildingTypes[p.type].model->GetCollisionMesh().Get();

		// FIXME: geoms need a userdata to tell gameplay code what we actually hit.
		// We don't want to create a separate Body for each instance of the buildings, so we
		// scam the code by pretending we're part of the host planet.
		Geom *geom = new Geom(cmesh->GetGeomTree(), stationOrient * matrix4x4d::RotateYMatrix(p.yaw), pos, GetPlanet());

		// add it to the list of buildings to render
		m_buildings.push_back({ vector3f(pos - stationOrigin), float(cmesh->GetRadius()), p.type, float(p.yaw), geom });
	}

	_genTimer.Stop();

	Log::Verbose("\tCityOnPlanet: generated {} buildings in {}ms", m_buildings.size(), _genTimer.milliseconds());

	_genTimer.SoftReset();

	// Compute the total AABB of this city
	Aabb totalAABB;

	for (const auto &buildingInst : m_buildings) {
		totalAABB.Update(vector3d(buildingInst.pos));
	}

	m_realCentre = totalAABB.min + ((totalAABB.max - totalAABB.min) * 0.5);
	m_clipRadius = totalAABB.GetRadius();

	_genTimer.Stop();

	Log::Verbose("\tCityOnPlanet: generated AABB for city in {}ms", _genTimer.milliseconds());

	GenerateFixedLights(station);
}

// Calculate the radius for this city based on the SystemBody's parameters
//static
void CityOnPlanet::CalcCityRadius(const CityFlavourType &cityType, const SystemBody *body, Random &rand, double &radius, double &density)
{
	double population = body->GetPopulation();
	double atmo = body->GetAtmosOxidizing();

	// (reverse_container() does not take a const container)
	for (auto it = cityType.sizeDefs.rbegin(); it != cityType.sizeDefs.rend(); ++it) {
		const CityRadiusDef &radii = *it;
		bool isLast = (&radii == &cityType.sizeDefs.front());

		if (population > radii.population && !isLast)
			continue;

		radius = radii.baseSize + (atmo * radii.atmoSize) + (rand.Double() * radii.randomSize);
		density = radii.density * 0.8;
		break;
	}
}

void CityOnPlanet::GenerateFixedLights(const SpaceStation *station)
{
	PROFILE_SCOPED()
	m_fixedLights.clear();

	// one kind of street lamp per city: sodium, warm or cool white
	static const vector3f lampColours[] = {
		vector3f(1.0f, 0.6f, 0.28f),
		vector3f(1.0f, 0.84f, 0.62f),
		vector3f(0.86f, 0.92f, 1.0f),
	};
	const vector3f lampColour = lampColours[m_rand.Int32(3)] * LAMP_BRIGHTNESS;

	const vector3d stationOrigin = station->GetPosition();
	const double bodyRadius = m_body->GetRadius();
	// a point of the city's plan on its ground, in city space
	auto onGround = [&](double x, double z) {
		const vector3d dir = (stationOrigin + m_stationOrient * vector3d(x, 0.0, z)).Normalized();
		return vector3f((dir * (bodyRadius + m_layout->GroundAt(x, z)) - stationOrigin) * m_stationOrient);
	};

	// street lamps along the roads: both sides of the main roads, the
	// streets' sides in turn, at the edge of the carriageway
	for (const CityLayout::Segment &road : m_layout->GetSegments()) {
		const double dx = road.x1 - road.x0, dz = road.z1 - road.z0;
		const double len = std::hypot(dx, dz);
		if (len < 1.0)
			continue;
		const bool main = road.type == CityLayout::ROAD_ARTERIAL;
		const double spacing = main ? MAIN_LAMP_SPACING : LAMP_SPACING;
		const double side = 0.5 * road.width + 0.5 * road.walk;
		const double ux = dx / len, uz = dz / len;
		for (double t = std::ceil(road.along / spacing) * spacing - road.along; t < len; t += spacing) {
			const int index = int(std::lround((road.along + t) / spacing));
			for (int k = 0; k < (main ? 2 : 1); k++) {
				const double sgn = main ? (k ? -1.0 : 1.0) : ((index & 1) ? -1.0 : 1.0);
				const double x = road.x0 + ux * t - uz * side * sgn, z = road.z0 + uz * t + ux * side * sgn;
				m_fixedLights.push_back({ onGround(x, z) + vector3f(0.0f, LAMP_HEIGHT, 0.0f), LAMP_RADIUS, lampColour, LAMP_HEIGHT });
			}
		}
	}

	// floodlights around the spaceport, on its ground at the station's origin
	const Aabb &aabb = station->GetAabb();
	const double margin = 10.0;
	const vector3d corners[4] = {
		vector3d(aabb.min.x - margin, 0.0, aabb.min.z - margin),
		vector3d(aabb.max.x + margin, 0.0, aabb.min.z - margin),
		vector3d(aabb.max.x + margin, 0.0, aabb.max.z + margin),
		vector3d(aabb.min.x - margin, 0.0, aabb.max.z + margin),
	};
	const vector3f floodColour = vector3f(0.9f, 0.95f, 1.0f) * FLOOD_BRIGHTNESS;
	for (int side = 0; side < 4; side++) {
		const vector3d &a = corners[side];
		const vector3d &b = corners[(side + 1) % 4];
		const int count = std::max(1, int(std::ceil((b - a).Length() / FLOOD_SPACING)));
		for (int i = 0; i < count; i++) {
			const vector3d p = a + (b - a) * ((i + 0.5) / count);
			m_fixedLights.push_back({ vector3f(p) + vector3f(0.0f, FLOOD_HEIGHT, 0.0f), FLOOD_RADIUS, floodColour, FLOOD_HEIGHT });
		}
	}

	// floodlights over the landing pads, where docked ships stand
	const vector3f padColour = vector3f(0.92f, 0.96f, 1.0f) * PAD_LIGHT_BRIGHTNESS;
	for (int bay = 0; bay < station->GetDockingPortCount(); bay++) {
		const vector3f pad = station->GetStationType()->GetStageTransform(bay, DockStage::DOCKED).GetTranslate();
		for (int i = 0; i < 4; i++) {
			const vector3f offset((i & 1 ? 1.0f : -1.0f) * PAD_LIGHT_OFFSET, PAD_LIGHT_HEIGHT, (i & 2 ? 1.0f : -1.0f) * PAD_LIGHT_OFFSET);
			m_fixedLights.push_back({ pad + offset, PAD_LIGHT_RADIUS, padColour, PAD_LIGHT_HEIGHT });
		}
	}

	Log::Verbose("\tCityOnPlanet: {} street lamps and floodlights", m_fixedLights.size());
}

void CityOnPlanet::BuildLighting()
{
	PROFILE_SCOPED()
	std::vector<Light> lights(m_fixedLights);

	// two facades of each enabled building, front and back as it is turned,
	// lit from their foot
	for (const BuildingInstance &building : m_enabledBuildings) {
		const BuildingType &type = m_cityType->buildingTypes[building.instIndex];
		if (type.lightIntensity <= 0.0f)
			continue;

		// building positions are relative to the station in its frame's axes
		const vector3f centre(vector3d(building.pos) * m_stationOrient);
		const float hx = 0.5f * (type.maxX - type.minX);
		const float hz = 0.5f * (type.maxZ - type.minZ);
		// its front and back, as it is turned
		const vector3f offset = matrix3x3f::RotateY(building.yaw) * vector3f(0.0f, 0.0f, hz + FACADE_OFFSET);
		const float length = 2.0f * hx;
		const float brightness = type.lightIntensity * Clamp(length / 50.0f, 0.6f, 2.0f);
		const float radius = Clamp(25.0f + 0.3f * length, 25.0f, 60.0f);
		const vector3f foot = centre + vector3f(0.0f, FACADE_HEIGHT, 0.0f);
		lights.push_back({ foot + offset, radius, type.lightColour * brightness, FACADE_HEIGHT });
		lights.push_back({ foot - offset, radius, type.lightColour * brightness, FACADE_HEIGHT });
	}

	m_retiredLightMap = m_lightMap;
	m_retiredLightGrid = m_lightGrid;
	m_lightMap.Reset();
	m_lightGrid.Reset();
	if (lights.empty())
		return;

	// the map and the grid cover a square around the station holding all the light
	float half = 0.0f;
	for (const Light &light : lights)
		half = std::max(half, std::max(std::abs(light.pos.x), std::abs(light.pos.z)) + light.radius);
	m_lightRect[0] = -half;
	m_lightRect[1] = -half;
	m_lightRect[2] = 1.0f / (2.0f * half);
	m_lightRect[3] = LIGHT_GRID_CELL;

	BakeLightMap(lights);
	BuildLightGrid(lights);

	Log::Verbose("\tCityOnPlanet: {} night lights over {}m", lights.size(), 2.0f * half);
}

// The light reaching level ground from all the lights, per texel, and the
// ground's height weighted by it (city-lights.glsl reads the height back).
void CityOnPlanet::BakeLightMap(const std::vector<Light> &lights)
{
	PROFILE_SCOPED()
	const float size = 1.0f / m_lightRect[2];
	const uint32_t n = Clamp(ceil_pow2(uint32_t(std::ceil(size / LIGHT_MAP_TEXEL))), 64U, LIGHT_MAP_MAX);
	const float texel = size / float(n);

	std::vector<float> map(size_t(n) * n * 4, 0.0f);
	for (const Light &light : lights) {
		const float h = light.height;
		if (light.radius <= h)
			continue;
		const float reach = std::sqrt(light.radius * light.radius - h * h);
		const float ground = light.pos.y - h;

		const int x0 = std::max(0, int(std::floor((light.pos.x - reach - m_lightRect[0]) / texel)));
		const int x1 = std::min(int(n) - 1, int(std::floor((light.pos.x + reach - m_lightRect[0]) / texel)));
		const int z0 = std::max(0, int(std::floor((light.pos.z - reach - m_lightRect[1]) / texel)));
		const int z1 = std::min(int(n) - 1, int(std::floor((light.pos.z + reach - m_lightRect[1]) / texel)));
		for (int tz = z0; tz <= z1; tz++) {
			for (int tx = x0; tx <= x1; tx++) {
				// averaged over 2x2 points of the texel; level ground is lit at h / d from straight above
				float E = 0.0f;
				for (int s = 0; s < 4; s++) {
					const float dx = m_lightRect[0] + (tx + 0.25f + 0.5f * (s & 1)) * texel - light.pos.x;
					const float dz = m_lightRect[1] + (tz + 0.25f + 0.5f * (s >> 1)) * texel - light.pos.z;
					const float d2 = dx * dx + dz * dz + h * h;
					E += LightFalloff(d2, light.radius) * h / std::sqrt(d2);
				}
				if (E <= 0.0f)
					continue;

				const vector3f c = light.colour * (0.25f * E);
				float *t = &map[(size_t(tz) * n + tx) * 4];
				t[0] += c.x;
				t[1] += c.y;
				t[2] += c.z;
				t[3] += Luminance(c) * ground;
			}
		}
	}

	const vector3f dataSize(float(n), float(n), 0.0f);
	const Graphics::TextureDescriptor desc(Graphics::TEXTURE_RGBA16F, dataSize, Graphics::LINEAR_CLAMP, true, false, false, 0, Graphics::TEXTURE_2D);
	m_lightMap.Reset(Pi::renderer->CreateTexture(desc));
	m_lightMap->Update(map.data(), dataSize, Graphics::TEXTURE_RGBA16F);
}

// The grid over the light map's square, listing the lights reaching each cell
// (the brightest if there are too many), packed into one float texture: see
// city-lights.glsl.
void CityOnPlanet::BuildLightGrid(const std::vector<Light> &lights)
{
	PROFILE_SCOPED()
	const float size = 1.0f / m_lightRect[2];
	const uint32_t width = uint32_t(std::ceil(size / LIGHT_GRID_CELL));
	const size_t numCells = size_t(width) * width;

	// visits the cells within a light's reach
	auto forCells = [&](const Light &light, auto &&visit) {
		const float r = light.radius;
		const int x0 = std::max(0, int(std::floor((light.pos.x - r - m_lightRect[0]) / LIGHT_GRID_CELL)));
		const int x1 = std::min(int(width) - 1, int(std::floor((light.pos.x + r - m_lightRect[0]) / LIGHT_GRID_CELL)));
		const int z0 = std::max(0, int(std::floor((light.pos.z - r - m_lightRect[1]) / LIGHT_GRID_CELL)));
		const int z1 = std::min(int(width) - 1, int(std::floor((light.pos.z + r - m_lightRect[1]) / LIGHT_GRID_CELL)));
		for (int cz = z0; cz <= z1; cz++) {
			for (int cx = x0; cx <= x1; cx++) {
				// the nearest point of the cell
				const float minX = m_lightRect[0] + cx * LIGHT_GRID_CELL, minZ = m_lightRect[1] + cz * LIGHT_GRID_CELL;
				const float dx = Clamp(light.pos.x, minX, minX + LIGHT_GRID_CELL) - light.pos.x;
				const float dz = Clamp(light.pos.z, minZ, minZ + LIGHT_GRID_CELL) - light.pos.z;
				if (dx * dx + dz * dz < r * r)
					visit(size_t(cz) * width + cx);
			}
		}
	};

	std::vector<uint32_t> counts(numCells, 0);
	for (const Light &light : lights)
		forCells(light, [&](size_t cell) { counts[cell]++; });

	std::vector<uint32_t> offsets(numCells + 1, 0);
	for (size_t c = 0; c < numCells; c++)
		offsets[c + 1] = offsets[c] + counts[c];

	std::vector<uint32_t> indices(offsets[numCells]);
	std::fill(counts.begin(), counts.end(), 0);
	for (uint32_t i = 0; i < lights.size(); i++)
		forCells(lights[i], [&](size_t cell) { indices[offsets[cell] + counts[cell]++] = i; });

	// per cell its first kept index and their number
	std::vector<uint32_t> firsts(numCells, 0);
	std::vector<uint32_t> kept;
	kept.reserve(indices.size());
	bool overflow = false;
	for (size_t c = 0; c < numCells; c++) {
		uint32_t *first = indices.data() + offsets[c];
		uint32_t count = counts[c];
		if (count > LIGHT_CELL_MAX) {
			// the brightest at the cell's centre
			const float cx = m_lightRect[0] + (c % width + 0.5f) * LIGHT_GRID_CELL;
			const float cz = m_lightRect[1] + (c / width + 0.5f) * LIGHT_GRID_CELL;
			auto brightness = [&](uint32_t i) {
				const Light &l = lights[i];
				const float dx = l.pos.x - cx, dz = l.pos.z - cz;
				return Luminance(l.colour) * LightFalloff(dx * dx + dz * dz + l.height * l.height, l.radius);
			};
			std::partial_sort(first, first + LIGHT_CELL_MAX, first + count,
				[&](uint32_t a, uint32_t b) { return brightness(a) > brightness(b); });
			count = LIGHT_CELL_MAX;
		}
		if (kept.size() + count > LIGHT_MAX_INDICES) {
			overflow = true;
			count = 0;
		}
		firsts[c] = uint32_t(kept.size());
		counts[c] = count;
		kept.insert(kept.end(), first, first + count);
	}
	if (overflow)
		Log::Warning("CityOnPlanet: too many night lights, some are left out");

	const uint32_t headerTexels = uint32_t((numCells + 1) / 2);
	const uint32_t indexTexels = uint32_t((kept.size() + 3) / 4);
	const uint32_t lightTexels = uint32_t(lights.size() * 2);
	const uint32_t rows = (headerTexels + indexTexels + lightTexels + LIGHT_ATLAS_WIDTH - 1) / LIGHT_ATLAS_WIDTH;

	std::vector<float> atlas(size_t(rows) * LIGHT_ATLAS_WIDTH * 4, 0.0f);
	for (size_t c = 0; c < numCells; c++) {
		atlas[2 * c] = float(firsts[c]);
		atlas[2 * c + 1] = float(counts[c]);
	}
	float *index = atlas.data() + size_t(headerTexels) * 4;
	for (size_t k = 0; k < kept.size(); k++)
		index[k] = float(kept[k]);
	float *texel = atlas.data() + size_t(headerTexels + indexTexels) * 4;
	for (const Light &light : lights) {
		*texel++ = light.pos.x;
		*texel++ = light.pos.y;
		*texel++ = light.pos.z;
		*texel++ = light.radius;
		*texel++ = light.colour.x;
		*texel++ = light.colour.y;
		*texel++ = light.colour.z;
		*texel++ = light.height;
	}

	m_lightGridInfo[0] = float(width);
	m_lightGridInfo[1] = float(width);
	m_lightGridInfo[2] = float(headerTexels);
	m_lightGridInfo[3] = float(headerTexels + indexTexels);

	const vector3f dataSize(float(LIGHT_ATLAS_WIDTH), float(rows), 0.0f);
	const Graphics::TextureDescriptor desc(Graphics::TEXTURE_RGBA32F, dataSize, Graphics::NEAREST_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
	m_lightGrid.Reset(Pi::renderer->CreateTexture(desc));
	m_lightGrid->Update(atlas.data(), dataSize, Graphics::TEXTURE_RGBA32F);
}

void CityOnPlanet::GetLightData(Graphics::CityLightDataBlock &out) const
{
	for (int i = 0; i < 4; i++) {
		out.mapRect[i] = m_lightRect[i];
		out.grid[i] = m_lightGridInfo[i];
	}
	out.state[0] = 1.0f;
	out.state[1] = LIGHT_NEAR_DISTANCE;
	out.state[2] = LIGHT_HEIGHT_RANGE;
	out.state[3] = 0.0f;
}

void CityOnPlanet::GetGroundData(Graphics::CityGroundDataBlock &out) const
{
	for (int i = 0; i < 4; i++) {
		out.roadRect[i] = m_roadRect[i];
		out.lotRect[i] = m_lotRect[i];
	}
	out.atlas[0] = m_roadAtlas[0];
	out.atlas[1] = m_roadAtlas[1];
	out.atlas[2] = 1.0f;
	out.atlas[3] = 0.0f;
}

void CityOnPlanet::BuildGround()
{
	PROFILE_SCOPED()
	if (!m_layout)
		return;

	// the roads (CityLayout::PackRoads)
	std::vector<float> atlas;
	float info[4], rect[4];
	m_layout->PackRoads(atlas, info, rect);
	const vector3f roadSize(float(CityLayout::TEXEL_WIDTH), info[3], 0.0f);
	const Graphics::TextureDescriptor roadDesc(Graphics::TEXTURE_RGBA32F, roadSize, Graphics::NEAREST_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
	m_roadTexture.Reset(Pi::renderer->CreateTexture(roadDesc));
	m_roadTexture->Update(atlas.data(), roadSize, Graphics::TEXTURE_RGBA32F);
	for (int i = 0; i < 4; i++)
		m_roadRect[i] = rect[i];
	m_roadAtlas[0] = info[1];
	m_roadAtlas[1] = info[2];

	// the lots, a texel per cell (RGBA, so the rows stay aligned)
	const int n = m_layout->GetSize();
	const std::vector<uint8_t> &cells = m_layout->GetLots();
	std::vector<uint8_t> lots(cells.size() * 4, 0);
	for (size_t k = 0; k < cells.size(); k++)
		lots[4 * k] = cells[k];
	const vector3f lotSize(float(n), float(n), 0.0f);
	const Graphics::TextureDescriptor lotDesc(Graphics::TEXTURE_RGBA_8888, lotSize, Graphics::NEAREST_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
	m_lotTexture.Reset(Pi::renderer->CreateTexture(lotDesc));
	m_lotTexture->Update(lots.data(), lotSize, Graphics::TEXTURE_RGBA_8888);
	m_lotRect[0] = float(m_layout->GetCorner());
	m_lotRect[1] = float(m_layout->GetCorner());
	m_lotRect[2] = float(CityLayout::CELL);
	m_lotRect[3] = float(n);
}

void CityOnPlanet::Render(Graphics::Renderer *r, const CameraContext *camera, const SpaceStation *station, const vector3d &viewCoords, const matrix4x4d &viewTransform)
{
	PROFILE_SCOPED()

	// Early out for failure case
	if (!m_cityType)
		return;

	// Early frustum test of whole city.
	const vector3d stationPos = viewTransform * (station->GetPosition() + m_realCentre);

	if (!camera->GetFrustum().TestSphere(stationPos, m_clipRadius))
		return;

	// Use the station-centered frame-oriented coordinate system we generated building positions in
	matrix4x4f modelView = matrix4x4f(viewTransform * matrix4x4d::Translation(station->GetInterpPosition()));

	// Combine all transformations to do as few computations as possible inside of the frustum-test loop
	// with as much precision as possible
	Graphics::FrustumF frustum = Graphics::FrustumF(modelView, camera->GetProjectionMatrix());

	// change detail level if necessary
	const bool bDetailChanged = m_detailLevel != Pi::detail.cities;
	if (bDetailChanged) {
		RemoveStaticGeomsFromCollisionSpace();
		AddStaticGeomsToCollisionSpace();
		PrecalcInstanceTransforms(station->GetOrient());
		BuildLighting();
	}

	// update any idle animations
	// TODO: this is kind of a horrible idea in many ways
	// animated buildings need per-instance state and need to support instanced drawing with GPU animation transform of some kind
	// individual cities at the *very* least need to have their own copy of the model used in them so color + animation state
	// doesn't "leak" into other users of the model
	for (auto &buildingType : m_cityType->buildingTypes) {
		SceneGraph::Animation *anim = buildingType.idleAnimation;
		if (anim) {
			anim->SetProgress(fmod(anim->GetProgress() + (Pi::game->GetTimeStep() / anim->GetDuration()), 1.0));
			anim->Interpolate();
		}
	}

	uint32_t uCount = 0;
	uint32_t numBuildings = m_cityType->buildingTypes.size();

	std::vector<std::vector<matrix4x4f>> transform;

	transform.resize(numBuildings);

	// TODO: we could store all instance transforms in a single array by
	// sorting the list of buildings according to instance index...
	// Of course, we can't do that because Model::RenderInstanced wants a full-
	// owning std::vector of instance data
	for (uint32_t i = 0; i < numBuildings; i++) {
		transform[i].reserve(m_buildingCounts[i]);
	}

	{
		// Because we cull each individual instance for performance reasons, we
		// have to repack instance transform data into an instance buffer to be
		// uploaded to the GPU
		// TODO: this could potentially be improved by clustering buildings
		// according to a spatial distance metric and then frustum culling those
		// clusters instead. It would result in more instanced draws however.

		PROFILE_SCOPED_DESC("CityOnPlanet::Render() Frustum Culling")

		for (size_t i = 0; i < m_enabledBuildings.size(); i++) {
			const auto &building = m_enabledBuildings[i];

			if (!frustum.TestSphere(building.pos, building.clipRadius))
				continue;

			transform[building.instIndex].emplace_back(m_instanceTransforms[i]);
			++uCount;
		}
	}

	// render the building models using instancing
	for (Uint32 i = 0; i < numBuildings; i++) {
		if (!transform[i].empty())
			m_cityType->buildingTypes[i].model->RenderInstanced(modelView, transform[i]);
	}

	// Draw debug extents
	/*
	r->SetTransform(matrix4x4f(viewTransform));
	Graphics::VertexArray va(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_DIFFUSE);

	vector3f origin = vector3f(m_gridOrigin);
	vector3f incX = vector3f(station->GetOrient().VectorX() * double(m_citySize * CELLSIZE));
	vector3f incZ = vector3f(station->GetOrient().VectorZ() * double(m_citySize * CELLSIZE));

	va.Add(origin, Color(255, 0, 0));
	va.Add(origin + incX, Color(255, 0, 0));

	va.Add(origin + incX, Color(0, 255, 0));
	va.Add(origin + incX + incZ, Color(0, 255, 0));

	va.Add(origin + incX + incZ, Color(255, 0, 0));
	va.Add(origin + incZ, Color(255, 0, 0));

	va.Add(origin + incZ, Color(0, 255, 0));
	va.Add(origin, Color(0, 255, 0));

	r->DrawBuffer(&va, s_debugMat.get());
	*/

	r->GetStats().AddToStatCount(Graphics::Stats::STAT_BUILDINGS, uCount);
	r->GetStats().AddToStatCount(Graphics::Stats::STAT_CITIES, 1);
}

void CityOnPlanet::RenderShadowCaster(Graphics::Renderer *r, const matrix4x4d &frameToLight, const SpaceStation *station, const Aabb &lightBox)
{
	PROFILE_SCOPED()

	// instance transforms are rebuilt by Render() when the detail level changes
	if (!m_cityType || m_detailLevel != Pi::detail.cities)
		return;

	// the station-centered frame-oriented coordinate system the buildings are positioned in (see Render)
	const matrix4x4d cityToLight = frameToLight * matrix4x4d::Translation(station->GetInterpPosition());

	const uint32_t numBuildings = m_cityType->buildingTypes.size();
	std::vector<std::vector<matrix4x4f>> transform(numBuildings);

	for (size_t i = 0; i < m_enabledBuildings.size(); i++) {
		const auto &building = m_enabledBuildings[i];

		const vector3d pos = cityToLight * vector3d(building.pos);
		const double rad = building.clipRadius;
		if (pos.x + rad < lightBox.min.x || pos.x - rad > lightBox.max.x ||
			pos.y + rad < lightBox.min.y || pos.y - rad > lightBox.max.y ||
			pos.z + rad < lightBox.min.z || pos.z - rad > lightBox.max.z)
			continue;

		transform[building.instIndex].emplace_back(m_instanceTransforms[i]);
	}

	const matrix4x4f modelView(cityToLight);
	for (uint32_t i = 0; i < numBuildings; i++) {
		if (!transform[i].empty())
			m_cityType->buildingTypes[i].model->RenderShadowInstanced(modelView, transform[i]);
	}
}

//static
bool CityOnPlanet::NeedsForcefield(const SystemBody *body, const ForcefieldDef &def)
{
	if (!body->HasAtmosphere())
		return true;
	const double density = body->GetVolatileGas();
	const int temperature = body->GetAverageTemp();
	const bool breathable = body->GetAtmosOxidizing() > def.minOxidizing &&
		density >= def.minDensity && density < def.maxDensity &&
		temperature >= def.minTemperature && temperature <= def.maxTemperature;
	return !breathable;
}

// The triangles of a geodesic sphere: an icosahedron with each edge cut in
// freq, one vertex straight up (+y), corners on the unit sphere
static std::vector<vector3d> GeodesicSphere(int freq)
{
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	vector3d ico[12] = {
		vector3d(-1.0, phi, 0.0), vector3d(1.0, phi, 0.0), vector3d(-1.0, -phi, 0.0), vector3d(1.0, -phi, 0.0),
		vector3d(0.0, -1.0, phi), vector3d(0.0, 1.0, phi), vector3d(0.0, -1.0, -phi), vector3d(0.0, 1.0, -phi),
		vector3d(phi, 0.0, -1.0), vector3d(phi, 0.0, 1.0), vector3d(-phi, 0.0, -1.0), vector3d(-phi, 0.0, 1.0)
	};
	static const int faces[20][3] = {
		{ 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 },
		{ 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
		{ 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 },
		{ 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 }
	};
	// turn vertex 5 about x to point up
	const double t = -std::atan2(phi, 1.0), c = std::cos(t), s = std::sin(t);
	for (vector3d &v : ico) {
		v = v.Normalized();
		v = vector3d(v.x, v.y * c - v.z * s, v.y * s + v.z * c);
	}

	std::vector<vector3d> tris;
	tris.reserve(20 * freq * freq * 3);
	for (const auto &f : faces) {
		const vector3d &A = ico[f[0]], &B = ico[f[1]], &C = ico[f[2]];
		auto pt = [&](int i, int j) {
			return (A * double(freq - i - j) + B * double(i) + C * double(j)).Normalized();
		};
		for (int i = 0; i < freq; i++) {
			for (int j = 0; j < freq - i; j++) {
				tris.push_back(pt(i, j));
				tris.push_back(pt(i + 1, j));
				tris.push_back(pt(i, j + 1));
				if (i + j < freq - 1) {
					tris.push_back(pt(i + 1, j));
					tris.push_back(pt(i + 1, j + 1));
					tris.push_back(pt(i, j + 1));
				}
			}
		}
	}
	return tris;
}

void CityOnPlanet::BuildForcefields(SpaceStation *station)
{
	m_ffUp = m_stationOrient.VectorY();
	if (m_layout && !m_layout->GetDomes().empty()) {
		for (const CityLayout::Dome &dome : m_layout->GetDomes())
			BuildForcefield(station, dome.x, dome.z, dome.radius);
	} else {
		BuildForcefield(station, 0.0, 0.0, m_cityRadius * m_cityType->forcefield.radiusScale);
	}
}

void CityOnPlanet::BuildForcefield(SpaceStation *station, double cx, double cz, double a)
{
	PROFILE_SCOPED()
	const ForcefieldDef &def = m_cityType->forcefield;
	std::unique_ptr<Forcefield> field(new Forcefield);
	Forcefield &ff = *field;

	const vector3d axisX = m_stationOrient.VectorX();
	const vector3d up = m_stationOrient.VectorY();
	const vector3d axisZ = m_stationOrient.VectorZ();
	const vector3d stationPos = station->GetPosition();
	// its middle on the ground, from the station, and the ground's height there over the station
	const vector3d middle = axisX * cx + axisZ * cz;
	const vector3d middleDir = (stationPos + middle).Normalized();
	const double ground = (middleDir * m_planet->GetTerrainHeight(middleDir) - stationPos).Dot(up);
	// the buildings it holds: those under it (the plan's domes), or all
	const double reach = (m_layout && !m_layout->GetDomes().empty()) ? a + 100.0 : 1e30;

	// the outer top corner of each building (the station's among them):
	// distance out from its middle and height over the station, m
	std::vector<std::pair<double, double>> tops;
	tops.reserve(m_buildings.size() + 1);
	auto addTop = [&](const vector3d &pos, const Aabb &aabb) {
		const double height = pos.Dot(up);
		const double out = (pos - up * height - middle).Length();
		if (out > reach)
			return;
		const double halfX = std::max(std::abs(aabb.min.x), std::abs(aabb.max.x));
		const double halfZ = std::max(std::abs(aabb.min.z), std::abs(aabb.max.z));
		tops.emplace_back(out + std::sqrt(halfX * halfX + halfZ * halfZ), height + aabb.max.y);
	};
	addTop(vector3d(0.0), station->GetAabb());
	for (const BuildingInstance &building : m_buildings)
		addTop(vector3d(building.pos), m_cityType->buildingTypes[building.instIndex].model->GetCollisionMesh()->GetAabb());

	// a cap of a sphere, radius a at its ground and h high, made big enough
	// to hold every building clear of it
	double h = 0.0, rho = 0.0;
	for (int i = 0; i <= FORCEFIELD_FIT_STEPS; i++) {
		h = a * def.heightRatio;
		rho = (a * a + h * h) / (2.0 * h);
		const double centreHeight = ground + h - rho;
		const double inner = rho - FORCEFIELD_CLEARANCE;
		const bool fits = std::all_of(tops.begin(), tops.end(), [&](const std::pair<double, double> &top) {
			const double dy = top.second - centreHeight;
			return top.first * top.first + dy * dy <= inner * inner;
		});
		if (fits || i == FORCEFIELD_FIT_STEPS)
			break;
		a *= FORCEFIELD_FIT_GROWTH;
	}

	// its skirt reaches below the ground all round, which hides it
	double bottom = ground;
	for (int i = 0; i < FORCEFIELD_RIM_SAMPLES; i++) {
		const double angle = 2.0 * M_PI * i / FORCEFIELD_RIM_SAMPLES;
		const vector3d out = axisX * std::cos(angle) + axisZ * std::sin(angle);
		for (const double share : { 0.8, 1.0 }) {
			const vector3d dir = (stationPos + middle + out * (a * share)).Normalized();
			bottom = std::min(bottom, (dir * m_planet->GetTerrainHeight(dir) - stationPos).Dot(up));
		}
	}
	bottom -= FORCEFIELD_SKIRT;

	ff.centre = middle + up * (ground + h - rho);
	ff.radius = rho;
	ff.bottom = bottom;

	// the field: the triangles reaching above the skirt's foot
	const std::vector<vector3d> sphere = GeodesicSphere(def.frequency);
	Graphics::VertexArray va(Graphics::ATTRIB_POSITION, int(sphere.size()));
	std::vector<vector3f> vertices;
	vertices.reserve(sphere.size());
	// and the joints at their corners, and the struts along their edges
	std::vector<vector3d> joints;
	std::map<std::tuple<int64_t, int64_t, int64_t>, Uint32> jointIndex;
	std::set<std::pair<Uint32, Uint32>> edges;
	auto joint = [&](const vector3d &p) {
		const auto key = std::make_tuple(int64_t(std::llround(p.x * 100.0)), int64_t(std::llround(p.y * 100.0)), int64_t(std::llround(p.z * 100.0)));
		const auto it = jointIndex.find(key);
		if (it != jointIndex.end())
			return it->second;
		joints.push_back(p);
		return jointIndex[key] = Uint32(joints.size() - 1);
	};
	for (size_t i = 0; i + 2 < sphere.size(); i += 3) {
		vector3d p[3];
		double top = -1e30;
		for (int k = 0; k < 3; k++) {
			const vector3d &u = sphere[i + k];
			p[k] = ff.centre + (axisX * u.x + up * u.y + axisZ * u.z) * rho;
			top = std::max(top, p[k].Dot(up));
		}
		if (top < bottom)
			continue;
		// counter-clockwise seen from outside
		if ((p[1] - p[0]).Cross(p[2] - p[0]).Dot(p[0] + p[1] + p[2] - ff.centre * 3.0) < 0.0)
			std::swap(p[1], p[2]);
		Uint32 j[3];
		for (int k = 0; k < 3; k++) {
			va.Add(vector3f(p[k]));
			vertices.push_back(vector3f(p[k]));
			ff.extent = std::max(ff.extent, p[k].Length());
			j[k] = joint(p[k]);
		}
		for (int k = 0; k < 3; k++)
			edges.insert(std::make_pair(std::min(j[k], j[(k + 1) % 3]), std::max(j[k], j[(k + 1) % 3])));
	}
	if (vertices.empty())
		return;

	// struts: tubes made of rings round their axis, each vertex at the axis
	// with its normal out, widened in forcefield_struts.vert
	ff.strutRadius = Clamp(rho * FORCEFIELD_STRUT_RADIUS * def.strutWidth, FORCEFIELD_STRUT_RADIUS_MIN, FORCEFIELD_STRUT_RADIUS_MAX);
	Graphics::VertexArray strutVa(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_NORMAL | Graphics::ATTRIB_UV0);
	std::vector<Uint32> strutIndices;
	// a ring's vertices have normals (side1 cos + side2 sin) * spread + lift;
	// rings follow each other along side2 x side1, so the band between them
	// is counter-clockwise seen from outside
	struct Ring {
		vector3d centre, side1, side2, lift;
		double spread;
	};
	auto addBand = [&](const std::vector<Ring> &rings, float scale) {
		const Uint32 sides = FORCEFIELD_STRUT_SIDES;
		const Uint32 first = strutVa.GetNumVerts();
		for (const Ring &ring : rings) {
			for (Uint32 k = 0; k < sides; k++) {
				const double angle = 2.0 * M_PI * k / sides;
				const vector3d normal = (ring.side1 * std::cos(angle) + ring.side2 * std::sin(angle)) * ring.spread + ring.lift;
				strutVa.Add(vector3f(ring.centre), vector3f(normal), vector2f(scale, 0.0f));
			}
		}
		for (Uint32 s = 0; s + 1 < rings.size(); s++) {
			for (Uint32 k = 0; k < sides; k++) {
				const Uint32 i0 = first + s * sides + k, i1 = first + s * sides + (k + 1) % sides;
				for (Uint32 index : { i0, i1, i0 + sides, i1, i1 + sides, i0 + sides })
					strutIndices.push_back(index);
			}
		}
	};
	for (const auto &edge : edges) {
		const vector3d &p0 = joints[edge.first], &p1 = joints[edge.second];
		const vector3d along = (p1 - p0).Normalized();
		const vector3d side1 = along.Cross((p0 + p1) * 0.5 - ff.centre).Normalized();
		const vector3d side2 = along.Cross(side1);
		// cut so the ripples can bend it
		const int segments = Clamp(int(std::ceil((p1 - p0).Length() / FORCEFIELD_STRUT_SEGMENT)), 1, FORCEFIELD_STRUT_MAX_SEGMENTS);
		std::vector<Ring> rings;
		for (int s = 0; s <= segments; s++)
			rings.push_back({ p0 + (p1 - p0) * (double(s) / segments), side1, side2, vector3d(0.0), 1.0 });
		addBand(rings, 1.0f);
	}
	// joints: balls of rings from their outer pole to their inner one
	for (const vector3d &p : joints) {
		const vector3d pole = (p - ff.centre).Normalized();
		const vector3d side1 = pole.Cross(std::abs(pole.Dot(axisX)) < 0.9 ? axisX : axisZ).Normalized();
		const vector3d side2 = side1.Cross(pole);
		std::vector<Ring> rings;
		for (int s = 0; s <= 4; s++) {
			const double lat = M_PI * s / 4.0;
			rings.push_back({ p, side1, side2, pole * std::cos(lat), std::sin(lat) });
		}
		addBand(rings, FORCEFIELD_JOINT_SCALE);
	}

	Graphics::Renderer *r = Pi::renderer;
	ff.mesh.reset(r->CreateMeshObjectFromArray(&va));
	Graphics::RenderStateDesc rsd;
	rsd.blendMode = Graphics::BLEND_ALPHA_PREMULT;
	rsd.depthWrite = false;
	rsd.cullMode = Graphics::CULL_FRONT;
	ff.backMaterial.reset(r->CreateMaterial("forcefield", Graphics::MaterialDescriptor(), rsd, ff.mesh->GetFormat()));
	rsd.cullMode = Graphics::CULL_BACK;
	ff.frontMaterial.reset(r->CreateMaterial("forcefield", Graphics::MaterialDescriptor(), rsd, ff.mesh->GetFormat()));

	Graphics::IndexBuffer *strutIndexBuffer = r->CreateIndexBuffer(strutIndices.size(), Graphics::BUFFER_USAGE_STATIC);
	std::copy(strutIndices.begin(), strutIndices.end(), strutIndexBuffer->Map(Graphics::BUFFER_MAP_WRITE));
	strutIndexBuffer->Unmap();
	ff.strutMesh.reset(r->CreateMeshObjectFromArray(&strutVa, strutIndexBuffer));
	// lit by the stars, and drawn into the depth buffer so the near ones hide the far
	Graphics::MaterialDescriptor strutDesc;
	strutDesc.lighting = true;
	rsd.depthWrite = true;
	ff.strutMaterial.reset(r->CreateMaterial("forcefield_struts", strutDesc, rsd, ff.strutMesh->GetFormat()));

	// a static geom of the station, so its contacts come to this city (Space.cpp)
	const int numTris = int(vertices.size() / 3);
	std::vector<Uint32> indices(vertices.size());
	std::iota(indices.begin(), indices.end(), 0);
	const std::vector<Uint32> flags(numTris, SceneGraph::CollisionGeometry::FORCEFIELD);
	ff.geomTree.reset(new GeomTree(int(vertices.size()), numTris, vertices, indices, flags));
	ff.geom.reset(new Geom(ff.geomTree.get(), matrix4x4d::Identity, stationPos, static_cast<Body *>(station)));
	ff.geom->SetRayTransparent(true);
	Frame::GetFrame(m_frame)->AddStaticGeom(ff.geom.get());

	// the player may be inside already (a game loaded there)
	const Player *player = Pi::player;
	if (player && player->GetFrame() == station->GetFrame())
		ff.playerInside = (player->GetPosition() - stationPos - ff.centre).LengthSqr() < rho * rho;

	m_ffExtent = std::max(m_ffExtent, ff.extent);
	Log::Verbose("\tCityOnPlanet: forcefield {:.0f} m across, {:.0f} m high, {} triangles", 2.0 * a, h, numTris);
	m_forcefields.push_back(std::move(field));
}

bool CityOnPlanet::ForcefieldPasses(const SpaceStation *station, const Forcefield &field, const Ship *ship) const
{
	// other ships are traffic
	if (!ship->IsType(ObjectType::PLAYER))
		return true;
	// going out, or already in, so nobody is shut in
	const vector3d rel = ship->GetPosition() - (station->GetPosition() + field.centre);
	if (field.playerInside || rel.LengthSqr() < field.radius * field.radius || rel.Dot(ship->GetVelocity()) >= 0.0)
		return true;
	return PlayerMayEnter(station, field, ship);
}

bool CityOnPlanet::PlayerMayEnter(const SpaceStation *station, const Forcefield &field, const Ship *player) const
{
	// with landing clearance, or back soon after leaving
	return station->GetMyDockingPort(player) != -1 ||
		Pi::game->GetTime() - field.playerLeftTime < FORCEFIELD_RETURN_TIME;
}

void CityOnPlanet::AddForcefieldImpact(Forcefield &field, const vector3d &point, const Ship *ship)
{
	// none where one just started
	for (const ForcefieldImpact &impact : field.impacts) {
		if (impact.time < FORCEFIELD_IMPACT_REPEAT && (impact.point - point).LengthSqr() < impact.reach * impact.reach)
			return;
	}
	if (field.impacts.size() >= FORCEFIELD_MAX_IMPACTS)
		field.impacts.erase(field.impacts.begin());
	const double reach = Clamp(ship->GetClipRadius() * FORCEFIELD_IMPACT_SCALE, FORCEFIELD_IMPACT_MIN, FORCEFIELD_IMPACT_MAX);
	field.impacts.push_back({ point, float(reach), 0.0f });
	Sound::BodyMakeNoise(ship, "MembraneBounce", 1.0f);
}

bool CityOnPlanet::ForcefieldStops(SpaceStation *station, Ship *ship)
{
	// the dome whose shell the ship is at
	Forcefield *field = nullptr;
	double best = 1e30;
	for (const auto &f : m_forcefields) {
		const double d = std::abs((ship->GetPosition() - (station->GetPosition() + f->centre)).Length() - f->radius);
		if (d < best) {
			best = d;
			field = f.get();
		}
	}
	if (!field || ForcefieldPasses(station, *field, ship))
		return false;

	// a wave from where it hit
	const vector3d n = (ship->GetPosition() - (station->GetPosition() + field->centre)).Normalized();
	AddForcefieldImpact(*field, field->centre + n * field->radius, ship);
	ForcefieldHit(station, ship, -ship->GetVelocity().Dot(n));
	return true;
}

void CityOnPlanet::ForcefieldHit(SpaceStation *station, Ship *ship, double inwardSpeed)
{
	// too fast and it is destroyed, as hitting the ground
	const float lethal = m_cityType->forcefield.lethalSpeed;
	if (lethal > 0.0f && inwardSpeed > lethal && !ship->IsDead() && !ship->IsInvulnerable()) {
		LuaEvent::Queue("onShipDestroyed", ship, static_cast<Body *>(station));
		ship->Explode();
	}
}

void CityOnPlanet::KeepPlayerOut(SpaceStation *station, Forcefield &field)
{
	Ship *player = Pi::player;
	if (!player || player->GetFrame() != station->GetFrame()) {
		field.playerInside = false;
		return;
	}

	const vector3d centre = station->GetPosition() + field.centre;
	vector3d rel = player->GetPosition() - centre;
	double dist = rel.Length();

	// coming to it from outside, not allowed in: put back at its own radius
	// outside and bounced off, however fast it came (collisions can miss a
	// fast ship stepping through)
	const double shell = field.radius + player->GetClipRadius() * FORCEFIELD_SHIP_SHELL;
	if (!field.playerInside && dist < shell && dist > 0.0 && player->GetFlightState() == Ship::FLYING &&
		(field.centre + rel * (field.radius / dist)).Dot(m_ffUp) > field.bottom && !PlayerMayEnter(station, field, player)) {
		const vector3d n = rel / dist;
		player->SetPosition(centre + n * shell);
		const vector3d vel = player->GetVelocity();
		const double inward = vel.Dot(n);
		if (inward < 0.0)
			player->SetVelocity(vel - n * (inward * (1.0 + FORCEFIELD_BOUNCE)));
		// not for resting on it
		if (inward < -FORCEFIELD_IMPACT_SPEED)
			AddForcefieldImpact(field, field.centre + n * field.radius, player);
		ForcefieldHit(station, player, -inward);
		rel = n * shell;
		dist = shell;
	}

	// when the player last went out
	const bool inside = dist < field.radius;
	if (field.playerInside && !inside)
		field.playerLeftTime = Pi::game->GetTime();
	field.playerInside = inside;
}

bool CityOnPlanet::IsPlayerInsideForcefield() const
{
	for (const auto &field : m_forcefields) {
		if (field->playerInside)
			return true;
	}
	return false;
}

void CityOnPlanet::UpdateForcefield(float timeStep, SpaceStation *station)
{
	if (!HasForcefield())
		return;
	PROFILE_SCOPED()

	for (const auto &f : m_forcefields) {
		Forcefield &field = *f;
		for (ForcefieldImpact &impact : field.impacts)
			impact.time += timeStep;
		field.impacts.erase(std::remove_if(field.impacts.begin(), field.impacts.end(),
								[](const ForcefieldImpact &impact) { return impact.time >= FORCEFIELD_IMPACT_TIME; }),
			field.impacts.end());

		KeepPlayerOut(station, field);

		for (ForcefieldOpening &opening : field.openings)
			opening.nearby = false;
	}

	// open round the ships they let through as they come to them
	for (Body *body : Pi::game->GetSpace()->GetBodiesMaybeNear(station, m_ffExtent + FORCEFIELD_OPENING_RANGE)) {
		if (!body->IsType(ObjectType::SHIP) || body->GetFrame() != station->GetFrame())
			continue;
		const Ship *ship = static_cast<const Ship *>(body);
		if (ship->GetFlightState() != Ship::FLYING)
			continue;

		for (const auto &f : m_forcefields) {
			Forcefield &field = *f;
			if (!ForcefieldPasses(station, field, ship))
				continue;
			const vector3d rel = ship->GetPosition() - (station->GetPosition() + field.centre);
			const double dist = rel.Length();
			const double radius = Clamp(ship->GetClipRadius() * FORCEFIELD_OPENING_SCALE, FORCEFIELD_OPENING_MIN, FORCEFIELD_OPENING_MAX);
			if (std::abs(dist - field.radius) > radius + ship->GetVelocity().Length() * FORCEFIELD_OPENING_LEAD)
				continue;
			// where it meets the field, if the field is there and not underground
			const vector3d point = field.centre + rel * (field.radius / dist);
			if (point.Dot(m_ffUp) < field.bottom)
				continue;

			auto it = std::find_if(field.openings.begin(), field.openings.end(),
				[body](const ForcefieldOpening &opening) { return opening.ship == body; });
			if (it != field.openings.end()) {
				it->centre = point;
				it->radius = float(radius);
				it->nearby = true;
			} else if (field.openings.size() < FORCEFIELD_MAX_OPENINGS) {
				field.openings.push_back({ body, point, float(radius), 0.0f, true });
				Sound::BodyMakeNoise(ship, "MembranePass", 1.0f);
			}
		}
	}

	for (const auto &f : m_forcefields) {
		Forcefield &field = *f;
		for (ForcefieldOpening &opening : field.openings) {
			opening.open = opening.nearby ?
				std::min(1.0f, opening.open + timeStep / FORCEFIELD_OPEN_TIME) :
				std::max(0.0f, opening.open - timeStep / FORCEFIELD_CLOSE_TIME);
		}
		field.openings.erase(std::remove_if(field.openings.begin(), field.openings.end(),
								 [](const ForcefieldOpening &opening) { return !opening.nearby && opening.open <= 0.0f; }),
			field.openings.end());
	}
}

void CityOnPlanet::QueueForcefield(const CameraContext *camera, const SpaceStation *station, const matrix4x4d &viewTransform) const
{
	if (!HasForcefield())
		return;
	const matrix4x4d modelView = viewTransform * matrix4x4d::Translation(station->GetInterpPosition());
	const vector3d origin = modelView * vector3d(0.0);
	for (const auto &field : m_forcefields) {
		if (!camera->GetFrustum().TestSphere(origin, field->extent))
			continue;
		// farthest first by the middle of its cap
		const double distance = (modelView * (field->centre + m_ffUp * field->radius)).Length();
		s_forcefieldQueue.push_back({ this, field.get(), modelView, distance });
	}
}

//static
void CityOnPlanet::BeginFrame()
{
	s_forcefieldQueue.clear();
}

//static
void CityOnPlanet::RenderForcefields(Graphics::Renderer *r)
{
	PROFILE_SCOPED()
	std::sort(s_forcefieldQueue.begin(), s_forcefieldQueue.end(),
		[](const QueuedForcefield &a, const QueuedForcefield &b) { return a.distance > b.distance; });

	for (const QueuedForcefield &queued : s_forcefieldQueue) {
		const CityOnPlanet *city = queued.city;
		const Forcefield *field = queued.field;
		const double t = Clamp((FORCEFIELD_FADE_FAR - queued.distance) / (FORCEFIELD_FADE_FAR - FORCEFIELD_FADE_NEAR), 0.0, 1.0);
		if (t <= 0.0)
			continue;

		ForcefieldDataBlock data = {};
		data.sphereCentre = vector3f(field->centre);
		data.sphereRadius = float(field->radius);
		size_t numOpenings = 0;
		for (const ForcefieldOpening &opening : field->openings) {
			if (opening.open <= 0.0f)
				continue;
			const float open = opening.open * opening.open * (3.0f - 2.0f * opening.open);
			data.openings[numOpenings].centre = vector3f(opening.centre);
			data.openings[numOpenings].radius = opening.radius * open;
			if (++numOpenings == FORCEFIELD_MAX_OPENINGS)
				break;
		}
		for (size_t i = 0; i < FORCEFIELD_MAX_IMPACTS; i++) {
			if (i < field->impacts.size()) {
				const ForcefieldImpact &impact = field->impacts[i];
				data.impacts[i].point = vector3f(impact.point);
				data.impacts[i].age = impact.time / FORCEFIELD_IMPACT_TIME;
				data.impactReach[i] = impact.reach;
			} else {
				data.impacts[i].age = -1.0f;
			}
		}
		data.strutRadius = float(field->strutRadius);
		// half the least width, in radians a pixel spans
		data.strutRadiusPerMetre = 0.5f * FORCEFIELD_STRUT_MIN_PX * float(Graphics::GetFovFactor()) / float(r->GetWindowHeight());
		data.colour = city->m_cityType->forcefield.colour;
		data.strutOpacity = city->m_cityType->forcefield.opacity;
		data.fill = FORCEFIELD_FILL;
		data.backFaces = FORCEFIELD_BACK_FACES;
		data.fade = float(t * t * (3.0 - 2.0 * t));

		field->backMaterial->SetBufferDynamic(s_forcefieldDataName, &data);
		field->frontMaterial->SetBufferDynamic(s_forcefieldDataName, &data);
		field->strutMaterial->SetBufferDynamic(s_forcefieldDataName, &data);
		Graphics::Renderer::MatrixTicket mt(r, matrix4x4f(queued.modelView));
		// the far side of the field, the struts, the near side
		r->DrawMesh(field->mesh.get(), field->backMaterial.get());
		r->DrawMesh(field->strutMesh.get(), field->strutMaterial.get());
		r->DrawMesh(field->mesh.get(), field->frontMaterial.get());
	}
	s_forcefieldQueue.clear();
}
