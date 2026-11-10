-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- A test system for the pulsar look, about 7 light years from Sol.
-- The pulsar has the Vela pulsar's spin (89 ms); the planets are loosely modelled on the real pulsar planets of PSR B1257+12.

local s = CustomSystem:new('Pulsar Test', { 'STAR_NEUTRON' })
	:short_desc('A test system around a young pulsar.')
	:long_desc([[A spinning neutron star, about 12 km across and heavier than the Sun, with jets of glowing gas along its spin axis.]])

local pulsar = CustomSystemBody:new('Pulsar Test', 'STAR_NEUTRON')
	:radius(f(12,696000))                 -- 12 km, in Sol radii
	:mass(f(14,10))                       -- 1.4 Sol masses
	:temp(700000)
	:rotation_period(f(89,86400000))      -- 89 ms, in days
	:axial_tilt(fixed.deg2rad(f(30,1)))   -- tilts the jets so they don't run straight up the system's plane

local a = CustomSystemBody:new('Pulsar Test a', 'PLANET_TERRESTRIAL')
	:seed(25701)
	:radius(f(25,100))
	:mass(f(2,100))
	:temp(260)
	:semi_major_axis(f(19,100))
	:eccentricity(f(1,100))
	:inclination(math.deg2rad(2))
	:rotation_period(f(25,1))
	:axial_tilt(fixed.deg2rad(f(3,1)))
	:metallicity(f(9,10))
	:volcanicity(f(1,10))
	:atmos_density(f(0,1))
	:atmos_oxidizing(f(0,1))
	:ocean_cover(f(0,1))
	:ice_cover(f(0,1))
	:life(f(0,1))
	:orbital_phase_at_start(fixed.deg2rad(f(40,1)))

local b = CustomSystemBody:new('Pulsar Test b', 'PLANET_TERRESTRIAL')
	:seed(25702)
	:radius(f(15,10))
	:mass(f(43,10))
	:temp(210)
	:semi_major_axis(f(36,100))
	:eccentricity(f(2,100))
	:inclination(math.deg2rad(1))
	:rotation_period(f(13,10))
	:axial_tilt(fixed.deg2rad(f(18,1)))
	:metallicity(f(8,10))
	:volcanicity(f(3,10))
	:atmos_density(f(1,10))
	:atmos_oxidizing(f(0,1))
	:ocean_cover(f(0,1))
	:ice_cover(f(3,10))
	:life(f(0,1))
	:orbital_phase_at_start(fixed.deg2rad(f(160,1)))

local b_station = CustomSystemBody:new('Pulsar Watch', 'STARPORT_ORBITAL')
	:semi_major_axis(f(1,10000))
	:rotation_period(f(11,24))

local c = CustomSystemBody:new('Pulsar Test c', 'PLANET_TERRESTRIAL')
	:seed(25703)
	:radius(f(15,10))
	:mass(f(39,10))
	:temp(180)
	:semi_major_axis(f(46,100))
	:eccentricity(f(3,100))
	:inclination(math.deg2rad(1))
	:rotation_period(f(9,10))
	:axial_tilt(fixed.deg2rad(f(7,1)))
	:metallicity(f(7,10))
	:volcanicity(f(1,10))
	:atmos_density(f(0,1))
	:atmos_oxidizing(f(0,1))
	:ocean_cover(f(0,1))
	:ice_cover(f(6,10))
	:life(f(0,1))
	:orbital_phase_at_start(fixed.deg2rad(f(280,1)))

local d = CustomSystemBody:new('Pulsar Test d', 'PLANET_GAS_GIANT')
	:seed(25704)
	:radius(f(10,1))
	:mass(f(150,1))
	:temp(90)
	:semi_major_axis(f(2,1))
	:eccentricity(f(4,100))
	:inclination(math.deg2rad(2))
	:rotation_period(f(4,10))
	:axial_tilt(fixed.deg2rad(f(12,1)))
	:orbital_phase_at_start(fixed.deg2rad(f(100,1)))

s:bodies(pulsar, {
	a,
	b,
	{
		b_station,
	},
	c,
	d,
})

s:add_to_sector(0,0,0,v(0.600,0.300,0.600))
