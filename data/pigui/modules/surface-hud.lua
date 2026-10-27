-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- The HUD out of the ship, on foot (Game.GetExcursionMode): a dot in the
-- middle of the view, a compass along the top with the bearing and distance
-- to the ship, what the interact key would do, and the jetpack's fuel.

local Game = require 'Game'
local Lang = require 'Lang'
local bindManager = require 'bind-manager'
local ui = require 'pigui'
local gameView = require 'pigui.views.game'
local Vector2 = _G.Vector2

local lc = Lang.GetResource("core")
local lui = Lang.GetResource("ui-core")
local pionillium = ui.fonts.pionillium

local interact = bindManager.registerAction('BindInteract')

-- degrees of heading across the compass
local COMPASS_SPAN = 120

local directions = {
	[0] = lc.COMPASS_N, [45] = lc.COMPASS_NE, [90] = lc.COMPASS_E, [135] = lc.COMPASS_SE,
	[180] = lc.COMPASS_S, [225] = lc.COMPASS_SW, [270] = lc.COMPASS_W, [315] = lc.COMPASS_NW
}

-- an angle in degrees as -180..180
local function wrap180(deg)
	return (deg + 180) % 360 - 180
end

local function displayCompass(heading, shipBearing, shipDistance)
	local colors = ui.theme.colors
	local width = math.min(ui.screenWidth * 0.4, 640)
	local centre = Vector2(ui.screenWidth / 2, ui.screenHeight * 0.07)
	local function xOf(deg)
		return centre.x + wrap180(deg - heading) / COMPASS_SPAN * width
	end

	ui.addLine(Vector2(centre.x - width / 2, centre.y), Vector2(centre.x + width / 2, centre.y), colors.reticuleCircle, 1)

	local first = math.ceil((heading - COMPASS_SPAN / 2) / 15) * 15
	for d = first, heading + COMPASS_SPAN / 2, 15 do
		local x = xOf(d)
		local label = directions[d % 360]
		local tick = label and 8 or 4
		ui.addLine(Vector2(x, centre.y), Vector2(x, centre.y - tick), colors.reticuleCircle, label and 2 or 1)
		if label then
			ui.addStyledText(Vector2(x, centre.y - tick - 2), ui.anchor.center, ui.anchor.bottom, label, colors.navigationalElements, pionillium.small, "")
		end
	end

	-- where the view looks
	ui.addLine(centre, centre + Vector2(0, 10), colors.white, 2)

	-- the ship, on the compass or held at its nearer end
	local offset = math.max(-COMPASS_SPAN / 2, math.min(COMPASS_SPAN / 2, wrap180(shipBearing - heading)))
	local shipPos = Vector2(centre.x + offset / COMPASS_SPAN * width, centre.y + 4)
	ui.addStyledText(shipPos, ui.anchor.center, ui.anchor.top,
		lui.SURFACE_YOUR_SHIP .. " " .. ui.Format.Distance(shipDistance), colors.navTarget, pionillium.small, "")
end

local function displayPrompt()
	local colors = ui.theme.colors
	if Game.GetExcursionAction() ~= "board" then return end

	local key = bindManager.getChordDesc(interact.action.binding)
	local text = lui.SURFACE_BOARD_SHIP
	if key ~= "" then
		text = "[" .. key .. "]  " .. text
	end
	ui.addStyledText(Vector2(ui.screenWidth / 2, ui.screenHeight * 0.62), ui.anchor.center, ui.anchor.center,
		text, colors.white, pionillium.large, nil, colors.lightBlackBackground)
end

-- the jetpack's fuel: a bar under the view, red while it refills after running dry
local function displayJetpack()
	local fuel, thrust, burntOut = Game.GetJetpackState()
	if not fuel then return end
	local colors = ui.theme.colors

	local size = Vector2(math.min(ui.screenWidth * 0.12, 200), 6)
	local min = Vector2((ui.screenWidth - size.x) / 2, ui.screenHeight * 0.9)
	local max = min + size
	local fill = burntOut and colors.alertRed or (thrust > 0 and colors.white or colors.navigationalElements)

	ui.addRectFilled(min, max, colors.lightBlackBackground, 0, ui.RoundCornersNone)
	ui.addRectFilled(min, Vector2(min.x + size.x * fuel, max.y), fill, 0, ui.RoundCornersNone)
	ui.addStyledText(Vector2(ui.screenWidth / 2, min.y - 2), ui.anchor.center, ui.anchor.bottom,
		lui.SURFACE_JETPACK, colors.navigationalElements, pionillium.small, "")
end

-- in space: how far the ship is and how fast it is coming or going
local function displayEvaShip()
	local colors = ui.theme.colors
	local me = Game.GetControlledBody()
	local distance = me:GetPositionRelTo(Game.player):length()
	local speed = me:GetVelocityRelTo(Game.player):length()
	local text = string.interp(lui.SURFACE_EVA_SHIP, {
		distance = ui.Format.Distance(distance),
		speed = ui.Format.Speed(speed)
	})
	ui.addStyledText(Vector2(ui.screenWidth / 2, ui.screenHeight * 0.07), ui.anchor.center, ui.anchor.top,
		text, colors.navTarget, pionillium.medium, "")
end

local function displaySurfaceHud()
	if Game.GetExcursionMode() == "ship" or ui.optionsWindow.isOpen then return end
	local colors = ui.theme.colors

	ui.addCircleFilled(gameView.center, 2, colors.white:opacity(0.7), 8)

	if Game.GetExcursionMode() == "eva" then
		displayEvaShip()
	else
		local heading, shipBearing, shipDistance = Game.GetExcursionNavInfo()
		if heading then
			displayCompass(heading, shipBearing, shipDistance)
		end
	end

	displayPrompt()
	displayJetpack()

	ui.addStyledText(Vector2(ui.screenWidth / 2, ui.screenHeight - 8), ui.anchor.center, ui.anchor.bottom,
		lui.SURFACE_FREE_CURSOR, colors.navigationalElements, pionillium.small, "")
end

gameView.registerModule("surface-hud", {
	showInHyperspace = false,
	showOffShip = true,
	draw = function(self, dT)
		displaySurfaceHud()
	end,
	debugReload = function()
		package.reimport()
	end
})

return {}
