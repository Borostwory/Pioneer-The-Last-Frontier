-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Debug tab for revolver bases (a turning floor under one shaft, closed by a
-- lid: SpaceStationType's "revolver"): what the floor and the lid are doing,
-- AI ships sent in to land from above the base, and docked ships sent off,
-- to watch them come and go from inside.

local Game  = require 'Game'
local Space = require 'Space'
local Ship  = require 'Ship'
local ShipDef = require 'ShipDef'
local Dev   = require 'Dev'
local utils = require 'utils'

local ui = require 'pigui'

local Notification = require 'pigui.libs.notification'
local debugView    = require 'pigui.views.debug'

local icons = ui.theme.icons
local colors = ui.theme.colors

--=============================================================================

local state = {
	shipList = nil,  -- ShipDefs that can be spawned, by name
	shipNames = {},
	shipIdx = 0,     -- 0-based, as ui.combo gives it
	count = 1,       -- ships sent in at once
	height = 1500,   -- metres above the base they start
	spawned = {},    -- the ships this tab sent in (weak keys)
}
setmetatable(state.spawned, { __mode = "k" })

local function buildShipList()
	state.shipList = utils.build_array(utils.filter(function(_, def)
		return def.tag == "SHIP"
	end, pairs(ShipDef)))
	table.sort(state.shipList, function(a, b) return a.name < b.name end)
	state.shipNames = utils.map_array(state.shipList, function(def) return def.name end)
end

-- the revolver base to work with: the navigation target, the one the player
-- is docked at, else the nearest in the system
local function findStation()
	local target = Game.player:GetNavTarget()
	if target and target:isa("SpaceStation") and target.isRevolver then return target end
	local docked = Game.player:GetDockedWith()
	if docked and docked.isRevolver then return docked end

	local best, bestDist = nil, math.huge
	for _, body in ipairs(Space.GetBodies("SpaceStation")) do
		if body.isRevolver then
			local d = Game.player:DistanceTo(body)
			if d < bestDist then best, bestDist = body, d end
		end
	end
	return best
end

-- Spawn ships above the base, side by side, and send them in to land
local function sendIn(station)
	local def = state.shipList[state.shipIdx + 1]
	for i = 1, state.count do
		local ok, err = pcall(function()
			local ship = Space.SpawnShipNear(def.id, station, 2, 2)
			ship:SetLabel(Ship.MakeRandomLabel())
			-- spread out across the base, so they don't start inside each other
			local side = (i - (state.count + 1) / 2) * 300
			Dev.PlaceNearStation(ship, station, state.height, side)
			ship:AIDockWith(station)
			state.spawned[ship] = true
		end)
		if not ok then
			Notification.add(Notification.Type.Error, "Debug: couldn't send a ship in", tostring(err))
			return
		end
	end
	Notification.add(Notification.Type.Info,
		"Debug: {} {} sent to land at {}" % { state.count, def.name, station.label })
end

-- Off the base and up to a low orbit round the body it stands on (the AI
-- launches a docked ship itself)
local function sendOff(ship, station)
	local ok, err = pcall(function()
		local body = station.path:GetSystemBody().parent.body
		ship:AIEnterLowOrbit(body)
	end)
	if not ok then
		Notification.add(Notification.Type.Error, "Debug: {} can't leave" % { ship.label }, tostring(err))
	end
end

local function drawStatus(station)
	ui.text("Base: " .. station.label)
	ui.sameLine()
	ui.textColored(colors.fontDim, string.format("(%.1f km away)", Game.player:DistanceTo(station) / 1000))
	ui.textWrapped(station:GetRevolverStatus())
end

local function drawSendIn(station)
	ui.separator()
	ui.text("Send ships in to land")

	ui.pushItemWidth(ui.getTextLineHeight() * 14)
	local changed, idx = ui.combo("Ship##revolverShip", state.shipIdx, state.shipNames)
	if changed then state.shipIdx = idx end
	state.count = ui.sliderInt("How many##revolverCount", state.count, 1, 4)
	state.height = ui.sliderInt("Start height (m)##revolverHeight", state.height, 600, 5000)
	ui.popItemWidth()

	if ui.button("Spawn above the base and land") then
		sendIn(station)
	end
	ui.textColored(colors.fontDim, "They wait off to the side until the shaft is free for them.")
end

local function drawDocked(station)
	ui.separator()
	ui.text("Ships at the base")

	local ships = station:GetDockedShips()
	local any = false
	for bay, ship in pairs(ships) do
		if ship ~= Game.player then
			any = true
			ui.text("Bay {}: {} ({}), {}" % { bay, ship.label, ShipDef[ship.shipId].name, ship:GetFlightState():lower() })
			if ship:GetFlightState() == "DOCKED" then
				ui.sameLine()
				if ui.button("Leave##revolverLeave" .. bay) then
					sendOff(ship, station)
				end
			end
		end
	end
	if not any then
		ui.textColored(colors.fontDim, "None (the player's ship isn't listed)")
		return
	end

	if ui.button("All leave") then
		for _, ship in pairs(ships) do
			if ship ~= Game.player and ship:GetFlightState() == "DOCKED" then
				sendOff(ship, station)
			end
		end
	end
end

debugView.registerTab("DebugRevolver", {
	mod = true,
	label = "Revolver base",
	icon = icons.spacestation,
	show = function() return Game.player ~= nil and Game.system ~= nil and not Game.InHyperspace() end,
	draw = function()
		if not state.shipList then buildShipList() end

		local station = findStation()
		if not station then
			ui.text("No revolver base in this system.")
			return
		end

		drawStatus(station)
		drawSendIn(station)
		drawDocked(station)
	end
})
