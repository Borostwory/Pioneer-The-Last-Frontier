-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Debug tab for the hangar: put a ship into the hangar at the station you're
-- docked at, bill a month of rent early, and pass time.

local Game = require 'Game'
local Format = require 'Format'
local Timer = require 'Timer'
local Engine = require 'Engine'
local ShipDef = require 'ShipDef'
local HullConfig = require 'HullConfig'
local Equipment = require 'Equipment'
local EquipSet = require 'EquipSet'
local PlayerState = require 'PlayerState'
local ModelSkin = require 'SceneGraph.ModelSkin'
local Hangar = require 'modules.Hangar.Hangar'
local Character = require 'Character'
local utils = require 'utils'

local ui = require 'pigui'
local debugView = require 'pigui.views.debug'

local shipList = nil
local shipNames = {}
local selectedShip = 0
local lastBill = nil
local equipList = nil
local equipNames = {}
local selectedEquip = 0
local pass = { n = 1, unit = 0 } -- unit: 0 days, 1 weeks, 2 months
local fastForwardTo = nil         -- set while time runs fast to a target

-- Time t plus n days, weeks or calendar months (the same day and time n
-- months later, or the month's last day if it is shorter)
local function addTime(t, n, unit)
	if unit == 0 then return t + n * 86400 end
	if unit == 1 then return t + n * 7 * 86400 end
	local year, month, day, hour, minute, second = util.gameTimeToTimeParts(t)
	month = month - 1 + n
	year, month = year + math.floor(month / 12), month % 12 + 1
	local nextYear, nextMonth = year, month + 1
	if nextMonth > 12 then nextYear, nextMonth = year + 1, 1 end
	local days = (util.timePartsToGameTime(nextYear, nextMonth, 1, 0, 0, 0)
		- util.timePartsToGameTime(year, month, 1, 0, 0, 0)) / 86400
	return util.timePartsToGameTime(year, month, math.min(day, days), hour, minute, second)
end

-- Jump the clock if the game can (needs a build with Game.DebugAdvanceTime),
-- otherwise run time at 10000x until the target and stop there
local function passTime(target)
	if Game.DebugAdvanceTime and Game.DebugAdvanceTime(target - Game.time) then return end
	fastForwardTo = target
	Game.SetTimeAcceleration("10000x")
	Timer:CallAt(target, function()
		if fastForwardTo == target then
			fastForwardTo = nil
			Game.SetTimeAcceleration("1x")
		end
	end)
end

local function drawPassTime()
	ui.text("Pass time:")
	ui.sameLine()
	ui.pushItemWidth(ui.getTextLineHeight() * 6)
	pass.n = ui.sliderInt("##debugPassN", pass.n, 1, 24)
	ui.sameLine()
	local changed, unit = ui.combo("##debugPassUnit", pass.unit, { "days", "weeks", "months" })
	if changed then pass.unit = unit end
	ui.popItemWidth()
	ui.sameLine()

	local target = addTime(Game.time, pass.n, pass.unit)
	if fastForwardTo then
		ui.text("running at 10000x until " .. Format.Date(fastForwardTo))
		ui.sameLine()
		if ui.button("Stop") then
			fastForwardTo = nil
			Game.SetTimeAcceleration("1x")
		end
	elseif ui.button("Pass") then
		passTime(target)
	end
	ui.text("Now {}, would go to {}" % { Format.Date(Game.time), Format.Date(target) })
	if not Game.DebugAdvanceTime then
		ui.textColored(ui.theme.colors.fontDim, "This build can't jump the clock: time runs at 10000x to the target instead.")
	end
end

local function buildShipList()
	shipList = utils.build_array(utils.filter(function(_, def)
		return def.tag == "SHIP" and def.basePrice > 0
	end, pairs(ShipDef)))
	table.sort(shipList, function(a, b) return a.name < b.name end)
	shipNames = utils.map_array(shipList, function(def) return def.name end)
end

-- A record for a new ship with the hull's default equipment, as the Ship
-- Market fits it
local function newRecord(def)
	local config = HullConfig.GetHullConfig(def.id)
	local equip = {}

	for _, slot in pairs(config.slots) do
		local proto = slot.default and Equipment.Get(slot.default)
		if proto then
			local inst = proto:Instance()
			if inst.SpecializeForShip then inst:SpecializeForShip(config) end
			if slot.count then inst:SetCount(slot.count) end
			equip[slot.id] = inst
		end
	end

	local hasDrive = false
	for _, item in pairs(equip) do
		if item.slot and item.slot.type:match("^hyperdrive") then hasDrive = true end
	end
	if def.hyperdriveClass > 0 and not hasDrive then
		local slot = utils.best_score(config.slots, function(_, s)
			return s.type:match("^hyperdrive") and 1 or nil
		end)
		local drive = slot and utils.best_score(Equipment.new, function(_, e)
			return EquipSet.CompatibleWithSlot(e, slot) and e.slot.type:match("%.civilian")
				and e.capabilities.hyperclass or nil
		end)
		if drive then equip[slot.id] = drive:Instance() end
	end

	local model = Engine.GetModel(def.modelName)
	local label = require 'Ship'.MakeRandomLabel()
	return {
		shipId = def.id,
		name = def.name,
		label = label,
		skin = ModelSkin.New():SetRandomColors(Engine.rand):SetDecal(def.manufacturer):SetLabel(label),
		pattern = model.numPatterns ~= 0 and Engine.rand:Integer(1, model.numPatterns) or nil,
		hull = 100,
		fuel = 100,
		equip = equip,
		cargo = {},
		state = {},
	}
end

debugView.registerTab("debug-hangar", {
	icon = ui.theme.icons.hangar,
	label = "Hangar",
	show = function() return Game.player ~= nil end,
	draw = function()
		if not shipList then buildShipList() end

		local station = Game.player:GetDockedWith()
		local h = station and Hangar.GetHangarAt(station)

		if not station then
			ui.text("Dock at a station first.")
		elseif not h then
			ui.text("No hangar here: lease one in the station's Hangar tab.")
		else
			ui.text("Hangar at {}: {} of {} bays free" % { h.station, Hangar.FreeBays(h), h.bays })
			ui.text("Rent {} per month · credit {} · debt {} ({} unpaid months) · next bill {}" % {
				math.floor(Hangar.MonthlyRent(h) + 0.5), math.floor(h.credit + 0.5), math.floor(h.debt + 0.5),
				h.unpaidMonths, Format.DateOnly(Hangar.NextBill() or Game.time) })

			local changed, idx = ui.combo("##debugHangarShip", selectedShip, shipNames)
			if changed then selectedShip = idx end
			ui.sameLine()
			if ui.button("Store a new ship here") then
				local def = shipList[selectedShip + 1]
				if def then Hangar.AddRecord(h, newRecord(def)) end
			end

			if not equipList then
				equipList = utils.build_array(pairs(Equipment.new))
				table.sort(equipList, function(a, b) return a:GetName() < b:GetName() end)
				equipNames = utils.map_array(equipList, function(e) return e:GetName() end)
			end
			local changedE, idxE = ui.combo("##debugHangarEquip", selectedEquip, equipNames)
			if changedE then selectedEquip = idxE end
			ui.sameLine()
			if ui.button("Put one in the store") then
				local proto = equipList[selectedEquip + 1]
				if proto then table.insert(h.store, proto:Instance()) end
			end
			ui.sameLine()
			ui.text("{} item(s) in the store" % { #h.store })

			ui.separator()
			if ui.button("Bill a month now") then
				local bill, fromCredit, fromMoney, unpaid = Hangar.DebugBillMonth(h)
				lastBill = "Billed {}: {} from credit, {} from cash, {} unpaid" % {
					math.floor(bill + 0.5), math.floor(fromCredit + 0.5), math.floor(fromMoney + 0.5), math.floor(unpaid + 0.5) }
			end
			ui.sameLine()
			if ui.button("Clear the credit") then
				Hangar.DebugClearCredit(h)
			end
			if lastBill then ui.text(lastBill) end
		end

		ui.separator()
		if Game.player:IsDocked() then
			drawPassTime()
		else
			ui.text("Dock to pass time.")
		end

		ui.separator()
		local waiting = 0
		for _ in Hangar.EachLoungeMember() do waiting = waiting + 1 end
		ui.text("Crew: {} aboard, {} in lounges, lounge debt {}" % { Game.player:CrewNumber(),
			waiting, math.floor(Hangar.LoungeDebt() + 0.5) })
		ui.sameLine()
		if ui.button("Hire a test crew member") then
			-- hired as from the bulletin board: the contract is set once aboard
			local c = Character.New()
			c:RollNew(true)
			if Game.player:Enroll(c) then
				c.contract = { wage = 50, payday = Game.time + 7 * 86400, outstanding = 0 }
			end
		end

		ui.separator()
		ui.text("Cash: {}" % { math.floor(PlayerState.GetMoney()) })
		ui.sameLine()
		if ui.button("Set cash to 0") then
			PlayerState.SetMoney(0)
		end
		ui.sameLine()
		if ui.button("Add 100 000 cash") then
			PlayerState.AddMoney(100000)
		end

		ui.separator()
		for _, rec in ipairs(Hangar.GetAllShips()) do
			local hh = Hangar.GetHangar(rec.hangar)
			ui.text("#{} {} ({}) at {}" % { rec.id, rec.name, rec.shipId, hh and hh.station or rec.hangar })
		end
	end
})
