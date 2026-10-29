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
local Pickers = require 'pigui.libs.hangar-pickers'
local Transfers = require 'pigui.libs.hangar-transfers'

local ui = require 'pigui'

local l = Lang.GetResource("module-hangar")
local lc = Lang.GetResource("ui-core")
local le = Lang.GetResource("equipment-core")
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
local equipShip = ACTIVE         -- Equipment sub-tab: ACTIVE or a stored ship's record id
local equipSlot = nil            -- the slot picked to fit the next item into

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

local bestSkill = Pickers.bestSkill
local skillsTooltip = Pickers.skillsTooltip
local crewAboard = Pickers.crewAboard

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

	local others = Hangar.GetHangars()
	if #others > 0 then
		ui.spacing()
		ui.text(l.YOUR_OTHER_HANGARS)
		for _, h in ipairs(others) do
			ui.textColored(colors.fontDim, "    " .. string.interp(l.OTHER_HANGAR, { station = h.station, used = h.bays - Hangar.FreeBays(h), bays = h.bays }))
			Pickers.mapButton("otherHangar" .. h.key, h.path)
		end
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
	ui.sameLine()
	local canBring = Hangar.FreeBays(h) > 0 and not Hangar.IsLocked(h)
	if button(Transfers.glyph(icons.ship_receive, l.TRANSFER_FROM) .. "##shipFrom", canBring,
			canBring and l.TRANSFER_SHIP_TOOLTIP or Hangar.IsLocked(h) and l.HANGAR_LOCKED or l.NO_FREE_BAY_HERE) then
		Transfers.ShipFrom(h)
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
					local t = Hangar.TransferOf(rec)
					if t then
						ui.textColored(colors.fontDim, ui.get_icon_glyph(icons.in_transit) .. " " .. shipCell(rec.name, rec.shipId, rec.hull)
							.. " · " .. Transfers.StatusText(t))
					else
						ui.text(shipCell(rec.name, rec.shipId, rec.hull))
					end
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

	local transfer = view.rec and Hangar.TransferOf(view.rec)
	local status = view.active and l.STATUS_ACTIVE
		or transfer and Transfers.StatusText(transfer)
		or string.interp(l.STATUS_STORED, { date = Format.DateOnly(view.rec.storedAt) })
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
	elseif transfer then
		ui.textColored(colors.fontDim, ui.get_icon_glyph(icons.in_transit) .. " " .. l.SHIP_ON_ITS_WAY)
		if ui.button(Transfers.glyph(icons.transfer_cancel, l.CANCEL_DELIVERY)) then
			Transfers.Cancel(transfer)
		end
	else
		local locked = Hangar.IsLocked(h)
		if button(l.FLY_THIS_SHIP, not locked, locked and l.HANGAR_LOCKED or nil) then
			-- Default: everything that fits comes along, mission cargo first
			swap = { rec = view.rec, take = Hangar.FitCargo(view.rec), aboard = Pickers.everyoneAboard() }
			swapModal:open()
		end
		ui.sameLine()
		if button(Transfers.glyph(icons.ship_send, l.SEND_TO), not locked, locked and l.HANGAR_LOCKED or l.SEND_SHIP_TOOLTIP) then
			Transfers.SendShip(view.rec)
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

	if next(rec.cargo) then
		ui.textColored(colors.fontDim, string.interp(l.ALREADY_ABOARD, { ship = rec.name, cargo = cargoText(rec.cargo, Hangar.RecordHeldCargo(rec)) }))
	end
	Pickers.drawCargo(swap, Hangar.RecordFreeSpace(rec), Hangar.RecordLifeSupport(rec), player.shipName)

	ui.spacing()
	Pickers.drawCrew(swap, ShipDef[rec.shipId], rec.name)

	local blockers, warnings = Hangar.CheckSwap(rec, swap.take, swap.aboard)
	Pickers.drawProblems(blockers, warnings)

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
-- Equipment sub-tab (the hangar store)
--=============================================================================

-- A slot's name, as the Equipment Market shows it
local function slotName(slot)
	if slot.i18n_key then
		return Lang.GetResource(slot.i18n_res)[slot.i18n_key]
	end
	local base = slot.type:match("([%w_-]+)%.?")
	return le[(slot.hardpoint and "HARDPOINT_" or "SLOT_") .. base:upper()] or slot.type
end

-- Extra state an item keeps: hyperdrive fuel, missiles in a rack
local function itemNote(item)
	local parts = { string.format("%s m³", ui.Format.Number(item.volume, 1)) }
	if item.storedFuel and item.GetMaxFuel then
		table.insert(parts, string.interp(l.DRIVE_FUEL, { fuel = string.format("%.1f", item.storedFuel) }))
	end
	return table.concat(parts, " · ")
end

-- Selling an item from the store asks first
local selling = nil ---@type { h: Hangar.Lease, item: EquipType }?

local sellModal = ModalWindow.New("HangarSellItem", function(self)
	local sale = selling
	local station = Game.player:GetDockedWith()
	if not sale or not station then self:close() return end

	ui.withFont(orbiteer.heading, function()
		ui.text(string.interp(l.SELL_TITLE, { item = sale.item:GetName() }))
	end)
	ui.text(string.interp(l.SELL_TEXT, { item = sale.item:GetName(), price = money(Hangar.SellPrice(station, sale.item)), station = station.label }))
	ui.spacing()

	if ui.button(lc.CANCEL) then
		selling = nil
		self:close()
	end
	ui.sameLine()
	if ui.button(l.SELL .. "##confirmSell") then
		Hangar.SellFromStore(sale.h, sale.item)
		selling = nil
		self:close()
	end
end)

local function drawEquipment(station, h)
	ui.withFont(orbiteer.heading, function()
		ui.icon(icons.hangar, Vector2(ui.getTextLineHeight()), colors.font)
		ui.sameLine()
		ui.text(string.interp(l.HANGAR_AT, { station = h.station }))
	end)
	if Hangar.IsLocked(h) then
		iconText(icons.lease_locked, colors.alertRed, l.HANGAR_LOCKED)
	end

	-- the ship being fitted: the active one or one stored here
	local stored = Hangar.StoredShips(h)
	local target, targetName = Hangar.ACTIVE, Game.player.shipName
	local names, ids = { string.interp(l.SHIP_ACTIVE_OPTION, { name = Game.player.shipName }) }, { ACTIVE }
	for _, rec in ipairs(stored) do
		if not rec.transit then
			table.insert(names, string.interp(l.SHIP_STORED_OPTION, { name = rec.name }))
			table.insert(ids, rec.id)
			if rec.id == equipShip then target, targetName = rec, rec.name end
		end
	end
	if target == Hangar.ACTIVE then equipShip = ACTIVE end

	local entries, loose, used, capacity = Hangar.EquipmentOf(target)
	local footer = ui.getTextLineHeightWithSpacing() * 2.5
	local size = ui.getContentRegion()
	local half = (size.x - widgetSizes.itemSpacing.x) / 2

	ui.child("equipFitted", Vector2(half, size.y - footer), function()
		ui.text(l.FITTED_ON)
		ui.sameLine()
		ui.pushItemWidth(half * 0.5)
		local current = 0
		for i, id in ipairs(ids) do if id == equipShip then current = i - 1 end end
		local changed, index = ui.combo("##equipShip", current, names)
		if changed then equipShip = ids[index + 1] equipSlot = nil end
		ui.popItemWidth()
		ui.sameLine()
		if button(l.UNFIT_ALL, not Hangar.IsLocked(h), Hangar.IsLocked(h) and l.HANGAR_LOCKED or l.UNFIT_ALL_TOOLTIP) then
			Hangar.UnfitAll(h, target)
		end
		ui.textColored(colors.fontDim, string.interp(l.EQUIP_VOLUME, {
			used = ui.Format.Number(used, 1), capacity = ui.Format.Number(capacity, 1) }))
		ui.separator()

		local function itemRow(id, item)
			local ok, why = Hangar.CanUnfit(h, target, item)
			if button(ui.get_icon_glyph(icons.forward) .. "##unfit" .. tostring(item), ok, ok and l.UNFIT_TOOLTIP or why) then
				Hangar.Unfit(h, target, item)
				equipSlot = nil
			end
			ui.sameLine()
			ui.text(item:GetName())
			ui.sameLine()
			ui.textColored(colors.fontDim, itemNote(item))
			-- while locked the banner above says why
			if not ok and why and not Hangar.IsLocked(h) then
				ui.textColored(colors.fontDim, "      " .. why)
			end
		end

		for _, e in ipairs(entries) do
			local indent = string.rep("    ", e.depth)
			if e.item then
				ui.textColored(colors.fontDim, indent .. slotName(e.slot))
				ui.sameLine(ui.getContentRegion().x * 0.3)
				itemRow(e.id, e.item)
			else
				local label = indent .. slotName(e.slot) .. "##slot" .. e.id
				if ui.selectable(label, equipSlot == e.id, {}) then
					equipSlot = equipSlot ~= e.id and e.id or nil
				end
				ui.sameLine(ui.getContentRegion().x * 0.3)
				ui.textColored(colors.fontDim, l.EMPTY_SLOT)
			end
		end
		if #loose > 0 then
			ui.spacing()
			ui.textColored(colors.fontDim, l.WITHOUT_A_SLOT)
			for _, e in ipairs(loose) do itemRow(e.key, e.item) end
		end
	end)
	ui.sameLine()
	ui.child("equipStore", Vector2(half, size.y - footer), function()
		local volume = 0
		for _, item in ipairs(h.store) do volume = volume + item.volume end
		ui.text(string.interp(l.STORE_AT, { station = h.station }))
		ui.textColored(colors.fontDim, string.interp(l.STORE_CONTENTS, { count = #h.store, volume = ui.Format.Number(volume, 1) }))
		local locked = Hangar.IsLocked(h)
		if button(Transfers.glyph(icons.ship_send, l.SEND_TO) .. "##sendEquip", not locked and #h.store > 0,
				locked and l.HANGAR_LOCKED or l.SEND_EQUIPMENT_TOOLTIP) then
			Transfers.SendEquipment(h)
		end
		ui.sameLine()
		if button(Transfers.glyph(icons.ship_receive, l.TRANSFER_FROM) .. "##equipFrom", not locked,
				locked and l.HANGAR_LOCKED or l.TRANSFER_EQUIPMENT_TOOLTIP) then
			Transfers.EquipmentFrom(h)
		end
		ui.separator()
		for _, t in ipairs(Hangar.TransfersTo(h.key, "equipment")) do
			local names = {}
			for _, item in ipairs(t.items) do table.insert(names, item:GetName()) end
			Transfers.DrawIncoming(t, table.concat(names, ", "))
		end
		if #h.store == 0 then
			ui.textColored(colors.fontDim, l.STORE_EMPTY)
		end
		-- a copy: fitting changes the store
		for _, item in ipairs(table.copy(h.store)) do
			local place, why = Hangar.FitPlace(h, target, item, equipSlot)
			local tooltip = place and string.interp(l.FIT_TOOLTIP, { ship = targetName }) or why
			if button(ui.get_icon_glyph(icons.backward) .. "##fit" .. tostring(item), place ~= nil, tooltip) then
				Hangar.Fit(h, target, item, equipSlot)
				equipSlot = nil
			end
			ui.sameLine()
			local locked = Hangar.IsLocked(h)
			if button(l.SELL .. "##sell" .. tostring(item), not locked, locked and l.HANGAR_LOCKED
					or string.interp(l.SELL_TOOLTIP, { price = money(Hangar.SellPrice(station, item)) })) then
				selling = { h = h, item = item }
				sellModal:open()
			end
			ui.sameLine()
			ui.text(item:GetName())
			ui.sameLine()
			ui.textColored(colors.fontDim, itemNote(item))
			if not place and not Hangar.IsLocked(h) then
				ui.textColored(colors.fontDim, "      " .. why)
			end
		end
	end)

	ui.separator()
	ui.textColored(colors.fontDim, l.STORE_HELP)
end

--=============================================================================
-- Cargo sub-tab (the hangar's cargo store)
--=============================================================================

-- Buttons moving 1, 10 or all tons of a lot; returns the amount clicked
local function amountButtons(id, glyph, count, enabled, tooltip)
	local clicked = nil
	for _, n in ipairs({ 1, 10 }) do
		if count >= n then
			if button(glyph .. " " .. n .. "##" .. id .. n, enabled, tooltip) then clicked = n end
			ui.sameLine()
		end
	end
	if button(glyph .. " " .. l.ALL .. "##" .. id .. "all", enabled, tooltip) then clicked = count end
	return clicked
end

local function lotName(lot)
	local c = Commodities[lot.name]
	local name = string.format("%d t %s", lot.count, c and c:GetName() or lot.name)
	if lot.tag ~= CargoManager.ORDINARY then
		name = ui.get_icon_glyph(icons.mission_cargo) .. " " .. name
	end
	return name
end

local function drawCargoStore(station, h)
	ui.withFont(orbiteer.heading, function()
		ui.icon(icons.hangar, Vector2(ui.getTextLineHeight()), colors.font)
		ui.sameLine()
		ui.text(string.interp(l.HANGAR_AT, { station = h.station }))
	end)
	local locked = Hangar.IsLocked(h)
	if locked then
		iconText(icons.lease_locked, colors.alertRed, l.HANGAR_LOCKED)
	end

	local cargoMgr = Game.player:GetComponent('CargoManager')
	local footer = ui.getTextLineHeightWithSpacing() * 2
	local size = ui.getContentRegion()
	local half = (size.x - widgetSizes.itemSpacing.x) / 2

	ui.child("cargoAboard", Vector2(half, size.y - footer), function()
		ui.text(string.interp(l.ABOARD_SHIP, { ship = Game.player.shipName }))
		ui.textColored(colors.fontDim, string.interp(l.HOLD_USE, {
			used = math.floor(cargoMgr:GetUsedSpace()), total = math.floor(cargoMgr:GetTotalSpace()) }))
		ui.separator()
		local lots = Hangar.CargoLots()
		if #lots == 0 then ui.textColored(colors.fontDim, l.NO_CARGO_ABOARD) end
		for _, lot in ipairs(lots) do
			local n = amountButtons("unload" .. lot.name .. tostring(lot.tag), ui.get_icon_glyph(icons.forward),
				lot.count, not locked, locked and l.HANGAR_LOCKED or l.UNLOAD_TOOLTIP)
			if n then Hangar.UnloadCargo(h, lot.name, lot.tag, n) end
			ui.sameLine()
			ui.text(lotName(lot))
			if lot.label then
				ui.sameLine()
				ui.textColored(colors.fontDim, lot.label)
			end
		end
	end)
	ui.sameLine()
	ui.child("cargoStore", Vector2(half, size.y - footer), function()
		ui.text(string.interp(l.CARGO_STORE_AT, { station = h.station }))
		ui.textColored(colors.fontDim, string.interp(l.CARGO_STORE_TOTAL, { total = Hangar.StoreCargoTotal(h) }))
		ui.separator()
		local lots = Hangar.StoreCargoLots(h)
		if #lots == 0 then ui.textColored(colors.fontDim, l.CARGO_STORE_EMPTY) end
		for _, lot in ipairs(lots) do
			local room, why = Hangar.CanLoadCargo(h, lot.name, lot.tag)
			local n = amountButtons("load" .. lot.name .. tostring(lot.tag), ui.get_icon_glyph(icons.backward),
				lot.count, room > 0, room > 0 and l.LOAD_TOOLTIP or why)
			if n then Hangar.LoadCargo(h, lot.name, lot.tag, n) end
			ui.sameLine()
			ui.text(lotName(lot))
			if lot.label then
				ui.sameLine()
				ui.textColored(colors.fontDim, lot.label)
			end
			if room == 0 and why and not locked then
				ui.textColored(colors.fontDim, "      " .. why)
			end
		end
	end)

	ui.separator()
	ui.textColored(colors.fontDim, l.CARGO_STORE_HELP)
end

--=============================================================================
-- Crew lounge sub-tab (works at every station, with or without a hangar)
--=============================================================================


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
			if ui.button(ui.get_icon_glyph(icons.ship_send) .. "##sendcrew" .. tostring(member), Vector2(0, 0),
					ui.theme.buttonColors.dark, l.SEND_CREW_TOOLTIP) then
				Transfers.SendCrew(Hangar.PlaceOf(station), member)
			end
			ui.sameLine()
			if ui.button(lc.DISMISS .. "##dismiss" .. tostring(member), Vector2(0, 0), ui.theme.buttonColors.dark) then
				Pickers.ConfirmDismiss(member)
			end
		end
		for _, t in ipairs(Hangar.TransfersTo(Hangar.StationKey(station), "crew")) do
			Transfers.DrawIncoming(t, Transfers.WhatText(t))
		end
		ui.spacing()
		if ui.button(Transfers.glyph(icons.ship_receive, l.TRANSFER_FROM) .. "##crewFrom", Vector2(0, 0), nil, l.TRANSFER_CREW_TOOLTIP) then
			Transfers.CrewFrom(Hangar.PlaceOf(station))
		end
	end)

	ui.separator()
	ui.textWrapped(string.interp(l.LOUNGE_RATE_LINE, { rate = money(rate), now = money(rate * #waiting) }))
	local first = true
	for _, other in ipairs(Hangar.GetLounges()) do
		if not lounge or other.key ~= lounge.key then
			if first then
				ui.textColored(colors.fontDim, l.ALSO_WAITING_ELSEWHERE)
				first = false
			end
			ui.sameLine()
			ui.textColored(colors.fontDim, string.interp(l.WAITING_AT, { count = #other.crew, station = other.station }))
			Pickers.mapButton("lounge" .. other.key, other.path)
		end
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
					{ ui.get_icon_glyph(icons.equipment_store) .. " " .. l.EQUIPMENT_TAB, function()
						-- the store is part of the hangar
						if h then drawEquipment(station, h) else drawLeaseForm(station) end
					end },
					{ ui.get_icon_glyph(icons.cargo_crate) .. " " .. l.CARGO_TAB, function()
						if h then drawCargoStore(station, h) else drawLeaseForm(station) end
					end },
					{ ui.get_icon_glyph(icons.crew_lounge) .. " " .. l.CREW_LOUNGE, function() drawLounge(station) end },
				})
			end)
		end)
	end,
	refresh = function()
		selected = ACTIVE
		equipShip = ACTIVE
		equipSlot = nil
		renaming = nil
		showPrepay = false
		spinnerKey = nil
	end,
	debugReload = function()
		package.reimport()
	end
})
