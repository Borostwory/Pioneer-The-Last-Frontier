// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "LuaGame.h"
#include "DateTime.h"
#include "DeathView.h"
#include "FileSystem.h"
#include "Game.h"
#include "GameConfig.h"
#include "GameSaveError.h"
#include "Json.h"
#include "Lang.h"
#include "LuaEvent.h"
#include "LuaObject.h"
#include "LuaTable.h"
#include "LuaUtils.h"
#include "Pi.h"
#include "Player.h"
#include "SaveGameManager.h"
#include "SectorView.h"
#include "Space.h"
#include "StringF.h"
#include "SystemView.h"
#include "WorldView.h"
#include "core/GZipFormat.h"
#include "galaxy/Galaxy.h"
#include "pigui/LuaPiGui.h"
#include "surface/Excursion.h"
#include "surface/Rover.h"
#include "surface/Walker.h"

/*
 * Interface: Game
 *
 * A global table that exposes a number of essential values relevant to the
 * current game.
 *
 */

/*
 * Function: StartGame
 *
 * Start a new game.
 *
 * > Game.StartGame(system_path, start_time, ship_type)
 *
 * Parameters:
 *
 *   system_path - A SystemBody to start at. If this is a starport, the player
 *                 will begin docked here; otherwise the player will begin in
 *                 orbit around the specified body.
 *
 *   start_time - Time to start at in seconds from the Pioneer epoch
 *                (i.e. from 3200-01-01 00:00 UTC).
 *
 *   ship_type - string, ship id, i.e. 'sinonatrix'
 *
 */
static int l_game_start_game(lua_State *l)
{
	if (Pi::game) {
		return luaL_error(l, "can't start a new game while a game is already running");
	}

	const auto path = LuaPull<SystemPath>(l, 1);
	const auto time = LuaPull<double>(l, 2);
	const auto shipType = LuaPull<const char *>(l, 3);

	Pi::StartGame(new Game(path, time, shipType));
	return 0;
}

/* Marshall the game info into a LuaTable and send it as an event to Lua.
 * Callback from the savegame_stats job when the job is finished. This function
 * will run in the main thread. */
static void onSaveGameStatsJobFinished(std::string_view filename, const Json &rootNode)
{
	PROFILE_SCOPED()
	auto ls = Lua::manager->GetLuaState();
	ScopedTable t(ls);
	t.Set("filename", filename);

	try {
		t.Set("time", rootNode["time"].get<double>());

		// if this is a newer saved game, show the embedded info
		if (rootNode["game_info"].is_object()) {
			const Json &gameInfo = rootNode["game_info"];
			t.Set("system", gameInfo["system"].get<std::string>());
			t.Set("ship", gameInfo["ship"].get<std::string>());
			t.Set("credits", gameInfo["credits"].get<float>());
			t.Set("flight_state", gameInfo["flight_state"].get<std::string>());
			if (gameInfo.count("docked_at")) {
				t.Set("docked_at", gameInfo["docked_at"].get<std::string>());
			}
			if (gameInfo.count("shipHull")) {
				t.Set("shipHull", gameInfo["shipHull"].get<std::string>());
			}
			if (gameInfo.count("shipName")) {
				t.Set("shipName", gameInfo["shipName"].get<std::string>());
			}
			if (gameInfo.count("duration")) {
				t.Set("duration", gameInfo["duration"].get<double>());
			}
			if (gameInfo.count("character")) {
				t.Set("character", gameInfo["character"].get<std::string>());
			}
		} else {
			// this is an older saved game...try to show something useful
			const Json &shipNode = rootNode["space"]["bodies"][rootNode["player"].get<int>() - 1];
			t.Set("frame", rootNode["space"]["bodies"][shipNode["body"]["index_for_frame"].get<int>() - 1]["body"]["label"].get<std::string>());
			t.Set("ship", shipNode["model_body"]["model_name"].get<std::string>());
		}

		int saveVersion = rootNode["version"].get<int>();
		t.Set("compatible", saveVersion == SaveGameManager::CurrentSaveVersion());

	} catch (const Json::type_error &) {
		t.Set("compatible", false);
	} catch (const Json::out_of_range &) {
		t.Set("compatible", false);
	}

	LuaEvent::Queue("onSaveGameStats", LuaTable(t));
	LuaEvent::Emit();
}

/*
 * Function: SaveGameStats
 *
 * Start a Job to read the SaveGameStats for a particular save game
 *
 * > Game.SaveGameStats(filename)
 *
 * Parameters:
 *
 *   filename - The filename of the save game to retrieve stats for.
 *              Stats will be loaded from the 'savefiles' directory in the user's game directory.
 *
 * Modified:
 *
 *   2024-10-11 - the function no longer returns the Stats directly, but starts
 *                a Job. The Lua caller is responsible for registering an
 *                "onSaveGameStats" event handler to process the data.
 */
static int l_game_savegame_stats(lua_State *l)
{
	const std::string filename = LuaPull<std::string>(l, 1);
	Lua::manager->ScheduleJob(SaveGameManager::LoadGameToJsonAsync(filename, onSaveGameStatsJobFinished));
	return 0;
}


/*
 * Function: CurrentSaveVersion
 *
 * Returns the current version of the game save file format.
 *
 * > Game.CurrentSaveVersion()
 *
 * Return:
 *
 *   number
 *
 */
static int l_game_current_save_version(lua_State *l)
{
	LuaPush(l, SaveGameManager::CurrentSaveVersion());
	return 1;
}

/*
 * Function: LoadGame
 *
 * Load a saved game.
 *
 * > Game.LoadGame(filename)
 *
 * Parameters:
 *
 *   filename - Filename to load. It will be loaded from the 'savefiles'
 *              directory in the user's game directory.
 */
static int l_game_load_game(lua_State *l)
{
	if (Pi::game) {
		return luaL_error(l, "can't load a game while a game is already running");
	}

	const std::string filename(luaL_checkstring(l, 1));

	try {
		Pi::StartGame(SaveGameManager::LoadGame(filename));
	} catch (const SavedGameCorruptException &) {
		return luaL_error(l, Lang::GAME_LOAD_CORRUPT);
	} catch (const SavedGameWrongVersionException &) {
		return luaL_error(l, Lang::GAME_LOAD_WRONG_VERSION);
	} catch (const CouldNotOpenFileException &) {
		const std::string msg = stringf(Lang::GAME_LOAD_CANNOT_OPEN, formatarg("filename", filename));
		return luaL_error(l, msg.c_str());
	}

	return 0;
}

/*
 * Function: CanLoadGame
 *
 * Does file exist for loading.
 *
 * > Game.CanLoadGame(filename)
 *
 * Parameters:
 *
 *   filename - Filename to find.
 *
 * Return:
 *
 *   bool - can the filename be found to load
 */
static int l_game_can_load_game(lua_State *l)
{
	const std::string filename(luaL_checkstring(l, 1));

	const bool success = SaveGameManager::CanLoadGame(filename);
	lua_pushboolean(l, success);

	return 1;
}

/*
 * Function: SaveGame
 *
 * Save the current game.
 *
 * > path = Game.SaveGame(filename)
 *
 * Parameters:
 *
 *   filename - Filename to save to. The file will be placed the 'savefiles'
 *              directory in the user's game directory.
 *
 * Return:
 *
 *   path - the full path to the saved file (so it can be displayed)
 */
static int l_game_save_game(lua_State *l)
{
	if (!Pi::game) {
		return luaL_error(l, "can't save when no game is running");
	}

	const std::string filename(luaL_checkstring(l, 1));
	std::string path;

	try {
		path = FileSystem::JoinPathBelow(SaveGameManager::GetSaveGameDirectory(), filename);
		SaveGameManager::SaveGame(filename, Pi::game);
		lua_pushlstring(l, path.c_str(), path.size());
		return 1;
	} catch (const CannotSaveInHyperspace &) {
		return luaL_error(l, "%s", Lang::CANT_SAVE_IN_HYPERSPACE);
	} catch (const CannotSaveDeadPlayer &) {
		return luaL_error(l, "%s", Lang::CANT_SAVE_DEAD_PLAYER);
	} catch (const CouldNotOpenFileException &) {
		const std::string message = stringf(Lang::COULD_NOT_OPEN_FILENAME, formatarg("path", path));
		lua_pushlstring(l, message.c_str(), message.size());
		return lua_error(l);
	} catch (const CouldNotWriteToFileException &) {
		return luaL_error(l, "%s", Lang::GAME_SAVE_CANNOT_WRITE);
	} catch (const std::invalid_argument &) {
		return luaL_error(l, "%s", Lang::GAME_SAVE_INVALID_NAME);
	}
}

/*
 * Function: DeleteSave
 *
 * Deletes save with specified file name.
 *
 * > Game.DeleteSave(filename)
 *
 * Parameters:
 *
 *   filename - Filename to delete. The file will be checked at 'savefiles'
 *              directory in the user's game directory.
 * Return:
 *
 *   bool - is save deleted successfully
 */
static int l_game_delete_save(lua_State *l)
{
	const std::string filename(luaL_checkstring(l, 1));
	lua_pushboolean(l, SaveGameManager::DeleteSave(filename));
	return 1;
}

/*
 * Function: ListSaves
 *
 * Retrieves a list of all game saves along with their last-modified time.
 *
 * > Game.ListSaves()
 *
 * Parameters:
 *
 * Return:
 *
 *   saves - a list of save-games as names and modification-date
 */
static int l_game_list_saves(lua_State *l)
{
	auto saves = SaveGameManager::ListSaves();

	lua_newtable(l);
	int savesTable = lua_gettop(l);
	int saves_len = 0;

	for (const auto &save : saves)
	{
		LuaTable saveTable(l);
		saveTable.Set("name", save.GetName());
		saveTable.Set("mtime", save.GetModificationTime());

		lua_rawseti(l, savesTable, ++saves_len);
	}

	return 1;
}

/*
 * Function: EndGame
 *
 * End the current game and return to the main menu.
 *
 * > Game.EndGame(filename)
 */
static int l_game_end_game(lua_State *l)
{
	if (Pi::game) {
		// Request to end the game as soon as possible.
		// Previously could be called from Lua UI and delete the object doing the calling causing a crash.
		Pi::RequestEndGame();
	}
	return 0;
}

/*
 * Attribute: player
 *
 * The <Player> object for the current player.
 */
static int l_game_attr_player(lua_State *l)
{
	if (!Pi::game)
		lua_pushnil(l);
	else
		LuaObject<Player>::PushToLua(Pi::player);
	return 1;
}

/*
 * Attribute: system
 *
 * The <StarSystem> object for the system the player is currently in.
 */
static int l_game_attr_system(lua_State *l)
{
	if (!Pi::game)
		lua_pushnil(l);
	else
		LuaObject<StarSystem>::PushToLua(Pi::game->GetSpace()->GetStarSystem().Get());
	return 1;
}

/*
 * Attribute: systemView
 *
 * The <SystemView> object for the system map view class
 */
static int l_game_attr_systemview(lua_State *l)
{
	if (!Pi::game)
		lua_pushnil(l);
	else
		LuaObject<SystemView>::PushToLua(Pi::game->GetSystemView());
	return 1;
}

/*
 * Attribute: sectorView
 *
 * The <SectorView> object for the sector map view class
 */
static int l_game_attr_sectorview(lua_State *l)
{
	if (!Pi::game)
		lua_pushnil(l);
	else
		LuaObject<SectorView>::PushToLua(Pi::game->GetSectorView());
	return 1;
}

/*
 * Attribute: time
 *
 * The current game time, in seconds since 12:00 01-01-3200
 */
static int l_game_attr_time(lua_State *l)
{
	if (!Pi::game)
		lua_pushnil(l);
	else
		lua_pushnumber(l, Pi::game->GetTime());
	return 1;
}

/*
 * Attribute: paused
 *
 * True if the game is paused.
 */
static int l_game_attr_paused(lua_State *l)
{
	if (!Pi::game)
		lua_pushboolean(l, 1);
	else
		lua_pushboolean(l, Pi::game->IsPaused());
	return 1;
}

/*
 * Function: InHyperspace
 *
 * Return true if the game is in hyperspace mode
 *
 * > hyperspace = Game.InHyperspace()
 *
 * Return:
 *
 *   hyperspace - true if the game is currently in hyperspace
 */

static int l_game_in_hyperspace(lua_State *l)
{
	LuaPush(l, Pi::game->IsHyperspace() || Pi::player->GetFlightState() == Ship::HYPERSPACE);
	return 1;
}

/*
 * Function: GetExcursionMode
 *
 * Where the player is: aboard the ship, out of it on foot, floating beside
 * it in space, or driving the surface rover.
 *
 * > mode = Game.GetExcursionMode()
 *
 * Return:
 *
 *   mode - "ship", "on_foot", "eva" or "rover"
 *
 * Status:
 *
 *   experimental
 */
static int l_game_get_excursion_mode(lua_State *l)
{
	if (!Pi::game)
		return luaL_error(l, "Game.GetExcursionMode only works when there is a game running");
	switch (Pi::game->GetExcursion()->GetMode()) {
	case Excursion::Mode::ON_FOOT: lua_pushstring(l, "on_foot"); break;
	case Excursion::Mode::EVA: lua_pushstring(l, "eva"); break;
	case Excursion::Mode::ROVER: lua_pushstring(l, "rover"); break;
	default: lua_pushstring(l, "ship"); break;
	}
	return 1;
}

/*
 * Function: CanDisembark
 *
 * Whether the player can get out of the ship now: landed (not docked) on a
 * planet with dry ground beside the ship, or flying in space (above any
 * atmosphere and terrain) without the autopilot, for an EVA.
 *
 * > ok = Game.CanDisembark()
 *
 * Status:
 *
 *   experimental
 */
static int l_game_can_disembark(lua_State *l)
{
	lua_pushboolean(l, Pi::game && Pi::game->GetExcursion()->CanDisembark());
	return 1;
}

/*
 * Function: Disembark
 *
 * Get out of the ship: on foot when landed, on EVA in space. Triggers
 * onPlayerDisembark.
 *
 * > ok = Game.Disembark()
 *
 * Return:
 *
 *   ok - false if the player can't get out now (see <CanDisembark>)
 *
 * Status:
 *
 *   experimental
 */
static int l_game_disembark(lua_State *l)
{
	lua_pushboolean(l, Pi::game && Pi::game->GetExcursion()->Disembark());
	return 1;
}

/*
 * Function: Board
 *
 * Get back into the ship from beside it. A parked rover stays where it is,
 * also when the ship leaves without it (see <DeployRover>). Triggers
 * onPlayerBoard.
 *
 * > ok = Game.Board()
 *
 * Return:
 *
 *   ok - false if the player is not on foot next to the ship
 *
 * Status:
 *
 *   experimental
 */
static int l_game_board(lua_State *l)
{
	lua_pushboolean(l, Pi::game && Pi::game->GetExcursion()->Board());
	return 1;
}

/*
 * Function: CanDeployRover
 *
 * Whether the surface rover can be set down now: the ship is landed on a
 * planet, carries a rover (the surface_rover capability) that is not out
 * already (<GetRoverPlanet>) and has open, dry ground beside it.
 *
 * > ok = Game.CanDeployRover()
 *
 * Status:
 *
 *   experimental
 */
static int l_game_can_deploy_rover(lua_State *l)
{
	lua_pushboolean(l, Pi::game && Pi::game->GetExcursion()->CanDeployRover());
	return 1;
}

/*
 * Function: DeployRover
 *
 * Set the rover down beside the landed ship with the player at the wheel.
 * It stays out, where it is parked (also when the ship leaves without it),
 * until it is driven back to the ship and stowed (the interact key,
 * triggering onRoverStowed and onPlayerBoard). Triggers onPlayerDisembark.
 *
 * > ok = Game.DeployRover()
 *
 * Return:
 *
 *   ok - false if it can't be done now (see <CanDeployRover>)
 *
 * Status:
 *
 *   experimental
 */
static int l_game_deploy_rover(lua_State *l)
{
	lua_pushboolean(l, Pi::game && Pi::game->GetExcursion()->DeployRover());
	return 1;
}

/*
 * Function: GetExcursionAction
 *
 * What the interact key would do now, for the surface HUD.
 *
 * > action = Game.GetExcursionAction()
 *
 * Return:
 *
 *   action - "board", "enter_rover", "exit_rover", "stow_rover" (driving
 *            it into the ship's hold and boarding), or nil if nothing
 *
 * Status:
 *
 *   experimental
 */
static int l_game_get_excursion_action(lua_State *l)
{
	switch (Pi::game ? Pi::game->GetExcursion()->GetAvailableAction() : Excursion::Action::NONE) {
	case Excursion::Action::BOARD: lua_pushstring(l, "board"); break;
	case Excursion::Action::ENTER_ROVER: lua_pushstring(l, "enter_rover"); break;
	case Excursion::Action::EXIT_ROVER: lua_pushstring(l, "exit_rover"); break;
	case Excursion::Action::STOW_ROVER: lua_pushstring(l, "stow_rover"); break;
	default: lua_pushnil(l); break;
	}
	return 1;
}

/*
 * Function: GetExcursionNavInfo
 *
 * The surface compass: the heading (of the view on foot, of the rover while
 * driving) and the bearing to the ship, in degrees clockwise from north, and
 * the distance to the ship; on foot with the rover out, the same for it.
 *
 * > heading, shipBearing, shipDistance, roverBearing, roverDistance = Game.GetExcursionNavInfo()
 *
 * Return:
 *
 *   heading - degrees, or nil when the player is aboard or in space
 *   shipBearing - degrees
 *   shipDistance - metres
 *   roverBearing - degrees, or nil without a rover to walk to
 *   roverDistance - metres
 *
 * Status:
 *
 *   experimental
 */
static int l_game_get_excursion_nav_info(lua_State *l)
{
	Excursion::NavInfo info;
	if (!Pi::game || !Pi::game->GetExcursion()->GetNavInfo(info))
		return 0;
	lua_pushnumber(l, info.heading);
	lua_pushnumber(l, info.shipBearing);
	lua_pushnumber(l, info.shipDistance);
	if (!info.hasRover)
		return 3;
	lua_pushnumber(l, info.roverBearing);
	lua_pushnumber(l, info.roverDistance);
	return 5;
}

/*
 * Function: GetRoverState
 *
 * The rover the player is driving, for the HUD.
 *
 * > speed, onGround, lightsLevel, maxLightsLevel = Game.GetRoverState()
 *
 * Return:
 *
 *   speed - m/s along its heading, negative in reverse, or nil when the
 *           player is not driving
 *   onGround - false while it flies off a bump
 *   lightsLevel - its lamps: 0 off, else brighter at each level
 *   maxLightsLevel - the brightest
 *
 * Status:
 *
 *   experimental
 */
static int l_game_get_rover_state(lua_State *l)
{
	const Excursion *excursion = Pi::game ? Pi::game->GetExcursion() : nullptr;
	if (!excursion || excursion->GetMode() != Excursion::Mode::ROVER || !excursion->GetRover())
		return 0;
	lua_pushnumber(l, excursion->GetRover()->GetSpeed());
	lua_pushboolean(l, excursion->GetRover()->IsOnGround());
	lua_pushinteger(l, excursion->GetRover()->GetLightsLevel());
	lua_pushinteger(l, Rover::MAX_LIGHTS_LEVEL);
	return 4;
}

/*
 * Function: CycleRoverLights
 *
 * Steps the lamps of the rover the player is driving through off, each
 * brightness level and off again, as the ship's lights button does.
 *
 * > Game.CycleRoverLights()
 *
 * Status:
 *
 *   experimental
 */
static int l_game_cycle_rover_lights(lua_State *l)
{
	Excursion *excursion = Pi::game ? Pi::game->GetExcursion() : nullptr;
	if (excursion && excursion->GetMode() == Excursion::Mode::ROVER && excursion->GetRover())
		excursion->GetRover()->CycleLights();
	return 0;
}

/*
 * Function: GetRover
 *
 * The player's surface rover while it is out of the ship in this star
 * system, driven or parked: a <Body> (its position: GetGroundPosition,
 * GetPositionRelTo, ...).
 *
 * > rover = Game.GetRover()
 *
 * Return:
 *
 *   rover - the rover, or nil when it is in the hold, parked in another
 *           system (<GetRoverPlanet>), or there is none
 *
 * Status:
 *
 *   experimental
 */
static int l_game_get_rover(lua_State *l)
{
	Rover *rover = Pi::game ? Pi::game->GetExcursion()->GetRover() : nullptr;
	if (!rover)
		return 0;
	LuaObject<Body>::PushToLua(rover);
	return 1;
}

/*
 * Function: DebugAdvanceTime
 *
 * Jump the game clock forward, for testing. Only while the player is docked.
 * Nothing is simulated in between: timers that fall due fire on the next
 * update, all at once.
 *
 * > ok = Game.DebugAdvanceTime(seconds)
 *
 * Return:
 *
 *   ok - true if the clock was moved
 *
 * Status:
 *
 *   debug
 */
static int l_game_debug_advance_time(lua_State *l)
{
	const double seconds = luaL_checknumber(l, 1);
	const bool ok = Pi::game && seconds > 0.0 && Pi::player->GetFlightState() == Ship::DOCKED;
	if (ok)
		Pi::game->AdvanceTime(seconds);
	LuaPush(l, ok);
	return 1;
}

/*
 * Function: GetRoverPlanet
 *
 * Where the player's surface rover is while it is out of the ship, in this
 * star system or parked in another one the ship left it in.
 *
 * > path = Game.GetRoverPlanet()
 *
 * Return:
 *
 *   path - the <SystemPath> of the planet (or moon) it is on, or nil when
 *          it is in the hold (or there is none)
 *
 * Status:
 *
 *   experimental
 */
static int l_game_get_rover_planet(lua_State *l)
{
	SystemPath path;
	if (!Pi::game || !Pi::game->GetExcursion()->GetRoverPlanet(path))
		return 0;
	LuaObject<SystemPath>::PushToLua(path);
	return 1;
}

/*
 * Function: GetJetpackState
 *
 * The jetpack of the player on foot, for the HUD.
 *
 * > fuel, thrust, burntOut = Game.GetJetpackState()
 *
 * Return:
 *
 *   fuel - 0 to 1, or nil when the player is aboard
 *   thrust - 0 to 1, how hard it is burning now
 *   burntOut - true while it refills after running dry, before it relights
 *
 * Status:
 *
 *   experimental
 */
static int l_game_get_jetpack_state(lua_State *l)
{
	const Walker *walker = Pi::game ? Pi::game->GetExcursion()->GetWalker() : nullptr;
	if (!walker)
		return 0;
	lua_pushnumber(l, walker->GetJetFuel());
	lua_pushnumber(l, walker->GetJetThrust());
	lua_pushboolean(l, walker->IsJetBurntOut());
	return 3;
}


/*
 * Function: GetControlledBody
 *
 * Where the player is: the player's ship aboard, the walker body on foot,
 * the rover while driving.
 *
 * > body = Game.GetControlledBody()
 *
 * Status:
 *
 *   experimental
 */
static int l_game_get_controlled_body(lua_State *l)
{
	if (!Pi::game)
		return luaL_error(l, "Game.GetControlledBody only works when there is a game running");
	Body *body = Pi::game->GetControlledBody();
	if (body == Pi::game->GetPlayer())
		LuaObject<Player>::PushToLua(Pi::game->GetPlayer());
	else
		LuaObject<Body>::PushToLua(body);
	return 1;
}

/*
 * Function: CurrentView
 *
 * Return the currently active game view
 *
 * > current_view = Game.CurrentView()
 *
 * Return:
 *
 *   view - a string describing the game view: "WorldView", "StationView", "InfoView", "SectorView", "SystemView", "DeathView"
 */

static int l_game_current_view(lua_State *l)
{
	const View *view = Pi::GetView();
	if( view != nullptr) {
		LuaPush(l, view->GetViewName());
	} else {
		lua_pushnil(l);
	}
	return 1;
}

// XXX temporary to support StationView "Launch" button
// remove once WorldView has been converted to the new UI
static int l_game_switch_view(lua_State *l)
{
	if (!Pi::game)
		return luaL_error(l, "can't switch view when no game is running");
	if (Pi::player->IsDead())
		Pi::SetView(Pi::game->GetDeathView());
	else
		Pi::SetView(Pi::game->GetWorldView());
	return 0;
}

static int pushTimeAccel(lua_State *l, const Game::TimeAccel accel)
{
	switch (accel) {
	case Game::TIMEACCEL_PAUSED: lua_pushstring(l, "paused"); break;
	case Game::TIMEACCEL_1X: lua_pushstring(l, "1x"); break;
	case Game::TIMEACCEL_10X: lua_pushstring(l, "10x"); break;
	case Game::TIMEACCEL_100X: lua_pushstring(l, "100x"); break;
	case Game::TIMEACCEL_1000X: lua_pushstring(l, "1000x"); break;
	case Game::TIMEACCEL_10000X: lua_pushstring(l, "10000x"); break;
	case Game::TIMEACCEL_HYPERSPACE: lua_pushstring(l, "hyperspace"); break;
	default:
		return luaL_error(l, "TimeAccel value of \"%d\" is outside of Game::TimeAccel enum", accel);
	}
	return 1;
}

static int l_game_get_time_acceleration(lua_State *l)
{
	const Game::TimeAccel accel = Pi::game->GetTimeAccel();
	return pushTimeAccel(l, accel);
}

static int l_game_get_requested_time_acceleration(lua_State *l)
{
	const Game::TimeAccel accel = Pi::game->GetRequestedTimeAccel();
	return pushTimeAccel(l, accel);
}

static int l_game_set_time_acceleration(lua_State *l)
{
	const std::string accel = LuaPull<std::string>(l, 1);
	const bool force = LuaPull<bool>(l, 2);
	Game::TimeAccel a = Game::TIMEACCEL_PAUSED;
	if (!accel.compare("paused"))
		a = Game::TIMEACCEL_PAUSED;
	else if (!accel.compare("1x"))
		a = Game::TIMEACCEL_1X;
	else if (!accel.compare("10x"))
		a = Game::TIMEACCEL_10X;
	else if (!accel.compare("100x"))
		a = Game::TIMEACCEL_100X;
	else if (!accel.compare("1000x"))
		a = Game::TIMEACCEL_1000X;
	else if (!accel.compare("10000x"))
		a = Game::TIMEACCEL_10000X;
	else if (!accel.compare("hyperspace"))
		a = Game::TIMEACCEL_HYPERSPACE;
	else
		return luaL_error(l, "Unknown time acceleration %s", accel.c_str());
	Pi::game->RequestTimeAccel(a, force);
	return 0;
}

static int l_game_get_date_time(lua_State *l)
{
	const Time::DateTime t(Pi::game->GetTime());
	int year, month, day, hour, minute, second;
	t.GetDateParts(&year, &month, &day);
	t.GetTimeParts(&hour, &minute, &second);
	lua_pushinteger(l, year);
	lua_pushinteger(l, month);
	lua_pushinteger(l, day);
	lua_pushinteger(l, hour);
	lua_pushinteger(l, minute);
	lua_pushinteger(l, second);
	return 6;
}

static int l_game_set_view(lua_State *l)
{
	if (!Pi::game) {
		return luaL_error(l, "can't set view when no game is running");
	}
	const std::string target = luaL_checkstring(l, 1);
	if (!Pi::SetView(target)) {
		return luaL_error(l, "Unknown view %s", target.c_str());
	}
	return 0;
}

static int l_game_get_world_cam_type(lua_State *l)
{
	switch (Pi::game->GetWorldView()->shipView->GetCamType()) {
	case ShipViewController::CAM_INTERNAL: lua_pushstring(l, "internal"); break;
	case ShipViewController::CAM_EXTERNAL: lua_pushstring(l, "external"); break;
	case ShipViewController::CAM_SIDEREAL: lua_pushstring(l, "sidereal"); break;
	case ShipViewController::CAM_FLYBY: lua_pushstring(l, "flyby"); break;
	default: Output("Unknown world view cam type\n"); break;
	}
	return 1;
}

static int l_game_set_world_cam_type(lua_State *l)
{
	const std::string cam = luaL_checkstring(l, 1);
	if (!cam.compare("internal"))
		Pi::game->GetWorldView()->shipView->SetCamType(ShipViewController::CAM_INTERNAL);
	else if (!cam.compare("external"))
		Pi::game->GetWorldView()->shipView->SetCamType(ShipViewController::CAM_EXTERNAL);
	else if (!cam.compare("sidereal"))
		Pi::game->GetWorldView()->shipView->SetCamType(ShipViewController::CAM_SIDEREAL);
	else if (!cam.compare("flyby"))
		Pi::game->GetWorldView()->shipView->SetCamType(ShipViewController::CAM_FLYBY);
	else
		return luaL_error(l, "Unknown world cam type %s", cam.c_str());
	return 0;
}

static int l_game_get_hyperspace_travelled_percentage(lua_State *l)
{
	LuaPush(l, Pi::game->GetHyperspaceArrivalProbability());
	return 1;
}

static int l_game_get_parts_from_date_time(lua_State *l)
{
	const double time = LuaPull<double>(l, 1);
	const Time::DateTime t(time);
	int year, month, day, hour, minute, second;
	t.GetDateParts(&year, &month, &day);
	t.GetTimeParts(&hour, &minute, &second);
	LuaPush(l, second);
	LuaPush(l, minute);
	LuaPush(l, hour);
	LuaPush(l, day);
	LuaPush(l, month);
	LuaPush(l, year);
	return 6;
}

template<typename T>
static int l_game_get_config(lua_State *l)
{
	// Restrict the template function to only a few types
	static_assert(
		std::is_same_v<T, bool> ||
		std::is_same_v<T, int> ||
		std::is_same_v<T, float> ||
		std::is_same_v<T, std::string>
	);

	if (lua_isnone(l, 1)) {
		return luaL_error(l, "GetConfig takes at least a key argument");
	}
	int arg = 1;
	std::string section = std::string();
	if (!lua_isnone(l, 2)) {
		section = LuaPull<std::string>(l, arg++);
	}
	const std::string key = LuaPull<std::string>(l, arg++);
	T value;
	if constexpr (std::is_same_v<std::decay_t<T>,bool>) {
		value = Pi::config->Int(section, key, 0) == 0? false : true;
	} else if constexpr (std::is_same_v<std::decay_t<T>,int>) {
		value = Pi::config->Int(section, key, 0);
	} else if constexpr (std::is_same_v<std::decay_t<T>,float>) {
		value = Pi::config->Float(section, 0.0f);
	} else if constexpr (std::is_same_v<std::decay_t<T>,std::string>) {
		value = Pi::config->String(section, "");
	} else {
		return luaL_error(l, "GetConfig was invoked with an unsupported type.");
	}
	LuaPush(l, value);
	return 1;
}
template<typename T>
static int l_game_set_config(lua_State *l)
{
	// Restrict the template function to only a few types
	static_assert(
		std::is_same_v<T, bool> ||
		std::is_same_v<T, int> ||
		std::is_same_v<T, float> ||
		std::is_same_v<T, std::string>
	);

	if (lua_isnone(l, 1) || lua_isnone(l, 2))
		return luaL_error(l, "SetConfig takes at least a key and a value argument");
	int arg = 1;
	std::string section = std::string();
	if (!lua_isnone(l, 3)) {
		section = LuaPull<std::string>(l, arg++);
	}
	const std::string key = LuaPull<std::string>(l, arg++);
	const T value = LuaPull<T>(l, arg++);
	if constexpr (std::is_same_v<std::decay_t<T>,bool>) {
		Pi::config->SetInt(section, key, value == 0? 0 : 1);
	} else if constexpr (std::is_same_v<std::decay_t<T>,int>) {
		Pi::config->SetInt(section, key, value);
	} else if constexpr (std::is_same_v<std::decay_t<T>,float>) {
		Pi::config->SetFloat(section, key, value);
	} else if constexpr (std::is_same_v<std::decay_t<T>,std::string>) {
		Pi::config->SetString(section, key, value);
	} else {
		return luaL_error(l, "GetConfig was invoked with an unsupported type.");
	}
	return 0;
}

void LuaGame::Register()
{
	lua_State *l = Lua::manager->GetLuaState();

	LUA_DEBUG_START(l);

	static const luaL_Reg l_methods[] = {
		{ "StartGame", l_game_start_game },
		{ "LoadGame", l_game_load_game },
		{ "CanLoadGame", l_game_can_load_game },
		{ "SaveGame", l_game_save_game },
		{ "DeleteSave", l_game_delete_save },
		{ "ListSaves", l_game_list_saves },
		{ "EndGame", l_game_end_game },
		{ "InHyperspace", l_game_in_hyperspace },
		{ "GetExcursionMode", l_game_get_excursion_mode },
		{ "CanDisembark", l_game_can_disembark },
		{ "Disembark", l_game_disembark },
		{ "Board", l_game_board },
		{ "CanDeployRover", l_game_can_deploy_rover },
		{ "DeployRover", l_game_deploy_rover },
		{ "GetExcursionAction", l_game_get_excursion_action },
		{ "GetExcursionNavInfo", l_game_get_excursion_nav_info },
		{ "GetRoverState", l_game_get_rover_state },
		{ "CycleRoverLights", l_game_cycle_rover_lights },
		{ "GetRover", l_game_get_rover },
		{ "GetRoverPlanet", l_game_get_rover_planet },
		{ "DebugAdvanceTime", l_game_debug_advance_time },
		{ "GetJetpackState", l_game_get_jetpack_state },
		{ "GetControlledBody", l_game_get_controlled_body },
		{ "SaveGameStats", l_game_savegame_stats },
		{ "CurrentSaveVersion", l_game_current_save_version },

		{ "SwitchView", l_game_switch_view },
		{ "CurrentView", l_game_current_view },
		{ "SetView", l_game_set_view },
		{ "GetDateTime", l_game_get_date_time },
		{ "GetPartsFromDateTime", l_game_get_parts_from_date_time },
		{ "SetTimeAcceleration", l_game_set_time_acceleration },
		{ "GetTimeAcceleration", l_game_get_time_acceleration },
		{ "GetRequestedTimeAcceleration", l_game_get_requested_time_acceleration },
		{ "GetHyperspaceTravelledPercentage", l_game_get_hyperspace_travelled_percentage },

		{ "SetWorldCamType", l_game_set_world_cam_type },
		{ "GetWorldCamType", l_game_get_world_cam_type },

		{ "GetConfigBool", l_game_get_config<bool> },
		{ "SetConfigBool", l_game_set_config<bool> },
		{ "GetConfigInt", l_game_get_config<int> },
		{ "SetConfigInt", l_game_set_config<int> },
		{ "GetConfigFloat", l_game_get_config<float> },
		{ "SetConfigFloat", l_game_set_config<float> },
		{ "GetConfigString", l_game_get_config<std::string> },
		{ "SetConfigString", l_game_set_config<std::string> },

		{ 0, 0 }
	};

	static const luaL_Reg l_attrs[] = {
		{ "player", l_game_attr_player },
		{ "system", l_game_attr_system },
		{ "systemView", l_game_attr_systemview },
		{ "sectorView", l_game_attr_sectorview },
		{ "time", l_game_attr_time },
		{ "paused", l_game_attr_paused },
		{ 0, 0 }
	};

	lua_getfield(l, LUA_REGISTRYINDEX, "CoreImports");
	LuaObjectBase::CreateObject(l_methods, l_attrs, 0);
	lua_setfield(l, -2, "Game");
	lua_pop(l, 1);

	LUA_DEBUG_END(l, 0);
}
