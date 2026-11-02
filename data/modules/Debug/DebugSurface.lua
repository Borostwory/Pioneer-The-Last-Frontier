-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Debug tab to put the player's ship on any planet or moon of the current
-- system: landed, or a given height above the ground, at any latitude and
-- longitude, for looking at terrain.

local Game   = require 'Game'
local Event  = require 'Event'
local Engine = require 'Engine'
local Dev    = require 'Dev'

local ui = require 'pigui'

local Notification = require 'pigui.libs.notification'
local debugView    = require 'pigui.views.debug'

local icons = ui.theme.icons
local colors = ui.theme.colors

--=============================================================================

-- heights to pick from, km
local presets = { 0.03, 0.1, 0.3, 1, 3, 10, 30, 100 }

local state = {
	system = nil,   ---@type SystemPath?
	bodies = {},    -- { sbody = SystemBody, depth = integer }
	bodyIdx = nil,

	latitude = 0.0,  -- degrees
	longitude = 0.0, -- degrees
	landed = true,
	altitude = 1.0,  -- km
}

--=============================================================================

local function canLand(sbody)
	return sbody.superType == "ROCKY_PLANET"
end

-- the planets and moons of the current system, nested under their parents
local function refreshBodies()
	state.system = Game.system.path
	state.bodies = {}
	state.bodyIdx = nil

	local frameBody = Game.player.frameBody
	for _, bodyPath in ipairs(Game.system:GetBodyPaths()) do
		local sbody = bodyPath:GetSystemBody()
		if sbody.superType == "ROCKY_PLANET" or sbody.superType == "GAS_GIANT" then
			local depth = 0
			local parent = sbody.parent
			while parent do
				if parent.type ~= "GRAVPOINT" then depth = depth + 1 end
				parent = parent.parent
			end
			table.insert(state.bodies, { sbody = sbody, depth = depth })

			-- the one the ship is near, else the first that can be landed on
			if frameBody and sbody.body == frameBody then
				state.bodyIdx = #state.bodies
			end
		end
	end

	if not state.bodyIdx then
		for i, entry in ipairs(state.bodies) do
			if canLand(entry.sbody) then
				state.bodyIdx = i
				break
			end
		end
	end
end

local function selectedBody()
	return state.bodyIdx and state.bodies[state.bodyIdx]
end

-- the ship's own latitude and longitude, where it is near the selected body
local function useCurrentPosition()
	local entry = selectedBody()
	if not entry or Game.player.frameBody ~= entry.sbody.body then
		Notification.add(Notification.Type.Info, "Debug surface: the ship is not near the selected body")
		return
	end
	local lat, lon = Game.player:GetGroundPosition()
	if lat then
		state.latitude = math.rad2deg(lat)
		state.longitude = math.rad2deg(lon)
	end
end

local function randomPosition()
	-- uniform over the sphere
	state.latitude = math.rad2deg(math.asin(Engine.rand:Number(-1, 1)))
	state.longitude = Engine.rand:Number(-180, 180)
end

local function go()
	local entry = selectedBody()
	if not entry then return end

	local body = entry.sbody.body
	if not body then
		Notification.add(Notification.Type.Error, "Debug surface: {} is not in this system" % { entry.sbody.name })
		return
	end

	local altitude = (state.landed and canLand(entry.sbody)) and 0 or state.altitude * 1000
	local ok, err = pcall(Dev.PlaceOnSurface, Game.player, body,
		math.deg2rad(state.latitude), math.deg2rad(state.longitude), altitude)
	if not ok then
		Notification.add(Notification.Type.Error, "Debug surface failed", tostring(err))
	end
end

--=============================================================================

local function drawBodyList()
	ui.child("surfaceBodyList", Vector2(0, ui.getTextLineHeightWithSpacing() * 10), function()
		for i, entry in ipairs(state.bodies) do
			local sbody = entry.sbody
			local label = string.format("%s%s  [%s, %.0f km]##surfaceBody%d",
				string.rep("    ", entry.depth), sbody.name, sbody.type:lower(), sbody.radius / 1000, i)
			if ui.selectable(label, i == state.bodyIdx) then
				state.bodyIdx = i
			end
		end
	end)
end

local function drawPosition()
	local width = ui.getContentRegion().x * 0.5

	ui.nextItemWidth(width)
	state.latitude = ui.dragFloat("Latitude (deg)", state.latitude, 0.1, -90, 90, "%.4f")
	ui.nextItemWidth(width)
	state.longitude = ui.dragFloat("Longitude (deg)", state.longitude, 0.1, -180, 180, "%.4f")

	if ui.button("Random") then
		randomPosition()
	end
	ui.sameLine()
	if ui.button("Ship's position") then
		useCurrentPosition()
	end
	ui.sameLine()
	if ui.button("Equator, 0") then
		state.latitude, state.longitude = 0.0, 0.0
	end

	ui.textColored(colors.fontDim, "Ctrl+click a field to type a value.")
end

local function drawAltitude(entry)
	if canLand(entry.sbody) then
		local changed, value = ui.checkbox("Landed", state.landed)
		if changed then state.landed = value end
	else
		ui.textColored(colors.fontDim, "No surface to land on: above the cloud tops only")
	end

	if state.landed and canLand(entry.sbody) then return end

	ui.nextItemWidth(ui.getContentRegion().x * 0.5)
	state.altitude = ui.dragFloat("Height above ground (km)", state.altitude, 0.01, 0.005, 10000, "%.3f")

	for i, km in ipairs(presets) do
		if i > 1 then ui.sameLine() end
		local label = km < 1 and "{} m" % { math.floor(km * 1000 + 0.5) } or "{} km" % { km }
		if ui.button("{}##preset{}" % { label, i }) then
			state.altitude = km
		end
	end
end

local function drawGo(entry)
	local flightState = Game.player:GetFlightState()
	if Game.GetExcursionMode() ~= "ship" then
		ui.textColored(colors.alertYellow, "Get back aboard the ship first")
	elseif flightState ~= "FLYING" and flightState ~= "LANDED" then
		ui.textColored(colors.alertYellow, "Undock first (ship is {})" % { flightState:lower() })
	elseif ui.button("Put the ship on {}" % { entry.sbody.name }) then
		go()
	end
end

--=============================================================================

Event.Register("onGameEnd", function()
	state.system = nil
	state.bodies = {}
	state.bodyIdx = nil
end)

debugView.registerTab("DebugSurface", {
	label = "Surface",
	icon = icons.rocky_planet,
	show = function() return Game.player ~= nil and Game.system ~= nil and not Game.InHyperspace() end,
	draw = function()
		if not state.system or not state.system:IsSameSystem(Game.system.path) then
			refreshBodies()
		end

		drawBodyList()

		local entry = selectedBody()
		if not entry then
			ui.text("No planets or moons in this system")
			return
		end

		ui.separator()
		drawPosition()
		ui.separator()
		drawAltitude(entry)
		ui.separator()
		drawGo(entry)
	end
})
