-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- Debug tab that counts the worlds within some distance of the player: how many planets and moons, what kind,
-- what their surface is like, and which have bases (ground or orbital stations). Shows a summary here and
-- writes it, with one line per world, to frontier_log.txt. Then pick a world by type (Earth-like, ocean world...),
-- as planetgen makes it, and jump straight into a circular orbit around it.

local Game = require 'Game'
local Event = require 'Event'
local Dev = require 'Dev'

local ui = require 'pigui'

local Notification = require 'pigui.libs.notification'
local debugView = require 'pigui.views.debug'

local icons = ui.theme.icons
local colors = ui.theme.colors

--=============================================================================

local state = {
	range = 20.0,          -- ly
	perWorldLines = true,  -- also log one line per world
	report = nil,          -- { summary = { string }, worlds = { string }, title = string }

	-- the worlds the last count found, nearest first:
	-- { path, systemPath, name, systemName, distance, types = { [typeIdx] = true }, hasBase, label }
	worlds = {},
	centreName = nil,
	typeCounts = {},       -- typeIdx -> { all = n, based = n }
	typeIdx = 2,           -- Earth-like
	basesOnly = false,
	picked = nil,          -- the filtered list for typeIdx and basesOnly
	selected = nil,        -- one entry of worlds
	radii = 2.0,           -- orbit radius, in the world's radii
	pending = nil,         -- orbit to take once the hyperjump arrives: { path = SystemPath, radii = number }
}

-- The types a world can be picked by; a world can be several at once. Each asks planetgen's own answer (p, from
-- Dev.PlanetSurface: what the terrain is built from) rather than the raw system data, which planetgen changes by
-- temperature: an "ocean" of 0.8 at 80 K is ice. p is nil for gas giants.
local function solid(s) return s.superType == "ROCKY_PLANET" and s.type ~= "PLANET_ASTEROID" end

-- plants green enough to read as green: under sun-like and hotter stars; under red and orange ones they're dark
local function greenPlants(p) return p.plants.g >= 1.3 * p.plants.r and p.plants.g >= 1.3 * p.plants.b end
-- planetgen paints grass and forest from life 0.3, and needs rain for them
local function hasPlants(s, p) return s.life >= 0.3 and p.rain > 0.1 end
local function lava(p) return p.lavaPlanet > 0.5 or p.melted > 0.5 end

local worldTypes = {
	{ "Any world", function(s, p) return true end },
	{ "Earth-like (water seas, rain, plants)", function(s, p)
		return p and solid(s) and p.seaState == "liquid water" and p.sea >= 0.2 and p.sea <= 0.9 and hasPlants(s, p)
	end },
	{ "Earth-like, green plants", function(s, p)
		return p and solid(s) and p.seaState == "liquid water" and p.sea >= 0.2 and p.sea <= 0.9 and hasPlants(s, p) and greenPlants(p)
	end },
	{ "Plants (any sea or none)", function(s, p) return p and hasPlants(s, p) end },
	{ "Ocean world (liquid sea 50%+)", function(s, p) return p and p.sea >= 0.5 end },
	{ "Water seas (some liquid water)", function(s, p) return p and p.seaState == "liquid water" and p.sea > 0 end },
	{ "Hydrocarbon seas", function(s, p) return p and p.seaState == "liquid hydrocarbon" and p.sea > 0 end },
	{ "Frozen ocean (sea frozen solid)", function(s, p) return p and p.seaState == "frozen" and p.frozen >= 0.3 end },
	{ "Ice world (ground ice or ice 50%+)", function(s, p)
		return p and solid(s) and (p.ground == "ICE" or p.ground == "DIRTY_ICE" or p.ice >= 0.5)
	end },
	{ "Lava world", function(s, p) return p and lava(p) end },
	{ "Desert with air (no sea, little ice)", function(s, p)
		return p and solid(s) and p.air > 0 and p.sea == 0 and p.ice < 0.2 and not p.iceCrust and not lava(p)
	end },
	{ "Thick air (3 atm+)", function(s, p) return p and s.surfacePressure >= 3 end },
	{ "Airless rock", function(s, p)
		return p and solid(s) and p.air == 0 and p.ground == "REGOLITH" and p.ice < 0.5 and not lava(p)
	end },
	{ "Gas giant", function(s, p) return s.superType == "GAS_GIANT" end },
	{ "Asteroid", function(s, p) return s.type == "PLANET_ASTEROID" end },
}

-- a type's name without its explanation in brackets, for the per-world log lines: "Ocean world (liquid sea 50%+)"
-- becomes "Ocean world"
local function shortTypeName(name)
	return (name:gsub("%s*%b()", ""))
end

local function plantsWord(p)
	if greenPlants(p) then return "green" end
	if p.plants.b > p.plants.g then return "teal" end
	return p.plants.r > p.plants.g and "dark red" or "olive"
end

-- each grouping's buckets, in the order they're shown; a value goes in the first bucket whose limit it is below
local bands = {
	size = {   -- radius, km
		{ 800, "tiny (< 800 km)" },
		{ 2500, "small (Moon-sized)" },
		{ 5000, "medium (Mars-sized)" },
		{ 8000, "Earth-sized" },
		{ math.huge, "super-Earth (> 8000 km)" },
	},
	air = {    -- surface pressure, atm
		{ 0.001, "none" },
		{ 0.3, "thin" },
		{ 3, "Earth-like" },
		{ 30, "thick" },
		{ math.huge, "crushing (> 30 atm)" },
	},
	temp = {   -- average temperature, K
		{ 150, "deep frozen (< 150 K)" },
		{ 240, "cold" },
		{ 310, "temperate (240-310 K)" },
		{ 500, "hot" },
		{ math.huge, "scorching (> 500 K)" },
	},
	ocean = {  -- volatileLiquid
		{ 0.01, "dry" },
		{ 0.1, "lakes" },
		{ 0.5, "seas" },
		{ math.huge, "ocean world (>= 50%)" },
	},
	ice = {    -- volatileIces
		{ 0.1, "little or none" },
		{ 0.5, "caps and patches" },
		{ math.huge, "mostly ice (>= 50%)" },
	},
	volcanic = {
		{ 0.3, "quiet" },
		{ 0.7, "active" },
		{ math.huge, "highly volcanic (>= 0.7)" },
	},
	life = {
		{ 0.0001, "none" },
		{ 0.5, "some" },
		{ math.huge, "rich (>= 0.5)" },
	},
	metal = {  -- metallicity of the crust
		{ 0.33, "light rock" },
		{ 0.66, "mixed" },
		{ math.huge, "metal-rich" },
	},
}

local function band(name, value)
	for _, b in ipairs(bands[name]) do
		if value < b[1] then return b[2] end
	end
	return bands[name][#bands[name]][2]
end

-- the one word that best sums up a rocky world's surface, the strongest feature first
local lookOrder = { "lava", "ocean", "ice", "living", "rock with air", "bare rock" }

---@param sbody SystemBody
local function look(sbody)
	if sbody.volcanicity >= 0.7 then return "lava" end
	if sbody.volatileLiquid >= 0.5 then return "ocean" end
	if sbody.volatileIces >= 0.5 then return "ice" end
	if sbody.life >= 0.1 then return "living" end
	if sbody.surfacePressure >= 0.001 then return "rock with air" end
	return "bare rock"
end

local kindOrder = { "gas giant", "rocky planet", "rocky moon", "asteroid" }

---@param sbody SystemBody
local function kind(sbody)
	if sbody.superType == "GAS_GIANT" then return "gas giant" end
	if sbody.type == "PLANET_ASTEROID" then return "asteroid" end
	return sbody.isMoon and "rocky moon" or "rocky planet"
end

local baseOrder = { "ground and orbital", "ground only", "orbital only", "none" }

local function baseKind(ground, orbital)
	if ground > 0 and orbital > 0 then return "ground and orbital" end
	if ground > 0 then return "ground only" end
	if orbital > 0 then return "orbital only" end
	return "none"
end

--=============================================================================

local function newCounter() return { n = 0, by = {} } end

local function add(counter, key)
	counter.by[key] = (counter.by[key] or 0) + 1
	counter.n = counter.n + 1
end

local function countLines(lines, title, counter, order)
	table.insert(lines, title)
	for _, key in ipairs(order) do
		local n = counter.by[key] or 0
		if n > 0 then
			table.insert(lines, string.format("    %-28s %5d  (%.0f%%)", key, n, 100 * n / math.max(counter.n, 1)))
		end
	end
end

local function bandOrder(name)
	local order = {}
	for _, b in ipairs(bands[name]) do table.insert(order, b[2]) end
	return order
end

local function count()
	local here = Game.system
	if not here then return end
	if not Dev.PlanetSurface then
		error("this game build has no Dev.PlanetSurface: rebuild the game first")
	end

	local systems = here:GetNearbySystems(state.range)
	table.insert(systems, 1, here)

	local found = {}
	for i, system in ipairs(systems) do
		table.insert(found, { system = system, distance = i == 1 and 0 or here:DistanceTo(system) })
	end
	table.sort(found, function(a, b) return a.distance < b.distance end)

	local totals = { systems = #found, explored = 0, withBases = 0, stars = 0, ground = 0, orbital = 0, starOrbit = 0 }
	local kinds = newCounter()
	local looks = newCounter()
	local surface = {}
	for name in pairs(bands) do surface[name] = newCounter() end
	local bases = newCounter()
	local basesByKind = {}   -- kind -> counter of baseKind
	local basesByLook = {}   -- look -> counter of baseKind
	local worldLines = {}
	local worldList = {}
	local typeCounts = {}
	for i = 1, #worldTypes do typeCounts[i] = { all = 0, based = 0 } end

	for _, entry in ipairs(found) do
		local system = entry.system
		if system.explored then totals.explored = totals.explored + 1 end
		if system.numberOfStations > 0 then totals.withBases = totals.withBases + 1 end

		for _, path in ipairs(system:GetBodyPaths()) do
			local sbody = path:GetSystemBody()

			if sbody.superType == "STAR" then
				totals.stars = totals.stars + 1

			elseif sbody.superType == "STARPORT" then
				if sbody.isGroundStation then totals.ground = totals.ground + 1 else totals.orbital = totals.orbital + 1 end
				local parent = sbody.parent
				if parent and parent.superType ~= "ROCKY_PLANET" and parent.superType ~= "GAS_GIANT" then
					totals.starOrbit = totals.starOrbit + 1
				end

			elseif sbody.superType == "ROCKY_PLANET" or sbody.superType == "GAS_GIANT" then
				local k = kind(sbody)
				add(kinds, k)

				local ground, orbital = 0, 0
				for _, child in ipairs(sbody.children) do
					if child.isGroundStation then ground = ground + 1
					elseif child.isSpaceStation then orbital = orbital + 1 end
				end
				local b = baseKind(ground, orbital)
				add(bases, b)
				basesByKind[k] = basesByKind[k] or newCounter()
				add(basesByKind[k], b)

				local line = string.format("%6.1f ly  %-20s %-22s %-13s", entry.distance, system.name, sbody.name, k)

				if sbody.superType == "ROCKY_PLANET" then
					local l = look(sbody)
					add(looks, l)
					basesByLook[l] = basesByLook[l] or newCounter()
					add(basesByLook[l], b)
					add(surface.size, band("size", sbody.radius / 1000))
					add(surface.air, band("air", sbody.surfacePressure))
					add(surface.temp, band("temp", sbody.averageTemp))
					add(surface.ocean, band("ocean", sbody.volatileLiquid))
					add(surface.ice, band("ice", sbody.volatileIces))
					add(surface.volcanic, band("volcanic", sbody.volcanicity))
					add(surface.life, band("life", sbody.life))
					add(surface.metal, band("metal", sbody.metallicity))

					line = line .. string.format(" %-13s r %5.0f km  g %4.2f  %4.0f K  air %7.3f atm  metal %.2f  volc %.2f  ice %.2f  ocean %.2f  life %.2f",
						l, sbody.radius / 1000, sbody.gravity / 9.81, sbody.averageTemp, sbody.surfacePressure,
						sbody.metallicity, sbody.volcanicity, sbody.volatileIces, sbody.volatileLiquid, sbody.life)
				else
					line = line .. string.format(" %-13s r %5.0f km", "", sbody.radius / 1000)
				end

				if b ~= "none" then
					line = line .. string.format("  BASES ground %d orbital %d", ground, orbital)
				end

				local p = sbody.superType == "ROCKY_PLANET" and Dev.PlanetSurface(sbody) or nil
				local types = {}
				local typeNames = {}
				for i, t in ipairs(worldTypes) do
					if t[2](sbody, p) then
						types[i] = true
						typeCounts[i].all = typeCounts[i].all + 1
						if b ~= "none" then typeCounts[i].based = typeCounts[i].based + 1 end
						if i > 1 then table.insert(typeNames, shortTypeName(t[1])) end
					end
				end
				if #typeNames > 0 then
					line = line .. "  TYPES " .. table.concat(typeNames, "; ")
				end
				table.insert(worldLines, line)
				local label = string.format("%5.1f ly  %s, %s  (%s, %.0f km", entry.distance, sbody.name, system.name, k, sbody.radius / 1000)
				if p then
					label = label .. string.format(", %.0f K, %.2f atm, sea %.0f%% %s, ice %.2f, rain %.2f, ground %s",
						sbody.averageTemp, sbody.surfacePressure, 100 * p.sea, p.seaState, p.ice, p.rain, p.ground:lower())
					if hasPlants(sbody, p) then
						label = label .. string.format(", %s plants (life %.2f)", plantsWord(p), sbody.life)
					end
				end
				label = label .. ")"
				if b ~= "none" then label = label .. string.format("  bases: %d ground, %d orbital", ground, orbital) end
				table.insert(worldList, {
					path = sbody.path, systemPath = system.path, name = sbody.name, systemName = system.name,
					distance = entry.distance, types = types, hasBase = b ~= "none", label = label,
				})
			end
		end
	end

	local s = {}
	table.insert(s, string.format("Within %.0f ly of %s: %d systems (%d explored, %d with bases), %d stars, %d worlds",
		state.range, here.name, totals.systems, totals.explored, totals.withBases, totals.stars, kinds.n))
	table.insert(s, string.format("Bases: %d ground stations, %d orbital stations (%d of them orbit a star, not a world)",
		totals.ground, totals.orbital, totals.starOrbit))

	table.insert(s, "Worlds by picker type (a world can be several types; same tests as the picker below):")
	local typeOrder = {}   -- most common first; "Any world" (1) left out
	for i = 2, #worldTypes do table.insert(typeOrder, i) end
	table.sort(typeOrder, function(a, b)
		if typeCounts[a].all ~= typeCounts[b].all then return typeCounts[a].all > typeCounts[b].all end
		return a < b
	end)
	for _, i in ipairs(typeOrder) do
		local c = typeCounts[i]
		table.insert(s, string.format("    %-40s %5d  (%.1f%%), %d with bases",
			worldTypes[i][1], c.all, 100 * c.all / math.max(kinds.n, 1), c.based))
	end

	countLines(s, "Worlds by kind:", kinds, kindOrder)
	countLines(s, "Rocky worlds (planets and moons, not asteroids) by look:", looks, lookOrder)
	countLines(s, "  size:", surface.size, bandOrder("size"))
	countLines(s, "  air:", surface.air, bandOrder("air"))
	countLines(s, "  temperature:", surface.temp, bandOrder("temp"))
	countLines(s, "  ocean:", surface.ocean, bandOrder("ocean"))
	countLines(s, "  ice:", surface.ice, bandOrder("ice"))
	countLines(s, "  volcanism:", surface.volcanic, bandOrder("volcanic"))
	countLines(s, "  life:", surface.life, bandOrder("life"))
	countLines(s, "  crust:", surface.metal, bandOrder("metal"))

	countLines(s, "Worlds with bases:", bases, baseOrder)
	for _, k in ipairs(kindOrder) do
		if basesByKind[k] then countLines(s, "  " .. k .. ":", basesByKind[k], baseOrder) end
	end
	for _, l in ipairs(lookOrder) do
		if basesByLook[l] then countLines(s, "  rocky, " .. l .. ":", basesByLook[l], baseOrder) end
	end

	state.report = { summary = s, worlds = worldLines }
	state.worlds = worldList
	state.centreName = here.name
	state.typeCounts = typeCounts
	state.picked = nil
	state.selected = nil
end

--=============================================================================

local function pickedWorlds()
	if not state.picked then
		state.picked = {}
		for _, w in ipairs(state.worlds) do
			if w.types[state.typeIdx] and (w.hasBase or not state.basesOnly) then
				table.insert(state.picked, w)
			end
		end
	end
	return state.picked
end

-- Put the player in a circular orbit around a world of the current system, radii of its radius from its centre,
-- going round its north-south axis' plane (the frame's y axis), engines off
---@param path SystemPath
local function enterOrbit(path, radii)
	local sbody = path:GetSystemBody()
	local body = sbody.body
	if not body then
		Notification.add(Notification.Type.Error, "Nearby worlds: {} is not in this system" % { sbody.name })
		return
	end

	local player = Game.player
	local ok, err = pcall(Dev.TeleportNearBody, player, body, radii)
	if not ok then
		Notification.add(Notification.Type.Error, "Nearby worlds: teleport failed", tostring(err))
		return
	end

	-- now at rest in the world's non-rotating frame: add the circular orbit speed, sideways to the world
	local r = -body:GetPositionRelTo(player)
	local dir = r:cross(Vector3(0, 1, 0))
	if dir:lengthSqr() < 1e-6 * r:lengthSqr() then dir = r:cross(Vector3(1, 0, 0)) end
	local gm = sbody.gravity * sbody.radius * sbody.radius
	local speed = math.sqrt(gm / r:length())
	player:SetVelocity(player:GetVelocity() + dir:normalized() * speed)
	player:SetFlightControlState("CONTROL_MANUAL")
end

local function goToOrbit(world)
	if Game.system and world.systemPath:IsSameSystem(Game.system.path) then
		enterOrbit(world.path, state.radii)
		return
	end

	state.pending = { path = world.path, radii = state.radii }
	-- 1 second warmup, no time in hyperspace, no range or fuel checks
	local status = Game.player:InitiateHyperjumpTo(world.systemPath, 1, 0, {})
	if status ~= "OK" then
		state.pending = nil
		Notification.add(Notification.Type.Error, "Nearby worlds: can't jump", status)
	end
end

local function drawPicker()
	if #state.worlds == 0 then
		ui.text("Press Count first")
		return
	end

	local function typeLabel(i)
		local c = state.typeCounts[i]
		return string.format("%s  (%d, %d with a base)", worldTypes[i][1], c.all, c.based)
	end

	ui.nextItemWidth(ui.getContentRegion().x * 0.7)
	ui.comboBox("##worldType", typeLabel(state.typeIdx), function()
		for i = 1, #worldTypes do
			if ui.selectable(typeLabel(i), i == state.typeIdx) then
				state.typeIdx = i
				state.picked = nil
			end
		end
	end)
	ui.sameLine()
	local changed, v = ui.checkbox("With a base", state.basesOnly)
	if changed then
		state.basesOnly = v
		state.picked = nil
	end

	local picked = pickedWorlds()
	local shown = math.min(#picked, 500)
	ui.textColored(colors.fontDim, string.format("%d worlds, distance from %s%s", #picked, state.centreName,
		#picked > shown and string.format(" (nearest %d shown)", shown) or ""))

	ui.child("nearbyWorldsPick", Vector2(0, ui.getTextLineHeightWithSpacing() * 12), function()
		for i = 1, shown do
			local w = picked[i]
			if ui.selectable("{}##pick{}" % { w.label, i }, w == state.selected) then
				state.selected = w
			end
		end
	end)

	ui.nextItemWidth(ui.getContentRegion().x * 0.5)
	state.radii = ui.sliderFloat("Orbit (radii from centre)", state.radii, 1.1, 10.0, "%.1f")

	local flightState = Game.player:GetFlightState()
	if flightState ~= "FLYING" then
		ui.textColored(colors.alertYellow, "Undock or take off first (ship is {})" % { flightState:lower() })
	elseif state.pending then
		ui.text("Jumping...")
	elseif state.selected then
		if ui.button("Go into orbit around {}" % { state.selected.name }) then
			goToOrbit(state.selected)
		end
	end
end

local function writeToLog()
	local report = state.report
	if not report then return end
	Dev.LogNote("NEARBY WORLDS ----------------------------------------")
	for _, line in ipairs(report.summary) do Dev.LogNote(line) end
	if state.perWorldLines then
		Dev.LogNote("Every world, nearest first:")
		for _, line in ipairs(report.worlds) do Dev.LogNote(line) end
	end
	Dev.LogNote("NEARBY WORLDS END ------------------------------------")
	Notification.add(Notification.Type.Info, "Nearby worlds written to frontier_log.txt")
end

--=============================================================================

Event.Register("onEnterSystem", function(ship)
	if not ship:IsPlayer() or not state.pending then return end

	local pending = state.pending
	state.pending = nil
	enterOrbit(pending.path, pending.radii)
end)

Event.Register("onGameEnd", function()
	state.report = nil
	state.worlds = {}
	state.picked = nil
	state.selected = nil
	state.pending = nil
end)

debugView.registerTab("DebugNearbyWorlds", {
	mod = true,
	label = "Nearby worlds",
	icon = icons.system_map,
	show = function() return Game.player ~= nil and not Game.InHyperspace() end,
	draw = function()
		ui.nextItemWidth(ui.getContentRegion().x * 0.5)
		state.range = ui.sliderFloat("Within (ly)", state.range, 5.0, 40.0, "%.0f")

		ui.sameLine()
		if ui.button("Count") then
			local ok, err = pcall(count)
			if not ok then
				Notification.add(Notification.Type.Error, "Nearby worlds: count failed", tostring(err))
			end
		end

		if state.report then
			ui.sameLine()
			if ui.button("Write to log") then
				writeToLog()
			end
			ui.sameLine()
			local _, v = ui.checkbox("with every world", state.perWorldLines)
			state.perWorldLines = v
		end

		ui.textColored(colors.fontDim, "Loads every system in range: large ranges take a few seconds. Writing needs the frontier log turned on.")
		ui.separator()

		if ui.collapsingHeader("Pick a world and go into orbit", { "DefaultOpen" }) then
			drawPicker()
		end

		if state.report and ui.collapsingHeader("Summary") then
			for _, line in ipairs(state.report.summary) do
				ui.text(line)
			end
		end
	end
})
