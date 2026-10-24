// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _CITYONPLANET_H
#define _CITYONPLANET_H

#include "FrameId.h"
#include "Random.h"
#include "JsonFwd.h"
#include "RefCounted.h"
#include "matrix4x4.h"
#include "vector3.h"

class CameraContext;
class Geom;
class Planet;
class SpaceStation;
class Frame;
class SystemPath;
class SystemBody;

struct Aabb;

namespace Graphics {
	class Renderer;
	class Material;
	class Texture;
	struct CityLightDataBlock;
} // namespace Graphics

namespace FileSystem {
	class FileInfo;
} // namespace FileSystem

namespace SceneGraph {
	class Model;
	class Animation;
} // namespace SceneGraph

class CityOnPlanet {
public:
	CityOnPlanet() = delete;
	CityOnPlanet(Planet *planet, SpaceStation *station, const Uint32 seed);
	virtual ~CityOnPlanet();

	void Render(Graphics::Renderer *r, const CameraContext *camera, const SpaceStation *station, const vector3d &viewCoords, const matrix4x4d &viewTransform);
	// draws the buildings inside lightBox into the current sun shadow map, see ModelBody::RenderShadowCaster
	void RenderShadowCaster(Graphics::Renderer *r, const matrix4x4d &frameToLight, const SpaceStation *station, const Aabb &lightBox);
	inline Planet *GetPlanet() const { return m_planet; }
	float GetClipRadius() const { return m_clipRadius; }

	// Night lights (city-lights.glsl): street lamps, building and spaceport
	// lights, in city space (the station's frame, metres, y up). Baked into a
	// map of the light reaching the ground and a grid listing the lights of
	// each cell, rebuilt when the detail level changes the buildings.
	struct Light {
		vector3f pos;	 // city space
		float radius;	 // reach, m
		vector3f colour; // brightness included
		float height;	 // above the ground, m
	};

	bool HasLights() const { return m_lightMap.Valid(); }
	// the city's part of the scene's light data: all but the view transform and the strength
	void GetLightData(Graphics::CityLightDataBlock &out) const;
	Graphics::Texture *GetLightMap() const { return m_lightMap.Get(); }
	Graphics::Texture *GetLightGrid() const { return m_lightGrid.Get(); }

	// The radius (m) of the city a ground station will have, without generating
	// it, for the planet's night lights seen from afar (city-glow.glsl).
	// False if it will have none.
	static bool GetCitySummary(const SystemBody *planet, const SpaceStation *station, double &radius);
	// share of a city's light shown by its own lights at a camera distance (m);
	// the rest is its glow from afar
	static double LightsFade(double distance);

	static void Init();
	static void Uninit();

	static void SetCityModelPatterns(const SystemPath &path);

	static constexpr double RADIUS = 5000.0;
	// size of a city grid cell
	static constexpr uint32_t CELLSIZE = 50;
	// maximum number of cells a single building may take up
	static constexpr uint32_t CELLMAX = 32;
	static constexpr uint32_t CELLMASK = CELLMAX - 1;

private:

	// Defines which "city sector" a building should optimally be placed in
	enum class SectorKind : uint32_t {
		None,		// no specific sector for a building
		Storage,	// warehouses and personal storage buildings
		Industry,	// industrial buildings with high pollution or power draw
		Monument,	// civic / recreational buildings
		Habitat,	// living spaces and office buildings
		Frontier,	// general buildings scattered at the edge of the city
	};

	// Building type definition
	struct BuildingType {
		uint8_t cellSize[2] = {1, 1};

		float rarityAirless = 0;
		float rarityAtmo = 0;

		SectorKind buildingKind = SectorKind::None;

		// lights at the foot of each facade at night
		vector3f lightColour;
		float lightIntensity = 0;

		SceneGraph::Model *model;
		SceneGraph::Animation *idleAnimation;
	};

	struct CityRadiusDef {
		double population = 0.0;	// billions of people
		float baseSize = 0.0;		// base size of the city (meters)
		float atmoSize = 0.0;		// additional size from atmosphere (meters)
		float randomSize = 0.0;		// random additional size (meters)
		float density = 0.0;        // density of buildings within the city
	};

	struct CityFlavourType {
		std::string flavourName;
		std::vector<CityRadiusDef> sizeDefs;
		std::vector<BuildingType> buildingTypes;
	};

private:

	void Generate(SpaceStation *station);
	static void CalcCityRadius(const CityFlavourType &cityType, const SystemBody *body, Random &rand, double &radius, double &density);

	// street lamps and spaceport floodlights, while the occupancy grid exists;
	// cellHeights: the ground above the planet's radius of the taken cells
	void GenerateFixedLights(const SpaceStation *station, uint32_t cityExtents, const vector3d &incX, const vector3d &incZ, const std::vector<float> &cellHeights);
	// all the lights of the enabled buildings, baked into the light map and grid
	void BuildLighting();
	void BakeLightMap(const std::vector<Light> &lights);
	void BuildLightGrid(const std::vector<Light> &lights);

	void SetGridOccupancy(uint32_t x, uint32_t y, const uint8_t size[2]);
	bool TestGridOccupancy(uint32_t x, uint32_t y, const uint8_t size[2]);

	// Quickly check if the given single grid cell is set.
	// It is expected as a precondition that the position is valid and within
	// the extents of the grid.
	inline bool TestGridQuick(uint32_t x, uint32_t y) const
	{
		// bitset is stored in lsb order with 8 cells per byte
		return m_gridBitset[y * m_gridPitch + x / 8] & (1 << (x & 7));
	}

	void AddStaticGeomsToCollisionSpace();
	void RemoveStaticGeomsFromCollisionSpace();

	void PrecalcInstanceTransforms(const matrix4x4d &stationOrient);

	// NOTE: position here is stored as a 32-bit float offset from the station
	// This should retain decent precision within the size of our cities (5-10km radius).
	struct BuildingInstance {
		vector3f pos;
		float clipRadius;
		Uint32 instIndex;
		int rotation; // 0-3
		Geom *geom;
	};

	const SystemBody *m_body;
	Planet *m_planet;

	double m_cityRadius;
	double m_cityDensity;
	uint32_t m_citySize;

	FrameId m_frame;
	Random m_rand;

	// Both of these vectors are sorted by building instance index
	std::vector<BuildingInstance> m_buildings;
	std::vector<BuildingInstance> m_enabledBuildings;

	std::vector<Uint32> m_buildingCounts;

	std::vector<matrix4x4f> m_instanceTransforms;

	// bitmask occupancy grid for quick population of the city
	std::unique_ptr<uint8_t[]> m_gridBitset;
	// width of a single grid row in bytes
	uint32_t m_gridPitch;
	uint32_t m_gridLen;

	int m_detailLevel;
	float m_clipRadius;
	vector3d m_realCentre;
	vector3d m_gridOrigin;

	CityFlavourType *m_cityType;

	// night lights: city space is the station's frame around its position
	matrix3x3d m_stationOrient;
	std::vector<Light> m_fixedLights; // street lamps, spaceport floodlights
	float m_lightRect[4] = {};		  // as CityLightDataBlock::mapRect
	float m_lightGridInfo[4] = {};	  // as CityLightDataBlock::grid
	RefCountedPtr<Graphics::Texture> m_lightMap;
	RefCountedPtr<Graphics::Texture> m_lightGrid;
	// the textures before the last rebuild, which this frame's draws may still use
	RefCountedPtr<Graphics::Texture> m_retiredLightMap;
	RefCountedPtr<Graphics::Texture> m_retiredLightGrid;

	// --------------------------------------------------------
	// statics
private:

	static std::vector<CityFlavourType> s_cityFlavours;

	static std::unique_ptr<Graphics::Material> s_debugMat;

	static void LoadCityFlavour(const FileSystem::FileInfo &file);
	static void LoadBuildingType(std::string_view key, const Json &buildingDef, BuildingType &out);
	static void GetModelSize(const Aabb &aabb, uint8_t size[2]);
};

#endif /* _CITYONPLANET_H */
