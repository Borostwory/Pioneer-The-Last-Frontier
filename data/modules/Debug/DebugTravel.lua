-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Debug tab to jump to any system and appear next to any of its bodies,
-- for looking at stars and planets around the galaxy.

local Game  = require 'Game'
local Event = require 'Event'
local Dev   = require 'Dev'

local ui = require 'pigui'

local Notification = require 'pigui.libs.notification'
local debugView    = require 'pigui.views.debug'

local icons = ui.theme.icons
local colors = ui.theme.colors

--=============================================================================

-- every star type, in SystemBodyType order
local starTypes = {
	{ "BROWN_DWARF", "Brown dwarf" },
	{ "WHITE_DWARF", "White dwarf" },
	{ "STAR_M", "M (red)" },
	{ "STAR_K", "K (orange)" },
	{ "STAR_G", "G (yellow)" },
	{ "STAR_F", "F (white)" },
	{ "STAR_A", "A (blue-white)" },
	{ "STAR_B", "B (blue)" },
	{ "STAR_O", "O (blue-violet)" },
	{ "STAR_M_GIANT", "M giant" },
	{ "STAR_K_GIANT", "K giant" },
	{ "STAR_G_GIANT", "G giant" },
	{ "STAR_F_GIANT", "F giant" },
	{ "STAR_A_GIANT", "A giant" },
	{ "STAR_B_GIANT", "B giant" },
	{ "STAR_O_GIANT", "O giant" },
	{ "STAR_M_SUPER_GIANT", "M super giant" },
	{ "STAR_K_SUPER_GIANT", "K super giant" },
	{ "STAR_G_SUPER_GIANT", "G super giant" },
	{ "STAR_F_SUPER_GIANT", "F super giant" },
	{ "STAR_A_SUPER_GIANT", "A super giant" },
	{ "STAR_B_SUPER_GIANT", "B super giant" },
	{ "STAR_O_SUPER_GIANT", "O super giant" },
	{ "STAR_M_HYPER_GIANT", "M hyper giant" },
	{ "STAR_K_HYPER_GIANT", "K hyper giant" },
	{ "STAR_G_HYPER_GIANT", "G hyper giant" },
	{ "STAR_F_HYPER_GIANT", "F hyper giant" },
	{ "STAR_A_HYPER_GIANT", "A hyper giant" },
	{ "STAR_B_HYPER_GIANT", "B hyper giant" },
	{ "STAR_O_HYPER_GIANT", "O hyper giant" },
	{ "STAR_M_WF", "M Wolf-Rayet" },
	{ "STAR_B_WF", "B Wolf-Rayet" },
	{ "STAR_O_WF", "O Wolf-Rayet" },
	{ "STAR_S_BH", "Stellar black hole" },
	{ "STAR_IM_BH", "Intermediate-mass black hole" },
	{ "STAR_SM_BH", "Supermassive black hole" },
}

local state = {
	searchText = "",
	nameResults = {},

	typeIdx = 3,
	typeRadius = 10,
	typeResults = nil,

	system = nil,   ---@type SystemPath?
	systemName = nil,
	bodies = {},    -- { sbody = SystemBody, depth = integer }
	bodyIdx = nil,

	radii = 4.0,

	-- teleport to do once the hyperjump arrives: { path = SystemPath, radii = number }
	pending = nil,
}

--=============================================================================

---@param path SystemPath
local function selectSystem(path)
	path = path:SystemOnly()
	local system = path:GetStarSystem()

	state.system = path
	state.systemName = system.name
	state.bodies = {}
	state.bodyIdx = nil

	for _, bodyPath in ipairs(system:GetBodyPaths()) do
		local sbody = bodyPath:GetSystemBody()
		if sbody.type ~= "GRAVPOINT" then
			local depth = 0
			local parent = sbody.parent
			while parent do
				if parent.type ~= "GRAVPOINT" then depth = depth + 1 end
				parent = parent.parent
			end

			table.insert(state.bodies, { sbody = sbody, depth = depth })

			if not state.bodyIdx and sbody.superType == "STAR" then
				state.bodyIdx = #state.bodies
			end
		end
	end

	state.bodyIdx = state.bodyIdx or (#state.bodies > 0 and 1 or nil)
end

---@param sbody SystemBody
local function teleportTo(sbody, radii)
	local body = sbody.body
	if not body then
		Notification.add(Notification.Type.Error, "Debug travel: {} is not in this system" % { sbody.name })
		return
	end

	local ok, err = pcall(Dev.TeleportNearBody, Game.player, body, radii)
	if not ok then
		Notification.add(Notification.Type.Error, "Debug travel failed", tostring(err))
	end
end

local function go()
	local entry = state.bodies[state.bodyIdx]
	if not entry then return end

	if Game.system and state.system:IsSameSystem(Game.system.path) then
		teleportTo(entry.sbody, state.radii)
		return
	end

	state.pending = { path = entry.sbody.path, radii = state.radii }
	-- 1 second warmup, no time in hyperspace, no range or fuel checks
	local status = Game.player:InitiateHyperjumpTo(state.system, 1, 0, {})
	if status ~= "OK" then
		state.pending = nil
		Notification.add(Notification.Type.Error, "Debug travel: can't jump", status)
	end
end

local function findByType()
	local center = Game.system and Game.system.path or state.system
	if not center then return end

	local ok, result = pcall(Dev.FindSystemsWithStarType, starTypes[state.typeIdx][1], center, state.typeRadius, 10)
	if ok then
		state.typeResults = result
	else
		state.typeResults = {}
		Notification.add(Notification.Type.Error, "Debug travel: search failed", tostring(result))
	end
end

local function searchByName()
	state.nameResults = {}
	if #state.searchText < 2 then return end

	local ok, result = pcall(function()
		return Game.sectorView:GetMap():SearchNearbyStarSystemsByName(state.searchText)
	end)
	if ok and result then
		state.nameResults = result
	end
end

--=============================================================================

local function drawSystemPicker()
	if ui.button("Current system") and Game.system then
		selectSystem(Game.system.path)
	end

	ui.sameLine()
	local mapSelection = Game.sectorView:GetSelectedSystemPath()
	if ui.button("Sector map selection") and mapSelection then
		selectSystem(mapSelection)
	end

	ui.nextItemWidth(-1)
	local text, changed = ui.inputText("##systemSearch", state.searchText, {})
	if changed then
		state.searchText = text
		searchByName()
	end

	for i, path in ipairs(state.nameResults) do
		if i > 10 then break end
		if ui.selectable("{}##name{}" % { path:GetStarSystem().name, i }, false) then
			selectSystem(path)
		end
	end
end

local function drawTypeFinder()
	ui.nextItemWidth(ui.getContentRegion().x * 0.5)
	ui.comboBox("##starType", starTypes[state.typeIdx][2], function()
		for i, t in ipairs(starTypes) do
			if ui.selectable(t[2], i == state.typeIdx) then
				state.typeIdx = i
				state.typeResults = nil
			end
		end
	end)

	ui.sameLine()
	ui.nextItemWidth(ui.getContentRegion().x - ui.calcTextSize("Find").x - 20)
	state.typeRadius = ui.sliderInt("##radius", state.typeRadius, 1, 40, "%d sectors")

	ui.sameLine()
	if ui.button("Find") then
		findByType()
	end

	ui.textColored(colors.fontDim, "Searches outwards from the current system. Black holes, Wolf-Rayet stars and hyper giants are very rare and only appear 80+ ly from Sol: large radii take a few seconds.")

	if state.typeResults then
		if #state.typeResults == 0 then
			ui.text("None found within {} sectors" % { state.typeRadius })
		end

		for i, r in ipairs(state.typeResults) do
			if ui.selectable(string.format("%s  (%.1f ly)##type%d", r.name, r.distance, i), false) then
				selectSystem(r.path)
			end
		end
	end
end

local function drawTarget()
	if not state.system then
		ui.text("No system selected")
		return
	end

	ui.text("System: {}  {}" % { state.systemName, tostring(state.system) })

	ui.child("bodyList", Vector2(0, ui.getTextLineHeightWithSpacing() * 10), function()
		for i, entry in ipairs(state.bodies) do
			local sbody = entry.sbody
			local label = string.format("%s%s  [%s, %.0f km]##body%d",
				string.rep("    ", entry.depth), sbody.name, sbody.type:lower(), sbody.radius / 1000, i)
			if ui.selectable(label, i == state.bodyIdx) then
				state.bodyIdx = i
			end
		end
	end)

	ui.nextItemWidth(ui.getContentRegion().x * 0.5)
	state.radii = ui.sliderFloat("Distance (radii)", state.radii, 1.5, 50.0, "%.1f")

	local flightState = Game.player:GetFlightState()
	if flightState ~= "FLYING" then
		ui.textColored(colors.alertYellow, "Undock or take off first (ship is {})" % { flightState:lower() })
	elseif state.pending then
		ui.text("Jumping...")
	elseif state.bodyIdx then
		local entry = state.bodies[state.bodyIdx]
		if ui.button("Go to {}" % { entry.sbody.name }) then
			go()
		end
	end
end

--=============================================================================

Event.Register("onEnterSystem", function(ship)
	if not ship:IsPlayer() or not state.pending then return end

	local pending = state.pending
	state.pending = nil
	teleportTo(pending.path:GetSystemBody(), pending.radii)
end)

Event.Register("onGameEnd", function()
	state.pending = nil
	state.system = nil
	state.bodies = {}
	state.typeResults = nil
end)

debugView.registerTab("DebugTravel", {
	label = "Travel",
	icon = icons.galaxy_map,
	show = function() return Game.player ~= nil and not Game.InHyperspace() end,
	draw = function()
		if not state.system and Game.system then
			selectSystem(Game.system.path)
		end

		if ui.collapsingHeader("Find system by name", { "DefaultOpen" }) then
			drawSystemPicker()
		end

		if ui.collapsingHeader("Find system by star type", { "DefaultOpen" }) then
			drawTypeFinder()
		end

		ui.separator()
		drawTarget()
	end
})
