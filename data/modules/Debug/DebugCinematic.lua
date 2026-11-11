-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Debug tab for the cinematic camera: the camera comes loose from the ship
-- and stays where it is, and the keys and the mouse fly it about to film the
-- ship and the world (src/ship/CinematicCamera.cpp). It is never saved.

local Game = require 'Game'
local Dev  = require 'Dev'

local ui = require 'pigui'

local debugView = require 'pigui.views.debug'

local icons = ui.theme.icons
local colors = ui.theme.colors

--=============================================================================

local KEYS = {
	{ "W / S", "forward / back" },
	{ "A / D", "left / right" },
	{ "R / F", "up / down" },
	{ "Q / E", "tilt (only with \"Keep the horizon level\" off)" },
	{ "Right mouse button", "hold and move the mouse to look about" },
	{ "Mouse wheel", "faster / slower" },
	{ "Shift", "hold for ten times faster" },
	{ "Ctrl+I", "hide or show this window" },
}

local function checkbox(label, value, field, tip)
	local changed, v = ui.checkbox(label, value)
	if tip and ui.isItemHovered() then ui.setTooltip(tip) end
	if changed then Dev.SetCinematicCamera({ [field] = v }) end
end

local function drawSettings(cam)
	checkbox("Keys move the camera", cam.keysMoveCamera, "keysMoveCamera",
		"Off: the camera holds still where it is and the keys fly the ship again.")
	checkbox("Keep looking at the ship", cam.lookAtShip, "lookAtShip",
		"The camera turns by itself to keep the ship in the middle.")
	checkbox("Travel with the ship", cam.travelWithShip, "travelWithShip",
		"The camera moves along with the ship and keeps the same gap to it.")
	checkbox("Turn with the station", cam.rideWithStation, "rideWithStation",
		"Near a space station the camera goes round with it as it spins, so it keeps its place by the station. Off: it holds still against the stars.")
	checkbox("Stop at the ground", cam.stopAtGround, "stopAtGround",
		"Off: the camera goes through the ground and into rock too. It always goes through bases, stations and ships.")
	checkbox("Keep the horizon level", cam.levelHorizon, "levelHorizon",
		"No tilting sideways: turning left and right goes round the up direction of the planet below.")
	checkbox("Hide the game's panels", cam.hideHud, "hideHud",
		"Hides the flight panels and markers while the camera is loose. This window stays: Ctrl+I hides it.")

	ui.separator()
	ui.pushItemWidth(ui.getTextLineHeight() * 14)

	-- the speed goes from 0.1 m/s to a million km/s, so the slider moves by powers of ten
	local power = math.log(cam.speed, 10)
	local newPower, changed = ui.sliderFloat("Speed##cinematicSpeed", power, -1, 9, ui.Format.Speed(cam.speed, true))
	if changed then Dev.SetCinematicCamera({ speed = 10 ^ newPower }) end

	local smoothing, changedS = ui.sliderFloat("Smoothness (s)##cinematicSmooth", cam.smoothing, 0, 3, "%.2f")
	if ui.isItemHovered() then ui.setTooltip("How long the camera takes to settle after a key or the mouse. 0 is instant.") end
	if changedS then Dev.SetCinematicCamera({ smoothing = smoothing }) end

	local fov, changedF = ui.sliderFloat("Field of view (degrees)##cinematicFov", cam.fov, 5, 120, "%.0f")
	if ui.isItemHovered() then ui.setTooltip("Small numbers zoom in. Set back when the camera goes back to the ship.") end
	if changedF then Dev.SetCinematicCamera({ fov = fov }) end

	ui.popItemWidth()
end

local function drawKeys()
	ui.separator()
	ui.text("Keys while the camera flies")
	for _, k in ipairs(KEYS) do
		ui.textColored(colors.fontDim, k[1])
		ui.sameLine(ui.getTextLineHeight() * 10)
		ui.text(k[2])
	end
end

debugView.registerTab("DebugCinematic", {
	mod = true,
	label = "Cinematic camera",
	icon = icons.view_flyby,
	show = function() return Game.player ~= nil end,
	draw = function()
		local cam = Dev.GetCinematicCamera()
		if not cam then
			ui.text("No game running.")
			return
		end

		if cam.on then
			if ui.button("Back to the ship's camera") then
				Dev.SetCinematicCamera({ on = false })
			end
			ui.sameLine()
			ui.textColored(colors.fontDim, "{} from the ship, moving at {}, in {}" % {
				ui.Format.Distance(cam.distance), ui.Format.Speed(cam.currentSpeed, true), cam.frame })
		elseif cam.available then
			if ui.button("Detach the camera") then
				Dev.SetCinematicCamera({ on = true })
			end
			ui.sameLine()
			ui.textColored(colors.fontDim, "It starts where the camera is now.")
		else
			ui.textColored(colors.fontDim, "Only from the ship's 3D view: not from the station's screen or a map, not on foot, not in hyperspace.")
		end

		if cam.on and cam.keysMoveCamera then
			ui.textColored(colors.fontDim, "The ship's controls wait while the keys fly the camera; it carries on as it was.")
		end

		ui.separator()
		drawSettings(cam)
		drawKeys()
	end
})
