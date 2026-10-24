// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "CityOnPlanet.h"

#include "Camera.h"
#include "FileSystem.h"
#include "Frame.h"
#include "Game.h"
#include "JsonUtils.h"
#include "MathUtil.h"
#include "ModelCache.h"
#include "Pi.h"
#include "Planet.h"
#include "SpaceStation.h"

#include "collider/Geom.h"
#include "core/Log.h"

#include "graphics/Frustum.h"
#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/RenderState.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "graphics/Types.h"
#include "graphics/VertexBuffer.h"
#include "scenegraph/Animation.h"
#include "scenegraph/Model.h"
#include "scenegraph/ModelSkin.h"

#include "matrix3x3.h"
#include "matrix4x4.h"

#include "utils.h"

#include <algorithm>

// Night lights (city-lights.glsl). Brightness is the light reaching a
// surface square on at 1 m, before the inverse square falloff.
// street lamps; cells without a ground height (no building) for them
static constexpr float NO_CELL_HEIGHT = -1e30f;
static constexpr float LAMP_HEIGHT = 8.0f;
static constexpr float LAMP_RADIUS = 40.0f;
static constexpr float LAMP_BRIGHTNESS = 80.0f;
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

std::vector<CityOnPlanet::CityFlavourType> CityOnPlanet::s_cityFlavours;
std::unique_ptr<Graphics::Material> CityOnPlanet::s_debugMat;

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

	// Precalculate building orientation matrices
	matrix3x3f rot[4] = { matrix3x3f(stationOrient.GetOrient()) };
	for (int i = 1; i < 4; i++) {
		rot[i] = rot[0] * matrix3x3f::RotateY(M_PI * 0.5 * double(i));
	}

	// Cache building instance transforms
	// TODO: this could easily be represented as a 4x3 matrix or dual quaternion for significantly less bandwidth
	// That requires an "instance buffer" that isn't a 4x4 matrix per instance though... :(
	for (const auto &building : m_enabledBuildings) {
		m_instanceTransforms.emplace_back(rot[building.rotation], building.pos);
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

	GetModelSize(out.model->GetCollisionMesh()->GetAabb(), out.cellSize);

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

	// Create the acceleration grid structure
	// ==========================================

	// Retrieve the station parameters
	uint8_t stationSize[2] = {};
	GetModelSize(station->GetAabb(), stationSize);

	uint32_t stationSizeMax = std::max<uint32_t>(stationSize[0] / 2, stationSize[1] / 2);

	// update the allowed radius to ensure the spaceport isn't larger than the entire city
	m_cityRadius += stationSizeMax * CELLSIZE;

	// For now, just assume the city is a circle around the spaceport in the center
	const uint32_t cityExtents = (m_cityRadius / CELLSIZE);
	m_citySize = cityExtents * 2;
	m_gridPitch = std::ceil(m_citySize / 8.0);

	Log::Verbose("Generating City for spacestation {}", station->GetSystemBody()->GetName());
	Log::Verbose("\tpopulation: {0} size {1}x{1}c (radius {2})",
		m_body->GetPopulation(), m_citySize, m_cityRadius);


	// reserve space off the 'edge' of the grid for fast 64-bit lookup
	// prevents reading or writing unallocated memory on grid edges
	m_gridPitch = (m_gridPitch + 7) & ~uint32_t(7);

	m_gridLen = m_gridPitch * m_citySize;

	// 64-bit accesses start on 4-byte boundaries, so the last row can read up
	// to 4 bytes past m_gridLen; pad the allocation to keep them in bounds
	m_gridBitset.reset(new uint8_t[m_gridLen + 8]);
	std::memset(m_gridBitset.get(), 0, m_gridLen + 8);

	// Setup the station model position relative to the city grid
	// ==========================================

	// get the increment vectors to multiply X/Z grid coordinates by
	vector3d incX = station->GetOrient().VectorX() * CELLSIZE;
	vector3d incZ = station->GetOrient().VectorZ() * CELLSIZE;

	// top-left corner of the grid is -X, -Z relative to station position at center
	// offset origin by half-grid to ensure grid centers are aligned with the station model
	const vector3d stationOrigin = station->GetPosition();
	m_gridOrigin = stationOrigin - incX * (cityExtents + 0.5) - incZ * (cityExtents + 0.5);

	// Setup the station somewhere in the city (defaults to center for now)
	const uint32_t stationPos[2] = {
		cityExtents - stationSize[0] / 2,
		cityExtents - stationSize[1] / 2
	};

	// Reserve space for the spacestation
	SetGridOccupancy(stationPos[0], stationPos[1], stationSize);

	// ground heights above the planet's radius of the cells taken by the
	// station and the buildings, for the street lamps beside them
	std::vector<float> cellHeights(size_t(m_citySize) * m_citySize, NO_CELL_HEIGHT);
	const float stationHeight = float(stationOrigin.Length() - m_body->GetRadius());
	for (uint32_t y = stationPos[1]; y < std::min(stationPos[1] + stationSize[1], m_citySize); y++)
		for (uint32_t x = stationPos[0]; x < std::min(stationPos[0] + stationSize[0], m_citySize); x++)
			cellHeights[size_t(y) * m_citySize + x] = stationHeight;

	Log::Verbose("\tCityOnPlanet: Station {} placed at grid {}:{} with extents {}x{}",
		station->GetModel()->GetName(), stationPos[0], stationPos[1], stationSize[0], stationSize[1]);


	// Begin generating buildings for the city
	// ==========================================

	// precalc orientation transforms (to rotate buildings to face north/south/east/west)
	const matrix4x4d &m = station->GetOrient();

	matrix4x4d orientcalc[4];
	orientcalc[0] = m * matrix4x4d::RotateYMatrix(M_PI * 0.5 * 0);
	orientcalc[1] = m * matrix4x4d::RotateYMatrix(M_PI * 0.5 * 1);
	orientcalc[2] = m * matrix4x4d::RotateYMatrix(M_PI * 0.5 * 2);
	orientcalc[3] = m * matrix4x4d::RotateYMatrix(M_PI * 0.5 * 3);

	// Reserve space for all buildings we expect to generate
	m_buildings.clear();
	m_buildings.reserve(cityExtents * cityExtents); // estimate 25% occupancy

	bool hasAtmo = m_body->HasAtmosphere();

	uint32_t discardedCells = 0;
	uint32_t skippedCells = 0;
	uint32_t occupiedCells = 0;
	uint32_t underwaterCells = 0;
	uint32_t rarityRolls = 0;

	double cityRadiusSqr = m_cityRadius * m_cityRadius;

	for (uint32_t y = 0; y < m_citySize; y++) {
		for (uint32_t x = 0; x < m_citySize; x++) {

			// Do a basic loop over every cell in the city and determine what could be placed there

			// TODO: future versions of cities should generate a random road network or place individual
			// important buildings and place additional buildings around said features.
			// The quality of the result relative to the generation cost would be much improved.

			double radiusSqr = vector2d(
				(cityExtents - double(x) + 0.5) * CELLSIZE,
				(cityExtents - double(y) + 0.5) * CELLSIZE
			).LengthSqr();

			// skip cells outside the city radius
			if (radiusSqr >= cityRadiusSqr) {
				skippedCells++;
				continue;
			}

			// skip already full cells
			if (TestGridQuick(x, y)) {
				occupiedCells++;
				continue;
			}

			// allow the population of cells to drop off as distance from city center increases
			// note: this should ideally sample e.g. perlin noise to generate "clusters" of city buildings
			double radiusNorm = sqrt(radiusSqr) / m_cityRadius;
			if (radiusNorm > abs(m_rand.Normal()) * m_cityDensity) {
				discardedCells++;
				continue;
			}

			// Attempt to select a building type for this grid cell
			const BuildingType *buildingType = nullptr;
			uint32_t typeIndex = 0;
			for (uint32_t retry = 0; retry < 8; retry++) {

				// pick a random building type
				typeIndex = m_rand.Int32(m_cityType->buildingTypes.size());
				buildingType = &m_cityType->buildingTypes[typeIndex];

				// skip it if it won't fit here
				// FIXME: this does not correctly handle the rotation case for non-square buildings.
				// Need to transpose the cell size and test as well.
				if (TestGridOccupancy(x, y, buildingType->cellSize)) {
					buildingType = nullptr;
					continue;
				}

				float rarity = hasAtmo ? buildingType->rarityAtmo : buildingType->rarityAirless;
				float distribution = radiusNorm; // 0.0 .. 1.0 at edge of city

				if (buildingType->buildingKind == SectorKind::Storage) {
					// rarer in the center and more present at the edges of the city
					rarity *= 0.5 + distribution * 0.5;
				} else if (buildingType->buildingKind == SectorKind::Industry) {
					// rarer in the center and more present at the edges of the city
					rarity *= 0.3 + distribution * 0.7;
				} else if (buildingType->buildingKind == SectorKind::Monument) {
					// prevalent in the city center but extremely rare on the outskirts
					rarity *= (1.0 - distribution) * 0.5;
				} else if (buildingType->buildingKind == SectorKind::Habitat) {
					// 1.0 at city center and 0.2 at outskirts
					rarity *= 1.0 - distribution * 0.8;
				} else if (buildingType->buildingKind == SectorKind::Frontier) {
					// extremely rare at city center and 1.5 at outskirts
					rarity *= (distribution * distribution) * 1.5;
				}

				// roll to see if the building spawns here or not
				if (rarity < 1.0 && m_rand.Double() > rarity) {
					rarityRolls++;
					buildingType = nullptr;
					continue;
				}

				// found a building that fits and passed its rarity roll
				break;
			}

			// if we cannot find a building type, leave the cell empty and move on
			if (buildingType == nullptr) {
				discardedCells++;
				continue;
			}

			SetGridOccupancy(x, y, buildingType->cellSize);

			// rotate the building to face a random direction
			const int32_t orient = m_rand.Int32(4);

			// TODO: position model inside cells, add to building list
			vector2d buildingPos = {
				double(x) + buildingType->cellSize[0] / 2.0,
				double(y) + buildingType->cellSize[1] / 2.0,
			};

			vector3d pos = m_gridOrigin + incX * buildingPos.x + incZ * buildingPos.y;
			vector3d posNorm = pos.Normalized();
			double height = m_planet->GetTerrainHeight(posNorm);

			// don't place under planetary sea-level if the body has >10% water
			// TODO: need a better way to sample both height and biome data to determine if the cell is actually water
			// This will not properly handle elevated lakes or dry inland depressions below sea-level
			if (m_body->GetVolatileLiquid() > 0.1 && height < m_body->GetRadius()) {
				underwaterCells++;
				continue;
			}

			// Compute the terrain relative height by scaling the normal of the building's ideal position
			// This may introduce horizontal inaccuracy errors with sufficiently small planetary radii
			pos = posNorm * height;

			const float groundHeight = float(height - m_body->GetRadius());
			for (uint32_t cy = y; cy < std::min(y + buildingType->cellSize[1], m_citySize); cy++)
				for (uint32_t cx = x; cx < std::min(x + buildingType->cellSize[0], m_citySize); cx++)
					cellHeights[size_t(cy) * m_citySize + cx] = groundHeight;

			const CollMesh *cmesh = buildingType->model->GetCollisionMesh().Get();

			// FIXME: geoms need a userdata to tell gameplay code what we actually hit.
			// We don't want to create a separate Body for each instance of the buildings, so we
			// scam the code by pretending we're part of the host planet.
			Geom *geom = new Geom(cmesh->GetGeomTree(), orientcalc[orient], pos, GetPlanet());

			// add it to the list of buildings to render
			m_buildings.push_back({ vector3f(pos - stationOrigin), float(cmesh->GetRadius()), typeIndex, orient, geom });

		}
	}

	_genTimer.Stop();

	Log::Verbose("\tCityOnPlanet: generated {} buildings in {}ms ( {} skipped, {} discarded, {} occupied, {} underwater, {} avg rolls / building )",
		m_buildings.size(), _genTimer.milliseconds(), skippedCells, discardedCells, occupiedCells, underwaterCells, rarityRolls / double(m_buildings.size()));

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

	GenerateFixedLights(station, cityExtents, incX, incZ, cellHeights);

	// Release the memory once we're done generating and reset
	m_gridBitset.reset();
	m_gridPitch = 0;
	m_gridLen = 0;
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

void CityOnPlanet::GenerateFixedLights(const SpaceStation *station, uint32_t cityExtents, const vector3d &incX, const vector3d &incZ, const std::vector<float> &cellHeights)
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
	const double cityRadiusSqr = m_cityRadius * m_cityRadius;

	// street lamps in the empty cells beside buildings, where the streets are,
	// on the ground of the buildings beside them
	for (uint32_t y = 0; y < m_citySize; y++) {
		for (uint32_t x = 0; x < m_citySize; x++) {
			if (TestGridQuick(x, y))
				continue;

			// as the buildings are placed in Generate()
			const double radiusSqr = vector2d(
				(cityExtents - double(x) + 0.5) * CELLSIZE,
				(cityExtents - double(y) + 0.5) * CELLSIZE
			).LengthSqr();
			if (radiusSqr >= cityRadiusSqr)
				continue;

			float heightSum = 0.0f;
			int neighbours = 0;
			auto beside = [&](uint32_t nx, uint32_t ny) {
				const float h = cellHeights[size_t(ny) * m_citySize + nx];
				if (h != NO_CELL_HEIGHT) {
					heightSum += h;
					neighbours++;
				}
			};
			if (x > 0) beside(x - 1, y);
			if (x + 1 < m_citySize) beside(x + 1, y);
			if (y > 0) beside(x, y - 1);
			if (y + 1 < m_citySize) beside(x, y + 1);
			if (!neighbours)
				continue;

			const vector3d posNorm = (m_gridOrigin + incX * (x + 0.5) + incZ * (y + 0.5)).Normalized();
			const double height = m_body->GetRadius() + heightSum / neighbours;
			const vector3f ground((posNorm * height - stationOrigin) * m_stationOrient);
			m_fixedLights.push_back({ ground + vector3f(0.0f, LAMP_HEIGHT, 0.0f), LAMP_RADIUS, lampColour, LAMP_HEIGHT });
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
		const float hx = type.cellSize[0] * CELLSIZE * 0.5f;
		const float hz = type.cellSize[1] * CELLSIZE * 0.5f;
		const bool alongX = (building.rotation & 1) == 0;
		const vector3f offset = alongX ? vector3f(0.0f, 0.0f, hz + FACADE_OFFSET) : vector3f(hx + FACADE_OFFSET, 0.0f, 0.0f);
		const float length = alongX ? 2.0f * hx : 2.0f * hz;
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

#ifdef __BIG_ENDIAN__
#error "CityOnPlanet does not (yet) support big-endian architectures"
#endif

// Supports up to 32-cell wide buildings to ensure 8-byte aligned addressing in a single operation
static inline uint64_t CalcBitmaskForGrid(uint32_t x, uint32_t xsize)
{
	uint32_t maskOffset = x & CityOnPlanet::CELLMASK;
	// 64-bit shift: a 32-bit shift by xsize == CELLMAX would be undefined
	uint64_t bitmask = ((uint64_t(1) << xsize) - 1) << maskOffset;
	// note: need crossplatform bswap_64 here for big-endian compat
	// we should be able to swap the bitmask rather than the in-memory bytes and have the same effect

	return bitmask;
}

void CityOnPlanet::SetGridOccupancy(uint32_t x, uint32_t y, const uint8_t size[2])
{
	if (x + size[0] > m_citySize || y + size[1] > m_citySize)
		return; // footprint would be off-grid, prevent writing outside the bitset

	// City grid is stored as lsb-first bitset with one bit per grid cell;
	// bit 1 of byte 1 is grid 0, while bit 8 of byte 4 is grid 31
	// Support maximum size of 32 cells for a single building to enable fast 64-bit aligned operations

	uint8_t *dataPtr = m_gridBitset.get() + (x & ~CELLMASK) / 8;
	uint64_t bitmask = CalcBitmaskForGrid(x, size[0]);

	for (uint32_t idx = y; idx < y + size[1]; idx++) {
		*reinterpret_cast<uint64_t *>(dataPtr + idx * m_gridPitch) |= bitmask;
	}
}

bool CityOnPlanet::TestGridOccupancy(uint32_t x, uint32_t y, const uint8_t size[2])
{
	if (x + size[0] > m_citySize || y + size[1] > m_citySize)
		return true; // always occupied if footprint would be off-grid


	// City grid is stored as lsb-first bitset with one bit per grid cell;
	// bit 1 of byte 1 is grid 0, while bit 8 of byte 4 is grid 31
	// Support maximum size of 32 cells for a single building to enable fast 64-bit aligned operations
	uint8_t *dataPtr = m_gridBitset.get() + (x & ~CELLMASK) / 8;
	uint64_t bitmask = CalcBitmaskForGrid(x, size[0]);

	uint64_t test = 0;

	for (uint32_t idx = y; idx < y + size[1]; idx++) {
		test |= *reinterpret_cast<uint64_t *>(dataPtr + idx * m_gridPitch) & bitmask;
	}

	return test != 0;
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
