-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

local ui = require 'pigui'
local InfoView = require 'pigui.views.info-view'
local Lang = require 'Lang'
local Game = require 'Game'
local ShipDef = require 'ShipDef'
local StationView = require 'pigui.views.station-view'
local Passengers = require 'Passengers'
local Commodities = require 'Commodities'
local CommodityType = require 'CommodityType'
local PlayerState   = require 'PlayerState'
local CargoManager  = require 'CargoManager'
local ModalWindow   = require 'pigui.libs.modal-win'

local l = Lang.GetResource("ui-core")
local lm = Lang.GetResource("mission-cargo")

local colors = ui.theme.colors
local icons = ui.theme.icons
local pionillium = ui.fonts.pionillium
local orbiteer = ui.fonts.orbiteer
local Vector2 = _G.Vector2

-- FIXME: need to manually set itemSpacing to be able to properly size columns
-- Need a style-var query system for best effect
local itemSpacing = ui.rescaleUI(Vector2(6, 12), Vector2(1600, 900))

-- What a tag (usually a mission) holding cargo is called in the UI
local function tagLabel(tag)
	return type(tag) == "table" and tag.GetCargoLabel and tag:GetCargoLabel() or lm.A_MISSION
end

-- Jettisoning a ton held for a mission asks first
local confirmJettison = nil ---@type { item: CommodityType, tag: any }?

local confirmModal = ModalWindow.New("JettisonMissionCargo", function(self)
	if not confirmJettison then self:close() return end
	local item, tag = confirmJettison.item, confirmJettison.tag

	ui.withFont(orbiteer.heading, function()
		ui.text(string.interp(lm.JETTISON_TITLE, { commodity = item:GetName() }))
	end)
	ui.text(string.interp(lm.JETTISON_HELD, { commodity = item:GetName(), mission = tagLabel(tag) }))
	ui.text(lm.JETTISON_MISSION_FAILS)
	ui.spacing()

	if ui.button(l.CANCEL) then
		confirmJettison = nil
		self:close()
	end
	ui.sameLine()
	if ui.button(lm.JETTISON_ANYWAY) then
		Game.player:Jettison(item, 1, 1800, tag)
		confirmJettison = nil
		self:close()
	end
end)

local jettison = function (item)
	local enabled = Game.player.flightState == "FLYING"
	local variant = enabled and ui.theme.buttonColors.default or ui.theme.buttonColors.disabled

	local button = ui.iconButton("Jettison " .. item:GetName(), icons.cargo_crate_illegal, l.JETTISON, variant)

	if button and enabled then
		-- Ordinary tons go first; the last ones may be held for a mission
		local cargoMgr = Game.player:GetComponent('CargoManager')
		if cargoMgr:CountCommodity(item, CargoManager.ORDINARY) > 0 then
			Game.player:Jettison(item, 1, 1800, CargoManager.ORDINARY)
		else
			confirmJettison = { item = item, tag = next(cargoMgr:GetTags(item)) }
			confirmModal:open()
		end
	end

	return button
end

-- The contents of the player's cargo hold. Rebuilt every frame: missions can
-- claim or release tons without changing the totals.
---@return { commodity: CommodityType, count: number, held: { tag: any, count: integer, label: string }[] }[]
local function buildCargoList()
	local count = {}

	---@type CargoManager
	local cargoMgr = Game.player:GetComponent('CargoManager')
	for name, info in pairs(cargoMgr.commodities) do
		local held = {}
		for tag, n in pairs(info.tagged or {}) do
			table.insert(held, { tag = tag, count = n, label = tagLabel(tag) })
		end
		table.sort(held, function(a, b) return a.label < b.label end)
		table.insert(count, {
			commodity = CommodityType.GetCommodity(name),
			count = info.count,
			held = held,
		})
	end

	table.sort(count, function(a, b) return a.count > b.count end)

	return count
end

local function cargolist ()
	local count = buildCargoList()

	if not ui.beginTable("cargoTable", 3, { "SizingFixedFit" }) then return end

	ui.tableSetupColumn("Amount")
	ui.tableSetupColumn("Name", { "WidthStretch" })
	ui.tableSetupColumn("Jettison")

	for _, entry in ipairs(count) do
		ui.tableNextRow()

		-- count
		ui.tableNextColumn()
		ui.alignTextToButtonPadding()
		ui.text(entry.count .. "t")

		-- name, and the missions holding some of it
		ui.tableNextColumn()
		ui.alignTextToButtonPadding()
		local heldTotal = 0
		for _, h in ipairs(entry.held) do heldTotal = heldTotal + h.count end
		if heldTotal >= entry.count and #entry.held == 1 then
			-- all of it is one mission's: mark the row
			ui.text(ui.get_icon_glyph(icons.mission_cargo) .. " " .. entry.commodity:GetProperName())
			ui.sameLine()
			ui.textColored(colors.fontDim, entry.held[1].label)
		else
			ui.text(entry.commodity:GetProperName())
			for _, h in ipairs(entry.held) do
				ui.textColored(colors.fontDim, string.interp(lm.OF_WHICH_HELD, {
					icon = ui.get_icon_glyph(icons.mission_cargo), amount = h.count, mission = h.label
				}))
			end
		end

		-- jettison button
		ui.tableNextColumn()
		jettison(entry.commodity)
	end

	ui.endTable()
end


local pumpDown = function (fuel)
	local player = Game.player
	---@type CargoManager
	local cargoMgr = player:GetComponent('CargoManager')

	if player.fuel == 0 or cargoMgr:GetFreeSpace() == 0 then
		print("No fuel left to pump!")
		return
	end

	-- internal tanks capacity
	local fuelTankMass = ShipDef[player.shipId].fuelTankMass

	-- current fuel in internal tanks, in tonnes
	local availableFuel = math.floor(player.fuel / 100 * fuelTankMass)

	-- Convert fuel to positive number, don't take more fuel than is in the tank
	local drainedFuel = math.min(math.abs(fuel), availableFuel)
	-- Don't pump down more fuel than we can fit in the cargo space available
	-- Cargo transfer UI should stay in whole-ton units
	local freeCargoInt = math.floor(cargoMgr:GetFreeSpace())
	drainedFuel = math.min(drainedFuel, freeCargoInt)

	-- Military fuel is only used for the hyperdrive
	local ok = cargoMgr:AddCommodity(Commodities.hydrogen, drainedFuel)
	if not ok then
		print("Couldn't pump fuel into cargo hold!")
		return
	end

	-- Set internal thruster fuel tank state
	-- (player.fuel is percentage, between 0-100)
	player:SetFuelPercent(math.clamp(player.fuel - drainedFuel * 100 / fuelTankMass, 0, 100))
end

-- wrapper around gaugees, for consistent size, and vertical spacing
local function gauge_bar(x, text, min, max, icon)
	local height = ui.getTextLineHeightWithSpacing()
	local cursorPos = ui.getCursorScreenPos()
	local fudge_factor = 1.0
	local gaugeWidth = ui.getContentRegion().x * fudge_factor
	local gaugePos = Vector2(cursorPos.x, cursorPos.y + height * 0.5)

	ui.gauge(gaugePos, x, '', text, min, max, icon, true,
		colors.gaugeEquipmentMarket, '', gaugeWidth, height)

	-- ui.addRect(cursorPos, cursorPos + Vector2(gaugeWidth, height), colors.gaugeCargo, 0, ui.RoundCornersNone, 1)
	ui.dummy(Vector2(gaugeWidth, height))
end

-- Gauge bar for internal, interplanetary, fuel tank
local function gauge_fuel()
	local player = Game.player
	local tankSize = ShipDef[player.shipId].fuelTankMass
	local text = string.format(l.FUEL .. ": %0.1f t \t" .. l.DELTA_V .. ": %d km/s",
		tankSize/100 * player.fuel, player:GetRemainingDeltaV()/1000)

	gauge_bar(player.fuel, text, 0, 100, icons.fuel)
end

-- Gauge bar for hyperdrive fuel / range
local function gauge_hyperdrive(drive)
	local text = string.format(l.FUEL .. ": %%0.1f t \t" .. l.HYPERSPACE_RANGE .. ": %0.1f " .. l.LY, Game.player:GetHyperspaceRange())

	gauge_bar(drive.storedFuel, text, 0, drive:GetMaxFuel(), icons.hyperspace)
end

-- Gauge bar for used/free cargo space
local function gauge_cargo()
	---@type CargoManager
	local cargoMgr = Game.player:GetComponent('CargoManager')

	local fmtString = string.format('%%i t %s / %i t %s', l.USED, cargoMgr:GetFreeSpace(), l.FREE)
	gauge_bar(cargoMgr:GetUsedSpace(), fmtString, 0, cargoMgr:GetFreeSpace() + cargoMgr:GetUsedSpace(), icons.market)
end

-- Gauge bar for used/free cabins
local function gauge_cabins()
	local player = Game.player
	local berths_free = Passengers.CountFreeBerths(player)
	local berths_used = Passengers.CountOccupiedBerths(player)
	local berths_total = berths_used + berths_free

	if berths_total == 0 then
		ui.text(l.NONE)
	else
		gauge_bar(berths_used, string.format('%%i %s / %i %s', l.USED, berths_free, l.FREE),
			0, berths_total, icons.personal)
	end
end

---@param hyperdrive Equipment.HyperdriveType
local function transfer_hyperfuel_hydrogen(hyperdrive, amt)
	local fuelTankSize = ShipDef[Game.player.shipId].fuelTankMass
	local fuelMassLeft = Game.player.fuelMassLeft

	-- Ensure we're not transferring more than the hyperdrive holds
	local delta = math.clamp(amt, -hyperdrive.storedFuel, hyperdrive:GetMaxFuel() - hyperdrive.storedFuel)
	-- Ensure we're not transferring more than the fuel tank holds
	delta = math.clamp(delta, fuelMassLeft - fuelTankSize, fuelMassLeft)

	if math.abs(delta) > 0.0001 then
		Game.player:SetFuelPercent((fuelMassLeft - delta) * 100 / fuelTankSize)
		hyperdrive:SetFuel(Game.player, hyperdrive.storedFuel + delta)
	end
end

---@param hyperdrive Equipment.HyperdriveType
local function transfer_hyperfuel_mil(hyperdrive, amt)
	local cargoMgr = Game.player:GetComponent('CargoManager')
	local availableFuel = cargoMgr:CountCommodity(hyperdrive.fuel, CargoManager.ORDINARY)
	local availableCargo = cargoMgr:GetFreeSpace()

	-- Ensure we're not transferring more than the hyperdrive holds
	local delta = math.clamp(amt, -hyperdrive.storedFuel, hyperdrive:GetMaxFuel() - hyperdrive.storedFuel)
	-- Ensure we're not transferring more than the cargo bay holds
	delta = math.clamp(delta, -availableCargo, availableFuel)

	-- TODO(CargoManager): liquid tanks / partially-filled cargo containers
	-- Until then, we round down to a whole integer in both directions since we're dealing with integer cargo masses
	delta = math.modf(delta)

	if delta > 0 then
		cargoMgr:RemoveCommodity(hyperdrive.fuel, delta, CargoManager.ORDINARY)
		hyperdrive:SetFuel(Game.player, hyperdrive.storedFuel + delta)
	elseif delta < 0 then
		cargoMgr:AddCommodity(hyperdrive.fuel, math.abs(delta))
		hyperdrive:SetFuel(Game.player, hyperdrive.storedFuel + delta)
	end
end

-- every fuel button is as wide as the widest label any of them carries, so
-- none is cut off at high resolutions and the rows stay aligned
local function fuelButtonSize()
	return Vector2(ui.calcButtonSize(ui.get_icon_glyph(icons.chevron_right) .. "-100").x, 0)
end

local function fuelTransferButton(drive, amt)
	local driveEnabled = false

	-- Don't allow transferring fuel while the hyperdrive is doing its thing
	if Game.player:IsHyperspaceActive() or
			Game.player.flightState == "JUMPING" or
			Game.player.flightState == "HYPERSPACE" then
		driveEnabled = true
	end

	local color = driveEnabled and ui.theme.buttonColors.disabled or ui.theme.buttonColors.default
	local icon = amt < 0 and icons.chevron_up or icons.chevron_down
	local tooltip = amt < 0 and l.PUMP_DOWN_FROM_HYPERDRIVE_TOOLTIP or l.PUMP_DOWN_TO_HYPERDRIVE_TOOLTIP

	if ui.button(ui.get_icon_glyph(icon) .. tostring(math.abs(amt)), fuelButtonSize(), color, string.format(tooltip, math.abs(amt))) then
		if not driveEnabled then
			if drive.fuel == Commodities.hydrogen then
				transfer_hyperfuel_hydrogen(drive, amt)
			elseif drive.fuel == Commodities.military_fuel then
				transfer_hyperfuel_mil(drive, amt)
			end
		end
	end
end

local function drawCentered(id, fun)
	if not ui.beginTable(id .. "##centered", 3) then return end

	ui.tableSetupColumn("leftPadding")
	ui.tableSetupColumn("content", { "WidthFixed" })
	ui.tableSetupColumn("rightPadding")

	ui.tableNextRow()
	ui.tableSetColumnIndex(1)

	fun()

	ui.endTable()
end

local function drawFuelTransfer(drive)
	drawCentered("Hyperdrive", function()
		ui.horizontalGroup(function()
			fuelTransferButton(drive, 10)
			fuelTransferButton(drive, 1)

			ui.text(l.TRANSFER_FUEL)

			fuelTransferButton(drive, -1)
			fuelTransferButton(drive, -10)
		end)
	end)
end

local function drawPumpDialog()
	local width1 = ui.calcTextSize(l.PUMP_DOWN)
	local width2 = ui.calcTextSize(l.REFUEL)

	local width
	if width2.x > width1.x then width = width2.x else width = width1.x end
	width = width * 1.2

	-- to-do: maybe show disabled version of button if fuel==100 or hydrogen==0
	-- (if so, what about military fuel?)
	-- at the moment: no visual cue for this.
	ui.alignTextToLineHeight(ui.getButtonHeight())
	ui.text(l.REFUEL)
	ui.sameLine(width)
	local options = {1, 10, 100}
	for _, k in ipairs(options) do
		if ui.button(ui.get_icon_glyph(icons.chevron_left) .. tostring(k)  .. "##fuel", fuelButtonSize(), nil, string.format(l.PUMP_DOWN_FROM_CARGO_TOOLTIP, k)) then
			-- Refuel k tonnes from cargo hold
			Game.player:Refuel(Commodities.hydrogen, k)
		end
		ui.sameLine()
	end
	ui.text("")

	-- To-do: Maybe also show disabled version of button if currentfuel == 0 or player:GetEquipFree("cargo") == 0
	-- at the moment: no visual cue for this.
	ui.alignTextToLineHeight(ui.getButtonHeight())
	ui.text(l.PUMP_DOWN)
	ui.sameLine(width)
	for _, v in ipairs(options) do
		local fuel = -1*v
		if ui.button(ui.get_icon_glyph(icons.chevron_right) .. fuel .. "##pump", fuelButtonSize(), nil, string.format(l.PUMP_DOWN_TO_CARGO_TOOLTIP, v)) then
			pumpDown(fuel)
		end
		ui.sameLine()
	end
	ui.text("")
end

local function drawEconTrade()
	local player = Game.player
	local drive = player:GetInstalledHyperdrive()

	ui.withFont(orbiteer.heading, function() ui.text(l.FUEL) end)

	gauge_fuel()

	drawCentered("Pump Dialog", function()
		drawPumpDialog()
	end)

	if drive then
		ui.withFont(orbiteer.heading, function() ui.text(l.HYPERDRIVE) end)

		drawFuelTransfer(drive)
		gauge_hyperdrive(drive)
	end

	ui.newLine()
	ui.withFont(orbiteer.heading, function() ui.text(l.CABINS) end)

	gauge_cabins()

	ui.newLine()

	ui.withFont(orbiteer.heading, function() ui.text(l.FINANCE) end)
	ui.text(l.CASH)
	ui.sameLine()
	ui.text(ui.Format.Money(PlayerState.GetMoney()))

end

InfoView:registerView({
	id = "econTrade",
	name = l.ECONOMY_TRADE,
	icon = ui.theme.icons.cargo_manifest,
	showView = true,

	draw = function()
		ui.withStyleVars({ItemSpacing = itemSpacing}, function()
			ui.withFont(pionillium.body, function()
				ui.horizontalGroup(function()
					local spacing = itemSpacing.x + ui.getWindowPadding().x
					local region = ui.getContentRegion()
					local sizex = region.x / 2 - spacing
					local sizey = region.y - StationView.style.height

					ui.child("leftpanel", Vector2(sizex, sizey), function()
						drawEconTrade()
					end)

					-- add innner padding
					ui.sameLine(0, spacing)

					ui.separator()
					-- add innner padding
					ui.sameLine(0, spacing)

					ui.child("rightpanel", Vector2(sizex, sizey), function()
						ui.withFont(orbiteer.heading, function() ui.text(l.CARGO) end)
						gauge_cargo()
						cargolist()
					end)
				end)
			end)
		end)
	end,

	refresh = function()
		confirmJettison = nil
	end,

	debugReload = function()
		package.reimport()
	end
})
