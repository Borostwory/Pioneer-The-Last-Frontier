-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Debug tab for a model's fill light ("fill" in its .model's materials: the
-- surface glows softly with its own colour, as if lit from all round). The
-- sliders change it while playing, on every copy of the model; Save writes
-- the values into the .model (the game's copy and the source tree's).

local Game  = require 'Game'
local Space = require 'Space'
local Dev   = require 'Dev'

local ui = require 'pigui'

local Notification = require 'pigui.libs.notification'
local debugView    = require 'pigui.views.debug'

local icons = ui.theme.icons

--=============================================================================

local SOURCES = { "Navigation target", "Docked / nearest station", "My ship" }

local state = {
	source = 0,       -- 0-based, as ui.combo gives it
	saved = {},       -- model name -> the fills as last saved or loaded, to show changes
}

-- the body whose model is edited
local function findBody()
	if state.source == 0 then
		local target = Game.player:GetNavTarget()
		if target and target:isa("ModelBody") then return target end
		return nil
	elseif state.source == 1 then
		local docked = Game.player:GetDockedWith()
		if docked then return docked end
		local best, bestDist = nil, math.huge
		for _, body in ipairs(Space.GetBodies("SpaceStation")) do
			local d = Game.player:DistanceTo(body)
			if d < bestDist then best, bestDist = body, d end
		end
		return best
	end
	return Game.player
end

local function drawMaterials(body, info)
	local saved = state.saved[info.model]
	if not saved then
		saved = {}
		for _, m in ipairs(info.materials) do saved[m.name] = m.fill end
		state.saved[info.model] = saved
	end

	local changed = false
	for _, m in ipairs(info.materials) do
		local value = ui.sliderFloat(m.name, m.fill, 0.0, 1.0, "%.3f")
		if value ~= m.fill then
			Dev.SetModelFill(body, m.name, value)
		end
		if math.abs(value - (saved[m.name] or 0)) > 0.0005 then
			changed = true
			ui.sameLine()
			ui.text("(was " .. string.format("%.3f", saved[m.name] or 0) .. ")")
		end
	end
	return changed, saved
end

debugView.registerTab("DebugFill", {
	mod = true,
	label = "Fill light",
	icon = icons.sun,
	show = function() return Game.player ~= nil end,
	draw = function()
		local _, source = ui.combo("Model of", state.source, SOURCES)
		state.source = source

		local body = findBody()
		local info = body and Dev.GetModelFills(body)
		if not info then
			ui.text("Nothing to edit: pick a navigation target, or come near a station.")
			return
		end

		ui.text(body:GetLabel() .. "  (" .. info.model .. ")")
		ui.text("0 = no fill; the game shows the fill as the surface's own colour times this.")
		ui.separator()

		local changed, saved = drawMaterials(body, info)

		ui.separator()
		if ui.button("Save to .model") then
			local ok, files = pcall(Dev.SaveModelFills, body)
			if ok then
				for _, m in ipairs(info.materials) do saved[m.name] = m.fill end
				Notification.add(Notification.Type.Info, "Fill saved: " .. info.model, table.concat(files, "\n"))
			else
				Notification.add(Notification.Type.Error, "Fill not saved", tostring(files))
			end
		end
		ui.sameLine()
		if ui.button("Undo changes") then
			for name, fill in pairs(saved) do Dev.SetModelFill(body, name, fill) end
		end
		if changed then
			ui.sameLine()
			ui.text("unsaved changes")
		end
	end
})
