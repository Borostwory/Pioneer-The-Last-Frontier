-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- The Last Frontier: windows for the delivery service, used by the Hangar
-- tab: send a ship, equipment or crew to another station ("Send to..."),
-- bring them here from one ("Transfer from..."), and cancel a delivery.

local Game = require 'Game'
local Lang = require 'Lang'
local Format = require 'Format'
local ShipDef = require 'ShipDef'
local PlayerState = require 'PlayerState'
local ModalWindow = require 'pigui.libs.modal-win'
local Hangar = require 'modules.Hangar.Hangar'
local Pickers = require 'pigui.libs.hangar-pickers'

local ui = require 'pigui'

local l = Lang.GetResource("module-hangar")
local lc = Lang.GetResource("ui-core")
local ls = Lang.GetResource("ships")

local colors = ui.theme.colors
local icons = ui.theme.icons
local orbiteer = ui.fonts.orbiteer
local pionillium = ui.fonts.pionillium

local Vector2 = _G.Vector2

local Transfers = {}

local state = nil ---@type table?
local modal -- created below

local function money(n) return Format.Money(math.floor(n + 0.5), false) end

local function button(label, enabled, tooltip)
	local variant = not enabled and ui.theme.buttonColors.disabled or nil
	return ui.button(label, Vector2(0, 0), variant, tooltip) and enabled
end

local function glyph(icon, text) return ui.get_icon_glyph(icon) .. " " .. text end

local function shipType(shipId)
	local def = ShipDef[shipId]
	return def and ls[def.i18n_key] or shipId
end

local function distanceText(ly)
	return ly == 0 and l.SAME_SYSTEM or string.format("%.2f %s", ly, lc.LY)
end

-- "5.00 ly · 3d 12h, arrives 22 Apr 3201 · 490 cr"
local function quoteLine(cost, time, ly)
	return string.interp(l.QUOTE_LINE, {
		distance = distanceText(ly),
		time = ui.Format.Duration(time, 2),
		date = Format.DateOnly(Game.time + time),
		cost = money(cost),
	})
end

-- A combo over `list`, labelled by `label(item)`; returns the picked item.
-- A window opened with a destination (`state.toKey`) starts on it.
local function pick(id, list, index, label)
	local names = {}
	for i, item in ipairs(list) do
		table.insert(names, label(item))
		if state.toKey and item.key == state.toKey then index = i - 1 end
	end
	state.toKey = nil
	if #names == 0 then return nil, 0 end
	index = math.min(index or 0, #names - 1)
	ui.pushItemWidth(ui.getContentRegion().x * 0.7)
	local changed, i = ui.combo(id, index, names)
	ui.popItemWidth()
	if changed then index = i end
	return list[index + 1], index
end

local function otherHangars(h)
	local list = {}
	for _, other in ipairs(Hangar.GetHangars()) do
		if other ~= h then table.insert(list, other) end
	end
	return list
end

-- One footer for every window: the reason it can't be booked, Cancel, Book
local function footer(self, blocked, bookLabel, book)
	if blocked then
		ui.spacing()
		ui.textColored(colors.alertRed, blocked)
	end
	ui.spacing()
	if ui.button(lc.CANCEL .. "##transferCancel") then
		state = nil
		self:close()
		return
	end
	ui.sameLine()
	if button(bookLabel .. "##transferBook", not blocked, blocked) then
		book()
		state = nil
		self:close()
	end
end

local title = Pickers.title

--=============================================================================
-- The windows
--=============================================================================

local draw = {}

-- Send a stored ship to another hangar
function draw.sendShip(self)
	local rec = state.rec
	local from = Hangar.GetHangar(rec.hangar)
	title(icons.ship_send, string.interp(l.DELIVER_SHIP_TITLE, { ship = rec.name }))
	ui.text(string.interp(l.FROM_LINE, { station = from.station }))

	local targets = otherHangars(from)
	ui.text(l.TO_LABEL)
	ui.sameLine()
	local to
	to, state.index = pick("##sendShipTo", targets, state.index, function(h)
		return string.interp(l.HANGAR_WITH_BAYS, { station = h.station, free = Hangar.FreeBays(h) })
	end)
	if not to then
		ui.textColored(colors.fontDim, l.NO_OTHER_HANGARS)
		footer(self, l.NO_OTHER_HANGARS, l.BOOK_DELIVERY, function() end)
		return
	end
	Pickers.mapButton("sendShipTo", to.path)
	local cost, time, ly = Hangar.ShipDeliveryQuote(rec.shipId, from.path, to.path)
	ui.text(quoteLine(cost, time, ly))
	ui.textWrapped(l.SHIP_TRAVELS_NOTE)
	local dock = Hangar.DockWarning(rec.shipId, to.path, to.station)
	if dock then ui.textColored(colors.alertYellow, dock) end
	footer(self, Hangar.SendShipBlocked(rec, to), l.BOOK_DELIVERY, function() Hangar.SendShip(rec, to) end)
end

-- Bring a ship stored elsewhere into a bay here
function draw.shipFrom(self)
	local h = state.hangar
	title(icons.ship_receive, string.interp(l.TRANSFER_SHIP_TITLE, { station = h.station }))
	ui.text(string.interp(l.INTO_BAY_LINE, { free = Hangar.FreeBays(h) }))
	ui.spacing()

	local rows = {}
	for _, other in ipairs(otherHangars(h)) do
		if not Hangar.IsLocked(other) then
			for _, rec in ipairs(Hangar.StoredShips(other)) do
				if not rec.transit then
					local cost, time, ly = Hangar.ShipDeliveryQuote(rec.shipId, other.path, h.path)
					table.insert(rows, { rec = rec, from = other, cost = cost, time = time, ly = ly })
				end
			end
		end
	end
	table.sort(rows, function(a, b) return a.ly < b.ly end)
	if #rows == 0 then
		ui.textColored(colors.fontDim, l.NO_SHIPS_ELSEWHERE)
	end
	if #rows > 0 and ui.beginTable("shipFromList", 6, { "RowBg", "BordersInnerH", "SizingFixedFit", "NoSavedSettings" }) then
		ui.tableSetupColumn(l.COL_SHIP, { "WidthStretch" })
		ui.tableSetupColumn(l.COL_STATION, { "WidthFixed" })
		ui.tableSetupColumn(l.COL_DISTANCE, { "WidthFixed" })
		ui.tableSetupColumn(l.COL_ARRIVES, { "WidthFixed" })
		ui.tableSetupColumn(l.COL_COST, { "WidthFixed" })
		ui.tableSetupColumn("##map", { "WidthFixed" })
		ui.tableHeadersRow()
		for i, row in ipairs(rows) do
			ui.tableNextRow()
			ui.tableNextColumn()
			local label = string.format("%s (%s)", row.rec.name, shipType(row.rec.shipId)) .. "##shipFrom" .. i
			-- a selectable closes the popup it's in unless told not to
			if ui.selectable(label, state.picked == row.rec, { "SpanAllColumns", "DontClosePopups", "AllowOverlap" }) then
				state.picked = row.rec
			end
			ui.tableNextColumn()
			ui.text(row.from.station)
			ui.tableNextColumn()
			ui.text(distanceText(row.ly))
			ui.tableNextColumn()
			ui.text(string.format("%s (%s)", Format.DateOnly(Game.time + row.time), ui.Format.Duration(row.time, 2)))
			ui.tableNextColumn()
			ui.text(money(row.cost))
			ui.tableNextColumn()
			ui.dummy(Vector2(0, 0))
			Pickers.mapButton("shipFrom" .. i, row.from.path)
		end
		ui.endTable()
	end
	ui.textColored(colors.fontDim, l.SHIPS_ELSEWHERE_NOTE)
	ui.textWrapped(l.SHIP_TRAVELS_NOTE)

	local rec = state.picked
	local dock = rec and Hangar.DockWarning(rec.shipId, h.path, h.station)
	if dock then ui.textColored(colors.alertYellow, dock) end
	local blocked = not rec and l.PICK_A_SHIP or Hangar.SendShipBlocked(rec, h)
	footer(self, blocked, l.BOOK_DELIVERY, function() Hangar.SendShip(rec, h) end)
end

-- Items of one hangar's store to another's, either way
function draw.equipment(self)
	local here = state.hangar
	local sending = state.sending
	title(sending and icons.ship_send or icons.ship_receive,
		string.interp(sending and l.SEND_EQUIPMENT_TITLE or l.TRANSFER_EQUIPMENT_TITLE, { station = here.station }))

	ui.text(sending and l.TO_LABEL or l.FROM_LABEL)
	ui.sameLine()
	local other
	other, state.index = pick("##equipOther", otherHangars(here), state.index, function(h) return h.station end)
	if not other then
		footer(self, l.NO_OTHER_HANGARS, l.BOOK_DELIVERY, function() end)
		return
	end
	Pickers.mapButton("equipOther", other.path)
	if other ~= state.other then state.other, state.picked = other, {} end
	local from, to = sending and here or other, sending and other or here

	ui.spacing()
	local items = {}
	if #from.store == 0 then ui.textColored(colors.fontDim, l.STORE_EMPTY) end
	for _, item in ipairs(from.store) do
		local changed, value = ui.checkbox(item:GetName() .. "##sendItem" .. tostring(item), state.picked[item] == true)
		if changed then state.picked[item] = value or nil end
		ui.sameLine()
		ui.textColored(colors.fontDim, string.format("%s m³", ui.Format.Number(item.volume, 1)))
		if state.picked[item] then table.insert(items, item) end
	end

	ui.spacing()
	if #items > 0 then
		local cost, time, ly = Hangar.EquipmentDeliveryQuote(items, from.path, to.path)
		ui.text(string.interp(l.ITEMS_PICKED, { count = #items }))
		ui.text(quoteLine(cost, time, ly))
	end
	footer(self, Hangar.SendEquipmentBlocked(from, to, items), l.BOOK_DELIVERY,
		function() Hangar.SendEquipment(from, to, items) end)
end

-- People waiting in one lounge to another, either way
function draw.crew(self)
	local hereKey = state.place.key
	local sending = state.sending
	title(sending and icons.ship_send or icons.ship_receive,
		string.interp(sending and l.SEND_CREW_TITLE or l.TRANSFER_CREW_TITLE, { station = state.place.station }))

	local others
	if sending then
		others = Hangar.CrewDestinations(hereKey)
	else
		others = {}
		for _, lounge in ipairs(Hangar.GetLounges()) do
			if lounge.key ~= hereKey then table.insert(others, lounge) end
		end
	end
	ui.text(sending and l.TO_LABEL or l.FROM_LABEL)
	ui.sameLine()
	local other
	other, state.index = pick("##crewOther", others, state.index, function(p) return p.station end)
	if not other then
		footer(self, sending and l.NO_DESTINATION or l.NOBODY_WAITING_ELSEWHERE, l.BOOK_PASSAGE, function() end)
		return
	end
	Pickers.mapButton("crewOther", other.path)
	if not sending and other ~= state.other then state.other, state.picked = other, {} end

	local fromKey = sending and hereKey or other.key
	local fromLounge = nil
	for _, lounge in ipairs(Hangar.GetLounges()) do
		if lounge.key == fromKey then fromLounge = lounge end
	end
	local to = sending and other or state.place

	ui.spacing()
	local people = {}
	for _, entry in ipairs(fromLounge and fromLounge.crew or {}) do
		local c = entry.character
		local changed, value = ui.checkbox(c.name .. "##sendCrew" .. tostring(c), state.picked[c] == true)
		if changed then state.picked[c] = value or nil end
		if state.picked[c] then table.insert(people, c) end
	end

	ui.spacing()
	if #people > 0 then
		local cost, time, ly = Hangar.CrewTicketQuote(#people, fromLounge.path, to.path)
		ui.text(quoteLine(cost, time, ly))
		ui.textWrapped(l.CREW_TRAVEL_NOTE)
	end
	footer(self, Hangar.SendCrewBlocked(fromKey, to, people), l.BOOK_PASSAGE,
		function() Hangar.SendCrew(fromKey, to, people) end)
end

-- Cancel a delivery
function draw.cancel(self)
	local t = state.transfer
	title(icons.transfer_cancel, string.interp(l.CANCEL_TITLE, { what = Transfers.WhatText(t) }))
	ui.textWrapped(string.interp(l.CANCEL_TEXT, {
		refund = money(t.cost * 0.5), station = t.from.station, date = Format.DateOnly(Hangar.ReturnTime(t)) }))
	footer(self, Hangar.CancelBlocked(t), l.CANCEL_DELIVERY, function() Hangar.CancelTransfer(t) end)
end

modal = Pickers.NewModal("HangarTransfer", function(self)
	if not state then self:close() return end
	draw[state.window](self)
end)

local function open(s)
	state = s
	modal:open()
end

--=============================================================================
-- Opening them, and texts for things on their way
--=============================================================================

function Transfers.SendShip(rec, toKey) open({ window = "sendShip", rec = rec, index = 0, toKey = toKey }) end
function Transfers.ShipFrom(h) open({ window = "shipFrom", hangar = h }) end
function Transfers.SendEquipment(h, toKey) open({ window = "equipment", hangar = h, sending = true, index = 0, picked = {}, toKey = toKey }) end
function Transfers.EquipmentFrom(h) open({ window = "equipment", hangar = h, sending = false, index = 0, picked = {} }) end
function Transfers.Cancel(t) open({ window = "cancel", transfer = t }) end

---@param place Hangar.Place
function Transfers.SendCrew(place, character, toKey)
	open({ window = "crew", place = place, sending = true, index = 0, picked = character and { [character] = true } or {}, toKey = toKey })
end
function Transfers.CrewFrom(place) open({ window = "crew", place = place, sending = false, index = 0, picked = {} }) end

-- What a transfer carries: a ship's name, "3 items", a person's name
function Transfers.WhatText(t)
	if t.kind == "ship" then return t.rec.name end
	if t.kind == "equipment" then return string.interp(l.N_ITEMS, { count = #t.items }) end
	local names = {}
	for _, c in ipairs(t.crew) do table.insert(names, c.name) end
	return table.concat(names, ", ")
end

-- "from Lave Station, arrives 22 Apr 3201" (or "returning to ...")
function Transfers.StatusText(t)
	return string.interp(t.returning and l.RETURNING_STATUS or l.ON_ITS_WAY_STATUS, {
		from = t.from.station, to = t.to.station, date = Format.DateOnly(t.arrives) })
end

-- A row for something on its way here: the in-transit mark, what, when, and a
-- cancel button
function Transfers.DrawIncoming(t, label)
	ui.textColored(colors.fontDim, ui.get_icon_glyph(icons.in_transit) .. " " .. label .. " · " .. Transfers.StatusText(t))
	ui.sameLine()
	if ui.button(ui.get_icon_glyph(icons.transfer_cancel) .. "##cancelTransfer" .. t.id, Vector2(0, 0),
			ui.theme.buttonColors.dark, l.CANCEL_DELIVERY) then
		Transfers.Cancel(t)
	end
end

Transfers.glyph = glyph

return Transfers
