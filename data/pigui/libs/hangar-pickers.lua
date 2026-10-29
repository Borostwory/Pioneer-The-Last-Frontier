-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- The Last Frontier: the cargo and crew pickers shared by the Hangar tab's
-- swap window and the Ship Market, when the current ship is stored and
-- another one flown.

local Comms = require 'Comms'
local Event = require 'Event'
local Game = require 'Game'
local PlayerState = require 'PlayerState'
local ModalWindow = require 'pigui.libs.modal-win'
local Lang = require 'Lang'
local Format = require 'Format'
local Commodities = require 'Commodities'
local CargoManager = require 'CargoManager'
local Hangar = require 'modules.Hangar.Hangar'

local ui = require 'pigui'

local l = Lang.GetResource("module-hangar")
local lc = Lang.GetResource("ui-core")

local colors = ui.theme.colors
local icons = ui.theme.icons
local orbiteer = ui.fonts.orbiteer

local Vector2 = _G.Vector2

local sliderWidth = ui.rescaleUI(Vector2(220, 0), Vector2(1600, 900)).x

local Pickers = {}

function Pickers.money(n) return Format.Money(math.floor(n + 0.5), false) end

-- A crew member's best skill, e.g. "Piloting 38"
function Pickers.bestSkill(c)
	local best, name = -1, ""
	for _, skill in ipairs({ { "engineering", lc.ENGINEERING }, { "piloting", lc.PILOTING },
			{ "navigation", lc.NAVIGATION }, { "sensors", lc.SENSORS } }) do
		if (c[skill[1]] or 0) > best then best, name = c[skill[1]] or 0, skill[2] end
	end
	return string.format("%s %d", name, best)
end

-- All four skills, as a tooltip on the last item
function Pickers.skillsTooltip(c)
	if ui.isItemHovered() then
		ui.setTooltip(string.format("%s %d\n%s %d\n%s %d\n%s %d",
			lc.ENGINEERING, c.engineering or 0, lc.PILOTING, c.piloting or 0,
			lc.NAVIGATION, c.navigation or 0, lc.SENSORS, c.sensors or 0))
	end
end

function Pickers.crewAboard()
	local list = {}
	for member in Game.player:EachCrewMember() do table.insert(list, member) end
	return list
end

-- Everyone aboard now, as a set: the default for who flies on
function Pickers.everyoneAboard()
	local aboard = {}
	for _, member in ipairs(Pickers.crewAboard()) do aboard[member] = true end
	return aboard
end

-- The cargo picker: one row per lot (mission cargo marked and named), how
-- much of it goes to the other ship. `state.take` holds the amounts (see
-- Hangar, Cargo on a swap). `room` and `lifeSupport` are the other ship's,
-- `restStaysOn` names the ship the rest stays on.
function Pickers.drawCargo(state, room, lifeSupport, restStaysOn)
	local taken = Hangar.TakeTotal(state.take)

	ui.text(l.CARGO)
	ui.sameLine()
	ui.textColored(taken > room and colors.alertRed or colors.econProfit, string.format("%d / %d t", taken, room))

	local lots = Hangar.CargoLots()
	for _, lot in ipairs(lots) do
		local c = Commodities[lot.name]
		local held = lot.tag ~= CargoManager.ORDINARY
		state.take[lot.name] = state.take[lot.name] or {}
		local parts = state.take[lot.name]

		local name = string.format("%s (%d t)", c:GetName(), lot.count)
		if held then name = ui.get_icon_glyph(icons.mission_cargo) .. " " .. name end
		ui.text(name)
		if held and ui.isItemHovered() then ui.setTooltip(lot.label) end
		ui.sameLine(sliderWidth * 1.3)
		if (c.life_support or 0) > lifeSupport then
			ui.textColored(colors.fontDim, l.NEEDS_CARGO_LIFE_SUPPORT)
			parts[lot.tag] = 0
		else
			ui.pushItemWidth(sliderWidth)
			parts[lot.tag] = ui.sliderInt("##take" .. lot.name .. tostring(lot.tag), parts[lot.tag] or 0, 0, lot.count, l.TAKE_FORMAT)
			ui.popItemWidth()
		end
		if held then
			ui.sameLine()
			ui.textColored(colors.fontDim, lot.label)
		end
	end

	if #lots == 0 then
		ui.textColored(colors.fontDim, l.NO_CARGO_ABOARD)
		return
	end
	if ui.button(l.ALL_THAT_FITS) then
		state.take = Hangar.FitCargoInto(room, lifeSupport)
	end
	ui.sameLine()
	if ui.button(l.TAKE_NONE) then
		state.take = {}
	end
	ui.textColored(colors.fontDim, string.interp(l.REST_STAYS, { ship = restStaysOn }))
	for _, lot in ipairs(lots) do
		local parts = state.take[lot.name]
		if lot.tag ~= CargoManager.ORDINARY and (parts and parts[lot.tag] or 0) < lot.count then
			ui.textColored(colors.alertYellow, l.MISSION_CARGO_STAYS)
			break
		end
	end
end

-- The crew picker: everyone aboard and everyone in the lounge here; ticked
-- people fly ship `def` (named `shipName`), the others wait in the lounge.
-- `state.aboard` is the set of ticked people.
function Pickers.drawCrew(state, def, shipName)
	local player = Game.player
	local ticked = 0
	for _ in pairs(state.aboard) do ticked = ticked + 1 end
	ui.text(string.interp(l.CREW_FOR, { ship = shipName, min = def.minCrew, max = def.maxCrew }))
	ui.sameLine()
	local countColor = ticked > def.maxCrew and colors.alertRed or ticked < def.minCrew and colors.alertYellow or colors.econProfit
	ui.textColored(countColor, string.format("%d / %d", ticked, def.maxCrew))

	local station = player:GetDockedWith()
	local lounge = Hangar.GetLoungeAt(station)
	local people = {}
	for _, member in ipairs(Pickers.crewAboard()) do table.insert(people, { member = member, waiting = false }) end
	for _, entry in ipairs(lounge and lounge.crew or {}) do table.insert(people, { member = entry.character, waiting = true }) end

	local inLoungeAfter = 0
	for _, person in ipairs(people) do
		local member = person.member
		if member.player then
			ui.text("     " .. l.YOU_COMMANDER)
		else
			local changed, value = ui.checkbox(member.name .. "##pickcrew" .. tostring(member), state.aboard[member] == true)
			if changed then state.aboard[member] = value or nil end
			Pickers.skillsTooltip(member)
			ui.sameLine()
			ui.textColored(colors.fontDim, Pickers.bestSkill(member) .. " · " .. (person.waiting and l.IN_LOUNGE or l.ABOARD))
			if person.waiting == (state.aboard[member] == true) then
				ui.sameLine()
				ui.textColored(colors.alertYellow, person.waiting and l.COMES_ABOARD or l.GOES_TO_LOUNGE)
			end
			if not state.aboard[member] then inLoungeAfter = inLoungeAfter + 1 end
		end
	end
	if inLoungeAfter > 0 and station then
		local rate = Hangar.LoungeRate(Hangar.StationFactor(station))
		ui.textColored(colors.fontDim, string.interp(l.LOUNGE_AFTER_SWAP, { count = inLoungeAfter, cost = Pickers.money(rate * inLoungeAfter) }))
	end
end

-- Let go of someone waiting in the lounge, with the same consequences as
-- dismissing them on the Crew tab
local function dismissFromLounge(member)
	if not Hangar.DismissFromLounge(member) then return end

	local contract = member.contract
	if contract then
		if contract.outstanding > 0 then
			Comms.Message(lc.IM_TIRED_OF_WORKING_FOR_NOTHING_DONT_YOU_KNOW_WHAT_A_CONTRACT_IS, member.name)
			member.playerRelationship = member.playerRelationship - 5
			if contract.outstanding > 5e2 then
				PlayerState.AddCrime("CONTRACT_FRAUD", contract.outstanding * 1.1)
			end
		elseif member:TestRoll('playerRelationship') then
			Comms.Message(lc.ITS_BEEN_GREAT_WORKING_FOR_YOU_IF_YOU_NEED_ME_AGAIN_ILL_BE_HERE_A_WHILE, member.name)
		elseif not member:TestRoll('lawfulness') then
			Comms.Message(lc.YOURE_GOING_TO_REGRET_SACKING_ME, member.name)
			member.playerRelationship = member.playerRelationship - 1
		else
			Comms.Message(lc.GOOD_RIDDANCE_TO_YOU_TOO, member.name)
			member.playerRelationship = member.playerRelationship - 1
		end
	end

	-- CrewContracts ends the contract, as for anyone leaving the crew
	Event.Queue('onLeaveCrew', Game.player, member)
	member:Save() -- they can turn up on bulletin boards again
end

-- Dismissing asks first
local dismissing = nil ---@type Character?

local dismissModal = ModalWindow.New("LoungeDismiss", function(self)
	local member = dismissing
	if not member then self:close() return end

	ui.withFont(orbiteer.heading, function()
		ui.text(string.interp(l.DISMISS_TITLE, { name = member.name }))
	end)
	ui.text(string.interp(l.DISMISS_TEXT, { name = member.name }))
	local owed = member.contract and member.contract.outstanding or 0
	if owed > 0 then
		ui.textColored(owed > 5e2 and colors.alertRed or colors.alertYellow,
			string.interp(l.DISMISS_OWED, { owed = Pickers.money(owed), limit = Pickers.money(5e2) }))
	end
	ui.spacing()

	if ui.button(lc.CANCEL) then
		dismissing = nil
		self:close()
	end
	ui.sameLine()
	if ui.button(lc.DISMISS .. "##confirmDismiss") then
		dismissFromLounge(member)
		dismissing = nil
		self:close()
	end
end)

-- Ask, then let go of someone waiting in a lounge
function Pickers.ConfirmDismiss(member)
	dismissing = member
	dismissModal:open()
end

-- A small button selecting the system of `path` on the sector map; nothing
-- when it's the current system
function Pickers.mapButton(id, path)
	if Game.system and path:IsSameSystem(Game.system.path) then return end
	ui.sameLine()
	if ui.inlineIconButton("##map" .. id, icons.sector_map, l.SHOW_ON_MAP) then
		Game.sectorView:SwitchToPath(path:SystemOnly())
		Game.SetView("SectorView")
	end
end

-- Blockers in red, warnings in amber
function Pickers.drawProblems(blockers, warnings)
	if #blockers > 0 or #warnings > 0 then ui.spacing() end
	for _, b in ipairs(blockers) do ui.textColored(colors.alertRed, b) end
	for _, w in ipairs(warnings) do ui.textColored(colors.alertYellow, w) end
end

return Pickers
