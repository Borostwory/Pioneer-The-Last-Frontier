-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Ascent guidance: the climb from a planet's surface to a circular orbit at a
-- target altitude the player sets before lift-off. It only advises - where to
-- point the nose (the flight director), when to cut the engines and when to
-- circularize - and the player flies it.
--
-- Phases: READY (landed or docked, waiting for lift-off), LIFT (straight up
-- until clear of the ground), PITCH (the gravity turn, following the
-- director, until apoapsis reaches the target), then one of two ways on:
-- HOLD (keep burning along the horizon with apoapsis held at the target,
-- while the burn that would circularize at apoapsis is too long to fit
-- before it - low thrust) or COAST (engines off up to apoapsis, where the
-- circularization burn is planned - high thrust). CUT asks to cut the
-- engines (the coast can start, or the orbit is done) and DONE is the
-- orbit: periapsis clear of the atmosphere and the terrain, and circular at
-- the target or as fast as a circle there.
--
-- Once apoapsis has reached the target the gravity turn never comes back.
-- High thrust reaches it low, still in the air, and drag pulls it down on
-- the coast: below the target the coast asks for a prograde top-up until it
-- is back (then moves the circularization burn to the new apoapsis), and a
-- coast that misses apoapsis carries on holding.
--
-- Tried in a point-mass simulation for thrust-to-weight 1.15 to 10, from
-- airless moons to a 90 atm world: low thrust holds all the way to within
-- 3% of a circle at the target, high thrust coasts with a burn of at most
-- coastBurnMax and ends within some 10-25% above it.
--
-- Everything is worked out from the ship's inertial state around the planet,
-- in the player's frame: inside the planet's rotating frame the velocity
-- relative to it gets the rotation added back (v + w x r, the frame spinning
-- about its +y axis), so the orbit is known all the way up through the
-- atmosphere.

local Event = require 'Event'
local Game = require 'Game'
local Vector3 = _G.Vector3

local AscentGuidance = {}

AscentGuidance.READY = "READY"
AscentGuidance.LIFT = "LIFT"
AscentGuidance.PITCH = "PITCH"
AscentGuidance.HOLD = "HOLD"
AscentGuidance.CUT = "CUT"
AscentGuidance.COAST = "COAST"
AscentGuidance.DONE = "DONE"

-- straight up until this high above the ground (m)
local liftAltitude = 1000
-- the lowest climb angle allowed in an atmosphere (deg) is pitchFloorMax at
-- 1 atm and above, falling with the square root of the pressure; heatPitch
-- more as the hull heats from heatStart to heatFull. While the ship climbs
-- steeper than that the gravity turn may point the nose lower
local pitchFloorMax = 70
local heatPitch = 30
local heatStart, heatFull = 0.5, 0.8
-- apoapsis counts as at the target once within this fraction of it, and
-- drops back below only past twice that, so the phase does not flicker
local apTolerance = 0.005
-- the orbit is done with periapsis within this fraction of the target
local doneTolerance = 0.03
-- coast to apoapsis when the circularization burn there takes no more than
-- coastBurnMax seconds and starts (half its time before apoapsis) at least
-- coastMargin seconds from now; otherwise hold
local coastBurnMax = 45
local coastMargin = 10
-- holding, the climb rate is corrected over this many seconds
local holdResponse = 10
-- a planned burn counts as being flown from this many seconds before its start
local burnLead = 5
-- seconds the DONE phase shows before guidance switches itself off
local doneHold = 5
-- linear thrust above this counts as burning
local thrustThreshold = 0.05
-- the default target is this much above the lowest safe altitude, rounded up
-- to defaultStep (m)
local defaultMargin = 1.2
local defaultExtra = 5000
local defaultStep = 5000

-- the launch heading by default (deg, compass): east, where the planet's
-- spin helps most
local defaultHeading = 90

local state = {
	armed = false,
	body = nil,	   -- the planet guidance is armed for
	target = nil,  -- target orbit altitude above the surface (m)
	heading = defaultHeading, -- compass heading to launch along (deg)
	phase = nil,
	normal = nil,  -- the target orbital plane's normal, in the player's frame
	normalTime = nil,
	normalRotating = nil, -- whether that frame was the planet's rotating frame
	reachedTarget = false, -- apoapsis has been at the target: the gravity turn is over
	coasting = false, -- coasting up to apoapsis for the circularization burn
	topUp = false,	   -- coasting, drag pulled apoapsis below the target: burn prograde
	ownPlan = false,   -- the circularization burn in the planner is ours
	doneTime = nil,
	updated = nil, -- Game.time of the cached result
	result = nil,
}

local function isPlanet(body)
	return body ~= nil and (body.superType == "ROCKY_PLANET" or body.superType == "GAS_GIANT")
end

-- the planet's angular speed (rad/s) about its rotating frame's +y axis
local function spinRate(sbody)
	local days = sbody.rotationPeriod
	if not days or days <= 0 then return 0 end
	return 2 * math.pi / (days * 86400)
end

-- the ship's position from the planet's centre and its inertial velocity,
-- both in the player's frame
local function inertialState(player, body)
	local r = -body:GetPositionRelTo(player)
	local v = -body:GetVelocityRelTo(player)
	if player.frameRotating then
		v = v + Vector3(0, spinRate(body:GetSystemBody()), 0):cross(r)
	end
	return r, v
end

-- the orbit through r, v: periapsis radius, apoapsis radius (nil when it
-- escapes) and the time to apoapsis (nil for a circle or an escape)
local function orbitOf(r, v, mu)
	local radius = r:length()
	local energy = v:lengthSqr() / 2 - mu / radius
	local h = r:cross(v):length()
	local e = math.sqrt(math.max(0, 1 + 2 * energy * h * h / (mu * mu)))
	local orbit = { pe = h * h / (mu * (1 + e)) }
	if energy < 0 then
		local a = -mu / (2 * energy)
		orbit.ap = a * (1 + e)
		if e > 1e-6 then
			-- the eccentric anomaly is in (0, pi) climbing, (pi, 2 pi) falling
			local E = math.acos(math.max(-1, math.min(1, (1 - radius / a) / e)))
			if r:dot(v) < 0 then E = 2 * math.pi - E end
			local M = E - e * math.sin(E)
			orbit.timeToAp = ((M <= math.pi and math.pi or 3 * math.pi) - M) / math.sqrt(mu / a ^ 3)
		end
	end
	return orbit
end

local function gravityParameter(sbody)
	return sbody.gravity * sbody.radius * sbody.radius
end

-- rotate v about the +y axis by angle (rad)
local function rotateY(v, angle)
	local c, s = math.cos(angle), math.sin(angle)
	return Vector3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c)
end

-- the lowest altitude an orbit clears the atmosphere and the terrain at (m)
local function safeAltitude(body)
	return body:GetPhysicalRadius() - body:GetSystemBody().radius
end

local function targetRange(body)
	local low = math.ceil(safeAltitude(body) / 1000) * 1000
	return low, math.max(body:GetSystemBody().radius, low * 2)
end

local function defaultTarget(body)
	local low, high = targetRange(body)
	local target = math.ceil((low * defaultMargin + defaultExtra) / defaultStep) * defaultStep
	return math.min(math.max(target, low), high)
end

-- set the target plane from where the ship is: along the launch heading in
-- the rotating frame (its +y the spin axis, so north and east are known),
-- the plane it flies in otherwise; nil when that is not defined (straight up
-- outside the rotating frame, or at a pole)
local function captureNormal(player, r, v)
	local up = r:normalized()
	local normal
	if player.frameRotating then
		local east = Vector3(0, 1, 0):cross(up)
		if east:lengthSqr() > 1e-12 then
			east = east:normalized()
			-- headings run clockwise from north seen from above: east = north x up
			local north = up:cross(east)
			local heading = math.rad(state.heading)
			local along = north * math.cos(heading) + east * math.sin(heading)
			normal = up:cross(along):normalized()
		end
	else
		local n = r:cross(v)
		if n:lengthSqr() > 1e-6 * r:lengthSqr() then
			normal = n:normalized()
		end
	end
	state.normal = normal
	state.normalTime = Game.time
	state.normalRotating = player.frameRotating
end

-- keep the target plane's normal in the player's frame: an inertial direction
-- turns against the planet's spin seen from its rotating frame, and a frame
-- change sets it again from the plane the ship flies in
local function updateNormal(player, body, r, v)
	if state.normalRotating ~= player.frameRotating then
		captureNormal(player, r, v)
	elseif state.normal and player.frameRotating then
		local spin = spinRate(body:GetSystemBody())
		state.normal = rotateY(state.normal, -spin * (Game.time - state.normalTime))
		state.normalTime = Game.time
	elseif not state.normal then
		captureNormal(player, r, v)
	end
end

-- the lowest climb angle allowed here (rad): steep in thick air, more when the hull heats
local function pitchFloor(player, body)
	local floor = 0
	local pressure = body:GetAtmosphericState(player)
	if pressure and pressure > 0 then
		floor = pitchFloorMax * math.sqrt(math.min(1, pressure))
	end
	local heat = (player:GetHullTemperature() - heatStart) / (heatFull - heatStart)
	floor = floor + heatPitch * math.max(0, math.min(1, heat))
	return math.rad(math.min(floor, 90))
end

-- prograde along the target plane, or along the motion without one
local function alongPlane(up, horizontal)
	local along = state.normal and state.normal:cross(up)
	if along and along:lengthSqr() > 1e-6 then
		return along:normalized()
	end
	return horizontal:normalized()
end

-- where to point the nose in the gravity turn, in the player's frame.
-- It aims to reach circular speed for the target radius, along the target
-- plane, together with the climb rate that would just coast up to it, both
-- in the same time T at full thrust a:
--   |dh / T|^2 + (dv / T + g)^2 = a^2
-- where dh is the horizontal velocity still to gain, dv the vertical, and g
-- gravity less the lift of the horizontal speed (v_h^2 / r). The nose then
-- points along (dh / T, dv / T + g): low as horizontal speed is what is left
-- to gain, high while gravity dominates.
local function pitchDirection(player, body, r, v, mu, targetRadius)
	local radius = r:length()
	local up = r / radius
	local verticalSpeed = v:dot(up)
	local horizontal = v - up * verticalSpeed
	local g = math.max(0, mu / (radius * radius) - horizontal:lengthSqr() / radius)
	local along = alongPlane(up, horizontal)

	local dh = along * math.sqrt(mu / targetRadius) - horizontal
	local dv = math.sqrt(2 * g * math.max(0, targetRadius - radius)) - verticalSpeed
	local accel = player:GetAcceleration("forward")

	local horizontalCmd, verticalCmd = Vector3(0, 0, 0), 1
	local A = dh:lengthSqr() + dv * dv
	local B = dv * g
	local C = g * g - accel * accel
	local discriminant = B * B - A * C
	if accel > g and A > 1e-6 and discriminant >= 0 then
		local u = (-B + math.sqrt(discriminant)) / A -- 1 / T
		horizontalCmd = dh * u
		verticalCmd = dv * u + g
	end

	local horizontalLength = horizontalCmd:length()
	local pitch = math.atan2(verticalCmd, horizontalLength)
	-- in thick air, no lower while the climb is shallower than the floor
	local floor = pitchFloor(player, body)
	if math.atan2(verticalSpeed, horizontal:length()) < floor then
		pitch = math.max(floor, pitch)
	end
	pitch = math.min(math.pi / 2, pitch)
	local heading = horizontalLength > 1e-9 and horizontalCmd / horizontalLength or along
	return heading * math.cos(pitch) + up * math.sin(pitch), pitch
end

-- where to point the nose holding apoapsis at the target: along the target
-- plane, climbing at the rate that would just coast up to the target
-- altitude (or down to it from above) with the error corrected over
-- holdResponse seconds; no lower than 30 degrees below the horizon
local function holdDirection(player, r, v, mu, targetRadius)
	local radius = r:length()
	local up = r / radius
	local verticalSpeed = v:dot(up)
	local horizontal = v - up * verticalSpeed
	local g = math.max(0, mu / (radius * radius) - horizontal:lengthSqr() / radius)
	local accel = player:GetAcceleration("forward")
	if accel <= 0 then return up, math.pi / 2 end

	local d = targetRadius - radius
	local need = (d >= 0 and 1 or -1) * math.sqrt(2 * g * math.abs(d))
	local sine = (g + (need - verticalSpeed) / holdResponse) / accel
	local pitch = math.asin(math.max(-0.5, math.min(1, sine)))
	return alongPlane(up, horizontal) * math.cos(pitch) + up * math.sin(pitch), pitch
end

-- how long the burn that circularizes at apoapsis takes at full thrust (s)
local function circularizeBurnTime(orbit, mu, accel)
	if not orbit.ap or accel <= 0 then return math.huge end
	local a = (orbit.ap + orbit.pe) / 2
	local atApoapsis = math.sqrt(mu * math.max(0, 2 / orbit.ap - 1 / a))
	return (math.sqrt(mu / orbit.ap) - atApoapsis) / accel
end

local function orbitNow(player, body)
	local sbody = body:GetSystemBody()
	local r, v = inertialState(player, body)
	return orbitOf(r, v, gravityParameter(sbody)), sbody.radius
end

-- whether the player is somewhere guidance could take it to orbit from: on
-- or over a planet (landed, docked at a surface port, or flying), and not
-- already in a safe orbit
function AscentGuidance.CanArm(player)
	local body = player.frameBody
	if not isPlanet(body) then return false end
	if player:IsLanded() then return true end
	if player:IsDocked() then
		local station = player:GetDockedWith()
		return station ~= nil and station:IsGroundStation()
	end
	local orbit, radius = orbitNow(player, body)
	return not (orbit.ap and orbit.pe - radius >= safeAltitude(body))
end

function AscentGuidance.IsArmed()
	return state.armed
end

function AscentGuidance.Arm(player)
	local body = player.frameBody
	if not isPlanet(body) then return end
	if state.body ~= body or not state.target then
		state.target = defaultTarget(body)
	end
	state.armed = true
	state.body = body
	state.phase = nil
	state.normal = nil
	state.normalRotating = nil
	state.reachedTarget, state.coasting, state.topUp, state.ownPlan = false, false, false, false
	state.doneTime = nil
	state.updated, state.result = nil, nil
end

function AscentGuidance.Disarm()
	state.armed = false
	state.phase = nil
	state.updated, state.result = nil, nil
end

-- follow the planet the player is at while guidance is off, so the target
-- altitude can be set (and shows the planet's default) before switching it on
function AscentGuidance.Sync(player)
	local body = player.frameBody
	if state.armed or not isPlanet(body) or body == state.body then return end
	state.body = body
	state.target = defaultTarget(body)
end

-- target orbit altitude (m) and its allowed range
function AscentGuidance.GetTarget()
	return state.target
end

-- the launch heading (deg, compass: 0 north, 90 east), set before lift-off
function AscentGuidance.GetHeading()
	return state.heading
end

function AscentGuidance.CanSetHeading()
	return not state.armed or state.phase == nil or state.phase == AscentGuidance.READY
		or state.phase == AscentGuidance.LIFT
end

function AscentGuidance.SetHeading(heading)
	if not AscentGuidance.CanSetHeading() then return end
	state.heading = heading % 360
	-- the target plane follows on the next update
	state.normal, state.normalRotating = nil, nil
	state.updated, state.result = nil, nil
end

-- the inclination (deg) launching along the heading from latitude latitude
-- (deg) gives, by spherical trigonometry: cos i = cos(latitude) sin(heading).
-- Over 90 the orbit runs against the planet's spin
function AscentGuidance.InclinationFrom(latitude)
	local c = math.cos(math.rad(latitude)) * math.sin(math.rad(state.heading))
	return math.deg(math.acos(math.max(-1, math.min(1, c))))
end

function AscentGuidance.GetTargetRange()
	if not state.body then return nil end
	return targetRange(state.body)
end

function AscentGuidance.SetTarget(altitude)
	if not state.body then return end
	local low, high = targetRange(state.body)
	state.target = math.max(low, math.min(high, altitude))
	state.updated, state.result = nil, nil
end

-- whether the circularization burn can be planned now: false and why not
-- ("plan" when a plan with burns still to fly exists)
function AscentGuidance.CanCircularize(player)
	if not Game.systemView then return false end
	if player:GetActiveBurnIndex() >= 0 then return false, "plan" end
	return true
end

-- whether the plan in the planner is the circularization burn guidance made:
-- still its one burn, still to fly
function AscentGuidance.HasOwnPlan(player)
	local systemView = Game.systemView
	return state.ownPlan and systemView ~= nil and systemView:TransferPlannerGetBurnCount() == 1
		and player:GetActiveBurnIndex() == 0
end

-- plan the circularization burn at the next apsis (apoapsis on the way up),
-- replacing a finished plan; the flight HUD's burn list flies it
function AscentGuidance.Circularize(player)
	if not AscentGuidance.CanCircularize(player) then return false end
	local systemView = Game.systemView
	for idx = systemView:TransferPlannerGetBurnCount() - 1, 0, -1 do
		systemView:TransferPlannerRemoveBurn(idx)
	end
	local idx = systemView:TransferPlannerInsertBurn(-1)
	if not idx or idx < 0 then return false end
	if not systemView:TransferPlannerCircularize(idx) then
		systemView:TransferPlannerRemoveBurn(idx)
		return false
	end
	return true
end

-- work out this moment's guidance; nil when not armed. Once per game time,
-- so every caller in a frame sees the same result. The result holds:
--   phase, target (m), safe (m, lowest safe orbit altitude),
--   ap, pe (altitudes, m; ap nil when escaping), timeToAp (s or nil),
--   speed (inertial horizontal, m/s), circularSpeed (at the target, m/s),
--   twr (main engine against surface gravity),
--   direction (Vector3 in the player's frame, LIFT, PITCH, HOLD and a top-up
--   only), pitch (rad), topUp (true coasting with apoapsis to top up)
-- Entering COAST it plans the circularization burn when no other plan is
-- in progress.
function AscentGuidance.Update(player)
	if not state.armed then return nil end
	if state.updated == Game.time and state.result then return state.result end

	local body = player.frameBody
	if body ~= state.body or not isPlanet(body) then
		AscentGuidance.Disarm()
		return nil
	end

	local sbody = body:GetSystemBody()
	local radius = sbody.radius
	local mu = gravityParameter(sbody)
	local targetRadius = radius + state.target
	local r, v = inertialState(player, body)
	local orbit = orbitOf(r, v, mu)
	local ap = orbit.ap and orbit.ap - radius
	local pe = orbit.pe - radius
	local safe = safeAltitude(body)
	local up = r:normalized()
	local climbing = v:dot(up) > 0
	local horizontalSpeed = (v - up * v:dot(up)):length()
	local accel = player:GetAcceleration("forward")
	local thrusting = player:GetThrusterState():length() > thrustThreshold
	-- flying a planned burn (the circularization) now, not to be told to cut:
	-- one is planned and its start, half its time before the node, is near
	local activeBurn = player:GetActiveBurnIndex()
	local planned = false
	if activeBurn >= 0 and accel > 0 then
		local untilNode = player:GetManeuverTime() - Game.time
		planned = untilNode <= player:GetManeuverVelocity():length() / accel / 2 + burnLead
	end

	updateNormal(player, body, r, v)

	-- in orbit: clear of the ground, and circular at the target or as fast
	-- as a circle here (more thrust along the horizon only raises apoapsis)
	local orbitDone = ap and pe >= safe
		and (pe >= state.target * (1 - doneTolerance) or horizontalSpeed >= math.sqrt(mu / r:length()))
	local previous = state.phase
	local atTarget = previous == AscentGuidance.HOLD or previous == AscentGuidance.CUT or previous == AscentGuidance.COAST
	local apAbove = not ap or ap >= state.target * (1 - (atTarget and 2 or 1) * apTolerance)
	-- whether the circularization burn at apoapsis fits before it from here
	local burnTime = circularizeBurnTime(orbit, mu, accel)
	local coastFits = climbing and orbit.timeToAp ~= nil and burnTime <= coastBurnMax
		and burnTime / 2 + coastMargin <= orbit.timeToAp
	local landed = player:IsLanded() or player:IsDocked()
	-- once apoapsis has reached the target the gravity turn is over: a sag
	-- below it after that is topped up, not climbed again
	state.reachedTarget = not landed and (state.reachedTarget or apAbove)
	local wasCoasting = state.coasting
	local phase
	if landed then
		phase = AscentGuidance.READY
	elseif previous == AscentGuidance.DONE then
		phase = AscentGuidance.DONE
	elseif orbitDone then
		phase = (thrusting and not planned) and AscentGuidance.CUT or AscentGuidance.DONE
	elseif state.coasting and (climbing or planned) then
		-- coasting up to apoapsis (or flying the circularization burn). In
		-- the air drag pulls apoapsis down: below the target, burn prograde
		-- until it is back, then cut again
		if state.topUp then
			state.topUp = ap ~= nil and ap < state.target
			-- back at the target: the circularization burn moves to the new apoapsis
			if not state.topUp and AscentGuidance.HasOwnPlan(player) then
				Game.systemView:TransferPlannerCircularize(activeBurn)
			end
		else
			state.topUp = ap ~= nil and ap < state.target * (1 - 2 * apTolerance) and not planned
		end
		if state.topUp then
			phase = AscentGuidance.COAST
		elseif thrusting and not planned then
			phase = AscentGuidance.CUT
		else
			phase = AscentGuidance.COAST
		end
	elseif state.reachedTarget then
		state.coasting, state.topUp = apAbove and coastFits, false
		if state.coasting then
			phase = (thrusting and not planned) and AscentGuidance.CUT or AscentGuidance.COAST
		else
			phase = AscentGuidance.HOLD
		end
	elseif (previous == nil or previous == AscentGuidance.READY or previous == AscentGuidance.LIFT)
		and player:GetAltitudeRelTo(body) < liftAltitude then
		phase = AscentGuidance.LIFT
	else
		phase = AscentGuidance.PITCH
	end
	if phase ~= AscentGuidance.COAST and phase ~= AscentGuidance.CUT then
		state.coasting, state.topUp = false, false
	end

	if phase == AscentGuidance.DONE then
		state.doneTime = state.doneTime or Game.time
		if Game.time - state.doneTime > doneHold then
			AscentGuidance.Disarm()
			return nil
		end
	end
	if state.coasting and not wasCoasting and AscentGuidance.CanCircularize(player) then
		state.ownPlan = AscentGuidance.Circularize(player)
	end
	state.phase = phase

	local result = {
		phase = phase,
		target = state.target,
		safe = safe,
		ap = ap,
		pe = pe,
		timeToAp = orbit.timeToAp,
		speed = horizontalSpeed,
		circularSpeed = math.sqrt(mu / targetRadius),
		twr = accel / sbody.gravity,
	}
	if phase == AscentGuidance.LIFT then
		result.direction, result.pitch = up, math.pi / 2
	elseif phase == AscentGuidance.PITCH then
		result.direction, result.pitch = pitchDirection(player, body, r, v, mu, targetRadius)
	elseif phase == AscentGuidance.HOLD then
		result.direction, result.pitch = holdDirection(player, r, v, mu, targetRadius)
	elseif phase == AscentGuidance.COAST and state.topUp then
		-- prograde raises apoapsis the most for the delta-v
		result.topUp = true
		result.direction = v:normalized()
		result.pitch = math.asin(math.max(-1, math.min(1, result.direction:dot(up))))
	end

	state.updated, state.result = Game.time, result
	return result
end

local function reset()
	state.armed = false
	state.body = nil
	state.target = nil
	state.heading = defaultHeading
	state.phase = nil
	state.updated, state.result = nil, nil
end

Event.Register("onGameStart", reset)
Event.Register("onGameEnd", reset)

return AscentGuidance
