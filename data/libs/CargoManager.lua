-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

local ShipDef = require 'ShipDef'
local Serializer = require 'Serializer'

local utils = require 'utils'

--
-- Class: CargoManager
--
-- CargoManager represents and manages all ship-based cargo data storage, in
-- contrast to EquipSet which manages active ship equipment.
--

---@class CargoManager
local CargoManager = utils.class('CargoManager')

--
-- Tagged cargo
--
-- Tons of a commodity can be tagged as held for an owner, usually a Mission
-- object. Totals don't change: `commodities[name].count` is still everything
-- aboard, and `tagged = { [tag] = n }` says how much of it each owner holds.
-- The rest are ordinary tons.
--
-- A tag may implement GetCargoLabel() to describe itself in the UI.
--
-- Constant: ORDINARY
--
-- Pass instead of a tag to count or remove only the tons no one holds.
CargoManager.ORDINARY = setmetatable({}, { __tostring = function() return "ORDINARY" end })

local ORDINARY = CargoManager.ORDINARY

local function taggedTotal(storage)
	local n = 0
	for _, c in pairs(storage.tagged or {}) do n = n + c end
	return n
end

local function changeTag(storage, tag, delta)
	storage.tagged = storage.tagged or {}
	local n = (storage.tagged[tag] or 0) + delta
	storage.tagged[tag] = n > 0 and n or nil
	if not next(storage.tagged) then storage.tagged = nil end
end

-- Constructor
--
-- Creates and initializes a CargoManager object for the given ship
function CargoManager:Constructor(ship)
	self.ship = ship

	-- Cargo space and cargo mass are the same thing right now
	self.usedCargoSpace = 0
	self.usedCargoMass = 0

	-- Initialize property variables on owning ship for backwards compatibility
	-- don't initialize them if they're already created after first save
	if not ship:hasprop("cargo_cap") then
		ship:setprop("cargo_cap", ShipDef[ship.shipId].cargo)
	end

	if not ship:hasprop("usedCargo") then
		ship:setprop("usedCargo", 0)
	end

	-- TODO: stored commodities should be represented as array of { name, count, meta } entries
	-- to allow for e.g. tracking stolen/scooped cargo, or special mission-related cargoes

	-- Commodity storage is implemented as simple hashtable of name -> { count=n } values
	-- to ease initial implementation
	---@type table<string, { count: number, life_support: number?, tagged: table<any, integer>? }>
	self.commodities = {}

	-- Event listeners for changes to commodities stored in this manager
	self.listeners = {}
end

-- Method: OnShipTypeChanged
--
-- Reinitialize ship properties after changing the type of the ship
-- Note: cargo mass is not removed from ship.mass_cap when changing ship types
function CargoManager:OnShipTypeChanged()
	self.ship:setprop("cargo_cap", ShipDef[self.ship.shipId].cargo)
	self.ship:setprop("usedCargo", self.usedCargoSpace)

	self.ship:UpdateEquipStats()
end

-- Method: GetFreeSpace
--
-- Returns the available amount of cargo space currently present on the vessel.
function CargoManager:GetFreeSpace()
	return self:GetTotalSpace() - self.usedCargoSpace
end

-- Method: GetUsedSpace
--
-- Returns the amount of cargo space currently occupied on the vessel.
function CargoManager:GetUsedSpace()
	return self.usedCargoSpace
end

-- Method: GetTotalSpace
--
-- Returns the maximum amount of cargo that could be stored on the vessel.
function CargoManager:GetTotalSpace()
	return self.ship["cargo_cap"]
end

-- Method: AddCommodity
--
-- Add a specific number of the given commodity to this cargo manager.
-- Will return false if the total number specified cannot be removed from the
-- cargo manager.
--
-- Parameters:
--   type - CommodityType object of the commodity to add
--   count - number of commodities
--   tag - optional owner the added tons are held for (see Tagged cargo)
--
-- Returns:
--   success - boolean indicating whether there was enough space on the vessel
--             to store the commodity
---@param type CommodityType
---@param count integer
---@param tag any?
function CargoManager:AddCommodity(type, count, tag)
	count = count or 1

	-- Adding nothing always fits; don't leave an empty 0t entry behind
	if count <= 0 then
		return true
	end

	-- TODO: use a cargo volume metric with variable mass instead of fixed 1t == 1m^3
	local required_space = (type.mass or 1) * count

	if self:GetFreeSpace() < required_space then
		return false
	end

	self.usedCargoSpace = self.usedCargoSpace + required_space
	self.ship:setprop("usedCargo", self.usedCargoSpace)

	self.usedCargoMass = self.usedCargoMass + required_space
	self.ship:setprop("mass_cap", self.ship.mass_cap + required_space)

	local storage = self.commodities[type.name]

	if not storage then
		storage = { count = 0, life_support = type.life_support }
		self.commodities[type.name] = storage
	end

	storage.count = storage.count + count
	if tag == ORDINARY then tag = nil end
	if tag then
		changeTag(storage, tag, count)
	end

	self.ship:UpdateEquipStats()

	-- Notify listeners that the cargo contents have changed
	for _, fn in pairs(self.listeners) do
		fn(type, count, tag)
	end

	return true
end

-- Method: RemoveCommodity
--
-- Remove a specific number of the specified commodity from this cargo manager.
-- Will return the number of commodities removed, even if that number is less
-- than initially desired.
--
-- Parameters:
--   type - CommodityType object of the commodity to remove
--   count - maximum number of commodity items to remove
--   tag - optional: remove only the tons held for this tag, or only ordinary
--         tons with CargoManager.ORDINARY. Without it, ordinary tons go
--         first, then tagged ones.
--
-- Returns:
--   numRemoved - total number of commodity items removed, or 0 if no items
--                were removed from the cargo
---@param type CommodityType
---@param count integer
---@param tag any?
function CargoManager:RemoveCommodity(type, count, tag)
	local storage = self.commodities[type.name]

	if not storage then
		return 0
	end

	if storage.count == 0 then
		self.commodities[type.name] = nil
		return 0
	end

	count = count or 1

	-- How much comes out of the ordinary tons and out of each tag
	local ordinary = storage.count - taggedTotal(storage)
	local parts = {} ---@type { tag: any?, n: integer }[]
	if tag == ORDINARY then
		table.insert(parts, { n = math.min(count, ordinary) })
	elseif tag then
		table.insert(parts, { tag = tag, n = math.min(count, storage.tagged and storage.tagged[tag] or 0) })
	else
		local left = count
		local n = math.min(left, ordinary)
		table.insert(parts, { n = n })
		left = left - n
		for t, c in pairs(storage.tagged or {}) do
			if left <= 0 then break end
			n = math.min(left, c)
			table.insert(parts, { tag = t, n = n })
			left = left - n
		end
	end

	local removed = 0
	for _, part in ipairs(parts) do
		if part.tag then changeTag(storage, part.tag, -part.n) end
		removed = removed + part.n
	end

	if removed == 0 then
		return 0
	end

	storage.count = storage.count - removed

	-- Remove the storage entry from the ship if the commodity is no longer stored here
	if storage.count == 0 then
		self.commodities[type.name] = nil
	end

	-- TODO: use a cargo volume metric with variable mass instead of fixed 1m^3 == 1t
	local freed_space = (type.mass or 1) * removed
	self.usedCargoSpace = self.usedCargoSpace - freed_space
	self.ship:setprop("usedCargo", self.usedCargoSpace)

	self.usedCargoMass = self.usedCargoMass - freed_space
	self.ship:setprop("mass_cap", self.ship.mass_cap - freed_space)

	self.ship:UpdateEquipStats()

	-- Notify listeners that the cargo contents have changed, once per owner
	for _, part in ipairs(parts) do
		if part.n > 0 then
			for _, fn in pairs(self.listeners) do
				fn(type, -part.n, part.tag)
			end
		end
	end

	return removed
end

-- Method: CountCommodity
--
-- Returns total amount of a commodity available in this cargo manager.
--
-- Parameters:
--   type - CommodityType object of the commodity to query
--   tag - optional: count only the tons held for this tag, or only ordinary
--         tons with CargoManager.ORDINARY
---@param type CommodityType
---@param tag any?
function CargoManager:CountCommodity(type, tag)
	local storage = self.commodities[type.name]
	if not storage then
		return 0
	end

	if tag == ORDINARY then
		return storage.count - taggedTotal(storage)
	elseif tag then
		return storage.tagged and storage.tagged[tag] or 0
	end

	return storage.count
end

-- Method: Tag
--
-- Hold up to `count` ordinary tons already aboard for `tag`, e.g. goods a
-- mission needs that the player bought. Returns how many were tagged.
---@param type CommodityType
---@param count integer
---@param tag any
function CargoManager:Tag(type, count, tag)
	local storage = self.commodities[type.name]
	if not storage or not tag or tag == ORDINARY then return 0 end
	local n = math.min(count, storage.count - taggedTotal(storage))
	if n <= 0 then return 0 end
	changeTag(storage, tag, n)
	return n
end

-- Method: Untag
--
-- Make up to `count` tons held for `tag` ordinary again. Returns how many.
---@param type CommodityType
---@param count integer
---@param tag any
function CargoManager:Untag(type, count, tag)
	local storage = self.commodities[type.name]
	local n = math.min(count, storage and storage.tagged and storage.tagged[tag] or 0)
	if n <= 0 then return 0 end
	changeTag(storage, tag, -n)
	return n
end

-- Method: UntagAll
--
-- Make everything held for `tag` ordinary again (its mission ended).
function CargoManager:UntagAll(tag)
	for _, storage in pairs(self.commodities) do
		if storage.tagged and storage.tagged[tag] then
			changeTag(storage, tag, -storage.tagged[tag])
		end
	end
end

-- Method: GetTags
--
-- Return { [tag] = count } for a commodity (empty when nothing is held).
-- Don't change the table.
---@param type CommodityType
---@return table<any, integer>
function CargoManager:GetTags(type)
	local storage = self.commodities[type.name]
	return storage and storage.tagged or {}
end

-- Method: TaggedCommodities
--
-- Return an iterator over { name, count } of the cargo held for `tag`.
function CargoManager:TaggedCommodities(tag)
	local list = {}
	for name, storage in pairs(self.commodities) do
		local n = storage.tagged and storage.tagged[tag]
		if n then table.insert(list, { name, n }) end
	end
	local i = 0
	return function()
		i = i + 1
		if list[i] then return list[i][1], list[i][2] end
	end
end

-- Method: AllTags
--
-- Return a set of every tag holding cargo here.
---@return table<any, boolean>
function CargoManager:AllTags()
	local tags = {}
	for _, storage in pairs(self.commodities) do
		for tag in pairs(storage.tagged or {}) do tags[tag] = true end
	end
	return tags
end

-- Method: Commodities
--
-- Return an iterator over { name, count } pairs of commodity cargo in this ship.
function CargoManager:Commodities()
	---@type fun(s: table, k: string): string, integer
	return function(s, k)
		k = next(s, k)
		if not k then return end
		return k, s[k].count
	end, self.commodities, nil
end

-- Method: DoLifeSupportChecks
--
-- Check for commodities on this vessel which would perish under the given
-- cargo life-support level.
-- Returns the name of the first commodity found that would perish.
---@param supportLevel integer
---@return string?
function CargoManager:DoLifeSupportChecks(supportLevel)
	for name, info in pairs(self.commodities) do
		if (info.life_support or 0) > supportLevel then
			return name
		end
	end

	return nil
end

-- Method: AddListener
--
-- Register a callback function to be notified when the cargo stored in this manager is changed.
-- The provided key will be used to uniquely identify the callback function and can be used to
-- later remove the event listener.
--
-- The callback function receives three arguments:
--
--   cargoType - an object describing the type of cargo that was added or removed. Usually a CommodityType.
--   count     - a number specifying how many items were added (positive) or removed (negative).
--   tag       - the owner of the tons added or removed, or nil for ordinary tons.
--
-- Parameters:
--   key - a unique value identifying the listener function being added.
--   fn - a callback function following the above format to be notified of any changes in cargo manifest.
function CargoManager:AddListener(key, fn)
	self.listeners[key] = fn
end

-- Method: RemoveListener
--
-- Remove a previously-added listener by providing the same key as was used to register it.
--
-- Parameters:
--   key - a unique value identifiying a listener function previously added.
function CargoManager:RemoveListener(key)
	self.listeners[key] = nil
end

function CargoManager:Serialize()
	return utils.filter_table(self, function(k, v) return k ~= "listeners" end)
end

function CargoManager:Unserialize()
	setmetatable(self, CargoManager.meta)
	self.listeners = {}

	-- Older saves can hold empty 0t entries
	for name, storage in pairs(self.commodities) do
		if storage.count <= 0 then self.commodities[name] = nil end
	end

	return self
end

Serializer:RegisterClass('CargoManager', CargoManager)

return CargoManager
