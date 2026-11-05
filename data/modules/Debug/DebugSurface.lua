-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Debug tab to put the player's ship on any planet or moon of the current
-- system: landed, or a given height above the ground, at any latitude and
-- longitude, for looking at terrain. Finds rivers and other features of the
-- physical surface generator, and says what is under the ship.

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

-- what can be found; size says how to read a feature's size in metres
local featureKinds = {
	{ id = "river", label = "River", size = "wide" },
	{ id = "fork", label = "Y fork", size = "wide" },
	{ id = "mouth", label = "River's end", size = "wide" },
	{ id = "lake", label = "Lake", size = "across" },
	{ id = "smalllake", label = "Small lake", size = "across", around = true },
	{ id = "coast", label = "Coast" },
	{ id = "peak", label = "Mountain", size = "above sea" },
	{ id = "volcano", label = "Volcano", size = "across" },
	{ id = "lavafield", label = "Lava field", size = "across" },
	{ id = "patera", label = "Patera", size = "across" },
	{ id = "block", label = "Mountain block", size = "across" },
}

local state = {
	system = nil,   ---@type SystemPath?
	bodies = {},    -- { sbody = SystemBody, depth = integer }
	bodyIdx = nil,

	latitude = 0.0,  -- degrees
	longitude = 0.0, -- degrees
	landed = true,
	altitude = 1.0,  -- km

	-- what the text fields hold, as typed or pasted
	latText = "0",
	lonText = "0",
	altText = "1",

	-- the feature finder
	found = {},          -- kind id -> features of the body below
	foundBody = nil,
	kind = nil,          -- the kind in the list
	list = {},           -- in the order they are visited
	listIdx = 0,
	largestFirst = false,

	-- what is under the ship, read a few times a second
	underShip = "",
	underShipFrames = 0,
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

local function formatNumber(v)
	return string.format("%.4f", v):gsub("0+$", ""):gsub("%.$", "")
end

-- show the values in the text fields after they were set otherwise
local function updateTexts()
	state.latText = formatNumber(state.latitude)
	state.lonText = formatNumber(state.longitude)
	state.altText = formatNumber(state.altitude)
end

-- Degrees from text: decimal ("-56.5492"), or degrees, minutes and seconds
-- as the HUD shows them ("S 056°32'57\"", "56 32 57 S"); S and W, or a
-- leading minus, make it negative. nil if there is no number.
local function parseDegrees(text)
	local numbers = {}
	for n in text:gmatch("%d+%.?%d*") do
		table.insert(numbers, tonumber(n))
	end
	if #numbers == 0 then return nil end

	local value = numbers[1] + (numbers[2] or 0) / 60 + (numbers[3] or 0) / 3600
	local upper = text:upper()
	if upper:find("[SW]") or text:match("^%s*%-") then
		value = -value
	end
	return value
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
		updateTexts()
	end
end

local function randomPosition()
	-- uniform over the sphere
	state.latitude = math.rad2deg(math.asin(Engine.rand:Number(-1, 1)))
	state.longitude = Engine.rand:Number(-180, 180)
	updateTexts()
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

local function unitVector(lat, lon)
	return math.cos(lat) * math.sin(lon), math.sin(lat), math.cos(lat) * math.cos(lon)
end

local function featuresOf(body, kind, lat, lon)
	-- (what is only looked for around a place is looked for each time)
	if kind.around then
		return Dev.GetTerrainFeatures(body, kind.id, lat, lon)
	end
	if state.foundBody ~= body then
		state.foundBody = body
		state.found = {}
	end
	if not state.found[kind.id] then
		state.found[kind.id] = Dev.GetTerrainFeatures(body, kind.id)
	end
	return state.found[kind.id]
end

local function shipIsNear(entry)
	return Game.player.frameBody == entry.sbody.body
end

-- list the features of a kind, nearest the ship first (or the position in the fields, if it is
-- away from the body), or largest first
local function makeList(entry, kind)
	local x0, y0, z0
	local lat, lon
	if shipIsNear(entry) then lat, lon = Game.player:GetGroundPosition() end
	if not lat then lat, lon = math.deg2rad(state.latitude), math.deg2rad(state.longitude) end
	x0, y0, z0 = unitVector(lat, lon)

	local list = {}
	for i, feature in ipairs(featuresOf(entry.sbody.body, kind, lat, lon)) do
		local x, y, z = unitVector(feature.lat, feature.lon)
		list[i] = { feature = feature, closeness = x * x0 + y * y0 + z * z0 }
	end
	if state.largestFirst then
		table.sort(list, function(a, b) return a.feature.size > b.feature.size end)
	else
		table.sort(list, function(a, b) return a.closeness > b.closeness end)
	end
	state.kind = kind
	state.list = list
	state.listIdx = 0
end

local function formatLength(metres)
	if metres >= 10000 then return string.format("%.0f km", metres / 1000) end
	if metres >= 1000 then return string.format("%.1f km", metres / 1000) end
	return string.format("%.0f m", metres)
end

-- to the next in the list, or the one before: the fields show it, and the ship goes if it can
local function visit(step, canGo)
	local count = #state.list
	if count == 0 then return end
	state.listIdx = (state.listIdx + step - 1) % count + 1
	local feature = state.list[state.listIdx].feature
	state.latitude = math.rad2deg(feature.lat)
	state.longitude = math.rad2deg(feature.lon)
	updateTexts()
	if canGo then go() end
	Dev.LogNote(string.format("went to %s %d of %d, size %.0f m, lat %.4f lon %.4f, height %s", state.kind.label, state.listIdx, count, feature.size, state.latitude, state.longitude, state.landed and "landed" or (state.altitude .. " km")))
end

--=============================================================================

local function drawBodyList()
	ui.child("surfaceBodyList", Vector2(0, ui.getTextLineHeightWithSpacing() * 10), function()
		for i, entry in ipairs(state.bodies) do
			local sbody = entry.sbody
			local label = string.format("%s%s  [%s, %.0f km]##surfaceBody%d",
				string.rep("    ", entry.depth), sbody.name, sbody.type:lower(), sbody.radius / 1000, i)
			if ui.selectable(label, i == state.bodyIdx) then
				if state.bodyIdx ~= i then
					-- the list belongs to the body it was made on
					state.kind = nil
					state.list = {}
					state.listIdx = 0
				end
				state.bodyIdx = i
			end
		end
	end)
end

-- a text field for degrees; the parsed value (or the text's problem) beside it
local function degreesField(label, key, textKey, limit)
	ui.nextItemWidth(ui.getContentRegion().x * 0.4)
	local text, changed = ui.inputText(label, state[textKey], {})
	if changed then
		state[textKey] = text
		local value = parseDegrees(text)
		if value and math.abs(value) <= limit then
			state[key] = value
		end
	end

	ui.sameLine()
	local value = parseDegrees(state[textKey])
	if not value then
		ui.textColored(colors.alertYellow, "not a number")
	elseif math.abs(value) > limit then
		ui.textColored(colors.alertYellow, "out of range")
	else
		ui.textColored(colors.fontDim, string.format("= %.4f", state[key]))
	end
end

local function drawPosition()
	degreesField("Latitude##surfaceLat", "latitude", "latText", 90)
	degreesField("Longitude##surfaceLon", "longitude", "lonText", 180)

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
		updateTexts()
	end

	ui.textColored(colors.fontDim, "Type or paste degrees: -56.5492, or as the HUD shows them: S 056°32'57\" (S and W are negative).")
end

local function drawAltitude(entry)
	if canLand(entry.sbody) then
		local changed, value = ui.checkbox("Landed", state.landed)
		if changed then state.landed = value end
	else
		ui.textColored(colors.fontDim, "No surface to land on: above the cloud tops only")
	end

	if state.landed and canLand(entry.sbody) then return end

	ui.nextItemWidth(ui.getContentRegion().x * 0.4)
	local text, changed = ui.inputText("Height above ground (km)##surfaceAlt", state.altText, {})
	if changed then
		state.altText = text
		local value = tonumber(text)
		if value and value > 0 then
			state.altitude = value
		end
	end

	for i, km in ipairs(presets) do
		if i > 1 then ui.sameLine() end
		local label = km < 1 and "{} m" % { math.floor(km * 1000 + 0.5) } or "{} km" % { km }
		if ui.button("{}##preset{}" % { label, i }) then
			state.altitude = km
			updateTexts()
		end
	end
end

-- the text fields hold usable degrees (what Go would use)
local function validPosition()
	local lat, lon = parseDegrees(state.latText), parseDegrees(state.lonText)
	return lat and lon and math.abs(lat) <= 90 and math.abs(lon) <= 180
end

-- nil if the ship can be put somewhere, else why not
local function whyNotGo()
	local flightState = Game.player:GetFlightState()
	if Game.GetExcursionMode() ~= "ship" then
		return "Get back aboard the ship first"
	elseif flightState ~= "FLYING" and flightState ~= "LANDED" then
		return "Undock first (ship is {})" % { flightState:lower() }
	end
	return nil
end

local function drawFinder(entry)
	if not entry.sbody.body or not canLand(entry.sbody) then return end

	ui.text("Find and go there, with the height set below:")
	local canGo = whyNotGo() == nil
	for i, kind in ipairs(featureKinds) do
		if i > 1 then ui.sameLine() end
		if ui.button("{}##find{}" % { kind.label, kind.id }) then
			makeList(entry, kind)
			visit(1, canGo)
		end
	end

	local changed, value = ui.checkbox("Largest first (nearest to the ship if off)", state.largestFirst)
	if changed then
		state.largestFirst = value
		if state.kind then makeList(entry, state.kind) end
	end

	if state.kind then
		local count = #state.list
		if count == 0 then
			ui.textColored(colors.alertYellow, "None on this body: {}" % { state.kind.label:lower() })
		else
			if ui.button("Previous##find") then visit(-1, canGo) end
			ui.sameLine()
			if ui.button("Next##find") then visit(1, canGo) end
			ui.sameLine()
			if state.listIdx > 0 then
				local at = state.list[state.listIdx]
				local away = math.acos(math.max(-1, math.min(1, at.closeness))) * entry.sbody.radius
				local size = state.kind.size and ", {} {}" % { formatLength(at.feature.size), state.kind.size } or ""
				ui.text("{} {} of {}{}, was {} from the ship" % { state.kind.label, state.listIdx, count, size, formatLength(away) })
			end
		end
	end
end

-- what is under the ship, if the terrain can say
local function drawUnderShip(entry)
	if not entry.sbody.body or not canLand(entry.sbody) or not shipIsNear(entry) then return end

	state.underShipFrames = state.underShipFrames - 1
	if state.underShipFrames <= 0 then
		state.underShipFrames = 20
		local lat, lon = Game.player:GetGroundPosition()
		state.underShip = lat and Dev.DescribeTerrain(entry.sbody.body, lat, lon) or ""
	end
	if state.underShip ~= "" then
		ui.separator()
		ui.text("Under the ship:")
		ui.text(state.underShip)
	end
end

-- the map's click: there, with the sun high
local function goFromMap(entry, lat, lon)
	state.latitude, state.longitude = lat, lon
	updateTexts()
	local why = whyNotGo()
	if why then
		Notification.add(Notification.Type.Error, "Debug surface: " .. why)
		return
	end
	local ok, err = pcall(Dev.SetNoonAt, entry.sbody.body, math.deg2rad(lat), math.deg2rad(lon))
	if not ok then
		Notification.add(Notification.Type.Error, "Debug surface: can't make it day there", tostring(err))
	end
	go()
	Dev.LogNote(string.format("went from the map to lat %.4f lon %.4f, height %s", lat, lon, state.landed and "landed" or (state.altitude .. " km")))
end

local function drawMap(entry)
	local body = entry.sbody.body
	if not body or not canLand(entry.sbody) then return end

	if ui.button("Make a map of {}" % { entry.sbody.name }) then
		local ok, result = pcall(Dev.MakeTerrainMap, body)
		if ok then
			state.map = { body = body, id = result }
		else
			state.map = nil
			Notification.add(Notification.Type.Error, "Debug surface: no map", tostring(result))
		end
	end

	local map = state.map
	if not map or map.body ~= body then return end

	ui.sameLine()
	if ui.button("Close the map") then
		state.map = nil
		Dev.ReleaseTerrainMap()
		return
	end

	local width = ui.getContentRegion().x
	local size = Vector2(width, width / 2)
	local pos = ui.getCursorScreenPos()
	ui.dummy(size)
	ui.addImage(map.id, pos, pos + size, Vector2(0, 0), Vector2(1, 1), Color(255, 255, 255))

	-- the ship
	if shipIsNear(entry) then
		local lat, lon = Game.player:GetGroundPosition()
		if lat then
			local at = pos + Vector2((lon / (2 * math.pi) + 0.5) * size.x, (0.5 - lat / math.pi) * size.y)
			ui.addLine(at - Vector2(6, 0), at + Vector2(6, 0), Color(255, 255, 255), 2.0, false)
			ui.addLine(at - Vector2(0, 6), at + Vector2(0, 6), Color(255, 255, 255), 2.0, false)
		end
	end

	if ui.isItemHovered() then
		local mouse = ui.getMousePos() - pos
		local lat = (0.5 - mouse.y / size.y) * 180
		local lon = (mouse.x / size.x - 0.5) * 360
		ui.text(string.format("Click to go there, at noon: %.2f, %.2f", lat, lon))
		if ui.isMouseClicked(0) then
			goFromMap(entry, lat, lon)
		end
	else
		ui.textColored(colors.fontDim, "Click to go there, at noon, at the height set below. The cross is the ship.")
	end
end

local function drawGo(entry)
	local why = whyNotGo()
	if why then
		ui.textColored(colors.alertYellow, why)
	elseif not validPosition() then
		ui.textColored(colors.alertYellow, "Fix the latitude or longitude first")
	elseif ui.button("Put the ship on {}" % { entry.sbody.name }) then
		go()
	end
end

--=============================================================================

Event.Register("onGameEnd", function()
	state.system = nil
	state.bodies = {}
	state.bodyIdx = nil
	state.found = {}
	state.foundBody = nil
	state.kind = nil
	state.list = {}
	state.listIdx = 0
	state.underShip = ""
	state.map = nil
	Dev.ReleaseTerrainMap()
end)

debugView.registerTab("DebugSurface", {
	mod = true,
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
		drawMap(entry)
		ui.separator()
		drawFinder(entry)
		ui.separator()
		drawPosition()
		ui.separator()
		drawAltitude(entry)
		ui.separator()
		drawGo(entry)
		drawUnderShip(entry)
	end
})
