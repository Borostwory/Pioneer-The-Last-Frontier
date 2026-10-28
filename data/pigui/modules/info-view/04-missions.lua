-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

local Character	= require 'Character'
local Commodities	= require 'Commodities'
local Game		= require 'Game'
local Lang		= require 'Lang'
local Space		= require 'Space'
local Hangar	= require 'modules.Hangar.Hangar'

local ui		= require 'pigui'
local textTable	= require 'pigui.libs.text-table'
local PiGuiFace	= require 'pigui.libs.face'
local InfoView	= require 'pigui.views.info-view'

local pionillium = ui.fonts.pionillium
local orbiteer = ui.fonts.orbiteer
local l = Lang.GetResource("ui-core")
local lm = Lang.GetResource("mission-cargo")
local colors = ui.theme.colors
local icons = ui.theme.icons

local columnPadding = ui.rescaleUI(Vector2(18, 18), Vector2(1600, 900))
local itemSpacing = ui.rescaleUI(Vector2(6, 12), Vector2(1600, 900))
local detailsSpacing = ui.rescaleUI(Vector2(6, 10), Vector2(1600, 900))
local framePadding = ui.rescaleUI(Vector2(8, 4), Vector2(1600, 900))
local face = nil

local activeMission = nil

local function setLocationAsTarget(location)
	if location:isa("Body") and location:IsDynamic() then
		Game.player:SetNavTarget(location)
		ui.playSfx("OK")
	elseif Game.system and location:IsSameSystem(Game.system.path) then
		if location.bodyIndex then
			Game.player:SetNavTarget(Space.GetBody(location.bodyIndex))
			ui.playSfx("OK")
		end
	elseif not Game.InHyperspace() then
		-- if a specific systembody is given, set the sector map to the correct star (if the system is multiple)
		Game.sectorView:SwitchToPath(location:IsBodyPath() and location:GetSystemBody().nearestJumpable.path or location:SystemOnly())
		ui.playBoinkNoise()
	end
end

local function shouldShowCustomAction(action)
	if action.enabled == nil then
		return true
	end
	if type(action.enabled) == "function" then
		return action.enabled()
	end
	return action.enabled
end

-- Where the cargo held for a mission is: aboard, on a stored ship, or missing
local function drawMissionCargo(mission)
	if not mission then return end

	local lines = {} ---@type { icon: integer, color: Color, text: string }[]
	local found = {} -- commodity name -> tons found
	local function commodityName(name)
		return Commodities[name] and Commodities[name]:GetName() or name
	end

	for name, n in Game.player:GetComponent('CargoManager'):TaggedCommodities(mission) do
		found[name] = (found[name] or 0) + n
		table.insert(lines, { icon = icons.map_checkmark, color = colors.econProfit,
			text = string.interp(lm.HELD_ABOARD, { amount = n, commodity = commodityName(name), ship = Game.player.shipName }) })
	end
	for _, entry in ipairs(Hangar.FindHeldCargo(mission)) do
		found[entry.name] = (found[entry.name] or 0) + entry.count
		table.insert(lines, { icon = icons.alert_generic, color = colors.alertYellow,
			text = string.interp(lm.HELD_ON_STORED_SHIP, {
				amount = entry.count, commodity = commodityName(entry.name), ship = entry.rec.name,
				station = entry.hangar and entry.hangar.station or "?" }) })
	end

	-- Deliveries know how much they carry: say if some of it is gone
	if mission.cargo_picked_up and mission.cargotype and mission.amount then
		local missing = mission.amount - (found[mission.cargotype.name] or 0)
		if missing > 0 then
			table.insert(lines, { icon = icons.alert_generic, color = colors.alertYellow,
				text = string.interp(lm.HELD_MISSING, { amount = missing, commodity = mission.cargotype:GetName() }) })
		end
	end

	if #lines == 0 then return end

	ui.text(lm.MISSION_CARGO)
	local size = Vector2(ui.getTextLineHeight())
	for _, line in ipairs(lines) do
		ui.icon(line.icon, size, line.color)
		ui.sameLine()
		ui.textColored(line.color, line.text)
	end
	ui.newLine()
end

local function drawMissionDescription(missionDesc)
	local contentRegion = ui.getContentRegion()
	local leftColWidth = contentRegion.x / 1.618 - columnPadding.x / 2
	ui.child("MissionDetailsColumn", Vector2(leftColWidth, 0), function()
		ui.withFont(orbiteer.heading, function() ui.text(l.MISSION_DETAILS) end)
		ui.newLine()

		if missionDesc.description then
			ui.textWrapped(missionDesc.description or "<ERROR> No Description Available.")
			ui.newLine()
		end

		ui.withStyleVars({ItemSpacing=detailsSpacing}, function()
			textTable.draw(missionDesc.details)
		end)
		ui.newLine()

		drawMissionCargo(missionDesc._mission)

		if missionDesc.customDetails then
			missionDesc:customDetails()
			ui.newLine()
		end

		if ui.button(l.GO_BACK, Vector2(0, 0)) then
			activeMission = nil
		end

		if missionDesc.location then
			ui.sameLine()
			if ui.button(l.SET_AS_TARGET, Vector2(0, 0)) then setLocationAsTarget(missionDesc.location) end
		end

		if missionDesc.returnLocation then
			ui.sameLine()
			if ui.button(l.SET_RETURN_ROUTE, Vector2(0, 0)) then setLocationAsTarget(missionDesc.returnLocation) end
		end

		-- Optional per-mission buttons
		if missionDesc.customActions then
			for i, action in ipairs(missionDesc.customActions) do
				ui.sameLine()
				local show = shouldShowCustomAction(action)
				if ui.button(action.label, Vector2(0, 0), not show and ui.theme.buttonColors.disabled) then
					if show and type(action.onClick) == "function" then
						local needRefresh = action.onClick()
						local mission = activeMission._mission
						if needRefresh and mission then
							local makeForm = mission:GetClick()
							activeMission = makeForm(mission)
							activeMission._mission = mission
							rowCache = nil
						end
					end
				end
			end
		end
	end)

	ui.sameLine(0, columnPadding.x)

	ui.child("MissionGiverFace", Vector2(0, 0), function()
		if not face or face.character ~= missionDesc.client then
			face = PiGuiFace.New(missionDesc.client)
		end

		face:render()
	end)
end

local rowCache = nil

local function makeMissionRows()
	rowCache = {
		separated = true,
		{ l.TYPE, l.CLIENT, l.LOCATION, l.DUE, l.REWARD, l.STATUS, font = orbiteer.heading }
	}

	for _, mission in pairs(Character.persistent.player.missions) do
		local locationName, dist_display

		if type(mission.destination) == "string" then
			locationName = mission.destination
			dist_display = ""
		else
			locationName = mission.destination:GetStarSystem().name -- ui.Format.SystemPath(mission.destination)
			if mission.destination.bodyIndex then
				locationName = mission.destination:GetSystemBody().name .. ", " .. locationName
			end

			local playerSystem = Game.system or Game.player:GetHyperspaceTarget():GetStarSystem()

			-- Use AU for interplanetary, LY for interstellar distances
			local dist
			if mission.destination:IsSameSystem(playerSystem.path) and not Game.InHyperspace() then
				if mission.destination:IsBodyPath() then
					local body = mission.destination:GetSystemBody().body
					dist = Game.player:GetPositionRelTo(body):length()
					dist_display = "\n" .. ui.Format.Distance(dist)
				else
					dist_display = "\n-"
				end
			else
				dist = playerSystem:DistanceTo(mission.destination)
				dist_display = string.format("\n%.2f %s", dist, l.LY)
			end
		end

		local row = {
			mission:GetTypeDescription(),
			mission.client.name,
			locationName .. dist_display,
			ui.Format.Date(mission.due) .."\n".. l.D_DAYS_LEFT .." ".. ui.Format.Duration(mission.due - Game.time, 2),
			ui.Format.Money(mission.reward),
		}

		local makeForm = mission:GetClick()
		if makeForm then
			row[6] = function()
				ui.text(mission.status and l[mission.status] or l.INACTIVE)
				ui.sameLine(ui.getColumnWidth() - (framePadding.x * 2) - ui.calcTextSize(l.MORE_INFO).x)
				if ui.button(l.MORE_INFO.."##"..tostring(mission), Vector2(0, 0)) then
					activeMission = makeForm(mission)
					activeMission._mission = mission
				end
			end
		end

		table.insert(rowCache, row)
	end
end

InfoView:registerView({
    id = "missions",
    name = l.MISSIONS,
    icon = ui.theme.icons.star,
    showView = true,
	draw = function()
		ui.withFont(pionillium.body, function()
			if not rowCache then makeMissionRows() end
			if activeMission then
				drawMissionDescription(activeMission)
				return
			end

			ui.withStyleVars({ItemSpacing = itemSpacing, FramePadding = framePadding}, function()
				textTable.drawTable(6, nil, rowCache)
			end)
		end)
	end,
	refresh = function()
		rowCache = nil
		activeMission = nil
	end,
	debugReload = function()
		package.reimport()
	end
})
