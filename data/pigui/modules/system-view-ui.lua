-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

local Game = require 'Game'
local Engine = require 'Engine'
local Event = require 'Event'
local Lang = require 'Lang'
local ui = require 'pigui'
local Format = require 'Format'
local Gravity = require 'pigui.libs.gravity'
local SpaceStation = require 'SpaceStation'
local ShipDef = require 'ShipDef'
local Constants = _G.Constants

local Vector2 = _G.Vector2
local lc = Lang.GetResource("core")
local ls = Lang.GetResource("ships")
local luc = Lang.GetResource("ui-core")
local layout = require 'pigui.libs.window-layout'
local Sidebar = require 'pigui.libs.sidebar'
local ApproachTarget = require 'pigui.libs.approach-target'

local systemEconView = require 'pigui.modules.system-econ-view'.New()

local player = nil
local colors = ui.theme.colors
local icons = ui.theme.icons
local styles = ui.theme.styles

local systemView = Game and Game.systemView -- for hot-reload

local indicatorSize = Vector2(30, 30)
local bodyIconSize = Vector2(18, 18)

local selectedObject -- object, centered in SystemView

local hudfont = ui.fonts.pionillium.small
local hudfont_highlight = ui.fonts.pionillium.medium
local detailfont = ui.fonts.pionillium.medium
local winfont = ui.fonts.pionillium.medlarge

local atlasfont = ui.fonts.pionillium.medium
local atlasfont_highlight = ui.fonts.pionillium.medlarge

local atlas_line_length = ui.rescaleUI(24)
local atlas_label_offset = ui.rescaleUI(Vector2(12, -8))

local comboStyle = require 'pigui.styles'.combo

--load enums Projectable::types and Projectable::bases in one table "Projectable"
local Projectable = {}
for _, key in pairs(Constants.ProjectableTypes) do Projectable[key] = Engine.GetEnumValue("ProjectableTypes", key) end
for _, key in pairs(Constants.ProjectableBases) do Projectable[key] = Engine.GetEnumValue("ProjectableBases", key) end

-- all colors, used in this module
local svColor = {
	COMBAT_TARGET = colors.combatTarget,
	FONT = colors.font,
	GRID = colors.systemMapGrid,
	GRID_LEG = colors.systemMapGridLeg,
	IMPACT_LABEL = colors.alertRed,
	LAGRANGE = colors.systemMapLagrangePoint,
	NAV_TARGET = colors.navTarget,
	OBJECT = colors.systemMapObject,
	PLANNER = colors.systemMapPlanner,
	PLANNER_BURN = colors.systemMapPlannerBurn,
	PLANNER_BURN_DONE = colors.fontDisabled,
	PLANNER_ORBIT = colors.systemMapPlannerOrbit,
	PLAYER = colors.systemMapPlayer,
	PLAYER_ORBIT = colors.systemMapPlayerOrbit,
	SELECTED = ui.theme.colors.font,
	SELECTED_SHIP_ORBIT = colors.systemMapSelectedShipOrbit,
	SHIP = colors.systemMapShip,
	SHIP_ORBIT = colors.systemMapShipOrbit,
	SYSTEMBODY = colors.systemMapSystemBody,
	SYSTEMBODY_ICON = colors.systemMapSystemBodyIcon,
	SYSTEMBODY_ORBIT = colors.systemMapSystemBodyOrbit,
	UNKNOWN = colors.unknown
}

-- button states
local colorset = ui.theme.buttonColors

local buttonState = {
	[true]        = {                                  state = colorset.default },
	[false]       = {                                  state = colorset.transparent },
	DISABLED      = {                                  state = colorset.dark }
}

--- @param s string A string to test
--- @return boolean if string "s" is nil or the empty string
local function isEmptyString(s)
	return s == nil or s == ""
end

--- A helper-class to represent a combo dropdown state used to control the
--- visibility of something in the system view. Whenever the update() function
--- is called the visibility of the managed item is updated in the system view.
--- Options in the drop-down map to direct systemview visibility settings and
--- optionally there is support for a separate on/off toggle as well.
local SystemViewComboState = {
	new = function(self, o)
		o = o or {}
		setmetatable(o, self)
		self.__index = self
		return o
	end,

	--- The currently selected item
	selectedItem = 0,
	--- The human-readable strings to be displayed in the combo box
	items = {
	},
	--- The system view display modes corresponding to each item in items.
	displayModes = {
	},
	--- Whether the system view item should be displayed or not. This is only
	--- used when "displayModeOff" is not nil or empty.
	--- If "displayModeOff" is nil or empty, the "Off" system view mode should
	--- be part of "displayModes".
	doDisplay = false,
	--- The system view setting for when the item should not be displayed. When
	--- this is set, "configKeyDoDisplay" should also be set in order to
	--- correctly save the configuration.
	displayModeOff = nil,

	--- The game configuration key
	configKeySelectedItem = "",
	--- The game configuration key for doDisplay, should only be set if
	--- "displayModeOff" has been set.
	configKeyDoDisplay = nil,

	-- Returns the currently-selected mode
	getMode = function(self)
		if not self.doDisplay and not isEmptyString(self.displayModeOff) then
			return self.displayModeOff
		end
		return self.displayModes[self.selectedItem+1]
	end,

	-- update current mode and update the system view
	update = function(self, args)
		if args.doDisplay ~= nil then
			self.doDisplay = args.doDisplay
		end
		self.selectedItem = args.selectedItem or self.selectedItem
		systemView:SetVisibility(self:getMode())
	end,

	-- loads the configuration and applies it
	loadConfig = function(self)
		local selectedItemSetting = Game.GetConfigInt("SystemView", self.configKeySelectedItem)
		local doDisplaySetting = false
		if not isEmptyString(self.configKeyDoDisplay) then
			doDisplaySetting = Game.GetConfigBool("SystemView", self.configKeyDoDisplay)
		end
		self:update{
			doDisplay = doDisplaySetting,
			selectedItem = selectedItemSetting
		}
	end,

	-- saves the configuration
	saveConfig = function(self)
		Game.SetConfigInt("SystemView", self.configKeySelectedItem, self.selectedItem)
		if not isEmptyString(self.configKeyDoDisplay) then
			Game.SetConfigBool("SystemView", self.configKeyDoDisplay, self.doDisplay)
		end
	end,

	--- Toggle visibility on/off
	--- This function is only active if the "displayModeOff" member is set
	toggleDisplay = function(self)
		self:update{doDisplay = not self.doDisplay}
	end
}

local shipDisplayMode = SystemViewComboState:new{
	items = {
		lc.SHIPS_DISPLAY_MODE_SHIPS_ONLY,
		lc.SHIPS_DISPLAY_MODE_SHIPS_ORBITS
	},
	displayModes = {
		"SHIPS_ON",
		"SHIPS_ORBITS"
	},
	displayModeOff = "SHIPS_OFF",
	configKeySelectedItem = "ShipDisplayMode",
	configKeyDoDisplay = "ShipDisplayModeOn"
}

local cloudDisplayMode = SystemViewComboState:new{
	items = {
		lc.HYPERSPACE_CLOUDS_DISPLAY_MODE_ALL,
		lc.HYPERSPACE_CLOUDS_DISPLAY_MODE_ARRIVAL_ONLY,
		lc.HYPERSPACE_CLOUDS_DISPLAY_MODE_DEPARTURE_ONLY
	},
	displayModes = {
		"CLOUDS_ON",
		"CLOUDS_ARRIVAL",
		"CLOUDS_DEPARTURE"
	},
	displayModeOff = "CLOUDS_OFF",
	configKeySelectedItem = "CloudDisplayMode",
	configKeyDoDisplay = "CloudDisplayModeOn"
}

local gridDisplayMode = SystemViewComboState:new{
	items = {
		lc.GRID_DISPLAY_MODE_OFF,
		lc.GRID_DISPLAY_MODE_GRID_ONLY,
		lc.GRID_DISPLAY_MODE_GRID_AND_LEGS
	},
	displayModes = {
		"GRID_OFF",
		"GRID_ON",
		"GRID_AND_LEGS"
	},
	configKeySelectedItem = "GridDisplayMode",
}

local l4l5DisplayMode = SystemViewComboState:new{
	items = {
		lc.L4L5_DISPLAY_MODE_OFF,
		lc.L4L5_DISPLAY_MODE_ICONS_ONLY,
		lc.L4L5_DISPLAY_MODE_ICONS_AND_TEXT
	},
	displayModes = {
		"LAG_OFF",
		"LAG_ICON",
		"LAG_ICONTEXT"
	},
	configKeySelectedItem = "L4L5DisplayMode",
}

-- latitude and longitude lines on planets and moons, toggled by their own
-- button
local LATLONG_DISPLAY_MODE = luc.SYSTEM_VIEW_LATLONG_LINES
local latLongDisplayMode = SystemViewComboState:new{
	items = {
		luc.SYSTEM_VIEW_LATLONG_EVERY_30,
		luc.SYSTEM_VIEW_LATLONG_EVERY_15
	},
	displayModes = {
		"LATLONG_30",
		"LATLONG_15"
	},
	displayModeOff = "LATLONG_OFF",
	configKeySelectedItem = "LatLongDisplayMode",
	configKeyDoDisplay = "LatLongDisplayModeOn"
}

local onGameStart = function ()
	--connect to class SystemView
	systemView = Game.systemView
	--export several colors to class SystemView (only those which mentioned in the enum SystemViewColorIndex)
	for _, key in pairs(Constants.SystemViewColorIndex) do
		systemView:SetColor(key, svColor[key])
	end
	-- update visibility states
	shipDisplayMode:loadConfig()
	cloudDisplayMode:loadConfig()
	gridDisplayMode:loadConfig()
	l4l5DisplayMode:loadConfig()
	latLongDisplayMode:loadConfig()
end

local onGameEnd = function ()
	-- save visibility states
	shipDisplayMode:saveConfig()
	cloudDisplayMode:saveConfig()
	gridDisplayMode:saveConfig()
	l4l5DisplayMode:saveConfig()
	latLongDisplayMode:saveConfig()
	Engine.SaveSettings()
end

local onEnterSystem = function (ship)
	Game.systemView:SetVisibility("RESET_VIEW");
end

-- all windows in this view
local Windows = {
	systemName = layout.NewWindow("SystemMapSystemName"),
	edgeButtons = layout.NewWindow("SystemMapEdgeButtons"),
	timeButtons = layout.NewWindow("SystemMapTimeButtons"),
	unexplored = layout.NewWindow("SystemMapUnexplored")
}

Windows.systemName.style_colors["WindowBg"] = colors.transparent

local systemViewLayout = layout.New(Windows)
systemViewLayout.mainFont = winfont

local leftSidebar = Sidebar.New("##SystemMapLeft", "left")
local rightSidebar = Sidebar.New("##SystemMapRight", "right")

local systemOverviewWidget = require 'pigui.modules.system-overview-window'.New()
systemOverviewWidget.visible = true

function systemOverviewWidget:onBodySelected(sBody)
	systemView:SetSelectedObject(Projectable.OBJECT, Projectable.SYSTEMBODY, sBody)
end

function systemOverviewWidget:onBodyDoubleClicked(sBody)
	systemView:ViewSelectedObject()
end

--
-- Helper functions
--

local function textIcon(icon, tooltip)
	ui.icon(icon, Vector2(ui.getTextLineHeight()), svColor.FONT, tooltip)
	ui.sameLine()
end

-- Draw one transfer planner stepper: decrease / reset / increase and its value.
-- idx is the burn the control edits. idx == nil keeps the index-less legacy calls,
-- which the global factor control relies on (factor ignores the index entirely).
local function showDvLine(idx, key, leftIcon, resetIcon, rightIcon, Formatter, leftTooltip, resetTooltip, rightTooltip, buttonSize)
	local add = function(delta)
		if idx ~= nil then
			systemView:TransferPlannerAdd(idx, key, delta)
		else
			systemView:TransferPlannerAdd(key, delta)
		end
	end
	local reset = function()
		if idx ~= nil then
			systemView:TransferPlannerReset(idx, key)
		else
			systemView:TransferPlannerReset(key)
		end
	end
	local read = function()
		if idx ~= nil then
			return systemView:TransferPlannerGet(idx, key)
		end
		return systemView:TransferPlannerGet(key)
	end
	local wheel = function()
		if ui.isItemHovered() then
			local w = ui.getMouseWheel()
			if w ~= 0 then
				add(w * 10)
			end
		end
	end
	-- per-burn rows need ids of their own; the legacy factor row keeps its existing id
	local id =  "##" .. key .. (idx ~= nil and ("_" .. idx) or "")
	local size = buttonSize and Vector2(buttonSize, buttonSize)

	local press = ui.mainMenuButton(leftIcon, leftTooltip..id, nil, size)
	if press or (key ~= "factor" and ui.isItemActive()) then
		add(-10)
	end
	wheel()
	ui.sameLine()
	if ui.mainMenuButton(resetIcon, resetTooltip..id, nil, size) then
		reset()
	end
	wheel()
	ui.sameLine()
	press = ui.mainMenuButton(rightIcon, rightTooltip..id, nil, size)
	if press or (key ~= "factor" and ui.isItemActive()) then
		add(10)
	end
	wheel()
	ui.sameLine()
	local speed, speed_unit = Formatter(read())
	ui.text(speed .. " " .. speed_unit)
	return 0
end

local time_selected_button_icon = icons.time_center

local function timeButton(icon, tooltip, factor)
	if ui.mainMenuButton(icon, tooltip) then
		time_selected_button_icon = icon
	end
	local active = ui.isItemActive()
	if active then
		systemView:AccelerateTime(factor)
	end
	ui.sameLine()
	return active
end

-- Format the mass in tonnes
-- The ui.Format() function reformats the units whereas we always want to
-- display using tonnes.
local function formatMass(massInTonnes)
	return string.format("%d %s", massInTonnes or 0, lc.UNIT_TONNES)
end

-- cache some data to reduce per-frame overheads
local data_cache = {
	body = {},
	prev_body = nil
}

local function appendPlayerTWR(data, systemBody)
	if not player then return data end

	local out = {}
	for _, row in ipairs(data) do
		table.insert(out, row)
		if row.name == lc.SURFACE_GRAVITY then
			table.insert(out, {
				name = luc.TWR_AT_SURFACE,
				icon = icons.body_radius,
				value = ui.Format.TWR(player:GetAcceleration("up") / (Gravity.GetSurfaceGravity(systemBody) or math.huge), 2),
			})
		end
	end
	return out
end

-- populate a data structure with information about the currently selected body
local function getObjectData(obj)
	local isSystemBody = obj.base == Projectable.SYSTEMBODY
	local body = obj.ref

	local data = {}

	--SystemBody data is static so we use cache
	--Ship data migh be dynamic in the future
	if isSystemBody and data_cache.prev_body == body then
		data = data_cache.body
	else
		data_cache.prev_body = body

		if isSystemBody then -- system body
			local parent = body.parent
			local starport = body.superType == "STARPORT"
			local surface = body.type == "STARPORT_SURFACE"
			local sma = body.semiMajorAxis
			local semimajoraxis = nil
			if sma and sma > 0 then
				semimajoraxis = ui.Format.Distance(sma)
			end
			local rp = body.rotationPeriod * 24 * 60 * 60
			local op = body.orbitPeriod * 24 * 60 * 60
			local pop = math.round(body.population * 1e9)
			local techLevel = starport and SpaceStation.GetTechLevel(body) or nil
			if techLevel == 11 then
				techLevel = luc.MILITARY
			end
			data = {
				{ name = lc.MASS, icon = icons.body_radius,
					value = (not starport) and ui.Format.Mass(body.mass) or nil },
				{ name = lc.RADIUS, icon = icons.body_radius,
					value = (not starport) and ui.Format.Distance(body.radius) or nil },
				{ name = lc.SURFACE_TEMPERATURE, icon = icons.temperature,
					value = (not starport) and ui.Format.Temperature(body.averageTemp) or nil },
				{ name = lc.SURFACE_PRESSURE, icon = icons.pressure,
					value = (not starport) and ui.Format.Pressure(body.surfacePressure) or nil },
				{ name = lc.SURFACE_GRAVITY, icon = icons.body_radius,
					value = (not starport) and ui.Format.Speed(body.gravity, true).."²"..
												" ("..ui.Format.Gravity(body.gravity / 9.80665)..")" or nil },
				{ name = lc.ESCAPE_VELOCITY, icon = icons.body_radius,
					value = (not starport) and ui.Format.Speed(body.escapeVelocity , true) or nil },
				{ name = lc.MEAN_DENSITY, icon = icons.body_radius,
					value = (not starport) and ui.Format.Number(body.meanDensity, 0).." "..lc.UNIT_DENSITY or nil },
				{ name = lc.ORBITAL_PERIOD, icon = icons.body_orbit_period,
					value = op and op > 0 and ui.Format.Duration(op, 2) or nil },
				{ name = lc.DAY_LENGTH, icon = icons.body_day_length,
					value = rp > 0 and ui.Format.Duration(rp, 2) or nil },
				{ name = luc.ORBIT_APOAPSIS, icon = icons.body_semi_major_axis,
					value = (parent and not surface) and ui.Format.Distance(body.apoapsis) or nil },
				{ name = luc.ORBIT_PERIAPSIS, icon = icons.body_semi_major_axis,
					value = (parent and not surface) and ui.Format.Distance(body.periapsis) or nil },
				{ name = lc.SEMI_MAJOR_AXIS, icon = icons.body_semi_major_axis,
					value = semimajoraxis },
				{ name = lc.ECCENTRICITY, icon = icons.body_semi_major_axis,
					value = (parent and not surface) and string.format("%0.2f", body.eccentricity) or nil },
				{ name = lc.AXIAL_TILT, icon = icons.body_semi_major_axis,
					value = (not starport) and string.format("%0.2f", body.axialTilt) or nil },
				{ name = lc.POPULATION, icon = icons.personal,
					value = pop > 0 and ui.Format.NumberAbbv(pop) or nil },
				{ name = luc.TECH_LEVEL, icon = icons.equipment,
					value = starport and techLevel or nil }
			}

			--change the internal cached data only when new is fully built
			--prevents additional flickering
			data_cache.body = data

		elseif obj.ref:IsShip() then -- physical body
			---@cast body Ship
			-- TODO: the advanced target scanner should add additional data here,
			-- but we really do not want to hardcode that here. there should be
			-- some kind of hook that the target scanner can hook into to display
			-- more info here.
			-- This is what should be inserted:
			local shipdef = ShipDef[body.shipId]
			table.insert(data, { name = luc.SHIP_TYPE, value = ls[shipdef.i18n_key] })
			if (player["target_scanner_level_cap"] or 0) > 0 then
				local hd = body:GetInstalledHyperdrive()
				table.insert(data, { name = luc.HYPERDRIVE, value = hd and hd:GetName() or lc.NO_HYPERDRIVE })
				table.insert(data, { name = luc.MASS, value = Format.MassTonnes(body.staticMass) })
				table.insert(data, { name = luc.CARGO, value = Format.MassTonnes(body.usedCargo) })
			end
		elseif obj.ref:IsHyperspaceCloud() then
			---@cast body HyperspaceCloud
			local hypercloud_level = (player["hypercloud_analyzer_cap"] or 0)
			local ship = body:GetShip()
			local shipdef = ShipDef[ship.shipId]
			-- TODO: different levels of hypercloud analyser should provide
			-- increasingly detailed amounts of information.
			if hypercloud_level > 0 and ship then
				local _,systemName = ship:GetHyperspaceDestination()
				local systemLabel = body:IsArrival() and luc.HUD_HYPERSPACE_ORIGIN or luc.HUD_HYPERSPACE_DESTINATION
				table.insert(data, { name = luc.SHIP_TYPE, value = ls[shipdef.i18n_key] })
				table.insert(data, { name = luc.HUD_MASS, value = formatMass(ship.staticMass) })
				table.insert(data, { name = systemLabel, value = systemName })
				table.insert(data, { name = luc.HUD_ARRIVAL_DATE, value = ui.Format.Datetime(body:GetDueDate()) })
			end
		else
			data = {}
		end
	end

	if isSystemBody then
		return appendPlayerTWR(data, body)
	end
	return data
end

-- render a two-column dataset
local function tabular(data, maxSize)
	if data and #data > 0 then
		ui.columns(2, "Attributes", false)
		local nameWidth = 0
		local valueWidth = 0

		for _,item in pairs(data) do
			if item.value then
				local nWidth = ui.calcTextSize(item.name).x + ui.getItemSpacing().x
				local vWidth = ui.calcTextSize(item.value).x + ui.getItemSpacing().x
				if ui.getColumnWidth() < nWidth then
					textIcon(item.icon or icons.info, item.name)
				else
					ui.text(item.name)
				end
				ui.nextColumn()
				ui.text(item.value)
				ui.nextColumn()

				nameWidth = math.max(nameWidth, nWidth)
				valueWidth = math.max(valueWidth, vWidth)
			end
		end
		if maxSize > 0 and nameWidth + valueWidth > maxSize then
			-- first of all, we want to see the values, but the keys should not be too small either
			nameWidth = math.max(maxSize - valueWidth, maxSize * 0.1)
		end
		ui.setColumnWidth(0, nameWidth)

		-- reset columns
		ui.columns(1)
	end
end

local _getBodyIcon = require 'pigui.modules.flight-ui.body-icons'
local function getBodyIcon(obj, forWorld, isOrrery)
	if obj.type == Projectable.APOAPSIS then return icons.apoapsis
	elseif obj.type == Projectable.PERIAPSIS then return icons.periapsis
	elseif obj.type == Projectable.ASCENDING_NODE then return icons.normal_thin
	elseif obj.type == Projectable.DESCENDING_NODE then return icons.antinormal_thin
	elseif obj.type == Projectable.IMPACT then return icons.impact_warning
	elseif obj.type == Projectable.L4 then return icons.lagrange_marker
	elseif obj.type == Projectable.L5 then return icons.lagrange_marker
	elseif obj.base == Projectable.PLAYER or obj.base == Projectable.PLANNER or obj.base == Projectable.PLANNER_BURN then
		local shipClass = obj.ref:GetShipClass()
		if icons[shipClass] then
			return icons[shipClass]
		else
			return icons.ship
		end
	elseif forWorld and not isOrrery and obj.ref.superType ~= "STARPORT" and obj.ref.type ~= "PLANET_ASTEROID" then
		return icons.empty
	else
		return _getBodyIcon(obj.ref, forWorld)
	end
end


--
-- The sidebar views
--

---@type UI.Sidebar.Module
local overviewView = {
	showInHyperspace = false,
	icon = icons.system_overview,
	tooltip = luc.TOGGLE_OVERVIEW_WINDOW,
	exclusive = true,

	drawTitle = function()
		if Windows.unexplored.visible then
			ui.text(lc.UNEXPLORED_SYSTEM_NO_SYSTEM_VIEW)
		else
			systemOverviewWidget:displaySidebarTitle(systemView:GetSystem())
		end
	end,

	drawBody = function()
		local system = systemView:GetSystem() ---@type StarSystem
		if not system or Windows.unexplored.visible then
			return
		end

		systemOverviewWidget:displaySearch()

		systemOverviewWidget.size.y = math.max(ui.getContentRegion().y, ui.screenHeight / 1.6)

		local root = system.rootSystemBody
		local selected = { [systemView:GetSelectedObject().ref or 0] = true }

		systemOverviewWidget:display(Game.system, root, selected)
	end
}

---@type UI.Sidebar.Module
local economyView = {
	showInHyperspace = false,
	icon = icons.money,
	tooltip = luc.ECONOMY_TRADE,
	exclusive = true,

	drawTitle = function()
		ui.text(luc.ECONOMY_TRADE)
	end,

	drawBody = function()
		local selected = systemView:GetSelectedObject().ref ---@type SystemBody?
		local docked = Game.player:GetDockedWith()
		local current

		if docked then
			if not selected or not selected.isStation then
				selected = docked:GetSystemBody()
			else
				current = docked:GetSystemBody()
			end
		end

		if not selected or not selected.isStation then
			systemEconView:drawSystemComparison(systemView:GetSystem())
		else
			systemEconView:drawStationComparison(selected, current)
		end
	end
}

---@type UI.Sidebar.Module
local settingsView = {
	icon = icons.settings,
	tooltip = luc.SETTINGS,
	title = luc.SETTINGS,

	drawBody = function(self)
		if systemView:GetDisplayMode() == "Orrery" then
			comboStyle:withStyle(function()
				local c,ret

				ui.text(lc.SHIPS_DISPLAY_MODE)
				c,ret = ui.combo("##Ships", shipDisplayMode.selectedItem, shipDisplayMode.items)
				if c then
					shipDisplayMode:update{selectedItem = ret}
				end

				ui.text(lc.L4L5_DISPLAY_MODE)
				c,ret = ui.combo("##LagrangePoint", l4l5DisplayMode.selectedItem, l4l5DisplayMode.items)
				if c then
					l4l5DisplayMode:update{selectedItem = ret}
				end

				ui.text(lc.GRID_DISPLAY_MODE)
				c,ret = ui.combo("##Grid", gridDisplayMode.selectedItem, gridDisplayMode.items)
				if c then
					gridDisplayMode:update{selectedItem = ret}
				end

				ui.text(lc.HYPERSPACE_CLOUDS_DISPLAY_MODE)
				c,ret = ui.combo("##Cloud", cloudDisplayMode.selectedItem, cloudDisplayMode.items)
				if c then
					cloudDisplayMode:update{selectedItem = ret}
				end

				ui.text(LATLONG_DISPLAY_MODE)
				c,ret = ui.combo("##LatLong", latLongDisplayMode.selectedItem, latLongDisplayMode.items)
				if c then
					latLongDisplayMode:update{selectedItem = ret}
				end
			end)
		end
	end
}

---@type UI.Sidebar.Module
local infoView = {
	icon = icons.info,
	tooltip = lc.OBJECT_INFO,
	showInHyperspace = false,
	--exclusive = true,

	drawTitle = function()
		local obj = systemView:GetSelectedObject()
		if obj ~= nil and obj.ref ~= nil then
			local isSystemBody = obj.base == Projectable.SYSTEMBODY
			local body = obj.ref
			ui.text(ui.get_icon_glyph(getBodyIcon(obj)) .. " " .. (isSystemBody and body.name or body.label))
		end
	end,

	drawBody = function()
		local obj = systemView:GetSelectedObject()

		if obj ~= nil and obj.ref ~= nil then
			local isSystemBody = obj.base == Projectable.SYSTEMBODY
			local body = obj.ref

			if isSystemBody then
				ui.textWrapped(body.astroDescription)

				ui.separator()
				ui.spacing()
			end

			local data = getObjectData(obj)
			tabular(data, 0)
		else
			ui.textColored(colors.fontDim, lc.SELECT_A_TARGET .. "...")
		end
	end
}

-- the burn list's and the planner's strings, from the ui-core lang resource.
-- The status labels and the Complete control share their keys with the flight
-- HUD's burn list (flight-ui/burn-hud.lua), so the map sidebar and the HUD read
-- identically; the numeric drag format stays literal
local L = {
	STATUS_PENDING = luc.BURN_STATUS_PLANNED,
	STATUS_ACTIVE = luc.BURN_STATUS_ACTIVE,
	STATUS_COMPLETED = luc.BURN_STATUS_COMPLETED,
	BURN = luc.BURN_NUMBER,
	ADD_BURN = luc.PLANNER_ADD_BURN,
	REMAINING_DV = luc.PLANNER_REMAINING_DV,
	ZOOM_TO_SHIP = luc.SYSTEM_VIEW_ZOOM_TO_SHIP,
	FOLLOW_PLAN = luc.SYSTEM_VIEW_FOLLOW_PLAN,
	COMPLETE = luc.BURN_MARK_COMPLETE,
	IMPACT_IN = luc.ORBIT_IMPACT_IN,
	-- snap row: the burn's node time jumps to an event of the orbit leading
	-- into it (the current orbit for the first burn)
	SNAP_TIP = luc.PLANNER_SNAP_TIP,
	SNAP_NODE_TIP = luc.PLANNER_SNAP_NODE_TIP,
	SNAP_NONE = luc.PLANNER_SNAP_NONE,
	SNAP_NODE_NONE = luc.PLANNER_SNAP_NODE_NONE,
	SNAP_COMPLETED = luc.PLANNER_BURN_IS_COMPLETE,
	CIRCULARIZE = luc.PLANNER_CIRCULARIZE,
	CIRCULARIZE_TIP = luc.PLANNER_CIRCULARIZE_TIP,
	CIRCULARIZE_AT_APSIS = luc.PLANNER_CIRCULARIZE_AT_APSIS,
	CIRCULARIZE_AT_POINT = luc.PLANNER_CIRCULARIZE_AT_POINT,
	-- closest-approach planning
	APPROACH_TITLE = luc.PLANNER_APPROACH_TITLE,
	APPROACH_TARGET = luc.PLANNER_APPROACH_TARGET,
	APPROACH_FROM_SELECTION = luc.PLANNER_APPROACH_FROM_SELECTION,
	APPROACH_FROM_NAV = luc.PLANNER_APPROACH_FROM_NAV,
	APPROACH_NO_TARGET = luc.PLANNER_APPROACH_NO_TARGET,
	APPROACH_INTERCEPT = luc.PLANNER_APPROACH_INTERCEPT,
	APPROACH_INTERCEPT_TIP = luc.PLANNER_APPROACH_INTERCEPT_TIP,
	APPROACH_RENDEZVOUS = luc.PLANNER_APPROACH_RENDEZVOUS,
	APPROACH_RENDEZVOUS_TIP = luc.PLANNER_APPROACH_RENDEZVOUS_TIP,
	APPROACH_PLAN = luc.PLANNER_APPROACH_PLAN,
	APPROACH_PLAN_TIP = luc.PLANNER_APPROACH_PLAN_TIP,
	APPROACH_ONE_BURN = luc.PLANNER_APPROACH_ONE_BURN,
	APPROACH_TWO_BURNS = luc.PLANNER_APPROACH_TWO_BURNS,
	APPROACH_STAND_OFF = luc.PLANNER_APPROACH_STAND_OFF,
	APPROACH_STAND_OFF_FORMAT = luc.PLANNER_APPROACH_STAND_OFF_FORMAT,
	APPROACH_STAND_OFF_TIP = luc.PLANNER_APPROACH_STAND_OFF_TIP,
	APPROACH_FLYBY = luc.PLANNER_APPROACH_FLYBY,
	APPROACH_FLYBY_FORMAT = "%.0f km",
	APPROACH_FLYBY_TIP = luc.PLANNER_APPROACH_FLYBY_TIP,
	APPROACH_INTERCEPT_BODY_TIP = luc.PLANNER_APPROACH_INTERCEPT_BODY_TIP,
	APPROACH_RENDEZVOUS_BODY_TIP = luc.PLANNER_APPROACH_RENDEZVOUS_BODY_TIP,
	APPROACH_ORBIT = luc.PLANNER_APPROACH_ORBIT,
	APPROACH_ORBIT_TIP = luc.PLANNER_APPROACH_ORBIT_TIP,
	APPROACH_RESULT = luc.PLANNER_APPROACH_RESULT,
	APPROACH_ALTITUDE = luc.PLANNER_APPROACH_ALTITUDE,
	APPROACH_IMPACT = luc.PLANNER_APPROACH_IMPACT,
	APPROACH_SPEED = luc.PLANNER_APPROACH_SPEED,
	APPROACH_RESULT_TIP = luc.PLANNER_APPROACH_RESULT_TIP,
	CIRCULARIZE_NONE = luc.PLANNER_CIRCULARIZE_NONE,
	-- the plan's delta-v against the ship's
	DV_PLAN = luc.PLANNER_DV_PLAN,
	DV_PLAN_TIP = luc.PLANNER_DV_PLAN_TIP,
	DV_AVAILABLE = luc.PLANNER_DV_AVAILABLE,
	DV_AVAILABLE_TIP = luc.PLANNER_DV_AVAILABLE_TIP,
	DV_AFTER = luc.PLANNER_DV_AFTER,
	DV_AFTER_TIP = luc.PLANNER_DV_AFTER_TIP,
	DV_SHORT = luc.PLANNER_DV_SHORT,
	DV_SHORT_TIP = luc.PLANNER_DV_SHORT_TIP,
	REF_TARGET = luc.PLANNER_REF_TARGET_ORBIT,
	REF_EQUATOR = luc.PLANNER_REF_EQUATOR,
}

-- the snap events, in button order: short label, the name the tooltip uses,
-- and whether it is a node (measured against a reference plane)
local snapEvents = {
	{ key = "periapsis", label = luc.ORBIT_MARKER_PERIAPSIS, name = luc.PLANNER_EVENT_PERIAPSIS },
	{ key = "apoapsis", label = luc.ORBIT_MARKER_APOAPSIS, name = luc.PLANNER_EVENT_APOAPSIS },
	{ key = "ascending", label = luc.ORBIT_MARKER_ASCENDING_NODE, name = luc.PLANNER_EVENT_ASCENDING_NODE, node = true },
	{ key = "descending", label = luc.ORBIT_MARKER_DESCENDING_NODE, name = luc.PLANNER_EVENT_DESCENDING_NODE, node = true },
}

-- TransferPlannerGetCircularizeDeltaV names its apsis by key; the tooltip
-- shows the same names as the snap row
local apsisNames = {
	periapsis = luc.PLANNER_EVENT_PERIAPSIS,
	apoapsis = luc.PLANNER_EVENT_APOAPSIS,
}

-- cells render "-" when a value is unavailable
local UNAVAILABLE = "-"

-- TransferPlanner burn status values, as returned by
-- SystemView:TransferPlannerGetBurnStatus (see src/TransferPlanner.h)
local BURN_STATUS_PENDING = 0
local BURN_STATUS_ACTIVE = 1
local BURN_STATUS_COMPLETED = 2

-- the row's status label, icon and colour. The planner only stores COMPLETED;
-- ACTIVE is the planner's active burn (TransferPlannerGetActiveIndex), so the
-- sidebar derives it from the index and never caches it
local burn_status = {
	[BURN_STATUS_PENDING] = {text = L.STATUS_PENDING, icon = icons.burn_pending, color = svColor.FONT},
	[BURN_STATUS_ACTIVE] = {text = L.STATUS_ACTIVE, icon = icons.burn_active, color = svColor.PLANNER_BURN},
	[BURN_STATUS_COMPLETED] = {text = L.STATUS_COMPLETED, icon = icons.burn_completed, color = svColor.PLANNER_BURN_DONE},
}

local function statusOf(status, isActive)
	if status == BURN_STATUS_COMPLETED then
		return burn_status[BURN_STATUS_COMPLETED]
	end
	return isActive and burn_status[BURN_STATUS_ACTIVE] or burn_status[BURN_STATUS_PENDING]
end

local function formatRemainingDv(dv)
	if not dv or dv ~= dv then
		return UNAVAILABLE
	end
	local speed, unit = ui.Format.SpeedUnit(dv)
	return speed .. " " .. unit
end

--
-- Function: drawSnapRow
--
-- Four buttons that snap burn idx's node time to the next periapsis,
-- apoapsis, ascending or descending node of the orbit leading into it. Each
-- is greyed when its event does not exist there; the tooltip names when the
-- event is and, for the nodes, the plane they are measured against.
--
local function drawSnapRow(idx, width, completed)
	local spacing = ui.getItemSpacing().x
	local size = Vector2(math.floor((width - spacing * (#snapEvents - 1)) / #snapEvents), 0)

	local refName, refIsTarget = systemView:TransferPlannerGetNodeReference()
	local reference = refName and string.format(refIsTarget and L.REF_TARGET or L.REF_EQUATOR, refName) or UNAVAILABLE

	for i, event in ipairs(snapEvents) do
		if i > 1 then ui.sameLine(0, spacing) end

		local time = not completed and systemView:TransferPlannerGetSnapTime(idx, event.key) or nil
		local tooltip
		if completed then
			tooltip = L.SNAP_COMPLETED
		elseif time then
			local delay = ui.Format.Duration(math.max(0, math.floor(time - Game.time)))
			tooltip = event.node and string.format(L.SNAP_NODE_TIP, event.name, reference, delay)
				or string.format(L.SNAP_TIP, event.name, delay)
		else
			tooltip = event.node and string.format(L.SNAP_NODE_NONE, event.name, reference)
				or string.format(L.SNAP_NONE, event.name)
		end

		local variant = not time and colorset.disabled or nil
		if ui.button(event.label .. "##plannerSnap" .. event.key .. idx, size, variant, tooltip) and time then
			systemView:TransferPlannerSnap(idx, event.key)
		end
	end
end

--
-- Function: drawCircularizeButton
--
-- One full-width button that moves burn idx to the next periapsis or apoapsis
-- (keeping one it already sits on) and sets its Δv so the orbit after it is
-- circular there; the tooltip says where, when and how much Δv that takes.
--
local function drawCircularizeButton(idx, width, completed)
	local dv, time, apsis
	if not completed then
		dv, time, apsis = systemView:TransferPlannerGetCircularizeDeltaV(idx)
	end

	local tooltip
	if completed then
		tooltip = L.SNAP_COMPLETED
	elseif not dv then
		tooltip = L.CIRCULARIZE_NONE
	else
		local where = L.CIRCULARIZE_AT_POINT
		if apsis then
			local delay = ui.Format.Duration(math.max(0, math.floor(time - Game.time)))
			where = string.format(L.CIRCULARIZE_AT_APSIS, apsisNames[apsis] or apsis, delay)
		end
		tooltip = string.format(L.CIRCULARIZE_TIP, where, ui.Format.Speed(dv))
	end
	local variant = not dv and colorset.disabled or nil
	if ui.button(ui.get_icon_glyph(icons.circularize) .. " " .. L.CIRCULARIZE .. "##plannerCircularize" .. idx, Vector2(width, 0), variant, tooltip) and dv then
		systemView:TransferPlannerCircularize(idx)
	end
end

--
-- Function: drawDeltaVSummary
--
-- The Δv the burns still to fly need, the Δv the ship has, and what is left
-- after the plan - or how much it is short, in the alert colour.
--
local function drawDeltaVSummary(count)
	local planned = 0
	for idx = 0, count - 1 do
		if systemView:TransferPlannerGetBurnStatus(idx) ~= BURN_STATUS_COMPLETED then
			planned = planned + systemView:TransferPlannerGetBurnRemainingDeltaV(idx)
		end
	end
	local available = Game.player:GetRemainingDeltaV()
	local after = available - planned

	-- one label / value pair per line, values aligned in a column
	local labelWidth = math.max(ui.calcTextSize(L.DV_PLAN).x, ui.calcTextSize(L.DV_AVAILABLE).x,
		ui.calcTextSize(L.DV_AFTER).x) + ui.getItemSpacing().x * 2 + ui.getTextLineHeight()
	local function line(label, tooltip, value, color)
		local x = ui.getCursorPos().x
		textIcon(icons.deltav, tooltip)
		ui.textColored(colors.fontDim, label)
		ui.sameLine(x + labelWidth)
		ui.textColored(color or colors.font, value)
		ui.setItemTooltip(tooltip)
	end

	line(L.DV_PLAN, L.DV_PLAN_TIP, ui.Format.Speed(planned))
	line(L.DV_AVAILABLE, L.DV_AVAILABLE_TIP, ui.Format.Speed(available))
	if after >= 0 then
		line(L.DV_AFTER, L.DV_AFTER_TIP, ui.Format.Speed(after))
	else
		line(L.DV_AFTER, L.DV_SHORT_TIP, string.format(L.DV_SHORT, ui.Format.Speed(-after)), colors.alertRed)
	end
end

-- closest-approach planning: how many burns to plan (1 intercept, 2
-- rendezvous), and the last planning failure with the target it was for
local approachBurns = 1
local approachError, approachErrorTarget = nil, nil

-- How far from the target the approach ends, in metres, persisted in the game
-- settings: for a ship or station the stand-off behind it on its own orbit,
-- for a planet or moon the flyby altitude above its surface. GetConfigInt
-- answers 0 for a missing key, so the values are stored biased by 1
local DEFAULT_STAND_OFF_M = 1000
local STAND_OFF_MAX_M = 100000
local DEFAULT_FLYBY_ALTITUDE_M = 100000
local FLYBY_ALTITUDE_MAX_M = 10000000
local approachStandOff = DEFAULT_STAND_OFF_M
local approachFlybyAltitude = DEFAULT_FLYBY_ALTITUDE_M

local function loadApproachSettings()
	local stored = Game.GetConfigInt("OrbitPlanner", "approachStandOffM")
	approachStandOff = (stored > 0) and math.min(stored - 1, STAND_OFF_MAX_M) or DEFAULT_STAND_OFF_M
	stored = Game.GetConfigInt("OrbitPlanner", "approachFlybyAltitudeM")
	approachFlybyAltitude = (stored > 0) and math.min(stored - 1, FLYBY_ALTITUDE_MAX_M) or DEFAULT_FLYBY_ALTITUDE_M
end

local function saveApproachSettings()
	Game.SetConfigInt("OrbitPlanner", "approachStandOffM", approachStandOff + 1)
	Game.SetConfigInt("OrbitPlanner", "approachFlybyAltitudeM", approachFlybyAltitude + 1)
	Engine.SaveSettings()
end

loadApproachSettings()

-- the approach target: the object selected on the map when it has a physical
-- body in this system, otherwise the navigation target; with where it came from
local function approachTarget()
	local selected = systemView:GetSelectedObject()
	if selected and selected.type == Projectable.OBJECT and selected.ref then
		local body
		if selected.base == Projectable.SYSTEMBODY then
			body = selected.ref.physicsBody
		elseif selected.base == Projectable.SHIP or selected.base == Projectable.BODY then
			body = selected.ref
		end
		if body then return body, L.APPROACH_FROM_SELECTION end
	end
	local navTarget = Game.player:GetNavTarget()
	if navTarget then return navTarget, L.APPROACH_FROM_NAV end
	return nil
end

--
-- Function: drawApproachSection
--
-- The target, the intercept / rendezvous choice, the Plan button, and the
-- closest approach of the plan as it stands - which also follows burns
-- edited by hand.
--
local function drawApproachSection()
	ui.separator()
	ui.textColored(colors.fontDim, L.APPROACH_TITLE)

	local target, source = approachTarget()
	if not target then
		ui.textColored(colors.fontDim, L.APPROACH_NO_TARGET)
		return
	end
	local targetName = target:GetLabel()
	ui.text(string.format(L.APPROACH_TARGET, targetName))
	ui.setItemTooltip(source)

	-- a planet, moon or star is flown past or orbited at an altitude, a ship
	-- or station met at a stand-off behind it
	local isBody = target:isa("Planet") or target:isa("Star")
	-- the plan's closest approach; on failure the second value is the reason
	local distance, time, speed, radius = systemView:TransferPlannerGetClosestApproach(target)

	local width = ui.getContentRegion().x
	local spacing = ui.getItemSpacing().x
	local half = Vector2(math.floor((width - spacing) / 2), 0)
	if ui.button(ui.get_icon_glyph(icons.approach_intercept) .. " " .. L.APPROACH_INTERCEPT .. "##plannerApproach1", half,
		approachBurns == 1 and colorset.selected or nil, isBody and L.APPROACH_INTERCEPT_BODY_TIP or L.APPROACH_INTERCEPT_TIP) then
		approachBurns = 1
	end
	ui.sameLine(0, spacing)
	if ui.button(ui.get_icon_glyph(icons.approach_rendezvous) .. " " .. L.APPROACH_RENDEZVOUS .. "##plannerApproach2", half,
		approachBurns == 2 and colorset.selected or nil, isBody and L.APPROACH_RENDEZVOUS_BODY_TIP or L.APPROACH_RENDEZVOUS_TIP) then
		approachBurns = 2
	end

	-- the stand-off behind a ship or station, or the flyby altitude over a
	-- planet or moon, in kilometres
	local label, tip, format = L.APPROACH_STAND_OFF, L.APPROACH_STAND_OFF_TIP, L.APPROACH_STAND_OFF_FORMAT
	local icon = icons.stand_off
	local value, maxValue, step = approachStandOff, STAND_OFF_MAX_M, 0.1
	if isBody then
		label, tip, format = L.APPROACH_FLYBY, L.APPROACH_FLYBY_TIP, L.APPROACH_FLYBY_FORMAT
		icon = icons.approach_flyby
		if approachBurns == 2 then label, tip, icon = L.APPROACH_ORBIT, L.APPROACH_ORBIT_TIP, icons.approach_capture end
		value, maxValue, step = approachFlybyAltitude, FLYBY_ALTITUDE_MAX_M, 1
	end
	ui.alignTextToFramePadding()
	ui.text(ui.get_icon_glyph(icon) .. " " .. label)
	ui.setItemTooltip(tip)
	ui.sameLine()
	ui.pushItemWidth(ui.getContentRegion().x)
	local km, changed = ui.incrementDrag("##plannerApproachStandOff", value / 1000, step,
		0, maxValue / 1000, format, false)
	ui.popItemWidth()
	ui.setItemTooltip(tip)
	if changed then
		value = math.min(math.max(math.floor(km * 1000 + 0.5), 0), maxValue)
		if isBody then approachFlybyAltitude = value else approachStandOff = value end
		saveApproachSettings()
	end

	local planTip = string.format(L.APPROACH_PLAN_TIP, approachBurns == 1 and L.APPROACH_ONE_BURN or L.APPROACH_TWO_BURNS)
	if ui.button(L.APPROACH_PLAN .. "##plannerApproachPlan", Vector2(width, 0), nil, planTip) then
		local ok, err = systemView:TransferPlannerPlanApproach(target, approachBurns, value)
		approachError = not ok and err or nil
		approachErrorTarget = targetName
		-- the flight HUD can stop the clock before the closest approach to it
		if ok then ApproachTarget.Set(target) end
		-- the readout below shows the new plan
		distance, time, speed, radius = systemView:TransferPlannerGetClosestApproach(target)
	end

	if distance then
		local when = time > Game.time + 1 and ui.Format.Duration(math.floor(time - Game.time)) or lc.NOW
		if radius > 0 and distance < radius then
			ui.textColored(colors.alertRed, string.format(L.APPROACH_IMPACT, when))
		else
			local shown = radius > 0 and string.format(L.APPROACH_ALTITUDE, ui.Format.Distance(distance - radius))
				or ui.Format.Distance(distance)
			ui.text(string.format(L.APPROACH_RESULT, shown, when))
		end
		ui.setItemTooltip(L.APPROACH_RESULT_TIP)
		ui.textColored(colors.fontDim, string.format(L.APPROACH_SPEED, ui.Format.Speed(speed)))
	else
		ui.withStyleColors({ Text = colors.fontDim }, function() ui.textWrapped(time) end)
	end

	if approachError and approachErrorTarget == targetName then
		ui.withStyleColors({ Text = colors.alertYellow }, function() ui.textWrapped(approachError) end)
	end
end

-- the burn the plan's controls are currently aimed at
local selectedBurn = 0

-- keep the selection inside the plan whenever the number of burns changes
local function clampSelectedBurn(count)
	local maxIdx = math.max(0, count - 1)
	if selectedBurn > maxIdx then
		selectedBurn = maxIdx
	end
	if selectedBurn < 0 then
		selectedBurn = 0
	end
end

---@type UI.Sidebar.Module
local plannerView = {
	icon = icons.semi_major_axis,
	tooltip = lc.ORBIT_PLANNER,
	title = lc.ORBIT_PLANNER,
	disabled = false,

	drawBody = function(self)
		-- the factor is global, so it stays a single index-less row above the table
		showDvLine(nil, "factor", icons.decrease, icons.delta, icons.increase, function(i) return i, "x" end, luc.DECREASE, lc.PLANNER_RESET_FACTOR, luc.INCREASE)

		local count = systemView:TransferPlannerGetBurnCount()
		clampSelectedBurn(count)

		if count > 0 then
			-- the controls of a single burn: its start time and its three Δv axes
			local burnRows = {
				{key = "starttime", icons = {icons.decrease, icons.clock, icons.increase},
					resetTooltip = lc.PLANNER_RESET_START,
					formatter = function(start)
						if start and start > Game.time then
							return ui.Format.Duration(math.floor(start - Game.time)), ""
						else
							return lc.NOW, ""
						end
					end},
				{key = "prograde", icons = {icons.decrease, icons.orbit_prograde, icons.increase},
					resetTooltip = lc.PLANNER_RESET_PROGRADE, formatter = ui.Format.SpeedUnit},
				{key = "normal", icons = {icons.decrease, icons.orbit_normal, icons.increase},
					resetTooltip = lc.PLANNER_RESET_NORMAL, formatter = ui.Format.SpeedUnit},
				{key = "radial", icons = {icons.decrease, icons.orbit_radial, icons.increase},
					resetTooltip = lc.PLANNER_RESET_RADIAL, formatter = ui.Format.SpeedUnit},
			}

			-- the table places the controls of a burn on the left and its summary on the
			-- right, so every string is built and measured before the first row is drawn
			local plan = {}
			-- Stable geometry comes from the sidebar's content region and fixed
			-- display envelopes, never from the values changing every frame.
			local durationSample = ui.Format.Duration(
				6 * 24 * 60 * 60 + 23 * 60 * 60 + 59 * 60)
			local valueWidth = math.max(
				ui.calcTextSize(ui.Format.Speed(-999999)).x,
				ui.calcTextSize(durationSample).x,
				ui.calcTextSize(lc.NOW).x)

			-- the row descriptors are drawn in this window's ambient font, so the
			-- measurement below runs in the same context (no font is pushed)
			local spacing = ui.getItemSpacing().x
			-- the active burn comes from the planner every frame, so the sidebar, the
			-- flight HUD and the reticle can never disagree about it
			local activeIndex = systemView:TransferPlannerGetActiveIndex()

			for idx = 0, count - 1 do
				local prograde = systemView:TransferPlannerGet(idx, "prograde")
				local normal = systemView:TransferPlannerGet(idx, "normal")
				local radial = systemView:TransferPlannerGet(idx, "radial")
				for _, burnRow in ipairs(burnRows) do
					local speed, unit = burnRow.formatter(systemView:TransferPlannerGet(idx, burnRow.key))
					valueWidth = math.max(valueWidth, ui.calcTextSize(speed .. " " .. unit).x)
				end
				-- total Δv magnitude of the burn, the offset of its start time, its
				-- remaining Δv and its status label
				local magnitude = ui.Format.Speed(math.sqrt(prograde * prograde + normal * normal + radial * radial))
				local start = systemView:TransferPlannerGet(idx, "starttime")
				local offset = (start and start > Game.time) and ui.Format.Duration(math.floor(start - Game.time)) or lc.NOW
				local status = systemView:TransferPlannerGetBurnStatus(idx)
				local statusInfo = statusOf(status, idx == activeIndex)
				local remaining = formatRemainingDv(systemView:TransferPlannerGetBurnRemainingDeltaV(idx))
				-- only a completed row has no Complete control, so the widest status
				-- line is one that carries both buttons
				local completable = status ~= BURN_STATUS_COMPLETED
				plan[idx + 1] = {
					magnitude = magnitude, offset = offset, remaining = remaining,
					status = statusInfo, completable = completable,
				}

			end

			local contentWidth = ui.getContentRegion().x
			local controlsBudget = math.floor(contentWidth * 0.46)
			-- Three stable square controls plus a fixed value slot.
			local buttonSize = math.floor((controlsBudget - valueWidth - spacing * 3) / 3)
			buttonSize = math.min(math.max(buttonSize, ui.rescaleUI(14)), styles.MainButtonSize.x)
			local controlsWidth = buttonSize * 3 + spacing * 3 + valueWidth
			local summaryWidth = math.max(contentWidth - controlsWidth, 1)

			ui.columns(2, "##plannerBurns", false)
			ui.setColumnWidth(0, controlsWidth)
			ui.setColumnWidth(1, summaryWidth)

			for idx = 0, count - 1 do
				-- deleting a burn shifts every later index, so stop at the live count
				if idx >= systemView:TransferPlannerGetBurnCount() then break end
				local row = plan[idx + 1]

				-- clicking the header selects this burn, the highlight marks the
				-- selection; the map time jumps to the burn and holds there (the
				-- game clock is untouched)
				if ui.selectable(string.format(L.BURN, idx + 1) .. "##plannerBurnHeader" .. idx, idx == selectedBurn, {"SpanAllColumns"}) then
					selectedBurn = idx
					local burnTime = systemView:TransferPlannerGet(idx, "starttime")
					if burnTime then
						systemView:SetMapTime(burnTime)
						-- off "now", so the map is not pulled back to the present
						time_selected_button_icon = nil
					end
				end

				for _, burnRow in ipairs(burnRows) do
					showDvLine(idx, burnRow.key, burnRow.icons[1], burnRow.icons[2], burnRow.icons[3],
						burnRow.formatter, luc.DECREASE, burnRow.resetTooltip, luc.INCREASE, buttonSize)
				end
				drawSnapRow(idx, ui.getContentRegion().x, not row.completable)
				drawCircularizeButton(idx, ui.getContentRegion().x, not row.completable)

				ui.nextColumn()

				-- One summary item per line keeps every icon and value at a stable x
				-- while leaving the full summary-column width available to its text.
				textIcon(icons.deltav, luc.DELTA_V)
				ui.text(row.magnitude)
				textIcon(icons.clock, lc.TIME_POINT)
				ui.text(row.offset)

				-- The planner stores COMPLETED; ACTIVE is
				-- the planner's active burn, taken from TransferPlannerGetActiveIndex
				-- above, so this row and the flight HUD cannot diverge
				textIcon(row.status.icon, row.status.text)
				ui.withStyleColors({Text = row.status.color}, function()
					ui.text(row.status.text)
				end)
				textIcon(icons.delta, L.REMAINING_DV)
				ui.text(row.remaining)

				-- line 3: the row's actions. Complete is offered for the active and
				-- the pending burns only - a completed row shows its status alone.
				-- ui.inlineIconButton is the form this column drew before, and the one
				-- the flight HUD's action controls use: it hands ui.iconButton
				-- Vector2(0, ui.getLineHeight()) with the inline padding, so the button
				-- box is derived from the measured glyph (Draw::GlyphButton sizes the
				-- item from glyph size + FramePadding when no size is given) and the
				-- glyph is rasterised from the same line context the box is sized from
				-- (libs/buttons.lua). Passing an explicit size instead fixes the box
				-- from the caller's text line height while the glyph is drawn at that
				-- height less the inline padding, which is not a size this window
				-- rasterises its icon font at.
				if row.completable then
					if ui.inlineIconButton("##plannerBurnComplete" .. idx, icons.burn_completed, L.COMPLETE) then
						systemView:TransferPlannerCompleteBurn(idx)
					end
					ui.sameLine()
				end
				if ui.inlineIconButton("##plannerBurnDelete" .. idx, icons.cross, luc.DELETE) then
					systemView:TransferPlannerRemoveBurn(idx)
					clampSelectedBurn(systemView:TransferPlannerGetBurnCount())
				end

				ui.nextColumn()
			end

			-- reset columns
			ui.columns(1)
		end

		-- footer: insert a burn after the selected one, then select the new burn
		if ui.button(ui.get_icon_glyph(icons.burn_add) .. " " .. L.ADD_BURN .. "##plannerAddBurn") then
			selectedBurn = systemView:TransferPlannerInsertBurn(selectedBurn)
			clampSelectedBurn(systemView:TransferPlannerGetBurnCount())
		end

		ui.separator()
		drawDeltaVSummary(systemView:TransferPlannerGetBurnCount())

		drawApproachSection()
	end
}

table.insert(leftSidebar.modules, infoView)
table.insert(leftSidebar.modules, economyView)
table.insert(leftSidebar.modules, settingsView)

table.insert(rightSidebar.modules, overviewView)
table.insert(rightSidebar.modules, plannerView)

--
-- Window functions
--

function Windows.edgeButtons.ShouldShow()
	return true
end

function Windows.timeButtons.ShouldShow()
	return systemView:GetDisplayMode() == 'Orrery'
end

function Windows.edgeButtons.Show()
	local isCurrent = systemView:GetSystemSelectionMode() == "CURRENT_SYSTEM"
	local isOrrery = systemView:GetDisplayMode() == "Orrery"

	ui.horizontalGroup(function()
		-- system selection button (current/selected)
		if not isCurrent and ui.mainMenuButton(icons.planet_grid, luc.HUD_BUTTON_SWITCH_TO_CURRENT_SYSTEM) then
			systemView:SetSystemSelectionMode("CURRENT_SYSTEM")
		end
		if isCurrent and ui.mainMenuButton(icons.galaxy_map, luc.HUD_BUTTON_SWITCH_TO_SELECTED_SYSTEM) then
			systemView:SetSystemSelectionMode("SELECTED_SYSTEM")
		end

		-- view control buttons (reset, rotate, zoom)
		if ui.mainMenuButton(icons.reset_view, luc.RESET_ORIENTATION_AND_ZOOM) then
			systemView:SetVisibility("RESET_VIEW")
		end
		if isOrrery then
			ui.mainMenuButton(icons.rotate_view, luc.ROTATE_VIEW)
		else
			ui.mainMenuButton(icons.distance, luc.TRANSLATE_VIEW)
		end
		systemView:SetRotateMode(ui.isItemActive())
		ui.mainMenuButton(icons.search_lens, luc.ZOOM)
		systemView:SetZoomMode(ui.isItemActive())
		-- the ship is on the map whenever it shows the system the player is
		-- in, also when that system is reached through "selected system"
		if isOrrery and systemView:IsViewingCurrentSystem() and ui.mainMenuButton(icons.zoom_to_ship, L.ZOOM_TO_SHIP) then
			systemView:ViewPlayer()
		end
		-- follow where the plan puts the ship as the map time is scrubbed;
		-- highlighted while it does
		if isOrrery and systemView:IsViewingCurrentSystem() and systemView:TransferPlannerGetBurnCount() > 0 then
			if ui.mainMenuButton(icons.follow_plan, L.FOLLOW_PLAN .. "##followPlannedShip", systemView:IsViewingPlannedShip()) then
				systemView:ViewPlannedShip()
			end
		end

		-- view settings buttons
		if isOrrery then
			ui.spacing()
			if ui.mainMenuButton(icons.ships_no_orbits, lc.SHIPS_DISPLAY_MODE_TOGGLE, buttonState[shipDisplayMode.doDisplay].state) then
				shipDisplayMode:toggleDisplay()
			end
			if ui.mainMenuButton(icons.hyperspace, lc.HYPERSPACE_CLOUDS_DISPLAY_MODE_TOGGLE, buttonState[cloudDisplayMode.doDisplay].state) then
				cloudDisplayMode:toggleDisplay()
			end
			if ui.mainMenuButton(icons.planet_grid, LATLONG_DISPLAY_MODE, buttonState[latLongDisplayMode.doDisplay].state) then
				latLongDisplayMode:toggleDisplay()
			end
		end
	end)
end

-- time conntrol buttons
function Windows.timeButtons.Show()
	local t = systemView:GetOrbitPlannerTime()
	ui.text(t and ui.Format.Datetime(t) or lc.NOW)
	local r = false
	r = timeButton(icons.time_backward_100x, "-10,000,000x",-10000000) or r
	r = timeButton(icons.time_backward_10x, "-100,000x", -100000) or r
	r = timeButton(icons.time_backward_1x, "-1,000x", -1000) or r
	r = timeButton(icons.time_center, lc.NOW, nil) or r
	r = timeButton(icons.time_forward_1x, "1,000x", 1000) or r
	r = timeButton(icons.time_forward_10x, "100,000x", 100000) or r
	r = timeButton(icons.time_forward_100x, "10,000,000x", 10000000) or r
	if not r then
		if time_selected_button_icon == icons.time_center then
			systemView:AccelerateTime(nil)
		else
			systemView:AccelerateTime(0.0)
		end
	end
end

-- orbit markers labelled on the player's and the planner's orbits, which
-- carry the marker's altitude (other orbits' apsides stay icon-only)
local markerLabels = {
	[Projectable.PERIAPSIS] = luc.ORBIT_MARKER_PERIAPSIS,
	[Projectable.APOAPSIS] = luc.ORBIT_MARKER_APOAPSIS,
	[Projectable.ASCENDING_NODE] = luc.ORBIT_MARKER_ASCENDING_NODE,
	[Projectable.DESCENDING_NODE] = luc.ORBIT_MARKER_DESCENDING_NODE,
}

local function getLabel(obj)
	if markerLabels[obj.type] and obj.altitude then
		return markerLabels[obj.type] .. " " .. ui.Format.Distance(obj.altitude)
	end
	if obj.type == Projectable.IMPACT then
		return string.format(L.IMPACT_IN, ui.Format.Duration(math.max((obj.time or 0) - Game.time, 0), 2))
	end
	if obj.type == Projectable.OBJECT then
		if obj.base == Projectable.SYSTEMBODY then return obj.ref.name
		elseif obj.base == Projectable.PLANNER then return ""
		elseif obj.base == Projectable.PLANNER_BURN then return string.format(L.BURN, obj.burnIndex or 1)
		else return obj.ref:GetLabel() end
	--elseif obj.type == Projectable.L4 and show_lagrange == "LAG_ICONTEXT" then return "L4"
	elseif obj.type == Projectable.L4 and l4l5DisplayMode:getMode() == "LAG_ICONTEXT" then return "L4"
	--elseif obj.type == Projectable.L4 and show_lagrange == "LAG_ICONTEXT" then return "L4"
	elseif obj.type == Projectable.L5 and l4l5DisplayMode:getMode() == "LAG_ICONTEXT" then return "L5"
	else return ""
	end
end

local function getColor(obj)
	if obj.type == Projectable.OBJECT then
		if obj.base == Projectable.SYSTEMBODY then return svColor.SYSTEMBODY_ICON
		elseif obj.base == Projectable.SHIP then return svColor.SHIP
		elseif obj.base == Projectable.PLAYER then return svColor.PLAYER
		elseif obj.base == Projectable.PLANNER then return svColor.PLANNER
		elseif obj.base == Projectable.PLANNER_BURN then return svColor.PLANNER_BURN
		else return svColor.OBJECT
		end
	elseif markerLabels[obj.type] or obj.type == Projectable.IMPACT then
		if obj.base == Projectable.SYSTEMBODY then return svColor.SYSTEMBODY_ORBIT
		elseif obj.base == Projectable.SHIP then
			if obj.ref == selectedObject then return svColor.SELECTED_SHIP_ORBIT
			else return svColor.SHIP_ORBIT
			end
		elseif obj.base == Projectable.PLAYER then return svColor.PLAYER_ORBIT
		elseif obj.base == Projectable.PLANNER then return svColor.PLANNER_ORBIT
		else return svColor.UNKNOWN -- unknown base
		end
	elseif obj.type == Projectable.L4 or obj.type == Projectable.L5 then return svColor.LAGRANGE
	else return svColor.UNKNOWN
	end
end

function Windows.systemName.Show()
	local path
	if systemView:GetSystemSelectionMode() == "SELECTED_SYSTEM" then
		path = Game.sectorView:GetSelectedSystemPath()
	else
		path = systemView:GetSystem().path
	end
	ui.text(ui.Format.SystemPath(path))
end

local function drawGroupIcons(coords, icon, color, iconSize, group, isSelected)
	-- indicators
	local stackedSize = indicatorSize
	local stackStep = Vector2(10, 10)
	if isSelected then
		ui.addIcon(coords, icons.square, svColor.SELECTED, stackedSize, ui.anchor.center, ui.anchor.center)
		stackedSize = stackedSize + stackStep
	end
	if group.hasPlayer then
		ui.addIcon(coords, icons.square, svColor.PLAYER, stackedSize, ui.anchor.center, ui.anchor.center)
		stackedSize = stackedSize + stackStep
	end
	if group.hasNavTarget then
		ui.addIcon(coords, icons.square, svColor.NAV_TARGET, stackedSize, ui.anchor.center, ui.anchor.center)
		stackedSize = stackedSize + stackStep
	end
	if group.hasCombatTarget then
		ui.addIcon(coords, icons.square, svColor.COMBAT_TARGET, stackedSize, ui.anchor.center, ui.anchor.center)
		stackedSize = stackedSize + stackStep
	end
	if group.hasPlanner then
		ui.addIcon(coords, icons.square, svColor.PLANNER, stackedSize, ui.anchor.center, ui.anchor.center)
		stackedSize = stackedSize + stackStep
	end

	ui.addIcon(coords, icon, color, iconSize, ui.anchor.center, ui.anchor.center)
end

-- handle positioning and drawing a label for the given object in Atlas mode
local function drawAtlasBodyLabel(label, screenSize, mainCoords, isHovered, isSelected)
	-- Larger font for hovered bodies, slight emphasis on the selected body
	local font = isHovered and atlasfont_highlight or atlasfont
	local fontColor = isSelected and colors.systemAtlasLabelActive or colors.systemAtlasLabel
	local lineColor = isSelected and colors.systemAtlasLineActive or colors.systemAtlasLine

	local textSize = ui.calcTextSize(label, font)
	-- lineOffset is half the screen-size radius of the body
	local lineOffsetSize = math.max(screenSize * 0.66, bodyIconSize.x * 0.5) -- most icons use about 60% of the actual radius
	-- lineLength is how long to draw the "pointer" line between the label and the edge of the body
	local lineLength = (atlas_line_length / math.max(systemView:GetZoom(), 1.0)) * (isHovered and 1.0 or 0.6)

	local lineStartPos = mainCoords + Vector2(lineOffsetSize, -lineOffsetSize * 0.667)
	local lineEndPos = lineStartPos + Vector2(lineLength, -lineLength)
	local underlinePos = lineEndPos + Vector2(textSize.x + atlas_label_offset.x * 2, 0)

	-- draw a background behind the label, then an indicator line
	if isHovered then
		ui.addRectFilled(lineEndPos - Vector2(0, -atlas_label_offset.y + textSize.y), underlinePos, colors.lightBlackBackground, 4, ui.RoundCornersNone)
		ui.addLine(lineStartPos, lineEndPos, lineColor, 2)
		ui.addLine(lineEndPos, underlinePos, lineColor, 3)
	end

	-- draw the label and it's shadow for clarity
	local labelPos = (isHovered and lineEndPos or lineStartPos) + atlas_label_offset
	local shadowPos = labelPos + Vector2(2, 1)

	ui.addStyledText(shadowPos, ui.anchor.left, ui.anchor.baseline, label, colors.black, font)
	ui.addStyledText(labelPos, ui.anchor.left, ui.anchor.baseline, label, fontColor, font)
end

Windows.unexplored.visible = false
function Windows.unexplored.Show()
	ui.text(lc.UNEXPLORED_SYSTEM_NO_SYSTEM_VIEW)
end

local function checkSurfPorts(sbody)
	local children = sbody.children
	if #children == 0 then return nil end
	local ports = {}
	local has_indicators
	local navTarget = Game.player:GetNavTarget()
	local selected = systemView:GetSelectedObject().ref
	for _, child in ipairs(children) do
		if child.type == "STARPORT_SURFACE" then
			table.insert(ports, child)
			local body = child.physicsBody
			has_indicators = has_indicators or body and body == navTarget or child == selected
		end
	end
	if #ports == 0 then return nil end
	return ports, has_indicators
end

local function displaySurfPorts(center, radius, ports, mouseover)
	local nports = #ports
	if nports == 0 then return nil end
	local icon_size = bodyIconSize.x
	-- pretty rough estimate
	local min_rad = icon_size * nports / ui.twoPi
	local pixPerUnit = systemView:AtlasViewPixelPerUnit()
	local gap = systemView:AtlasViewPlanetGap(radius / pixPerUnit) * pixPerUnit
	local icons_radius = math.max(icon_size, min_rad, radius + icon_size / 2)
	local can_show_ring = icons_radius + icon_size / 2 < radius + gap or nports < 3

	local start_ang = math.deg2rad(225)
	local step_ang = ui.twoPi / nports
	local hover_i = 0
	local hover_center
	local select_i = 0
	local select_center
	local navtarget_in_ring = false
	for i = 1, nports do
		local ang = start_ang + step_ang * (i - 1)
		local p_center = center + Vector2(math.cos(ang), math.sin(ang)) * icons_radius
		-- HACK we pretend to be a group, to use drawGroupIcons
		local body = ports[i].physicsBody
		local group = { hasNavTarget = body and body == Game.player:GetNavTarget() }
		local isSelected = ports[i] == systemView:GetSelectedObject().ref
		if can_show_ring then
			drawGroupIcons(p_center, _getBodyIcon(ports[i]), svColor.SYSTEMBODY_ICON, bodyIconSize, group, isSelected)
			-- hover mouse
			if (ui.getMousePos() - p_center):length() < icon_size / 2 then
				hover_i = i
				hover_center = p_center
			end
		else
			navtarget_in_ring = navtarget_in_ring or group.hasNavTarget
		end
		if isSelected then
			select_i = i
			select_center = p_center
		end
	end
	if not can_show_ring then
		local p_center = center + Vector2(math.cos(start_ang), math.sin(start_ang)) * icons_radius
		local group = { hasNavTarget = navtarget_in_ring }
		drawGroupIcons(p_center, icons.plus, svColor.SYSTEMBODY_ICON, bodyIconSize, group, select_i ~= 0)
		return nil
	end
	if hover_i ~= 0 then
		return ports[hover_i], hover_center
	elseif select_i ~= 0 and not mouseover then
		return ports[select_i], select_center
	else
		return nil
	end
end

local popup_object
function makePopup()
	ui.popup("system-view-ui-popup", function()
		local isOrrery = systemView:GetDisplayMode() == "Orrery"
		local combatTarget = player:GetCombatTarget()
		local navTarget = Game.player:GetNavTarget()
		local isObject = popup_object.type == Projectable.OBJECT
		local isSystemBody = isObject and popup_object.base == Projectable.SYSTEMBODY
		local isShip = isObject and not isSystemBody and popup_object.ref:IsShip()
		local isCloud = isObject and not isSystemBody and popup_object.ref:IsHyperspaceCloud()
		ui.text(getLabel(popup_object))
		ui.separator()
		if isOrrery and ui.selectable(lc.CENTER, false, {}) then
			selectedObject = popup_object.ref
			systemView:SetSelectedObject(popup_object.type, popup_object.base, popup_object.ref)
			systemView:ViewSelectedObject()
		end
		if (isShip or isCloud or isSystemBody and popup_object.ref.physicsBody) and ui.selectable(lc.SET_AS_TARGET, false, {}) then
			if isSystemBody then
				player:SetNavTarget(popup_object.ref.physicsBody)
				ui.playSfx("OK")
			elseif isCloud then
				player:SetNavTarget(popup_object.ref)
				ui.playSfx("OK")
			else
				if combatTarget == popup_object.ref then player:SetCombatTarget(nil) end
				player:SetNavTarget(popup_object.ref)
				ui.playSfx("OK")
			end
		end
		if isShip and ui.selectable(lc.SET_AS_COMBAT_TARGET, false, {}) then
			if navTarget == popup_object.ref then player:SetNavTarget(nil) end
			player:SetCombatTarget(popup_object.ref)
		end
	end)
end

local expanded_hover_body = nil -- body with expanded hover area (atlas view only)
-- forked from data/pigui/views/game.lua
local function displayOnScreenObjects()
	local isOrrery = systemView:GetDisplayMode() == 'Orrery'

	Windows.unexplored.visible = not systemView:GetSystem().explored
	if Windows.unexplored.visible then return end

	local should_show_label = isOrrery and ui.shouldShowLabels()

	local label_offset = 14 -- enough so that the target rectangle fits
	local collapse = bodyIconSize -- size of clusters to be collapsed into single bodies
	local click_radius = collapse:length() * 0.5
	if not isOrrery then
		click_radius = collapse:length() * 0.8 / systemView:GetZoom()
	end
	-- make click_radius sufficiently smaller than the cluster size
	-- to prevent overlap of selection regions
	local objectCounter = 0
	local objects_grouped = systemView:GetProjectedGrouped(collapse, 1e64)

	local hoveredObject = nil
	local was_hovered = false
	local atlas_label_objects = {}

	for _,group in ipairs(objects_grouped) do
		local mainObject = group.mainObject

		local mainCoords = Vector2(group.screenCoordinates.x, group.screenCoordinates.y)
		local isSelected = mainObject.type == Projectable.OBJECT and mainObject.ref == systemView:GetSelectedObject().ref
		group.hasPlanner = mainObject.type == Projectable.OBJECT and mainObject.base == Projectable.PLANNER
		local hoverSize = group.screenSize

		local ports = {} -- ground ports
		local should_show_ports = false -- if we want to show ports even if we are not hovered
		if not isOrrery and mainObject.type == Projectable.OBJECT and mainObject.base == Projectable.SYSTEMBODY then
			-- should show ports if there is some indicators
			ports, should_show_ports = checkSurfPorts(mainObject.ref)
			-- or if the central body is selected
			should_show_ports = should_show_ports or isSelected
			-- if we hover over a body with ports, we expand its hover radius to make it easier to hover ports
			if mainObject.ref == expanded_hover_body or should_show_ports then hoverSize = group.screenSize + bodyIconSize.x end
		end

		drawGroupIcons(mainCoords, getBodyIcon(mainObject, true, isOrrery), getColor(mainObject), bodyIconSize, group, isSelected)

		local mp = ui.getMousePos()
		local label = getLabel(mainObject)
		local mouseover = ui.isWindowHovered() and
			(mp - mainCoords):length() < (isOrrery and click_radius or math.max(click_radius, hoverSize))

		if #label > 0 and (should_show_label or mouseover or should_show_ports) then
			if group.objects then
				label = label .. " (" .. #group.objects .. ")"
			end

			if isOrrery then
				local pos = mainCoords + Vector2(label_offset, 0)
				local hovered = mouseover and mainObject.type == Projectable.OBJECT
				local font = (hovered or isSelected) and hudfont_highlight or hudfont
				-- the impact warning reads in red; its icon keeps its orbit's colour
				local labelColor = mainObject.type == Projectable.IMPACT and svColor.IMPACT_LABEL or getColor(mainObject)
				ui.addStyledText(pos + Vector2(2, 1), ui.anchor.left, ui.anchor.center, label , ui.theme.colors.black, font)
				ui.addStyledText(pos, ui.anchor.left, ui.anchor.center, label , labelColor, font)
			else
				local labeled = nil -- hovered or selected
				local labeled_center = nil
				if ports then
					if mouseover then
						was_hovered = true
						expanded_hover_body = mainObject.ref
					end
					labeled, labeled_center = displaySurfPorts(mainCoords, group.screenSize, ports, mouseover)
				end
				if labeled then
					-- HACK we just replace the current object with its selected or hovered ground port (if has)
					mainObject.type = Projectable.OBJECT
					mainObject.base = Projectable.SYSTEMBODY
					mainObject.ref = labeled
					mainCoords = labeled_center
					group.screenSize = bodyIconSize.x
					isSelected = labeled == systemView:GetSelectedObject().ref
					label = labeled.name
				end
				table.insert(atlas_label_objects, { label, group.screenSize, mainCoords, mouseover, isSelected })
			end
		end

		if mainObject.type == Projectable.OBJECT and (mainObject.base == Projectable.SYSTEMBODY or mainObject.base == Projectable.SHIP or mainObject.base == Projectable.PLAYER or mainObject.base == Projectable.OBJECT) then
			-- mouse release handler for right button
			if mouseover then
				if ui.isWindowHovered() and ui.isMouseReleased(1) then
					popup_object = mainObject
					ui.openPopup("system-view-ui-popup")
				end
			end
		end
		-- mouse release handler for left button
		if mouseover and mainObject.type == Projectable.OBJECT then
			hoveredObject = mainObject
		end
		objectCounter = objectCounter + 1
	end
	makePopup()
	if not was_hovered then expanded_hover_body = nil end

	-- atlas body labels have to be drawn after icons for proper ordering
	for _, v in ipairs(atlas_label_objects) do
		drawAtlasBodyLabel(table.unpack(v))
	end

	-- click once: select or deselect a body
	-- double click: zoom to body or reset viewpoint
	local clicked = ui.isWindowHovered() and (ui.isMouseClicked(0) or ui.isMouseDoubleClicked(0))
	if clicked then
		if hoveredObject then
			selectedObject = hoveredObject.ref
			systemView:SetSelectedObject(hoveredObject.type, hoveredObject.base, hoveredObject.ref)
			if ui.isMouseDoubleClicked(0) then systemView:ViewSelectedObject() end
		else
			selectedObject = nil
			systemView:ClearSelectedObject()
			if ui.isMouseDoubleClicked(0) then systemView:ResetViewpoint() end
		end
	end
end

function systemViewLayout:onUpdateWindowPivots(w)
	w.systemName.anchors   = { ui.anchor.center, ui.anchor.top }
	w.edgeButtons.anchors  = { ui.anchor.center,  ui.anchor.bottom }
	w.timeButtons.anchors  = { ui.anchor.right,  ui.anchor.bottom }
	w.unexplored.anchors   = { ui.anchor.center, ui.anchor.center }
end

function systemViewLayout:onUpdateWindowConstraints(w)
	-- resizing, aligning windows - static
	w.systemName.pos.x = ui.screenWidth * 0.5
	w.systemName.pos.y = styles.MainButtonSize.y + styles.WindowPadding.y * 2 -- matches fx-window.lua
	w.systemName.size.x = 0 -- adaptive width

	w.edgeButtons.size.x = 0 -- adaptive width
end

local systemViewContents = ui.makeFullScreenHandler("SystemView", displayOnScreenObjects)

local function displaySystemViewUI(delta_t, refresh)
	if not systemView then onGameStart() end

	if refresh then
		leftSidebar:Refresh()
		rightSidebar:Refresh()

		if not leftSidebar.active and not infoView.active then
			leftSidebar:MakeActive(infoView)
		end
	end

	if not ui.shouldDrawUI() then return end

	player = Game.player

	if ui.isKeyReleased(ui.keys.tab) then
		systemViewLayout.enabled = not systemViewLayout.enabled
	end

	-- the planner only works on the orrery, so the 2D atlas has no button for it
	plannerView.disabled = systemView:GetDisplayMode() ~= "Orrery"
	plannerView.hidden = plannerView.disabled

	systemViewLayout:display()
	if systemViewLayout.enabled then
		ui.withStyleColors({ WindowBg = colors.transparent }, function()
			leftSidebar:Draw()
			rightSidebar:Draw()
		end)
	end

	systemViewContents()

	if ui.escapeKeyReleased() then
		Game.SetView("SectorView")
	end

	if ui.ctrlHeld() and ui.isKeyReleased(ui.keys.delete) then
		package.reimport 'pigui.libs.sidebar'
		package.reimport 'pigui.modules.system-overview-window'
		systemEconView = package.reimport('pigui.modules.system-econ-view').New()
		package.reimport()
	end
end

Event.Register("onGameStart", onGameStart)
Event.Register("onGameEnd", onGameEnd)
Event.Register("onEnterSystem", onEnterSystem)
ui.registerHandler("SystemView", displaySystemViewUI)

return {}
