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
local Event = require 'Event'
local Game = require 'Game'
local HullConfig = require 'HullConfig'
local Lang = require 'Lang'
local Passengers = require 'Passengers'
local PlayerState = require 'PlayerState'
local Serializer = require 'Serializer'
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
	local take = {}
	local room = Hangar.RecordFreeSpace(rec)
	local lifeSupport = Hangar.RecordLifeSupport(rec)
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
	for _, rec in pairs(ships) do
		for name, tags in pairs(rec.cargoTags or {}) do
			tags[tag] = nil
			if not next(tags) then rec.cargoTags[name] = nil end
		end
	end
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
		tonnes = tonnes + hullMass(rec.shipId)
	end
	return (Hangar.BAY_RENT * h.bays + Hangar.TONNE_RENT * tonnes) * h.factor
end

-- Rent per month for one bay: the bay fee, and the parking fee for the ship
-- in it (0 when the bay is empty)
---@param h Hangar.Lease
---@param rec Hangar.Record?
function Hangar.BayRent(h, rec)
	local bay = Hangar.BAY_RENT * h.factor
	local parking = rec and Hangar.TONNE_RENT * hullMass(rec.shipId) * h.factor or 0
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
	return #Hangar.StoredShips(h) == 0 and #h.store == 0 and h.debt <= 0
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
	local key = Hangar.StationKey(station)
	local lounge = lounges[key]
	if not lounge then
		lounge = { key = key, path = station.path, station = station.label,
			factor = Hangar.StationFactor(station), crew = {} }
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

-- Let go of someone waiting in the lounge where the player is docked. Only
-- takes them out of the lounge: the caller ends their contract as for any
-- dismissal.
---@param character Character
function Hangar.DismissFromLounge(character)
	local station = Game.player:GetDockedWith()
	local lounge, entry, index = Hangar.FindInLounge(character)
	if not station or not lounge or lounge.key ~= Hangar.StationKey(station) then return false end
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

	-- Leaving anyone in the lounge needs the lounge fees paid
	if aboard and Hangar.LoungeBlocked() then
		for member in player:EachCrewMember() do
			if not member.player and not aboard[member] then
				table.insert(blockers, Hangar.LoungeBlocked())
				break
			end
		end
	end

	local crew = crewAfter(aboard)
	if crew > def.maxCrew then
		table.insert(blockers, string.interp(l.TOO_MANY_CREW, { ship = rec.name, max = def.maxCrew, crew = crew, over = crew - def.maxCrew }))
	elseif crew < def.minCrew then
		table.insert(warnings, string.interp(l.TOO_FEW_CREW, { ship = rec.name, min = def.minCrew, crew = crew }))
	end

	local room = Hangar.RecordFreeSpace(rec)
	local taken = Hangar.TakeTotal(take)
	if taken > room then
		table.insert(blockers, string.interp(l.TOO_MUCH_CARGO, { ship = rec.name, room = room, over = taken - room }))
	end

	local lifeSupport = Hangar.RecordLifeSupport(rec)
	for name, parts in pairs(take) do
		local commodity = Commodities[name]
		local n = 0
		for _, c in pairs(parts) do n = n + c end
		if n > 0 and commodity and (commodity.life_support or 0) > lifeSupport then
			table.insert(blockers, string.interp(l.NEEDS_LIFE_SUPPORT, { commodity = commodity:GetName() }))
		end
	end

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

	-- Cargo that stays with the ship being stored, lot by lot
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

	accrue(h, Game.time)

	for name, n in pairs(leaveOrdinary) do
		cargoMgr:RemoveCommodity(Commodities[name], n, ORDINARY)
	end
	for name, tags in pairs(leaveTags) do
		for tag, n in pairs(tags) do
			cargoMgr:RemoveCommodity(Commodities[name], n, tag)
		end
	end

	-- Rebuild the player's ship from the record. SetShipType removes all
	-- equipment (the stored ship keeps the item instances) and keeps cargo.
	player:SetShipType(rec.shipId)
	player:SetSkin(rec.skin)
	if rec.pattern then player.model:SetPattern(rec.pattern) end
	player:SetLabel(rec.label)
	player:SetShipName(rec.name)

	-- Crew never stays on a stored ship: who doesn't fly on waits here
	if aboard then
		local leaving = {}
		for member in player:EachCrewMember() do
			if not aboard[member] and not member.player then table.insert(leaving, member) end
		end
		for _, member in ipairs(leaving) do Hangar.SendToLounge(member) end
		for member in pairs(aboard) do
			if member.waitingAt then Hangar.TakeFromLounge(member) end
		end
	end

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
		for _, h in pairs(hangars) do h.store = h.store or {} end
		loaded_data = nil
	end

	-- Missions that ended while the save held their cargo on a stored ship
	for _, rec in pairs(ships) do
		for _, tags in pairs(rec.cargoTags or {}) do
			for tag in pairs(tags) do
				local meta = getmetatable(tag)
				if meta and meta.class == "Mission" and not tag:IsActive() then releaseTag(tag) end
			end
		end
	end

	runBilling()
end)

Event.Register("onMissionRemoved", releaseTag)

Event.Register("onGameEnd", function()
	hangars = {}
	ships = {}
	lounges = {}
	loungeDebt = 0
	nextId = 1
	nextBill = nil
	billingToken = billingToken + 1
end)

local serialize = function()
	return { hangars = hangars, ships = ships, nextId = nextId, nextBill = nextBill, lounges = lounges, loungeDebt = loungeDebt }
end

local unserialize = function(data)
	loaded_data = data
end

Serializer:Register("Hangar", serialize, unserialize)

return Hangar
