-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Floating 3D System Orbit View widget for the flight HUD (WorldView only).
--
-- Displays an independent 3D Orrery viewport in a movable, resizable floating
-- window with mouse-only camera controls (right/middle-drag to rotate, scroll
-- wheel to zoom, left-drag to move/resize the window).
--
-- Scope: celestial bodies, starports and stations, Lagrange points (L4/L5),
-- orbits, planned multi-burn orbits/markers, and the player ship.
-- No new keybindings are added or polled.

local Engine = require 'Engine'
local Event = require 'Event'
local Game = require 'Game'
local Lang = require 'Lang'
local ui = require 'pigui'
local Constants = _G.Constants
local Vector2 = _G.Vector2
local lui = Lang.GetResource("ui-core")

local gameView = require 'pigui.views.game'
local _getBodyIcon = require 'pigui.modules.flight-ui.body-icons'

local icons = ui.theme.icons
local colors = ui.theme.colors
local styles = ui.theme.styles

local hudfont = ui.fonts.pionillium.small
local hudfont_highlight = ui.fonts.pionillium.medium

local indicatorSize = Vector2(24, 24)
local bodyIconSize = Vector2(16, 16)

local Projectable = {}
for _, key in pairs(Constants.ProjectableTypes) do
	Projectable[key] = Engine.GetEnumValue("ProjectableTypes", key)
end
for _, key in pairs(Constants.ProjectableBases) do
	Projectable[key] = Engine.GetEnumValue("ProjectableBases", key)
end

local windowFlags = ui.WindowFlags {
	"NoTitleBar",
	"NoCollapse",
	"NoScrollbar",
	"NoScrollWithMouse",
	"NoFocusOnAppearing",
}

local toggleFlags = ui.WindowFlags {
	"NoTitleBar",
	"NoResize",
	"NoMove",
	"NoSavedSettings",
	"NoFocusOnAppearing",
	"NoBringToFrontOnFocus",
	"NoBackground",
}

local L = {
	SHOW = lui.ORBIT_VIEW_SHOW,
	HIDE = lui.ORBIT_VIEW_HIDE,
	FOCUS_BODY = lui.ORBIT_VIEW_FOCUS_BODY,
	FOCUS_PLAYER = lui.ORBIT_VIEW_FOCUS_PLAYER,
}

local visible = false
local focusPlayer = false
local configLoaded = false

local function applyFocus(systemView)
	if not systemView then return end
	systemView:SetHudMapFocus(focusPlayer and "PLAYER" or "FRAME_BODY")
end

local function loadConfig()
	visible = Game.GetConfigBool("MultiBurnHud", "orbitViewVisible") and true or false
	focusPlayer = Game.GetConfigBool("MultiBurnHud", "orbitViewFocusPlayer") and true or false
	configLoaded = true
	if Game.systemView then
		applyFocus(Game.systemView)
	end
end

local function shouldShowHud()
	if ui.optionsWindow.isOpen then return false end
	if not Game.player then return false end
	if Game.CurrentView() ~= "WorldView" then return false end
	if not ui.shouldDrawUI() then return false end
	return true
end

local function getBodyIcon(obj)
	if obj.type == Projectable.APOAPSIS then return icons.apoapsis
	elseif obj.type == Projectable.PERIAPSIS then return icons.periapsis
	elseif obj.type == Projectable.ASCENDING_NODE then return icons.nav_normal
	elseif obj.type == Projectable.DESCENDING_NODE then return icons.nav_antinormal
	elseif obj.type == Projectable.IMPACT then return icons.impact_warning
	elseif obj.type == Projectable.L4 or obj.type == Projectable.L5 then return icons.lagrange_marker
	elseif obj.base == Projectable.PLAYER or obj.base == Projectable.PLANNER or obj.base == Projectable.PLANNER_BURN then
		local shipClass = obj.ref and obj.ref:GetShipClass()
		if shipClass and icons[shipClass] then
			return icons[shipClass]
		end
		return icons.ship
	elseif obj.base == Projectable.SYSTEMBODY then
		if obj.ref.superType ~= "STARPORT" and obj.ref.type ~= "PLANET_ASTEROID" then
			return icons.empty
		end
		return _getBodyIcon(obj.ref, true)
	else
		return _getBodyIcon(obj.ref, true)
	end
end

-- orbit markers labelled on the player's and the planner's orbits, which
-- carry the marker's altitude (other orbits' apsides stay icon-only)
local markerLabels = {
	[Projectable.PERIAPSIS] = lui.ORBIT_MARKER_PERIAPSIS,
	[Projectable.APOAPSIS] = lui.ORBIT_MARKER_APOAPSIS,
	[Projectable.ASCENDING_NODE] = lui.ORBIT_MARKER_ASCENDING_NODE,
	[Projectable.DESCENDING_NODE] = lui.ORBIT_MARKER_DESCENDING_NODE,
}

local function getLabel(obj)
	if markerLabels[obj.type] and obj.altitude then
		return markerLabels[obj.type] .. " " .. ui.Format.Distance(obj.altitude)
	end
	if obj.type == Projectable.IMPACT then
		return string.format(lui.ORBIT_IMPACT_IN, ui.Format.Duration(math.max((obj.time or 0) - Game.time, 0), 2))
	end
	if obj.type == Projectable.OBJECT then
		if obj.base == Projectable.SYSTEMBODY then return obj.ref.name
		elseif obj.base == Projectable.PLANNER then return ""
		elseif obj.base == Projectable.PLANNER_BURN then return string.format(lui.BURN_NUMBER, obj.burnIndex or 1)
		elseif obj.ref then return obj.ref:GetLabel()
		else return "" end
	elseif obj.type == Projectable.L4 then return "L4"
	elseif obj.type == Projectable.L5 then return "L5"
	else return ""
	end
end

local function getColor(obj)
	if obj.type == Projectable.OBJECT then
		if obj.base == Projectable.SYSTEMBODY then return colors.systemMapSystemBodyIcon
		elseif obj.base == Projectable.PLAYER then return colors.systemMapPlayer
		elseif obj.base == Projectable.PLANNER then return colors.systemMapPlanner
		elseif obj.base == Projectable.PLANNER_BURN then return colors.systemMapPlannerBurn
		else return colors.systemMapObject
		end
	elseif markerLabels[obj.type] or obj.type == Projectable.IMPACT then
		if obj.base == Projectable.SYSTEMBODY then return colors.systemMapSystemBodyOrbit
		elseif obj.base == Projectable.PLAYER then return colors.systemMapPlayerOrbit
		elseif obj.base == Projectable.PLANNER then return colors.systemMapPlannerOrbit
		else return colors.unknown
		end
	elseif obj.type == Projectable.L4 or obj.type == Projectable.L5 then
		return colors.systemMapLagrangePoint
	else
		return colors.unknown
	end
end

local function drawGroupIcons(coords, icon, color, iconSize, group)
	local stackedSize = indicatorSize
	local stackStep = Vector2(8, 8)
	if group.hasPlayer then
		ui.addIcon(coords, icons.square, colors.systemMapPlayer, stackedSize, ui.anchor.center, ui.anchor.center)
		stackedSize = stackedSize + stackStep
	end
	if group.hasNavTarget then
		ui.addIcon(coords, icons.square, colors.navTarget, stackedSize, ui.anchor.center, ui.anchor.center)
		stackedSize = stackedSize + stackStep
	end
	if group.hasPlanner then
		ui.addIcon(coords, icons.square, colors.systemMapPlanner, stackedSize, ui.anchor.center, ui.anchor.center)
		stackedSize = stackedSize + stackStep
	end

	ui.addIcon(coords, icon, color, iconSize, ui.anchor.center, ui.anchor.center)
end

local function drawProjectedOverlay(systemView, widgetOrigin, widgetSize)
	local objects_grouped = systemView:GetHudMapProjectedGrouped(bodyIconSize)
	if not objects_grouped then return end

	local should_show_label = ui.shouldShowLabels()
	local label_offset = 12
	local hover_radius = bodyIconSize:length() * 0.6
	local mp = ui.getMousePos()
	local winHovered = ui.isWindowHovered()

	ui.withClipRect(widgetOrigin, widgetOrigin + widgetSize, function()
		for _, group in ipairs(objects_grouped) do
			local mainObject = group.mainObject
			local mainCoords = widgetOrigin + Vector2(group.screenCoordinates.x, group.screenCoordinates.y)
			group.hasPlanner = mainObject.type == Projectable.OBJECT and mainObject.base == Projectable.PLANNER

			drawGroupIcons(mainCoords, getBodyIcon(mainObject), getColor(mainObject), bodyIconSize, group)

			local label = getLabel(mainObject)
			if #label > 0 then
				local mouseover = winHovered and (mp - mainCoords):length() < hover_radius
				if should_show_label or mouseover then
					if group.objects then
						label = label .. " (" .. #group.objects .. ")"
					end
					local pos = mainCoords + Vector2(label_offset, 0)
					local font = mouseover and hudfont_highlight or hudfont
					-- the impact warning reads in red; its icon keeps its orbit's colour
					local labelColor = mainObject.type == Projectable.IMPACT and colors.alertRed or getColor(mainObject)
					ui.addStyledText(pos + Vector2(1, 1), ui.anchor.left, ui.anchor.center, label, colors.black, font)
					ui.addStyledText(pos, ui.anchor.left, ui.anchor.center, label, labelColor, font)
				end
			end
		end
	end)
end

local function displayOrbitViewWindow(systemView)
	local minSize = Vector2(ui.rescaleUI(180), ui.rescaleUI(140))
	local maxSize = Vector2(ui.screenWidth * 0.85, ui.screenHeight * 0.85)
	local defaultSize = Vector2(ui.rescaleUI(360), ui.rescaleUI(280))
	local defaultPos = Vector2(ui.rescaleUI(24), math.max(ui.rescaleUI(24), ui.screenHeight - ui.rescaleUI(380)))

	ui.setNextWindowSizeConstraints(minSize, maxSize)
	ui.setNextWindowSize(defaultSize, "FirstUseEver")
	ui.setNextWindowPos(defaultPos, "FirstUseEver")
	ui.setNextWindowPadding(Vector2(0, 0))

	ui.withStyleColorsAndVars(
		{ WindowBg = colors.black, Border = colors.windowFrame },
		{ WindowBorderSize = 1.0, WindowRounding = styles.StyleRounding },
		function()
			ui.window("OrbitViewHud", windowFlags, function()
				local widgetOrigin = ui.getCursorScreenPos()
				local contentSize = ui.getContentRegion()
				if contentSize.x >= 16 and contentSize.y >= 16 then
					systemView:SetHudMapSize(contentSize)
					systemView:DrawHudMap()
					drawProjectedOverlay(systemView, widgetOrigin, contentSize)
				end
			end)
		end
	)
end

local function displayControlsToggle(min, max, systemView)
	local spacing = ui.getItemSpacing()
	local padding = ui.getWindowPadding()
	local width = styles.MainButtonSize.x + padding.x * 2
	local height = styles.MainButtonSize.y * 2 + spacing.y + padding.y * 2

	local size = Vector2(
		math.min(width, math.max(0, max.x - min.x)),
		math.min(height, math.max(0, max.y - min.y))
	)
	local pos = Vector2(max.x - size.x, max.y - size.y)
	max.y = max.y - size.y

	ui.setNextWindowSize(size, "Always")
	ui.setNextWindowPos(pos, "Always")

	ui.window("OrbitViewHudToggle", toggleFlags, function()
		if ui.mainMenuButton(icons.orbit_view_3d, (visible and L.HIDE or L.SHOW) .. "##orbitViewHudToggle",
			visible, styles.MainButtonSize) then
			visible = not visible
			Game.SetConfigBool("MultiBurnHud", "orbitViewVisible", visible)
			Engine.SaveSettings()
		end

		local focusIcon = focusPlayer and icons.ship or icons.rocky_planet
		local focusTip = (focusPlayer and L.FOCUS_PLAYER or L.FOCUS_BODY) .. "##orbitViewHudFocus"
		if ui.mainMenuButton(focusIcon, focusTip, focusPlayer, styles.MainButtonSize) then
			focusPlayer = not focusPlayer
			applyFocus(systemView)
			Game.SetConfigBool("MultiBurnHud", "orbitViewFocusPlayer", focusPlayer)
			Engine.SaveSettings()
		end
	end)
end

local function displayOrbitViewHud(min, max)
	if not shouldShowHud() then return end
	if not configLoaded then loadConfig() end

	colors = ui.theme.colors
	icons = ui.theme.icons
	styles = ui.theme.styles

	local player = Game.player
	local systemView = Game.systemView
	if not player or not systemView then return end

	applyFocus(systemView)

	if visible then
		displayOrbitViewWindow(systemView)
	end

	displayControlsToggle(min, max, systemView)
end

local function onDebugReload()
	package.reimport()
end

gameView.registerHudModule("orbit-view-hud", {
	side = "right",
	showInHyperspace = false,
	debugReload = onDebugReload,
	draw = function(_, min, max)
		displayOrbitViewHud(min, max)
	end,
})

loadConfig()
Event.Register("onGameStart", function()
	loadConfig()
end)

return {}

