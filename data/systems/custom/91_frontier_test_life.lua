-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- For working on life in the physical surface generator: living worlds around stars of each colour, in the same sector
-- as Frontier Test Worlds, which has those around a star like the Sun. Each lies where its star keeps it mild.

local function system(name, starType, radius, mass, temperature, position, bodies)
	local s = CustomSystem:new(name, { starType })
		:govtype('CISSOCDEM')
		:short_desc('Test worlds for life on the surface generator')
		:long_desc([[Living worlds around a star of this colour, for testing the physical surface generator.]])
		:seed(20261106)
	local star = CustomSystemBody:new(name, starType)
		:radius(radius)
		:mass(mass)
		:temp(temperature)
	s:bodies(star, bodies)
	s:add_to_sector(0, 1, 0, position)
end

local function world(name, seed, orbit, phase, temperature, ocean, ice, life, volcanicity)
	return CustomSystemBody:new(name, 'PLANET_TERRESTRIAL')
		:seed(seed)
		:radius(f(1,1))
		:mass(f(1,1))
		:temp(temperature)
		:semi_major_axis(orbit)
		:eccentricity(f(1,100))
		:orbital_phase_at_start(fixed.deg2rad(f(phase,1)))
		:rotation_period(f(1,1))
		:axial_tilt(fixed.deg2rad(f(20,1)))
		:metallicity(f(1,2))
		:volcanicity(volcanicity)
		:atmos_density(f(1,1))
		:atmos_oxidizing(f(9,10))
		:ocean_cover(ocean)
		:ice_cover(ice)
		:life(life)
end

system('Test Life M', 'STAR_M', f(4,10), f(4,10), 3300, v(0.200,0.300,0.200), {
	world('Test Red Forest', 410021, f(13,100), 0, 288, f(6,10), f(3,10), f(9,10), f(1,10)),
	world('Test Red Lichen', 410022, f(17,100), 90, 279, f(35,100), f(3,10), f(2,10), f(1,10)),
})

system('Test Life K', 'STAR_K', f(75,100), f(75,100), 4600, v(0.800,0.300,0.200), {
	world('Test Amber', 420031, f(48,100), 40, 290, f(5,10), f(3,10), f(8,10), f(2,10)),
	world('Test Amber Steppe', 420032, f(55,100), 200, 296, f(25,100), f(2,10), f(5,10), f(1,10)),
})

system('Test Life F', 'STAR_F', f(13,10), f(13,10), 6700, v(0.200,0.300,0.800), {
	world('Test Blue Jungle', 430041, f(16,10), 10, 302, f(85,100), f(1,10), f(1,1), f(6,10)),
	world('Test Pale Moss', 430042, f(20,10), 120, 281, f(3,10), f(3,10), f(25,100), f(1,10)),
})

system('Test Life A', 'STAR_A', f(19,10), f(19,10), 8500, v(0.800,0.300,0.800), {
	world('Test Cyan', 440051, f(42,10), 70, 292, f(5,10), f(2,10), f(7,10), f(3,10)),
})
