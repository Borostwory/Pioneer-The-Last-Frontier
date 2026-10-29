-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- The Last Frontier: station hangars where the player stores ships.
--
-- A stored ship is a record (type, paint, hull, fuel, equipment, cargo and
-- per-ship module state), not a body in space: only the current system exists
-- in Space. Swapping rebuilds Game.player from a record, as the Ship Market
-- does when buying a ship.
--
-- Rent is billed monthly, on the 1st of each calendar month. Money paid
-- upfront becomes credit on the hangar and is drawn before the player's money.
-- Unpaid rent becomes debt (with a late charge, up to a cap) and locks the
-- hangar until it is paid.

local CargoManager = require 'CargoManager'
local Comms = require 'Comms'
local Commodities = require 'Commodities'
local EquipSet = require 'EquipSet'
local Economy = require 'Economy'
local Event = require 'Event'
local Game = require 'Game'
local HullConfig = require 'HullConfig'
local Lang = require 'Lang'
local Passengers = require 'Passengers'
local PlayerState = require 'PlayerState'
local Serializer = require 'Serializer'
local Space = require 'Space'
local ShipDef = require 'ShipDef'
local Timer = require 'Timer'
local Format = require 'Format'
local utils = require 'utils'

local l = Lang.GetResource("module-hangar")

local Hangar = {}

Hangar.MAX_BAYS = 4
Hangar.BAY_RENT = 40            -- cr per bay per month
Hangar.TONNE_RENT = 4           -- cr per tonne of hull parked, per month
Hangar.LATE_CHARGE = 0.1        -- added to each unpaid bill
Hangar.DEBT_CAP_MONTHS = 6      -- unpaid months after which debt stops growing
Hangar.LOUNGE_NIGHT = 3         -- cr per person per night in a crew lounge

-- Discount for paying upfront, by number of months
Hangar.PREPAY_DISCOUNTS = {
	{ months = 12, discount = 0.15 },
	{ months = 6, discount = 0.10 },
	{ months = 3, discount = 0.05 },
}

---@class Hangar.Record
---@field id integer
---@field hangar string            key of the hangar the ship is stored in
---@field storedAt number          game time it was stored
---@field order integer            bay order: a swapped-in ship takes the order of the one it replaces
---@field shipId string
---@field name string
---@field label string
---@field skin SceneGraph.ModelSkin
---@field pattern integer?
---@field hull number
---@field fuel number
---@field equip table<string|integer, EquipType>
---@field cargo table<string, integer>   commodity name -> tons aboard (all of them)
---@field cargoTags table<string, table<any, integer>>? commodity name -> { [tag] = tons held for it }
---@field state table<string, any> per-ship module state (Hangar.RegisterShipState)
---@field check table<string, number> ship properties when stored, compared when taken out
---@field transit integer?         id of the delivery it is on (see Transfers)

---@class Hangar.Lease
---@field key string
---@field path SystemPath
---@field station string
---@field system string
---@field factor number
---@field bays integer
---@field credit number            rent covered by money paid upfront
---@field creditCost number        what that credit cost
---@field accrued number           rent since the last bill
---@field accruedTo number         game time `accrued` is counted to
---@field billedFrom number?       game time the next bill counts from (the lease or the last bill)
---@field debt number
---@field unpaidMonths integer
---@field store EquipType[]        equipment kept in the hangar
---@field creditWarned boolean?   the "prepaid rent running out" message was sent
---@field cargo table<string, integer>?  goods kept in the hangar: commodity name -> tons
---@field cargoTags table<string, table<any, integer>>? how much of them missions hold

---@class Hangar.LoungeEntry
---@field character Character
---@field since number             game time they started waiting
---@field paidTo number            game time their nights are paid to

---@class Hangar.Lounge
---@field key string
---@field path SystemPath
---@field station string
---@field factor number
---@field crew Hangar.LoungeEntry[]

local hangars = {} ---@type table<string, Hangar.Lease>
local ships = {} ---@type table<integer, Hangar.Record>
local lounges = {} ---@type table<string, Hangar.Lounge>
local loungeDebt = 0               -- unpaid lounge fees, paid from the next money
local transfers = {} ---@type table<integer, Hangar.Transfer>
local nextTransferId = 1
local transferToken = 0
local nextId = 1
local nextBill = nil ---@type number?

local shipStates = {} ---@type table<string, { get: fun(): any, set: fun(data: any) }>

local loaded_data
local billingToken = 0

local ORDINARY = CargoManager.ORDINARY

--=============================================================================
-- Helpers
--=============================================================================

local function round(x) return math.floor(x + 0.5) end

local function hullMass(shipId)
	return ShipDef[shipId] and ShipDef[shipId].hullMass or 0
end
Hangar.HullMass = hullMass

-- Rent is a price per calendar month: a full month costs the monthly rent
-- whatever its length, a part month its share of that month's days.

-- The game time of the 1st of the month that time t is in
local function monthStart(t)
	local year, month = util.gameTimeToTimeParts(t)
	return util.timePartsToGameTime(year, month, 1, 0, 0, 0)
end
Hangar.MonthStart = monthStart

-- The game time of the 1st of the month after time t
local function firstOfNextMonth(t)
	local year, month = util.gameTimeToTimeParts(t)
	month = month + 1
	if month > 12 then month, year = 1, year + 1 end
	return util.timePartsToGameTime(year, month, 1, 0, 0, 0)
end

-- Months between t0 and t1, part months counted by their share of the days
local function monthsBetween(t0, t1)
	local months = 0
	while t0 < t1 do
		local start, stop = monthStart(t0), firstOfNextMonth(t0)
		local t = math.min(t1, stop)
		months = months + (t - t0) / (stop - start)
		t0 = t
	end
	return months
end

-- The time `credit` runs out, spent from time t at `monthly` rent
function Hangar.CreditLastsUntil(t, credit, monthly)
	if monthly <= 0 then return nil end
	for _ = 1, 1200 do
		local start, stop = monthStart(t), firstOfNextMonth(t)
		local cost = monthly * (stop - t) / (stop - start)
		if cost >= credit then
			return t + credit / monthly * (stop - start)
		end
		credit = credit - cost
		t = stop
	end
	return t
end

-- The game time of the next monthly bill
function Hangar.NextBill()
	return nextBill
end

---@param station SpaceStation
function Hangar.StationKey(station)
	return tostring(station.path)
end

-- Busy systems cost more, as with the landing fee; surface starports less.
---@param station SpaceStation
function Hangar.StationFactor(station)
	local body = station.path:GetSystemBody()
	local pop = body and body.parent and body.parent.population or 0
	local f = math.clamp(0.7 + 0.15 * pop, 0.7, 1.75)
	if station.isGroundStation then f = f * 0.8 end
	return f
end

---@param equip table<string|integer, EquipType>
local function sumCapability(equip, name)
	local total = 0
	for _, item in pairs(equip) do
		if item.capabilities and item.capabilities[name] then
			total = total + item.capabilities[name]
		end
	end
	return total
end

local function cargoTotal(cargo)
	local total = 0
	for _, n in pairs(cargo) do total = total + n end
	return total
end

--=============================================================================
-- Cargo on a swap
--
-- The player's cargo is split in lots: the ordinary tons of a commodity, and
-- the tons each mission holds. `take` says how much of each lot comes along:
-- take[name] = { [CargoManager.ORDINARY] = n, [mission] = n }.
--=============================================================================

---@class Hangar.CargoLot
---@field name string
---@field tag any        CargoManager.ORDINARY or the owner of the tons
---@field count integer

-- What a tag (usually a mission) holding cargo is called in the UI
function Hangar.TagLabel(tag)
	return type(tag) == "table" and tag.GetCargoLabel and tag:GetCargoLabel() or l.A_MISSION
end

-- The player's cargo by lot: per commodity, mission tons first
---@return Hangar.CargoLot[]
function Hangar.CargoLots()
	local cargoMgr = Game.player:GetComponent('CargoManager')
	local lots = {}
	for name in cargoMgr:Commodities() do
		local commodity = Commodities[name]
		local held = {}
		for tag, n in pairs(cargoMgr:GetTags(commodity)) do
			table.insert(held, { name = name, tag = tag, count = n, label = Hangar.TagLabel(tag) })
		end
		table.sort(held, function(a, b) return a.label < b.label end)
		for _, lot in ipairs(held) do table.insert(lots, lot) end
		local ordinary = cargoMgr:CountCommodity(commodity, ORDINARY)
		if ordinary > 0 then table.insert(lots, { name = name, tag = ORDINARY, count = ordinary }) end
	end
	table.sort(lots, function(a, b)
		if a.name ~= b.name then return a.name < b.name end
		return a.tag ~= ORDINARY and b.tag == ORDINARY
	end)
	return lots
end

function Hangar.TakeTotal(take)
	local total = 0
	for _, parts in pairs(take) do
		for _, n in pairs(parts) do total = total + n end
	end
	return total
end

-- Free cargo space on a stored ship
---@param rec Hangar.Record
function Hangar.RecordFreeSpace(rec)
	return Hangar.RecordCargoCapacity(rec) - cargoTotal(rec.cargo)
end

-- Take as much as fits on `rec`: mission cargo first (the delivery is made
-- with the ship you arrive in), then ordinary cargo
---@param rec Hangar.Record
function Hangar.FitCargo(rec)
	return Hangar.FitCargoInto(Hangar.RecordFreeSpace(rec), Hangar.RecordLifeSupport(rec))
end

-- Take as much as fits in `room` tons with `lifeSupport`, mission cargo first
function Hangar.FitCargoInto(room, lifeSupport)
	local take = {}
	local lots = Hangar.CargoLots()
	table.sort(lots, function(a, b)
		local am, bm = a.tag ~= ORDINARY, b.tag ~= ORDINARY
		if am ~= bm then return am end
		return a.name < b.name
	end)
	for _, lot in ipairs(lots) do
		local commodity = Commodities[lot.name]
		local n = 0
		if (commodity.life_support or 0) <= lifeSupport then
			n = math.max(0, math.min(lot.count, room))
		end
		take[lot.name] = take[lot.name] or {}
		take[lot.name][lot.tag] = n
		room = room - n
	end
	return take
end

-- Where the cargo held for `tag` is on stored ships
---@return { rec: Hangar.Record, hangar: Hangar.Lease?, name: string, count: integer }[]
function Hangar.FindHeldCargo(tag)
	local found = {}
	for _, rec in pairs(ships) do
		for name, tags in pairs(rec.cargoTags or {}) do
			if tags[tag] then
				table.insert(found, { rec = rec, hangar = hangars[rec.hangar], name = name, count = tags[tag] })
			end
		end
	end
	table.sort(found, function(a, b) return a.rec.id < b.rec.id end)
	-- and in the hangars' cargo stores (rec is nil)
	for _, h in pairs(hangars) do
		for name, tags in pairs(h.cargoTags or {}) do
			if tags[tag] then
				table.insert(found, { hangar = h, name = name, count = tags[tag] })
			end
		end
	end
	return found
end

-- Tons of each commodity on a stored ship held for missions
---@param rec Hangar.Record
---@return table<string, integer>
function Hangar.RecordHeldCargo(rec)
	local held = {}
	for name, tags in pairs(rec.cargoTags or {}) do
		for _, n in pairs(tags) do held[name] = (held[name] or 0) + n end
	end
	return held
end

-- A mission ended: what a stored ship held for it becomes ordinary cargo
local function releaseTag(tag)
	local function release(holder)
		for name, tags in pairs(holder.cargoTags or {}) do
			tags[tag] = nil
			if not next(tags) then holder.cargoTags[name] = nil end
		end
	end
	for _, rec in pairs(ships) do release(rec) end
	for _, h in pairs(hangars) do release(h) end
end

-- Properties compared between storing a ship and taking it out again.
---@param ship Ship
local function checkProps(ship)
	local equip = ship:GetComponent('EquipSet'):GetInstalledEquipment()
	local props = {
		equipVolume = ship.equipVolume,
		totalVolume = ship.totalVolume,
		cargo_cap = ship.cargo_cap,
		equipMass = ship.mass_cap - ship.usedCargo,
		hull = ship.hullPercent,
		fuel = ship.fuel,
	}
	for _, item in pairs(equip) do
		for k in pairs(item.capabilities or {}) do
			local cap = k .. "_cap"
			props[cap] = ship:hasprop(cap) and ship[cap] or 0
		end
	end
	return props
end

--=============================================================================
-- Per-ship module state
--=============================================================================

-- Function: RegisterShipState
--
-- Let a module keep per-ship state across swaps. `get` returns the state of
-- the player's current ship (it must not be changed later by the module), `set`
-- restores a state taken earlier. `set` runs after the swap's queued
-- onShipTypeChanged event, so it wins over any reset done there.
---@param name string
---@param get fun(): any
---@param set fun(data: any)
function Hangar.RegisterShipState(name, get, set)
	shipStates[name] = { get = get, set = set }
end

--=============================================================================
-- Rent and billing
--=============================================================================

---@param h Hangar.Lease
function Hangar.StoredShips(h)
	local list = {}
	for _, rec in pairs(ships) do
		if rec.hangar == h.key then table.insert(list, rec) end
	end
	table.sort(list, function(a, b) return (a.order or a.id) < (b.order or b.id) end)
	return list
end

---@param h Hangar.Lease
function Hangar.FreeBays(h)
	return h.bays - #Hangar.StoredShips(h)
end

-- Rent per month for what the hangar holds now
---@param h Hangar.Lease
function Hangar.MonthlyRent(h)
	local tonnes = 0
	for _, rec in ipairs(Hangar.StoredShips(h)) do
		-- a ship on its way holds its bay; parking counts from arrival
		if not rec.transit then tonnes = tonnes + hullMass(rec.shipId) end
	end
	return (Hangar.BAY_RENT * h.bays + Hangar.TONNE_RENT * tonnes) * h.factor
end

-- Rent per month for one bay: the bay fee, and the parking fee for the ship
-- in it (0 when the bay is empty)
---@param h Hangar.Lease
---@param rec Hangar.Record?
function Hangar.BayRent(h, rec)
	local bay = Hangar.BAY_RENT * h.factor
	local parking = rec and not rec.transit and Hangar.TONNE_RENT * hullMass(rec.shipId) * h.factor or 0
	return bay, parking
end

-- Monthly rent for a hangar that doesn't exist yet
function Hangar.QuoteRent(factor, bays, tonnes)
	return (Hangar.BAY_RENT * bays + Hangar.TONNE_RENT * (tonnes or 0)) * factor
end

function Hangar.PrepayDiscount(months)
	for _, d in ipairs(Hangar.PREPAY_DISCOUNTS) do
		if months >= d.months then return d.discount end
	end
	return 0
end

-- Credit and its cost for paying `months` of the given monthly rent upfront
function Hangar.QuotePrepay(monthly, months)
	local value = monthly * months
	local discount = Hangar.PrepayDiscount(months)
	return value, round(value * (1 - discount)), discount
end

-- Count rent up to time t at the hangar's current rate. Called before
-- anything that changes the rate (ships parked or taken out, bays changed).
---@param h Hangar.Lease
local function accrue(h, t)
	if t > h.accruedTo then
		h.accrued = h.accrued + Hangar.MonthlyRent(h) * monthsBetween(h.accruedTo, t)
		h.accruedTo = t
	end
end

---@param h Hangar.Lease
local function drawCredit(h, amount)
	if h.credit <= 0 then return 0 end
	local used = math.min(h.credit, amount)
	h.creditCost = h.creditCost - h.creditCost * used / h.credit
	h.credit = h.credit - used
	if h.credit < 0.01 then h.credit, h.creditCost = 0, 0 end
	return used
end

-- Take what you can of `amount` from the player's money
local function takeMoney(amount)
	local paid = math.min(math.max(PlayerState.GetMoney(), 0), amount)
	if paid > 0 then PlayerState.AddMoney(-paid) end
	return paid
end

---@param h Hangar.Lease
function Hangar.IsLocked(h)
	return h.debt > 0
end

-- Rent counted since the last bill, up to time t (now by default), at the
-- current rate
---@param h Hangar.Lease
function Hangar.RentSoFar(h, t)
	t = t or Game.time
	return h.accrued + Hangar.MonthlyRent(h) * monthsBetween(h.accruedTo, t)
end

-- Where the next bill starts counting from
---@param h Hangar.Lease
function Hangar.BilledFrom(h)
	return h.billedFrom or h.accruedTo
end

-- Estimated time the prepaid credit runs out, or nil when there is none
---@param h Hangar.Lease
function Hangar.PaidUntil(h)
	if h.credit <= 0 then return nil end
	return Hangar.CreditLastsUntil(Game.time, math.max(0, h.credit - Hangar.RentSoFar(h)), Hangar.MonthlyRent(h))
end

-- Bill one hangar at time t (the 1st of a month). Returns the bill and how
-- much of it came from credit, from money, and was left unpaid.
---@param h Hangar.Lease
local function billHangar(h, t)
	accrue(h, t)
	local bill = h.accrued
	h.accrued = 0
	h.billedFrom = t

	local fromCredit = drawCredit(h, bill)
	local due = bill - fromCredit

	local fromMoney, unpaid = 0, 0
	if due > 0 then
		fromMoney = takeMoney(due)
		unpaid = due - fromMoney
	end

	if unpaid > 0.5 then
		if h.unpaidMonths < Hangar.DEBT_CAP_MONTHS then
			h.debt = h.debt + unpaid * (1 + Hangar.LATE_CHARGE)
			h.unpaidMonths = h.unpaidMonths + 1
		end
		Comms.ImportantMessage(string.interp(l.RENT_UNPAID, {
			station = h.station, debt = Format.Money(round(h.debt), false)
		}), h.station)
	end

	-- Old debt is paid off from whatever money there is
	if h.debt > 0 then
		h.debt = h.debt - takeMoney(h.debt)
		if h.debt < 0.5 then
			h.debt, h.unpaidMonths = 0, 0
		end
	end

	return bill, fromCredit, fromMoney, unpaid
end

local scheduleBilling
local billLounges -- defined with the lounges below

-- Bill every 1st that has passed, in order (time may have jumped months)
local function runBilling()
	while nextBill and nextBill <= Game.time do
		-- wages are paid first by the game's own timers, then lounges, then rent
		billLounges(nextBill)
		for _, h in pairs(hangars) do
			billHangar(h, nextBill)
		end
		nextBill = firstOfNextMonth(nextBill + 1)
	end
	scheduleBilling()
end

scheduleBilling = function()
	if not nextBill then nextBill = firstOfNextMonth(Game.time) end
	billingToken = billingToken + 1
	local token = billingToken
	Timer:CallAt(nextBill, function()
		if token == billingToken then runBilling() end
	end)
end

--=============================================================================
-- Leases
--=============================================================================

---@param station SpaceStation
---@return Hangar.Lease?
function Hangar.GetHangarAt(station)
	return station and hangars[Hangar.StationKey(station)]
end

---@return Hangar.Lease?
function Hangar.GetHangar(key)
	return hangars[key]
end

---@return Hangar.Lease[]
function Hangar.GetHangars()
	local list = utils.build_array(pairs(hangars))
	table.sort(list, function(a, b) return a.station < b.station end)
	return list
end

-- Lease a hangar at the station the player is docked at, paying `months` of
-- rent upfront (0 for none). The upfront payment is worked out for the rent
-- with `tonnes` of hull parked (the player's own ship, typically).
---@param station SpaceStation
function Hangar.Lease(station, bays, months, tonnes)
	local key = Hangar.StationKey(station)
	if hangars[key] then return false, l.ALREADY_LEASED end
	bays = math.clamp(bays, 1, Hangar.MAX_BAYS)

	local factor = Hangar.StationFactor(station)
	local value, cost = Hangar.QuotePrepay(Hangar.QuoteRent(factor, bays, tonnes), months)
	if cost > PlayerState.GetMoney() then return false, l.NOT_ENOUGH_MONEY end
	PlayerState.AddMoney(-cost)

	hangars[key] = {
		key = key,
		path = station.path,
		station = station.label,
		system = Game.system.name,
		factor = factor,
		bays = bays,
		credit = value,
		creditCost = cost,
		accrued = 0,
		accruedTo = Game.time,
		billedFrom = Game.time,
		debt = 0,
		unpaidMonths = 0,
		store = {},
	}
	return true
end

---@param h Hangar.Lease
function Hangar.Prepay(h, months)
	if months <= 0 then return false end
	local value, cost = Hangar.QuotePrepay(Hangar.MonthlyRent(h), months)
	if cost > PlayerState.GetMoney() then return false, l.NOT_ENOUGH_MONEY end
	PlayerState.AddMoney(-cost)
	h.credit = h.credit + value
	h.creditCost = h.creditCost + cost
	h.creditWarned = nil
	return true
end

---@param h Hangar.Lease
function Hangar.AddBay(h)
	if h.bays >= Hangar.MAX_BAYS then return false end
	accrue(h, Game.time)
	h.bays = h.bays + 1
	return true
end

---@param h Hangar.Lease
function Hangar.RemoveBay(h)
	if h.bays <= 1 or Hangar.FreeBays(h) < 1 then return false end
	accrue(h, Game.time)
	h.bays = h.bays - 1
	return true
end

---@param h Hangar.Lease
function Hangar.PayDebt(h)
	if h.debt <= 0 then return true end
	local debt = round(h.debt)
	if PlayerState.GetMoney() < debt then return false, l.NOT_ENOUGH_MONEY end
	PlayerState.AddMoney(-debt)
	h.debt, h.unpaidMonths = 0, 0
	return true
end

---@param h Hangar.Lease
function Hangar.CanEndLease(h)
	if #Hangar.StoredShips(h) > 0 or #h.store > 0 or Hangar.StoreCargoTotal(h) > 0 or h.debt > 0 then return false end
	for _, t in pairs(transfers) do
		if t.from.key == h.key or t.to.key == h.key then return false end
	end
	return true
end

-- End a lease: rent so far is settled, unused credit is refunded at the price
-- that was paid for it.
---@param h Hangar.Lease
function Hangar.EndLease(h)
	if not Hangar.CanEndLease(h) then return false end
	accrue(h, Game.time)
	local due = h.accrued - drawCredit(h, h.accrued)
	if due > PlayerState.GetMoney() then return false, l.NOT_ENOUGH_MONEY end
	PlayerState.AddMoney(round(h.creditCost) - round(due))
	hangars[h.key] = nil
	return true
end

--=============================================================================
-- Crew lounges
--
-- Every station has one. Crew waiting there keep their contract at half wage
-- (CrewContracts reads `character.waitingAt`), gain no skills, and cost
-- LOUNGE_NIGHT per night, settled on the 1st and when they leave.
--=============================================================================

---@param station SpaceStation
---@return Hangar.Lounge?
function Hangar.GetLoungeAt(station)
	return station and lounges[Hangar.StationKey(station)]
end

---@return Hangar.Lounge[]
function Hangar.GetLounges()
	local list = utils.build_array(pairs(lounges))
	table.sort(list, function(a, b) return a.station < b.station end)
	return list
end

-- Cost per person per night at a station
function Hangar.LoungeRate(factor)
	return Hangar.LOUNGE_NIGHT * factor
end

-- Everyone waiting in any lounge
function Hangar.EachLoungeMember()
	local list = {}
	for _, lounge in pairs(lounges) do
		for _, entry in ipairs(lounge.crew) do table.insert(list, entry.character) end
	end
	-- and crew travelling between lounges (paid as if waiting)
	for _, t in pairs(transfers) do
		if t.kind == "crew" then
			for _, c in ipairs(t.crew) do table.insert(list, c) end
		end
	end
	local i = 0
	return function()
		i = i + 1
		return list[i]
	end
end

-- The lounge and entry of someone waiting
---@param character Character
---@return Hangar.Lounge?, Hangar.LoungeEntry?, integer?
function Hangar.FindInLounge(character)
	local lounge = character.waitingAt and lounges[character.waitingAt]
	for i, entry in ipairs(lounge and lounge.crew or {}) do
		if entry.character == character then return lounge, entry, i end
	end
	return nil
end

function Hangar.LoungeDebt()
	return loungeDebt
end

-- Charge lounge fees: what can't be paid joins the lounge debt
local function chargeLounge(amount)
	if amount <= 0 then return end
	loungeDebt = loungeDebt + amount - takeMoney(amount)
end

-- Settle someone's nights up to time t
---@param lounge Hangar.Lounge
---@param entry Hangar.LoungeEntry
local function settleEntry(lounge, entry, t)
	if t > entry.paidTo then
		chargeLounge(Hangar.LoungeRate(lounge.factor) * (t - entry.paidTo) / 86400)
		entry.paidTo = t
	end
end

-- On the 1st: settle every lounge, then pay what's owed from any money
billLounges = function(t)
	local before = loungeDebt
	for _, lounge in pairs(lounges) do
		for _, entry in ipairs(lounge.crew) do settleEntry(lounge, entry, t) end
	end
	if loungeDebt > 0.5 and loungeDebt > before then
		Comms.ImportantMessage(string.interp(l.LOUNGE_UNPAID, { debt = Format.Money(round(loungeDebt), false) }))
	end
	loungeDebt = loungeDebt - takeMoney(loungeDebt)
	if loungeDebt < 0.5 then loungeDebt = 0 end
end

function Hangar.PayLoungeDebt()
	local debt = round(loungeDebt)
	if PlayerState.GetMoney() < debt then return false, l.NOT_ENOUGH_MONEY end
	PlayerState.AddMoney(-debt)
	loungeDebt = 0
	return true
end

-- Nobody new is put in a lounge while lounge fees are owed. Returns the
-- reason, or nil.
function Hangar.LoungeBlocked()
	if loungeDebt > 0.5 then
		return string.interp(l.LOUNGE_DEBT_BLOCKS, { debt = Format.Money(round(loungeDebt), false) })
	end
end

-- Add someone to the lounge at `station` (they're on no ship's roster)
local function addToLounge(station, character)
	return Hangar.AddToLoungeAt(Hangar.PlaceOf(station), character)
end

-- A station as transfers and lounges keep it
---@class Hangar.Place
---@field key string
---@field path SystemPath
---@field station string   its name
---@field factor number

---@param station SpaceStation
---@return Hangar.Place
function Hangar.PlaceOf(station)
	return { key = Hangar.StationKey(station), path = station.path, station = station.label,
		factor = Hangar.StationFactor(station) }
end

---@param place Hangar.Place
function Hangar.AddToLoungeAt(place, character)
	local key = place.key
	local lounge = lounges[key]
	if not lounge then
		lounge = { key = key, path = place.path, station = place.station, factor = place.factor, crew = {} }
		lounges[key] = lounge
	end
	table.insert(lounge.crew, { character = character, since = Game.time, paidTo = Game.time })
	character.waitingAt = key
end

-- Take someone out of their lounge, their nights settled
local function removeFromLounge(lounge, entry, index)
	settleEntry(lounge, entry, Game.time)
	table.remove(lounge.crew, index)
	entry.character.waitingAt = nil
	if #lounge.crew == 0 then lounges[lounge.key] = nil end
end

-- Put a crew member of the active ship into the lounge where it is docked
---@param character Character
function Hangar.SendToLounge(character)
	local player = Game.player
	local station = player:GetDockedWith()
	local blocked = Hangar.LoungeBlocked()
	if blocked then return false, blocked end
	if not station or not player:SendCrewToLounge(character) then return false end
	addToLounge(station, character)
	return true
end

-- Someone hired at this station who waits in the lounge until they're
-- needed aboard. The caller sets up their contract.
---@param character Character
function Hangar.HireIntoLounge(character)
	local station = Game.player:GetDockedWith()
	local blocked = Hangar.LoungeBlocked()
	if blocked then return false, blocked end
	if not station then return false end
	character:CheckOut() -- as Ship:Enroll does: not offered for other jobs
	addToLounge(station, character)
	return true
end

-- Let go of someone waiting in a lounge (here, or from afar in the Fleet
-- tab). Only takes them out of the lounge: the caller ends their contract as
-- for any dismissal.
---@param character Character
function Hangar.DismissFromLounge(character)
	local lounge, entry, index = Hangar.FindInLounge(character)
	if not lounge then return false end
	removeFromLounge(lounge, entry, index)
	return true
end

-- Bring someone waiting in the lounge where the player is docked aboard
---@param character Character
function Hangar.TakeFromLounge(character)
	local player = Game.player
	local station = player:GetDockedWith()
	local lounge, entry, index = Hangar.FindInLounge(character)
	if not station or not lounge or lounge.key ~= Hangar.StationKey(station) then return false end
	if not player:TakeCrewFromLounge(character) then return false, l.SHIP_FULL end
	removeFromLounge(lounge, entry, index)
	return true
end

--=============================================================================
-- Equipment store
--
-- Equipment moves between the hangar's store and the ships here: the active
-- ship (through its EquipSet, as the Equipment Market does) or a stored ship
-- (its record is edited, with the same rules checked against the hull).
-- `target` is Hangar.ACTIVE or a stored ship's record.
--=============================================================================

Hangar.ACTIVE = "active"

---@class Hangar.SlotEntry
---@field id string           slot id, compound for slots provided by fitted items
---@field slot HullConfig.Slot
---@field item EquipType?
---@field depth integer       0 for hull slots, 1 for slots on a fitted item, ...

-- Slots provided by fitted items are named after the key their item is
-- fitted under: "<key>##<slot>"
local function depthOf(id)
	local _, n = tostring(id):gsub("##", "")
	return n
end

-- All slots of a stored ship: the hull's and those of the items on it
local function recordSlots(rec)
	local slots = {}
	for _, slot in pairs(HullConfig.GetHullConfig(rec.shipId).slots) do slots[slot.id] = slot end
	local changed = true
	while changed do
		changed = false
		for key, item in pairs(rec.equip) do
			local fitted = type(key) ~= "string" or slots[key]
			if item.provides_slots and fitted then
				for _, sub in pairs(item.provides_slots) do
					local id = tostring(key) .. "##" .. sub.id
					if not slots[id] then slots[id] = sub changed = true end
				end
			end
		end
	end
	return slots
end

-- slot id -> slot, slot id or loose key -> item, used and total volume, hull id
local function targetState(target)
	if target == Hangar.ACTIVE then
		local player = Game.player
		local es = player:GetComponent('EquipSet')
		local items = {}
		for key, item in pairs(es:GetInstalledEquipment()) do
			if type(key) == "number" then key = es.cache[item] end
			items[key] = item
		end
		return es.slotCache, items, player.equipVolume, player.totalVolume, player.shipId
	end
	local used = 0
	for _, item in pairs(target.equip) do used = used + item.volume end
	return recordSlots(target), target.equip, used, HullConfig.GetHullConfig(target.shipId).equipCapacity, target.shipId
end

-- The volume an item takes on a hull: items sized to their ship (the cargo
-- scoop) are sized again for it
local function volumeOn(item, shipId)
	if not item.SpecializeForShip then return item.volume end
	local probe = item:GetPrototype():Instance()
	probe:SpecializeForShip(HullConfig.GetHullConfig(shipId))
	return probe.volume
end

local function specializeFor(item, shipId)
	if not item.SpecializeForShip then return end
	rawset(item, "mass", nil)
	rawset(item, "volume", nil)
	rawset(item, "price", nil)
	item:SpecializeForShip(HullConfig.GetHullConfig(shipId))
end

-- The slots of a ship here, hull slots first, each followed by the slots of
-- the item fitted in it; and the items fitted without a slot
---@return Hangar.SlotEntry[] slots, { key: integer, item: EquipType }[] loose, number used, number capacity
function Hangar.EquipmentOf(target)
	local slots, items, used, capacity = targetState(target)
	local list = {}
	local function add(id, depth)
		table.insert(list, { id = id, slot = slots[id], item = items[id], depth = depth })
		local children = {}
		for childId in pairs(slots) do
			if childId:sub(1, #id + 2) == id .. "##" and depthOf(childId) == depth + 1 then table.insert(children, childId) end
		end
		table.sort(children)
		for _, childId in ipairs(children) do add(childId, depth + 1) end
	end
	local top = {}
	for id in pairs(slots) do
		if depthOf(id) == 0 then table.insert(top, id) end
	end
	table.sort(top, function(a, b)
		local sa, sb = slots[a], slots[b]
		if sa.type ~= sb.type then return sa.type < sb.type end
		return a < b
	end)
	for _, id in ipairs(top) do add(id, 0) end

	local loose = {}
	for key, item in pairs(items) do
		if type(key) == "number" then table.insert(loose, { key = key, item = item }) end
	end
	table.sort(loose, function(a, b) return a.key < b.key end)
	-- slots provided by loose items follow the loose list
	for _, entry in ipairs(loose) do
		for id in pairs(slots) do
			if id:sub(1, #tostring(entry.key) + 2) == tostring(entry.key) .. "##" and depthOf(id) == 1 then
				add(id, 1)
			end
		end
	end
	return list, loose, used, capacity
end

-- Why an item fitted under `key` can't come off now, or nil
local function unfitBlocker(target, key, item)
	local slots, items = targetState(target)
	local prefix = tostring(key) .. "##"
	for id in pairs(items) do
		if type(id) == "string" and id:sub(1, #prefix) == prefix then return l.UNFIT_CHILDREN_FIRST end
	end
	if item:IsA("Equipment.CabinType") and item:GetNumPassengers() > 0 then
		return l.CABIN_OCCUPIED
	end
	if item.capabilities and item.capabilities.surface_rover and target == Hangar.ACTIVE
			and Game.GetRoverPlanet and Game.GetRoverPlanet() then
		return l.ROVER_IS_OUT
	end
	local cargoCap = item.capabilities and item.capabilities.cargo or 0
	local lifeSupport = item.capabilities and item.capabilities.cargo_life_support or 0
	if target == Hangar.ACTIVE then
		if not item:CanBeSold(Game.player) then
			return cargoCap > 0 and l.CARGO_RACK_IN_USE or l.CANT_BE_REMOVED
		end
	else
		if cargoCap > 0 and Hangar.RecordCargoCapacity(target) - cargoCap < cargoTotal(target.cargo) then
			return l.CARGO_RACK_IN_USE
		end
		if lifeSupport > 0 then
			for name in pairs(target.cargo) do
				local c = Commodities[name]
				if c and (c.life_support or 0) > Hangar.RecordLifeSupport(target) - lifeSupport then
					return l.LIFE_SUPPORT_IN_USE
				end
			end
		end
	end
	return nil
end

-- The key an item is fitted under on a ship here
local function keyOf(target, item)
	local _, items = targetState(target)
	for key, it in pairs(items) do
		if it == item then return key end
	end
end

---@param h Hangar.Lease
function Hangar.CanUnfit(h, target, item)
	if Hangar.IsLocked(h) then return false, l.HANGAR_LOCKED end
	if target ~= Hangar.ACTIVE and target.transit then return false, l.ALREADY_IN_TRANSIT end
	local key = keyOf(target, item)
	if key == nil then return false end
	local blocker = unfitBlocker(target, key, item)
	return blocker == nil, blocker
end

-- Unfit an item into the hangar's store
---@param h Hangar.Lease
function Hangar.Unfit(h, target, item)
	local ok, reason = Hangar.CanUnfit(h, target, item)
	if not ok then return false, reason end
	if target == Hangar.ACTIVE then
		if not Game.player:GetComponent('EquipSet'):Remove(item) then return false end
	else
		target.equip[keyOf(target, item)] = nil
		target.check = nil -- its stats changed on purpose
	end
	table.insert(h.store, item)
	return true
end

-- Unfit everything that can come off, items on other items first. Returns how
-- many came off.
---@param h Hangar.Lease
function Hangar.UnfitAll(h, target)
	local count = 0
	local progress = true
	while progress do
		progress = false
		local entries, loose = Hangar.EquipmentOf(target)
		local items = {}
		for _, e in ipairs(entries) do if e.item then table.insert(items, { item = e.item, depth = e.depth }) end end
		for _, e in ipairs(loose) do table.insert(items, { item = e.item, depth = 0 }) end
		table.sort(items, function(a, b) return a.depth > b.depth end)
		for _, e in ipairs(items) do
			if Hangar.Unfit(h, target, e.item) then
				count = count + 1
				progress = true
			end
		end
	end
	return count
end

-- Where an item from the store would go on a ship here: a slot id, the loose
-- marker true, or nil and the reason it can't. `slotId` is a preferred slot.
---@param h Hangar.Lease
function Hangar.FitPlace(h, target, item, slotId)
	if Hangar.IsLocked(h) then return nil, l.HANGAR_LOCKED end
	if target ~= Hangar.ACTIVE and target.transit then return nil, l.ALREADY_IN_TRANSIT end
	local slots, items, used, capacity, shipId = targetState(target)
	local free = capacity - used
	local volume = volumeOn(item, shipId)

	if not item.slot then
		if free < volume then return nil, l.NO_EQUIP_VOLUME end
		return true
	end

	local hardpoint = item.slot.hardpoint or false
	local function fits(id, slot)
		return not items[id] and (slot.hardpoint or false) == hardpoint and EquipSet.CompatibleWithSlot(item, slot)
	end
	local typeMatch, sizeMatch, freeSlot = false, false, nil
	if slotId and slots[slotId] and fits(slotId, slots[slotId]) then freeSlot = slotId end
	local ids = {}
	for id in pairs(slots) do table.insert(ids, id) end
	table.sort(ids)
	for _, id in ipairs(ids) do
		local slot = slots[id]
		if (slot.hardpoint or false) == hardpoint and EquipSet.SlotTypeMatches(item.slot.type, slot.type) then
			typeMatch = true
			if EquipSet.CompatibleWithSlot(item, slot) then
				sizeMatch = true
				if not freeSlot and not items[id] then freeSlot = id end
			end
		end
	end
	if not typeMatch then return nil, l.NO_SLOT_FOR_IT end
	if not sizeMatch then return nil, l.SLOT_WRONG_SIZE end
	if not freeSlot then return nil, l.SLOTS_TAKEN end
	if free < volume then return nil, l.NO_EQUIP_VOLUME end
	return freeSlot
end

-- Fit an item from the store to a ship here
---@param h Hangar.Lease
function Hangar.Fit(h, target, item, slotId)
	local place, reason = Hangar.FitPlace(h, target, item, slotId)
	if not place then return false, reason end
	local _, _, _, _, shipId = targetState(target)
	specializeFor(item, shipId)

	if target == Hangar.ACTIVE then
		local es = Game.player:GetComponent('EquipSet')
		local ok = place == true and es:Install(item) or place ~= true and es:Install(item, es:GetSlotHandle(place))
		if not ok then return false end
	else
		local key = place
		if place == true then
			-- loose items are keyed by a number no other loose item uses
			key = 1
			for k in pairs(target.equip) do
				if type(k) == "number" and k >= key then key = k + 1 end
			end
		end
		target.equip[key] = item
		target.check = nil
	end
	utils.remove_elem(h.store, item)
	return true
end

--=============================================================================
-- Cargo store: goods kept in the hangar, tons held for missions keep their
-- tags. Unlimited and free, like the equipment store.
--=============================================================================

local function storeCargoAdd(h, name, n, tag)
	if n <= 0 then return end
	h.cargo = h.cargo or {}
	h.cargo[name] = (h.cargo[name] or 0) + n
	if tag then
		h.cargoTags = h.cargoTags or {}
		h.cargoTags[name] = h.cargoTags[name] or {}
		h.cargoTags[name][tag] = (h.cargoTags[name][tag] or 0) + n
	end
end

-- Take up to n tons of a lot out of the store; returns how many
local function storeCargoRemove(h, name, n, tag)
	local total = h.cargo and h.cargo[name] or 0
	local tags = h.cargoTags and h.cargoTags[name] or {}
	local held = 0
	for _, c in pairs(tags) do held = held + c end
	local available = tag == ORDINARY and total - held or tags[tag] or 0
	n = math.min(n, available)
	if n <= 0 then return 0 end
	if tag ~= ORDINARY then
		tags[tag] = tags[tag] - n > 0 and tags[tag] - n or nil
		if not next(tags) then h.cargoTags[name] = nil end
	end
	h.cargo[name] = total - n > 0 and total - n or nil
	return n
end

-- The store's goods by lot: per commodity, mission tons first
---@return Hangar.CargoLot[]
function Hangar.StoreCargoLots(h)
	local lots = {}
	for name, total in pairs(h.cargo or {}) do
		local ordinary = total
		local held = {}
		for tag, n in pairs(h.cargoTags and h.cargoTags[name] or {}) do
			ordinary = ordinary - n
			table.insert(held, { name = name, tag = tag, count = n, label = Hangar.TagLabel(tag) })
		end
		table.sort(held, function(a, b) return a.label < b.label end)
		for _, lot in ipairs(held) do table.insert(lots, lot) end
		if ordinary > 0 then table.insert(lots, { name = name, tag = ORDINARY, count = ordinary }) end
	end
	table.sort(lots, function(a, b)
		if a.name ~= b.name then return a.name < b.name end
		return a.tag ~= ORDINARY and b.tag == ORDINARY
	end)
	return lots
end

function Hangar.StoreCargoTotal(h)
	return cargoTotal(h.cargo or {})
end

-- Unload up to n tons of a lot of the active ship's cargo into the store
---@param h Hangar.Lease
function Hangar.UnloadCargo(h, name, tag, n)
	if Hangar.IsLocked(h) then return 0, l.HANGAR_LOCKED end
	local removed = Game.player:GetComponent('CargoManager'):RemoveCommodity(Commodities[name], n, tag)
	storeCargoAdd(h, name, removed, tag ~= ORDINARY and tag or nil)
	return removed
end

-- How many tons of a store lot the active ship can take now, and why not
-- more when it's none
function Hangar.CanLoadCargo(h, name, tag)
	if Hangar.IsLocked(h) then return 0, l.HANGAR_LOCKED end
	local commodity = Commodities[name]
	local player = Game.player
	if (commodity.life_support or 0) > (player:hasprop("cargo_life_support_cap") and player.cargo_life_support_cap or 0) then
		return 0, l.NEEDS_CARGO_LIFE_SUPPORT
	end
	local room = math.floor(player:GetComponent('CargoManager'):GetFreeSpace() / (commodity.mass or 1))
	if room <= 0 then return 0, l.HOLD_FULL end
	return room
end

-- Load up to n tons of a store lot into the active ship
---@param h Hangar.Lease
function Hangar.LoadCargo(h, name, tag, n)
	local room, reason = Hangar.CanLoadCargo(h, name, tag)
	if room <= 0 then return 0, reason end
	local taken = storeCargoRemove(h, name, math.min(n, room), tag)
	if taken > 0 then
		Game.player:GetComponent('CargoManager'):AddCommodity(Commodities[name], taken, tag ~= ORDINARY and tag or nil)
	end
	return taken
end

-- What the station here pays for an item from the store: the Equipment
-- Market's resale price
---@param station SpaceStation
function Hangar.SellPrice(station, item)
	return (rawget(item, "price") or station:GetEquipmentPrice(item:GetPrototype())) * Economy.BaseResellPriceModifier
end

-- Sell an item from the store to the station the hangar is at
---@param h Hangar.Lease
function Hangar.SellFromStore(h, item)
	local station = Game.player:GetDockedWith()
	if not station or Hangar.StationKey(station) ~= h.key then return false end
	if Hangar.IsLocked(h) then return false, l.HANGAR_LOCKED end
	if not utils.contains(h.store, item) then return false end
	PlayerState.AddMoney(Hangar.SellPrice(station, item))
	station:AddEquipmentStock(item:GetPrototype(), 1)
	utils.remove_elem(h.store, item)
	return true
end

--=============================================================================
-- Transfers: the delivery service for ships, equipment and crew
--
-- Paid when booked; the time and price grow with the distance. A ship on its
-- way holds its bay at the destination (its record's hangar is already the
-- destination, with `transit` set). Cancelling refunds half and sends it back,
-- taking as long as it has travelled so far.
--=============================================================================

---@class Hangar.Transfer
---@field id integer
---@field kind "ship"|"equipment"|"crew"
---@field rec Hangar.Record?
---@field items EquipType[]?
---@field crew Character[]?
---@field from Hangar.Place
---@field to Hangar.Place
---@field departed number
---@field arrives number
---@field cost number
---@field returning boolean?

local DAY = 86400

local function placeOfHangar(h)
	return { key = h.key, path = h.path, station = h.station, factor = h.factor }
end

local function lightYears(a, b)
	if a:IsSameSystem(b) then return 0 end
	return a:DistanceTo(b)
end

-- Price, travel time in seconds and light years for delivering a ship
function Hangar.ShipDeliveryQuote(shipId, fromPath, toPath)
	local ly = lightYears(fromPath, toPath)
	local mass = hullMass(shipId)
	local cost = ly == 0 and 50 + 0.2 * mass or 40 + 60 * ly + 0.3 * mass * ly
	return round(cost), (1 + 0.5 * ly) * DAY, ly
end

-- ... for equipment (one booking carries several items)
function Hangar.EquipmentDeliveryQuote(items, fromPath, toPath)
	local ly = lightYears(fromPath, toPath)
	local mass = 0
	for _, item in ipairs(items) do mass = mass + (item.mass or 0) end
	local cost = ly == 0 and 20 or 30 + 0.5 * mass * ly
	return round(cost), (1 + 0.5 * ly) * DAY, ly
end

-- ... for crew (one ticket each)
function Hangar.CrewTicketQuote(count, fromPath, toPath)
	local ly = lightYears(fromPath, toPath)
	local cost = count * (ly == 0 and 15 or 20 + 30 * ly)
	return round(cost), (1 + 0.4 * ly) * DAY, ly
end

-- Surface starports only take ships that can land: ones with a hull slot
-- for atmospheric shielding, as their Ship Markets sell
local function canLandAt(shipId, h)
	local body = h.path:GetSystemBody()
	if not body or body.type ~= "STARPORT_SURFACE" then return true end
	for _, slot in pairs(HullConfig.GetHullConfig(shipId).slots) do
		if slot.type:match("^hull") then return true end
	end
	return false
end

---@return Hangar.Transfer[]
function Hangar.GetTransfers()
	local list = utils.build_array(pairs(transfers))
	table.sort(list, function(a, b) return a.arrives < b.arrives end)
	return list
end

-- Transfers of `kind` going to the place with `key`
function Hangar.TransfersTo(key, kind)
	local list = {}
	for _, t in ipairs(Hangar.GetTransfers()) do
		if t.to.key == key and (not kind or t.kind == kind) then table.insert(list, t) end
	end
	return list
end

---@param rec Hangar.Record
function Hangar.TransferOf(rec)
	return rec.transit and transfers[rec.transit]
end

local scheduleArrivals

local function book(t)
	t.id = nextTransferId
	nextTransferId = nextTransferId + 1
	transfers[t.id] = t
	PlayerState.AddMoney(-t.cost)
	scheduleArrivals()
	return t
end

-- A warning when ship type `shipId` fits none of the docking ports at the
-- station at `path` (named `station`): once it leaves, it can't dock there
-- again. Nil when it fits, or when the game can't tell (an older build
-- without Game.CanDockAt).
function Hangar.DockWarning(shipId, path, station)
	if not Game.CanDockAt then return nil end
	local ok, fits = pcall(Game.CanDockAt, path, shipId)
	if ok and fits == false then
		return string.interp(l.DOCK_SIZE_WARNING, { station = station })
	end
	return nil
end

-- Why ship `rec` can't be sent to hangar `to` now, or nil
function Hangar.SendShipBlocked(rec, to)
	local from = hangars[rec.hangar]
	if rec.transit then return l.ALREADY_IN_TRANSIT end
	if not from or not to or from == to then return l.NO_DESTINATION end
	if Hangar.IsLocked(from) or Hangar.IsLocked(to) then return l.HANGAR_LOCKED end
	if Hangar.FreeBays(to) < 1 then return string.interp(l.NO_FREE_BAY_AT, { station = to.station }) end
	if not canLandAt(rec.shipId, to) then return string.interp(l.CANT_LAND_AT, { station = to.station }) end
	local cost = Hangar.ShipDeliveryQuote(rec.shipId, from.path, to.path)
	if PlayerState.GetMoney() < cost then return l.NOT_ENOUGH_MONEY end
	return nil
end

-- Book the delivery of a stored ship to another of your hangars
function Hangar.SendShip(rec, to)
	local blocked = Hangar.SendShipBlocked(rec, to)
	if blocked then return false, blocked end
	local from = hangars[rec.hangar]
	local cost, time = Hangar.ShipDeliveryQuote(rec.shipId, from.path, to.path)
	accrue(from, Game.time) -- its parking ends here
	accrue(to, Game.time)
	local t = book({ kind = "ship", rec = rec, from = placeOfHangar(from), to = placeOfHangar(to),
		departed = Game.time, arrives = Game.time + time, cost = cost })
	rec.transit = t.id
	rec.hangar = to.key
	rec.order = nextId
	nextId = nextId + 1
	return true, t
end

-- Why equipment can't be sent from hangar `from` to `to` now, or nil
function Hangar.SendEquipmentBlocked(from, to, items)
	if not from or not to or from == to then return l.NO_DESTINATION end
	if #items == 0 then return l.NOTHING_PICKED end
	if Hangar.IsLocked(from) or Hangar.IsLocked(to) then return l.HANGAR_LOCKED end
	for _, item in ipairs(items) do
		if not utils.contains(from.store, item) then return l.NO_DESTINATION end
	end
	local cost = Hangar.EquipmentDeliveryQuote(items, from.path, to.path)
	if PlayerState.GetMoney() < cost then return l.NOT_ENOUGH_MONEY end
	return nil
end

-- Book a delivery of items from one hangar's store to another's
function Hangar.SendEquipment(from, to, items)
	local blocked = Hangar.SendEquipmentBlocked(from, to, items)
	if blocked then return false, blocked end
	local cost, time = Hangar.EquipmentDeliveryQuote(items, from.path, to.path)
	for _, item in ipairs(items) do utils.remove_elem(from.store, item) end
	return true, book({ kind = "equipment", items = utils.build_array(ipairs(items)), from = placeOfHangar(from),
		to = placeOfHangar(to), departed = Game.time, arrives = Game.time + time, cost = cost })
end

-- Places crew can be sent to: your hangars' stations, lounges in use, and
-- the stations of the current system. `exceptKey` is left out.
---@return Hangar.Place[]
function Hangar.CrewDestinations(exceptKey)
	local seen, list = {}, {}
	local function add(place)
		if place.key ~= exceptKey and not seen[place.key] then
			seen[place.key] = true
			table.insert(list, place)
		end
	end
	for _, h in ipairs(Hangar.GetHangars()) do add(placeOfHangar(h)) end
	for _, lounge in ipairs(Hangar.GetLounges()) do
		add({ key = lounge.key, path = lounge.path, station = lounge.station, factor = lounge.factor })
	end
	if Game.system then
		local here = {}
		for _, station in ipairs(Space.GetBodies("SpaceStation")) do table.insert(here, Hangar.PlaceOf(station)) end
		table.sort(here, function(a, b) return a.station < b.station end)
		for _, place in ipairs(here) do add(place) end
	end
	return list
end

-- Why people waiting in the lounge `fromKey` can't be sent to `to`, or nil
function Hangar.SendCrewBlocked(fromKey, to, people)
	local lounge = lounges[fromKey]
	if not lounge or not to or to.key == fromKey then return l.NO_DESTINATION end
	if #people == 0 then return l.NOTHING_PICKED end
	local blocked = Hangar.LoungeBlocked()
	if blocked then return blocked end
	for _, c in ipairs(people) do
		if c.waitingAt ~= fromKey then return l.NO_DESTINATION end
	end
	local cost = Hangar.CrewTicketQuote(#people, lounge.path, to.path)
	if PlayerState.GetMoney() < cost then return l.NOT_ENOUGH_MONEY end
	return nil
end

-- Book passage for people waiting in a lounge to the lounge at `to`
---@param to Hangar.Place
function Hangar.SendCrew(fromKey, to, people)
	local blocked = Hangar.SendCrewBlocked(fromKey, to, people)
	if blocked then return false, blocked end
	local lounge = lounges[fromKey]
	local from = { key = lounge.key, path = lounge.path, station = lounge.station, factor = lounge.factor }
	local cost, time = Hangar.CrewTicketQuote(#people, from.path, to.path)
	for _, c in ipairs(people) do
		local l2, entry, index = Hangar.FindInLounge(c)
		removeFromLounge(l2, entry, index)
	end
	local t = book({ kind = "crew", crew = utils.build_array(ipairs(people)), from = from, to = to,
		departed = Game.time, arrives = Game.time + time, cost = cost })
	for _, c in ipairs(people) do c.inTransit = t.id end
	return true, t
end

-- Why a transfer can't be cancelled now, or nil
function Hangar.CancelBlocked(t)
	if t.returning then return l.ALREADY_RETURNING end
	if t.kind == "ship" then
		local origin = hangars[t.from.key]
		if not origin then return l.ORIGIN_GONE end
		if Hangar.FreeBays(origin) < 1 then return string.interp(l.NO_BAY_TO_RETURN, { station = origin.station }) end
	elseif t.kind == "equipment" and not hangars[t.from.key] then
		return l.ORIGIN_GONE
	end
	return nil
end

-- When a cancelled transfer would be back
function Hangar.ReturnTime(t)
	return Game.time + (Game.time - t.departed)
end

-- Cancel: half the price back, and it goes back where it came from
function Hangar.CancelTransfer(t)
	local blocked = Hangar.CancelBlocked(t)
	if blocked then return false, blocked end
	PlayerState.AddMoney(round(t.cost * 0.5))
	t.arrives = Hangar.ReturnTime(t)
	t.departed = Game.time
	t.from, t.to = t.to, t.from
	t.returning = true
	if t.kind == "ship" then
		t.rec.hangar = t.to.key -- its bay at the destination is free again
	end
	scheduleArrivals()
	return true
end

local function arrive(t)
	transfers[t.id] = nil
	if t.kind == "ship" then
		local h = hangars[t.to.key]
		if h then accrue(h, Game.time) end
		t.rec.transit = nil
		t.rec.storedAt = Game.time
		Comms.ImportantMessage(string.interp(t.returning and l.SHIP_RETURNED or l.SHIP_ARRIVED,
			{ ship = t.rec.name, station = t.to.station }), l.DELIVERY_SERVICE)
	elseif t.kind == "equipment" then
		local h = hangars[t.to.key]
		if h then
			for _, item in ipairs(t.items) do table.insert(h.store, item) end
		end
		Comms.ImportantMessage(string.interp(l.EQUIPMENT_ARRIVED,
			{ count = #t.items, station = t.to.station }), l.DELIVERY_SERVICE)
	elseif t.kind == "crew" then
		for _, c in ipairs(t.crew) do
			c.inTransit = nil
			Hangar.AddToLoungeAt(t.to, c)
			Comms.ImportantMessage(string.interp(l.CREW_ARRIVED, { name = c.name, station = t.to.station }), l.DELIVERY_SERVICE)
		end
	end
end

-- Complete every transfer due by now, in order, then wait for the next
local function processArrivals()
	for _, t in ipairs(Hangar.GetTransfers()) do
		if t.arrives <= Game.time then arrive(t) end
	end
	scheduleArrivals()
end

scheduleArrivals = function()
	transferToken = transferToken + 1
	local token = transferToken
	local next = nil
	for _, t in pairs(transfers) do
		if not next or t.arrives < next then next = t.arrives end
	end
	if next then
		Timer:CallAt(math.max(next, Game.time + 1), function()
			if token == transferToken then processArrivals() end
		end)
	end
end

--=============================================================================
-- Ship records
--=============================================================================

---@param rec Hangar.Record
function Hangar.RecordCargoCapacity(rec)
	return ShipDef[rec.shipId].cargo + sumCapability(rec.equip, "cargo")
end

---@param rec Hangar.Record
function Hangar.RecordLifeSupport(rec)
	return sumCapability(rec.equip, "cargo_life_support")
end

---@param rec Hangar.Record
function Hangar.RecordHasHyperdrive(rec)
	for _, item in pairs(rec.equip) do
		if item.slot and item.slot.type:match("^hyperdrive") then return true end
	end
	return false
end

-- Snapshot the player's ship into a new record. Doesn't change the ship.
---@param cargo table<string, integer> cargo that stays with the stored ship
---@param cargoTags table<string, table<any, integer>> how much of it missions hold
local function snapshotPlayer(cargo, cargoTags)
	local player = Game.player
	local equipSet = player:GetComponent('EquipSet')
	local equip = {}
	for k, item in pairs(equipSet:GetInstalledEquipment()) do
		-- Items without a slot are listed by position, but the slots they
		-- provide are named after their cache index: keep them under that
		if type(k) == "number" then k = equipSet.cache[item] end
		equip[k] = item
	end

	local state = {}
	for name, hooks in pairs(shipStates) do
		state[name] = hooks.get()
	end

	return {
		shipId = player.shipId,
		name = player.shipName,
		label = player.label,
		skin = player:GetSkin(),
		pattern = player.model.pattern,
		hull = player.hullPercent,
		fuel = player.fuel,
		equip = equip,
		cargo = cargo,
		cargoTags = cargoTags,
		state = state,
		check = checkProps(player),
	}
end

-- Install a record's equipment on the player's (empty) ship: hull slots
-- first, then items without a slot, then items in slots provided by other
-- items, parents before children. Items without a slot get new cache indexes,
-- so compound slot ids are remapped. Returns the items that couldn't be fitted.
---@param equip table<string|integer, EquipType>
local function installEquipment(equip)
	local equipSet = Game.player:GetComponent('EquipSet')

	local function depth(k)
		if type(k) ~= "string" then return 0 end
		local _, n = k:gsub("##", "")
		return n
	end

	local entries = {}
	for k, item in pairs(equip) do table.insert(entries, { key = k, item = item }) end
	table.sort(entries, function(a, b)
		local da, db = depth(a.key), depth(b.key)
		if da ~= db then return da < db end
		if type(a.key) ~= type(b.key) then return type(a.key) == "string" end
		return a.key < b.key
	end)

	local idMap = {}
	local failed = {}

	for _, e in ipairs(entries) do
		local ok = false
		if type(e.key) == "number" then
			ok = equipSet:Install(e.item)
			if ok then idMap[tostring(e.key)] = tostring(equipSet.cache[e.item]) end
		else
			local first, rest = e.key:match("^([^#]+)(.*)$")
			local id = (idMap[first] or first) .. rest
			local handle = equipSet:GetSlotHandle(id)
			ok = handle ~= nil and equipSet:Install(e.item, handle)
		end
		if not ok then
			logWarning("Hangar: could not refit {} ({}) on {}" % { e.item:GetName(), tostring(e.key), Game.player.shipId })
			table.insert(failed, e.item)
		end
	end

	return failed
end

---@param rec Hangar.Record
local function compareChecks(rec)
	if not rec.check then return end
	local now = checkProps(Game.player)
	for k, v in pairs(rec.check) do
		local n = now[k] or 0
		if math.abs(n - v) > 0.01 then
			logWarning("Hangar: '{}' {} was {} when stored, {} after taking it out" % { rec.name, k, v, n })
		end
	end
	for k, n in pairs(now) do
		if rec.check[k] == nil and math.abs(n) > 0.01 then
			logWarning("Hangar: '{}' {} is {} after taking it out, missing when stored" % { rec.name, k, n })
		end
	end
end

--=============================================================================
-- Swapping
--=============================================================================

-- Who would be aboard after a swap: `aboard` is the set of characters picked
-- to fly on (crew aboard and crew in the lounge here), nil for the crew
-- aboard now
local function crewAfter(aboard)
	if not aboard then return Game.player:CrewNumber() end
	local n = 0
	for _ in pairs(aboard) do n = n + 1 end
	return n
end

-- Checks shared by swapping and by buying a ship while storing the current
-- one: being aboard, the rover, passengers, crew for ship `def` named `name`,
-- and the cargo taken along into `room` tons with `lifeSupport`
local function checkBoarding(blockers, warnings, def, name, take, aboard, room, lifeSupport)
	local player = Game.player

	if Game.GetExcursionMode and Game.GetExcursionMode() ~= "ship" then
		table.insert(blockers, l.NOT_ABOARD)
	end

	local roverPlanet = Game.GetRoverPlanet and Game.GetRoverPlanet()
	if roverPlanet then
		local body = roverPlanet:GetSystemBody()
		table.insert(blockers, string.interp(l.ROVER_OUT, { planet = body and body.name or "?" }))
	end

	if Passengers.CountOccupiedBerths(player) > 0 then
		table.insert(blockers, l.PASSENGERS_ABOARD)
	end

	Hangar.CheckCrew(blockers, warnings, def, name, aboard)

	local taken = Hangar.TakeTotal(take)
	if taken > room then
		table.insert(blockers, string.interp(l.TOO_MUCH_CARGO, { ship = name, room = room, over = taken - room }))
	end

	for cname, parts in pairs(take) do
		local commodity = Commodities[cname]
		local n = 0
		for _, c in pairs(parts) do n = n + c end
		if n > 0 and commodity and (commodity.life_support or 0) > lifeSupport then
			table.insert(blockers, string.interp(l.NEEDS_LIFE_SUPPORT, { commodity = commodity:GetName() }))
		end
	end
end

-- Crew checks for flying ship `def` (named `name`) with `aboard`: the
-- maximum blocks, the minimum warns, and leaving anyone in the lounge needs
-- the lounge fees paid
function Hangar.CheckCrew(blockers, warnings, def, name, aboard)
	if aboard and Hangar.LoungeBlocked() then
		for member in Game.player:EachCrewMember() do
			if not member.player and not aboard[member] then
				table.insert(blockers, Hangar.LoungeBlocked())
				break
			end
		end
	end

	local crew = crewAfter(aboard)
	if crew > def.maxCrew then
		table.insert(blockers, string.interp(l.TOO_MANY_CREW, { ship = name, max = def.maxCrew, crew = crew, over = crew - def.maxCrew }))
	elseif crew < def.minCrew then
		table.insert(warnings, string.interp(l.TOO_FEW_CREW, { ship = name, min = def.minCrew, crew = crew }))
	end
end

-- Crew never stays on a stored ship: who isn't in `aboard` waits in the
-- lounge here, and lounge crew in `aboard` come aboard. Call after the
-- player's ship has its new type, so its crew limit applies.
function Hangar.MoveCrew(aboard)
	if not aboard then return end
	local player = Game.player
	local leaving = {}
	for member in player:EachCrewMember() do
		if not aboard[member] and not member.player then table.insert(leaving, member) end
	end
	for _, member in ipairs(leaving) do Hangar.SendToLounge(member) end
	for member in pairs(aboard) do
		if member.waitingAt then Hangar.TakeFromLounge(member) end
	end
end

-- Take the player's ship as a record, leaving aboard only the cargo in
-- `take`: the rest stays with the record. Changes the player's cargo.
local function storePlayer(take)
	local cargoMgr = Game.player:GetComponent('CargoManager')

	local leave, leaveTags, leaveOrdinary = {}, {}, {}
	for _, lot in ipairs(Hangar.CargoLots()) do
		local taken = take[lot.name] and take[lot.name][lot.tag] or 0
		local n = lot.count - math.clamp(taken, 0, lot.count)
		if n > 0 then
			leave[lot.name] = (leave[lot.name] or 0) + n
			if lot.tag == ORDINARY then
				leaveOrdinary[lot.name] = n
			else
				leaveTags[lot.name] = leaveTags[lot.name] or {}
				leaveTags[lot.name][lot.tag] = n
			end
		end
	end

	local out = snapshotPlayer(leave, leaveTags)

	for name, n in pairs(leaveOrdinary) do
		cargoMgr:RemoveCommodity(Commodities[name], n, ORDINARY)
	end
	for name, tags in pairs(leaveTags) do
		for tag, n in pairs(tags) do
			cargoMgr:RemoveCommodity(Commodities[name], n, tag)
		end
	end
	return out
end

-- Reasons a swap can't happen (blockers) and things the player should know
-- (warnings). `take` is the cargo to take along (see Cargo on a swap),
-- `aboard` who flies on (see crewAfter).
---@param rec Hangar.Record
---@param take table<string, table<any, integer>>
---@param aboard table<Character, boolean>?
function Hangar.CheckSwap(rec, take, aboard)
	local player = Game.player
	local blockers, warnings = {}, {}
	local station = player:GetDockedWith()
	local def = ShipDef[rec.shipId]

	if not station or Hangar.StationKey(station) ~= rec.hangar then
		table.insert(blockers, l.NOT_DOCKED_HERE)
	end

	local h = hangars[rec.hangar]
	if h and Hangar.IsLocked(h) then
		table.insert(blockers, l.HANGAR_LOCKED)
	end
	if rec.transit then
		table.insert(blockers, l.ALREADY_IN_TRANSIT)
	end

	checkBoarding(blockers, warnings, def, rec.name, take, aboard,
		Hangar.RecordFreeSpace(rec), Hangar.RecordLifeSupport(rec))

	local here = hangars[rec.hangar]
	local dock = here and Hangar.DockWarning(rec.shipId, here.path, here.station)
	if dock then table.insert(warnings, dock) end

	if rec.hull < 70 or rec.fuel < 50 then
		table.insert(warnings, string.interp(l.LOW_HULL_FUEL, {
			ship = rec.name, hull = string.format("%d", rec.hull), fuel = string.format("%d", rec.fuel)
		}))
	end

	return blockers, warnings
end

-- Store the player's ship in its bay and take the stored ship `rec` out.
---@param rec Hangar.Record
---@param take table<string, table<any, integer>> cargo to take along, by lot (see Cargo on a swap)
---@param aboard table<Character, boolean>? who flies on; the rest of the crew waits in the lounge here
function Hangar.Swap(rec, take, aboard)
	local blockers = Hangar.CheckSwap(rec, take, aboard)
	if #blockers > 0 then return false, blockers[1] end

	local player = Game.player
	local h = assert(hangars[rec.hangar])
	local cargoMgr = player:GetComponent('CargoManager')

	accrue(h, Game.time)
	local out = storePlayer(take)

	-- Rebuild the player's ship from the record. SetShipType removes all
	-- equipment (the stored ship keeps the item instances) and keeps cargo.
	player:SetShipType(rec.shipId)
	player:SetSkin(rec.skin)
	if rec.pattern then player.model:SetPattern(rec.pattern) end
	player:SetLabel(rec.label)
	player:SetShipName(rec.name)

	Hangar.MoveCrew(aboard)

	local failed = installEquipment(rec.equip)
	for _, item in ipairs(failed) do table.insert(h.store, item) end

	player:SetHullPercent(math.clamp(rec.hull, 0, 100))
	player:SetFuelPercent(math.clamp(rec.fuel, 0, 100))

	-- The stored ship's cargo comes aboard with its tags
	local function keep(name, n, tag)
		-- CheckSwap made room for it; keep it rather than lose it
		logWarning("Hangar: no room for {} t {} from '{}'" % { n, name, rec.name })
		out.cargo[name] = (out.cargo[name] or 0) + n
		if tag then
			out.cargoTags[name] = out.cargoTags[name] or {}
			out.cargoTags[name][tag] = (out.cargoTags[name][tag] or 0) + n
		end
	end
	for name, total in pairs(rec.cargo) do
		local commodity = Commodities[name]
		local ordinary = total
		for tag, n in pairs(rec.cargoTags and rec.cargoTags[name] or {}) do
			ordinary = ordinary - n
			if not cargoMgr:AddCommodity(commodity, n, tag) then keep(name, n, tag) end
		end
		if ordinary > 0 and not cargoMgr:AddCommodity(commodity, ordinary) then keep(name, ordinary) end
	end

	compareChecks(rec)

	-- The stored ship goes into the bay the other one leaves
	ships[rec.id] = nil
	out.id = nextId
	nextId = nextId + 1
	out.hangar = rec.hangar
	out.storedAt = Game.time
	out.order = rec.order or rec.id
	ships[out.id] = out

	-- Per-ship module state is restored after the queued onShipTypeChanged
	Event.Queue("onHangarShipSwapped", player, rec.state or {})

	if #failed > 0 then
		Comms.ImportantMessage(string.interp(l.ITEMS_MOVED_TO_STORE, { count = #failed }), h.station)
	end

	return true, out
end

--=============================================================================
-- Buying a ship (Ship Market)
--=============================================================================

-- Cargo space and life support of a new ship with `equip` fitted ({ slot id,
-- item } pairs, as the Ship Market lists its default equipment)
function Hangar.NewShipCargo(def, equip)
	local room, lifeSupport = def.cargo, 0
	for _, pair in ipairs(equip) do
		local caps = pair[2].capabilities or {}
		room = room + (caps.cargo or 0)
		lifeSupport = lifeSupport + (caps.cargo_life_support or 0)
	end
	return room, lifeSupport
end

-- How the current ship is dealt with when buying another:
--   "tradein" - sold with everything on it, all cargo moves to the new ship
--   "store"   - put in a free bay here as it is, with its equipment and cargo
--   "hull"    - the bare hull is sold; its equipment goes to the hangar's
--               store, its cargo to the hangar's cargo store and the crew to
--               the lounge
Hangar.BUY_MODES = { "tradein", "store", "hull" }

-- Why a buy mode can't be used here now (nil when it can)
function Hangar.BuyModeBlocked(mode)
	if mode == "tradein" then return nil end
	local h = Hangar.GetHangarAt(Game.player:GetDockedWith())
	if not h then return l.STORE_NEEDS_HANGAR end
	if Hangar.IsLocked(h) then return l.HANGAR_LOCKED end
	if mode == "store" and Hangar.FreeBays(h) < 1 then return l.STORE_NEEDS_BAY end
	return nil
end

-- Everyone aboard but the commander waits in the lounge ("hull" mode)
function Hangar.OnlyCommander()
	for member in Game.player:EachCrewMember() do
		if member.player then return { [member] = true } end
	end
	return {}
end

-- Reasons ship `def` can't be bought here now with `mode` (see BUY_MODES).
-- `aboard` is who flies the new ship (nil: the crew aboard now).
function Hangar.CheckBuy(def, equip, mode, aboard)
	local blockers, warnings = {}, {}
	local name = def.name
	local room, lifeSupport = Hangar.NewShipCargo(def, equip)

	local blocked = Hangar.BuyModeBlocked(mode)
	if blocked then table.insert(blockers, blocked) end

	if mode == "store" then
		-- nothing comes along but the crew
		checkBoarding(blockers, warnings, def, name, {}, aboard, room, lifeSupport)
	elseif mode == "hull" then
		checkBoarding(blockers, warnings, def, name, {}, Hangar.OnlyCommander(), room, lifeSupport)
	else
		Hangar.CheckCrew(blockers, warnings, def, name, aboard)
		local used = Game.player:GetComponent('CargoManager'):GetUsedSpace()
		if used > room then
			table.insert(blockers, string.interp(l.TRADE_IN_CARGO, {
				ship = name, room = room, used = used, over = used - room, current = Game.player.shipName }))
		end
	end

	-- the Ship Market doesn't check docking ports either
	local station = Game.player:GetDockedWith()
	local dock = station and Hangar.DockWarning(def.id, station.path, station.label)
	if dock then table.insert(warnings, dock) end
	return blockers, warnings
end

-- Before the bare hull is sold: all cargo into the hangar's cargo store, all
-- crew but the commander into the lounge, all equipment into the store.
-- Returns tons, crew and items moved.
function Hangar.StripCurrentShip()
	local h = Hangar.GetHangarAt(Game.player:GetDockedWith())
	if not h or Hangar.IsLocked(h) then return false end
	local tons = 0
	for _, lot in ipairs(Hangar.CargoLots()) do
		tons = tons + Hangar.UnloadCargo(h, lot.name, lot.tag, lot.count)
	end
	local crew = Game.player:CrewNumber()
	Hangar.MoveCrew(Hangar.OnlyCommander())
	crew = crew - Game.player:CrewNumber()
	local items = Hangar.UnfitAll(h, Hangar.ACTIVE)
	return true, tons, crew, items
end

-- Put the player's current ship into a free bay of the hangar here, keeping
-- aboard only the cargo in `take` (nil: none). The caller then makes the
-- player's ship the new one and calls Hangar.MoveCrew.
function Hangar.StoreCurrentShip(take)
	local h = Hangar.GetHangarAt(Game.player:GetDockedWith())
	if not h or Hangar.FreeBays(h) < 1 or Hangar.IsLocked(h) then return false end
	accrue(h, Game.time)
	local rec = storePlayer(take or {})
	rec.id = nextId
	nextId = nextId + 1
	rec.hangar = h.key
	rec.storedAt = Game.time
	rec.order = rec.id
	ships[rec.id] = rec
	return true, rec
end

---@param rec Hangar.Record
function Hangar.RenameStored(rec, name)
	name = name and name:match("^%s*(.-)%s*$") or ""
	if #name > 0 then rec.name = name end
end

---@return Hangar.Record[]
function Hangar.GetAllShips()
	local list = utils.build_array(pairs(ships))
	table.sort(list, function(a, b) return a.id < b.id end)
	return list
end

-- Add a ship record directly (debug tool, later the Ship Market). The record
-- needs shipId, name, label, skin, pattern, hull, fuel, equip, cargo.
---@param h Hangar.Lease
function Hangar.AddRecord(h, rec)
	if Hangar.FreeBays(h) < 1 then return false end
	accrue(h, Game.time)
	rec.id = nextId
	nextId = nextId + 1
	rec.hangar = h.key
	rec.storedAt = Game.time
	rec.order = rec.id
	rec.cargo = rec.cargo or {}
	rec.equip = rec.equip or {}
	rec.state = rec.state or {}
	ships[rec.id] = rec
	return true, rec
end

-- Debug: bill a hangar now as if a whole month had passed at today's rent
---@param h Hangar.Lease
function Hangar.DebugBillMonth(h)
	accrue(h, Game.time)
	h.accrued = h.accrued + Hangar.MonthlyRent(h)
	return billHangar(h, Game.time)
end

-- Debug: drop the upfront credit, so the next bill comes from money
---@param h Hangar.Lease
function Hangar.DebugClearCredit(h)
	h.credit, h.creditCost = 0, 0
end

--=============================================================================
-- Events and saving
--=============================================================================

--=============================================================================
-- Messages
--=============================================================================

-- What you have at a station: ships stored here (and on their way here),
-- crew waiting here, and the hangar (nil without one)
function Hangar.SummaryAt(station)
	local key = Hangar.StationKey(station)
	local h = hangars[key]
	local stored, incoming = 0, 0
	if h then
		for _, rec in ipairs(Hangar.StoredShips(h)) do
			if rec.transit then incoming = incoming + 1 else stored = stored + 1 end
		end
	end
	local lounge = lounges[key]
	return { hangar = h, ships = stored, incoming = incoming, crew = lounge and #lounge.crew or 0 }
end

-- Docking where you have a hangar or crew waiting
local function onPlayerDocked(player, station)
	local sum = Hangar.SummaryAt(station)
	if not sum.hangar and sum.crew == 0 then return end
	local parts = {}
	if sum.hangar then
		table.insert(parts, string.interp(l.WELCOME_HANGAR, { ships = sum.ships, bays = sum.hangar.bays }))
	end
	if sum.crew > 0 then
		table.insert(parts, string.interp(l.WELCOME_CREW, { crew = sum.crew }))
	end
	Comms.Message(l.WELCOME_BACK .. " " .. table.concat(parts, " "), station.label)
end

-- A week before prepaid rent runs out, once per payment
local function checkCredit()
	for _, h in pairs(hangars) do
		local paidUntil = Hangar.PaidUntil(h)
		if paidUntil and not h.creditWarned and paidUntil - Game.time < 7 * 86400 then
			h.creditWarned = true
			Comms.ImportantMessage(string.interp(l.CREDIT_RUNNING_OUT, {
				station = h.station, date = Format.DateOnly(paidUntil) }), h.station)
		end
	end
end

Event.Register("onShipDocked", function(ship, station)
	if ship:IsPlayer() then onPlayerDocked(ship, station) end
end)

Event.Register("onHangarShipSwapped", function(ship, state)
	if not ship:IsPlayer() then return end
	for name, hooks in pairs(shipStates) do
		if state[name] ~= nil then hooks.set(state[name]) end
	end
end)

Event.Register("onGameStart", function()
	if loaded_data then
		hangars = loaded_data.hangars or {}
		ships = loaded_data.ships or {}
		nextId = loaded_data.nextId or 1
		nextBill = loaded_data.nextBill
		lounges = loaded_data.lounges or {}
		loungeDebt = loaded_data.loungeDebt or 0
		transfers = loaded_data.transfers or {}
		nextTransferId = loaded_data.nextTransferId or 1
		for _, h in pairs(hangars) do h.store = h.store or {} end
		loaded_data = nil
	end

	-- Missions that ended while the save held their cargo on a stored ship
	-- or in a hangar
	local holders = utils.build_array(pairs(ships))
	for _, h in pairs(hangars) do table.insert(holders, h) end
	for _, holder in ipairs(holders) do
		for _, tags in pairs(holder.cargoTags or {}) do
			for tag in pairs(tags) do
				local meta = getmetatable(tag)
				if meta and meta.class == "Mission" and not tag:IsActive() then releaseTag(tag) end
			end
		end
	end

	runBilling()
	processArrivals()
	checkCredit()
	Timer:CallEvery(86400, function()
		if not Game.player then return true end
		checkCredit()
	end)
end)

Event.Register("onMissionRemoved", releaseTag)

Event.Register("onGameEnd", function()
	hangars = {}
	ships = {}
	lounges = {}
	loungeDebt = 0
	transfers = {}
	nextTransferId = 1
	transferToken = transferToken + 1
	nextId = 1
	nextBill = nil
	billingToken = billingToken + 1
end)

local serialize = function()
	return { hangars = hangars, ships = ships, nextId = nextId, nextBill = nextBill, lounges = lounges, loungeDebt = loungeDebt,
		transfers = transfers, nextTransferId = nextTransferId }
end

local unserialize = function(data)
	loaded_data = data
end

Serializer:Register("Hangar", serialize, unserialize)

return Hangar
