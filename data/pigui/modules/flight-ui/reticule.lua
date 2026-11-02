-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

local ui = require 'pigui'
local Engine = require 'Engine'
local Event = require 'Event'
local Game = require 'Game'
local Vector2 = _G.Vector2
local Vector3 = _G.Vector3
local bindManager = require 'bind-manager'
local Gravity = require 'pigui.libs.gravity'
local AscentGuidance = require 'pigui.libs.ascent-guidance'

-- cache ui
local pionillium = ui.fonts.pionillium
local pionicons = ui.fonts.pionicons
local colors = ui.theme.colors
local icons = ui.theme.icons

local lc = require 'Lang'.GetResource("core")
local lui = require 'Lang'.GetResource("ui-core")

local reticuleCircleRadius = math.min(ui.screenWidth, ui.screenHeight) / 8
local reticuleCircleThickness = 2.0

-- the reticle's two halves, moved apart: arcs of a wider circle left and right
-- of the view, each spanning sideArcHours clock hours either side of 9 / 3 o'clock
local sideArcRadius = math.min(ui.screenHeight * 0.45, ui.screenWidth * 0.27)
local sideArcHours = 1
local sideArcHalfAngle = sideArcHours / 6 * math.pi
-- the delta-v and brake gauge bars along the arcs (px): how wide each bar is,
-- its gap from the arc, and the gap between bars side by side
local gaugeThickness = 7
local gaugeOffset = 5
local gaugeGap = 3
-- the frame and target readouts sit just outside the arcs and their gauges
local sideLabelRadius = sideArcRadius + gaugeOffset + gaugeThickness * 2 + gaugeGap + reticuleCircleRadius * 0.1
-- the space the arcs frame: an ellipse as wide as the arcs but flatter, clear of
-- the heading tape above and the radar below; the small marker indicators run
-- along its inside
local reticuleFrame = Vector2(sideArcRadius, sideArcRadius * 0.55)

-- degrees of heading across the heading tape
local headingTapeSpan = 60

-- the pitch ladder in the middle of the view (fractions of sideArcRadius):
-- rungs show only within ladderClip of the center, the horizon line reaches
-- horizonHalfWidth either side, and each rung runs from rungInner to rungOuter
local ladderClip = 0.27
local horizonHalfWidth = 0.4
local rungInner = 0.1
local rungOuter = 0.22
-- degrees between rungs, and degrees of heading either side shown on the horizon
local ladderStep = 5
local horizonHeadingSpan = 60

-- the speed (left) and altitude (right) tapes: distance from the center and
-- half height (fractions of sideArcRadius), and line thickness (px)
local tapeOffset = 0.55
local tapeHalfHeight = 0.33
local tapeThickness = 2

-- the ladder and the tapes hide above hudCeiling planet radii of altitude and
-- show again below hudFloor, so they do not flicker at the boundary
local hudCeiling = 1.0
local hudFloor = 0.9

-- the vertical speed indicator just outside the altitude tape: in flight a
-- compressed scale out to vsiMax (m/s) either way, and near the ground a
-- linear one from +vsiLandingTop down to -vsiLandingBottom with the
-- touchdown bands (descent rates, m/s, and their colors)
local vsiMax = 1e4
local vsiLandingTop = 20
local vsiLandingBottom = 60
local vsiLandingBands = { { 10, "brakeNow" }, { 30, "alertYellow" }, { 60, "alertRed" } }
-- the landing scale shows below vsiLandingAltitude (m) while descending
-- slower than vsiLandingDescent and climbing slower than vsiLandingClimb
-- (m/s); once shown, these grow by vsiLandingHysteresis so it does not flicker
local vsiLandingAltitude = 2000
local vsiLandingDescent = 80
local vsiLandingClimb = 25
local vsiLandingHysteresis = 1.5
-- the trend line on the altitude tape shows the altitude this many seconds ahead
local vsiTrendSeconds = 10

-- the ascent guidance director turns to its aligned color with the nose
-- within this many degrees of it
local ascentAlignedAngle = 2

local flightAssistIconSize = ui.theme.styles.SmallButtonSize.x
-- first we calculate the padding so that it does not turn out to be fractional
-- precise padding is needed so that the radial menu is the same for both menus
local flightAssistButtonPadding = math.ceil((32 / 18 * flightAssistIconSize - flightAssistIconSize) / 2)
local flightAssistButtonSize = flightAssistIconSize + flightAssistButtonPadding * 2

local frameSwitchStyle = ui.Style:clone({
	colors = {},
	vars = {
		ItemSpacing = Vector2(0, 0),
		FrameRounding = ui.rescaleUI(6)
	}
})

local frameDetailStyle = ui.Style:clone({
	colors = {
		Text = colors.frame
	},
	vars = {
		ItemSpacing = ui.rescaleUI(Vector2(8, 3))
	}
})

local gameView = require 'pigui.views.game'

-- for modules
ui.reticuleCircleRadius = reticuleCircleRadius
ui.reticuleCircleThickness = reticuleCircleThickness
ui.reticuleFrame = reticuleFrame

-- center of screen, set each frame by the handler
local center = nil

-- cache player each frame
local player = Game.player ---@type Player

-- active target information to display on the flight reticle
local reticuleTarget = "frame"

-- this should go into HUD settings
local showNavigationalNumbers = true

-- to interact with actions and axes in the input system
local bindings = {
	assistRadial = bindManager.registerAction('BindFlightAssistRadial'),
	fixheadingRadial = bindManager.registerAction('BindFixheadingRadial'),
	targetRadial = bindManager.registerAction('BindTargetRadial'),
}

local function textShadowedRight(label)
	ui.addCursorPos(Vector2(-ui.calcTextSize(label).x, 0))
	ui.textShadowed(label)
end

-- display the pitch indicator on the inside of the right arc
local function displayReticulePitch(pitch_degrees)
	local tick_length = 4
	local radius = sideArcRadius - 1
	local top, bottom = 3 - sideArcHours, 3 + sideArcHours
	ui.lineOnClock(center, 3, tick_length, radius, colors.navigationalElements, 1)
	ui.lineOnClock(nil, 3 - sideArcHours / 2, tick_length, radius, colors.navigationalElements, 1)
	ui.lineOnClock(nil, 3 + sideArcHours / 2, tick_length, radius, colors.navigationalElements, 1)
	ui.lineOnClock(nil, top, tick_length, radius, colors.navigationalElements, 1)
	ui.lineOnClock(nil, bottom, tick_length, radius, colors.navigationalElements, 1)

	local xpitch = (pitch_degrees + 90) / 180
	local xpitch_h = bottom - xpitch * (bottom - top)
	ui.lineOnClock(nil, xpitch_h, tick_length * 1.5, radius, colors.navigationalElements, 2)
end

-- the camera's focal length in pixels, so that the ladder's degrees match the view
local function cameraFocalLength()
	local angle = math.rad(10)
	local ahead = Engine.CameraSpaceToScreenSpace(Vector3(0, 0, -1))
	local above = Engine.CameraSpaceToScreenSpace(Vector3(0, math.sin(angle), -math.cos(angle)))
	return (ahead.y - above.y) / math.tan(angle)
end

-- display the pitch ladder, rolled with the ship: the horizon line carries the
-- heading every 5 degrees (labelled every 10) and a caret under the nose, and a
-- rung every ladderStep degrees, solid above the horizon and dashed below
-- (pitch and roll in radians, heading in degrees)
local function displayPitchLadder(heading, pitch, roll)
	local focal = cameraFocalLength()
	-- along the rungs, and towards the sky
	local along = Vector2(math.cos(roll), math.sin(roll))
	local up = Vector2(math.sin(roll), -math.cos(roll))
	local clip = sideArcRadius * ladderClip
	local color = colors.reticuleCircle

	local function point(x, y)
		return center + along * x + up * y
	end

	for deg = -90, 90, ladderStep do
		local angle = math.rad(deg) - pitch
		local offset = focal * math.tan(angle)
		if math.abs(angle) < ui.pi_2 and math.abs(offset) <= clip then
			if deg == 0 then
				local half = sideArcRadius * horizonHalfWidth
				ui.addLine(point(-half, offset), point(half, offset), color, 1.5)

				-- headings where they meet the horizon; the ground is below the line
				local cos_pitch = math.cos(pitch)
				if cos_pitch > 0.05 then
					local first = math.ceil((heading - horizonHeadingSpan) / 5) * 5
					for d = first, heading + horizonHeadingSpan, 5 do
						local x = focal * math.tan(math.rad(d - heading)) / cos_pitch
						if math.abs(x) <= half then
							local labelled = d % 10 == 0
							local p = point(x, offset)
							ui.addLine(p, p - up * (labelled and 6 or 3), color, 1)
							if labelled then
								local tens = math.floor(d % 360 / 10)
								local label = string.format("%02d", tens == 0 and 36 or tens)
								ui.addStyledText(p - up * 14, ui.anchor.center, ui.anchor.center, label, color, pionillium.tiny, "")
							end
						end
					end
				end
				local nose = point(0, offset)
				ui.addLine(nose - up * 9 - along * 4, nose - up * 3, color, 1)
				ui.addLine(nose - up * 3, nose - up * 9 + along * 4, color, 1)
			else
				local inner = sideArcRadius * rungInner
				local outer = sideArcRadius * rungOuter
				-- the rungs' ends turn towards the horizon
				local tick = up * (deg > 0 and -6 or 6)
				for _, side in ipairs({ -1, 1 }) do
					local a = point(side * inner, offset)
					local b = point(side * outer, offset)
					if deg > 0 then
						ui.addLine(a, b, color, 1)
					else
						for k = 0, 2 do
							ui.addLine(a + (b - a) * (k / 3), a + (b - a) * (k / 3 + 0.2), color, 1)
						end
					end
					ui.addLine(b, b + tick, color, 1)
					ui.addStyledText(point(side * (outer + 12), offset), ui.anchor.center, ui.anchor.center, tostring(deg), color, pionillium.tiny, "")
				end
			end
		end
	end
end

-- the largest of 1, 2 or 5 times a power of ten that is no more than x
local function niceStep(x)
	local magnitude = 10 ^ math.floor(math.log(x, 10))
	local n = x / magnitude
	if n >= 5 then
		return 5 * magnitude
	elseif n >= 2 then
		return 2 * magnitude
	end
	return magnitude
end

-- 16490.5 -> 16,490.5
local function groupThousands(text)
	local n
	repeat
		text, n = text:gsub("^(-?%d+)(%d%d%d)", "%1,%2")
	until n == 0
	return text
end

-- a diamond centered on p, d from the center to each corner: filled or outlined
local function diamond(p, d, color, filled)
	local top, right, bottom, left = p + Vector2(0, -d), p + Vector2(d, 0), p + Vector2(0, d), p + Vector2(-d, 0)
	if filled then
		ui.addTriangleFilled(top, right, bottom, color)
		ui.addTriangleFilled(top, bottom, left, color)
	else
		ui.addLine(top, right, color, 2)
		ui.addLine(right, bottom, color, 2)
		ui.addLine(bottom, left, color, 2)
		ui.addLine(left, top, color, 2)
	end
end

-- display a scrolling tape beside the ladder, left (side -1) or right (side 1),
-- with the current value boxed: value and minStep are in units of scale,
-- ticks every half step and labels every step, about ten steps to the value;
-- returns the tape's pixels per unit of value
local function displayTape(side, value, scale, minStep, valueText, unit, tooltip)
	local v = value / scale
	local step = v > 0 and math.max(minStep, niceStep(v / 10)) or minStep
	local half = step / 2
	local decimals = math.max(0, math.ceil(-math.log(step, 10) - 1e-6))
	local x = center.x + side * sideArcRadius * tapeOffset
	local tapeHalf = sideArcRadius * tapeHalfHeight
	-- two labelled steps either side of the value
	local stepPx = tapeHalf / 2
	local color = colors.reticuleCircle
	local outward = side < 0 and ui.anchor.right or ui.anchor.left

	ui.addLine(Vector2(x, center.y - tapeHalf), Vector2(x, center.y + tapeHalf), color, tapeThickness)
	for i = math.ceil((v - 2 * step) / half), math.floor((v + 2 * step) / half) do
		local val = i * half
		if val >= 0 then
			local y = center.y - (val - v) / step * stepPx
			local major = i % 2 == 0
			local length = major and 8 or 4
			ui.addLine(Vector2(x, y), Vector2(x + side * length, y), color, tapeThickness)
			if major then
				local label = groupThousands(string.format("%." .. decimals .. "f", val))
				ui.addStyledText(Vector2(x + side * (length + 4), y), outward, ui.anchor.center, label, color, pionillium.small, "")
			end
		end
	end

	-- the current value, boxed over the labels and pointing at the tape
	local size = ui.calcTextSize(valueText, pionillium.body)
	local padding = 3
	local inner = x + side * 10
	local outer = inner + side * (size.x + padding * 2)
	local topLeft = Vector2(math.min(inner, outer), center.y - size.y / 2 - padding)
	local bottomRight = Vector2(math.max(inner, outer), center.y + size.y / 2 + padding)
	ui.addRectFilled(topLeft, bottomRight, colors.lightBlackBackground, 0, ui.RoundCornersNone)
	ui.addRect(topLeft, bottomRight, color, 0, ui.RoundCornersNone, tapeThickness)
	ui.addLine(Vector2(inner, center.y - 4), Vector2(x, center.y), color, tapeThickness)
	ui.addLine(Vector2(x, center.y), Vector2(inner, center.y + 4), color, tapeThickness)
	ui.addStyledText(Vector2((inner + outer) / 2, center.y), ui.anchor.center, ui.anchor.center, valueText, color, pionillium.body, tooltip)

	ui.addStyledText(Vector2(x + side * 8, center.y + tapeHalf + 4), outward, ui.anchor.top, unit, color, pionillium.small, "")

	return stepPx / (step * scale)
end

-- whether the VSI showed its landing scale last time, for the hysteresis
local vsiLanding = false

local function isVsiLanding(verticalSpeed, altitude)
	local grow = vsiLanding and vsiLandingHysteresis or 1
	vsiLanding = altitude < vsiLandingAltitude * grow
		and verticalSpeed > -vsiLandingDescent * grow
		and verticalSpeed < vsiLandingClimb * grow
	return vsiLanding
end

-- how far above the middle of the VSI (px, half its height) a vertical speed sits:
-- logarithmic on the compressed scale, linear on the landing scale, clamped to the ends
local function vsiOffset(verticalSpeed, landing, half)
	if landing then
		local clamped = math.max(-vsiLandingBottom, math.min(vsiLandingTop, verticalSpeed))
		return half - (vsiLandingTop - clamped) / (vsiLandingTop + vsiLandingBottom) * 2 * half
	end
	local fraction = math.min(math.log(1 + math.abs(verticalSpeed), 10) / math.log(1 + vsiMax, 10), 1)
	return (verticalSpeed < 0 and -fraction or fraction) * half
end

-- -1.234 -> "-1.2 m/s", 142.6 -> "+143 m/s", -2500 -> "-2.50 km/s"
local function formatVerticalSpeed(verticalSpeed)
	local speed = math.abs(verticalSpeed) < 0.05 and 0 or verticalSpeed
	local text, unit
	if math.abs(speed) < 10 then
		text, unit = string.format("%.1f", speed), lc.UNIT_METERS_PER_SECOND
	elseif math.abs(speed) < 1000 then
		text, unit = string.format("%.0f", speed), lc.UNIT_METERS_PER_SECOND
	else
		text, unit = ui.Format.SpeedUnit(speed)
	end
	return (speed > 0 and "+" or "") .. text .. " " .. unit
end

-- display the trend line on the inner side of the altitude tape: from the
-- current altitude to where it will be in vsiTrendSeconds (pixels, up positive)
local function displayAltitudeTrend(pixels)
	local tapeHalf = sideArcRadius * tapeHalfHeight
	pixels = math.max(-tapeHalf, math.min(tapeHalf, pixels))
	if math.abs(pixels) < 2 then return end
	local x = center.x + sideArcRadius * tapeOffset - 4
	ui.addLine(Vector2(x, center.y), Vector2(x, center.y - pixels), colors.reticuleCircle, 3)
end

-- display the vertical speed indicator at x, just outside the altitude tape:
-- the scale, a pointer with the vertical speed, and a diamond at the fastest
-- descent the up thrusters can still stop before the ground
local function displayVerticalSpeed(verticalSpeed, altitude, x)
	local half = sideArcRadius * tapeHalfHeight
	local landing = isVsiLanding(verticalSpeed, altitude)
	local color = colors.reticuleCircle

	local function yAt(speed)
		return center.y - vsiOffset(speed, landing, half)
	end

	local function tick(speed, length, label)
		local y = yAt(speed)
		ui.addLine(Vector2(x, y), Vector2(x + length, y), color, 1)
		if label then
			ui.addStyledText(Vector2(x + length + 3, y), ui.anchor.left, ui.anchor.center, label, color, pionillium.tiny, "")
		end
	end

	ui.addLine(Vector2(x, center.y - half), Vector2(x, center.y + half), color, tapeThickness)

	if landing then
		-- the touchdown bands down the inner side of the scale
		local from = 0
		for _, band in ipairs(vsiLandingBands) do
			ui.addRectFilled(Vector2(x - 6, yAt(-from)), Vector2(x - 2, yAt(-band[1])), colors[band[2]], 0, ui.RoundCornersNone)
			from = band[1]
		end
		local labelled = { [20] = true, [0] = true, [-10] = true, [-30] = true, [-60] = true }
		for speed = -vsiLandingBottom, vsiLandingTop, 10 do
			tick(speed, speed == 0 and 8 or 4, labelled[speed] and tostring(math.abs(speed)))
		end
	else
		tick(0, 8, "0")
		for _, mark in ipairs({ { 10, "10" }, { 100, "100" }, { 1000, "1k" } }) do
			tick(mark[1], 5, mark[2])
			tick(-mark[1], 5, mark[2])
		end
	end

	-- the fastest safe descent: v^2 = 2 (a - g) h with the up thrusters
	local gravity = Gravity.GetGravityAtBody(player)
	local accel = player:GetAcceleration("up")
	local safeDescent = gravity and accel > gravity and math.sqrt(2 * (accel - gravity) * altitude)
	if safeDescent and (verticalSpeed < 0 or landing) then
		local p = Vector2(x, yAt(-safeDescent))
		local d = 5
		diamond(p, d, colors.alertYellow, true)
		if (ui.getMousePos() - p):length() < d * 2 then
			ui.setTooltip(lui.HUD_VSI_SAFE_DESCENT)
		end
	end

	-- the pointer: red when descending faster than is safe, and on the
	-- landing scale the color of the band it is in
	local pointerColor = color
	if safeDescent and verticalSpeed < -safeDescent then
		pointerColor = colors.alertRed
	elseif landing and verticalSpeed < 0 then
		for _, band in ipairs(vsiLandingBands) do
			pointerColor = colors[band[2]]
			if -verticalSpeed <= band[1] then break end
		end
	end
	local y = yAt(verticalSpeed)
	ui.addTriangleFilled(Vector2(x + 2, y), Vector2(x + 10, y - 5), Vector2(x + 10, y + 5), pointerColor)
	ui.addStyledText(Vector2(x + 14, y), ui.anchor.left, ui.anchor.center, formatVerticalSpeed(verticalSpeed),
		pointerColor, pionillium.small, lui.HUD_VERTICAL_SPEED, colors.lightBlackBackground)

	ui.addStyledText(Vector2(x + 4, center.y + half + 4), ui.anchor.left, ui.anchor.top, lc.UNIT_METERS_PER_SECOND, color, pionillium.small, "")
end

-- whether the ladder and tapes showed last time, for the ceiling's hysteresis
local flightHudShown = false

-- the pitch ladder and the tapes only make sense flying over a planet or moon,
-- not when the frame is a star, a station or empty space, nor high above it
local function isFlightHudShown(frame)
	if frame == nil or not (frame.superType == "ROCKY_PLANET" or frame.superType == "GAS_GIANT") then
		flightHudShown = false
		return false
	end
	local radius = frame.path:GetSystemBody().radius
	local ceiling = (flightHudShown and hudCeiling or hudFloor) * radius
	flightHudShown = player:GetAltitudeRelTo(frame) < ceiling
	return flightHudShown
end

-- display the speed tape (left, in the game's speed units) and the altitude
-- tape (right, in meters, then kilometers from 10 km) relative to the frame,
-- with the altitude trend line and the vertical speed indicator beside it
local function displaySpeedAltitudeTapes(frame)
	local velocity = player:GetVelocityRelTo(frame)
	local speed = velocity:length()
	local speedText, speedUnit = ui.Format.SpeedUnit(speed)
	local speedScale = speed < 1e3 and 1 or speed < 1e6 and 1e3 or 1e6
	displayTape(-1, speed, speedScale, speedScale == 1 and 1 or 0.01, tostring(speedText), speedUnit)

	local altitude = player:GetAltitudeRelTo(frame)
	local pixelsPerMeter
	if altitude < 1e4 then
		pixelsPerMeter = displayTape(1, altitude, 1, 1, groupThousands(string.format("%.0f", altitude)), lc.UNIT_METERS)
	else
		local altitudeText = groupThousands(string.format(altitude < 1e6 and "%.1f" or "%.0f", altitude / 1e3))
		pixelsPerMeter = displayTape(1, altitude, 1e3, 0.1, altitudeText, lc.UNIT_KILOMETERS)
	end

	-- positive climbing, negative descending
	local verticalSpeed = velocity:dot(player:GetPositionRelTo(frame):normalized())
	displayAltitudeTrend(verticalSpeed * vsiTrendSeconds * pixelsPerMeter)

	-- clear of the altitude tape's value box, sized for its widest value
	local boxWidth = ui.calcTextSize("00,000.0", pionillium.body).x + 16
	displayVerticalSpeed(verticalSpeed, altitude, center.x + sideArcRadius * tapeOffset + boxWidth + 12)
end

-- display the heading tape across the top of the view: a tick every 5 degrees,
-- taller every 15, the compass points as letters, and a caret with the heading
local function displayHeadingTape(heading)
	-- labelled points on the tape
	local directions = {
		[0] = lc.COMPASS_N, [45] = lc.COMPASS_NE, [90] = lc.COMPASS_E, [135] = lc.COMPASS_SE,
		[180] = lc.COMPASS_S, [225] = lc.COMPASS_SW, [270] = lc.COMPASS_W, [315] = lc.COMPASS_NW
	}
	local width = sideArcRadius * 0.9
	local tape = Vector2(center.x, center.y - sideArcRadius * 0.7)

	local first = math.ceil((heading - headingTapeSpan / 2) / 5) * 5
	for d = first, heading + headingTapeSpan / 2, 5 do
		local x = tape.x + (d - heading) / headingTapeSpan * width
		local label = directions[d % 360]
		if label then
			ui.addStyledText(Vector2(x, tape.y), ui.anchor.center, ui.anchor.bottom, label, colors.navigationalElements, pionillium.tiny, "")
		else
			local height = d % 15 == 0 and 8 or 4
			ui.addLine(Vector2(x, tape.y), Vector2(x, tape.y - height), colors.reticuleCircle, 1)
		end
	end

	-- caret under the tape where the ship points
	ui.addLine(Vector2(tape.x - 5, tape.y + 9), Vector2(tape.x, tape.y + 4), colors.reticuleCircle, 1)
	ui.addLine(Vector2(tape.x, tape.y + 4), Vector2(tape.x + 5, tape.y + 9), colors.reticuleCircle, 1)
	if showNavigationalNumbers then
		ui.addStyledText(Vector2(tape.x, tape.y + 11), ui.anchor.center, ui.anchor.top, math.floor(heading + 0.5) .. "°", colors.reticuleCircle, pionillium.small, lui.HUD_CURRENT_HEADING)
	end
end

-- display the delta-v gauges along the left arc
local function displayReticuleDeltaV()
	-- ratio is 1.0 for full, 0.0 for empty
	local function gauge(ratio, radius, color, thickness)
		if ratio < 0 then
			ratio = 0
		end
		-- clamp to reduce flickering
		if ratio > 0 and ratio < 0.01 then
			ratio = 0.01
		end
		if ratio > 1 then
			ratio = 1
		end
		local bottom = ui.pi - sideArcHalfAngle
		ui.pathArcTo(center, radius + thickness / 2, bottom, bottom + 2 * sideArcHalfAngle * ratio, 64)
		ui.pathStroke(color, false, thickness)
	end

	local offset = gaugeOffset
	local thickness = gaugeThickness

	local deltav_max = player:GetMaxDeltaV()
	local deltav_remaining = player:GetRemainingDeltaV()
	local dvr = deltav_remaining / deltav_max
	local deltav_maneuver = player:GetManeuverVelocity():length()
	local dvm = deltav_maneuver / deltav_max
	local deltav_current = player:GetCurrentDeltaV()
	local dvc = deltav_current / deltav_max

	-- draw full gauge in background color
	gauge(1.0, sideArcRadius + offset, colors.deltaVTotal, thickness)
	-- draw remaining from bottom
	if dvr > 0 then
	  gauge(dvr, sideArcRadius + offset, colors.deltaVRemaining, thickness)
	end
	-- draw maneuver thinner inside remaining
	if dvm > 0 then
	  gauge(dvm, sideArcRadius + offset + thickness / 4, colors.deltaVManeuver, thickness / 2)
	end
	-- draw current beside the others
	if dvc > 0 then
	  gauge(dvc, sideArcRadius + offset + thickness + gaugeGap, colors.deltaVCurrent, thickness)
	end

	-- the readouts sit just past the arc's ends: delta-v below, percentage above
	ui.withFont(pionillium.reticle_sm, function()
		local uiPos = ui.pointOnClock(center, sideArcRadius + thickness * 2 + gaugeGap + ui.getTextLineHeight(), 9 - sideArcHours - 0.1)

		local iconSize = pionillium.reticle_md.size + ui.theme.styles.FramePadding.y * 2

		ui.setCursorPos(uiPos - Vector2(iconSize, ui.getTextLineHeight() * 0.5))
		textShadowedRight(ui.Format.Speed(deltav_remaining))

		ui.withFont(pionicons.medium.name, iconSize, function()
			ui.addTextShadowed(uiPos - Vector2(iconSize, iconSize * 0.5), colors.font, ui.get_icon_glyph(icons.deltav))
			ui.setItemTooltip(lui.HUD_DELTA_V)
		end)

		uiPos = ui.pointOnClock(center, sideArcRadius + thickness * 2 + gaugeGap, 9 + sideArcHours + 0.1)
		ui.setCursorPos(uiPos - Vector2(ui.getTextLineHeight() * 0.5))

		textShadowedRight(ui.Format.Number(dvr * 100, 2) .. "%")
		ui.setItemTooltip(lui.HUD_DELTA_V_PERCENT)
	end)
end

-- if the ratio of current distance to brake distance is greater than this,
-- display the gauge in green (or whatever the theme's colour is)
-- to tell the user to flip and start braking now
local brakeNowRatio = 0.93

-- display the velocity vs. braking distance gauges along the right arc
local function displayReticuleBrakeGauge(ratio_primary, ratio_secondary)
	local function gauge(ratio, radius, color, thickness)
		if ratio < 0 then
			ratio = 0
		end
		-- clamp to reduce flickering
		if ratio > 0 and ratio < 0.01 then
			ratio = 0.01
		end
		if ratio > 1 then
			ratio = 1
		end
		ui.pathArcTo(center, radius + thickness / 2, sideArcHalfAngle, -sideArcHalfAngle + 2 * sideArcHalfAngle * (1 - ratio), 64)
		ui.pathStroke(color, false, thickness)
	end
	local thickness = gaugeThickness
	local offset = gaugeOffset

	if ratio_primary <= 1 then
		gauge(1, sideArcRadius + offset, colors.brakeBackground, thickness)
		local color
		if ratio_primary > brakeNowRatio then
			color = colors.brakeNow
		else
			color = colors.brakePrimary
		end
		gauge(ratio_secondary, sideArcRadius + offset, colors.brakeSecondary, thickness)
		gauge(ratio_primary, sideArcRadius + offset, color, thickness)
	else
		gauge(1, sideArcRadius + offset, colors.brakeOvershoot, thickness)
		gauge(2 - math.min(ratio_primary, 2), sideArcRadius + offset, colors.brakePrimary, thickness)
	end
end

-- display heading (on the tape), pitch (on the right arc), roll (in the middle)
-- and the pitch ladder
local function displayReticulePitchHorizonCompass()
	local heading, pitch, roll = Game.player:GetHeadingPitchRoll("planet")
	local pitch_degrees = (pitch / ui.twoPi * 360)
	local heading_degrees = (heading / ui.twoPi * 360)
	local roll_degrees = (roll / ui.twoPi * 360);

	if showNavigationalNumbers then
		-- just past the bottom of the right arc
		local uiPos = ui.pointOnClock(center, sideArcRadius + 5, 3 + sideArcHours + 0.05)
		ui.addStyledText(uiPos, ui.anchor.left, ui.anchor.top, math.floor(pitch_degrees + 0.5) .. "°", colors.reticuleCircle, pionillium.small, lui.HUD_CURRENT_PITCH)

		uiPos = ui.pointOnClock(center, reticuleCircleRadius + 5, 6)
		ui.addStyledText(uiPos, ui.anchor.center, ui.anchor.top, math.floor(roll_degrees + 0.5) .. "°", colors.reticuleCircle, pionillium.small, lui.HUD_CURRENT_ROLL)
	end

	displayReticulePitch(pitch_degrees)
	if isFlightHudShown(player.frameBody) then
		displayPitchLadder(heading_degrees, pitch, roll)
	end
	displayHeadingTape(heading_degrees)
end

local function displayReticuleLandingAssist()
	local target = Game.player:GetNavTarget()

	if not target or not target:IsStation() then return end

	local displayLandingAsst = Game.player:GetFlightState() == "FLYING"
		and target:GetAssignedBayNumber(Game.player) >= 0

	if displayLandingAsst then
		-- position and { heading, pitch, roll } deviation from the target
		local _, dev = target:GetAssignedBayNavError(Game.player)
		assert(dev)

		local roll = dev.z

		-- Render a line inside the reticle indicating the plane of the landing pad relative to the ship's orientation.
		-- Use the line-circle intersection quadratic algorithm to compute the endpoints of the line segment
		local line_origin = Vector2(0, reticuleCircleRadius * math.sin(dev.y)):rotate(roll)
		local center_sqr = line_origin:lengthSqr()
		local orient = Vector2(math.cos(roll), math.sin(roll))

		local b = orient:dot(-line_origin)
		-- because the origin point of the line is always inside the circle, the determinant
		-- (b^2 - c^2 + r^2) will always be >= 0
		local d = math.sqrt(b * b - center_sqr + (reticuleCircleRadius - 2)^2)

		local proj_center = center + line_origin + orient * b
		local extent = orient * d

		local pos_a = proj_center + extent
		local pos_b = proj_center - extent

		ui.addLine(pos_a, pos_b, colors.landingAsstHorizon, 1)

		-- Render the yaw error tick below the line.
		-- This tick does not indicate direction to the pad, but rather the ship's
		-- rotation error relative to the pad's orientation.
		local heading_line_pos = proj_center - extent * (dev.x / ui.pi)
		local heading_line_dir = orient:left()

		local yaw_aligned = math.abs(dev.x) < math.deg2rad(10)
		local yaw_col = yaw_aligned and colors.landingAsstHorizon or colors.landingAsstYawTick
		ui.addLine(heading_line_pos, heading_line_pos + heading_line_dir * 6, yaw_col, yaw_aligned and 2 or 1)

		-- Render downwards half of artificial horizon indicator with a slight tint
		-- to indicate the ground.
		if target:IsGroundStation() then

			local theta_a = (pos_a - center):angle()
			local theta_b = (pos_b - center):angle()

			if theta_b < theta_a then theta_b = theta_b + math.pi * 2 end

			ui.pathArcTo(center, reticuleCircleRadius - 1, theta_a, theta_b, 32)
			ui.pathFillConvex(colors.landingAsstGround)

		end

	end
end

local function makeTargetData(target, inv_approach)
	local velocity = player:GetVelocityRelTo(target)
	local position = player:GetPositionRelTo(target)

	local approach_speed = math.trunc(position:normalized():dot(inv_approach and velocity or -velocity))

	return {
		ship_speed = velocity:length(),
		-- positive values indicate closure unless inv_approach is true
		-- flush negative zero to positive zero
		approach_speed = approach_speed == -0.0 and 0 or approach_speed,
		brake_distance = player:GetDistanceToZeroV(velocity:length(), "forward"),
		brake_distance_retro = player:GetDistanceToZeroV(velocity:length(), "reverse"),
		distance = player:GetAltitudeRelTo(target)
	}
end

local function displayFrameData(frame, radius, tooltip)

	local is_station = frame:isa("SpaceStation")

	local data = makeTargetData(frame, not is_station)
	local uiPos = ui.pointOnClock(center, radius, 9)

	ui.setCursorPos(uiPos - Vector2(0, pionillium.heading.size * 3))

	-- NOTE: because this is a right-to-left group, hover tests etc. will not work on it!
	ui.group(function()
		ui.withFont(pionillium.heading, function()
			textShadowedRight(frame.label)
		end)

		if ui.isItemClicked(1) then
			ui.openDefaultRadialMenu("game", frame, ui.getMousePos())
		end

		ui.setItemTooltip(tooltip)

		frameDetailStyle:withStyle(function()
			ui.withFont(pionillium.reticle_md, function()

				textShadowedRight(ui.Format.Speed(data.ship_speed) .. " " .. ui.get_icon_glyph(icons.nav_prograde))
				ui.setItemTooltip(lui.HUD_SPEED_RELATIVE_TO_TARGET)

				textShadowedRight(ui.Format.Speed(data.approach_speed) .. " " .. ui.get_icon_glyph(icons.nav_normal))
				ui.setItemTooltip(lui.HUD_SPEED_OF_APPROACH_TO_FRAME)

				ui.spacing()
				local dist_icon = is_station and icons.nav_radial_out or icons.altitude

				textShadowedRight(ui.Format.Distance(data.distance) .. " ".. ui.get_icon_glyph(dist_icon))
				ui.setItemTooltip(lui.HUD_DISTANCE_TO_SURFACE_OF_TARGET)

				textShadowedRight("~" .. ui.Format.Distance(data.brake_distance) .. " " .. ui.get_icon_glyph(icons.nav_radial_in))
				ui.setItemTooltip(lui.HUD_BRAKE_DISTANCE_MAIN_THRUSTERS)

			end)
		end)
	end)

end

local function displayFrameLabel(label, radius, tooltip)
	local uiPos = ui.pointOnClock(center, radius, 9)

	ui.withFont(pionillium.heading, function()
		local sz = ui.calcTextSize(label)

		ui.setCursorPos(uiPos - Vector2(sz.x, sz.y * 0.5))
		ui.textShadowed(label)
	end)

	ui.setItemTooltip(tooltip)
end

local function displayTargetSwitcher(targets)
	local size = Vector2(22)
	local padding = Vector2(0)

	frameSwitchStyle:withStyle(function()
		if targets.navTarget and targets.combatTarget then
			ui.addCursorPos(Vector2(-ui.getTextLineHeight() * 0.25, 0))

			ui.withStyleColors({ Text = reticuleTarget == "navTarget" and colors.font or colors.reticuleCircleDark }, function()
				if ui.iconButton("navTarget", icons.navtarget, lui.HUD_SHOW_NAV_TARGET, ui.theme.buttonColors.transparent, size, padding) then
					reticuleTarget = "navTarget"
				end
			end)

			ui.sameLine(0, 0)

			ui.withStyleColors({ Text = reticuleTarget == "combatTarget" and colors.font or colors.reticuleCircleDark }, function()
				if ui.iconButton("combatTarget", icons.combattarget, lui.HUD_SHOW_COMBAT_TARGET, ui.theme.buttonColors.transparent, size, padding) then
					reticuleTarget = "combatTarget"
				end
			end)
		end
	end)
end

local function displayDetailData(target, data, radius, tooltip)
	local targets = {
		combatTarget = player:GetCombatTarget(),
		navTarget = player:GetNavTarget(),
	}

	local is_planet = not (target:isa("Ship") or target:isa("SpaceStation"))

	local uiPos = ui.pointOnClock(center, radius, 3)
	ui.setCursorPos(uiPos - Vector2(0, pionillium.heading.size * 3))

	ui.group(function()
		ui.withFont(pionillium.heading, function()
			local color = target == targets.combatTarget and colors.combatTarget
				or target == targets.navTarget and colors.navTarget
				or colors.font

			ui.withStyleColors({ Text = color }, function()
				ui.textShadowed(target.label)
			end)
		end)

		if ui.isItemClicked(1) or bindings.targetRadial.action:IsJustActive() then
			local action_binding = bindings.targetRadial.action:IsActive() and bindings.targetRadial.action
			ui.openDefaultRadialMenu("game", target, ui.getMousePos(), action_binding)
		end

		ui.setItemTooltip(tooltip)

		frameDetailStyle:withStyle(function()

			ui.withFont(pionillium.reticle_md, function()
				ui.textShadowed(ui.get_icon_glyph(icons.nav_prograde) .. " " .. ui.Format.Speed(data.ship_speed))
				ui.setItemTooltip(lui.HUD_SPEED_RELATIVE_TO_TARGET)

				ui.textShadowed(ui.get_icon_glyph(icons.nav_antinormal) .. " " .. ui.Format.Speed(data.approach_speed))
				ui.setItemTooltip(lui.HUD_SPEED_OF_APPROACH_TO_FRAME)

				ui.spacing()

				local dist_icon = is_planet and icons.altitude or icons.nav_radial_out

				ui.textShadowed(ui.get_icon_glyph(dist_icon) .. " " .. ui.Format.Distance(data.distance))
				ui.setItemTooltip(lui.HUD_DISTANCE_TO_SURFACE_OF_TARGET)

				ui.textShadowed(ui.get_icon_glyph(icons.nav_radial_in) .. " ~" .. ui.Format.Distance(data.brake_distance))
				ui.setItemTooltip(lui.HUD_BRAKE_DISTANCE_MAIN_THRUSTERS)
			end)

		end)

		ui.spacing()

		displayTargetSwitcher(targets)

	end)

end

-- display current maneuver data below the reticule circle
-- the maneuver readout always targets the active burn (the earliest burn that
-- has not been completed): GetManeuverVelocity/GetManeuverTime with no index
-- return that burn's Δv and start time, so the burn-duration estimate below is
-- that single burn's, never a sum.
local function displayManeuverData(radius)
	local burnCount = player:GetBurnCount()
	-- -1 = no plan, or every planned burn is complete: no burn to aim at
	local activeBurn = player:GetActiveBurnIndex()
	local maneuverVelocity = player:GetManeuverVelocity()
	local maneuverSpeed = maneuverVelocity:length()
	local showManeuver = maneuverSpeed > 0 and not (player:IsDocked() or player:IsLanded())
	if showManeuver then
		-- while Auto Orientation holds a direction lock, project along the
		-- locked direction (scaled by the live residual) so the indicator stays
		-- where the ship is pointed; otherwise follow the live residual. The
		-- live magnitude still drives visibility, text and burn duration.
		local projectedVelocity = maneuverVelocity
		local lockedDirection = player:GetLockedManeuverDirection()
		if lockedDirection:lengthSqr() > 0 then
			projectedVelocity = lockedDirection * maneuverSpeed
		end
		local onscreen,position,direction = Engine.ProjectRelDirection(projectedVelocity)
		gameView.displayIndicator(onscreen, position, direction, icons.bullseye, colors.maneuver, true, lui.HUD_INDICATOR_MANEUVER_PROGRADE)
		local uiPos = ui.pointOnClock(center, radius, 6)
		local speed, speed_unit = ui.Format.SpeedUnit(maneuverSpeed)
		-- the burn is split 50:50 around the planned node time: it starts half
		-- its duration before the node. Use the planned (not remaining) Δv for
		-- that, so the start does not drift as the burn progresses.
		local acceleration = player:GetAcceleration("forward")
		local burn_duration = maneuverSpeed / acceleration
		local plannedDv = Game.systemView and Game.systemView:TransferPlannerGetBurnDeltaV(activeBurn) or maneuverSpeed
		local halfBurn = (acceleration > 0) and (plannedDv / acceleration / 2) or 0
		local untilNode = player:GetManeuverTime() - Game.time
		local untilStart = untilNode - halfBurn
		local startText = untilStart > 0 and ui.Format.Duration(untilStart) or lc.NOW
		local duration = ui.Format.Duration(math.max(untilNode, 0))
		local burn_time = ui.Format.Duration(burn_duration)
		ui.addFancyText(uiPos, ui.anchor.center, ui.anchor.top, {
			{ text=startText,          color=colors.maneuver,     font=pionillium.medium, tooltip=lui.HUD_DURATION_UNTIL_MANEUVER_BURN },
			{ text=" / " .. duration,  color=colors.maneuverDark, font=pionillium.small,  tooltip=lui.HUD_DURATION_UNTIL_MANEUVER_BURN },
			{ text="  " .. speed,      color=colors.maneuver,     font=pionillium.medium, tooltip=lui.HUD_DELTA_V_OF_MANEUVER_BURN },
			{ text=speed_unit,         color=colors.maneuverDark, font=pionillium.small,  tooltip=lui.HUD_DELTA_V_OF_MANEUVER_BURN },
			{ text="  ~" .. burn_time, color=colors.maneuver,     font=pionillium.medium, tooltip=lui.HUD_DURATION_OF_MANEUVER_BURN }
		}, colors.lightBlackBackground)
	end
	-- burn status line: k/n where k is the active burn (GetActiveBurnIndex) - the
	-- same burn the maneuver readout above targets. Nothing is drawn when the
	-- index is -1: an empty plan and an all-completed plan both read as "no
	-- plan", which also keeps a "Burn 0/n" label from ever appearing.
	if activeBurn >= 0 then
		local uiPos = ui.pointOnClock(center, radius, 6)
		if showManeuver then
			-- stack below the maneuver readout when it is on screen
			uiPos.y = uiPos.y + ui.getTextLineHeight() + 2
		end
		ui.addStyledText(uiPos, ui.anchor.center, ui.anchor.top, string.format(lui.BURN_PROGRESS, activeBurn + 1, burnCount), colors.maneuver, pionillium.small, nil, colors.lightBlackBackground)
	end

	-- how far down the readout reaches, for what stacks below it
	local bottom = ui.pointOnClock(center, radius, 6).y
	if showManeuver then
		bottom = bottom + pionillium.medium.size + 4
	end
	if activeBurn >= 0 then
		bottom = bottom + pionillium.small.size + 4
	end
	return bottom
end

local aicommand_info = {
	CMD_DOCK = { icon = icons.autopilot_dock, tooltip = lui.HUD_BUTTON_AUTOPILOT_DOCKING },
	CMD_FLYTO = { icon = icons.autopilot_fly_to, tooltip = lui.HUD_BUTTON_AUTOPILOT_FLYING_TO_TARGET },
	CMD_FORMATION = { icon = icons.autopilot_fly_to, tooltip = lui.HUD_BUTTON_AUTOPILOT_FLYING_TO_TARGET },
	CMD_FLYAROUND = { icon = icons.autopilot_medium_orbit, tooltip = lui.HUD_BUTTON_AUTOPILOT_ENTERING_ORBIT },
}

-- the plan-heading control state (Phase 3) is labelled in the same local
-- table pattern as flight-ui/burn-hud.lua, from the ui-core lang resource. The engine
-- side of the planner is not a flight control state at all - Auto Burn is an
-- independent enable-flag (player:GetPlanBurnEnabled), so it has no entry here.
local L = {
	PLAN_HEADING = lui.HUD_BUTTON_PLAN_HEADING,
	UNKNOWN_STATE = lui.HUD_BUTTON_UNKNOWN_FLIGHT_STATE,
}

local flightstate_info = {
	CONTROL_MANUAL = { icon = icons.empty, tooltip = lui.HUD_BUTTON_MANUAL_CONTROL },
	-- "CONTROL_AUTOPILOT" - depends on the current command
	-- "CONTROL_FIXSPEED" - depends on the cruise mode
	CONTROL_FIXHEADING_FORWARD = { icon = icons.nav_prograde, tooltip = lui.HUD_BUTTON_FIX_PROGRADE },
	CONTROL_FIXHEADING_BACKWARD = { icon = icons.nav_retrograde , tooltip = lui.HUD_BUTTON_FIX_RETROGRADE },
	CONTROL_FIXHEADING_NORMAL = { icon = icons.nav_normal, tooltip = lui.HUD_BUTTON_FIX_NORMAL },
	CONTROL_FIXHEADING_ANTINORMAL = { icon = icons.nav_antinormal, tooltip = lui.HUD_BUTTON_FIX_ANTINORMAL },
	CONTROL_FIXHEADING_RADIALLY_INWARD = { icon = icons.nav_radial_in, tooltip = lui.HUD_BUTTON_FIX_RADIAL_IN },
	CONTROL_FIXHEADING_RADIALLY_OUTWARD = { icon = icons.nav_radial_out, tooltip = lui.HUD_BUTTON_FIX_RADIAL_OUT },
	-- "CONTROL_FIXHEADING_KILLROT" uses the same icon as rotation damping on
	CONTROL_FIXHEADING_KILLROT = { icon = icons.rotation_damping_on , tooltip = lui.HUD_BUTTON_KILL_ROTATION },
	-- the transfer-planner orientation state (flight-ui/burn-hud.lua's Auto
	-- Orientation toggle); the icon matches the panel's button icon
	CONTROL_PLAN_HEADING = { icon = icons.auto_orientation, tooltip = L.PLAN_HEADING }
}

-- Defensive lookup: a control-state string outside the table must never abort
-- the HUD draw — an unknown state falls back to the neutral entry. A raise
-- inside the draw disables the whole reticule module (gameView:draw's pcall),
-- which is how selecting Auto Orientation made the flight UI disappear.
local function flightStateInfo(state)
	return flightstate_info[state] or { icon = icons.empty, tooltip = L.UNKNOWN_STATE }
end

-- icons colored by marker type, KSP style
local radial_menu_actions_orbital = {
	{icon = ui.theme.icons.nav_normal,
	 color = colors.navMarkerNormal,
	 tooltip=lc.HEADING_LOCK_NORMAL,
	 action=function(_) Game.player:SetFlightControlState("CONTROL_FIXHEADING_NORMAL") end},
	{icon = ui.theme.icons.nav_radial_out,
	 color = colors.navMarkerRadial,
	 tooltip=lc.HEADING_LOCK_RADIALLY_OUTWARD,
	 action=function(_) Game.player:SetFlightControlState("CONTROL_FIXHEADING_RADIALLY_OUTWARD") end},
	{icon = ui.theme.icons.nav_retrograde,
	 color = colors.navMarkerPrograde,
	 tooltip=lc.HEADING_LOCK_BACKWARD,
	 action=function(_) Game.player:SetFlightControlState("CONTROL_FIXHEADING_BACKWARD") end},
	{icon = ui.theme.icons.nav_antinormal,
	 color = colors.navMarkerNormal,
	 tooltip=lc.HEADING_LOCK_ANTINORMAL,
	 action=function(_) Game.player:SetFlightControlState("CONTROL_FIXHEADING_ANTINORMAL") end},
	{icon = ui.theme.icons.nav_radial_in,
	 color = colors.navMarkerRadial,
	 tooltip=lc.HEADING_LOCK_RADIALLY_INWARD,
	 action=function(_) Game.player:SetFlightControlState("CONTROL_FIXHEADING_RADIALLY_INWARD") end},
	{icon = ui.theme.icons.nav_prograde,
	 color = colors.navMarkerPrograde,
	 tooltip=lc.HEADING_LOCK_FORWARD,
	 action=function(_) Game.player:SetFlightControlState("CONTROL_FIXHEADING_FORWARD") end},
}

local color_active = colors.white
local color_disable = colors.white:opacity(0.3)
local color_inactive = colors.lightBlackBackground

local flight_assist_buttons = {

	cruise_forward = {
		icon = icons.circ_cruise_fwd,
		tooltip = lui.HUD_BUTTON_CRUISE_FORWARD,
		color = color_active,
		action = function(_)
			Game.player:SetCruiseDirection("CRUISE_FWD")
			Game.player:SetFlightControlState("CONTROL_FIXSPEED")
		end
	},

	cruise_up = {
		icon = icons.circ_cruise_up,
		tooltip = lui.HUD_BUTTON_CRUISE_UP,
		color = color_active,
		action = function(_)
			Game.player:SetCruiseDirection("CRUISE_UP")
			Game.player:SetFlightControlState("CONTROL_FIXSPEED")
		end
	},

	disable_cruise = {
		icon = icons.circ_clear_flwtarget,
		tooltip = lui.HUD_BUTTON_DISABLE_CRUISE,
		color = color_disable,
		action = function(_) Game.player:SetFlightControlState("CONTROL_MANUAL") end
	},

	disable_follow = {
		icon = icons.circ_clear_flwtarget,
		tooltip = lui.HUD_BUTTON_DISABLE_FOLLOW_MODE,
		color = color_disable,
		action = function(_) Game.player:SetFollowTarget(nil) end
	}

}

-- pos is the center of the flight assist button
local function flightAssistButton(pos)
	local icon_size = flightAssistIconSize
	local icon_padding = flightAssistButtonPadding
	local button_size = flightAssistButtonSize
	local tooltip = ""
	local back_color = colors.lightBlueBackground:opacity(0.3)
	-- info:
	local flightstate = player:GetFlightState()
	local flightcontrolstate = player:GetFlightControlState()
	local followtarget = player:GetFollowTarget()
	local navtarget = player:GetNavTarget()
	local cmbtarget = player:GetCombatTarget()
	local frmtarget = player.frameBody
	local followmode = player:GetFollowMode()
	local cruisedir = player:GetCruiseDirection()

	local main_icon = nil -- must be overriden
	if flightcontrolstate == "CONTROL_AUTOPILOT" then
		local cmd = player:GetCurrentAICommand()
		local ci = aicommand_info[cmd]
		main_icon = icons.backward
		tooltip = "UNKNOWN AI COMMAND?"
		if ci then
			main_icon = ci.icon
			tooltip = ci.tooltip
		end
	elseif flightcontrolstate == "CONTROL_FIXSPEED" then
		main_icon = cruisedir == "CRUISE_FWD" and icons.cruise_fwd or icons.cruise_up
		tooltip = cruisedir == "CRUISE_FWD" and lui.HUD_BUTTON_CRUISE_FORWARD or lui.HUD_BUTTON_CRUISE_UP
	else
		local state_info = flightStateInfo(flightcontrolstate)
		main_icon = state_info.icon
		tooltip = state_info.tooltip
	end

	-- background
	ui.addIcon(pos, icons.follow_fill, back_color, Vector2(button_size, button_size), ui.anchor.center, ui.anchor.center, tooltip)
	ui.addIcon(pos, icons.follow_edge, back_color, Vector2(button_size, button_size), ui.anchor.center, ui.anchor.center, tooltip)
	-- edge icon
	if followtarget then
		local follow_icon = followmode == "FOLLOW_POS" and icons.follow_pos or icons.follow_ori
		ui.addIcon(pos, follow_icon, colors.white, Vector2(button_size, button_size), ui.anchor.center, ui.anchor.center, tooltip)
	end

	-- main icon
	ui.addIcon(pos, main_icon, colors.white, Vector2(icon_size, icon_size), ui.anchor.center, ui.anchor.center, tooltip)

	-- radial menu
	if ui.isMouseClicked(0) and ui.isWindowHovered() and (ui.getMousePos() - pos):length() < button_size / 2.0
		or bindings.assistRadial.action:IsJustActive() then

		local icon_left = { icon = icons.backward, tooltip = "NO_ACTION", action = function(_) end, color = color_inactive }
		local icon_right = { icon = icons.backward, tooltip = "NO_ACTION", action = function(_) end, color = color_inactive }
		local icon_up = { icon = icons.backward, tooltip = "NO_ACTION", action = function(_) end, color = color_inactive }
		local icon_down = { icon = icons.backward, tooltip = "NO_ACTION", action = function(_) end, color = color_inactive }

		-- up/down icons
		if flightcontrolstate == "CONTROL_MANUAL" or flightcontrolstate == "CONTROL_FIXSPEED" then
			icon_up = flight_assist_buttons.cruise_forward
			icon_down = flight_assist_buttons.cruise_up
			if flightcontrolstate == "CONTROL_FIXSPEED" then
				if cruisedir == "CRUISE_FWD" then
					icon_up = flight_assist_buttons.disable_cruise
				else
					icon_down = flight_assist_buttons.disable_cruise
				end
			end
		else
			local forward = cruisedir == "CRUISE_FWD"
			local fix_icon = forward and icon_up or icon_down
			local man_icon = forward and icon_down or icon_up
			fix_icon.icon = forward and icons.circ_cruise_fwd or icons.circ_cruise_up
			fix_icon.tooltip = lui.HUD_BUTTON_CRUISE_FORWARD
			fix_icon.color = color_active
			fix_icon.action = function(_) player:SetFlightControlState("CONTROL_FIXSPEED") end

			man_icon.icon = icons.circ_clear_flwtarget
			man_icon.tooltip = flightcontrolstate == "CONTROL_AUTOPILOT" and
				lui.HUD_BUTTON_DISABLE_AUTOPILOT or lui.HUD_BUTTON_DISABLE_FIXHEADING
			man_icon.color = color_disable
			man_icon.action = function(_) player:SetFlightControlState("CONTROL_MANUAL") end
		end

		-- side icons
		local settarget = followtarget

		if not followtarget and reticuleTarget then
			if     reticuleTarget == "frame"        then settarget = frmtarget
			elseif reticuleTarget == "navTarget"    then settarget = navtarget
			elseif reticuleTarget == "combatTarget" then settarget = cmbtarget
			end
		end

		if settarget then

			local settargetname = settarget:GetLabel()
			-- ori / pos does only works for dynamic bodies and orbitals
			local can_follow = not settarget.type or settarget.type == "STARPORT_ORBITAL"

			if can_follow then

				local is_follow_pos = followtarget and followmode == "FOLLOW_POS"
				local is_follow_ori = followtarget and followmode == "FOLLOW_ORI"
				local distance = player:DistanceTo(settarget)

				if is_follow_ori then
					icon_left = flight_assist_buttons.disable_follow
				elseif distance > player:GetMaxFollowDistance('FOLLOW_ORI') then
					icon_left.icon = icons.follow_ori
					icon_left.tooltip = string.interp(lui.HUD_FOLLOW_ORIENTATION_NOT_AVAILABLE_TOO_FAR, { targetname = settargetname })
				else
					icon_left.icon = icons.follow_ori
					icon_left.color = color_active
					icon_left.tooltip = string.interp(lui.HUD_FOLLOW_ORIENTATION, { targetname = settargetname })
					icon_left.action = function(_)
						if flightcontrolstate ~= "CONTROL_MANUAL" then
							player:SetFlightControlState("CONTROL_FIXSPEED")
						end
						player:SetFollowTarget(settarget)
						player:SetFollowMode("FOLLOW_ORI")
					end
				end

				if is_follow_pos then
					icon_right = flight_assist_buttons.disable_follow
				elseif distance > player:GetMaxFollowDistance('FOLLOW_POS') then
					icon_right.icon = icons.follow_pos
					icon_right.tooltip = string.interp(lui.HUD_FOLLOW_POSITION_NOT_AVAILABLE_TOO_FAR, { targetname = settargetname })
				else
					icon_right.icon = icons.follow_pos
					icon_right.color = color_active
					icon_right.tooltip = string.interp(lui.HUD_FOLLOW_POSITION, { targetname = settargetname })
					icon_right.action = function(_)
						player:SetFlightControlState("CONTROL_FIXSPEED")
						player:SetFollowTarget(settarget)
						player:SetFollowMode("FOLLOW_POS")
						-- switch to cruise mode automatically if follow position was enabled from manual mode
						if flightcontrolstate == "CONTROL_MANUAL" then
							player:SetCruiseDirection("CRUISE_FWD")
						end
					end
				end
			else
				icon_left.tooltip = string.interp( lui.HUD_FOLLOW_ORIENTATION_NOT_AVAILABLE, { targetname = settargetname })
				icon_right.tooltip = string.interp( lui.HUD_FOLLOW_POSITION_NOT_AVAILABLE, { targetname = settargetname })
			end
		end

		-- consider keyboard / joystick activation
		local action_binding = bindings.assistRadial.action:IsActive() and bindings.assistRadial.action

		-- create quad menu
		local my_quad_menu = { icon_right, icon_down, icon_left, icon_up }
		ui.openRadialMenu("game", nil, 0, button_size, my_quad_menu, 0, pos, action_binding)
	elseif ui.isMouseClicked(1) and ui.isWindowHovered() and (ui.getMousePos() - pos):length() < button_size / 2.0
		or bindings.fixheadingRadial.action:IsJustActive() then
		local frame = player.frameBody
		if frame then
			local action_binding = bindings.fixheadingRadial.action:IsActive() and bindings.fixheadingRadial.action
			ui.openRadialMenu("game", frame, 1, icon_size, radial_menu_actions_orbital, icon_padding, pos, action_binding)
		end
	end

	-- keyboard
	if (flightstate == "FLYING" and ui.noModifierHeld() and ui.isKeyReleased(ui.keys.f5)) then
		local newState = "CONTROL_MANUAL"
		if ui.ctrlHeld() and flightcontrolstate == "CONTROL_FIXSPEED" then
			newState = "CONTROL_FIXHEADING_FORWARD"
		elseif flightcontrolstate == "CONTROL_MANUAL" then
			newState = "CONTROL_FIXSPEED"
		end
		Game.player:SetFlightControlState(newState)
		ui.playBoinkNoise()
	end

	-- mouse wheel
	local wheel = ui.getMouseWheel()
	if wheel ~= 0 and (ui.getMousePos() - pos):length() < button_size / 2.0 then
		local delta = wheel
		if ui.shiftHeld() then
			delta = delta * 10
		end
		Game.player:ChangeCruiseSpeed(delta)
	end
end

local function displayFlightAssist(radius)
	local follow_target = player:GetFollowTarget()
	local assist_button_radius = flightAssistButtonSize / 2
	-- just past the bottom of the right arc
	local uiPos = ui.pointOnClock(center, radius + assist_button_radius * 2, 3 + sideArcHours + 0.1)
	local padding = 3.0
	local speed_limit_size = assist_button_radius

	flightAssistButton(Vector2(uiPos.x, uiPos.y))

	local cruise_speed = player:GetCruiseSpeed()
	local speed_limit = player:GetSpeedLimit()

	if speed_limit then
		local cur_speed = player:GetVelocity():length()
		local hit_speed_limit = math.abs(speed_limit) - math.abs(cruise_speed or cur_speed) < 0.001

		ui.setCursorPos(uiPos - Vector2(assist_button_radius + speed_limit_size + padding, speed_limit_size * 0.5))

		ui.withStyleColors({ Text = hit_speed_limit and colors.reticuleCircle or colors.reticuleCircleDark }, function()
			ui.withFont(pionicons.large.name, speed_limit_size, function()
				ui.textShadowed(ui.get_icon_glyph(icons.speed_limiter))
			end)
		end)
		ui.setItemTooltip(lui.HUD_SPEED_LIMITER_ACTIVE)

	end

	uiPos.x = uiPos.x + assist_button_radius + padding + ui.getItemSpacing().x * 0.5

	local cruise_speed_lbl = cruise_speed and ui.Format.Speed(cruise_speed)

	ui.withFont(pionillium.reticle_md, function()
		if cruise_speed then

			local sz = ui.calcTextSize(cruise_speed_lbl, pionillium.body)
			local follow_sz = follow_target and ui.calcTextSize(follow_target.label)
			local spacing = ui.getItemSpacing().y

			if follow_target then

				ui.withStyleColors({ Text = colors.fontDim }, function()
					ui.setCursorPos(uiPos - Vector2(math.min(0, -0.5 * (sz.x - follow_sz.x)), follow_sz.y + spacing * 0.5))

					ui.textShadowed(follow_target.label)
					ui.setItemTooltip(lui.HUD_FOLLOW_TARGET)
				end)

			end

			ui.withFont(pionillium.body, function()
				ui.setCursorPos(uiPos + Vector2(0, follow_target and spacing * 0.5 or sz.y * -0.5))

				ui.textShadowed(cruise_speed_lbl)
				ui.setItemTooltip(lui.HUD_CRUISE_SPEED)
			end)


		elseif follow_target then

			ui.withStyleColors({ Text = colors.fontDim }, function()
				ui.setCursorPos(uiPos - Vector2(0, ui.calcTextSize(follow_target.label).y * 0.5))

				ui.textShadowed(follow_target.label)
				ui.setItemTooltip(lui.HUD_FOLLOW_TARGET)
			end)

		end
	end)
end

local function displayAlertMarker()
	local alert = player:GetAlertState()
	local iconsize = Vector2(24, 24)
	if alert then
		-- by the top of the right arc
		local uiPos = ui.pointOnClock(center, sideLabelRadius, 3 - sideArcHours)
		if alert == "ship-firing" then
			ui.addIcon(uiPos, icons.alert2, colors.alertRed, iconsize, ui.anchor.center, ui.anchor.center, lc.LASER_FIRE_DETECTED)
		elseif alert == "ship-nearby" then
			ui.addIcon(uiPos, icons.alert1, colors.alertYellow, iconsize, ui.anchor.center, ui.anchor.center, lc.SHIP_DETECTED_NEARBY)
		end
	end
end


-- how far out a screen offset from the center is, relative to the reticle frame (1 = on it)
local function frameDistance(offset)
	return math.sqrt((offset.x / reticuleFrame.x)^2 + (offset.y / reticuleFrame.y)^2)
end

-- the heading (deg) of a direction in the player's frame, found from the
-- ship's own heading and the angle between them across the horizon (up is
-- the local vertical); nil when either is too near the vertical to tell
local function headingOf(direction, up, shipHeading)
	local forward = Vector3(0, 0, -1)
	local upShip = Engine.WorldSpaceToShipSpace(up)
	local target = Engine.WorldSpaceToShipSpace(direction)
	local f = forward - upShip * forward:dot(upShip)
	local t = target - upShip * target:dot(upShip)
	if f:lengthSqr() < 1e-6 or t:lengthSqr() < 1e-6 then return nil end
	-- clockwise seen from above, as headings run
	return shipHeading + math.deg(math.atan2(-f:cross(t):dot(upShip), f:dot(t)))
end

-- display the ascent guidance director: an outlined diamond where the nose
-- should point, and a small filled one at the edge of the reticle frame when
-- that is outside it
local function displayAscentDirector(direction)
	local inShip = Engine.WorldSpaceToShipSpace(direction):normalized()
	local aligned = inShip:dot(Vector3(0, 0, -1)) > math.cos(math.rad(ascentAlignedAngle))
	local color = aligned and colors.ascentDirectorAligned or colors.ascentDirector

	local onscreen, position, screenDirection = Engine.ProjectRelDirection(direction)
	if onscreen then
		diamond(position, 10, color, false)
		ui.addCircleFilled(position, 2, color, 8)
		if (ui.getMousePos() - position):length() < 12 then
			ui.setTooltip(lui.ASCENT_DIRECTOR)
		end
	end
	if not onscreen or frameDistance(position - center) > 1 then
		local dir = Vector2(screenDirection.x, screenDirection.y)
		diamond(center + dir * (0.9 / frameDistance(dir)), 6, color, true)
	end
end

-- display the target pitch beside the pitch indicator on the right arc
local function displayAscentPitchBug(pitch_degrees)
	local top, bottom = 3 - sideArcHours, 3 + sideArcHours
	local hour = bottom - (pitch_degrees + 90) / 180 * (bottom - top)
	diamond(ui.pointOnClock(center, sideArcRadius - 12, hour), 4, colors.ascentDirector, true)
end

-- display the target heading under the heading tape, held at its ends when off it
local function displayAscentHeadingBug(heading_degrees, target_degrees)
	local width = sideArcRadius * 0.9
	local tape = Vector2(center.x, center.y - sideArcRadius * 0.7)
	local offset = (target_degrees - heading_degrees + 180) % 360 - 180
	local x = tape.x + math.max(-0.5, math.min(0.5, offset / headingTapeSpan)) * width
	diamond(Vector2(x, tape.y + 6), 4, colors.ascentDirector, true)
end

local ascentPhaseText = {
	[AscentGuidance.READY] = lui.ASCENT_READY,
	[AscentGuidance.LIFT] = lui.ASCENT_LIFT,
	[AscentGuidance.PITCH] = lui.ASCENT_PITCH,
	[AscentGuidance.HOLD] = lui.ASCENT_HOLD,
	[AscentGuidance.CUT] = lui.ASCENT_CUT,
	[AscentGuidance.COAST] = lui.ASCENT_COAST,
	[AscentGuidance.DONE] = lui.ASCENT_DONE,
}

-- 1240, 7800 -> "1.24 / 7.80 km/s", in the unit of the larger
local function formatSpeedPair(speed, of)
	local _, unit = ui.Format.SpeedUnit(math.max(speed, of))
	local scale = unit == lc.UNIT_METERS_PER_SECOND and 1 or unit == lc.UNIT_KILOMETERS_PER_SECOND and 1e3 or 1e6
	local format = scale == 1 and "%.0f / %.0f %s" or "%.2f / %.2f %s"
	return string.format(format, speed / scale, of / scale, unit)
end

-- display the ascent guidance panel from top down, under the maneuver readout:
-- the phase, apoapsis against the target with a progress bar and periapsis,
-- then the time to apoapsis and the orbital speed against the circular speed
local function displayAscentPanel(guidance, top)
	local pos = Vector2(center.x, top)
	local background = colors.lightBlackBackground
	local phase = guidance.phase

	local text = guidance.topUp and lui.ASCENT_TOP_UP or ascentPhaseText[phase]
	local color = colors.reticuleCircle
	if phase == AscentGuidance.READY and guidance.twr < 1.1 then
		text = string.format(lui.ASCENT_TWR_LOW, ui.Format.TWR(guidance.twr, 2))
		color = colors.alertRed
	elseif phase == AscentGuidance.CUT then
		color = math.fmod(ui.getTime(), 1) < 0.5 and colors.alertYellow or colors.alertRed
	elseif phase == AscentGuidance.DONE then
		color = colors.brakeNow
	end
	ui.addStyledText(pos, ui.anchor.center, ui.anchor.top, text, color, pionillium.medium, nil, background)
	pos.y = pos.y + pionillium.medium.size + 8

	local apText = guidance.ap and ui.Format.Distance(guidance.ap, "%0.1f") or "-"
	local size = ui.addFancyText(pos, ui.anchor.center, ui.anchor.top, {
		{ text = ui.get_icon_glyph(icons.apoapsis) .. " " .. apText, color = colors.reticuleCircle, font = pionillium.small, tooltip = lui.HUD_ORBIT_APOAPSIS },
		{ text = " / " .. ui.Format.Distance(guidance.target, "%0.0f"), color = colors.reticuleCircleDark, font = pionillium.small, tooltip = lui.ASCENT_TARGET_TIP_SHORT },
		{ text = "    " .. ui.get_icon_glyph(icons.periapsis) .. " " .. ui.Format.Distance(guidance.pe, "%0.1f"), color = colors.reticuleCircle, font = pionillium.small, tooltip = lui.HUD_ORBIT_PERIAPSIS },
	}, background)
	pos.y = pos.y + size.y + 4

	-- apoapsis on the way to the target
	local fraction = guidance.ap and math.max(0, math.min(1, guidance.ap / guidance.target)) or 1
	local barHalf = math.max(size.x / 2, 60)
	ui.addRectFilled(Vector2(pos.x - barHalf, pos.y), Vector2(pos.x + barHalf, pos.y + 3), colors.brakeBackground, 0, ui.RoundCornersNone)
	ui.addRectFilled(Vector2(pos.x - barHalf, pos.y), Vector2(pos.x - barHalf + 2 * barHalf * fraction, pos.y + 3), color, 0, ui.RoundCornersNone)
	pos.y = pos.y + 8

	local timeToAp = guidance.timeToAp and ui.Format.Duration(guidance.timeToAp) or "-"
	size = ui.addFancyText(pos, ui.anchor.center, ui.anchor.top, {
		{ text = ui.get_icon_glyph(icons.eta) .. " " .. timeToAp, color = colors.reticuleCircle, font = pionillium.small, tooltip = lui.ASCENT_TIME_TO_AP_TIP },
		{ text = "    " .. ui.get_icon_glyph(icons.orbit_prograde) .. " " .. formatSpeedPair(guidance.speed, guidance.circularSpeed), color = colors.reticuleCircle, font = pionillium.small, tooltip = lui.ASCENT_SPEED_TIP },
	}, background)
	pos.y = pos.y + size.y + 6

	if phase == AscentGuidance.COAST and AscentGuidance.CanCircularize(player) then
		ui.addStyledText(pos, ui.anchor.center, ui.anchor.top, lui.ASCENT_PLAN_HINT, colors.maneuver, pionillium.small, nil, background)
	end
end

-- display ascent guidance while it is on: the director with its pitch and
-- heading bugs while there is a direction to fly, and the panel at top
local function displayAscentGuidance(top)
	local guidance = AscentGuidance.Update(player)
	if not guidance then return end

	if guidance.direction then
		displayAscentDirector(guidance.direction)
		displayAscentPitchBug(math.deg(guidance.pitch))
		if guidance.phase ~= AscentGuidance.LIFT and player.frameBody then
			local heading = math.deg(player:GetHeadingPitchRoll("planet"))
			local up = (-player.frameBody:GetPositionRelTo(player)):normalized()
			local target = headingOf(guidance.direction, up, heading)
			if target then
				displayAscentHeadingBug(heading, target)
			end
		end
	end

	displayAscentPanel(guidance, top)
end

local function displayReticule()
	-- the reticle's two halves, apart either side of the view
	ui.pathArcTo(center, sideArcRadius, ui.pi - sideArcHalfAngle, ui.pi + sideArcHalfAngle, 32)
	ui.pathStroke(colors.reticuleCircle, false, reticuleCircleThickness)
	ui.pathArcTo(center, sideArcRadius, -sideArcHalfAngle, sideArcHalfAngle, 32)
	ui.pathStroke(colors.reticuleCircle, false, reticuleCircleThickness)

	local frame = player.frameBody
	local frameLabel = player.frameLabel
	local navTarget = player:GetNavTarget()
	local combatTarget = player:GetCombatTarget()
	local radius = sideLabelRadius

	if reticuleTarget == "frame" and (navTarget or combatTarget) then
		reticuleTarget = (combatTarget and "combatTarget") or (navTarget and "navTarget") --[[@as string]]
	end

	if frame then
		displayFrameData(frame, radius, lui.HUD_CURRENT_FRAME)
		if isFlightHudShown(frame) then
			displaySpeedAltitudeTapes(frame)
		end
	else
		displayFrameLabel(frameLabel, radius, lui.HUD_CURRENT_FRAME)
	end

	local targetData = nil

	if reticuleTarget == "navTarget" and navTarget then
		targetData = makeTargetData(navTarget)
		displayDetailData(navTarget, targetData, radius, lui.HUD_CURRENT_NAV_TARGET)
	elseif reticuleTarget == "combatTarget" and combatTarget then
		targetData = makeTargetData(combatTarget)
		displayDetailData(combatTarget, targetData, radius, lui.HUD_CURRENT_COMBAT_TARGET)
	end

	if not targetData and frame then
		targetData = makeTargetData(frame)
	end

	-- current speed of approach
	if targetData and targetData.approach_speed > 0 and player:GetFlightState() == "FLYING" then
		displayReticuleBrakeGauge(
			targetData.brake_distance / targetData.distance,
			targetData.brake_distance_retro / targetData.distance)
	end

	displayFlightAssist(radius)
	-- the maneuver readout stays under the middle of the view, and the
	-- ascent guidance panel below it
	local maneuverBottom = displayManeuverData(reticuleCircleRadius * 1.2)
	displayAscentGuidance(maneuverBottom + 4)
	displayReticulePitchHorizonCompass()
	displayReticuleLandingAssist()
	displayReticuleDeltaV()
	displayAlertMarker()
end

gameView.registerModule("reticule", {
	showInHyperspace = false,
	draw = function(_)
		center = gameView.center
		colors = ui.theme.colors
		displayReticule()
	end,
	debugReload = function()
		package.reimport()
	end
})

Event.Register("onGameStart", function()
	player = Game.player
end)

ui.Events.Register("onPlayerTargetChanged", function(target)
	if target then
		reticuleTarget =
			(target == player:GetCombatTarget() and "combatTarget") or
			(target == player:GetNavTarget() and "navTarget") or
			"frame"
	else
		reticuleTarget =
			(player:GetCombatTarget() and "combatTarget") or
			(player:GetNavTarget() and "navTarget") or
			"frame"
	end
end)
