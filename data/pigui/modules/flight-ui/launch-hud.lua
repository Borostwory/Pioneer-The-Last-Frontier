-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Ascent guidance panel in the flight HUD (WorldView only): a toggle button
-- in the right-hand button column, above the burn list's, and a panel beside
-- it to switch guidance on and set the orbit it climbs to - the altitude and
-- the launch heading, which fixes the orbit's inclination. Coasting to
-- apoapsis without the circularization burn guidance plans by itself, the
-- panel offers to plan it.
--
-- The button is always there in the flight view; the panel opens only on or
-- over a planet, where guidance can take the ship to orbit (or while it is
-- on), and elsewhere the button is greyed out. The guidance itself is
-- pigui/libs/ascent-guidance.lua; the flight HUD draws its director and
-- readout (flight-ui/reticule.lua).
--
-- Named to load after burn-hud.lua and before orbit-view-hud.lua: HUD
-- modules stack up the column in load order.

local Engine = require 'Engine'
local Event = require 'Event'
local Game = require 'Game'
local Lang = require 'Lang'
local lc = Lang.GetResource("core")
local lui = Lang.GetResource("ui-core")
local ui = require 'pigui'
local Vector2 = _G.Vector2

local AscentGuidance = require 'pigui.libs.ascent-guidance'
local gameView = require 'pigui.views.game'

local icons = ui.theme.icons
local colors = ui.theme.colors
local styles = ui.theme.styles

-- the burn list panel's type scale
local titleFont = ui.fonts.orbiteer.small
local dataFont = ui.fonts.pionillium.details

local windowFlags = ui.WindowFlags {"NoTitleBar", "NoResize", "NoMove", "NoSavedSettings",
	"NoFocusOnAppearing", "NoBringToFrontOnFocus", "NoScrollbar", "AlwaysAutoResize"}
local toggleFlags = ui.WindowFlags {"NoTitleBar", "NoResize", "NoMove", "NoSavedSettings",
	"NoFocusOnAppearing", "NoBringToFrontOnFocus", "NoBackground"}

-- the four compass presets for the launch heading
local presets = {
	{ heading = 0, label = lc.COMPASS_N },
	{ heading = 90, label = lc.COMPASS_E },
	{ heading = 180, label = lc.COMPASS_S },
	{ heading = 270, label = lc.COMPASS_W },
}

local DRAG_WIDTH = dataFont.size * 6

-- the panel's visibility, persisted with the game settings, default hidden
local visible = false
local visibilityLoaded = false

local function loadVisibility()
	visible = Game.GetConfigBool("AscentGuidance", "visible") and true or false
	visibilityLoaded = true
end

local function shouldShowHud(player)
	if ui.optionsWindow.isOpen then return false end
	if not player then return false end
	if Game.CurrentView() ~= "WorldView" then return false end
	if not ui.shouldDrawUI() then return false end
	return true
end

-- guidance can take the ship to orbit from here (or is on): on or over a
-- planet and not yet in a safe orbit
local function isAvailable(player)
	return AscentGuidance.IsArmed() or AscentGuidance.CanArm(player)
end

-- a label for the row's control, in the dim caption colour
local function rowLabel(text, tooltip)
	ui.alignTextToFramePadding()
	ui.textColored(colors.fontDim, text)
	if tooltip then ui.setItemTooltip(tooltip) end
	ui.sameLine()
end

-- the switch, with its state spelled out beside it as the burn list does
local function drawSwitch(player)
	local armed = AscentGuidance.IsArmed()
	local tooltip = (armed and lui.ASCENT_GUIDANCE_OFF or lui.ASCENT_GUIDANCE_ON) .. "##ascentGuidanceSwitch"
	if ui.mainMenuButton(icons.autopilot_low_orbit, tooltip, armed, styles.MainButtonSize) then
		if armed then
			AscentGuidance.Disarm()
		else
			AscentGuidance.Arm(player)
		end
		armed = AscentGuidance.IsArmed()
	end
	ui.sameLine()
	ui.withFont(dataFont.name, dataFont.size, function()
		ui.alignTextToLineHeight(styles.MainButtonSize.y)
		ui.textColored(armed and colors.burnHudToggleOnText or colors.burnHudToggleOffText,
			string.format(lui.BURN_STATE_LINE, lui.ASCENT_GUIDANCE, armed and lui.BURN_STATE_ON or lui.BURN_STATE_OFF))
	end)
end

-- the orbit altitude, in km, within the planet's range
local function drawAltitude()
	local low, high = AscentGuidance.GetTargetRange()
	local target = AscentGuidance.GetTarget()
	if not low or not target then return end
	local tooltip = string.format(lui.ASCENT_TARGET_TIP, ui.Format.Distance(low, "%0.0f"))

	rowLabel(lui.ASCENT_ALTITUDE, tooltip)
	ui.pushItemWidth(DRAG_WIDTH)
	local value, changed = ui.incrementDrag("##ascentAltitude", target / 1000, 1, low / 1000, high / 1000,
		"%.0f " .. lc.UNIT_KILOMETERS, false)
	ui.popItemWidth()
	if ui.isItemHovered() then
		ui.setTooltip(tooltip)
		local wheel = ui.getMouseWheel()
		if wheel ~= 0 then
			value, changed = target / 1000 + wheel * (ui.shiftHeld() and 50 or 5), true
		end
	end
	if changed then
		AscentGuidance.SetTarget(math.floor(value + 0.5) * 1000)
	end
end

-- the launch heading: degrees and the four compass points, then the
-- inclination it gives from here; once off the ground it is fixed
local function drawHeading(player)
	local heading = AscentGuidance.GetHeading()
	rowLabel(lui.ASCENT_HEADING, lui.ASCENT_HEADING_TIP)

	if not AscentGuidance.CanSetHeading() then
		ui.textColored(colors.fontDim, string.format("%.0f°", heading))
		ui.setItemTooltip(lui.ASCENT_HEADING_LOCKED)
		return
	end

	ui.pushItemWidth(DRAG_WIDTH)
	local value, changed = ui.incrementDrag("##ascentHeading", heading, 1, 0, 359, "%.0f°", false)
	ui.popItemWidth()
	ui.setItemTooltip(lui.ASCENT_HEADING_TIP)
	if changed then
		AscentGuidance.SetHeading(math.floor(value + 0.5))
	end

	local selected = ui.theme.buttonColors.selected
	for _, preset in ipairs(presets) do
		ui.sameLine(0, preset.heading == 0 and ui.getItemSpacing().x or 2)
		local current = math.abs(heading - preset.heading) < 0.5
		if ui.button(preset.label .. "##ascentHeading" .. preset.heading, nil, current and selected or nil,
			string.format("%d°", preset.heading)) then
			AscentGuidance.SetHeading(preset.heading)
		end
	end

	local _, _, latitude = player:GetGPS()
	if latitude then
		local inclination = AscentGuidance.InclinationFrom(latitude)
		local text = string.format(lui.ASCENT_INCLINATION, string.format("%.1f°", inclination))
		if inclination > 90 then
			text = text .. " " .. lui.ASCENT_RETROGRADE
		end
		ui.textColored(colors.fontDim, text)
	end
end

-- coasting without the circularization burn guidance plans by itself (it
-- could not, or that plan was removed): plan it here, or say what is in the way
local function drawCircularize(player)
	local guidance = AscentGuidance.Update(player)
	if not guidance or guidance.phase ~= AscentGuidance.COAST or AscentGuidance.HasOwnPlan(player) then
		return
	end
	local canPlan = AscentGuidance.CanCircularize(player)
	local tooltip = (canPlan and lui.ASCENT_CIRCULARIZE or lui.ASCENT_CIRCULARIZE_BUSY) .. "##ascentCircularize"
	if ui.mainMenuButton(icons.circularize, tooltip, not canPlan and ui.theme.buttonColors.disabled or nil,
		styles.MainButtonSize) and canPlan then
		AscentGuidance.Circularize(player)
	end
end

-- the panel, anchored by its bottom-right corner to the left of the button
-- column, level with the bottom of its button - or just above the burn list
-- when that is open
local function displayPanel(max, player, toggleSize)
	local spacing = ui.getItemSpacing()
	local bottom = max.y
	if gameView.burnHudPanelTop then
		bottom = math.min(bottom, gameView.burnHudPanelTop - spacing.y)
	end
	local anchor = Vector2(max.x - toggleSize.x - spacing.x, math.max(bottom, 1))

	ui.withStyleColorsAndVars(
		{ WindowBg = colors.windowBackground, Border = colors.windowFrame },
		{ WindowBorderSize = 1.0, WindowRounding = styles.StyleRounding },
		function()
	ui.setNextWindowPos(anchor, "Always", Vector2(1, 1))
	ui.window("AscentGuidanceHud", windowFlags, function()
		ui.withFont(titleFont.name, titleFont.size, function()
			ui.textColored(colors.fontDim, lui.ASCENT_TITLE)
		end)
		ui.separator()
		drawSwitch(player)
		ui.withFont(dataFont.name, dataFont.size, function()
			drawAltitude()
			drawHeading(player)
		end)
		drawCircularize(player)
	end)
	end)
end

-- the toggle in its own window, stacked up the button column; clips the
-- HUD area above it for the modules after this one. Where guidance cannot
-- be used it stays in place, greyed out, and says why
local function displayToggle(min, max, available)
	local size = Vector2(styles.MainButtonSize.x + ui.getWindowPadding().x * 2,
		styles.MainButtonSize.y + ui.getWindowPadding().y * 2)
	size.x = math.min(size.x, max.x - min.x)
	size.y = math.min(size.y, max.y - min.y)

	ui.setNextWindowSize(size, "Always")
	ui.setNextWindowPos(Vector2(max.x - size.x, max.y - size.y), "Always")
	max.y = max.y - size.y

	ui.window("AscentGuidanceHudToggle", toggleFlags, function()
		if not available then
			ui.mainMenuButton(icons.autopilot_low_orbit, lui.ASCENT_UNAVAILABLE .. "##ascentGuidanceToggle",
				ui.theme.buttonColors.disabled, styles.MainButtonSize)
			return
		end
		if ui.mainMenuButton(icons.autopilot_low_orbit, (visible and lui.ASCENT_HIDE or lui.ASCENT_SHOW) .. "##ascentGuidanceToggle",
			visible, styles.MainButtonSize) then
			visible = not visible
			Game.SetConfigBool("AscentGuidance", "visible", visible)
			Engine.SaveSettings()
		end
	end)
	return size
end

local function displayLaunchHud(min, max)
	local player = Game.player
	if not shouldShowHud(player) then return end
	if not visibilityLoaded then loadVisibility() end

	colors = ui.theme.colors
	icons = ui.theme.icons
	styles = ui.theme.styles

	AscentGuidance.Sync(player)

	-- the area reaches down to the burn list's button (burn-hud.lua has
	-- clipped it and the time window away): the panel's bottom lines up with
	-- this button's, as the burn list's with its own
	local panelMax = Vector2(max.x, max.y)
	local available = isAvailable(player)
	local toggleSize = displayToggle(min, max, available)
	if visible and available then
		displayPanel(panelMax, player, toggleSize)
	end
end

gameView.registerHudModule("launch-hud", {
	side = "right",
	showInHyperspace = false,
	debugReload = function() package.reimport() end,
	draw = function(_, min, max)
		displayLaunchHud(min, max)
	end,
})

loadVisibility()
Event.Register("onGameStart", loadVisibility)

return {}
