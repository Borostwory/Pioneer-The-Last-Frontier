-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- The Last Frontier: the station's Hangar tab. Lease a hangar, see the ships
-- stored here and swap the active ship for one of them.

local Comms = require 'Comms'
local Event = require 'Event'
local Game = require 'Game'
local Lang = require 'Lang'
local Format = require 'Format'
local ShipDef = require 'ShipDef'
local Commodities = require 'Commodities'
local PlayerState = require 'PlayerState'
local StationView = require 'pigui.views.station-view'
local ModalWindow = require 'pigui.libs.modal-win'
local ModelSpinner = require 'PiGui.Modules.ModelSpinner'
local Hangar = require 'modules.Hangar.Hangar'
local CargoManager = require 'CargoManager'

local ui = require 'pigui'

local l = Lang.GetResource("module-hangar")
local lc = Lang.GetResource("ui-core")
local ls = Lang.GetResource("ships")

local colors = ui.theme.colors
local icons = ui.theme.icons
local pionillium = ui.fonts.pionillium
local orbiteer = ui.fonts.orbiteer

local Vector2 = _G.Vector2

local widgetSizes = ui.rescaleUI({
	itemSpacing = Vector2(8, 6),
	cellPadding = Vector2(12, 3),
	sliderWidth = 220,
}, Vector2(1600, 900))

local PREVIEW_WIDTH = 0.45 -- share of the width for the 3D preview

local ACTIVE = "active"

local modelSpinner = ModelSpinner()
local spinnerKey = nil

local selected = ACTIVE          -- ACTIVE or a stored ship's record id
local renaming = nil             -- record id being renamed
local renameText = ""
local showPrepay = false
local prepay = { n = 6, unit = 0 } -- unit: 0 months, 1 years
local lease = { bays = 1, n = 0, unit = 0 }
local swap = nil                 -- { rec = Hangar.Record, take = { [name] = { [tag] = n } }, aboard = { [Character] = true } }
local swapModal                  -- the swap window, created below

--=============================================================================
-- Helpers
--=============================================================================

local function money(n) return Format.Money(math.floor(n + 0.5), false) end

local function shipTypeName(shipId)
	local def = ShipDef[shipId]
	return def and ls[def.i18n_key] or shipId
end

local function months(sel)
	return sel.unit == 1 and sel.n * 12 or sel.n
end

-- A button that does nothing when disabled and says why in its tooltip
local function button(label, enabled, tooltip)
	local variant = not enabled and ui.theme.buttonColors.disabled or nil
	local clicked = ui.button(label, Vector2(0, 0), variant, tooltip)
	return clicked and enabled
end

local function iconText(icon, color, text)
	local size = Vector2(ui.getTextLineHeight())
	ui.icon(icon, size, color)
	ui.sameLine()
	ui.textColored(color, text)
end

local function drawUnitPicker(id, sel)
	ui.pushItemWidth(widgetSizes.sliderWidth)
	local max = sel.unit == 1 and 5 or 24
	sel.n = ui.sliderInt("##" .. id .. "n", math.min(sel.n, max), 0, max)
	ui.popItemWidth()
	ui.sameLine()
	ui.pushItemWidth(widgetSizes.sliderWidth * 0.6)
	local changed, unit = ui.combo("##" .. id .. "unit", sel.unit, { l.MONTHS, l.YEARS })
	if changed then sel.unit = unit end
	ui.popItemWidth()

	ui.sameLine()
	for _, q in ipairs({ { 1, 0, l.Q_1M }, { 3, 0, l.Q_3M }, { 6, 0, l.Q_6M }, { 1, 1, l.Q_1Y }, { 2, 1, l.Q_2Y } }) do
		if ui.button(q[3] .. "##" .. id .. q[3], Vector2(0, 0), ui.theme.buttonColors.dark) then
			sel.n, sel.unit = q[1], q[2]
		end
		ui.sameLine()
	end
	ui.newLine()
end

local function quoteLine(monthly, nMonths)
	local value, cost, discount = Hangar.QuotePrepay(monthly, nMonths)
	if nMonths <= 0 then
		ui.textColored(colors.fontDim, l.PAY_NOTHING_NOW)
		return 0
	end
	local text = money(value)
	if discount > 0 then
		text = string.interp(l.PRICE_WITH_DISCOUNT, { value = money(value), discount = string.format("%d", discount * 100), cost = money(cost) })
	end
	ui.text(text)
	return cost
end

-- A crew member's best skill, e.g. "Piloting 38"
local function bestSkill(c)
	local best, name = -1, ""
	for _, skill in ipairs({ { "engineering", lc.ENGINEERING }, { "piloting", lc.PILOTING },
			{ "navigation", lc.NAVIGATION }, { "sensors", lc.SENSORS } }) do
		if (c[skill[1]] or 0) > best then best, name = c[skill[1]] or 0, skill[2] end
	end
	return string.format("%s %d", name, best)
end

local function skillsTooltip(c)
	if ui.isItemHovered() then
		ui.setTooltip(string.format("%s %d\n%s %d\n%s %d\n%s %d",
			lc.ENGINEERING, c.engineering or 0, lc.PILOTING, c.piloting or 0,
			lc.NAVIGATION, c.navigation or 0, lc.SENSORS, c.sensors or 0))
	end
end

local function crewAboard()
	local list = {}
	for member in Game.player:EachCrewMember() do table.insert(list, member) end
	return list
end

--=============================================================================
-- Lease form (no hangar here)
--=============================================================================

local function drawLeaseForm(station)
	local player = Game.player
	local factor = Hangar.StationFactor(station)
	local tonnes = Hangar.HullMass(player.shipId)

	ui.withFont(orbiteer.heading, function()
		ui.text(string.interp(l.NO_HANGAR_HERE, { station = station.label }))
	end)
	ui.spacing()

	ui.text(l.BAYS)
	ui.sameLine()
	ui.pushItemWidth(widgetSizes.sliderWidth)
	lease.bays = ui.sliderInt("##leasebays", lease.bays, 1, Hangar.MAX_BAYS)
	ui.popItemWidth()
	ui.sameLine()
	ui.textColored(colors.fontDim, l.BAYS_CAN_CHANGE)

	local empty = Hangar.QuoteRent(factor, lease.bays, 0)
	local withShip = Hangar.QuoteRent(factor, lease.bays, tonnes)
	ui.text(string.interp(l.RENT_RULE, { bay = money(Hangar.BAY_RENT * factor), tonne = money(Hangar.TONNE_RENT * factor) }))
	ui.textColored(colors.fontDim, string.interp(l.RENT_EXAMPLE, {
		empty = money(empty), ship = player.shipName, tonnes = tonnes, rent = money(withShip)
	}))

	ui.spacing()
	ui.text(l.PAY_UPFRONT)
	drawUnitPicker("lease", lease)
	local n = months(lease)
	local cost = quoteLine(withShip, n)
	if n > 0 then
		ui.sameLine()
		local credit = Hangar.QuotePrepay(withShip, n)
		local date = Hangar.CreditLastsUntil(Game.time, credit, withShip)
		ui.textColored(colors.fontDim, string.interp(l.PAID_UNTIL_ABOUT, { date = Format.DateOnly(date) }))
	end

	ui.spacing()
	local canPay = PlayerState.GetMoney() >= cost
	if button(l.LEASE_HANGAR, canPay, not canPay and l.NOT_ENOUGH_MONEY or nil) then
		Hangar.Lease(station, lease.bays, n, tonnes)
		selected = ACTIVE
	end

	local others = {}
	for _, h in ipairs(Hangar.GetHangars()) do
		table.insert(others, string.interp(l.OTHER_HANGAR, { station = h.station, used = h.bays - Hangar.FreeBays(h), bays = h.bays }))
	end
	if #others > 0 then
		ui.spacing()
		ui.textWrapped(l.YOUR_OTHER_HANGARS .. " " .. table.concat(others, " · "))
	end
end

--=============================================================================
-- Hangar header: bays, rent, upfront payment, debt
--=============================================================================

local function drawHeader(h)
	ui.withFont(orbiteer.heading, function()
		ui.icon(icons.hangar, Vector2(ui.getTextLineHeight()), colors.font)
		ui.sameLine()
		ui.text(string.interp(l.HANGAR_AT, { station = h.station }))
	end)

	if button(l.PAY_UPFRONT_BUTTON, true) then showPrepay = not showPrepay end
	ui.sameLine()
	if button(l.ADD_BAY, h.bays < Hangar.MAX_BAYS, h.bays >= Hangar.MAX_BAYS and l.MAX_BAYS_REACHED or nil) then
		Hangar.AddBay(h)
	end
	ui.sameLine()
	local canRemove = h.bays > 1 and Hangar.FreeBays(h) > 0
	if button(l.REMOVE_BAY, canRemove, not canRemove and l.REMOVE_BAY_NEEDS_EMPTY or nil) then
		Hangar.RemoveBay(h)
	end
	ui.sameLine()
	local canEnd = Hangar.CanEndLease(h)
	if button(l.END_LEASE, canEnd, not canEnd and l.END_LEASE_NEEDS_EMPTY or nil) then
		Hangar.EndLease(h)
		return false
	end

	if showPrepay then
		ui.spacing()
		ui.text(l.PAY_UPFRONT)
		drawUnitPicker("prepay", prepay)
		local n = months(prepay)
		local cost = quoteLine(Hangar.MonthlyRent(h), n)
		ui.sameLine()
		local canPay = n > 0 and PlayerState.GetMoney() >= cost
		if button(l.PAY .. "##prepay", canPay, (n > 0 and not canPay) and l.NOT_ENOUGH_MONEY or nil) then
			Hangar.Prepay(h, n)
			showPrepay = false
		end
	end

	if Hangar.IsLocked(h) then
		ui.spacing()
		iconText(icons.lease_locked, colors.alertRed, string.interp(l.RENT_UNPAID_BANNER, { debt = money(h.debt), months = h.unpaidMonths }))
		ui.sameLine()
		local debt = math.floor(h.debt + 0.5)
		local canPay = PlayerState.GetMoney() >= debt
		if button(string.interp(l.PAY_AMOUNT, { amount = money(debt) }), canPay, not canPay and l.NOT_ENOUGH_MONEY or nil) then
			Hangar.PayDebt(h)
		end
	end

	if #h.store > 0 then
		ui.textColored(colors.alertYellow, string.interp(l.ITEMS_IN_STORE, { count = #h.store }))
	end

	return true
end

--=============================================================================
-- Ship list and details
--=============================================================================

local function activeView()
	local p = Game.player
	local equip = {}
	for _, item in pairs(p:GetComponent('EquipSet'):GetInstalledEquipment()) do table.insert(equip, item) end
	local cargoMgr = p:GetComponent('CargoManager')
	local cargo, held = {}, {}
	for name, n in cargoMgr:Commodities() do
		cargo[name] = n
		local ordinary = cargoMgr:CountCommodity(Commodities[name], CargoManager.ORDINARY)
		if ordinary < n then held[name] = n - ordinary end
	end
	return {
		active = true,
		shipId = p.shipId, name = p.shipName, label = p.label,
		skin = p:GetSkin(), pattern = p.model.pattern,
		hull = p.hullPercent, fuel = p.fuel,
		items = equip, cargo = cargo, held = held,
		cargoCap = p.cargo_cap,
		hyperdrive = p:GetInstalledHyperdrive(),
	}
end

local function recordView(rec)
	local items, hyperdrive = {}, nil
	for _, item in pairs(rec.equip) do
		table.insert(items, item)
		if item.slot and item.slot.type:match("^hyperdrive") then hyperdrive = item end
	end
	return {
		rec = rec,
		shipId = rec.shipId, name = rec.name, label = rec.label,
		skin = rec.skin, pattern = rec.pattern,
		hull = rec.hull, fuel = rec.fuel,
		items = items, cargo = rec.cargo, held = Hangar.RecordHeldCargo(rec),
		cargoCap = Hangar.RecordCargoCapacity(rec),
		hyperdrive = hyperdrive,
	}
end

-- "10 t diamonds (<mission mark> 10 t), 4 t grain"
local function cargoText(cargo, held)
	local parts = {}
	for name, n in pairs(cargo) do
		local c = Commodities[name]
		local text = string.format("%d t %s", n, c and c:GetName() or name)
		if held and held[name] then
			text = text .. string.format(" (%s %d t)", ui.get_icon_glyph(icons.mission_cargo), held[name])
		end
		table.insert(parts, text)
	end
	table.sort(parts)
	return #parts > 0 and table.concat(parts, ", ") or l.EMPTY_HOLD
end

local function shipCell(name, shipId, hull)
	return string.interp(l.SHIP_CELL, { name = name, type = shipTypeName(shipId), hull = string.format("%d", hull) })
end

-- The bays and what each one costs. One upfront credit covers all bays, so
-- every bay shows the same "paid until" date.
local function drawBays(h)
	local p = Game.player
	local stored = Hangar.StoredShips(h)
	local paidUntil = Hangar.PaidUntil(h)
	local paidText = paidUntil and Format.DateOnly(paidUntil) or l.NOT_PREPAID
	local dash = "—"

	local function dim(text) ui.textColored(colors.fontDim, text) end
	local function selectRow(text, id)
		if ui.selectable(text .. "##hangarRow" .. tostring(id), selected == id, { "SpanAllColumns" }) then
			selected = id
			renaming = nil
		end
	end

	ui.withStyleVars({ CellPadding = widgetSizes.cellPadding }, function()
		if ui.beginTable("hangarBays", 6, { "RowBg", "BordersInnerH", "SizingFixedFit", "NoSavedSettings" }) then
			ui.tableSetupColumn(l.COL_BAY, { "WidthFixed" })
			ui.tableSetupColumn(l.COL_SHIP, { "WidthStretch" })
			ui.tableSetupColumn(l.COL_BAY_FEE, { "WidthFixed" })
			ui.tableSetupColumn(l.COL_PARKING, { "WidthFixed" })
			ui.tableSetupColumn(l.COL_PER_MONTH, { "WidthFixed" })
			ui.tableSetupColumn(l.COL_PAID_UNTIL, { "WidthFixed" })
			ui.tableHeadersRow()

			-- The ship you're flying isn't in a bay and costs nothing
			ui.tableNextRow()
			ui.tableNextColumn()
			selectRow(l.ACTIVE, ACTIVE)
			ui.tableNextColumn()
			ui.text(shipCell(p.shipName, p.shipId, p.hullPercent))
			for _ = 1, 4 do ui.tableNextColumn() dim(dash) end

			local totalBay, totalParking = 0, 0
			for i = 1, h.bays do
				local rec = stored[i]
				local bay, parking = Hangar.BayRent(h, rec)
				bay, parking = math.floor(bay + 0.5), math.floor(parking + 0.5)
				totalBay, totalParking = totalBay + bay, totalParking + parking

				ui.tableNextRow()
				ui.tableNextColumn()
				if rec then
					selectRow(tostring(i), rec.id)
					ui.tableNextColumn()
					ui.text(shipCell(rec.name, rec.shipId, rec.hull))
				else
					dim(tostring(i))
					ui.tableNextColumn()
					ui.icon(icons.bay_empty, Vector2(ui.getTextLineHeight()), colors.fontDim)
					ui.sameLine()
					dim(l.EMPTY_BAY)
				end
				ui.tableNextColumn()
				ui.text(money(bay))
				ui.tableNextColumn()
				if rec then
					ui.text(string.interp(l.PARKING_CELL, { tonnes = Hangar.HullMass(rec.shipId), fee = money(parking) }))
				else
					dim(dash)
				end
				ui.tableNextColumn()
				ui.text(money(bay + parking))
				ui.tableNextColumn()
				ui.text(paidText)
			end

			ui.tableNextRow()
			ui.tableNextColumn()
			ui.tableNextColumn()
			dim(string.interp(l.TOTAL_ROW, { used = #stored, bays = h.bays }))
			ui.tableNextColumn()
			ui.text(money(totalBay))
			ui.tableNextColumn()
			ui.text(money(totalParking))
			ui.tableNextColumn()
			ui.text(money(totalBay + totalParking))
			ui.tableNextColumn()
			ui.text(paidText)
			ui.endTable()
		end
	end)

	if paidUntil then
		ui.textWrapped(string.interp(l.CREDIT_LINE, { credit = money(h.credit), date = paidText }))
	else
		ui.textWrapped(l.NO_CREDIT_LINE)
	end

	-- The next bill covers the days since the lease or the last bill: a part
	-- month the first time, whole calendar months after that
	local billDate = Hangar.NextBill() or Game.time
	local from = Hangar.BilledFrom(h)
	local bill = Hangar.RentSoFar(h, billDate)
	local source = h.credit >= bill and l.FROM_CREDIT
		or h.credit > 0 and l.FROM_CREDIT_AND_ACCOUNT
		or l.FROM_ACCOUNT
	local partMonth = from > Hangar.MonthStart(billDate - 1) + 1
	dim(string.interp(partMonth and l.NEXT_BILL_PART or l.NEXT_BILL_FULL, {
		date = Format.DateOnly(billDate), amount = money(bill), source = source,
		from = Format.DateOnly(from), to = Format.DateOnly(billDate - 1),
	}))
	if partMonth then
		dim(string.interp(l.THEN_MONTHLY, { rent = money(Hangar.MonthlyRent(h)) }))
	end
end

local function drawCompare(value, against, fmt)
	ui.text(fmt(value))
	if against and value ~= against then
		ui.sameLine()
		local better = value > against
		ui.textColored(better and colors.compareBetter or colors.compareWorse,
			string.format("(%s%s)", better and "+" or "", fmt(value - against)))
	end
end

local function selectedView(h)
	if selected ~= ACTIVE then
		for _, rec in ipairs(Hangar.StoredShips(h)) do
			if rec.id == selected then return recordView(rec) end
		end
		selected = ACTIVE
	end
	return activeView()
end

local function drawPreview(view)
	local key = view.shipId .. tostring(view.pattern) .. tostring(selected)
	if spinnerKey ~= key then
		spinnerKey = key
		modelSpinner:setModel(ShipDef[view.shipId].modelName, view.skin, view.pattern)
	end
	local size = ui.getContentRegion()
	modelSpinner:setSize(Vector2(size.x, math.min(size.y, size.x * 0.75)))
	modelSpinner:draw()
end

local function drawDetails(h, view)
	local def = ShipDef[view.shipId]

	if view.rec and renaming == view.rec.id then
		local text, entered = ui.inputText("##hangarRename", renameText, { "EnterReturnsTrue" })
		renameText = text
		ui.sameLine()
		if ui.button(l.SAVE_NAME) or entered then
			Hangar.RenameStored(view.rec, renameText)
			renaming = nil
		end
	else
		ui.withFont(orbiteer.heading, function() ui.text(view.name) end)
	end

	local status = view.active and l.STATUS_ACTIVE or string.interp(l.STATUS_STORED, { date = Format.DateOnly(view.rec.storedAt) })
	ui.textColored(colors.fontDim, string.format("%s · %s · %s", shipTypeName(view.shipId), view.label, status))
	ui.spacing()

	local cmpCargo = not view.active and Game.player.cargo_cap or nil

	if ui.beginTable("hangarShipDetails", 2, { "SizingFixedFit" }) then
		local function row(name, fn)
			ui.tableNextRow()
			ui.tableNextColumn()
			ui.textColored(colors.fontDim, name)
			ui.tableNextColumn()
			fn()
		end
		row(l.HULL, function() ui.textColored(view.hull < 70 and colors.alertYellow or colors.font, string.format("%d%%", view.hull)) end)
		row(l.FUEL, function() ui.textColored(view.fuel < 50 and colors.alertYellow or colors.font, string.format("%d%%", view.fuel)) end)
		row(l.HYPERDRIVE, function()
			if view.hyperdrive then ui.text(view.hyperdrive:GetName()) else ui.textColored(colors.alertYellow, l.NO_HYPERDRIVE) end
		end)
		row(l.CARGO_HOLD, function() drawCompare(view.cargoCap, cmpCargo, function(v) return string.format("%d t", v) end) end)
		row(l.CREW, function()
			local crew = Game.player:CrewNumber()
			local text = string.format("%d–%d", def.minCrew, def.maxCrew)
			if not view.active and (crew > def.maxCrew or crew < def.minCrew) then
				ui.textColored(colors.alertYellow, text .. "  " .. string.interp(l.YOU_HAVE_CREW, { crew = crew }))
			else
				ui.text(text)
			end
		end)
		row(l.CARGO_ABOARD, function() ui.textWrapped(cargoText(view.cargo, view.held)) end)
		row(l.EQUIPMENT, function()
			local names = {}
			for _, item in ipairs(view.items) do table.insert(names, item:GetName()) end
			table.sort(names)
			ui.textWrapped(#names > 0 and table.concat(names, ", ") or l.NONE)
		end)
		ui.endTable()
	end
	ui.spacing()

	if view.active then
		ui.textColored(colors.fontDim, l.THIS_IS_YOUR_SHIP)
		ui.textColored(colors.fontDim, #Hangar.StoredShips(h) > 0 and l.SELECT_A_SHIP or l.NO_SHIPS_STORED)
	else
		local locked = Hangar.IsLocked(h)
		if button(l.FLY_THIS_SHIP, not locked, locked and l.HANGAR_LOCKED or nil) then
			-- Default: everything that fits comes along, mission cargo first
			local aboard = {}
			for _, member in ipairs(crewAboard()) do aboard[member] = true end
			swap = { rec = view.rec, take = Hangar.FitCargo(view.rec), aboard = aboard }
			swapModal:open()
		end
		ui.sameLine()
		if renaming ~= view.rec.id and ui.button(l.RENAME) then
			renaming = view.rec.id
			renameText = view.rec.name
		end
	end
end

--=============================================================================
-- Swap window
--=============================================================================

swapModal = ModalWindow.New("HangarSwap", function(self)
	if not swap then self:close() return end
	local rec = swap.rec
	local player = Game.player

	ui.withFont(orbiteer.heading, function()
		ui.icon(icons.ship_swap, Vector2(ui.getTextLineHeight()), colors.font)
		ui.sameLine()
		ui.text(string.interp(l.SWAP_TITLE, { ship = rec.name }))
	end)
	ui.textColored(colors.fontDim, string.interp(l.SWAP_NOTE, { ship = player.shipName }))
	ui.spacing()

	-- Cargo picker: one row per lot, mission cargo marked and named
	local room = Hangar.RecordFreeSpace(rec)
	local taken = Hangar.TakeTotal(swap.take)

	ui.text(l.CARGO)
	ui.sameLine()
	ui.textColored(taken > room and colors.alertRed or colors.econProfit, string.format("%d / %d t", taken, room))
	if next(rec.cargo) then
		ui.textColored(colors.fontDim, string.interp(l.ALREADY_ABOARD, { ship = rec.name, cargo = cargoText(rec.cargo, Hangar.RecordHeldCargo(rec)) }))
	end

	local lifeSupport = Hangar.RecordLifeSupport(rec)
	local lots = Hangar.CargoLots()
	for _, lot in ipairs(lots) do
		local c = Commodities[lot.name]
		local held = lot.tag ~= CargoManager.ORDINARY
		swap.take[lot.name] = swap.take[lot.name] or {}
		local parts = swap.take[lot.name]

		local name = string.format("%s (%d t)", c:GetName(), lot.count)
		if held then name = ui.get_icon_glyph(icons.mission_cargo) .. " " .. name end
		ui.text(name)
		if held and ui.isItemHovered() then ui.setTooltip(lot.label) end
		ui.sameLine(widgetSizes.sliderWidth * 1.3)
		if (c.life_support or 0) > lifeSupport then
			ui.textColored(colors.fontDim, l.NEEDS_CARGO_LIFE_SUPPORT)
			parts[lot.tag] = 0
		else
			ui.pushItemWidth(widgetSizes.sliderWidth)
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
	else
		if ui.button(l.ALL_THAT_FITS) then
			swap.take = Hangar.FitCargo(rec)
		end
		ui.sameLine()
		if ui.button(l.TAKE_NONE) then
			swap.take = {}
		end
		ui.textColored(colors.fontDim, string.interp(l.REST_STAYS, { ship = player.shipName }))
		for _, lot in ipairs(lots) do
			local parts = swap.take[lot.name]
			if lot.tag ~= CargoManager.ORDINARY and (parts and parts[lot.tag] or 0) < lot.count then
				ui.textColored(colors.alertYellow, l.MISSION_CARGO_STAYS)
				break
			end
		end
	end

	ui.spacing()
	-- Crew picker: ticked people fly on, the others wait in the lounge here
	local def = ShipDef[rec.shipId]
	local ticked = 0
	for _ in pairs(swap.aboard) do ticked = ticked + 1 end
	ui.text(string.interp(l.CREW_FOR, { ship = rec.name, min = def.minCrew, max = def.maxCrew }))
	ui.sameLine()
	local countColor = ticked > def.maxCrew and colors.alertRed or ticked < def.minCrew and colors.alertYellow or colors.econProfit
	ui.textColored(countColor, string.format("%d / %d", ticked, def.maxCrew))

	local lounge = Hangar.GetLoungeAt(player:GetDockedWith())
	local people = {}
	for _, member in ipairs(crewAboard()) do table.insert(people, { member = member, waiting = false }) end
	for _, entry in ipairs(lounge and lounge.crew or {}) do table.insert(people, { member = entry.character, waiting = true }) end

	local inLoungeAfter = 0
	for _, person in ipairs(people) do
		local member = person.member
		if member.player then
			ui.text("     " .. l.YOU_COMMANDER)
		else
			local changed, value = ui.checkbox(member.name .. "##swapcrew" .. tostring(member), swap.aboard[member] == true)
			if changed then swap.aboard[member] = value or nil end
			skillsTooltip(member)
			ui.sameLine()
			ui.textColored(colors.fontDim, bestSkill(member) .. " · " .. (person.waiting and l.IN_LOUNGE or l.ABOARD))
			if person.waiting == (swap.aboard[member] == true) then
				ui.sameLine()
				ui.textColored(colors.alertYellow, person.waiting and l.COMES_ABOARD or l.GOES_TO_LOUNGE)
			end
			if not swap.aboard[member] then inLoungeAfter = inLoungeAfter + 1 end
		end
	end
	if inLoungeAfter > 0 then
		local station = player:GetDockedWith()
		local rate = Hangar.LoungeRate(Hangar.StationFactor(station))
		ui.textColored(colors.fontDim, string.interp(l.LOUNGE_AFTER_SWAP, { count = inLoungeAfter, cost = money(rate * inLoungeAfter) }))
	end

	local blockers, warnings = Hangar.CheckSwap(rec, swap.take, swap.aboard)
	if #blockers > 0 or #warnings > 0 then ui.spacing() end
	for _, b in ipairs(blockers) do ui.textColored(colors.alertRed, b) end
	for _, w in ipairs(warnings) do ui.textColored(colors.alertYellow, w) end

	ui.spacing()
	if ui.button(lc.CANCEL) then
		swap = nil
		self:close()
		return
	end
	ui.sameLine()
	if button(l.SWAP_SHIPS, #blockers == 0, blockers[1]) then
		local ok, result = Hangar.Swap(rec, swap.take, swap.aboard)
		swap = nil
		if ok then
			selected = ACTIVE
			spinnerKey = nil
		end
		self:close()
	end
end)

--=============================================================================
-- Crew lounge sub-tab (works at every station, with or without a hangar)
--=============================================================================

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
			string.interp(l.DISMISS_OWED, { owed = money(owed), limit = money(5e2) }))
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

local function drawLounge(station)
	local player = Game.player
	local def = ShipDef[player.shipId]
	local lounge = Hangar.GetLoungeAt(station)
	local waiting = lounge and lounge.crew or {}
	local crew = player:CrewNumber()
	local rate = Hangar.LoungeRate(Hangar.StationFactor(station))

	ui.withFont(orbiteer.heading, function()
		ui.icon(icons.crew_lounge, Vector2(ui.getTextLineHeight()), colors.font)
		ui.sameLine()
		ui.text(string.interp(l.LOUNGE_AT, { station = station.label }))
	end)

	local debt = Hangar.LoungeDebt()
	if debt > 0.5 then
		ui.textColored(colors.alertRed, string.interp(l.LOUNGE_DEBT_BANNER, { debt = money(debt) }))
		ui.sameLine()
		local canPay = PlayerState.GetMoney() >= math.floor(debt + 0.5)
		if button(string.interp(l.PAY_AMOUNT, { amount = money(debt) }) .. "##lounge", canPay, not canPay and l.NOT_ENOUGH_MONEY or nil) then
			Hangar.PayLoungeDebt()
		end
	end
	ui.spacing()

	local footer = ui.getTextLineHeightWithSpacing() * 3.5
	local size = ui.getContentRegion()
	local half = (size.x - widgetSizes.itemSpacing.x) / 2

	ui.child("loungeAboard", Vector2(half, size.y - footer), function()
		ui.textColored(colors.fontDim, string.interp(l.ABOARD_SHIP, { ship = player.shipName }))
		ui.text(string.interp(l.CREW_LIMITS, { crew = crew, min = def.minCrew, max = def.maxCrew }))
		if crew < def.minCrew then
			ui.textColored(colors.alertYellow, string.interp(l.CANT_LAUNCH_FEWER, { min = def.minCrew }))
		end
		ui.separator()
		local blocked = Hangar.LoungeBlocked()
		for _, member in ipairs(crewAboard()) do
			if member.player then
				ui.text(l.YOU_COMMANDER)
			else
				if button(ui.get_icon_glyph(icons.forward) .. "##tolounge" .. tostring(member), not blocked, blocked or l.SEND_TO_LOUNGE) then
					Hangar.SendToLounge(member)
				end
				ui.sameLine()
				ui.text(member.name)
				skillsTooltip(member)
				ui.sameLine()
				local wage = member.contract and member.contract.wage or 0
				ui.textColored(colors.fontDim, string.format("%s · %s", bestSkill(member), string.interp(l.WAGE_PER_WEEK, { wage = money(wage) })))
			end
		end
	end)
	ui.sameLine()
	ui.child("loungeWaiting", Vector2(half, size.y - footer), function()
		ui.textColored(colors.fontDim, l.IN_LOUNGE_HERE)
		ui.text(string.interp(l.WAITING_COUNT, { count = #waiting }))
		ui.separator()
		if #waiting == 0 then
			ui.textColored(colors.fontDim, l.NOBODY_WAITING)
		end
		local full = crew >= def.maxCrew
		for _, entry in ipairs(waiting) do
			local member = entry.character
			local tooltip = full and string.interp(l.SHIP_FULL, { ship = player.shipName, max = def.maxCrew }) or l.BRING_ABOARD
			if button(ui.get_icon_glyph(icons.backward) .. "##fromlounge" .. tostring(member), not full, tooltip) then
				Hangar.TakeFromLounge(member)
			end
			ui.sameLine()
			ui.text(member.name)
			skillsTooltip(member)
			ui.sameLine()
			ui.textColored(colors.fontDim, string.format("%s · %s", bestSkill(member),
				string.interp(l.WAITING_SINCE, { date = Format.DateOnly(entry.since) })))
			ui.sameLine()
			if ui.button(lc.DISMISS .. "##dismiss" .. tostring(member), Vector2(0, 0), ui.theme.buttonColors.dark) then
				dismissing = member
				dismissModal:open()
			end
		end
	end)

	ui.separator()
	ui.textWrapped(string.interp(l.LOUNGE_RATE_LINE, { rate = money(rate), now = money(rate * #waiting) }))
	local elsewhere = {}
	for _, other in ipairs(Hangar.GetLounges()) do
		if not lounge or other.key ~= lounge.key then
			table.insert(elsewhere, string.interp(l.WAITING_AT, { count = #other.crew, station = other.station }))
		end
	end
	if #elsewhere > 0 then
		ui.textColored(colors.fontDim, l.ALSO_WAITING_ELSEWHERE .. " " .. table.concat(elsewhere, " · "))
	end
end

--=============================================================================
-- Tab
--=============================================================================

StationView:registerView({
	id = "hangarView",
	name = l.HANGAR,
	icon = icons.hangar,
	showView = true,
	draw = function()
		local station = Game.player:GetDockedWith()
		if not station then return end
		local h = Hangar.GetHangarAt(station)

		local function drawShips()
			if not h then
				drawLeaseForm(station)
				return
			end

			if not drawHeader(h) then return end
			ui.spacing()
			drawBays(h)
			ui.separator()

			local view = selectedView(h)
			local previewWidth = ui.getContentRegion().x * PREVIEW_WIDTH
			ui.child("hangarPreview", Vector2(previewWidth, 0), function()
				drawPreview(view)
			end)
			ui.sameLine()
			ui.child("hangarDetails", Vector2(0, 0), function()
				drawDetails(h, view)
			end)
		end

		ui.withFont(pionillium.body, function()
			ui.withStyleVars({ ItemSpacing = widgetSizes.itemSpacing }, function()
				ui.tabBar("hangarTabs", {
					{ ui.get_icon_glyph(icons.hangar) .. " " .. l.SHIPS, drawShips },
					{ ui.get_icon_glyph(icons.crew_lounge) .. " " .. l.CREW_LOUNGE, function() drawLounge(station) end },
				})
			end)
		end)
	end,
	refresh = function()
		selected = ACTIVE
		renaming = nil
		showPrepay = false
		spinnerKey = nil
	end,
	debugReload = function()
		package.reimport()
	end
})
