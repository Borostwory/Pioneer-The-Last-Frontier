-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- The Last Frontier: the Info screen's Fleet tab. Everything you own away
-- from your ship, from anywhere: ships, hangars with their stores, and crew
-- waiting in lounges or travelling. Leases can be paid and ended, and
-- deliveries booked or cancelled, without docking there.

local Game = require 'Game'
local Lang = require 'Lang'
local Format = require 'Format'
local ShipDef = require 'ShipDef'
local Commodities = require 'Commodities'
local PlayerState = require 'PlayerState'
local InfoView = require 'pigui.views.info-view'
local ModalWindow = require 'pigui.libs.modal-win'
local Hangar = require 'modules.Hangar.Hangar'
local Pickers = require 'pigui.libs.hangar-pickers'
local Transfers = require 'pigui.libs.hangar-transfers'

local ui = require 'pigui'

local l = Lang.GetResource("module-hangar")
local lc = Lang.GetResource("ui-core")
local ls = Lang.GetResource("ships")

local colors = ui.theme.colors
local icons = ui.theme.icons
local pionillium = ui.fonts.pionillium
local orbiteer = ui.fonts.orbiteer

local Vector2 = _G.Vector2

local sizes = ui.rescaleUI({
	itemSpacing = Vector2(8, 6),
	cellPadding = Vector2(10, 3),
}, Vector2(1600, 900))

local selectedShip = nil   -- record id, or "active"
local selectedHangar = nil -- hangar key
local selectedCrew = nil   -- Character

local money = Pickers.money

local function button(label, enabled, tooltip)
	local variant = not enabled and ui.theme.buttonColors.disabled or nil
	return ui.button(label, Vector2(0, 0), variant, tooltip) and enabled
end

local function shipType(shipId)
	local def = ShipDef[shipId]
	return def and ls[def.i18n_key] or shipId
end

-- "Lave Station, Lave"
local function placeName(path, station)
	local ok, system = pcall(function() return path:GetStarSystem().name end)
	return ok and string.format("%s, %s", station, system) or station
end

-- The hangar where the player is docked, if any
local function hangarHere()
	return Hangar.GetHangarAt(Game.player:GetDockedWith())
end

local function showOnMap(path)
	Game.sectorView:SwitchToPath(path:SystemOnly())
	Game.SetView("SectorView")
end

local function section(title, icon)
	ui.spacing()
	ui.withFont(orbiteer.heading, function()
		ui.icon(icon, Vector2(ui.getTextLineHeight()), colors.font)
		ui.sameLine()
		ui.text(title)
	end)
end

local function beginTable(id, columns)
	return ui.beginTable(id, #columns, { "RowBg", "BordersInnerH", "SizingFixedFit", "NoSavedSettings" }) and (function()
		for i, name in ipairs(columns) do
			ui.tableSetupColumn(name, { i == 1 and "WidthStretch" or "WidthFixed" })
		end
		ui.tableHeadersRow()
		return true
	end)()
end

local function cells(...)
	for _, v in ipairs({ ... }) do
		ui.tableNextColumn()
		if type(v) == "table" then ui.textColored(v.color, v.text) else ui.text(tostring(v)) end
	end
end

local function dim(text) return { text = text, color = colors.fontDim } end

--=============================================================================
-- Paying rent upfront from afar
--=============================================================================

local prepay = nil ---@type { h: Hangar.Lease, months: integer }?

local prepayModal = Pickers.NewModal("FleetPrepay", function(self)
	if not prepay then self:close() return end
	local h = prepay.h
	ui.withFont(orbiteer.heading, function()
		ui.text(string.interp(l.PREPAY_TITLE, { station = h.station }))
	end)
	ui.text(string.interp(l.PREPAY_RENT_NOW, { rent = money(Hangar.MonthlyRent(h)) }))
	ui.pushItemWidth(ui.getTextLineHeight() * 12)
	prepay.months = ui.sliderInt("##fleetPrepayMonths", prepay.months, 1, 60, l.MONTHS_FORMAT)
	ui.popItemWidth()
	for _, n in ipairs({ 1, 3, 6, 12, 24 }) do
		ui.sameLine()
		if ui.button(tostring(n) .. "##fleetPrepay" .. n, Vector2(0, 0), ui.theme.buttonColors.dark) then prepay.months = n end
	end
	local value, cost, discount = Hangar.QuotePrepay(Hangar.MonthlyRent(h), prepay.months)
	ui.text(discount > 0
		and string.interp(l.PRICE_WITH_DISCOUNT, { value = money(value), discount = string.format("%d", discount * 100), cost = money(cost) })
		or money(value))
	ui.spacing()
	if ui.button(lc.CANCEL .. "##fleetPrepayCancel") then
		prepay = nil
		self:close()
		return
	end
	ui.sameLine()
	local canPay = PlayerState.GetMoney() >= cost
	if button(l.PAY .. "##fleetPrepayPay", canPay, not canPay and l.NOT_ENOUGH_MONEY or nil) then
		Hangar.Prepay(h, prepay.months)
		prepay = nil
		self:close()
	end
end)

--=============================================================================
-- Ships
--=============================================================================

local function drawShips()
	section(l.FLEET_SHIPS, icons.ship_swap)
	local player = Game.player
	local here = hangarHere()

	ui.withStyleVars({ CellPadding = sizes.cellPadding }, function()
		if not beginTable("fleetShips", { l.COL_SHIP, l.COL_TYPE, l.COL_LOCATION, l.COL_STATUS, l.HULL }) then return end

		-- the ship you're flying
		ui.tableNextRow()
		ui.tableNextColumn()
		if ui.selectable(player.shipName .. "##fleetActive", selectedShip == "active", { "SpanAllColumns" }) then selectedShip = "active" end
		local station = player:GetDockedWith()
		local where = station and placeName(station.path, station.label) or (Game.system and Game.system.name or l.IN_HYPERSPACE)
		cells(shipType(player.shipId), where, { text = l.STATUS_FLYING, color = colors.econProfit }, string.format("%d%%", player.hullPercent))

		for _, rec in ipairs(Hangar.GetAllShips()) do
			local h = Hangar.GetHangar(rec.hangar)
			local t = Hangar.TransferOf(rec)
			ui.tableNextRow()
			ui.tableNextColumn()
			local label = (t and ui.get_icon_glyph(icons.in_transit) .. " " or "") .. rec.name
			if ui.selectable(label .. "##fleetShip" .. rec.id, selectedShip == rec.id, { "SpanAllColumns" }) then selectedShip = rec.id end
			local status = t and dim(Transfers.StatusText(t))
				or h and Hangar.IsLocked(h) and { text = l.STATUS_LOCKED, color = colors.alertRed }
				or dim(l.STATUS_STORED_SHORT)
			cells(shipType(rec.shipId), h and placeName(h.path, h.station) or "?", status, string.format("%d%%", rec.hull))
		end
		ui.endTable()
	end)

	-- what can be done with the selected ship
	local rec = nil
	for _, r in ipairs(Hangar.GetAllShips()) do if r.id == selectedShip then rec = r end end
	if not rec then return end
	local h = Hangar.GetHangar(rec.hangar)
	local t = Hangar.TransferOf(rec)
	if t then
		if ui.button(Transfers.glyph(icons.transfer_cancel, l.CANCEL_DELIVERY) .. "##fleetCancelShip") then Transfers.Cancel(t) end
	else
		local locked = h and Hangar.IsLocked(h)
		if button(Transfers.glyph(icons.ship_send, l.SEND_TO) .. "##fleetSendShip", not locked, locked and l.HANGAR_LOCKED or l.SEND_SHIP_TOOLTIP) then
			Transfers.SendShip(rec)
		end
		if here and here ~= h then
			ui.sameLine()
			local why = Hangar.SendShipBlocked(rec, here)
			if button(Transfers.glyph(icons.ship_receive, l.TRANSFER_HERE) .. "##fleetShipHere", not why, why or l.TRANSFER_HERE_TOOLTIP) then
				Transfers.SendShip(rec, here.key)
			end
		end
	end
	if h then
		ui.sameLine()
		if ui.button(ui.get_icon_glyph(icons.sector_map) .. " " .. l.SHOW_ON_MAP .. "##fleetShipMap") then showOnMap(h.path) end
	end
end

--=============================================================================
-- Hangars
--=============================================================================

local function drawHangars()
	section(l.FLEET_HANGARS, icons.hangar)
	local hangars = Hangar.GetHangars()
	if #hangars == 0 then
		ui.textColored(colors.fontDim, l.NO_HANGARS)
		return
	end

	ui.withStyleVars({ CellPadding = sizes.cellPadding }, function()
		if not beginTable("fleetHangars", { l.COL_STATION, l.BAYS, l.COL_STORE, l.COL_PAID_UNTIL, l.COL_PER_MONTH }) then return end
		for _, h in ipairs(hangars) do
			ui.tableNextRow()
			ui.tableNextColumn()
			if ui.selectable(placeName(h.path, h.station) .. "##fleetHangar" .. h.key, selectedHangar == h.key, { "SpanAllColumns" }) then
				selectedHangar = h.key
			end
			local used = h.bays - Hangar.FreeBays(h)
			local store = string.interp(l.STORE_SUMMARY, { items = #h.store, tons = Hangar.StoreCargoTotal(h) })
			local paid
			if Hangar.IsLocked(h) then
				paid = { text = string.interp(l.UNPAID_SHORT, { debt = money(h.debt) }), color = colors.alertRed }
			else
				local untilT = Hangar.PaidUntil(h)
				paid = untilT and Format.DateOnly(untilT) or dim(l.NOT_PREPAID)
			end
			cells(string.format("%d / %d", used, h.bays), store, paid, money(Hangar.MonthlyRent(h)))
		end
		ui.endTable()
	end)

	local h = selectedHangar and Hangar.GetHangar(selectedHangar)
	if not h then return end
	if Hangar.IsLocked(h) then
		local debt = math.floor(h.debt + 0.5)
		local canPay = PlayerState.GetMoney() >= debt
		if button(string.interp(l.PAY_AMOUNT, { amount = money(debt) }) .. "##fleetPayDebt", canPay, not canPay and l.NOT_ENOUGH_MONEY or nil) then
			Hangar.PayDebt(h)
		end
		ui.sameLine()
	end
	if ui.button(l.PAY_UPFRONT_BUTTON .. "##fleetPrepay") then
		prepay = { h = h, months = 6 }
		prepayModal:open()
	end
	ui.sameLine()
	local canEnd = Hangar.CanEndLease(h)
	if button(l.END_LEASE .. "##fleetEnd", canEnd, not canEnd and l.END_LEASE_NEEDS_EMPTY or nil) then
		Hangar.EndLease(h)
		selectedHangar = nil
		return
	end
	ui.sameLine()
	local locked = Hangar.IsLocked(h)
	if button(Transfers.glyph(icons.ship_send, l.SEND_EQUIPMENT_BUTTON) .. "##fleetSendEquip", not locked and #h.store > 0,
			locked and l.HANGAR_LOCKED or l.SEND_EQUIPMENT_TOOLTIP) then
		Transfers.SendEquipment(h)
	end
	local here = hangarHere()
	if here and here ~= h then
		ui.sameLine()
		local ok = not locked and not Hangar.IsLocked(here) and #h.store > 0
		if button(Transfers.glyph(icons.ship_receive, l.EQUIPMENT_HERE) .. "##fleetEquipHere", ok, l.TRANSFER_EQUIPMENT_TOOLTIP) then
			Transfers.SendEquipment(h, here.key)
		end
	end
	ui.sameLine()
	if ui.button(ui.get_icon_glyph(icons.sector_map) .. " " .. l.SHOW_ON_MAP .. "##fleetHangarMap") then showOnMap(h.path) end

	-- what the hangar keeps
	local names = {}
	for _, item in ipairs(h.store) do table.insert(names, item:GetName()) end
	table.sort(names)
	ui.textColored(colors.fontDim, string.interp(l.FLEET_STORE_LINE, { items = #names > 0 and table.concat(names, ", ") or l.NONE }))
	for _, lot in ipairs(Hangar.StoreCargoLots(h)) do
		local c = Commodities[lot.name]
		local name = string.format("%d t %s", lot.count, c and c:GetName() or lot.name)
		ui.textColored(colors.fontDim, "    " .. name .. (lot.label and (" · " .. lot.label) or ""))
	end
	for _, t in ipairs(Hangar.TransfersTo(h.key)) do
		if t.kind ~= "crew" then
			Transfers.DrawIncoming(t, Transfers.WhatText(t))
		end
	end
end

--=============================================================================
-- Crew in lounges and travelling
--=============================================================================

local function drawCrew()
	section(l.FLEET_CREW, icons.crew_lounge)
	local rows = {}
	for _, lounge in ipairs(Hangar.GetLounges()) do
		for _, entry in ipairs(lounge.crew) do
			table.insert(rows, { c = entry.character, lounge = lounge, entry = entry })
		end
	end
	for _, t in ipairs(Hangar.GetTransfers()) do
		if t.kind == "crew" then
			for _, c in ipairs(t.crew) do table.insert(rows, { c = c, transfer = t }) end
		end
	end
	if #rows == 0 then
		ui.textColored(colors.fontDim, l.NOBODY_WAITING_ANYWHERE)
		return
	end

	local selectedRow = nil
	ui.withStyleVars({ CellPadding = sizes.cellPadding }, function()
		if not beginTable("fleetCrew", { l.COL_NAME, l.COL_SKILL, l.COL_STATION, l.COL_STATUS, l.COL_COST }) then return end
		for _, row in ipairs(rows) do
			local c = row.c
			if c == selectedCrew then selectedRow = row end
			ui.tableNextRow()
			ui.tableNextColumn()
			local label = (row.transfer and ui.get_icon_glyph(icons.in_transit) .. " " or "") .. c.name
			if ui.selectable(label .. "##fleetCrew" .. tostring(c), selectedCrew == c, { "SpanAllColumns" }) then
				selectedCrew = c
				selectedRow = row
			end
			local halfWage = c.contract and math.floor(c.contract.wage / 2 + 0.5) or 0
			if row.transfer then
				cells(Pickers.bestSkill(c), row.transfer.to.station, dim(Transfers.StatusText(row.transfer)),
					string.interp(l.COST_TRAVELLING, { wage = money(halfWage) }))
			else
				local rate = Hangar.LoungeRate(row.lounge.factor)
				cells(Pickers.bestSkill(c), row.lounge.station,
					dim(string.interp(l.WAITING_SINCE, { date = Format.DateOnly(row.entry.since) })),
					string.interp(l.COST_WAITING, { rate = money(rate), wage = money(halfWage) }))
			end
		end
		ui.endTable()
	end)

	if not selectedRow then return end
	local c = selectedRow.c
	if selectedRow.transfer then
		if ui.button(Transfers.glyph(icons.transfer_cancel, l.CANCEL_DELIVERY) .. "##fleetCancelCrew") then
			Transfers.Cancel(selectedRow.transfer)
		end
		return
	end
	local lounge = selectedRow.lounge
	local place = { key = lounge.key, path = lounge.path, station = lounge.station, factor = lounge.factor }
	if ui.button(Transfers.glyph(icons.ship_send, l.SEND_TO) .. "##fleetSendCrew") then
		Transfers.SendCrew(place, c)
	end
	local station = Game.player:GetDockedWith()
	if station and Hangar.StationKey(station) ~= lounge.key then
		ui.sameLine()
		if ui.button(Transfers.glyph(icons.ship_receive, l.TRANSFER_HERE) .. "##fleetCrewHere") then
			Transfers.SendCrew(place, c, Hangar.StationKey(station))
		end
	end
	ui.sameLine()
	if ui.button(lc.DISMISS .. "##fleetDismiss") then
		Pickers.ConfirmDismiss(c)
		selectedCrew = nil
	end
	ui.sameLine()
	if ui.button(ui.get_icon_glyph(icons.sector_map) .. " " .. l.SHOW_ON_MAP .. "##fleetCrewMap") then showOnMap(lounge.path) end
end

InfoView:registerView({
	id = "fleet",
	name = l.FLEET,
	icon = icons.fleet,
	showView = true,
	draw = function()
		ui.withFont(pionillium.body, function()
			ui.withStyleVars({ ItemSpacing = sizes.itemSpacing }, function()
				drawShips()
				drawHangars()
				drawCrew()
			end)
		end)
	end,
	refresh = function()
		selectedShip, selectedHangar, selectedCrew = nil, nil, nil
	end,
	debugReload = function()
		package.reimport()
	end
})
