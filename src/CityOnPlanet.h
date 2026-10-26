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

#include <memory>

class Body;
class CameraContext;
class CityLayout;
class Geom;
class GeomTree;
class Planet;
class Ship;
class SpaceStation;
class Frame;
class SystemPath;
class SystemBody;

struct Aabb;

namespace Graphics {
	class Renderer;
	class Material;
	class MeshObject;
	class Texture;
	struct CityLightDataBlock;
	struct CityGroundDataBlock;
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

	// Streets and paved ground (city-ground.glsl), drawn on the terrain out to
	// GROUND_RANGE from the station: the city's roads (CityLayout.h) and the
	// lots of its cells packed into two textures
	bool HasGround() const { return m_roadTexture.Valid(); }
	// the city's part of the scene's ground data: all but the view transform
	void GetGroundData(Graphics::CityGroundDataBlock &out) const;
	Graphics::Texture *GetRoadTexture() const { return m_roadTexture.Get(); }
	Graphics::Texture *GetLotTexture() const { return m_lotTexture.Get(); }
	static constexpr double GROUND_RANGE = 40000.0;

	// The radius (m) of the city a ground station will have, without generating
	// it, for the planet's night lights seen from afar (city-glow.glsl).
	// False if it will have none.
	static bool GetCitySummary(const SystemBody *planet, const SpaceStation *station, double &radius);
	// share of a city's light shown by its own lights at a camera distance (m);
	// the rest is its glow from afar
	static double LightsFade(double distance);

	// Forcefield: geodesic domes where the air can't be breathed (see the
	// flavour's ForcefieldDef), where the city's plan puts them (one over the
	// city, several small ones, one over a crater: CityLayout.h), built with
	// the city, in city space. Each collides as a static geom of the station
	// flagged CollisionGeometry::FORCEFIELD: only the player's ship coming in
	// without landing clearance is stopped (unharmed), everything else passes,
	// and rays (weapons, the camera) ignore it. A dome opens round ships
	// crossing it and sends a wave out from where it stops one. Drawn after
	// everything solid and the clouds (Camera::Draw), farthest first.
	bool HasForcefield() const { return !m_forcefields.empty(); }
	// radius round the station holding the forcefields, m; 0 without one
	double GetForcefieldExtent() const { return m_ffExtent; }
	// whether the player is inside one of its domes
	bool IsPlayerInsideForcefield() const;
	// for a contact of the ship with it (Space.cpp): true if it stops the ship
	bool ForcefieldStops(SpaceStation *station, Ship *ship);
	// openings round crossing ships, impact waves
	void UpdateForcefield(float timeStep, SpaceStation *station);
	// queues it for RenderForcefields() if the camera can see it
	void QueueForcefield(const CameraContext *camera, const SpaceStation *station, const matrix4x4d &viewTransform) const;
	static void BeginFrame();
	static void RenderForcefields(Graphics::Renderer *r);

	static void Init();
	static void Uninit();

	static void SetCityModelPatterns(const SystemPath &path);

	static constexpr double RADIUS = 5000.0;
	// size of a city grid cell
	static constexpr uint32_t CELLSIZE = 50;
	// maximum number of cells a single building may take up
	static constexpr uint32_t CELLMAX = 32;

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

		// footprint (model space x, z) and height, from the collision mesh
		float minX = 0, maxX = 0, minZ = 0, maxZ = 0;
		float height = 0;

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

	// "forcefield" in a city definition file
	struct ForcefieldDef {
		// no forcefield where the air is breathable: an O2 atmosphere (as the
		// planet's description has it) of normal density, neither frozen nor
		// scorching on average
		float minOxidizing = 0.95f;
		float minDensity = 0.2f; // as SystemBody::GetVolatileGas
		float maxDensity = 2.0f;
		int minTemperature = 250; // K
		int maxTemperature = 320;
		// shape: radius at the ground over the city's (at least: it grows
		// until every building fits), height over radius, subdivisions of
		// each icosahedron edge
		float radiusScale = 1.12f;
		float heightRatio = 0.4f;
		int frequency = 5;
		vector3f colour = vector3f(1.0f);
		float opacity = 0.45f; // of the struts
		float strutWidth = 1.0f; // times their usual thickness
		float lethalSpeed = 100.0f; // m/s into it that destroys a ship it stops; 0 never
	};

	struct CityFlavourType {
		std::string flavourName;
		std::vector<CityRadiusDef> sizeDefs;
		std::vector<BuildingType> buildingTypes;
		ForcefieldDef forcefield;
	};

private:

	// lays out the city (CityLayout) and puts the buildings on it
	void Generate(SpaceStation *station);
	static void CalcCityRadius(const CityFlavourType &cityType, const SystemBody *body, Random &rand, double &radius, double &density);

	// street lamps along the roads and spaceport floodlights
	void GenerateFixedLights(const SpaceStation *station);
	// the road and lot textures for city-ground.glsl
	void BuildGround();
	// all the lights of the enabled buildings, baked into the light map and grid
	void BuildLighting();
	void BakeLightMap(const std::vector<Light> &lights);
	void BuildLightGrid(const std::vector<Light> &lights);

	static bool NeedsForcefield(const SystemBody *body, const ForcefieldDef &def);
	struct Forcefield;
	// the domes of the city's plan (one over everything if it has none)
	void BuildForcefields(SpaceStation *station);
	// one round (cx, cz) in city space, a wide at the ground at least,
	// grown to hold the buildings under it
	void BuildForcefield(SpaceStation *station, double cx, double cz, double a);
	// whether it lets the ship through as things are now
	bool ForcefieldPasses(const SpaceStation *station, const Forcefield &field, const Ship *ship) const;
	// the player from outside: with landing clearance or soon after leaving
	bool PlayerMayEnter(const SpaceStation *station, const Forcefield &field, const Ship *player) const;
	// a wave where it stopped the ship, and its sound
	void AddForcefieldImpact(Forcefield &field, const vector3d &point, const Ship *ship);
	// bounces the player off it when not allowed in; when they last went out
	void KeepPlayerOut(SpaceStation *station, Forcefield &field);
	// a ship stopped coming in this fast (m/s): destroyed if too fast
	void ForcefieldHit(SpaceStation *station, Ship *ship, double inwardSpeed);

	void AddStaticGeomsToCollisionSpace();
	void RemoveStaticGeomsFromCollisionSpace();

	void PrecalcInstanceTransforms(const matrix4x4d &stationOrient);

	// NOTE: position here is stored as a 32-bit float offset from the station
	// This should retain decent precision within the size of our cities (5-10km radius).
	struct BuildingInstance {
		vector3f pos;
		float clipRadius;
		Uint32 instIndex;
		float yaw; // turned about the station's up axis, as matrix3x3::RotateY
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

	// the plan: shape, districts, roads (CityLayout.h)
	std::unique_ptr<CityLayout> m_layout;
	// and for the terrain shaders (city-ground.glsl)
	RefCountedPtr<Graphics::Texture> m_roadTexture;
	RefCountedPtr<Graphics::Texture> m_lotTexture;
	float m_roadRect[4] = {};  // as CityGroundDataBlock::roadRect
	float m_lotRect[4] = {};   // as CityGroundDataBlock::lotRect
	float m_roadAtlas[2] = {}; // first index texel, first segment texel

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

	// forcefields, in city space: caps of spheres
	struct ForcefieldOpening {
		const Body *ship; // only compared, never dereferenced
		vector3d centre;  // where its path meets the field
		float radius;
		float open; // 0..1
		bool nearby;
	};
	struct ForcefieldImpact {
		vector3d point;
		float reach; // m
		float time;	 // s since
	};
	struct Forcefield {
		std::unique_ptr<Graphics::MeshObject> mesh;
		std::unique_ptr<Graphics::Material> backMaterial; // the far side, drawn first
		std::unique_ptr<Graphics::Material> frontMaterial;
		std::unique_ptr<Graphics::MeshObject> strutMesh; // struts and joints, between the two
		std::unique_ptr<Graphics::Material> strutMaterial;
		double strutRadius = 0.0;
		std::unique_ptr<GeomTree> geomTree;
		std::unique_ptr<Geom> geom;
		vector3d centre; // of its sphere, from the station
		double radius = 0.0;
		double bottom = 0.0; // height of its lowest point over the station
		double extent = 0.0; // radius round the station holding it
		bool playerInside = false;
		double playerLeftTime = -1e30; // game time
		std::vector<ForcefieldOpening> openings;
		std::vector<ForcefieldImpact> impacts;
	};
	std::vector<std::unique_ptr<Forcefield>> m_forcefields;
	vector3d m_ffUp;
	double m_ffExtent = 0.0;

	struct QueuedForcefield {
		const CityOnPlanet *city;
		const Forcefield *field;
		matrix4x4d modelView;
		double distance;
	};
	static std::vector<QueuedForcefield> s_forcefieldQueue;

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
