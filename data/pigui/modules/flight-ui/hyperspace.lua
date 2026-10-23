-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

local ui = require 'pigui'
local Game = require 'Game'
local Vector2 = _G.Vector2

-- cache ui
local pionillium = ui.fonts.pionillium
local colors = ui.theme.colors

local lui = require 'Lang'.GetResource("ui-core")

local gameView = require 'pigui.views.game'

-- speed as a fraction of c, with enough decimals to show all the nines
local function formatBeta(beta, oneMinusBeta)
	if oneMinusBeta >= 1e-3 then
		return string.format("%.3f", beta)
	end
	local nines = math.floor(-math.log(oneMinusBeta) / math.log(10))
	return string.format("%." .. math.min(nines + 2, 15) .. "f", beta)
end

-- the transit is shown at the top of the screen, out of the way of the bright point ahead
local function displayHyperspace()
	local uiPos = Vector2(ui.screenWidth / 2, ui.screenHeight / 6)
	local path,destName = Game.player:GetHyperspaceDestination()
	local label = string.interp(lui.HUD_IN_TRANSIT_TO_N_X_X_X, { system = destName, x = path.sectorX, y = path.sectorY, z = path.sectorZ })
	local r = ui.addStyledText(uiPos, ui.anchor.center, ui.anchor.top, label, colors.hyperspaceInfo, pionillium.large, nil, colors.lightBlackBackground)
	uiPos.y = uiPos.y + r.y + 10
	local percent = Game.GetHyperspaceTravelledPercentage() * 100
	label = string.interp(lui.HUD_JUMP_COMPLETE, { percent = string.format("%2.1f", percent) })
	r = ui.addStyledText(uiPos, ui.anchor.center, ui.anchor.top, label, colors.hyperspaceInfo, pionillium.large, nil, colors.lightBlackBackground)

	local beta, oneMinusBeta, gamma = Game.GetHyperspaceRelativity()
	uiPos.y = uiPos.y + r.y + 10
	label = string.interp(lui.HUD_HYPERSPACE_VELOCITY, { velocity = formatBeta(beta, oneMinusBeta), gamma = string.format(gamma < 100 and "%.2f" or "%.0f", gamma) })
	ui.addStyledText(uiPos, ui.anchor.center, ui.anchor.top, label, colors.hyperspaceInfo, pionillium.medium, nil, colors.lightBlackBackground)
end

local function displayHyperspaceCountdown()
	local player = Game.player
	if player:IsHyperspaceActive() then
		local countdown = math.ceil(player:GetHyperspaceCountdown())
                local path,destName = player:GetHyperspaceDestination()
		local uiPos = Vector2(ui.screenWidth / 2, ui.screenHeight / 3 - 20)
		ui.addStyledText(uiPos, ui.anchor.center, ui.anchor.bottom, string.interp(lui.HUD_HYPERSPACING_TO_N_IN_N_SECONDS ,{ destination = destName, countdown = countdown }), colors.hyperspaceInfo, pionillium.large)
	end
end

gameView.registerModule("hyperspace-percentage", {
    showInHyperspace = true,
    draw = function(self, dT)
        colors = ui.theme.colors
        if Game.InHyperspace() then
            displayHyperspace()
        elseif gameView.player:IsHyperspaceActive() then
            displayHyperspaceCountdown()
        end
    end
})
