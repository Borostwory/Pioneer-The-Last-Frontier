// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "LuaDev.h"
#include "FrontierLog.h"
#include "Frame.h"
#include "Game.h"
#include "LuaConstants.h"
#include "LuaObject.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Planet.h"
#include "Ship.h"
#include "Space.h"
#include "SpaceStation.h"
#include "WorldView.h"
#include "galaxy/Galaxy.h"
#include "galaxy/GalaxyCache.h"
#include "galaxy/GalaxyGenerator.h"
#include "galaxy/Sector.h"
#include "galaxy/SystemBody.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "surface/Excursion.h"
#include "terrain/PlanetPhysics.h"
#include "terrain/Terrain.h"
#include "terrain/TerrainNoise.h"
#include "terrain/TerrainPhysicalHydro.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

/*
 * Lua commands used in development & debugging
 * Everything here is subject to rapid changes
 */

/*
 * Method: GalaxyStats
 *
 * Output to stdout and lua console statictics in the given cube. Start the game before using!
 *
 * using:
 * > GalaxyStats(centerX, centerY, centerZ, radius of cube, processors)
 *
 * example using from console:
 * > require 'Dev'.GalaxyStats(0,0,0,5, "CountSystemNames CountSystems CountPopulation")
 *
 * Parameters:
 *   centerX, centerY, centerZ - integer, coordinates of center of the cube, 0, 0, 0 = Sol
 *   radius - integer - distance in sectors from center to edge of the cube
 *   processors - string - Processor names separated by spaces
 *     available processor names:
 *       CountSystems
 *       CountSystemNames
 *       CountPopulation
 *       PlanetsGravity
 */

static int l_dev_galaxy_stats(lua_State *l)
{
	// base class to process data from star system
	class Processor {
	public:
		virtual void ProcessSystem(const Sector::System &system) = 0;
		virtual std::string Report() = 0;
	};

	// specific processors

	class : public Processor {
		uint32_t systems = 0;

	public:
		void ProcessSystem(const Sector::System &system) override
		{
			systems++;
		}
		std::string Report() override
		{
			return std::to_string(systems) + " systems total.\n";
		}
	} CountSystems;

	class : public Processor {
		std::map<std::string, int> names;

	public:
		void ProcessSystem(const Sector::System &system) override
		{
			// counting repeats of each name
			names[system.GetName()]++;
		}
		std::string Report() override
		{
			// sorting
			std::vector<std::pair<std::string, int>> sorted(names.begin(), names.end());
			std::sort(sorted.begin(), sorted.end(), [](std::pair<std::string, int> &n1, std::pair<std::string, int> &n2) {
				if (n1.second == n2.second)
					return n1.first < n2.first;
				else
					return n1.second > n2.second;
			});
			std::string s;
			s += "Top 10 system names:\n";
			for (unsigned i = 0; i < 10; i++)
				s += std::to_string(i + 1) + ". " + sorted[i].first + " : " + std::to_string(sorted[i].second) + "\n";
			s += "Bottom 10 system names:\n";
			for (unsigned i = sorted.size() - 10; i < sorted.size(); i++)
				s += std::to_string(i + 1) + ". " + sorted[i].first + " : " + std::to_string(sorted[i].second) + "\n";
			return s + "Total names: " + std::to_string(sorted.size()) + "\n";
		}
	} CountSystemNames;

	class : public Processor {
		uint32_t explored = 0;
		uint32_t inhabited = 0;
		double population;
		RefCountedPtr<Galaxy> galaxy = Pi::game->GetGalaxy();

	public:
		void ProcessSystem(const Sector::System &system) override
		{
			if (system.IsExplored()) explored++;
			double current = galaxy->GetStarSystem(SystemPath(system.sx, system.sy, system.sz, system.idx))->GetTotalPop().ToDouble();
			if (current > 0) {
				inhabited++;
				population += current;
			}
		}
		std::string Report() override
		{
			return std::to_string(explored) + " explored.\n" +
				std::to_string(inhabited) + " inhabited.\n" +
				"population: " + std::to_string(population) + " billion.\n";
		}
	} CountPopulation;

	class : public Processor {
		RefCountedPtr<Galaxy> galaxy = Pi::game->GetGalaxy();
		struct planet {
			std::string name;
			std::string systemname;
			double gravity;
			SystemPath path;
			planet(const std::string &n, const std::string &sn, double g, const SystemPath &p) :
				name(n), systemname(sn), gravity(g), path(p) {}
		};
		std::vector<planet> Planets;

	public:
		void ProcessSystem(const Sector::System &system) override
		{
			RefCountedPtr<StarSystem> starsystem = galaxy->GetStarSystem(system.GetPath());
			for (const auto &b : starsystem->GetBodies()) {
				auto children = b->GetChildren();
				if (std::find_if(children.cbegin(), children.cend(), [](const SystemBody *kid) {
						return kid->GetType() == SystemBody::TYPE_STARPORT_SURFACE;
					}) != children.cend())
					// the radius and the mass of the planet is returned in the radii and the mass of the earth
					// therefore the result is obtained in g
					Planets.emplace_back(b->GetName(), system.GetName(), b->GetMassAsFixed().ToDouble() / b->GetRadiusAsFixed().ToDouble() / b->GetRadiusAsFixed().ToDouble(), b->GetPath());
			}
		}
		std::string Report() override
		{
			std::sort(Planets.begin(), Planets.end(), [](const planet &p1, const planet &p2) {
				return p1.gravity > p2.gravity;
			});
			const double step = 0.1;
			const double from = std::floor(Planets.back().gravity / step) * step;
			const double to = std::floor(Planets.front().gravity / step) * step + step * 0.5; // this is the middle of the last range
			uint32_t top_amount = 20;														  // number of planets for the best / worst chart
			if (top_amount > Planets.size() / 2) top_amount = Planets.size() / 2;
			std::stringstream result;
			result.precision(3);
			result << "Total number of planets with star ports: " << Planets.size();
			result << "\nNumber of planets by gravity on the surface:";
			for (double i = from; i < to; i += step) {
				uint32_t amount = 0;
				for (const auto &p : Planets)
					if (p.gravity > i && p.gravity <= i + step) amount++;
				result << "\n"
					   << std::fixed << i << "g .. " << std::fixed << i + step << "g: " << amount / static_cast<float>(Planets.size()) * 100 << "% (" << amount << " planets)";
			}
			result << "\n------------------------------";
			result << "\nTop " << top_amount << " planets with max gravity:";
			for (uint32_t i = 0; i < top_amount; ++i) {
				result << "\n"
					   << i + 1 << ". " << Planets[i].name << " : " << Planets[i].gravity << "g ";
				result << "(" << Planets[i].systemname;
				result << " " << Planets[i].path.sectorX << ", " << Planets[i].path.sectorY << ", " << Planets[i].path.sectorZ << ")";
			}
			result << "\n------------------------------";
			result << "\nBottom " << top_amount << " planets with min gravity:";
			for (uint32_t i = Planets.size() - top_amount; i < Planets.size(); ++i) {
				result << "\n"
					   << i + 1 << ". " << Planets[i].name << " : " << Planets[i].gravity << "g ";
				result << "(" << Planets[i].systemname;
				result << " " << Planets[i].path.sectorX << ", " << Planets[i].path.sectorY << ", " << Planets[i].path.sectorZ << ")";
			}
			return result.str() + "\n";
		}
	} PlanetsGravity;

	// lua args
	int centerX = LuaPull<int>(l, 1);
	int centerY = LuaPull<int>(l, 2);
	int centerZ = LuaPull<int>(l, 3);
	int radius = LuaPull<int>(l, 4);
	std::string options_string = LuaPull<std::string>(l, 5);

	// parse options string to separate strings
	std::istringstream iss(options_string);
	std::vector<std::string> options((std::istream_iterator<std::string>(iss)),
		std::istream_iterator<std::string>());

	// map strings into processors
	std::map<const std::string, Processor *> processors = { { "CountSystemNames", &CountSystemNames },
		{ "CountPopulation", &CountPopulation },
		{ "CountSystems", &CountSystems },
		{ "PlanetsGravity", &PlanetsGravity } };
	std::vector<Processor *> P;
	for (auto &option : options)
		if (processors.find(option) != processors.end())
			P.push_back(processors[option]);

	// iterating all processors on all systems in given sectors
	RefCountedPtr<Galaxy> galaxy = Pi::game->GetGalaxy();
	for (int sx = centerX - radius; sx <= centerX + radius; ++sx) {
		for (int sy = centerY - radius; sy <= centerY + radius; ++sy) {
			for (int sz = centerZ - radius; sz <= centerZ + radius; ++sz) {
				SystemPath sp(sx, sy, sz);
				auto sec = galaxy->GetSector(sp);
				for (auto &system : sec->m_systems)
					for (auto &p : P)
						p->ProcessSystem(system);
			}
		}
	}

	// reporting from all processors
	std::string result = "";
	for (auto &p : P)
		result += p->Report();
	LuaPush<std::string>(l, result);
	return 1;
}

/*
 * Set current camera offset to vector,
 * (the offset will reset when switching cameras)
 *
 * Dev.SetCameraOffset(x, y, z)
 */
static int l_dev_set_camera_offset(lua_State *l)
{
	if (!Pi::game || !Pi::game->GetWorldView())
		return luaL_error(l, "Dev.SetCameraOffset only works when there is a game running");
	CameraController *cam = Pi::game->GetWorldView()->shipView->GetCameraController();
	const float x = luaL_checknumber(l, 1);
	const float y = luaL_checknumber(l, 2);
	const float z = luaL_checknumber(l, 3);
	cam->SetPosition(vector3d(x, y, z));
	return 0;
}

/*
 * Find the systems nearest to a system that have a star of the given type,
 * searching outwards in shells of sectors. Only the sector data is generated,
 * so this is cheap per system, but large radii still take a moment.
 *
 * > results = Dev.FindSystemsWithStarType(type, center, maxRadius, maxResults)
 *
 *   type - a <Constants.BodyType> star type, e.g. 'STAR_K_GIANT'
 *   center - <SystemPath> to search around
 *   maxRadius - furthest shell to search, in sectors
 *   maxResults - stop after the shell in which this many systems were found
 *
 * Returns an array of { path = <SystemPath>, name = string, distance = light years },
 * nearest first
 */
static int l_dev_find_systems_with_star_type(lua_State *l)
{
	if (!Pi::game)
		return luaL_error(l, "Dev.FindSystemsWithStarType only works when there is a game running");

	const auto type = static_cast<SystemBody::BodyType>(LuaConstants::GetConstantFromArg(l, "BodyType", 1));
	const SystemPath *center = LuaObject<SystemPath>::CheckFromLua(2);
	const int maxRadius = LuaPull<int>(l, 3);
	const size_t maxResults = size_t(std::max(LuaPull<int>(l, 4), 1));

	RefCountedPtr<Galaxy> galaxy = Pi::game->GetGalaxy();

	vector3f centerPos = Sector::SIZE * vector3f(center->sectorX + 0.5f, center->sectorY + 0.5f, center->sectorZ + 0.5f);
	if (center->HasValidSystem()) {
		RefCountedPtr<const Sector> sec = galaxy->GetSector(center->SectorOnly());
		if (center->systemIndex < sec->m_systems.size())
			centerPos = sec->m_systems[center->systemIndex].GetFullPosition();
	}

	struct Found {
		SystemPath path;
		std::string name;
		float distance;
	};
	std::vector<Found> found;

	for (int r = 0; r <= maxRadius && found.size() < maxResults; r++) {
		// the shell of sectors at Chebyshev distance r
		for (int dx = -r; dx <= r; dx++) {
			for (int dy = -r; dy <= r; dy++) {
				for (int dz = -r; dz <= r; dz++) {
					if (std::max({ std::abs(dx), std::abs(dy), std::abs(dz) }) != r)
						continue;

					const SystemPath secPath(center->sectorX + dx, center->sectorY + dy, center->sectorZ + dz);
					// generated without the cache, so a big search doesn't fill it
					RefCountedPtr<const Sector> sec = galaxy->GetGenerator()->Generate<Sector, SectorCache>(galaxy, secPath, nullptr);
					for (const Sector::System &sys : sec->m_systems) {
						for (unsigned i = 0; i < sys.GetNumStars(); i++) {
							if (sys.GetStarType(i) == type) {
								found.push_back({ sys.GetPath(), sys.GetName(), (sys.GetFullPosition() - centerPos).Length() });
								break;
							}
						}
					}
				}
			}
		}
	}

	std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.distance < b.distance; });
	if (found.size() > maxResults)
		found.resize(maxResults);

	lua_createtable(l, int(found.size()), 0);
	for (size_t i = 0; i < found.size(); i++) {
		lua_createtable(l, 0, 3);
		LuaObject<SystemPath>::PushToLua(found[i].path);
		lua_setfield(l, -2, "path");
		lua_pushstring(l, found[i].name.c_str());
		lua_setfield(l, -2, "name");
		lua_pushnumber(l, found[i].distance);
		lua_setfield(l, -2, "distance");
		lua_rawseti(l, -2, int(i + 1));
	}
	return 1;
}

/*
 * Move a flying ship to a body in the current system, a number of the body's
 * radii from its centre, at rest relative to it and facing it. Bodies that
 * orbit a star (planets, but also companion stars and brown dwarfs) are
 * approached from that star's side, so it doesn't sit behind them; the
 * system's main star from the ship's side.
 *
 * > Dev.TeleportNearBody(ship, body, radii)
 */
static int l_dev_teleport_near_body(lua_State *l)
{
	if (!Pi::game || !Pi::game->GetSpace())
		return luaL_error(l, "Dev.TeleportNearBody only works when there is a game running");

	Ship *ship = LuaObject<Ship>::CheckFromLua(1);
	Body *body = LuaObject<Body>::CheckFromLua(2);
	const double radii = std::max(luaL_checknumber(l, 3), 1.1);

	if (ship->GetFlightState() != Ship::FLYING)
		return luaL_error(l, "Dev.TeleportNearBody: the ship must be flying");

	const SystemBody *sbody = body->GetSystemBody();
	const double radius = std::max(sbody ? sbody->GetRadius() : 0.0, body->GetPhysRadius());
	const double distance = radius * radii;

	const FrameId frameId = Frame::GetFrame(body->GetFrame())->GetNonRotFrame();
	const vector3d bodyPos = body->GetPositionRelTo(frameId);

	// the side to approach from: towards the body's star, or towards the ship for stars
	vector3d towards = ship->GetPositionRelTo(frameId) - bodyPos;
	const SystemBody *star = sbody ? sbody->GetParent() : nullptr;
	while (star && star->GetSuperType() != SystemBody::SUPERTYPE_STAR)
		star = star->GetParent();
	if (star) {
		const SystemPath starPath = star->GetPath();
		if (Body *starBody = Pi::game->GetSpace()->FindBodyForPath(&starPath))
			towards = starBody->GetPositionRelTo(frameId) - bodyPos;
	}
	if (towards.LengthSqr() < 1e-6)
		towards = vector3d(0.0, 0.0, 1.0);
	towards = towards.Normalized();

	// turn 25 degrees off that line, so planets show a sliver of their night side
	vector3d side = towards.Cross(vector3d(0.0, 1.0, 0.0));
	if (side.LengthSqr() < 1e-6)
		side = vector3d(1.0, 0.0, 0.0);
	side = side.Normalized();
	const double offAngle = DEG2RAD(25.0);
	const vector3d dir = (towards * cos(offAngle) + side * sin(offAngle)).Normalized();

	// face the body: ships look along -z
	const vector3d z = dir;
	vector3d x = vector3d(0.0, 1.0, 0.0).Cross(z);
	if (x.LengthSqr() < 1e-6)
		x = vector3d(1.0, 0.0, 0.0);
	x = x.Normalized();
	const vector3d y = z.Cross(x);

	ship->AIClearInstructions();
	ship->ClearThrusterState();
	ship->SetFrame(frameId);
	ship->SetPosition(bodyPos + dir * distance);
	ship->SetVelocity(body->GetVelocityRelTo(frameId));
	ship->SetAngVelocity(vector3d(0.0));
	ship->SetOrient(matrix3x3d::FromVectors(x, y, z));
	return 0;
}

/*
 * Put a flying ship at rest near a station, in the station's own axes: height
 * metres up its axis (above the ground for a ground station) and side metres
 * out along its x axis, level with the station and with no AI orders. For
 * watching ships come and go.
 *
 * > Dev.PlaceNearStation(ship, station, height, side)
 */
static int l_dev_place_near_station(lua_State *l)
{
	if (!Pi::game || !Pi::game->GetSpace())
		return luaL_error(l, "Dev.PlaceNearStation only works when there is a game running");

	Ship *ship = LuaObject<Ship>::CheckFromLua(1);
	SpaceStation *station = LuaObject<SpaceStation>::CheckFromLua(2);
	const double height = luaL_checknumber(l, 3);
	const double side = luaL_optnumber(l, 4, 0.0);

	if (ship->GetFlightState() != Ship::FLYING)
		return luaL_error(l, "Dev.PlaceNearStation: the ship must be flying");

	const matrix3x3d &orient = station->GetOrient();
	ship->AIClearInstructions();
	ship->ClearThrusterState();
	ship->SetFrame(station->GetFrame());
	ship->SetPosition(station->GetPosition() + orient * vector3d(side, height, 0.0));
	// level with the station: top up its axis
	ship->SetOrient(orient);
	ship->SetVelocity(vector3d(0.0));
	ship->SetAngVelocity(vector3d(0.0));
	return 0;
}

/*
 * Put a ship on a planet or moon of the current system at a latitude and
 * longitude (radians, as Body:GetGroundPosition gives them): landed if
 * altitude is 0 or less, else that many metres above the terrain, at rest
 * relative to the ground, level and facing north. The ship must be flying
 * or landed, with the player aboard if it is the player's.
 *
 * > Dev.PlaceOnSurface(ship, body, latitude, longitude, altitude)
 */
static int l_dev_place_on_surface(lua_State *l)
{
	if (!Pi::game || !Pi::game->GetSpace())
		return luaL_error(l, "Dev.PlaceOnSurface only works when there is a game running");

	Ship *ship = LuaObject<Ship>::CheckFromLua(1);
	Body *body = LuaObject<Body>::CheckFromLua(2);
	const double latitude = luaL_checknumber(l, 3);
	const double longitude = luaL_checknumber(l, 4);
	const double altitude = luaL_checknumber(l, 5);

	const Ship::FlightState state = ship->GetFlightState();
	if (state != Ship::FLYING && state != Ship::LANDED)
		return luaL_error(l, "Dev.PlaceOnSurface: the ship must be flying or landed");
	if (ship->IsType(ObjectType::PLAYER) && Pi::game->GetExcursion()->IsOffShip())
		return luaL_error(l, "Dev.PlaceOnSurface: get back aboard first");
	if (!body->IsType(ObjectType::PLANET))
		return luaL_error(l, "Dev.PlaceOnSurface: %s is not a planet or moon", body->GetLabel().c_str());
	Planet *planet = static_cast<Planet *>(body);
	if (altitude <= 0.0 && planet->GetSystemBody()->GetSuperType() != SystemBody::SUPERTYPE_ROCKY_PLANET)
		return luaL_error(l, "Dev.PlaceOnSurface: %s has no surface to land on", body->GetLabel().c_str());

	ship->AIClearInstructions();
	if (state == Ship::LANDED)
		ship->OnTakeoff(Frame::GetFrame(ship->GetFrame())->GetBody());

	if (altitude <= 0.0) {
		ship->SetLandedOn(planet, float(latitude), float(longitude));
		return 0;
	}

	// as SetLandedOn
	const vector3d up(cos(latitude) * sin(longitude), sin(latitude), cos(latitude) * cos(longitude));
	// level, facing north (ships look along -z); any way at the poles
	vector3d north = vector3d(0.0, 1.0, 0.0) - up * up.y;
	if (north.LengthSqr() < 1e-12)
		north = vector3d(0.0, 0.0, 1.0) - up * up.z;
	north = north.Normalized();
	const vector3d right = up.Cross(-north).Normalized();

	ship->SetFrame(Frame::GetFrame(planet->GetFrame())->GetRotFrame());
	ship->SetFlightState(Ship::FLYING);
	ship->ClearThrusterState();
	ship->SetPosition(up * (planet->GetTerrainHeight(up) + altitude));
	ship->SetOrient(matrix3x3d::FromVectors(right, up));
	ship->SetVelocity(vector3d(0.0));
	ship->SetAngVelocity(vector3d(0.0));
	return 0;
}

/*
 * The places of a kind of terrain feature on a planet or moon, as an array of
 * { lat, lon, size } (radians, metres). Empty if its terrain has none, as with
 * the classic generator. Kinds: river, fork, mouth, lake, coast, peak.
 *
 * > Dev.GetTerrainFeatures(body, kind, latitude, longitude)
 *
 * Latitude and longitude (radians): the place that kinds looked for only
 * around a place look around.
 */
static int l_dev_get_terrain_features(lua_State *l)
{
	Body *body = LuaObject<Body>::CheckFromLua(1);
	const std::string kind = luaL_checkstring(l, 2);
	// (before the table goes on the stack, where a missing argument would find it)
	const double latitude = luaL_optnumber(l, 3, 0.0);
	const double longitude = luaL_optnumber(l, 4, 0.0);
	const vector3d around(cos(latitude) * sin(longitude), sin(latitude), cos(latitude) * cos(longitude));
	lua_newtable(l);
	if (!body->IsType(ObjectType::PLANET))
		return 1;
	const Terrain *terrain = static_cast<Planet *>(body)->GetTerrain();
	if (!terrain)
		return 1;
	int index = 0;
	for (const Terrain::Feature &feature : terrain->FindFeatures(kind, around)) {
		lua_newtable(l);
		// as Dev.PlaceOnSurface reads them
		lua_pushnumber(l, asin(std::max(-1.0, std::min(1.0, feature.position.y))));
		lua_setfield(l, -2, "lat");
		lua_pushnumber(l, atan2(feature.position.x, feature.position.z));
		lua_setfield(l, -2, "lon");
		lua_pushnumber(l, feature.size);
		lua_setfield(l, -2, "size");
		lua_rawseti(l, -2, ++index);
	}
	return 1;
}

static RefCountedPtr<Graphics::Texture> s_terrainMap;

/*
 * Makes a map of a planet or moon, latitude and longitude on an image twice as
 * wide as it is high, with north at the top and longitude -180 at the left, and
 * returns it for ui.addImage. It holds up the game for a moment. There is one
 * map at a time, which lasts until the next or Dev.ReleaseTerrainMap.
 *
 * > Dev.MakeTerrainMap(body)
 */
static int l_dev_make_terrain_map(lua_State *l)
{
	Body *body = LuaObject<Body>::CheckFromLua(1);
	if (!body->IsType(ObjectType::PLANET))
		return luaL_error(l, "Dev.MakeTerrainMap: %s is not a planet or moon", body->GetLabel().c_str());
	const Terrain *terrain = static_cast<Planet *>(body)->GetTerrain();
	if (!terrain)
		return luaL_error(l, "Dev.MakeTerrainMap: %s has no terrain", body->GetLabel().c_str());

	const auto start = std::chrono::steady_clock::now();
	constexpr int W = 2048, H = 1024;
	const double spacing = 2.0 * M_PI / W;
	const double maxFrequency = 0.5 / spacing;
	std::vector<vector3d> dirs(size_t(W) * H);
	std::vector<double> heights(dirs.size());
	std::vector<unsigned char> pixels(dirs.size() * 4);
	ParallelRows(H, [&](const int r) {
		TerrainNoise::ScopedBandLimit bandLimit(maxFrequency);
		const double latitude = M_PI / 2.0 - (r + 0.5) / H * M_PI;
		for (int c = 0; c < W; c++) {
			const double longitude = (c + 0.5) / W * 2.0 * M_PI - M_PI;
			// as Dev.PlaceOnSurface reads them
			dirs[size_t(r) * W + c] = vector3d(cos(latitude) * sin(longitude), sin(latitude), cos(latitude) * cos(longitude));
		}
		terrain->GetHeights(&dirs[size_t(r) * W], &heights[size_t(r) * W], W);
	});
	// relief as the planet lab shows it, scaled by this body's own steepest slopes
	std::vector<float> slopes;
	slopes.reserve(dirs.size() / 16);
	for (int r = 0; r < H; r += 4) {
		const int north = std::max(r - 1, 0), south = std::min(r + 1, H - 1);
		const double across = 2.0 * spacing * std::max(cos(M_PI / 2.0 - (r + 0.5) / H * M_PI), 0.05);
		const double along = double(south - north) * spacing;
		for (int c = 0; c < W; c += 4) {
			const double slopeEast = (heights[size_t(r) * W + (c + 1) % W] - heights[size_t(r) * W + (c + W - 1) % W]) / across;
			const double slopeNorth = (heights[size_t(north) * W + c] - heights[size_t(south) * W + c]) / along;
			slopes.push_back(float(sqrt(slopeEast * slopeEast + slopeNorth * slopeNorth)));
		}
	}
	const size_t steepAt = size_t(0.98 * double(slopes.size() - 1));
	std::nth_element(slopes.begin(), slopes.begin() + steepAt, slopes.end());
	const double steep = std::max(double(slopes[steepAt]), 1e-5);
	ParallelRows(H, [&](const int r) {
		TerrainNoise::ScopedBandLimit bandLimit(maxFrequency);
		const int north = std::max(r - 1, 0), south = std::min(r + 1, H - 1);
		const double across = 2.0 * spacing * std::max(cos(M_PI / 2.0 - (r + 0.5) / H * M_PI), 0.05);
		const double along = double(south - north) * spacing;
		for (int c = 0; c < W; c++) {
			const size_t i = size_t(r) * W + c;
			const size_t e = size_t(r) * W + (c + 1) % W, w = size_t(r) * W + (c + W - 1) % W;
			const size_t n = size_t(north) * W + c, s = size_t(south) * W + c;
			const auto position = [&](const size_t k) { return dirs[k] * (1.0 + heights[k]); };
			vector3d normal = (position(e) - position(w)).Cross(position(n) - position(s)).Normalized();
			if (normal.Dot(dirs[i]) < 0.0)
				normal = -normal;
			const vector3d colour = terrain->GetColor(dirs[i], heights[i], normal);
			// lit from the north-west, so that the relief shows
			const double slopeEast = (heights[e] - heights[w]) / across, slopeNorth = (heights[n] - heights[s]) / along;
			const double shade = std::max(std::min(0.55 + (-slopeEast + slopeNorth) * 0.7 / steep * 0.4, 1.0), 0.0);
			const double light = 0.35 + 1.25 * shade;
			// (the terrain's colours are in linear light, as the shaders take them)
			const auto toScreen = [light](const double v) { return std::pow(std::max(std::min(v * light, 1.0), 0.0), 1.0 / 2.2) * 255.0; };
			unsigned char *pixel = &pixels[i * 4];
			pixel[0] = (unsigned char)toScreen(colour.x);
			pixel[1] = (unsigned char)toScreen(colour.y);
			pixel[2] = (unsigned char)toScreen(colour.z);
			pixel[3] = 255;
		}
	});

	const vector3f size(float(W), float(H), 0.0f);
	const Graphics::TextureDescriptor desc(Graphics::TEXTURE_RGBA_8888, size, Graphics::LINEAR_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
	s_terrainMap.Reset(Pi::renderer->CreateTexture(desc));
	s_terrainMap->Update(pixels.data(), size, Graphics::TEXTURE_RGBA_8888);

	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	char note[160];
	snprintf(note, sizeof(note), "made a map of %s in %.2f s", body->GetLabel().c_str(), seconds);
	FrontierLog::Note(note);
	lua_pushlightuserdata(l, s_terrainMap.Get());
	return 1;
}

/*
 * > Dev.ReleaseTerrainMap()
 */
static int l_dev_release_terrain_map(lua_State *l)
{
	s_terrainMap.Reset();
	return 0;
}

/*
 * Turns a planet or moon so that it's noon at a latitude and longitude
 * (radians): the sun is as high there as it gets. The game's clock and the rest
 * of the system stay as they are. What is on the body turns with it.
 *
 * > Dev.SetNoonAt(body, latitude, longitude)
 */
static int l_dev_set_noon_at(lua_State *l)
{
	Body *body = LuaObject<Body>::CheckFromLua(1);
	const double longitude = luaL_checknumber(l, 3);
	if (!body->IsType(ObjectType::PLANET))
		return luaL_error(l, "Dev.SetNoonAt: %s is not a planet or moon", body->GetLabel().c_str());
	Frame *rot = Frame::GetFrame(Frame::GetFrame(body->GetFrame())->GetRotFrame());
	if (!rot->IsRotFrame())
		return luaL_error(l, "Dev.SetNoonAt: %s doesn't turn", body->GetLabel().c_str());
	// the nearest star
	Body *star = nullptr;
	double nearest = 0.0;
	for (Body *b : Pi::game->GetSpace()->GetBodies()) {
		if (!b->IsType(ObjectType::STAR))
			continue;
		const double distance = b->GetPositionRelTo(body).LengthSqr();
		if (!star || distance < nearest) {
			star = b;
			nearest = distance;
		}
	}
	if (!star)
		return luaL_error(l, "Dev.SetNoonAt: there is no star");
	const vector3d sun = star->GetPositionRelTo(rot->GetId());
	const double turn = atan2(sun.x, sun.z) - longitude;
	// (RotateY is backwards)
	rot->SetOrient(rot->GetOrient() * matrix3x3d::RotateY(-turn), Pi::game->GetTime());
	return 0;
}

/*
 * What the terrain has at a latitude and longitude (radians), as text; an
 * empty string if its generator can't say.
 *
 * > Dev.DescribeTerrain(body, latitude, longitude)
 */
static int l_dev_describe_terrain(lua_State *l)
{
	Body *body = LuaObject<Body>::CheckFromLua(1);
	const double latitude = luaL_checknumber(l, 2);
	const double longitude = luaL_checknumber(l, 3);
	std::string text;
	if (body->IsType(ObjectType::PLANET)) {
		if (const Terrain *terrain = static_cast<Planet *>(body)->GetTerrain())
			text = terrain->DescribePoint(vector3d(cos(latitude) * sin(longitude), sin(latitude), cos(latitude) * cos(longitude)));
	}
	lua_pushstring(l, text.c_str());
	return 1;
}

/*
 * Adds a line to frontier_log.txt.
 *
 * > Dev.LogNote(text)
 */
static int l_dev_log_note(lua_State *l)
{
	FrontierLog::Note(luaL_checkstring(l, 1));
	return 0;
}

/*
 * What planetgen makes of a rocky planet's, moon's or asteroid's surface, from its system data alone: the same
 * PlanetPhysics steps TerrainPhysical starts from (the "Became:" line of the log), without building the terrain,
 * so it's quick enough for thousands of bodies. Any system's body, not only the current system's.
 *
 * > surface = Dev.PlanetSurface(systemBody)
 *
 * Returns nil for bodies planetgen doesn't make (stars, gas giants, stations), else a table:
 *   sea         0..1 of the surface under a liquid sea
 *   seaState    "none", "liquid water", "liquid hydrocarbon", "frozen" or "too hot"
 *   frozen      0..1, the data's ocean frozen solid
 *   ice, air, rain, lava, lavaPlanet, melted, molten    0..1
 *   ground      the ground's layer: "SOIL", "DUST", "ICE", "DIRTY_ICE", "REGOLITH", "BASALT", "LAVA", "SALT"
 *   analogue    what the interior is like: "Earth", "Mars", "Callisto", ...
 *   iceCrust    true when the crust is ice
 *   temperature, starTemperature    K
 *   plants      { r, g, b }, the colour its plants would have (by its star)
 */
static int l_dev_planet_surface(lua_State *l)
{
	const SystemBody *sbody = LuaObject<SystemBody>::CheckFromLua(1);
	if (sbody->GetType() != SystemBody::TYPE_PLANET_TERRESTRIAL && sbody->GetType() != SystemBody::TYPE_PLANET_ASTEROID) {
		lua_pushnil(l);
		return 1;
	}

	const PlanetPhysics::Core core = PlanetPhysics::CoreOf(sbody);
	const PlanetPhysics::SurfaceInputs inputs = PlanetPhysics::SurfaceInputsOf(sbody);
	const PlanetPhysics::Interior interior = PlanetPhysics::MakeInterior(core);
	const PlanetPhysics::Surface st = PlanetPhysics::MakeSurface(core, inputs, interior);
	const vector3d plants = PlanetPhysics::PlantColour(core);

	lua_newtable(l);
	const auto number = [l](const char *key, const double value) {
		lua_pushnumber(l, value);
		lua_setfield(l, -2, key);
	};
	const auto text = [l](const char *key, const char *value) {
		lua_pushstring(l, value);
		lua_setfield(l, -2, key);
	};
	number("sea", st.sea);
	text("seaState", st.seaState);
	number("frozen", st.frozen);
	number("ice", st.ice);
	number("air", st.air);
	number("rain", st.rain);
	number("lava", st.lava);
	number("lavaPlanet", st.lavaPlanet);
	number("melted", st.melted);
	number("molten", st.molten);
	text("ground", PlanetPhysics::LayerName(st.ground));
	text("analogue", interior.analogue);
	lua_pushboolean(l, interior.iceCrust);
	lua_setfield(l, -2, "iceCrust");
	number("temperature", core.temperature);
	number("starTemperature", core.starTemperature);

	lua_newtable(l);
	number("r", plants.x);
	number("g", plants.y);
	number("b", plants.z);
	lua_setfield(l, -2, "plants");
	return 1;
}

void LuaDev::Register()
{
	lua_State *l = Lua::manager->GetLuaState();

	LUA_DEBUG_START(l);

	static const luaL_Reg methods[] = {
		{ "GalaxyStats", l_dev_galaxy_stats },
		{ "SetCameraOffset", l_dev_set_camera_offset },
		{ "FindSystemsWithStarType", l_dev_find_systems_with_star_type },
		{ "TeleportNearBody", l_dev_teleport_near_body },
		{ "PlaceOnSurface", l_dev_place_on_surface },
		{ "PlaceNearStation", l_dev_place_near_station },
		{ "GetTerrainFeatures", l_dev_get_terrain_features },
		{ "DescribeTerrain", l_dev_describe_terrain },
		{ "MakeTerrainMap", l_dev_make_terrain_map },
		{ "ReleaseTerrainMap", l_dev_release_terrain_map },
		{ "SetNoonAt", l_dev_set_noon_at },
		{ "LogNote", l_dev_log_note },
		{ "PlanetSurface", l_dev_planet_surface },
		{ 0, 0 }
	};

	lua_getfield(l, LUA_REGISTRYINDEX, "CoreImports");
	luaL_newlib(l, methods);
	lua_setfield(l, -2, "Dev");
	lua_pop(l, 1);

	LUA_DEBUG_END(l, 0);
}
