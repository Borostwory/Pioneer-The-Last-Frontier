// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "EnumStrings.h"
#include "Game.h"
#include "LuaColor.h"
#include "LuaObject.h"
#include "LuaTable.h"
#include "LuaVector.h"
#include "LuaVector2.h"
#include "Pi.h"
#include "Player.h"
#include "SystemView.h"
#include "core/Log.h"
#include "graphics/Graphics.h"

LuaTable projectable_to_lua_row(const Projectable &p, lua_State *l)
{
	LuaTable proj_table(l, 0, 5);
	proj_table.Set("type", int(p.type));
	if (p.type == Projectable::NONE) return proj_table;
	proj_table.Set("base", int(p.base));
	if (p.base == Projectable::SYSTEMBODY)
		proj_table.Set("ref", const_cast<SystemBody *>(p.ref.sbody));
	else
		proj_table.Set("ref", const_cast<Body *>(p.ref.body));
	// planned burn markers carry their (0-based) burn index in orbitIdx
	if (p.base == Projectable::PLANNER_BURN)
		proj_table.Set("burnIndex", p.orbitIdx + 1);
	// apsis and node markers of the player's and the planner's orbits are
	// labelled with their altitude
	const bool marker = p.type == Projectable::APOAPSIS || p.type == Projectable::PERIAPSIS ||
		p.type == Projectable::ASCENDING_NODE || p.type == Projectable::DESCENDING_NODE;
	if (marker && (p.base == Projectable::PLAYER || p.base == Projectable::PLANNER))
		proj_table.Set("altitude", p.altitude);
	return proj_table;
}

static int l_systemview_set_color(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	auto color_index = static_cast<SystemMapViewport::ColorIndex>(EnumStrings::GetValue("SystemViewColorIndex", LuaPull<const char *>(l, 2)));
	auto color_value = LuaColor::CheckFromLua(l, 3);
	sv->GetMap()->SetColor(color_index, color_value);
	sv->GetHudMap()->SetColor(color_index, color_value);
	return 0;
}

static int l_systemview_set_selected_object(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	Projectable::types type = static_cast<Projectable::types>(luaL_checkinteger(l, 2));
	Projectable::bases base = static_cast<Projectable::bases>(luaL_checkinteger(l, 3));
	if (base == Projectable::SYSTEMBODY)
		sv->GetMap()->SetSelectedObject({ type, base, LuaObject<SystemBody>::CheckFromLua(4) });
	else
		sv->GetMap()->SetSelectedObject({ type, base, LuaObject<Body>::CheckFromLua(4) });
	return 0;
}

/*
 * Method: GetProjectedGrouped
 *
 * Get a table of projected point objects from the SystemView class, having
 * previously grouped them.
 *
 * In the process of rendering the current frame in a SystemView class, an
 * array of visible point objects (centers of planets, ships, special points
 * of orbits) is created. This function creates a lua table based on this
 * array, after grouping them according to certain criteria.
 *
 * Resulting table:
 *
 * {
 * 	*** ORBIT ICONS (apoapsis, periapsis) ***
 *
 * 	{	screenCoordinates: vector2d
 * 		mainObject (see struct Projectable in SystemView.h)
 * 		{	type: Projectable::types
 * 			base: Projectable::bases
 * 			ref: SystemBody* or Body*
 * 			}
 * 		objects (if multiple only, main object is duplicated there)
 * 		{
 * 			{type, base, ref}
 * 			{type, base, ref}
 * 			...
 * 			}
 * 		hasPlayer: boolean
 * 		hasCombatTarget: boolean
 * 		hasNavTarget: boolean
 * 	}
 * 	{ screencoordinates, mainobject, ... }
 * 	{ screencoordinates, mainobject, ... }
 * 	...
 *
 * *** LAGRANGE ICONS ***
 * 	{ screenCoordinates, mainObject, ... }
 * 	{ screenCoordinates, mainObject, ... }
 * 	...
 *
 * *** BODY ICONS (stars, planets, ships) ***
 * 	{ screenCoordinates, mainObject, ... }
 * 	{ screenCoordinates, mainObject, ... }
 * 	...
 *
 * }
 *
 */

static int push_projected_grouped(lua_State *l, SystemMapViewport *map, const vector2d &gap)
{
	using GroupInfo = Projectable::GroupInfo;

	auto &projected = map->GetProjected();

	const Body *nav_target = Pi::game->GetPlayer()->GetNavTarget();
	const Body *combat_target = Pi::game->GetPlayer()->GetCombatTarget();

	const char *special_object_lua_name[] = { "hasPlayer", "hasCombatTarget", "hasNavTarget" };
	std::vector<Projectable> specials {
		{ Projectable::OBJECT, Projectable::PLAYER, Pi::player },
		{ Projectable::OBJECT, Projectable::SHIP, combat_target }
	};

	if (nav_target && nav_target->GetSystemBody())
		specials.push_back({ Projectable::OBJECT, Projectable::SYSTEMBODY, nav_target->GetSystemBody() });
	else if (nav_target && nav_target->IsType(ObjectType::SHIP))
		specials.push_back({ Projectable::OBJECT, Projectable::SHIP, nav_target });
	else if (nav_target)
		specials.push_back({ Projectable::OBJECT, Projectable::BODY, nav_target });

	//no need to sort, because the bodies are so recorded in good order
	//because they are written recursively starting from the root
	//body of the system, and ships go after the system bodies
	std::vector<GroupInfo> groups = map->GroupProjectables(vector2f(gap), specials);

	LuaTable result(l, groups.size(), 0);
	int index = 1;

	// groups are stably ordered by projectable type
	// the sooner is displayed, the more in the background
	// so it goes orbit icons -> lagrange icons -> bodies
	for (GroupInfo &group : groups) {
		LuaTable info_table(l, 0, 6);
		const Projectable &mainObject = projected[group.tracks[0]];
		info_table.Set("screenCoordinates", vector3d(group.screenpos));
		info_table.Set("screenSize", mainObject.screensize);
		info_table.Set("mainObject", projectable_to_lua_row(mainObject, l));
		lua_pop(l, 1);
		if (group.tracks.size() > 1) {
			LuaTable objects_table(l, group.tracks.size(), 0);
			int objects_table_index = 1;
			for (int idx : group.tracks) {
				objects_table.Set(objects_table_index++, projectable_to_lua_row(projected[idx], l));
				lua_pop(l, 1);
			}
			info_table.Set("objects", objects_table);
			lua_pop(l, 1);
		}
		for (size_t object_type = 0; object_type < specials.size(); object_type++)
			info_table.Set(special_object_lua_name[object_type], group.hasSpecial(object_type));
		result.Set(index++, info_table);
		lua_pop(l, 1);
	}

	LuaPush(l, result);
	return 1;
}

static int l_systemview_get_projected_grouped(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	const vector2d gap = LuaPull<vector2d>(l, 2);
	return push_projected_grouped(l, sv->GetMap(), gap);
}

static int l_systemview_get_hud_map_projected_grouped(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	const vector2d gap = LuaPull<vector2d>(l, 2);
	return push_projected_grouped(l, sv->GetHudMap(), gap);
}

static int l_systemview_get_system(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	LuaPush(l, sv->GetMap()->GetCurrentSystem());
	return 1;
}

static int l_systemview_get_selected_object(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	Projectable *p = sv->GetMap()->GetSelectedObject();
	LuaPush(l, projectable_to_lua_row(*p, l));
	return 1;
}

static int l_systemview_clear_selected_object(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	sv->GetMap()->ClearSelectedObject();
	return 0;
}

static int l_systemview_view_selected_object(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	sv->GetMap()->ViewSelectedObject();
	return 0;
}

/*
 * Method: ViewPlayer
 *
 * > systemView:ViewPlayer()
 *
 * Centre the map on the player's ship and zoom to frame its orbit.
 */
static int l_systemview_view_player(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	sv->ViewPlayer();
	return 0;
}

/*
 * Method: IsViewingCurrentSystem
 *
 * > viewing = systemView:IsViewingCurrentSystem()
 *
 * True when the map shows the system the player is in, in either selection
 * mode.
 */
static int l_systemview_is_viewing_current_system(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	LuaPush(l, sv->IsViewingCurrentSystem());
	return 1;
}

/*
 * Method: ViewPlannedShip
 *
 * > systemView:ViewPlannedShip()
 *
 * Keep the map centred on where the transfer plan puts the ship at map time.
 */
static int l_systemview_view_planned_ship(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	sv->ViewPlannedShip();
	return 0;
}

/*
 * Method: IsViewingPlannedShip
 *
 * > following = systemView:IsViewingPlannedShip()
 */
static int l_systemview_is_viewing_planned_ship(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	LuaPush(l, sv->IsViewingPlannedShip());
	return 1;
}

static int l_systemview_reset_viewpoint(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	sv->GetMap()->ResetViewpoint();
	return 0;
}

static int l_systemview_set_visibility(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	std::string param = LuaPull<std::string>(l, 2);
	sv->GetMap()->SetVisibility(param);
	if (param == "LAG_OFF" || param == "LAG_ICON" || param == "LAG_ICONTEXT") {
		sv->GetHudMap()->SetVisibility(param);
	}
	return 0;
}

static int l_systemview_set_hud_map_size(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	const vector2d size = LuaPull<vector2d>(l, 2);
	sv->SetHudMapSize(size);
	return 0;
}

static int l_systemview_draw_hud_map(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	sv->DrawHudMapPiGui();
	return 0;
}

static int l_systemview_set_hud_map_focus(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	std::string mode = LuaPull<std::string>(l, 2);
	if (mode == "PLAYER")
		sv->SetHudMapFocus(SystemView::HudMapFocus::PLAYER);
	else
		sv->SetHudMapFocus(SystemView::HudMapFocus::FRAME_BODY);
	return 0;
}

static int l_systemview_get_hud_map_focus(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	const char *mode = (sv->GetHudMapFocus() == SystemView::HudMapFocus::PLAYER) ? "PLAYER" : "FRAME_BODY";
	LuaPush(l, mode);
	return 1;
}

static int l_systemview_get_orbit_planner_start_time(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	double t = sv->GetOrbitPlannerStartTime();
	if (std::fabs(t) < 1.)
		lua_pushnil(l);
	else
		LuaPush<double>(l, t);
	return 1;
}

static int l_systemview_get_orbit_planner_time(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	double t = sv->GetMap()->GetTime();
	LuaPush<double>(l, t);
	return 1;
}

static int l_systemview_accelerate_time(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	if (lua_isnil(l, 2))
		sv->GetMap()->SetRealTime();
	else {
		double step = LuaPull<double>(l, 2);
		sv->GetMap()->AccelerateTime(step);
	}
	return 0;
}

static int l_systemview_set_rotate_mode(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	bool b = LuaPull<bool>(l, 2);
	sv->GetMap()->SetRotateMode(b);
	return 0;
}

static int l_systemview_set_zoom_mode(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	bool b = LuaPull<bool>(l, 2);
	sv->GetMap()->SetZoomMode(b);
	return 0;
}

static int l_systemview_get_zoom(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	LuaPush(l, sv->GetMap()->GetZoom());
	return 1;
}

static int l_systemview_atlas_view_planet_gap(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	auto radius = LuaPull<float>(l, 2);
	LuaPush(l, sv->GetMap()->AtlasViewPlanetGap(radius));
	return 1;
}

static int l_systemview_atlas_view_pixel_per_unit(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	LuaPush(l, sv->GetMap()->AtlasViewPixelPerUnit());
	return 1;
}

static int l_systemview_get_display_mode(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	LuaPush(l, EnumStrings::GetString("SystemViewMode", int(sv->GetDisplayMode())));
	return 1;
}

static int l_systemview_set_display_mode(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int mode = EnumStrings::GetValue("SystemViewMode", LuaPull<const char *>(l, 2));
	sv->SetDisplayMode(SystemView::Mode(mode));
	return 1;
}

static int l_systemview_get_system_selection_mode(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	LuaPush(l, EnumStrings::GetString("SystemSelectionMode", int(sv->GetSystemSelectionMode())));
	return 1;
}

static int l_systemview_set_system_selection_mode(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int mode = EnumStrings::GetValue("SystemSelectionMode", LuaPull<const char *>(l, 2));
	sv->SetSystemSelectionMode(SystemView::SystemSelectionMode(mode));
	return 1;
}
// shared argument parsing for the transfer planner bindings:
// TransferPlanner*(idx, key, ...) takes an optional leading burn index, while the
// legacy TransferPlanner*(key, ...) form defaults to index 0 so existing call
// sites behave exactly as before.
namespace {
struct TransferPlannerCallArgs {
	int idx = 0;
	bool hasIdx = false;
	int keyArg = 2;
	int valueArg = 3;
};

TransferPlannerCallArgs ParseTransferPlannerCallArgs(lua_State *l)
{
	TransferPlannerCallArgs args;
	if (lua_isnumber(l, 2)) {
		args.idx = static_cast<int>(luaL_checkinteger(l, 2));
		args.hasIdx = true;
		args.keyArg = 3;
		args.valueArg = 4;
	}
	return args;
}
} // namespace

static int l_systemview_transfer_planner_get(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	TransferPlannerCallArgs args = ParseTransferPlannerCallArgs(l);
	std::string key = LuaPull<std::string>(l, args.keyArg);

	TransferPlanner *planner = sv->GetTransferPlanner();
	if (key == "prograde") {
		LuaPush<double>(l, planner->GetDv(args.idx, TransferPlanner::PROGRADE));
	} else if (key == "normal") {
		LuaPush<double>(l, planner->GetDv(args.idx, TransferPlanner::NORMAL));
	} else if (key == "radial") {
		LuaPush<double>(l, planner->GetDv(args.idx, TransferPlanner::RADIAL));
	} else if (key == "starttime") {
		LuaPush<double>(l, planner->GetStartTime(args.idx));
	} else if (key == "factor") {
		LuaPush<double>(l, planner->GetFactor() * 10); // factor is shown as "x 10"
	} else {
		Warning("Unknown transfer planner key %s\n", key.c_str());
		lua_pushnil(l);
	}
	return 1;
}

static int l_systemview_transfer_planner_add(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	TransferPlannerCallArgs args = ParseTransferPlannerCallArgs(l);
	std::string key = LuaPull<std::string>(l, args.keyArg);
	double delta = LuaPull<double>(l, args.valueArg);

	TransferPlanner *planner = sv->GetTransferPlanner();
	if (key == "prograde") {
		planner->AddDv(args.idx, TransferPlanner::PROGRADE, delta);
	} else if (key == "normal") {
		planner->AddDv(args.idx, TransferPlanner::NORMAL, delta);
	} else if (key == "radial") {
		planner->AddDv(args.idx, TransferPlanner::RADIAL, delta);
	} else if (key == "starttime") {
		// TransferPlanner::AddStartTime applies the factor internally, so pass
		// the raw timeStep; the legacy overload keeps its plan-clearing semantics
		if (args.hasIdx)
			planner->AddStartTime(args.idx, delta);
		else
			planner->AddStartTime(delta);
	} else if (key == "factor") {
		if (delta > 0)
			planner->IncreaseFactor();
		else
			planner->DecreaseFactor();
	} else {
		Warning("Unknown transfer planner key %s\n", key.c_str());
	}
	return 0;
}

static int l_systemview_transfer_planner_reset(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	TransferPlannerCallArgs args = ParseTransferPlannerCallArgs(l);
	std::string key = LuaPull<std::string>(l, args.keyArg);

	TransferPlanner *planner = sv->GetTransferPlanner();
	if (key == "prograde") {
		if (args.hasIdx)
			planner->ResetDv(args.idx, TransferPlanner::PROGRADE);
		else
			planner->ResetDv(TransferPlanner::PROGRADE);
	} else if (key == "normal") {
		if (args.hasIdx)
			planner->ResetDv(args.idx, TransferPlanner::NORMAL);
		else
			planner->ResetDv(TransferPlanner::NORMAL);
	} else if (key == "radial") {
		if (args.hasIdx)
			planner->ResetDv(args.idx, TransferPlanner::RADIAL);
		else
			planner->ResetDv(TransferPlanner::RADIAL);
	} else if (key == "starttime") {
		if (args.hasIdx)
			planner->ResetStartTime(args.idx);
		else
			planner->ResetStartTime();
	} else if (key == "factor") {
		planner->ResetFactor();
	} else {
		Warning("Unknown transfer planner key %s\n", key.c_str());
	}

	return 0;
}

static int l_systemview_transfer_planner_get_burn_count(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	LuaPush(l, static_cast<uint64_t>(sv->GetTransferPlanner()->GetBurnCount()));
	return 1;
}

static int l_systemview_transfer_planner_insert_burn(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int afterIdx = LuaPull<int>(l, 2);
	LuaPush(l, sv->GetTransferPlanner()->InsertBurn(afterIdx));
	return 1;
}

static int l_systemview_transfer_planner_remove_burn(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int idx = LuaPull<int>(l, 2);
	sv->GetTransferPlanner()->RemoveBurn(idx);
	return 0;
}

static int l_systemview_transfer_planner_complete_burn(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int idx = LuaPull<int>(l, 2);
	sv->GetTransferPlanner()->CompleteBurn(idx);
	return 0;
}

static int l_systemview_transfer_planner_get_burn_status(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int idx = LuaPull<int>(l, 2);
	LuaPush(l, sv->GetTransferPlanner()->GetBurnStatus(idx));
	return 1;
}

static int l_systemview_transfer_planner_get_burn_delta_v(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int idx = LuaPull<int>(l, 2);
	LuaPush<double>(l, sv->GetTransferPlanner()->GetBurnDeltaV(idx));
	return 1;
}

static int l_systemview_transfer_planner_get_burn_remaining_delta_v(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int idx = LuaPull<int>(l, 2);
	LuaPush<double>(l, sv->GetTransferPlanner()->GetBurnRemainingDeltaV(idx));
	return 1;
}

static int l_systemview_transfer_planner_get_active_index(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	// -1 once every planned burn has been completed
	LuaPush(l, sv->GetTransferPlanner()->GetActiveIndex());
	return 1;
}

// snap event names, as the Lua side passes them
static bool PullSnapEvent(lua_State *l, int arg, TransferPlanner::SnapEvent &event)
{
	const std::string name = LuaPull<std::string>(l, arg);
	if (name == "periapsis")
		event = TransferPlanner::SNAP_PERIAPSIS;
	else if (name == "apoapsis")
		event = TransferPlanner::SNAP_APOAPSIS;
	else if (name == "ascending")
		event = TransferPlanner::SNAP_ASCENDING_NODE;
	else if (name == "descending")
		event = TransferPlanner::SNAP_DESCENDING_NODE;
	else
		return false;
	return true;
}

/*
 * Method: TransferPlannerGetSnapTime
 *
 * > time = systemView:TransferPlannerGetSnapTime(idx, event)
 *
 * The node time a snap of burn idx to event ("periapsis", "apoapsis",
 * "ascending", "descending") would set, or nil when its orbit has no such
 * event ahead.
 */
static int l_systemview_transfer_planner_get_snap_time(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int idx = LuaPull<int>(l, 2);
	TransferPlanner::SnapEvent event;
	double time;
	if (!PullSnapEvent(l, 3, event) || !sv->GetTransferPlanner()->GetSnapTime(idx, event, time))
		lua_pushnil(l);
	else
		LuaPush<double>(l, time);
	return 1;
}

/*
 * Method: TransferPlannerSnap
 *
 * > snapped = systemView:TransferPlannerSnap(idx, event)
 *
 * Move burn idx to the time TransferPlannerGetSnapTime reports.
 */
static int l_systemview_transfer_planner_snap(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int idx = LuaPull<int>(l, 2);
	TransferPlanner::SnapEvent event;
	LuaPush(l, PullSnapEvent(l, 3, event) && sv->GetTransferPlanner()->SnapStartTime(idx, event));
	return 1;
}

/*
 * Method: TransferPlannerGetNodeReference
 *
 * > name, isTarget = systemView:TransferPlannerGetNodeReference()
 *
 * What the ascending/descending nodes are measured against: the navigation
 * target's orbit (isTarget) or the central body's equator, by name; nil when
 * there is no reference.
 */
static int l_systemview_transfer_planner_get_node_reference(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	vector3d normal;
	std::string name;
	bool isTarget;
	if (!sv->GetTransferPlanner()->GetNodeReference(normal, name, isTarget)) {
		lua_pushnil(l);
		return 1;
	}
	LuaPush(l, name);
	LuaPush(l, isTarget);
	return 2;
}

/*
 * Method: TransferPlannerGetCircularizeDeltaV
 *
 * > dv, time, apsis = systemView:TransferPlannerGetCircularizeDeltaV(idx)
 *
 * What circularizing burn idx would do: the delta-v magnitude, the node time
 * it would move to, and the apsis that is ("periapsis", "apoapsis", or nil
 * for the burn's own time). nil when it cannot be circularized.
 */
static int l_systemview_transfer_planner_get_circularize_delta_v(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int idx = LuaPull<int>(l, 2);
	double time, magnitude, dv[3];
	int apsis;
	if (!sv->GetTransferPlanner()->GetCircularizePlan(idx, time, apsis, dv, magnitude)) {
		lua_pushnil(l);
		return 1;
	}
	LuaPush<double>(l, magnitude);
	LuaPush<double>(l, time);
	if (apsis == TransferPlanner::SNAP_PERIAPSIS)
		LuaPush(l, "periapsis");
	else if (apsis == TransferPlanner::SNAP_APOAPSIS)
		LuaPush(l, "apoapsis");
	else
		lua_pushnil(l);
	return 3;
}

/*
 * Method: TransferPlannerCircularize
 *
 * > done = systemView:TransferPlannerCircularize(idx)
 *
 * Replace burn idx's delta-v with the one that circularizes the orbit there.
 */
static int l_systemview_transfer_planner_circularize(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	int idx = LuaPull<int>(l, 2);
	LuaPush(l, sv->GetTransferPlanner()->Circularize(idx));
	return 1;
}

/*
 * Method: TransferPlannerPlanApproach
 *
 * > ok, error = systemView:TransferPlannerPlanApproach(target, burns, standOff)
 *
 * Append burns that take the ship to the target body: burns = 1 intercepts
 * it, burns = 2 also matches its velocity on arrival. A ship or station is
 * met standOff metres (optional, default 0) behind it on its orbit. On
 * failure ok is false and error says why.
 */
static int l_systemview_transfer_planner_plan_approach(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	const Body *target = LuaObject<Body>::GetFromLua(2);
	const int burns = LuaPull<int>(l, 3);
	const double standOff = luaL_optnumber(l, 4, 0.0);
	std::string error;
	if (sv->GetTransferPlanner()->PlanApproach(target, burns, standOff, error)) {
		LuaPush(l, true);
		return 1;
	}
	LuaPush(l, false);
	LuaPush(l, error);
	return 2;
}

/*
 * Method: TransferPlannerGetClosestApproach
 *
 * > distance, time, speed, radius = systemView:TransferPlannerGetClosestApproach(target)
 *
 * The closest approach of the planned trajectory to the target: the distance
 * between the ship and the target's centre, its game time, the relative speed
 * then, and the target's radius (0 for ships and stations). On failure nil and
 * a reason.
 */
static int l_systemview_transfer_planner_get_closest_approach(lua_State *l)
{
	SystemView *sv = LuaObject<SystemView>::CheckFromLua(1);
	const Body *target = LuaObject<Body>::GetFromLua(2);
	TransferPlanner::ClosestApproach approach;
	std::string error;
	if (!sv->GetTransferPlanner()->GetClosestApproach(target, approach, error)) {
		lua_pushnil(l);
		LuaPush(l, error);
		return 2;
	}
	LuaPush<double>(l, approach.distance);
	LuaPush<double>(l, approach.time);
	LuaPush<double>(l, approach.relativeSpeed);
	LuaPush<double>(l, approach.targetRadius);
	return 4;
}

template <>
const char *LuaObject<SystemView>::s_type = "SystemView";

template <>
void LuaObject<SystemView>::RegisterClass()
{
	static const luaL_Reg l_methods[] = {

		{ "GetSystem", l_systemview_get_system },
		{ "ClearSelectedObject", l_systemview_clear_selected_object },
		{ "GetProjectedGrouped", l_systemview_get_projected_grouped },
		{ "GetHudMapProjectedGrouped", l_systemview_get_hud_map_projected_grouped },
		{ "SetHudMapSize", l_systemview_set_hud_map_size },
		{ "DrawHudMap", l_systemview_draw_hud_map },
		{ "SetHudMapFocus", l_systemview_set_hud_map_focus },
		{ "GetHudMapFocus", l_systemview_get_hud_map_focus },
		{ "GetSelectedObject", l_systemview_get_selected_object },
		{ "GetOrbitPlannerStartTime", l_systemview_get_orbit_planner_start_time },
		{ "GetOrbitPlannerTime", l_systemview_get_orbit_planner_time },
		{ "AccelerateTime", l_systemview_accelerate_time },
		{ "SetSelectedObject", l_systemview_set_selected_object },
		{ "ViewSelectedObject", l_systemview_view_selected_object },
		{ "ResetViewpoint", l_systemview_reset_viewpoint },
		{ "ViewPlayer", l_systemview_view_player },
		{ "ViewPlannedShip", l_systemview_view_planned_ship },
		{ "IsViewingPlannedShip", l_systemview_is_viewing_planned_ship },
		{ "IsViewingCurrentSystem", l_systemview_is_viewing_current_system },
		{ "SetVisibility", l_systemview_set_visibility },
		{ "SetColor", l_systemview_set_color },
		{ "SetRotateMode", l_systemview_set_rotate_mode },
		{ "SetZoomMode", l_systemview_set_zoom_mode },
		{ "GetDisplayMode", l_systemview_get_display_mode },
		{ "SetDisplayMode", l_systemview_set_display_mode },
		{ "GetSystemSelectionMode", l_systemview_get_system_selection_mode },
		{ "SetSystemSelectionMode", l_systemview_set_system_selection_mode },
		{ "GetZoom", l_systemview_get_zoom },
		{ "AtlasViewPlanetGap", l_systemview_atlas_view_planet_gap },
		{ "AtlasViewPixelPerUnit", l_systemview_atlas_view_pixel_per_unit },

		{ "TransferPlannerAdd", l_systemview_transfer_planner_add },
		{ "TransferPlannerGet", l_systemview_transfer_planner_get },
		{ "TransferPlannerReset", l_systemview_transfer_planner_reset },
		{ "TransferPlannerGetBurnCount", l_systemview_transfer_planner_get_burn_count },
		{ "TransferPlannerInsertBurn", l_systemview_transfer_planner_insert_burn },
		{ "TransferPlannerRemoveBurn", l_systemview_transfer_planner_remove_burn },
		{ "TransferPlannerCompleteBurn", l_systemview_transfer_planner_complete_burn },
		{ "TransferPlannerGetBurnStatus", l_systemview_transfer_planner_get_burn_status },
		{ "TransferPlannerGetBurnDeltaV", l_systemview_transfer_planner_get_burn_delta_v },
		{ "TransferPlannerGetBurnRemainingDeltaV", l_systemview_transfer_planner_get_burn_remaining_delta_v },
		{ "TransferPlannerGetActiveIndex", l_systemview_transfer_planner_get_active_index },
		{ "TransferPlannerGetSnapTime", l_systemview_transfer_planner_get_snap_time },
		{ "TransferPlannerSnap", l_systemview_transfer_planner_snap },
		{ "TransferPlannerGetNodeReference", l_systemview_transfer_planner_get_node_reference },
		{ "TransferPlannerGetCircularizeDeltaV", l_systemview_transfer_planner_get_circularize_delta_v },
		{ "TransferPlannerCircularize", l_systemview_transfer_planner_circularize },
		{ "TransferPlannerPlanApproach", l_systemview_transfer_planner_plan_approach },
		{ "TransferPlannerGetClosestApproach", l_systemview_transfer_planner_get_closest_approach },

		{ NULL, NULL }
	};

	LuaObjectBase::CreateClass(s_type, nullptr, l_methods, 0, 0);
}
