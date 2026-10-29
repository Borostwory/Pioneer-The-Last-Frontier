-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Multi-burn flight HUD widget.
--
-- A compact panel in the flight HUD (WorldView only) listing every planned
-- transfer burn, one row each: status, the Δv still to fly (with a progress
-- bar once the burn is under way), the countdowns to ignition and to the
-- node, the burn time left, and per-row Complete / Warp controls. A title
-- line sums up what is left of the plan; the two autopilot toggles (Auto
-- Orientation, Auto Burn) and the stop-before-burn margin sit below the table.
--
-- The table is rebuilt from the transfer planner every frame it draws. The
-- planner is the single source of truth (shared with the map sidebar), so this
-- module keeps no cached copy of the schedule.
--
-- Scope: Phase 4 (burn table, autopilot toggles) and Phase 5 (warp to burn)
-- of docs/implementation_plans/multi-burn-hud-autopilot_plan.md.
--
-- Phase 5: the Warp control warps the clock to (burn start − offset) with the
-- game's own time-acceleration levels and, on arrival, pauses or drops to 1x
-- as the player chose. The simulation is never un-paused by this module:
-- resume happens only through an explicit user action (Cancel Warp), using the
-- pause menu's restore pattern from views/game.lua. Arrival is detected on a
-- physics tick (Timer:CallAt, armed once per warp) and on every draw as a
-- supplement.
--
-- The Warp control runs in the same Lua VM as gameplay, so it can arm a game
-- Timer; the state machine itself lives in this module.

local Engine = require 'Engine'
local Event = require 'Event'
local Game = require 'Game'
local Input = require 'Input'
local Lang = require 'Lang'
local Timer = require 'Timer'
local lc = Lang.GetResource("core")
local lui = Lang.GetResource("ui-core")
local ui = require 'pigui'
local Vector2 = _G.Vector2

local icons = ui.theme.icons
local colors = ui.theme.colors
local styles = ui.theme.styles

-- the panel's type scale: a heading for the title, readable captions over
-- the columns, and one data font for every value in the table and below it
local titleFont = ui.fonts.orbiteer.small
local captionFont = ui.fonts.pionillium.small
local dataFont = ui.fonts.pionillium.details

local gameView = require 'pigui.views.game'
local ApproachTarget = require 'pigui.libs.approach-target'

-- TransferPlanner burn status values, as returned by
-- SystemView:TransferPlannerGetBurnStatus (see src/TransferPlanner.h)
local BURN_STATUS_PENDING = 0
local BURN_STATUS_ACTIVE = 1
local BURN_STATUS_COMPLETED = 2

-- every window this module opens is fixed: no dragging, no ImGui layout
-- persistence. No keybindings are used anywhere in this widget. The panel
-- sizes itself to its content and is anchored by its bottom-right corner.
local windowFlags = ui.WindowFlags {"NoTitleBar", "NoResize", "NoMove", "NoSavedSettings",
	"NoFocusOnAppearing", "NoBringToFrontOnFocus", "NoScrollbar", "AlwaysAutoResize"}
local toggleFlags = ui.WindowFlags {"NoTitleBar", "NoResize", "NoMove", "NoSavedSettings",
	"NoFocusOnAppearing", "NoBringToFrontOnFocus", "NoBackground"}
local tableFlags = ui.TableFlags {"SizingFixedFit", "NoSavedSettings", "NoHostExtendX"}

-- cells render "-" when a value is unavailable
local UNAVAILABLE = "-"

-- The plan cap, mirrored from src/TransferPlanner.cpp (kMaxBurns).
local MAX_BURN_COUNT = 8

-- the panel's strings, from the ui-core lang resource; the numeric drag
-- formats and the "1x" time-acceleration label stay literal
local L = {
	TITLE = lui.BURN_PLAN_TITLE,
	SUMMARY = lui.BURN_PLAN_SUMMARY,
	SUMMARY_TIP = lui.BURN_PLAN_SUMMARY_TIP,
	ALL_DONE = lui.BURN_PLAN_ALL_DONE,
	BURN = lui.BURN_NUMBER,
	NO_BURNS = lui.BURN_PLAN_NO_BURNS,
	CAPTION_BURN = lui.BURN_CAPTION_BURN,
	CAPTION_DV = lui.BURN_CAPTION_DV,
	CAPTION_IGNITION = lui.BURN_CAPTION_IGNITION,
	CAPTION_NODE = lui.BURN_CAPTION_NODE,
	CAPTION_DURATION = lui.BURN_CAPTION_DURATION,
	CAPTION_DV_TIP = lui.BURN_CAPTION_DV_TIP,
	CAPTION_IGNITION_TIP = lui.BURN_CAPTION_IGNITION_TIP,
	CAPTION_NODE_TIP = lui.BURN_CAPTION_NODE_TIP,
	CAPTION_DURATION_TIP = lui.BURN_CAPTION_DURATION_TIP,
	DV_TIP = lui.BURN_DV_TIP,
	DURATION_TIP = lui.BURN_DURATION_TIP,
	NO_THRUST = lui.BURN_NO_THRUST,
	DONE = lui.BURN_DONE,
	STATUS_PENDING = lui.BURN_STATUS_PLANNED,
	STATUS_ACTIVE = lui.BURN_STATUS_ACTIVE,
	STATUS_COMPLETED = lui.BURN_STATUS_COMPLETED,
	-- chained burns: this burn starts before the previous one ends
	OVERLAP = lui.BURN_OVERLAP,
	COMPLETE = lui.BURN_MARK_COMPLETE,
	-- the Warp tooltip names the configured stop-before-burn offset, so the row
	-- control states exactly what it will do; the greyed form states why it
	-- cannot (the target is at or before now, exactly startWarp's own rule)
	WARP = lui.BURN_WARP_TIP,
	WARP_REACHED = lui.BURN_WARP_REACHED,
	WARP_STRIP = lui.BURN_WARP_STRIP,
	WARP_STRIP_LIMITED = lui.BURN_WARP_STRIP_LIMITED,
	CANCEL_WARP = lui.BURN_CANCEL_WARP,
	OFFSET_LABEL = lui.BURN_STOP_BEFORE_BURN,
	OFFSET_FORMAT = "%.0f s",
	OFFSET_TIP = lui.BURN_STOP_BEFORE_BURN_TIP,
	ARRIVE_LABEL = lui.BURN_ARRIVE_THEN,
	ARRIVE_PAUSE = lui.BURN_ARRIVE_PAUSE,
	ARRIVE_PAUSE_TIP = lui.BURN_ARRIVE_PAUSE_TIP,
	ARRIVE_NORMAL = "1x",
	ARRIVE_NORMAL_TIP = lui.BURN_ARRIVE_NORMAL_TIP,
	APPROACH_STOP = lui.BURN_APPROACH_STOP,
	APPROACH_STOP_FORMAT = "%.0f s",
	APPROACH_STOP_TIP = lui.BURN_APPROACH_STOP_TIP,
	APPROACH_STATUS = lui.BURN_APPROACH_STATUS,
	APPROACH_NONE = lui.BURN_APPROACH_NONE,
	APPROACH_PASSED = lui.BURN_APPROACH_PASSED,
	ACCURACY_LABEL = lui.BURN_ACCURACY,
	ACCURACY_FORMAT = "%.1f %%",
	ACCURACY_TIP = lui.BURN_ACCURACY_TIP,
	-- each tooltip states only what its own control does. The two controls are
	-- genuinely independent: Auto Orientation is a flight control state, Auto
	-- Burn an engine flag applied on top of it; setting one never sets the
	-- other, and the tooltips say exactly that (plan Phase 3 re-architecture).
	AUTO_ORIENTATION = lui.BURN_AUTO_ORIENTATION_TIP,
	AUTO_BURN = lui.BURN_AUTO_BURN_TIP,
	NEEDS_AUTOPILOT = lui.BURN_NEEDS_AUTOPILOT,
	-- textual state indicator for the two toggles: the pressed state must read
	-- unambiguously, not only as a subtle button-colour shift
	STATE_ORIENTATION = lui.BURN_AUTO_ORIENTATION,
	STATE_BURN = lui.BURN_AUTO_BURN,
	STATE_ON = lui.BURN_STATE_ON,
	STATE_OFF = lui.BURN_STATE_OFF,
	-- the state line is "Label: ON" / "Label: OFF"
	STATE_LINE = lui.BURN_STATE_LINE,
	SHOW = lui.BURN_LIST_SHOW,
	HIDE = lui.BURN_LIST_HIDE,
}

-- module state: visibility is persisted; everything else is derived per frame
local visible = false
local visibilityLoaded = false

-- visibility is restored from the game settings, default hidden; read at load
-- and re-read at game start, because the settings are reloaded with the save
local function loadVisibility()
	visible = Game.GetConfigBool("MultiBurnHud", "visible") and true or false
	visibilityLoaded = true
end

--
-- Function: loadStopBeforeBurn, saveStopBeforeBurn
--
-- The "Stop Before Burn" margin: whole seconds, shared by all burns, default
-- 10, measured from the burn START (half a burn duration before the planned
-- node time, see burnStartTime), persisted in the game settings. GetConfigInt has no "unset" signal (it
-- answers 0 for a missing key), so the value is stored biased by 1: an unset
-- key reads 0 and falls back to the default, while a stored 0 round-trips.
--
local DEFAULT_STOP_BEFORE_BURN_S = 10
local OFFSET_MAX = 3600
local stopBeforeBurnS = DEFAULT_STOP_BEFORE_BURN_S

local function loadStopBeforeBurn()
	local stored = Game.GetConfigInt("MultiBurnHud", "stopBeforeBurnS")
	stopBeforeBurnS = (stored > 0) and (stored - 1) or DEFAULT_STOP_BEFORE_BURN_S
end

local function saveStopBeforeBurn()
	Game.SetConfigInt("MultiBurnHud", "stopBeforeBurnS", stopBeforeBurnS + 1)
	Engine.SaveSettings()
end

--
-- Function: loadBurnSettings, saveBurnSettings
--
-- What the warp does on arrival (pause, the default, or carry on at 1x) and
-- Auto Burn's accuracy, persisted in the game settings like the margin above.
-- The accuracy is in tenths of a percent, stored biased by 1 for the same
-- reason, default 1 %; the ship controller gets it on every draw (see
-- displayBurnHud), so it holds across saves and new ships.
--
local DEFAULT_ACCURACY_PERMILLE = 10
local ACCURACY_MIN_PERMILLE = 1
local ACCURACY_MAX_PERMILLE = 200
local accuracyPermille = DEFAULT_ACCURACY_PERMILLE
local arriveAtNormalSpeed = false
-- stopping before the closest approach to the planned approach's target:
-- whether, and how many seconds before (stored biased by 1, default 60)
local DEFAULT_APPROACH_STOP_S = 60
local APPROACH_STOP_MAX_S = 3600
local approachStopEnabled = false
local approachStopS = DEFAULT_APPROACH_STOP_S

local function loadBurnSettings()
	arriveAtNormalSpeed = Game.GetConfigBool("MultiBurnHud", "arriveAtNormalSpeed") and true or false
	local stored = Game.GetConfigInt("MultiBurnHud", "accuracyPermille")
	accuracyPermille = (stored > 0) and (stored - 1) or DEFAULT_ACCURACY_PERMILLE
	accuracyPermille = math.min(math.max(accuracyPermille, ACCURACY_MIN_PERMILLE), ACCURACY_MAX_PERMILLE)
	approachStopEnabled = Game.GetConfigBool("MultiBurnHud", "approachStopEnabled") and true or false
	stored = Game.GetConfigInt("MultiBurnHud", "approachStopS")
	approachStopS = (stored > 0) and math.min(stored - 1, APPROACH_STOP_MAX_S) or DEFAULT_APPROACH_STOP_S
end

local function saveBurnSettings()
	Game.SetConfigBool("MultiBurnHud", "arriveAtNormalSpeed", arriveAtNormalSpeed)
	Game.SetConfigInt("MultiBurnHud", "accuracyPermille", accuracyPermille + 1)
	Game.SetConfigBool("MultiBurnHud", "approachStopEnabled", approachStopEnabled)
	Game.SetConfigInt("MultiBurnHud", "approachStopS", approachStopS + 1)
	Engine.SaveSettings()
end

--
-- Burn centring: a planned burn is applied at its node time, but a real burn
-- takes d = Δv / acceleration seconds. The burn is split 50:50 around the node,
-- so it starts d/2 before and ends d/2 after the node time. The planned
-- (not remaining) Δv is used so the start does not drift as Δv is consumed;
-- with no usable acceleration the burn start falls back to the node time.
--
local function halfBurnDuration(dv, acceleration)
	if not dv or not acceleration or acceleration <= 0 then return 0 end
	return dv / acceleration / 2
end

local function burnStartTime(nodeTime, dv, acceleration)
	return nodeTime - halfBurnDuration(dv, acceleration)
end

-- warp target: the burn start minus the configured margin
local function warpTargetFor(systemView, idx, acceleration)
	local node = systemView:TransferPlannerGet(idx, "starttime")
	if not node then return nil end
	local dv = systemView:TransferPlannerGetBurnDeltaV(idx)
	return burnStartTime(node, dv, acceleration) - stopBeforeBurnS
end

--
-- Warp to burn (Phase 5).
--
-- One warp at a time, in module state: { idx, target, prevAccel,
-- lastRequest, armed }. target = burn start − offset; the module drives
-- Game.SetTimeAcceleration(level, true) down a staircase as the target
-- nears, and pauses (or drops to 1x) on arrival. It never un-pauses the simulation: resume
-- happens only through an explicit user action (Cancel Warp), following the
-- pause menu's restore pattern in views/game.lua.
--
local warp = nil

-- physics time-step per level: Game::s_timeAccelRates / PHYSICS_HZ
-- (src/Game.cpp, src/gameconsts.h) — the same rate table the game publishes
local PHYSICS_HZ = 60
local LEVEL_RATES = { ["1x"] = 1, ["10x"] = 10, ["100x"] = 100, ["1000x"] = 1000, ["10000x"] = 10000 }

-- the staircase: from each level, drop one when the remaining game time
-- falls below this many physics steps of that level, so the final approach
-- step (1x → 1/60 s) is smaller than any non-negative offset
local STAIRCASE = {
	{ level = "10000x", steps = 20 },
	{ level = "1000x", steps = 10 },
	{ level = "100x", steps = 3 },
	{ level = "10x", steps = 1 },
}

local function desiredLevel(remaining)
	for _, rung in ipairs(STAIRCASE) do
		if remaining >= rung.steps * (LEVEL_RATES[rung.level] / PHYSICS_HZ) then
			return rung.level
		end
	end
	return "1x"
end

-- the pause menu's snapshot, views/game.lua: the requested level when it
-- differs from the applied one, else the applied level
local function snapshotAccel()
	local current = Game.GetTimeAcceleration()
	local requested = Game.GetRequestedTimeAcceleration()
	return (current ~= requested) and requested or current
end

--
-- Function: arrive
--
-- Arrival: request the pause (a paused request is sticky), or normal speed
-- when the player chose to carry on at 1x, and clear the warp state. Called
-- from the armed Timer callback on a physics tick and from the per-draw check;
-- whichever runs first wins, the second is a no-op.
--
local function arrive()
	if not warp then return end
	Game.SetTimeAcceleration(arriveAtNormalSpeed and "1x" or "paused")
	warp = nil
end

--
-- Function: cancelWarp
--
-- Explicit user cancel: restore the saved level and leave the simulation
-- running (the pause-menu restore pattern: a "paused" snapshot resumes as 1x,
-- mirroring views/game.lua's onPauseMenuClosed).
--
local function cancelWarp()
	if not warp then return end
	local resume = warp.prevAccel
	warp = nil
	if resume == "paused" then resume = "1x" end
	Game.SetTimeAcceleration(resume)
	Input.EnableBindings()
end

--
-- Function: updateWarp
--
-- Drives the staircase once per draw. The per-draw arrival check is only a
-- supplement — under acceleration a physics tick can cross the target between
-- draws, which the armed Timer callback catches.
--
-- Manual interruption rule: when the observed requested level differs from
-- this module's last request, the player (or the engine's own clamp rewrite
-- for hyperspace/docking) has taken control of the clock — treat it as their
-- intent, stop driving, and never re-assert. Engine proximity / firing /
-- rotation clamps keep our request, so they only show as the clamped
-- indicator and are respected, not fought.
--
local function updateWarp()
	if not warp then return end

	if Game.time >= warp.target then
		arrive()
		return
	end

	local requested = Game.GetRequestedTimeAcceleration()
	if warp.lastRequest and requested ~= warp.lastRequest then
		warp = nil
		return
	end

	local level = desiredLevel(warp.target - Game.time)
	if level ~= requested then
		Game.SetTimeAcceleration(level, true)
	end
	-- the level we want the clock at, requested here or already requested by
	-- someone else when it coincided with ours — the manual-interruption
	-- guard compares against this on the next draw
	warp.lastRequest = level
end

--
-- Function: startWarp
--
-- The Warp row control. No backward warp: a target at or before now does
-- nothing. A warp for a different index first cancels the running one. The
-- arrival Timer is armed exactly here (never per frame) and only for a
-- strictly-future target — CallAt raises an error on a past target.
--
local function startWarp(idx)
	local systemView = Game.systemView
	if not systemView then return false end

	local count = systemView:TransferPlannerGetBurnCount()
	if idx < 0 or idx >= count then return false end
	local start = systemView:TransferPlannerGet(idx, "starttime")
	if not start then return false end

	local target = warpTargetFor(systemView, idx, Game.player and Game.player:GetAcceleration("forward"))
	if not target or target <= Game.time then return false end

	if warp then
		cancelWarp()
	end

	local session = {
		idx = idx,
		target = target,
		prevAccel = snapshotAccel(),
		lastRequest = nil,
		armed = false,
	}
	warp = session

	-- request the first staircase level immediately, so the climb starts on
	-- this frame's application pass
	updateWarp()

	if warp == session and target > Game.time then
		Timer:CallAt(target, function()
			-- the callback outlives cancel/switch: only act for our own warp
			if warp == session then
				arrive()
			end
		end)
		session.armed = true
	end

	return true
end

--
-- Stop before the closest approach.
--
-- While enabled, the closest approach of the plan to the target of the last
-- planned approach (pigui.libs.approach-target) is watched: time acceleration
-- is stepped down the warp's staircase as the stop time (closest approach -
-- approachStopS) nears - never up, so a slower clock the player chose is
-- kept - and at the stop time the clock pauses or drops to 1x, the arrival
-- choice the burn warp uses. Each approach stops the clock once. The closest
-- approach is recomputed every few draws only: it holds still while coasting.
--
local APPROACH_REFRESH_DRAWS = 15
local approach = { target = nil, time = nil, distance = nil, radius = 0, draws = 0, stoppedFor = nil }

local function updateApproachStop(systemView)
	local target = ApproachTarget.Get()
	if target ~= approach.target then
		approach.target, approach.time, approach.draws = target, nil, 0
	end
	if not target then return end

	approach.draws = approach.draws - 1
	if approach.draws <= 0 then
		approach.draws = APPROACH_REFRESH_DRAWS
		local distance, time, _, radius = systemView:TransferPlannerGetClosestApproach(target)
		approach.distance = distance
		approach.time = distance and time or nil
		approach.radius = distance and radius or 0
	end
	if not approachStopEnabled or not approach.time then return end

	local stopAt = approach.time - approachStopS
	-- this approach has stopped the clock already
	if approach.stoppedFor and math.abs(approach.stoppedFor - approach.time) < 60 then return end

	local remaining = stopAt - Game.time
	if remaining <= 0 then
		-- a stop time reached before the closest approach stops the clock,
		-- not one for an approach already passed
		if Game.time < approach.time then
			approach.stoppedFor = approach.time
			Game.SetTimeAcceleration(arriveAtNormalSpeed and "1x" or "paused")
		end
		return
	end

	local requested = Game.GetRequestedTimeAcceleration()
	local level = desiredLevel(remaining)
	if LEVEL_RATES[requested] and LEVEL_RATES[requested] > LEVEL_RATES[level] then
		Game.SetTimeAcceleration(level, true)
	end
end

--
-- Function: hasAutopilot
--
-- The same capability test the game's own UI gates on (libs/radial-menu.lua),
-- re-checked every frame so equipment loss mid-burn greys the controls.
--
local function hasAutopilot(player)
	return (player["autopilot_cap"] or 0) >= 1
end

-- the widget belongs to the flight HUD only: not in menus, not in the map views
local function shouldShowHud()
	if ui.optionsWindow.isOpen then return false end
	if not Game.player then return false end
	if Game.CurrentView() ~= "WorldView" then return false end
	if not ui.shouldDrawUI() then return false end
	return true
end

--
-- Estimated burn duration: Δv / forward acceleration. Shared by the burn time
-- cell and the plan summary; the same estimate the reticule shows
-- (flight-ui/reticule.lua).
--
local function estimateDuration(dv, acceleration)
	if not dv or not acceleration or acceleration <= 0 then return nil end
	return dv / acceleration
end

local function formatDuration(seconds)
	if not seconds then return UNAVAILABLE end
	return ui.Format.Duration(math.ceil(seconds), 2)
end

local function formatSpeed(dv)
	if not dv then return UNAVAILABLE end
	local speed, unit = ui.Format.SpeedUnit(dv)
	return speed .. " " .. unit
end

--
-- Countdowns read as a clock, so a column of them lines up and ticks
-- visibly: m:ss, h:mm:ss, or days plus h:mm:ss.
--
local function formatClock(seconds)
	local s = math.max(0, math.floor(seconds))
	local days = math.floor(s / 86400)
	local hours = math.floor(s / 3600) % 24
	local minutes = math.floor(s / 60) % 60
	s = s % 60
	if days > 0 then
		return string.format("%dd %d:%02d:%02d", days, hours, minutes, s)
	elseif hours > 0 then
		return string.format("%d:%02d:%02d", hours, minutes, s)
	end
	return string.format("%d:%02d", minutes, s)
end

-- countdown to a game time, "NOW" once it is reached
local function formatCountdown(time)
	if not time then return UNAVAILABLE end
	if time > Game.time then
		return formatClock(time - Game.time)
	end
	return lc.NOW
end

local status_glyph = {
	[BURN_STATUS_PENDING] = icons.burn_pending,
	[BURN_STATUS_ACTIVE] = icons.burn_active,
	[BURN_STATUS_COMPLETED] = icons.burn_completed,
}

local status_name = {
	[BURN_STATUS_PENDING] = L.STATUS_PENDING,
	[BURN_STATUS_ACTIVE] = L.STATUS_ACTIVE,
	[BURN_STATUS_COMPLETED] = L.STATUS_COMPLETED,
}

--
-- Function: collectRows
--
-- Read-only snapshot of the plan for this frame; every value comes from a
-- planner call and nothing here is kept between frames. Returns the rows, the
-- active index and the totals of what is left of the plan.
--
-- The planner stores only COMPLETED. The displayed status is derived exactly as
-- the map sidebar derives it (modules/system-view-ui.lua: statusOf): COMPLETED
-- as stored, ACTIVE for the planner's active burn (TransferPlannerGetActiveIndex),
-- PENDING for every other burn - so the status glyph, the active-row highlight
-- and the sidebar's row status can never disagree.
--
local function collectRows(systemView, acceleration)
	local count = systemView:TransferPlannerGetBurnCount()
	local activeIndex = systemView:TransferPlannerGetActiveIndex()
	local rows = {}
	local totals = { burns = 0, dv = 0, duration = 0 }
	local prevEnd = nil

	for idx = 0, count - 1 do
		local stored = systemView:TransferPlannerGetBurnStatus(idx)
		local completed = stored == BURN_STATUS_COMPLETED
		local status = completed and BURN_STATUS_COMPLETED
			or ((idx == activeIndex) and BURN_STATUS_ACTIVE or BURN_STATUS_PENDING)
		local dv = systemView:TransferPlannerGetBurnDeltaV(idx)
		local remaining = systemView:TransferPlannerGetBurnRemainingDeltaV(idx)
		local node = systemView:TransferPlannerGet(idx, "starttime")
		local ignition = node and burnStartTime(node, dv, acceleration) or nil
		-- the Warp control's availability is startWarp's own rule, computed from
		-- the same planner value: a target at or before now never warps
		local warpTarget = warpTargetFor(systemView, idx, acceleration)
		-- chained burns: this burn's window overlaps the previous one's when it
		-- starts before the previous burn ends (node + half duration)
		local overlap = (prevEnd ~= nil and ignition ~= nil and ignition < prevEnd) and (prevEnd - ignition) or nil
		prevEnd = node and (node + halfBurnDuration(dv, acceleration)) or prevEnd

		local timeLeft = estimateDuration(remaining, acceleration)
		if not completed then
			totals.burns = totals.burns + 1
			totals.dv = totals.dv + remaining
			totals.duration = totals.duration and timeLeft and (totals.duration + timeLeft) or nil
		end

		table.insert(rows, {
			idx = idx,
			status = status,
			completed = completed,
			dv = dv,
			remaining = remaining,
			-- the share already flown, for the Δv cell's progress bar
			progress = (not completed and dv > 0) and math.min(math.max(1 - remaining / dv, 0), 1) or 0,
			ignition = ignition,
			node = node,
			duration = estimateDuration(dv, acceleration),
			timeLeft = timeLeft,
			overlap = overlap,
			warppable = (warpTarget ~= nil) and (warpTarget > Game.time),
		})
	end

	return rows, activeIndex, totals
end

-- table columns, in order
local COL_STATUS, COL_LABEL, COL_DV, COL_IGNITION, COL_NODE, COL_DURATION, COL_ACTIONS = 1, 2, 3, 4, 5, 6, 7
local COL_COUNT = COL_ACTIONS

local columnCaptions = {
	[COL_STATUS] = "",
	[COL_LABEL] = L.CAPTION_BURN,
	[COL_DV] = L.CAPTION_DV,
	[COL_IGNITION] = L.CAPTION_IGNITION,
	[COL_NODE] = L.CAPTION_NODE,
	[COL_DURATION] = L.CAPTION_DURATION,
	[COL_ACTIONS] = "",
}

local columnTips = {
	[COL_DV] = L.CAPTION_DV_TIP,
	[COL_IGNITION] = L.CAPTION_IGNITION_TIP,
	[COL_NODE] = L.CAPTION_NODE_TIP,
	[COL_DURATION] = L.CAPTION_DURATION_TIP,
}

-- numeric columns are right-aligned, captions included, so a caption sits over
-- the digits it labels
local rightAlignedColumn = {
	[COL_DV] = true, [COL_IGNITION] = true, [COL_NODE] = true, [COL_DURATION] = true,
}

--
-- Function: measureColumns
--
-- Fixed column widths from worst-case strings, measured in the font each
-- column draws in, so the panel keeps one width while values tick: "Burn 8",
-- a four-digit km/s, a countdown in days, a burn time of an hour. Longer
-- values clip in their cell. The status and action columns hold icons sized
-- to the data line.
--
local function measureColumns(lineHeight)
	local spacing = ui.getItemSpacing().x
	local widths = {}

	ui.withFont(dataFont.name, dataFont.size, function()
		local function widest(...)
			local w = 0
			for _, s in ipairs({...}) do w = math.max(w, ui.calcTextSize(s).x) end
			return w
		end
		widths[COL_LABEL] = widest(string.format(L.BURN, MAX_BURN_COUNT))
		widths[COL_DV] = widest(formatSpeed(999990), formatSpeed(-999), UNAVAILABLE)
		local clock = widest(formatClock(99 * 86400 + 23 * 3600 + 59 * 60 + 59),
			formatClock(88 * 86400 + 18 * 3600 + 58 * 60 + 58), lc.NOW, L.DONE, UNAVAILABLE)
		widths[COL_IGNITION] = clock
		widths[COL_NODE] = clock
		widths[COL_DURATION] = widest(formatDuration(59 * 60 + 59), formatDuration(23 * 3600 + 59 * 60), UNAVAILABLE)
	end)

	widths[COL_STATUS] = lineHeight
	widths[COL_ACTIONS] = lineHeight * 2 + spacing

	ui.withFont(captionFont.name, captionFont.size, function()
		for col = 1, COL_COUNT do
			widths[col] = math.max(widths[col], ui.calcTextSize(columnCaptions[col]).x)
		end
	end)

	return widths
end

-- text in the current font, pushed to the right edge of the current cell
local function textRight(text, color)
	local width = ui.calcTextSize(text).x
	ui.addCursorPos(Vector2(math.max(0, ui.getContentRegion().x - width), 0))
	ui.textColored(color, text)
end

--
-- Function: drawProgress
--
-- A thin bar under the Δv value, filled by the share of the burn already
-- flown. It sits in the cell's bottom padding, so it never moves the text.
--
local function drawProgress(fraction, width, lineHeight)
	local pos = ui.getCursorScreenPos()
	local thickness = math.max(2, math.floor(lineHeight / 10))
	local top = Vector2(pos.x, pos.y + lineHeight)
	ui.addRectFilled(top, top + Vector2(width, thickness), colors.burnHudProgressTrack, 0, 0)
	ui.addRectFilled(top, top + Vector2(width * fraction, thickness), colors.burnHudProgress, 0, 0)
end

local function drawHeaderRow()
	ui.tableNextRow()
	ui.withFont(captionFont.name, captionFont.size, function()
		for col = 1, COL_COUNT do
			ui.tableNextColumn()
			local caption = columnCaptions[col]
			if caption ~= "" then
				if rightAlignedColumn[col] then
					textRight(caption, colors.fontDim)
				else
					ui.textColored(colors.fontDim, caption)
				end
				if columnTips[col] then
					ui.setItemTooltip(columnTips[col])
				end
			end
		end
	end)
end

-- the row action buttons: flat until hovered, so the column does not read as
-- a row of checkboxes; a greyed control keeps its slot with a dimmed glyph
local actionVariant = ui.theme.buttonColors.transparent
local disabledActionVariant = {
	normal = colors.transparent, hovered = colors.transparent, active = colors.transparent,
}

local function drawActions(row, systemView, slot)
	local spacing = ui.getItemSpacing().x
	-- Complete is drawn for the active burn and for pending burns, never on a
	-- completed row; the slot stays reserved so Warp keeps the same x
	if row.completed then
		ui.dummy(slot)
	elseif ui.iconButton("##burnHudComplete" .. row.idx, icons.burn_completed, L.COMPLETE,
		actionVariant, slot, styles.InlineIconPadding) then
		systemView:TransferPlannerCompleteBurn(row.idx)
	end

	ui.sameLine(0, spacing)
	-- Warp starts the Phase 5 warp state machine; it is greyed with "Target is
	-- already reached" when its target (burn start − margin) is at or before
	-- now - the exact rule startWarp() applies, evaluated on the row data
	local warpTip = row.warppable and string.format(L.WARP, stopBeforeBurnS) or L.WARP_REACHED
	local variant = row.warppable and actionVariant or disabledActionVariant
	ui.withStyleColors({ Text = row.warppable and colors.font or colors.fontDim }, function()
		if ui.iconButton("##burnHudWarp" .. row.idx, icons.warp_to_burn, warpTip,
			variant, slot, styles.InlineIconPadding) and row.warppable then
			startWarp(row.idx)
		end
	end)
end

--
-- Function: drawBurnRow
--
-- One burn: status glyph, label, Δv left (planned once complete) with its
-- progress bar, ignition and node countdowns, burn time left, actions. The
-- active burn's row is highlighted and its values take the maneuver colour
-- the reticle uses for the same burn; a completed row is dimmed.
--
local function drawBurnRow(row, isActive, systemView, lineHeight)
	ui.tableNextRow()
	if isActive then
		ui.tableSetBgColor("RowBg1", colors.burnHudActiveRowBg)
	end

	local textColor = row.completed and colors.burnHudCompletedText or colors.font
	local valueColor = isActive and colors.maneuver or textColor
	local glyphColor = isActive and colors.maneuver or colors.fontDim

	ui.tableNextColumn()
	ui.icon(status_glyph[row.status] or icons.empty, Vector2(lineHeight), glyphColor, status_name[row.status])

	ui.tableNextColumn()
	ui.textColored(textColor, string.format(L.BURN, row.idx + 1))
	ui.setItemTooltip(status_name[row.status] or UNAVAILABLE)

	ui.tableNextColumn()
	if row.progress > 0 then
		drawProgress(row.progress, ui.getContentRegion().x, lineHeight)
	end
	textRight(formatSpeed(row.completed and row.dv or row.remaining), valueColor)
	ui.setItemTooltip(string.format(L.DV_TIP, formatSpeed(row.dv),
		formatSpeed(math.max(0, row.dv - row.remaining)), formatSpeed(row.remaining)))

	ui.tableNextColumn()
	if row.completed then
		textRight(L.DONE, textColor)
	else
		textRight(formatCountdown(row.ignition), row.overlap and colors.alertYellow or valueColor)
		if row.overlap then
			ui.setItemTooltip(string.format(L.OVERLAP, formatDuration(row.overlap), row.idx))
		end
	end

	ui.tableNextColumn()
	textRight(row.completed and UNAVAILABLE or formatCountdown(row.node), row.completed and textColor or colors.fontDim)

	ui.tableNextColumn()
	textRight(row.completed and UNAVAILABLE or formatDuration(row.timeLeft), valueColor)
	if not row.completed then
		ui.setItemTooltip(row.duration and string.format(L.DURATION_TIP, formatDuration(row.duration),
			formatDuration(row.timeLeft)) or L.NO_THRUST)
	end

	ui.tableNextColumn()
	drawActions(row, systemView, Vector2(lineHeight, lineHeight))
end

local function drawBurnTable(rows, activeIndex, systemView)
	local lineHeight = ui.getTextLineHeight()
	local widths = measureColumns(lineHeight)

	if not ui.beginTable("##burnHudTable", COL_COUNT, tableFlags) then return end
	for col = 1, COL_COUNT do
		ui.tableSetupColumn("##burnHudCol" .. col, { "WidthFixed" }, widths[col])
	end

	drawHeaderRow()
	for _, row in ipairs(rows) do
		-- the planner is the live list: an action on an earlier row (complete,
		-- warp) never removes a burn, but stop at the live count regardless
		if row.idx >= systemView:TransferPlannerGetBurnCount() then break end
		drawBurnRow(row, row.idx == activeIndex, systemView, lineHeight)
	end

	ui.endTable()
end

--
-- Function: drawTitle
--
-- "BURN PLAN" with, right-aligned, what is left of it: burns to go, the Δv
-- they still need and their burn time.
--
local function drawTitle(totals, hasRows)
	ui.withFont(titleFont.name, titleFont.size, function()
		ui.textColored(colors.fontDim, L.TITLE)
	end)
	if not hasRows then return end

	-- the plan's Δv of the ship's: in the alert colour when the fuel falls short
	local available = Game.player:GetRemainingDeltaV()
	local summary = totals.burns > 0
		and string.format(L.SUMMARY, totals.burns, formatSpeed(totals.dv), formatSpeed(available), formatDuration(totals.duration))
		or L.ALL_DONE
	local color = colors.fontDim
	if totals.burns > 0 then
		color = (totals.dv > available) and colors.alertRed or colors.maneuver
	end
	ui.sameLine()
	ui.withFont(captionFont.name, captionFont.size, function()
		textRight(summary, color)
	end)
	ui.setItemTooltip(L.SUMMARY_TIP)
end


--
-- Function: drawAutopilotControls
--
-- Two genuinely independent controls, backed by two independent pieces of game
-- state (plan Phase 3 re-architecture):
--   Auto Orientation is the ship's flight control state:
--     CONTROL_MANUAL / CONTROL_PLAN_HEADING - a state machine, exclusive by
--     nature, read and written through Get/SetFlightControlState.
--   Auto Burn is an engine enable-flag on the ship controller, orthogonal to
--   the flight control state, read and written through Get/SetPlanBurnEnabled.
-- A click writes exactly its own control; no coupling term exists anywhere, so
-- setting one never sets the other.
--
-- Display rule — strict 1:1:
--   Auto Orientation on  <=>  player:GetFlightControlState() == CONTROL_PLAN_HEADING
--   Auto Burn on         <=>  player:GetPlanBurnEnabled()
-- The ON/OFF text indicators follow these two booleans unchanged. Both start
-- unpressed: the default control state is CONTROL_MANUAL and the engine flag
-- defaults to off.
--
-- Click matrix:
--   Auto Orientation, CONTROL_PLAN_HEADING -> CONTROL_MANUAL (releases)
--   Auto Orientation, any other state      -> CONTROL_PLAN_HEADING
--     (orientation only; the engine flag is untouched, so Auto Burn keeps
--     running - it fires only while the ship is aligned, which this control
--     now provides)
--   Auto Burn, enabled -> disabled (the engine stops thrusting at once)
--   Auto Burn, disabled -> enabled (no SetFlightControlState call: this
--     control never touches orientation; unaligned it produces no thrust)
--
local function drawAutopilotControls(player)
	local available = hasAutopilot(player)
	-- the two booleans come from the two independent sources, nothing else
	local orientationOn = (player:GetFlightControlState() == "CONTROL_PLAN_HEADING")
	local burnOn = player:GetPlanBurnEnabled()

	-- pressed = the theme's selected (pressed/active) button colours — the same
	-- mainMenuButton(true) pattern the time window uses for the engaged
	-- acceleration level — unpressed = the default variant, and greyed out
	-- when the ship has no autopilot equipment (per-frame check)
	local greyed = ui.theme.buttonColors.disabled
	local orientationVariant = (not available) and greyed or orientationOn
	local burnVariant = (not available) and greyed or burnOn

	-- this control owns the orientation control state only
	local orientationTip = L.AUTO_ORIENTATION .. "##burnHudAutoOrientation"
	if ui.mainMenuButton(icons.auto_orientation, orientationTip, orientationVariant, styles.MainButtonSize) then
		if available then
			-- PLAN_HEADING -> MANUAL, any other state -> PLAN_HEADING; never
			-- touches the engine flag.
			player:SetFlightControlState(orientationOn and "CONTROL_MANUAL" or "CONTROL_PLAN_HEADING")
		end
	end
	if not available then
		ui.setItemTooltip(L.NEEDS_AUTOPILOT)
	end

	ui.sameLine()

	local burnTip = L.AUTO_BURN .. "##burnHudAutoBurn"
	if ui.mainMenuButton(icons.auto_burn, burnTip, burnVariant, styles.MainButtonSize) then
		if available then
			-- the engine enable-flag only: never a SetFlightControlState call,
			-- so this control cannot change the ship's orientation
			player:SetPlanBurnEnabled(not burnOn)
		end
	end
	if not available then
		ui.setItemTooltip(L.NEEDS_AUTOPILOT)
	end

	-- explicit textual state indicator on the same line: "Auto Orientation:
	-- ON/OFF", "Auto Burn: ON/OFF" — the button-colour shift alone was too
	-- subtle to read. Width/height of this row are reserved by
	-- measureAutopilotRow().
	ui.sameLine()
	ui.withFont(dataFont.name, dataFont.size, function()
		ui.alignTextToLineHeight(styles.MainButtonSize.y)
		ui.textColored(orientationOn and colors.burnHudToggleOnText or colors.burnHudToggleOffText,
			string.format(L.STATE_LINE, L.STATE_ORIENTATION, orientationOn and L.STATE_ON or L.STATE_OFF))
		ui.sameLine()
		ui.textColored(burnOn and colors.burnHudToggleOnText or colors.burnHudToggleOffText,
			string.format(L.STATE_LINE, L.STATE_BURN, burnOn and L.STATE_ON or L.STATE_OFF))
	end)
end

--
-- Function: drawOffsetControl
--
-- "Stop before burn": the pre-burn offset, shared by all burns, as a drag the
-- player can nudge, drag and type into. Clamped to non-negative whole seconds
-- and persisted with the game settings. The always-drawn toggle window keeps
-- this module alive, so the value survives while the panel is hidden.
--
local OFFSET_DRAG_WIDTH = dataFont.size * 5

local function drawOffsetControl()
	local value, changed
	ui.withFont(dataFont.name, dataFont.size, function()
		ui.alignTextToFramePadding()
		ui.textColored(colors.fontDim, L.OFFSET_LABEL)
		ui.setItemTooltip(L.OFFSET_TIP)

		ui.sameLine()
		ui.pushItemWidth(OFFSET_DRAG_WIDTH)
		value, changed = ui.incrementDrag("##burnHudStopBeforeBurn", stopBeforeBurnS, 1, 0, OFFSET_MAX, L.OFFSET_FORMAT, false)
		ui.popItemWidth()
	end)
	if changed then
		stopBeforeBurnS = math.min(math.max(math.floor(value), 0), OFFSET_MAX)
		saveStopBeforeBurn()
	end
end

--
-- Function: drawArrivalChoice
--
-- On the margin's line: what the warp does when it arrives - Pause or 1x -
-- as two buttons, the chosen one highlighted.
--
local function drawArrivalChoice()
	local choice = nil
	ui.withFont(dataFont.name, dataFont.size, function()
		ui.sameLine()
		ui.textColored(colors.fontDim, L.ARRIVE_LABEL)
		ui.sameLine()
		local selected = ui.theme.buttonColors.selected
		if ui.button(L.ARRIVE_PAUSE .. "##burnHudArrivePause", nil,
			not arriveAtNormalSpeed and selected or nil, L.ARRIVE_PAUSE_TIP) then
			choice = false
		end
		ui.sameLine(0, 0)
		if ui.button(L.ARRIVE_NORMAL .. "##burnHudArriveNormal", nil,
			arriveAtNormalSpeed and selected or nil, L.ARRIVE_NORMAL_TIP) then
			choice = true
		end
	end)
	if choice ~= nil and choice ~= arriveAtNormalSpeed then
		arriveAtNormalSpeed = choice
		saveBurnSettings()
	end
end

--
-- Function: drawAccuracyControl
--
-- "Burn accuracy": Auto Burn's accuracy in percent, a drag like the margin's.
--
local function drawAccuracyControl()
	local value, changed
	ui.withFont(dataFont.name, dataFont.size, function()
		ui.alignTextToFramePadding()
		ui.textColored(colors.fontDim, L.ACCURACY_LABEL)
		ui.setItemTooltip(L.ACCURACY_TIP)

		ui.sameLine()
		ui.pushItemWidth(OFFSET_DRAG_WIDTH)
		value, changed = ui.incrementDrag("##burnHudAccuracy", accuracyPermille / 10, 0.1,
			ACCURACY_MIN_PERMILLE / 10, ACCURACY_MAX_PERMILLE / 10, L.ACCURACY_FORMAT, false)
		ui.popItemWidth()
		ui.setItemTooltip(L.ACCURACY_TIP)
	end)
	if changed then
		accuracyPermille = math.min(math.max(math.floor(value * 10 + 0.5), ACCURACY_MIN_PERMILLE), ACCURACY_MAX_PERMILLE)
		saveBurnSettings()
	end
end

--
-- Function: drawApproachStopControl
--
-- "Stop before closest approach": the switch and the seconds, then the
-- watched approach - target, distance (altitude for a planet or moon) and
-- the time to it.
--
local function drawApproachStopControl()
	local enabledChanged, enabled, value, changed
	ui.withFont(dataFont.name, dataFont.size, function()
		enabledChanged, enabled = ui.checkbox(L.APPROACH_STOP .. "##burnHudApproachStop", approachStopEnabled)
		ui.setItemTooltip(L.APPROACH_STOP_TIP)
		ui.sameLine()
		ui.pushItemWidth(OFFSET_DRAG_WIDTH)
		value, changed = ui.incrementDrag("##burnHudApproachStopS", approachStopS, 1, 0, APPROACH_STOP_MAX_S, L.APPROACH_STOP_FORMAT, false)
		ui.popItemWidth()
		ui.setItemTooltip(L.APPROACH_STOP_TIP)

		local status = L.APPROACH_NONE
		if approach.target then
			local name = approach.target:GetLabel()
			if approach.time and approach.time > Game.time then
				local distance = approach.radius > 0 and (approach.distance - approach.radius) or approach.distance
				status = string.format(L.APPROACH_STATUS, name, ui.Format.Distance(distance), formatCountdown(approach.time))
			else
				status = string.format(L.APPROACH_PASSED, name)
			end
		end
		ui.textColored(colors.fontDim, status)
	end)
	if enabledChanged then
		approachStopEnabled = enabled
		saveBurnSettings()
	end
	if changed then
		approachStopS = math.min(math.max(math.floor(value), 0), APPROACH_STOP_MAX_S)
		saveBurnSettings()
	end
end

--
-- Function: drawWarpStrip
--
-- While a warp is running: the target burn number, the configured offset, the
-- acceleration level this module last requested, the clamped indicator (the
-- time window's requested-vs-applied pattern, modules/time-window.lua), and
-- the Cancel Warp control. The engine's hard clamps (hyperspace, docking,
-- being fired on) rewrite the request - which hands the clock back to the
-- engine and ends the warp - while proximity clamps keep it; both show up on
-- this line, and neither is fought.
--
local function drawWarpStrip()
	local level = warp.lastRequest or Game.GetRequestedTimeAcceleration()
	local clamped = Game.GetTimeAcceleration() ~= Game.GetRequestedTimeAcceleration()
	local text = string.format(clamped and L.WARP_STRIP_LIMITED or L.WARP_STRIP, warp.idx + 1, stopBeforeBurnS, level)

	ui.withFont(dataFont.name, dataFont.size, function()
		ui.textColored(colors.maneuver, text)
		ui.sameLine()
		if ui.inlineIconButton("##burnHudCancelWarp", icons.cross, L.CANCEL_WARP) then
			cancelWarp()
		end
	end)
end

--
-- Function: displayBurnHudPanel
--
-- The panel: title and plan summary, the burn table, then the autopilot
-- toggles, the warp strip while a warp runs, and the stop-before-burn margin.
-- The window sizes itself to that content (every table column is fixed-width,
-- so the width only changes with the screen) and is anchored by its
-- bottom-right corner to the left of the right-edge button column, at the
-- bottom of the HUD area above the time window, so it grows up and to the
-- left into space no other HUD element claims.
--
local function displayBurnHudPanel(min, max, player, systemView, toggleSize)
	local acceleration = player:GetAcceleration("forward")
	local rows, activeIndex, totals = collectRows(systemView, acceleration)
	local hasRows = #rows > 0

	local spacing = ui.getItemSpacing()
	local padding = ui.getWindowPadding()
	local buttonWidth = toggleSize and toggleSize.x or (styles.MainButtonSize.x + padding.x * 2)
	local anchor = Vector2(max.x - buttonWidth - spacing.x, math.max(max.y, 1))

	-- the panel is the only HUD window with a background: views/game.lua wraps
	-- the whole HUD in WindowBg = colors.transparent, which is right for the
	-- overlay elements but would leave the table floating on the planet.
	-- Restore the theme's own window background and frame for this window only.
	ui.withStyleColorsAndVars(
		{ WindowBg = colors.windowBackground, Border = colors.windowFrame },
		{ WindowBorderSize = 1.0, WindowRounding = styles.StyleRounding,
		  CellPadding = Vector2(ui.rescaleUI(6), ui.rescaleUI(3)) },
		function()
	ui.setNextWindowPos(anchor, "Always", Vector2(1, 1))

	ui.window("BurnHud", windowFlags, function()
		drawTitle(totals, hasRows)
		ui.separator()

		ui.withFont(dataFont.name, dataFont.size, function()
			if hasRows then
				drawBurnTable(rows, activeIndex, systemView)
			else
				ui.textColored(colors.fontDim, L.NO_BURNS)
			end
		end)

		ui.separator()
		drawAutopilotControls(player)
		-- the warp strip and the offset stay live with an empty table: a warp
		-- survives the plan it was started from being cleared
		if warp then drawWarpStrip() end
		drawOffsetControl()
		drawArrivalChoice()
		drawAccuracyControl()
		drawApproachStopControl()
	end)
	end)
end

--
-- Function: displayVisibilityToggle
--
-- The visibility toggle is drawn in its own window and stays clickable while
-- the panel is hidden. The state is persisted with the game settings.
--
local function displayVisibilityToggle(min, max)
	local size = Vector2(styles.MainButtonSize.x + ui.getWindowPadding().x * 2,
		styles.MainButtonSize.y + ui.getWindowPadding().y * 2)
	size.x = math.min(size.x, max.x - min.x)
	size.y = math.min(size.y, max.y - min.y)

	local pos = Vector2(max.x - size.x, max.y - size.y)
	max.y = max.y - size.y
	ui.setNextWindowSize(size, "Always")
	ui.setNextWindowPos(pos, "Always")

	ui.window("BurnHudToggle", toggleFlags, function()
		if ui.mainMenuButton(icons.burn_list, (visible and L.HIDE or L.SHOW) .. "##burnHudToggle",
			visible, styles.MainButtonSize) then
			visible = not visible
			Game.SetConfigBool("MultiBurnHud", "visible", visible)
			Engine.SaveSettings()
		end
	end)

	return size
end

-- min/max is the HUD area for this side of the screen, with the space used by
-- the modules above it already clipped away by the sidebar
local function displayBurnHud(min, max)
	if not shouldShowHud() then return end
	if not visibilityLoaded then loadVisibility() end

	colors = ui.theme.colors
	icons = ui.theme.icons

	local player = Game.player
	local systemView = Game.systemView
	if not player or not systemView then return end

	-- Auto Burn's accuracy lives in the ship controller, which a new game or
	-- ship starts from its default: keep it on the setting
	local accuracy = accuracyPermille / 1000
	if math.abs(player:GetPlanBurnAccuracy() - accuracy) > 1e-9 then
		player:SetPlanBurnAccuracy(accuracy)
	end

	-- leave the time window's height clear at the bottom
	if ui.timeWindowSize then
		max.y = max.y - ui.timeWindowSize.y
	end

	-- drive the warp on every draw, not only while the panel is visible, so
	-- the staircase keeps stepping and the arrival check keeps running;
	-- drawing happens once per render frame, which is why the physics-tick
	-- Timer is the primary arrival detector
	updateWarp()
	-- likewise the closest-approach stop
	updateApproachStop(systemView)

	local initialMax = Vector2(max.x, max.y)
	local toggleSize = displayVisibilityToggle(min, max)

	if visible then
		displayBurnHudPanel(min, initialMax, player, systemView, toggleSize)
	end
end

local function onDebugReload()
	package.reimport()
end

gameView.registerHudModule('burn-hud', {
	side = "right",
	showInHyperspace = false,
	debugReload = onDebugReload,
	draw = function(_, min, max)
		displayBurnHud(min, max)
	end
})

-- pending Timer callbacks are dropped when a game ends (Pi.cpp's RemoveAll),
-- so clearing the warp state at both ends of a session is all that's needed
local function resetWarpState()
	warp = nil
end

loadVisibility()
loadStopBeforeBurn()
loadBurnSettings()
Event.Register("onGameStart", function()
	loadVisibility()
	loadStopBeforeBurn()
	loadBurnSettings()
	resetWarpState()
end)
Event.Register("onGameEnd", resetWarpState)

return {}
