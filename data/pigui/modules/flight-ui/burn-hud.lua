-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Multi-burn flight HUD widget.
--
-- A compact panel in the flight HUD (WorldView only) listing every planned
-- transfer burn: status, Δv, remaining Δv, countdown to the burn start,
-- estimated burn duration, time remaining, and per-row Complete / Warp
-- controls, plus the two autopilot toggles (Auto Orientation, Auto Burn).
--
-- The table is rebuilt from the transfer planner every frame it draws. The
-- planner is the single source of truth (shared with the map sidebar), so this
-- module keeps no cached copy of the schedule.
--
-- Scope: Phase 4 (burn table, autopilot toggles) and Phase 5 (warp to burn)
-- of docs/implementation_plans/multi-burn-hud-autopilot_plan.md.
--
-- Phase 5: the Warp control warps the clock to (burn start − offset) with the
-- game's own time-acceleration levels and pauses on arrival. The simulation is
-- never un-paused by this module: resume happens only through an explicit user
-- action (Cancel Warp), using the pause menu's restore pattern from
-- views/game.lua. Arrival is detected on a physics tick (Timer:CallAt, armed
-- once per warp) and on every draw as a supplement.
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
local ui = require 'pigui'
local Vector2 = _G.Vector2

local icons = ui.theme.icons
local colors = ui.theme.colors
local styles = ui.theme.styles

local labelFont = ui.fonts.pionillium.details
local dataFont = ui.fonts.pionillium.small
local captionFont = ui.fonts.pionillium.tiny
local glyphFont = ui.fonts.pionicons.small
-- the font the reticle draws its side data blocks in, used only to measure the
-- horizontal band the panel must stay clear of (see reticleKeepoutLeft)
local reticleDataFont = ui.fonts.pionillium.heading

local gameView = require 'pigui.views.game'

-- TransferPlanner burn status values, as returned by
-- SystemView:TransferPlannerGetBurnStatus (see src/TransferPlanner.h)
local BURN_STATUS_PENDING = 0
local BURN_STATUS_ACTIVE = 1
local BURN_STATUS_COMPLETED = 2

-- every window this module opens is fixed: no dragging, no ImGui layout
-- persistence. No keybindings are used anywhere in this widget.
local windowFlags = ui.WindowFlags {"NoTitleBar", "NoResize", "NoMove", "NoSavedSettings",
	"NoFocusOnAppearing", "NoBringToFrontOnFocus", "NoScrollbar"}
local toggleFlags = ui.WindowFlags {"NoTitleBar", "NoResize", "NoMove", "NoSavedSettings",
	"NoFocusOnAppearing", "NoBringToFrontOnFocus", "NoBackground"}

-- cells render "-" when a value is unavailable
local UNAVAILABLE = "-"

-- a worst-case reticle side-data line, measured only to keep the panel clear of
-- the reticle's data blocks (see reticleKeepoutLeft; never drawn). The widest
-- value the reticule prints is a speed in its heading font
-- (flight-ui/reticule.lua: displayDetailData).
local RETICLE_WIDEST_SAMPLE = "999,999,999.9 m/s"

-- The plan cap, mirrored from src/TransferPlanner.cpp (kMaxBurns).
local MAX_BURN_COUNT = 8

-- strings the game ships no lang key for; Phase 4 is scoped to this module,
-- the HUD registration and the theme, so no data/lang entries are added here
local L = {
	BURN = "Burn %d",
	NO_BURNS = "No planned burns",
	CAPTION_LABEL = "Burn",
	CAPTION_STATUS = "",
	CAPTION_DV = "Δv",
	CAPTION_REMAINING_DV = "Left",
	CAPTION_START = "Start",
	CAPTION_DURATION = "Est.",
	CAPTION_TIME_LEFT = "Time left",
	CAPTION_ACTIONS = "",
	STATUS_PENDING = "Planned burn",
	STATUS_ACTIVE = "Active burn",
	STATUS_COMPLETED = "Completed burn",
	COMPLETE = "Mark this burn complete",
	-- the Warp tooltip names the configured stop-before-burn offset, so the row
	-- control states exactly what it will do; the greyed form states why it
	-- cannot (the target is at or before now, exactly startWarp's own rule)
	WARP = "Warp: time acceleration to stop %ds before this burn",
	WARP_REACHED = "Target is already reached",
	WARP_STRIP = "Warp to Burn %d - stop %ds before, %s",
	WARP_LIMITED = " (limited)",
	CANCEL_WARP = "Cancel warp: restore the previous time acceleration",
	OFFSET_LABEL = "Stop Before Burn (s)",
	OFFSET_TIP = "Pause this far before the burn start; shared by all burns",
	-- each tooltip states only what its own control does. The two controls are
	-- genuinely independent: Auto Orientation is a flight control state, Auto
	-- Burn an engine flag applied on top of it; setting one never sets the
	-- other, and the tooltips say exactly that (plan Phase 3 re-architecture).
	AUTO_ORIENTATION = "Auto Orientation: hold the ship at the active burn's direction. Orientation only, it never fires the main engine",
	AUTO_BURN = "Auto Burn: fire the main engine for the active burn, and only while the ship is aligned with the burn direction - unaligned it produces no thrust. Use it together with Auto Orientation to have the ship point itself; it never changes the ship's orientation",
	NEEDS_AUTOPILOT = "Requires ship autopilot equipment",
	-- textual state indicator for the two toggles: the pressed state must read
	-- unambiguously, not only as a subtle button-colour shift
	STATE_ORIENTATION = "Auto Orientation",
	STATE_BURN = "Auto Burn",
	STATE_ON = "ON",
	STATE_OFF = "OFF",
	SHOW = "Show the multi-burn burn list",
	HIDE = "Hide the multi-burn burn list",
}

-- the state line is "Label: ON" / "Label: OFF"
local STATE_SEPARATOR = ": "

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
-- The "Stop Before Burn" offset: whole seconds, shared by all burns, default
-- 30, persisted in the game settings. GetConfigInt has no "unset" signal (it
-- answers 0 for a missing key), so the value is stored biased by 1: an unset
-- key reads 0 and falls back to the default, while a stored 0 round-trips.
--
local DEFAULT_STOP_BEFORE_BURN_S = 30
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
-- Warp to burn (Phase 5).
--
-- One warp at a time, in module state: { idx, target, prevAccel,
-- lastRequest, armed }. target = burn start − offset; the module drives
-- Game.SetTimeAcceleration(level, true) down a staircase as the target
-- nears, and pauses on arrival. It never un-pauses the simulation: resume
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
-- Arrival: request the pause (a paused request is sticky) and clear the warp
-- state. Called from the armed Timer callback on a physics tick and from the
-- per-draw check; whichever runs first wins, the second is a no-op.
--
local function arrive()
	if not warp then return end
	Game.SetTimeAcceleration("paused")
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

	local target = start - stopBeforeBurnS
	if target <= Game.time then return false end

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
-- Estimated burn duration: Δv / forward acceleration. Shared by the estimated
-- duration cell and the time-remaining cell; the same estimate the reticule
-- shows (flight-ui/reticule.lua).
--
local function estimateDuration(dv, acceleration)
	if not dv or not acceleration or acceleration <= 0 then return nil end
	return dv / acceleration
end

local function formatDuration(seconds)
	if not seconds then return UNAVAILABLE end
	return ui.Format.Duration(seconds)
end

local function formatSpeed(dv)
	if not dv then return UNAVAILABLE end
	local speed, unit = ui.Format.SpeedUnit(dv)
	return speed .. " " .. unit
end

-- countdown to the burn start, reusing the map sidebar's "Now" clamping
-- convention (modules/system-view-ui.lua)
local function formatCountdown(start)
	if start and start > Game.time then
		return ui.Format.Duration(math.floor(start - Game.time))
	end
	return lc.NOW
end

local status_glyph = {
	[BURN_STATUS_PENDING] = icons.square_dashed,
	[BURN_STATUS_ACTIVE] = icons.maneuver,
	[BURN_STATUS_COMPLETED] = icons.map_checkmark,
}

local function glyphOf(status)
	return status_glyph[status] or icons.empty
end

local status_name = {
	[BURN_STATUS_PENDING] = L.STATUS_PENDING,
	[BURN_STATUS_ACTIVE] = L.STATUS_ACTIVE,
	[BURN_STATUS_COMPLETED] = L.STATUS_COMPLETED,
}

--
-- Function: collectRows
--
-- Read-only snapshot of the plan for this frame; every value comes from a
-- planner call and nothing here is kept between frames.
--
local function collectRows(systemView, acceleration)
	local count = systemView:TransferPlannerGetBurnCount()
	local activeIndex = systemView:TransferPlannerGetActiveIndex()
	local rows = {}

	for idx = 0, count - 1 do
		local status = systemView:TransferPlannerGetBurnStatus(idx)
		local dv = systemView:TransferPlannerGetBurnDeltaV(idx)
		local remaining = systemView:TransferPlannerGetBurnRemainingDeltaV(idx)
		local start = systemView:TransferPlannerGet(idx, "starttime")
		-- the Warp control's availability is startWarp's own rule, computed from
		-- the same planner value: a target at or before now never warps
		local warpTarget = start and (start - stopBeforeBurnS) or nil

		table.insert(rows, {
			idx = idx,
			status = status,
			label = string.format(L.BURN, idx + 1),
			statusText = status_name[status] or UNAVAILABLE,
			dv = formatSpeed(dv),
			remainingDv = formatSpeed(remaining),
			countdown = formatCountdown(start),
			duration = formatDuration(estimateDuration(dv, acceleration)),
			timeLeft = formatDuration(estimateDuration(remaining, acceleration)),
			completable = status ~= BURN_STATUS_COMPLETED,
			warppable = (warpTarget ~= nil) and (warpTarget > Game.time),
		})
	end

	return rows, activeIndex
end

-- table columns: burn label / status glyph / Δv / remaining Δv / start /
-- estimated duration / time remaining / actions. Each column receives exactly
-- one item per row, which is what keeps the rows aligned.
local COL_LABEL, COL_STATUS, COL_DV, COL_REMAINING, COL_START, COL_DURATION, COL_TIME_LEFT, COL_ACTIONS = 0, 1, 2, 3, 4, 5, 6, 7
local COL_COUNT = COL_ACTIONS + 1

--
-- Function: measureLayout
--
-- The column widths plus the row heights, measured once per screen size.
--
-- Every string is measured in the exact same font context that draws it (the
-- same ui.withFont(...) wrapper the row drawing uses), and against worst-case
-- strings, not the current plan's values: the label column is sized for
-- "Burn 8" (the plan cap, MAX_BURN_COUNT), the Δv / remaining columns for the
-- widest string ui.Format.SpeedUnit can produce including its unit suffix, the
-- time columns for the longest strings ui.Format.Duration can produce plus
-- "NOW" and the "-" placeholder, and the action column for BOTH controls
-- (Complete + Warp) including the inter-control ui.sameLine spacing. The
-- layout is therefore stable for a given screen size and independent of the
-- plan's contents.
--
-- Font context matters: ui.calcTextSize is font-context dependent, and the HUD
-- draws inside the ambient font views/game.lua pushes (pionillium.medium).
-- Measuring a string outside the font it is drawn in measures the wrong text —
-- the exact under-sizing pass 2's screenshot showed ("950 m/", "Est",
-- "Time l", "Burn"). Every measurement below is wrapped in the same
-- ui.withFont(...) block the corresponding drawing uses.
--
-- The vertical metrics come from the same measurement: a text item's height is
-- the font's text height, and a row is as tall as its tallest item. The action
-- buttons have an explicit label-font text height, also reserved by the row;
-- their glyphs are measured at that height less the inline padding.
--
-- Numeric widths follow the current rows; labels and controls retain their
-- measured minimum sizes.
--
local function measureLayout(rows)
	-- widths are indexed by column (0..COL_ACTIONS)
	local widths = {}
	for i = COL_LABEL, COL_ACTIONS do widths[i] = 0 end

	-- column captions, measured in the caption font the header row is drawn in;
	-- the captions' text height is the caption row's height
	local captionLine = 0
	ui.withFont(captionFont.name, captionFont.size, function()
		-- the list is 1-based, the columns are 0-based: index widths[col - 1]
		for col, caption in ipairs({
			L.CAPTION_LABEL, L.CAPTION_STATUS, L.CAPTION_DV, L.CAPTION_REMAINING_DV,
			L.CAPTION_START, L.CAPTION_DURATION, L.CAPTION_TIME_LEFT, L.CAPTION_ACTIONS,
		}) do
			local size = ui.calcTextSize(caption)
			widths[col - 1] = math.max(widths[col - 1], size.x)
			captionLine = math.max(captionLine, size.y)
		end
	end)

	-- burn labels for the whole cap ("Burn 1" .. "Burn 8"), in the label font;
	-- the label's text height is the row's advance (see the note above). The
	-- empty-plan line is drawn in this same font, so its height is measured
	-- here too.
	local worstLabels = { L.NO_BURNS }
	for n = 1, MAX_BURN_COUNT do
		worstLabels[n + 1] = string.format(L.BURN, n)
	end
	local rowLine, emptyLine = 0, 0
	ui.withFont(labelFont.name, labelFont.size, function()
		for n, label in ipairs(worstLabels) do
			local size = ui.calcTextSize(label)
			-- n == 1 is the empty-plan line: it sizes the window, not a column
			if n > 1 then
				widths[COL_LABEL] = math.max(widths[COL_LABEL], size.x)
				rowLine = math.max(rowLine, size.y)
			else
				emptyLine = size.y
			end
		end
	end)

	-- status glyphs, measured in the icon font the glyph cell is drawn in
	ui.withFont(glyphFont.name, glyphFont.size, function()
		for _, glyph in pairs(status_glyph) do
			widths[COL_STATUS] = math.max(widths[COL_STATUS], ui.calcTextSize(ui.get_icon_glyph(glyph)).x)
		end
	end)

	-- Fit each numeric column to its actual formatted values, not hypothetical
	-- extreme burns. Measure in the same font used by drawCell.
	ui.withFont(dataFont.name, dataFont.size, function()
		for _, row in ipairs(rows) do
			for col, text in pairs({
				[COL_DV] = row.dv,
				[COL_REMAINING] = row.remainingDv,
				[COL_START] = row.countdown,
				[COL_DURATION] = row.duration,
				[COL_TIME_LEFT] = row.timeLeft,
			}) do
				widths[col] = math.max(widths[col], ui.calcTextSize(text).x)
			end
		end
	end)

	-- Explicit action sizes must not depend on the line height, which resets
	-- at nextColumn. Match iconButton's glyph font height and padding, and
	-- reserve the label-line height for both buttons and the completed dummy.
	local iconGlyphWidth = 0
	ui.withFont(glyphFont.name, rowLine - styles.InlineIconPadding.y * 2, function()
		for _, icon in ipairs({ icons.map_checkmark, icons.time_accel_10000x }) do
			iconGlyphWidth = math.max(iconGlyphWidth, ui.calcTextSize(ui.get_icon_glyph(icon)).x)
		end
	end)
	local iconWidth = iconGlyphWidth + styles.InlineIconPadding.x * 2
	widths[COL_ACTIONS] = math.max(widths[COL_ACTIONS], (iconWidth * 2) + ui.getItemSpacing().x)
	local actionSlotSize = Vector2(iconWidth, rowLine)

	-- the warp strip's height and its cancel control. The strip is one
	-- data-font text line (the status text and the inline cancel control share
	-- it, see drawWarpStrip), so its advance is that text height, and the
	-- cancel glyph is measured at exactly that height (less the inline
	-- padding). Measured on the widest string the strip can print — the
	-- highest burn number, the largest offset, the widest level name, the
	-- clamped suffix — so the layout does not depend on a warp running.
	local warpStripLine, cancelWidth = 0, 0
	local worstStrip = string.format(L.WARP_STRIP, MAX_BURN_COUNT, OFFSET_MAX, "10000x") .. L.WARP_LIMITED
	ui.withFont(dataFont.name, dataFont.size, function()
		warpStripLine = ui.calcTextSize(worstStrip).y
	end)
	ui.withFont(glyphFont.name, warpStripLine - styles.InlineIconPadding.y * 2, function()
		cancelWidth = ui.calcTextSize(ui.get_icon_glyph(icons.cross)).x
	end)

	local layout = {
		widths = widths,
		rowLine = rowLine,
		captionLine = captionLine,
		emptyLine = emptyLine,
		warpStripLine = warpStripLine,
		warpCancelWidth = cancelWidth,
		actionSlotSize = actionSlotSize,
	}
	return layout
end

-- the offset drag's fixed item width, in the data font's cap height
local OFFSET_DRAG_WIDTH = dataFont.size * 4

-- The offset row is a frame control, not a bare text line. Its height is
-- ui.getFrameHeight() under the caption font the row is drawn in
-- (drawOffsetControl draws the incrementDrag inside the same withFont block),
-- which is the font size plus FramePadding.y*2 — the same arithmetic, so the
-- layout reserves exactly the frame height the widget occupies.
local OFFSET_ROW_HEIGHT = captionFont.size + styles.FramePadding.y * 2

--
-- Function: reticleKeepoutLeft
--
-- The leftmost x the panel may reach. The reticle draws its side data blocks
-- from the 3 o'clock point of a circle of radius*1.2 around the screen centre
-- (flight-ui/reticule.lua: displayReticule passes radius * 1.2 to
-- displayDetailData / displayFrameData), extending rightward in the heading
-- font. The panel sizes from measured content and may extend left past the
-- reserved HUD column, so this keepout — screen centre + the reticle's data
-- radius + the widest reticle data line it can print — is the one hard width
-- limit.
--
-- The value only depends on the screen size, so the text measurement is cached
-- per screen size (the reticle computes its radius from the same numbers).
--
local reticleKeepoutCache = {}

local function reticleKeepoutLeft(screenWidth, screenHeight)
	local key = tostring(screenWidth) .. "x" .. tostring(screenHeight)
	local cached = reticleKeepoutCache[key]
	if cached then return cached end

	local center = gameView.center or Vector2(screenWidth / 2, screenHeight / 2)
	local radius = math.min(screenWidth, screenHeight) / 8
	local textWidth = ui.withFont(reticleDataFont.name, reticleDataFont.size, function()
		return ui.calcTextSize(RETICLE_WIDEST_SAMPLE).x
	end)
	local keepout = center.x + radius * 1.2 + textWidth + ui.getWindowPadding().x
	reticleKeepoutCache[key] = keepout
	return keepout
end

--
-- Function: warpStripText
--
-- The warp status line: target burn number, the configured offset, the
-- acceleration level this module last requested, and the clamped indicator.
-- The requested-vs-applied comparison is the time window's pattern
-- (modules/time-window.lua). The engine's hard clamps (hyperspace, docking,
-- being fired on) rewrite the request — which hands the clock back to the
-- engine and ends the warp — while proximity clamps keep it; both show up on
-- this line, and neither is fought.
--
local function warpStripText()
	local level = warp.lastRequest or Game.GetRequestedTimeAcceleration()
	local clamped = Game.GetTimeAcceleration() ~= Game.GetRequestedTimeAcceleration()
	return string.format(L.WARP_STRIP, warp.idx + 1, stopBeforeBurnS, level)
		.. (clamped and L.WARP_LIMITED or "")
end

--
-- Function: measureAutopilotRow
--
-- Measures the autopilot row: the two toggle buttons, the gap between them,
-- and the "Label: ON/OFF" state indicator that follows them on the same line.
-- Both the width and the row height come from measured text plus the fixed
-- button size, so the panel reserves exactly what the row needs.
--
local function measureAutopilotRow()
	local spacing = ui.getItemSpacing()
	local width = styles.MainButtonSize.x * 2 + spacing.x
	local height = styles.MainButtonSize.y

	ui.withFont(dataFont.name, dataFont.size, function()
		-- the widest form of each indicator is the OFF state (three letters)
		for _, label in ipairs({ L.STATE_ORIENTATION, L.STATE_BURN }) do
			local w = math.max(
				ui.calcTextSize(label .. STATE_SEPARATOR .. L.STATE_ON).x,
				ui.calcTextSize(label .. STATE_SEPARATOR .. L.STATE_OFF).x)
			width = width + spacing.x + w
			height = math.max(height, dataFont.size)
		end
	end)

	return width, height
end

--
-- Function: measurePanelExtraWidth
--
-- Measures the widest of the rows below the table — the offset stepper (label
-- + the fixed-width drag) and, while a warp is running, the warp status line
-- with its inline cancel control. Their heights are laid out from the same
-- measured quantities in displayBurnHudPanel (the offset stepper is a frame
-- control: caption font line + FramePadding.y*2 = OFFSET_ROW_HEIGHT; the warp
-- strip is one data-font text line plus its inline button). The warp strip is
-- measured on the widest string the strip can print, from the cached layout,
-- so the panel width does not change when a warp starts.
--
local function measurePanelExtraWidth(showWarpStrip, layout)
	local spacing = ui.getItemSpacing()
	local width = 0

	ui.withFont(captionFont.name, captionFont.size, function()
		width = math.max(width, ui.calcTextSize(L.OFFSET_LABEL).x)
	end)
	width = width + spacing.x + OFFSET_DRAG_WIDTH

	if showWarpStrip then
		-- the strip's own text plus its inline cancel control, which sits on
		-- the same line as an inline icon button: the cached glyph width
		-- (measured in the icon font at the strip line's height, in the
		-- layout pass) plus the inline padding the button pushes
		local stripWidth = ui.withFont(dataFont.name, dataFont.size, function()
			return ui.calcTextSize(warpStripText()).x
		end)
		width = math.max(width, stripWidth + spacing.x
			+ layout.warpCancelWidth + styles.InlineIconPadding.x * 2)
	end

	return width
end

--
-- Function: drawActiveRowHighlight
--
-- The active row's highlight: a filled band plus a 1px border across the full
-- content width of the table, at exactly the row band the layout measured
-- (layout.rowLine), in the theme's burnHudActiveRowBg / burnHudActiveRowBorder.
--
-- SUBMISSION ORDER CONTRACT (pass 5). ImGui paints a window's draw list in
-- submission order, so a rect is only ever *behind* the text it overlaps if it
-- is emitted first. This helper exists so that rule is a single call site and
-- can never drift: the row loop calls it as the FIRST draw of the row, before
-- any drawCell, and the row's cells are emitted after it. Geometry is the
-- caller's (rowTop/region/rowLine) — nothing here measures or moves the band.
--
-- Audit of every rect / background submission in this module, against the text
-- it could cover:
--   * this band, emitted before the row's cells (the only rect inside a row).
--   * the window's own background + frame, from the withStyleColorsAndVars
--     wrapper in displayBurnHudPanel: ImGui draws the window background behind
--     all of the window's items, and the frame border only at the window
--     edges, which the row band (content-region width) never reaches.
--   * drawCell's dim path: ui.addStyledText is called without a bg colour, and
--     that helper draws its background rect (when given one) before its own
--     text, so a dimmed cell paints over nothing.
--   * the autopilot buttons, the offset drag and the action buttons are ImGui
--     widgets: their frames and their own labels are emitted by the widget,
--     frame first. No free-standing rect is drawn on top of them.
--   * no addRectFaded / addImage / clip-rect background is used anywhere here.
--
local function drawActiveRowHighlight(rowTop, region, rowLine)
	ui.addRectFilled(rowTop, rowTop + Vector2(region, rowLine), colors.burnHudActiveRowBg,
		styles.StyleRounding, ui.RoundCornersNone)
	ui.addRect(rowTop, rowTop + Vector2(region, rowLine), colors.burnHudActiveRowBorder,
		styles.StyleRounding, ui.RoundCornersNone, 1)
end

--
-- Function: drawCell
--
-- Draw one table cell as a layout item so the columns keep advancing. The text
-- is drawn in exactly the font its column was measured in: the HUD is wrapped
-- in an ambient font by views/game.lua (pionillium.medium), so a cell drawn
-- without pushing its own font renders wider than the column measured for it —
-- the under-sizing pass 2 showed ("950 m/", "Est", "Time l", "Burn" with its
-- index clipped). A completed row is dimmed: the item is laid out empty and the
-- text is drawn over it with ui.addStyledText in the dim colour (that helper
-- pushes the font itself), keeping a completed row's geometry identical to a
-- live row.
--
local function drawCell(text, font, dimColor)
	if not dimColor then
		ui.withFont(font.name, font.size, function()
			ui.text(text)
		end)
		return
	end
	local pos = ui.getCursorScreenPos()
	ui.text("")
	ui.addStyledText(pos, ui.anchor.left, ui.anchor.top, text, dimColor, font)
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
	if ui.mainMenuButton(icons.prograde_thin, orientationTip, orientationVariant, styles.MainButtonSize) then
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
	if ui.mainMenuButton(icons.forward, burnTip, burnVariant, styles.MainButtonSize) then
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
			L.STATE_ORIENTATION .. STATE_SEPARATOR .. (orientationOn and L.STATE_ON or L.STATE_OFF))
		ui.sameLine()
		ui.textColored(burnOn and colors.burnHudToggleOnText or colors.burnHudToggleOffText,
			L.STATE_BURN .. STATE_SEPARATOR .. (burnOn and L.STATE_ON or L.STATE_OFF))
	end)
end

--
-- Function: drawOffsetControl
--
-- "Stop Before Burn (s)": the pre-burn offset, shared by all burns, as a
-- drag the player can nudge, drag and type into. Clamped to non-negative
-- whole seconds and persisted with the game settings. The always-drawn
-- toggle window keeps this module alive, so the value survives while the
-- panel is hidden.
--
local function drawOffsetControl()
	local value, changed
	ui.withFont(captionFont.name, captionFont.size, function()
		ui.text(L.OFFSET_LABEL)
	end)
	ui.setItemTooltip(L.OFFSET_TIP)

	ui.sameLine()
	-- the drag is a frame control: it is drawn in the same font the layout
	-- measured it in, so its height (font size + FramePadding.y*2 =
	-- OFFSET_ROW_HEIGHT) is exactly the height the panel reserved
	ui.withFont(captionFont.name, captionFont.size, function()
		ui.pushItemWidth(OFFSET_DRAG_WIDTH)
		value, changed = ui.incrementDrag("##burnHudStopBeforeBurn", stopBeforeBurnS, 1, 0, OFFSET_MAX, "%.0f", false)
		ui.popItemWidth()
	end)
	if changed then
		stopBeforeBurnS = math.min(math.max(math.floor(value), 0), OFFSET_MAX)
		saveStopBeforeBurn()
	end
end

--
-- Function: drawWarpStrip
--
-- While a warp is running: the target burn number, the configured offset, the
-- acceleration level this module last requested, the clamped indicator, and
-- the Cancel Warp control.
--
local function drawWarpStrip()
	-- the status text and its cancel control share one measured line: the
	-- inline button is sized to this line's height, so the strip never grows
	-- past the height the layout reserved for it
	ui.withFont(dataFont.name, dataFont.size, function()
		ui.text(warpStripText())

		ui.sameLine()
		if ui.inlineIconButton("##burnHudCancelWarp", icons.cross, L.CANCEL_WARP) then
			cancelWarp()
		end
	end)
end

--
-- Function: displayBurnHudPanel
--
-- Draws the burn table. Vertical space is reserved from the bottom of the HUD
-- area (above the time window) so the panel never overlaps the modules sharing
-- this side of the screen. Width is sized from measured content and the panel
-- is anchored to the right edge of the HUD column — the anchored-in-code
-- pattern of modules/mass-twr-display.lua:130-131 and
-- modules/time-window.lua:18-36 — so it does not steal width from the other
-- HUD elements: the reserved column's width is free space, never the panel's
-- width, which is what squeezed the panel and hid the time-skip controls. The
-- panel grows leftwards into space no other HUD element claims, kept clear of
-- the reticle's data blocks by reticleKeepoutLeft(). The columns are scaled
-- down only as a last resort, and the window is never narrower than the
-- measured width of the un-scalable rows (offset stepper, autopilot row, warp
-- strip), so no control is clipped.
--
local function displayBurnHudPanel(min, max, player, systemView)
	local acceleration = player:GetAcceleration("forward")
	local rows, activeIndex = collectRows(systemView, acceleration)
	local hasRows = #rows > 0

	-- Keep the table compact while allowing longer live values to fit.
	local layout = measureLayout(rows)
	local widths = layout.widths

	-- the rows below the table: the autopilot row, the offset stepper, and
	-- while a warp is running the warp status line with its cancel control
	local showWarpStrip = warp ~= nil
	local extraWidth = measurePanelExtraWidth(showWarpStrip, layout)
	local autopilotWidth, autopilotHeight = measureAutopilotRow()

	local spacing = ui.getItemSpacing()
	local padding = ui.getWindowPadding()
	-- the first drawn line is the column caption row with a table, and the
	-- "No planned burns" label (labelFont) without one; both heights come from
	-- the same font-context measurement the drawing uses
	local captionLine = hasRows and layout.captionLine or layout.emptyLine

	-- Legacy columns inset the cursor by ItemSpacing.x and clip one pixel
	-- before the next boundary. Use these same spans for columns and window.
	local columnSpans, tableWidth = {}, 0
	for i = COL_LABEL, COL_ACTIONS do
		columnSpans[i] = widths[i] + spacing.x + 1
		tableWidth = tableWidth + columnSpans[i]
	end
	local contentWidth = math.max(tableWidth, extraWidth, autopilotWidth)
	-- the un-scalable floor, used only when the panel is drawn without a table
	-- (an empty plan): the widest row that cannot shrink. With the table drawn
	-- the column sum is the floor, so there is nothing to compute here.
	local floorWidth = 0
	if not hasRows then
		-- the empty-plan line is a single label drawn in the label font, so it
		-- is a measured content width like every other row
		local emptyWidth = ui.withFont(labelFont.name, labelFont.size, function()
			return ui.calcTextSize(L.NO_BURNS).x
		end)
		contentWidth = math.max(contentWidth, emptyWidth)
		floorWidth = math.max(extraWidth, autopilotWidth, emptyWidth) + padding.x * 2
	end
	local measuredWidth = contentWidth + padding.x * 2

	-- height: every drawn row at its measured height — one line per burn at
	-- layout.rowLine (a row's tallest item is the label-font line its action
	-- buttons are sized to), the caption row at layout.captionLine, the
	-- empty-plan line at layout.emptyLine, the autopilot row at its button
	-- height, the offset row at its frame-control height
	-- OFFSET_ROW_HEIGHT = getFrameHeight() under the caption font, the warp
	-- strip at layout.warpStripLine (the data-font line its inline cancel
	-- control is sized to) — each followed by its line spacing, plus the
	-- window's top and bottom padding. The panel is fixed-size
	-- (NoResize, NoScrollbar), so anything past the bottom edge of the window
	-- is hidden: the window must cover the full content, including the offset
	-- stepper and the toggles.
	local contentHeight = captionLine + spacing.y
	contentHeight = contentHeight + #rows * (layout.rowLine + spacing.y)
	contentHeight = contentHeight + autopilotHeight + spacing.y
	-- the offset row is a frame control, not a bare text line
	contentHeight = contentHeight + OFFSET_ROW_HEIGHT + spacing.y
	if showWarpStrip then
		-- the warp status line and its inline cancel control share one
		-- data-font line (see drawWarpStrip); the control sets the line height
		contentHeight = contentHeight + layout.warpStripLine + spacing.y
	end
	local height = math.max(contentHeight + padding.y * 2, 1)

	-- the panel's right edge is the right edge of this side's HUD column, and
	-- its bottom edge is the bottom of the available area, just above the time
	-- window (whose height displayBurnHud clips out of max); rectcut's bottom
	-- side does not touch x, and it moves max.y to the top of the strip it
	-- cuts, so both edges have to be read before the reserve.
	local rightEdge = max.x
	local bottom = max.y

	-- reserve the vertical space from the bottom of the HUD area so the modules
	-- sharing this column are pushed up, never overlapped from below; the claim
	-- is capped at the available space, and a fixed-size window is never
	-- shorter than its measured content: when the available space is smaller
	-- than the measured height the window keeps the measured height and grows
	-- upward from the bottom anchor, so the time-skip, offset and cancel
	-- controls stay inside it. The only limit is the top of the screen.
	ui.rectcut(min, max, math.min(height, max.y - min.y), ui.sides.bottom)
	if bottom < 1 then bottom = 1 end
	height = math.min(height, bottom)
	local top = bottom - height

	-- width: exactly the measured content, never a squeezed default and never
	-- clamped below the table's own column sum — the pass-2 "last resort"
	-- scaling is gone: it re-clamped every column below the width its strings
	-- were measured at, which is what truncated "950 m/", "Est", "Time l" and
	-- clipped the burn index. The anchored-right growth pattern (pass 2) is
	-- unchanged: the window is sized from measured content, anchored to the
	-- right edge of the HUD column, and grows leftwards into space no other
	-- HUD element claims, so it never steals width from the reserved column.
	-- The reticle keepout stays the cap for the rows that are drawn without a
	-- table (an empty plan): there the panel is small and the keepout is a
	-- meaningful limit; with the table drawn, the column sum is the floor,
	-- because a clipped cell is worse than the panel reaching into a data band
	-- the reticle only fills for its widest readout.
	local freeSpace = rightEdge - reticleKeepoutLeft(ui.screenWidth, ui.screenHeight)
	local width = measuredWidth
	if not hasRows then
		width = math.min(measuredWidth, math.max(freeSpace, floorWidth))
	end
	-- never wider than the screen, which also keeps the anchored x non-negative
	width = math.min(width, ui.screenWidth)

	-- anchor to the right edge of the HUD column; the vertical extent is the
	-- reserved space, grown upward to the measured height above
	local winPos = Vector2(rightEdge - width, top)

	-- the panel is the only HUD window with a background: views/game.lua
	-- wraps the whole HUD in WindowBg = colors.transparent (line 276), which
	-- is right for the overlay elements (the reticle readouts, the time
	-- window, this module's visibility toggle) but left the burn table
	-- floating with no frame, so the highlighted active row read as a white
	-- block on the planet. Restore the theme's own window background and
	-- frame for this window only — the same colors every themed window uses
	-- (theme.colors.windowBackground / windowFrame) — without touching any
	-- window flag: NoTitleBar/NoResize/NoMove/NoSavedSettings/NoScrollbar all
	-- stay, the window is still anchored in code and not draggable, and no
	-- layout persistence.
	ui.withStyleColorsAndVars(
		{ WindowBg = colors.windowBackground, Border = colors.windowFrame },
		{ WindowBorderSize = 1.0, WindowRounding = styles.StyleRounding },
		function()
	ui.setNextWindowSize(Vector2(width, height), "Always")
	ui.setNextWindowPos(winPos, "Always")

	ui.window("BurnHud", windowFlags, function()
		local region = ui.getContentRegion().x

		if not hasRows then
			ui.withFont(labelFont.name, labelFont.size, function()
				ui.text(L.NO_BURNS)
			end)
			drawAutopilotControls(player)
			-- the warp strip and the offset stay live with an empty table:
			-- a warp survives the plan it was started from being cleared
			if showWarpStrip then drawWarpStrip() end
			drawOffsetControl()
			return
		end

		ui.columns(COL_COUNT, "##burnHudTable", false)
		for i = COL_LABEL, COL_ACTIONS do
			ui.setColumnWidth(i, columnSpans[i])
		end

		-- column captions
		ui.withFont(captionFont.name, captionFont.size, function()
			ui.text(L.CAPTION_LABEL)
			ui.nextColumn()
			ui.text(L.CAPTION_STATUS)
			ui.nextColumn()
			ui.text(L.CAPTION_DV)
			ui.nextColumn()
			ui.text(L.CAPTION_REMAINING_DV)
			ui.nextColumn()
			ui.text(L.CAPTION_START)
			ui.nextColumn()
			ui.text(L.CAPTION_DURATION)
			ui.nextColumn()
			ui.text(L.CAPTION_TIME_LEFT)
			ui.nextColumn()
			ui.text(L.CAPTION_ACTIONS)
			ui.nextColumn()
		end)

		for _, row in ipairs(rows) do
			-- the planner is the live list: removing a burn shifts every later
			-- index, so stop at the current count, as the sidebar table does
			if row.idx >= systemView:TransferPlannerGetBurnCount() then break end

			local isActive = row.idx == activeIndex
			local dimColor = row.status == BURN_STATUS_COMPLETED and colors.burnHudCompletedText or nil

			local rowTop = ui.getCursorScreenPos()

			-- (a) the active row's highlight FIRST, at the band the layout
			-- measured; the row is still chosen by the planner's active index
			if isActive then
				drawActiveRowHighlight(rowTop, region, layout.rowLine)
			end

			-- (b) then the row's cells, on top of that band: every drawCell and
			-- widget below is submitted after the highlight, so the label and its
			-- index digit are painted over it, never under it
			-- burn label
			drawCell(row.label, labelFont, dimColor)
			ui.nextColumn()

			-- status glyph
			ui.withFont(glyphFont.name, glyphFont.size, function()
				ui.text(ui.get_icon_glyph(glyphOf(row.status)))
			end)
			ui.setItemTooltip(row.statusText)
			ui.nextColumn()

			ui.withFont(dataFont.name, dataFont.size, function()
				drawCell(row.dv, dataFont, dimColor)
				ui.nextColumn()
				drawCell(row.remainingDv, dataFont, dimColor)
				ui.nextColumn()
				drawCell(row.countdown, dataFont, dimColor)
				ui.nextColumn()
				drawCell(row.duration, dataFont, dimColor)
				ui.nextColumn()
				drawCell(row.timeLeft, dataFont, dimColor)
				ui.nextColumn()
			end)

			-- actions: the column always holds both controls, in the two slots
			-- measureLayout() reserved for them, so a row never shifts the Warp
			-- control sideways.
			--   Complete is drawn for the active burn and for pending burns,
			--   never on a completed row (row.completable); the slot it would
			--   occupy is reserved with a dummy of the measured width instead.
			--   Warp starts the Phase 5 warp state machine for every row, and is
			--   greyed with "Target is already reached" when its target
			--   (start − offset) is at or before now - the exact rule
			--   startWarp() applies, evaluated on the row data. Its tooltip names
			--   the configured offset.
			-- Explicit button sizes and padding match measurement; their height
			-- is the label line reserved by layout.rowLine, even in a fresh column.
			-- iconButton takes its ID from its own id argument (below), so
			-- the tooltip is plain text — a "##id" suffix would be printed
			-- verbatim by the tooltip helper
			local warpTip = row.warppable
				and string.format(L.WARP, stopBeforeBurnS)
				or L.WARP_REACHED
			-- nil = the theme's default button variant; the greyed variant is
			-- only for a target that is already reached. Spelled as a branch
			-- because `row.warppable and nil or X` would yield X in both cases
			-- (`true and nil` is nil in Lua).
			local warpVariant = nil
			if not row.warppable then
				warpVariant = ui.theme.buttonColors.disabled
			end
			ui.withFont(labelFont.name, labelFont.size, function()
				if row.completable then
					if ui.iconButton("##burnHudComplete" .. row.idx, icons.map_checkmark, L.COMPLETE,
						nil, layout.actionSlotSize, styles.InlineIconPadding) then
						systemView:TransferPlannerCompleteBurn(row.idx)
					end
				else
					-- completed row: no Complete control, but the slot stays
					-- reserved (the measured single-control width) so Warp keeps
					-- the same x on every row
					ui.dummy(layout.actionSlotSize)
				end
				ui.sameLine()
				if ui.iconButton("##burnHudWarp" .. row.idx, icons.time_accel_10000x, warpTip,
					warpVariant, layout.actionSlotSize, styles.InlineIconPadding) and row.warppable then
					startWarp(row.idx)
				end
			end)
			ui.nextColumn()
		end

		-- reset columns
		ui.columns(1)

		drawAutopilotControls(player)

		if showWarpStrip then drawWarpStrip() end
		drawOffsetControl()
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
	ui.setNextWindowSize(size, "Always")
	ui.setNextWindowPos(pos, "Always")

	ui.window("BurnHudToggle", toggleFlags, function()
		if ui.mainMenuButton(icons.hud, (visible and L.HIDE or L.SHOW) .. "##burnHudToggle",
			visible, styles.MainButtonSize) then
			visible = not visible
			Game.SetConfigBool("MultiBurnHud", "visible", visible)
			Engine.SaveSettings()
		end
	end)
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

	-- leave the time window's height clear at the bottom
	if ui.timeWindowSize then
		max.y = max.y - ui.timeWindowSize.y
	end

	-- drive the warp on every draw, not only while the panel is visible, so
	-- the staircase keeps stepping and the arrival check keeps running;
	-- drawing happens once per render frame, which is why the physics-tick
	-- Timer is the primary arrival detector
	updateWarp()

	if visible then
		displayBurnHudPanel(min, max, player, systemView)
	end

	displayVisibilityToggle(min, max)
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
Event.Register("onGameStart", function()
	loadVisibility()
	loadStopBeforeBurn()
	resetWarpState()
end)
Event.Register("onGameEnd", resetWarpState)

return {}
