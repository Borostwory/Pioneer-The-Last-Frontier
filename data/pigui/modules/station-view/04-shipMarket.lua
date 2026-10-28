-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

local Lang = require 'Lang'
local Game = require 'Game'
local Format = require 'Format'
local ShipDef = require 'ShipDef'
local Equipment = require 'Equipment'
local StationView = require 'pigui.views.station-view'
local Table = require 'pigui.libs.table'
local PiImage = require 'pigui.libs.image'
local ModelSpinner = require 'PiGui.Modules.ModelSpinner'
local EquipSet = require 'EquipSet'
local HullConfig = require 'HullConfig'
local PlayerState= require 'PlayerState'
local Passengers = require 'Passengers'
local ModalWindow = require 'pigui.libs.modal-win'
local Hangar = require 'modules.Hangar.Hangar'
local Pickers = require 'pigui.libs.hangar-pickers'

local ui = require 'pigui'

local utils = require 'utils'

local pionillium = ui.fonts.pionillium
local orbiteer = ui.fonts.orbiteer
local l = Lang.GetResource("ui-core")
local ls = Lang.GetResource("ships")
local lh = Lang.GetResource("module-hangar")
local Vector2 = _G.Vector2

local vZero = Vector2(0,0)
local widgetSizes = ui.rescaleUI({
	iconSize = Vector2(64, 64),
	buyButton = Vector2(96, 36),
	confirmButtonSize = Vector2(384, 48),
	itemSpacing = Vector2(18, 4),
	cardPadding = Vector2(10, 6),
	modalItemSpacing = Vector2(8, 6),
	purchaseButtons = Vector2(220, 0),
}, Vector2(1600, 900))
widgetSizes.iconSpacer = (widgetSizes.buyButton - widgetSizes.iconSize)/2

local shipMarket
---@type table<table, { price: integer, equip: { [1]: string, [2]: EquipType }[] }
local advertDataCache = {}
local icons = {}
local manufacturerIcons = {}
local selectedItem

local shipSellPriceReduction = 0.65
local equipSellPriceReduction = 0.8

local modelSpinner = ModelSpinner()
local cachedShip = nil
local cachedPattern = nil

-- The Last Frontier: store the current ship in the hangar here instead of
-- trading it in, and pick the cargo and crew for the new ship
local buyMode = "tradein" -- see Hangar.BUY_MODES
local purchase = nil ---@type { sos: table, mode: string, aboard: table }?
local purchaseModal -- created below

local shipClassString = {
	light_cargo_shuttle        = l.LIGHT_CARGO_SHUTTLE,
	light_courier              = l.LIGHT_COURIER,
	light_fighter              = l.LIGHT_FIGHTER,
	light_freighter            = l.LIGHT_FREIGHTER,
	light_passenger_shuttle    = l.LIGHT_PASSENGER_SHUTTLE,
	light_passenger_transport  = l.LIGHT_PASSENGER_TRANSPORT,
	medium_cargo_shuttle       = l.MEDIUM_CARGO_SHUTTLE,
	medium_courier             = l.MEDIUM_COURIER,
	medium_fighter             = l.MEDIUM_FIGHTER,
	medium_freighter           = l.MEDIUM_FREIGHTER,
	medium_passenger_shuttle   = l.MEDIUM_PASSENGER_SHUTTLE,
	medium_passenger_transport = l.MEDIUM_PASSENGER_TRANSPORT,
	heavy_cargo_shuttle        = l.HEAVY_CARGO_SHUTTLE,
	heavy_courier              = l.HEAVY_COURIER,
	heavy_fighter              = l.HEAVY_FIGHTER,
	heavy_freighter            = l.HEAVY_FREIGHTER,
	heavy_passenger_shuttle    = l.HEAVY_PASSENGER_SHUTTLE,
	heavy_passenger_transport  = l.HEAVY_PASSENGER_TRANSPORT,

	unknown                    = "",
}

local function makeAdvertDataCacheEntry(shipOnSale)
	local def = shipOnSale.def ---@type ShipDef
	local config = assert(HullConfig.GetHullConfig(def.id))

	-- TODO: some sort of condition-based discount or an alteration to the
	-- price for purchasing directly from a manufacturer's station?
	local shipPrice = def.basePrice

	local equip = {}

	for _, slot in pairs(config.slots) do

		if slot.default then
			local defaultEquip = Equipment.Get(slot.default)

			-- Have to go through all of this to get an accurate cost for the ship...
			-- TODO: consider adding a "cost function" to EquipType which moves
			-- responsibility for computing accurate cost into the domain of the code
			-- that knows how the equipment will be specialized for the ship?
			if defaultEquip then
				local inst = defaultEquip:Instance()

				if inst.SpecializeForShip then
					inst:SpecializeForShip(config)
				end

				if slot.count then
					inst:SetCount(slot.count)
				end

				table.insert(equip, { slot.id, inst })
				shipPrice = shipPrice + inst.price
			end
		end

	end

	return { price = shipPrice, equip = equip }
end

local function refreshModelSpinner()
	if not selectedItem then return end
	cachedShip = selectedItem.def.modelName
	cachedPattern = selectedItem.pattern
	modelSpinner:setModel(cachedShip, selectedItem.skin, cachedPattern)
end

local function refreshShipMarket()
	widgetSizes.rowVerticalSpacing = Vector2(0, (widgetSizes.iconSize.y + widgetSizes.itemSpacing.y - pionillium.large.size)/2)

	local station = Game.player:GetDockedWith()
	if station then
		shipMarket.items = station:GetShipsOnSale()

		advertDataCache = utils.map_table(shipMarket.items, function(_, sos)
			return sos, makeAdvertDataCacheEntry(sos)
		end)
	else
		shipMarket.items = {}
		advertDataCache = {}
	end

	selectedItem = nil
	shipMarket.selectedItem = nil
end

local function manufacturerIcon (manufacturer)
	if(manufacturer ~= "unknown" and manufacturer ~= nil) then
		if manufacturerIcons[manufacturer] == nil then
			manufacturerIcons[manufacturer] = PiImage.New("icons/manufacturer/".. manufacturer ..".png")
		end
		manufacturerIcons[manufacturer]:Draw(widgetSizes.iconSize)
	end
end

---@param ship Ship
local tradeInValue = function(ship)
	local shipDef = ShipDef[ship.shipId]
	local value = shipDef.basePrice * shipSellPriceReduction * ship.hullPercent/100

	-- We don't need to remove the hyperdrive from the value of the ship since the player is charged for it when buying the ship
	local equipment = ship:GetComponent("EquipSet"):GetInstalledEquipment()
	for _, e in pairs(equipment) do
		value = value + e.price * equipSellPriceReduction
	end

	return math.ceil(value)
end

-- The bare hull's trade-in value ("hull" mode keeps the equipment)
local hullTradeInValue = function(ship)
	return math.ceil(ShipDef[ship.shipId].basePrice * shipSellPriceReduction * ship.hullPercent/100)
end

-- What the player pays with each buy mode (see Hangar.BUY_MODES)
local function purchaseCost(sos, mode)
	local price = advertDataCache[sos].price
	if mode == "store" then return price end
	if mode == "hull" then return price - hullTradeInValue(Game.player) end
	return price - tradeInValue(Game.player)
end

-- Buy ship `sos`, dealing with the current ship as `mode` says. `aboard` is
-- who flies the new ship (nil: everyone aboard now).
local function completePurchase (mkt, sos, mode, aboard)
	local player = Game.player
	local station = assert(player:GetDockedWith())
	local def = sos.def

	local shipData = advertDataCache[sos]

	local cost = purchaseCost(sos, mode)
	if math.floor(cost) ~= cost then
		error("Ship price non-integer value.")
	end
	if PlayerState.GetMoney() < cost then
		mkt.popup.msg = l.YOU_NOT_ENOUGH_MONEY
		mkt.popup:open()
		return
	end

	local blockers = Hangar.CheckBuy(def, shipData.equip, mode, aboard)
	if #blockers > 0 then
		mkt.popup.msg = blockers[1]
		mkt.popup:open()
		return
	end

	PlayerState.AddMoney(-cost)

	local oldName = player.shipName
	local message = l.THANKS_AND_REMEMBER_TO_BUY_FUEL
	if mode == "store" then
		-- as it is: equipment and cargo stay with it
		assert(Hangar.StoreCurrentShip(nil))
		station:RemoveShipOnSale(sos)
		message = string.interp(lh.BOUGHT_AND_STORED, { ship = oldName }) .. "\n" .. message
	else
		if mode == "hull" then
			local _, tons, crew, items = Hangar.StripCurrentShip()
			aboard = Hangar.OnlyCommander()
			message = string.interp(lh.BOUGHT_HULL_SOLD, { ship = oldName, items = items, tons = tons, crew = crew }) .. "\n" .. message
		end
		station:ReplaceShipOnSale(sos, {
			def     = ShipDef[player.shipId],
			skin    = player:GetSkin(),
			pattern = player.model.pattern,
			label   = player.label,
		})
	end

	player:SetShipType(def.id)
	player:SetSkin(sos.skin)
	if sos.pattern then player.model:SetPattern(sos.pattern) end
	player:SetLabel(sos.label)
	-- the stored ship keeps its name; the new one is named after its type
	-- until the player renames it
	if mode == "store" then player:SetShipName(ls[def.i18n_key]) end

	-- TODO: ship ads should support an explicit list of (pre-owned) equipment as well as / instead of factory-default items
	-- At current we just build a list from the HullConfig's default items

	local equipSet = player:GetComponent('EquipSet')

	-- Install pre-built list of default equipment into ship
	for _, pair in ipairs(shipData.equip) do
		local handle = assert(equipSet:GetSlotHandle(pair[1]))

		if equipSet:CanInstallInSlot(handle, pair[2]) then
			equipSet:Install(pair[2], handle)
		else
			logWarning("Default equipment item {} for ship slot {}.{} is not compatible with slot." % { handle.default, player.shipId, handle.id })
		end
	end

	-- FIXME: fallback pass. Hyperdrives should be specified as a default item on the hyperdrive slot
	-- Once all hyperdrives are specified as default, this pass should be removed
	if def.hyperdriveClass > 0 and not player:GetInstalledHyperdrive() then
		local slot = player:GetComponent('EquipSet'):GetAllSlotsOfType('hyperdrive')[1]

		-- Install the best-fitting non-military hyperdrive we can
		local hyperdrive = utils.best_score(Equipment.new, function(_, equip)
			return EquipSet.CompatibleWithSlot(equip, slot) and equip.slot.type:match("%.civilian")
				and equip.capabilities.hyperclass or nil
		end)

		player:GetComponent('EquipSet'):Install(hyperdrive:Instance(), slot)
	end

	player:SetFuelPercent(100)

	-- crew that doesn't fly the new ship waits in the lounge here
	Hangar.MoveCrew(aboard)

	refreshShipMarket();

	mkt.popup.msg = message
	mkt.popup:open()
end

-- The Buy ship button opens the purchase window: what happens to the current
-- ship, who flies the new one, and what's in the way
local function buyShip (mkt, sos)
	if Hangar.BuyModeBlocked(buyMode) then buyMode = "tradein" end
	purchase = { sos = sos, mode = buyMode, aboard = Pickers.everyoneAboard() }
	purchaseModal:open()
end

-- "You pay $X", or "You receive $X" when the trade-in is worth more
local function payText(cost)
	if cost < 0 then
		return string.interp(lh.YOU_RECEIVE, { amount = Format.Money(-cost, false) }), ui.theme.colors.econProfit
	end
	return string.interp(lh.YOU_PAY, { cost = Format.Money(cost, false) }), ui.theme.colors.font
end

-- The problem particular to a mode, shown on its card (nil when none)
local function modeProblem(sos, mode)
	local blocked = Hangar.BuyModeBlocked(mode)
	if blocked then return blocked end
	if mode == "tradein" then
		local room = Hangar.NewShipCargo(sos.def, advertDataCache[sos].equip)
		local used = Game.player:GetComponent('CargoManager'):GetUsedSpace()
		if used > room then
			return string.interp(lh.TRADE_IN_CARGO_SHORT, { used = used, room = room, ship = ls[sos.def.i18n_key] })
		end
	end
	return nil
end

-- One choice for the current ship, drawn as a card with a radio mark: the
-- title and price on the first line, what it does below, and what stops it.
-- Returns true when clicked.
local function modeCard(sos, mode, selected)
	local colors = ui.theme.colors
	local lineHeight = ui.getTextLineHeight()
	local pad = widgetSizes.cardPadding
	local width = ui.getContentRegion().x
	local problem = modeProblem(sos, mode)
	local usable = Hangar.BuyModeBlocked(mode) == nil
	local ship = Game.player.shipName
	local textColor = usable and colors.font or colors.fontDim

	local top = ui.getCursorScreenPos()
	local indent = pad.x + lineHeight * 1.2 -- room for the radio mark
	ui.dummy(Vector2(0, pad.y))
	ui.dummy(Vector2(indent, 0))
	ui.sameLine(0, 0)
	ui.beginGroup()
		ui.textColored(textColor, string.interp(lh["BUY_MODE_" .. mode:upper()], { ship = ship }))
		if usable then
			local price, priceColor = payText(purchaseCost(sos, mode))
			-- inside a group, the offset counts from the group's left edge
			ui.sameLine(width - indent - ui.calcTextSize(price).x - pad.x)
			ui.textColored(priceColor, price)
		end
		ui.pushTextWrapPos(width - pad.x)
		ui.textColored(colors.fontDim, string.interp(lh["BUY_DESC_" .. mode:upper()], { ship = ship }))
		if problem then
			ui.textColored(usable and colors.alertRed or colors.fontDim, problem)
		end
		ui.popTextWrapPos()
	ui.endGroup()
	ui.dummy(Vector2(0, pad.y))
	local bottom = ui.getCursorScreenPos()
	local br = Vector2(top.x + width, bottom.y - ui.getItemSpacing().y)

	-- frame, tint and radio mark over the card
	local frame = selected and colors.uiPrimaryLight or colors.uiPrimaryDark
	if selected then ui.addRectFilled(top, br, colors.lightBlueBackground, 4, ui.RoundCornersAll) end
	ui.addRect(top, br, usable and frame or colors.tableHighlightDisabled, 4, ui.RoundCornersAll, selected and 2 or 1)
	local center = Vector2(top.x + pad.x + lineHeight * 0.5, top.y + pad.y + lineHeight * 0.5)
	ui.addCircle(center, lineHeight * 0.4, usable and colors.font or colors.fontDim, 20, 2)
	if selected then ui.addCircleFilled(center, lineHeight * 0.22, colors.font, 20) end

	-- the whole card is the button
	ui.setCursorScreenPos(top)
	local clicked = ui.invisibleButton("##buymode" .. mode, br - top) and usable
	ui.setCursorScreenPos(Vector2(top.x, br.y + ui.getItemSpacing().y))
	return clicked
end

local function section(title)
	ui.spacing()
	ui.withFont(orbiteer.body, function() ui.text(title) end)
	ui.separator()
end

local drawPurchase

purchaseModal = ModalWindow.New("ShipMarketPurchase", function(self)
	ui.withFont(pionillium.body, function()
		ui.withStyleVars({ ItemSpacing = widgetSizes.modalItemSpacing }, function()
			drawPurchase(self)
		end)
	end)
end, function(_, innerFn)
	-- a fixed width so long lines wrap; the height follows the contents
	local width = math.max(ui.screenWidth * 0.42, 560)
	ui.setNextWindowSizeConstraints(Vector2(width, 0), Vector2(width, ui.screenHeight * 0.9))
	innerFn()
end)
purchaseModal.centered = true

drawPurchase = function(self)
	if not purchase then self:close() return end
	local player = Game.player
	local sos = purchase.sos
	local def = sos.def
	local shipName = ls[def.i18n_key]
	local equip = advertDataCache[sos] and advertDataCache[sos].equip or {}

	ui.withFont(orbiteer.heading, function()
		ui.text(string.interp(lh.BUY_TITLE, { ship = shipName }))
	end)
	ui.text(string.interp(lh.SHIP_PRICE, { price = Format.Money(advertDataCache[sos].price, false) }))

	-- what happens to the current ship: pick one
	section(string.interp(lh.CURRENT_SHIP_CHOICE, { ship = player.shipName }))
	local anyBlocked = false
	for _, m in ipairs(Hangar.BUY_MODES) do
		if Hangar.BuyModeBlocked(m) then anyBlocked = true end
		if modeCard(sos, m, purchase.mode == m) then
			purchase.mode = m
			buyMode = m
		end
	end
	if anyBlocked and ui.button(lh.GO_TO_HANGAR) then
		purchase = nil
		self:close()
		StationView:SwitchTo("hangarView")
		return
	end
	local mode = purchase.mode

	-- who flies the new ship
	section(lh.CREW_SECTION)
	local aboard = purchase.aboard
	if mode == "hull" then
		aboard = Hangar.OnlyCommander()
		ui.textWrapped(lh.HULL_ALL_CREW_LOUNGE)
	else
		Pickers.drawCrew(purchase, def, shipName)
		ui.textColored(ui.theme.colors.fontDim, lh.CREW_STAY_HINT)
	end

	local cost = purchaseCost(sos, mode)
	local blockers, warnings = Hangar.CheckBuy(def, equip, mode, aboard)
	if PlayerState.GetMoney() < cost then table.insert(blockers, 1, l.YOU_NOT_ENOUGH_MONEY) end
	ui.pushTextWrapPos(ui.getContentRegion().x)
	Pickers.drawProblems(blockers, warnings)
	ui.popTextWrapPos()

	ui.spacing()
	ui.separator()
	ui.spacing()
	local price, priceColor = payText(cost)
	ui.textColored(priceColor, price)
	ui.sameLine()
	ui.dummy(Vector2(ui.getContentRegion().x - widgetSizes.purchaseButtons.x, 0))
	ui.sameLine()
	if ui.button(l.CANCEL) then
		purchase = nil
		self:close()
		return
	end
	ui.sameLine()
	local enabled = #blockers == 0
	if ui.button(l.BUY_SHIP .. "##confirmPurchase", Vector2(0, 0), not enabled and ui.theme.buttonColors.disabled or nil, blockers[1]) and enabled then
		local p = purchase
		purchase = nil
		self:close()
		completePurchase(shipMarket, p.sos, p.mode, p.mode ~= "hull" and p.aboard or nil)
	end
end

local FormatAndCompareShips = utils.class("ui.ShipMarket.FormatAndCompareShips")

function FormatAndCompareShips:beginTable()
	if not ui.beginTable("specs", 7 ) then
		return false
	end

	ui.tableSetupColumn("name1")
	ui.tableSetupColumn("body1")
	ui.tableSetupColumn("indicator1", {"WidthFixed"})
	ui.tableSetupColumn("gap", {"WidthFixed"})
	ui.tableSetupColumn("name2")
	ui.tableSetupColumn("body2")
	ui.tableSetupColumn("indicator2", {"WidthFixed"})

	return true
end

---@param desc       string        Description of the item being drawn
---@param a	         number        The value for the new ship
---@param b          number        The value for the old shio
---@param fmt_a      function|nil  A function that takes a and formats it to a string, if required
---@param fmt_b      function|nil  A function that takes b and formats it to a string, if nil then fmt_a is used
function FormatAndCompareShips:compare_and_draw_column(desc, a, b, fmt_a, fmt_b)
	local compare = a-b

	if self.column == 0 then
		ui.tableNextRow()
	end

	fmt_b = fmt_b and fmt_b or fmt_a

	ui.tableSetColumnIndex(0 + self.column)
	ui.textColored(ui.theme.colors.fontDim, desc)

	ui.tableSetColumnIndex(1 + self.column)

	local new_str = fmt_a and fmt_a( a ) or a

	if compare < 0 then
		local old_str = fmt_b and fmt_b( b ) or b
		ui.withTooltip( l.CURRENT_SHIP .. ": " .. old_str, function ()
			ui.textAlignedColored(new_str, 1.0, ui.theme.colors.compareWorse)
			ui.tableSetColumnIndex(2 + self.column)
			ui.icon( ui.theme.icons.shipmarket_compare_worse, Vector2(ui.getTextLineHeight()), ui.theme.colors.compareWorse)
		end )
	elseif compare > 0 then
		local old_str = fmt_b and fmt_b( b ) or b
		ui.withTooltip( l.CURRENT_SHIP .. ": " .. old_str, function ()
			ui.textAlignedColored(new_str, 1.0,  ui.theme.colors.compareBetter)
			ui.tableSetColumnIndex(2 + self.column)
			ui.icon( ui.theme.icons.shipmarket_compare_better, Vector2(ui.getTextLineHeight()), ui.theme.colors.compareBetter)
		end )
	else
		ui.textAligned(new_str, 1.0)
		ui.tableSetColumnIndex(2 + self.column)
		ui.dummy( Vector2(ui.getTextLineHeight()) )
	end

	if self.column == 0 then
		self.column = 4
	else
		self.column = 0
	end
end

function FormatAndCompareShips:draw_hyperdrive_cell(desc)

	local function fmt( v )
		return v > 0 and v < 8 and
			Equipment.new["hyperspace.hyperdrive_" .. v]:GetName() or l.NONE
	end

	self:compare_and_draw_column( desc, self.def.hyperdriveClass, self.b.def.hyperdriveClass, fmt )
end

function FormatAndCompareShips:get_value(key)
	local v = self[key]
	if nil == v then
		v = self.def[key]
	end
	return v
end

function FormatAndCompareShips:draw_tonnage_cell(desc, key)
	self:compare_and_draw_column( desc, self:get_value(key), self.b:get_value(key), Format.MassTonnes )
end

function FormatAndCompareShips:draw_cargo_cell(desc, key)
	self:compare_and_draw_column( desc, self:get_value(key), self.b:get_value(key), ui.Format.Cargo )
end

function FormatAndCompareShips:draw_volume_cell(desc, key)
	self:compare_and_draw_column( desc, self:get_value(key), self.b:get_value(key), ui.Format.Volume )
end

function FormatAndCompareShips:draw_accel_cell(desc, thrustKey, massKey )
	local accelA = self.def.linearThrust[thrustKey] / (9.8106*1000*(self:get_value(massKey)))
	local accelB = self.b.def.linearThrust[thrustKey] / (9.8106*1000*(self.b:get_value(massKey)))
	self:compare_and_draw_column( desc, accelA, accelB, Format.AccelG )
end

function FormatAndCompareShips:draw_deltav_cell(desc, massNumeratorKey, massDenominatorKey)
	local deltavA = self.def.effectiveExhaustVelocity * math.log( self:get_value(massNumeratorKey) / self:get_value(massDenominatorKey) )
	local deltavB = self.b.def.effectiveExhaustVelocity * math.log( self.b:get_value(massNumeratorKey) / self.b:get_value(massDenominatorKey) )

	local function fmt( v )
		return string.format("%d km/s", v / 1000)
	end

	self:compare_and_draw_column( desc, deltavA, deltavB, fmt )
end

function FormatAndCompareShips:draw_unformated_cell(desc, key)
	self:compare_and_draw_column( desc, self:get_value(key),  self.b:get_value(key) )
end

local function getNumSlotsCompatibleWithType(def, type)
	local config = HullConfig.GetHullConfig(def.id)
	local count = 0

	for _, slot in pairs(config.slots) do
		if EquipSet.SlotTypeMatches(type, slot.type) then
			count = count + (slot.count or 1)
		end
	end

	return count
end

local function getBestSlotSizeOfType(def, type)
	local config = HullConfig.GetHullConfig(def.id)
	local slot, size = utils.best_score(config.slots, function(_, slot)
		return EquipSet.SlotTypeMatches(type, slot.type) and slot.size or nil
	end)

	return slot and size or 0
end

function FormatAndCompareShips:draw_equip_slot_cell(desc, key)
	self:compare_and_draw_column( desc, getNumSlotsCompatibleWithType(self.def, key), getNumSlotsCompatibleWithType(self.b.def, key) )
end

function FormatAndCompareShips:draw_berths_slot_cell(desc)
	local a_hull = HullConfig.GetHullConfig(self.def.id)
	local b_hull = (self.b and self.b.def) and HullConfig.GetHullConfig(self.b.def.id) or nil
	self:compare_and_draw_column( desc, Passengers.GetMaxPassengersForHull(a_hull), Passengers.GetMaxPassengersForHull(b_hull) )
end

function FormatAndCompareShips:draw_yes_no_equip_slot_cell(desc, key)

	local function fmt( v ) return v==1 and l.YES or l.NO end

	self:compare_and_draw_column( desc, getNumSlotsCompatibleWithType(self.def, key), getNumSlotsCompatibleWithType(self.b.def, key), fmt )
end

function FormatAndCompareShips:draw_atmos_pressure_limit_cell(desc)

	local a_shield = getBestSlotSizeOfType(self.def, "hull.atmo_shield")
	local b_shield = getBestSlotSizeOfType(self.b.def, "hull.atmo_shield")

	local function fmt( def, has_shield )
		local atmoSlot
		if has_shield > 1 then
			atmoSlot = string.format("%d(+%d/+%d) atm", def.atmosphericPressureLimit,
			def.atmosphericPressureLimit * (Equipment.new["hull.atmospheric_shielding_s2"].capabilities.atmo_shield - 1),
			def.atmosphericPressureLimit * (Equipment.new["hull.heavy_atmospheric_shielding_s2"].capabilities.atmo_shield - 1) )
		elseif has_shield > 0 then
			atmoSlot = string.format("%d(+%d) atm", def.atmosphericPressureLimit,
			def.atmosphericPressureLimit * (Equipment.new["hull.atmospheric_shielding_s2"].capabilities.atmo_shield - 1) )
		else
			atmoSlot = string.format("%d atm", def.atmosphericPressureLimit)
		end
		return atmoSlot
	end

	local function fmt_a( v )
		return fmt( self.def, a_shield )
	end

	local function fmt_b( v )
		return fmt( self.b.def, b_shield )
	end

	-- multiply the values by 1000 and then add on if there is capacity for atmo_shielding so that the compare takes that into account
	-- however, note the formatting ignores the passed in value and therefore displays correctly.
	self:compare_and_draw_column( desc, self.def.atmosphericPressureLimit*1000+a_shield, self.b.def.atmosphericPressureLimit*1000+b_shield, fmt_a, fmt_b )
end

function FormatAndCompareShips:Constructor(def, b)
	self.column = 0
	self.emptyMass = def.hullMass + def.fuelTankMass
	self.cargoCapacity = def.cargo
	self.def = def
	self.b = b
end


local tradeMenu = function()
	if(selectedItem) then
		ui.withStyleVars({ ItemSpacing = shipMarket.style.itemSpacing}, function()
			ui.child("TradeMenu", Vector2(0,0), function()
				local colHeadingWidth = ui.getContentRegion().x - widgetSizes.buyButton.x
				local def = selectedItem.def

				ui.columns(2, "shipMarketInfo")
				ui.setColumnWidth(0, colHeadingWidth)

				ui.withFont(orbiteer.title, function()
					ui.text(ls[selectedItem.def.i18n_key])
				end)
				ui.withFont(orbiteer.body, function()
					ui.text(shipClassString[selectedItem.def.shipClass])
				end)

				local cost = advertDataCache[selectedItem].price

				ui.withFont(pionillium.heading, function()
					ui.text(l.PRICE..": "..Format.Money(cost, false))
					ui.sameLine()
					ui.text(l.AFTER_TRADE_IN..": "..Format.Money(cost - tradeInValue(Game.player), false))
				end)

				ui.nextColumn()
				ui.dummy(widgetSizes.iconSpacer)
				ui.sameLine()
				manufacturerIcon(selectedItem.def.manufacturer)
				local shipBought = false
				ui.withFont(pionillium.body, function()
					shipBought = ui.button(l.BUY_SHIP, widgetSizes.buyButton)
				end)
				ui.columns(1, "")

				if shipBought then
					buyShip(shipMarket, selectedItem)
					return
				end

				local spinnerWidth = ui.getContentRegion().x
				modelSpinner:setSize(Vector2(spinnerWidth, spinnerWidth / 2.5))
				modelSpinner:draw()

				local currentShip = FormatAndCompareShips.New( ShipDef[Game.player.shipId], nil )
				local shipFormatAndCompare = FormatAndCompareShips.New( def, currentShip )

				ui.child("ShipSpecs", Vector2(0, 0), function()
					ui.withStyleVars({ CellPadding = Vector2(3,4) }, function()

						if not shipFormatAndCompare:beginTable() then return end

						shipFormatAndCompare:draw_hyperdrive_cell( l.HYPERDRIVE_FITTED )
						shipFormatAndCompare:draw_tonnage_cell( l.WEIGHT_EMPTY, "hullMass" )
						shipFormatAndCompare:draw_accel_cell( l.FORWARD_ACCEL_EMPTY, "FORWARD", "emptyMass" )
						shipFormatAndCompare:draw_tonnage_cell( l.FUEL_WEIGHT, "fuelTankMass" )
						shipFormatAndCompare:draw_accel_cell( l.REVERSE_ACCEL_EMPTY, "REVERSE", "emptyMass" )
						shipFormatAndCompare:draw_cargo_cell( l.CARGO_SPACE, "cargoCapacity" )
						shipFormatAndCompare:draw_accel_cell( l.UP_ACCEL_EMPTY, "UP", "emptyMass" )
						shipFormatAndCompare:draw_volume_cell( l.EQUIPMENT_CAPACITY, "equipCapacity" )
						shipFormatAndCompare:draw_deltav_cell( l.DELTA_V_EMPTY, "emptyMass", "hullMass")
						shipFormatAndCompare:draw_yes_no_equip_slot_cell( l.ATMOSPHERIC_SHIELDING, "hull.atmo_shield" )
						shipFormatAndCompare:draw_unformated_cell( l.MINIMUM_CREW, "minCrew" )
						shipFormatAndCompare:draw_atmos_pressure_limit_cell( l.ATMO_PRESS_LIMIT )
						shipFormatAndCompare:draw_unformated_cell( l.MAXIMUM_CREW, "maxCrew" )
						shipFormatAndCompare:draw_equip_slot_cell( l.MISSILE_MOUNTS, "missile" )
						shipFormatAndCompare:draw_berths_slot_cell( l.PASSENGER_CABIN_CAPACITY )
						shipFormatAndCompare:draw_equip_slot_cell( l.SCOOP_MOUNTS, "scoop" )
						shipFormatAndCompare:draw_yes_no_equip_slot_cell( l.ROVER_BAY, "vehicle_bay" )

						ui.endTable()

					end)
				end)

			end)
		end)
	end
end

shipMarket = Table.New("shipMarketWidget", false, {
	columnCount = 4,
	initTable = function(self)
		local iconColumnWidth = widgetSizes.iconSize.x + widgetSizes.itemSpacing.x
		local columnWidth = (self.style.size.x - iconColumnWidth) / (self.columnCount-1)
		ui.setColumnWidth(0, widgetSizes.iconSize.x + widgetSizes.itemSpacing.x)
		ui.setColumnWidth(1, columnWidth)
		ui.setColumnWidth(2, columnWidth)
		ui.setColumnWidth(3, columnWidth)
	end,
	renderHeaderRow = function(_)
		ui.withFont(orbiteer.heading, function()
			ui.text('')
			ui.nextColumn()
			ui.text(l.SHIP)
			ui.nextColumn()
			ui.text(l.PRICE)
			ui.nextColumn()
			ui.text(l.CAPACITY)
			ui.nextColumn()
		end)
	end,
	renderItem = function(_, item)
		if(icons[item.def.shipClass] == nil) then
			icons[item.def.shipClass] = PiImage.New("icons/shipclass/".. item.def.shipClass ..".png")
		end

		if not selectedItem then
			selectedItem = item
			shipMarket.selectedItem = item
			refreshModelSpinner()
		end

		if not advertDataCache[item] then
			advertDataCache[item] = makeAdvertDataCacheEntry(item)
		end

		icons[item.def.shipClass]:Draw(widgetSizes.iconSize)
		ui.nextColumn()
		ui.withStyleVars({ItemSpacing = vZero}, function()
			ui.dummy(widgetSizes.rowVerticalSpacing)
			ui.text(ls[item.def.i18n_key])
			ui.nextColumn()
			ui.dummy(widgetSizes.rowVerticalSpacing)
			ui.text(Format.Money(advertDataCache[item].price, false))
			ui.nextColumn()
			ui.dummy(widgetSizes.rowVerticalSpacing)
			ui.text(item.def.cargo.." cu")
			ui.nextColumn()
		end)
	end,
	onClickItem = function(_,e)
		selectedItem = e
		shipMarket.selectedItem = e
		refreshModelSpinner()
	end,
	sortingFunction = function(s1,s2)
		return ls[s1.def.i18n_key] < ls[s2.def.i18n_key]
	end
})

StationView:registerView({
	id = "shipMarketView",
	name = l.SHIP_MARKET,
	icon = ui.theme.icons.ship,
	showView = true,
	draw = function()
		ui.withFont(pionillium.heading, function()
			shipMarket:render()
		end)

		ui.sameLine(0, widgetSizes.itemSpacing.x)
		tradeMenu()
	end,
	refresh = function()
		refreshShipMarket()
		shipMarket.scrollReset = true
		buyMode = "tradein"
		purchase = nil
	end,
	debugReload = function()
		package.reimport()
	end
})
