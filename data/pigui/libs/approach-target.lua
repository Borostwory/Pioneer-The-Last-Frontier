-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- The target of the transfer planner's last planned approach, shared between
-- the map's planner sidebar (modules/system-view-ui.lua), which sets it when
-- an approach is planned, and the flight HUD's burn list
-- (modules/flight-ui/burn-hud.lua), which can stop the clock before the
-- closest approach to it. Forgotten when the game ends, and read back only
-- while the body still exists.

local Event = require 'Event'

local ApproachTarget = {}

local target = nil

function ApproachTarget.Set(body)
	target = body
end

-- the target, or nil when there is none or it has left the game
function ApproachTarget.Get()
	if target and target:exists() then
		return target
	end
	return nil
end

Event.Register("onGameEnd", function()
	target = nil
end)

return ApproachTarget
