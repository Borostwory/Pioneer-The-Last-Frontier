-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- A test system for the black hole look, about 8 light years from Sol.
-- The hole is a million Suns: a small one would be impossible to look at up close, as anything near it would fall
-- in or circle it in a split second. Around this one, four Schwarzschild radii out (the Travel tab's default), a ship
-- falls in over about two minutes. It gives no light, so its planets are dark.

local s = CustomSystem:new('Black Hole Test', { 'STAR_SM_BH' })
	:short_desc('A test system around a bare black hole.')
	:long_desc([[A black hole of a million Suns, with no gas falling into it: nothing to see but the sky bent round its shadow.]])

local hole = CustomSystemBody:new('Black Hole Test', 'STAR_SM_BH')
	:radius(f(424,100))      -- its event horizon, 4.24 Sol radii; the game sets it from the mass anyway
	:mass(f(1000000,1))

local a = CustomSystemBody:new('Black Hole Test a', 'PLANET_TERRESTRIAL')
	:seed(31801)
	:radius(f(9,10))
	:mass(f(7,10))
	:temp(40)
	:semi_major_axis(f(2,1))
	:eccentricity(f(2,100))
	:inclination(math.deg2rad(4))
	:rotation_period(f(3,1))
	:axial_tilt(fixed.deg2rad(f(10,1)))
	:metallicity(f(9,10))
	:volcanicity(f(1,10))
	:atmos_density(f(0,1))
	:atmos_oxidizing(f(0,1))
	:ocean_cover(f(0,1))
	:ice_cover(f(5,10))
	:life(f(0,1))
	:orbital_phase_at_start(fixed.deg2rad(f(70,1)))

local a_station = CustomSystemBody:new('Event Horizon Watch', 'STARPORT_ORBITAL')
	:semi_major_axis(f(1,10000))
	:rotation_period(f(11,24))

local b = CustomSystemBody:new('Black Hole Test b', 'PLANET_GAS_GIANT')
	:seed(31802)
	:radius(f(8,1))
	:mass(f(90,1))
	:temp(30)
	:semi_major_axis(f(5,1))
	:eccentricity(f(3,100))
	:inclination(math.deg2rad(2))
	:rotation_period(f(5,10))
	:axial_tilt(fixed.deg2rad(f(20,1)))
	:orbital_phase_at_start(fixed.deg2rad(f(200,1)))

s:bodies(hole, {
	a,
	{
		a_station,
	},
	b,
})

s:add_to_sector(0,0,0,v(0.750,0.600,0.200))
