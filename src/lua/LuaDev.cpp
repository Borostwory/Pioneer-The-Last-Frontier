// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "LuaDev.h"
#include "Frame.h"
#include "Game.h"
#include "LuaConstants.h"
#include "LuaObject.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Ship.h"
#include "Space.h"
#include "WorldView.h"
#include "galaxy/Galaxy.h"
#include "galaxy/GalaxyCache.h"
#include "galaxy/GalaxyGenerator.h"
#include "galaxy/Sector.h"
#include <algorithm>
#include <sstream>

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

void LuaDev::Register()
{
	lua_State *l = Lua::manager->GetLuaState();

	LUA_DEBUG_START(l);

	static const luaL_Reg methods[] = {
		{ "GalaxyStats", l_dev_galaxy_stats },
		{ "SetCameraOffset", l_dev_set_camera_offset },
		{ "FindSystemsWithStarType", l_dev_find_systems_with_star_type },
		{ "TeleportNearBody", l_dev_teleport_near_body },
		{ 0, 0 }
	};

	lua_getfield(l, LUA_REGISTRYINDEX, "CoreImports");
	luaL_newlib(l, methods);
	lua_setfield(l, -2, "Dev");
	lua_pop(l, 1);

	LUA_DEBUG_END(l, 0);
}
