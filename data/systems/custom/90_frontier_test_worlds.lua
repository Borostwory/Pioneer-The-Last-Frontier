-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- For working on the physical surface generator: the worlds of its test table, around a star
-- like the Sun, so that their colours are seen in white light. Air is in Earth's units here. Their orbits
-- matter: how far out a body formed decides whether its crust is rock or ice.

local s = CustomSystem:new('Frontier Test Worlds', { 'STAR_G' })
	:govtype('CISSOCDEM')
	:short_desc('Test worlds for the surface generator')
	:long_desc([[One world for each test of the physical surface generator, around a star like the Sun.]])
	:seed(20261105)

local star = CustomSystemBody:new('Frontier Test Worlds', 'STAR_G')
	:radius(f(1,1))
	:mass(f(1,1))
	:temp(5778)

-- Earth's values, with a surface of its own instead of Earth's map
local earth = CustomSystemBody:new('Test Earth', 'PLANET_TERRESTRIAL')
	:seed(1857401659)
	:radius(f(1,1))
	:mass(f(1,1))
	:temp(288)
	:semi_major_axis(f(1,1))
	:eccentricity(f(1,100))
	:rotation_period(f(1,1))
	:axial_tilt(fixed.deg2rad(f(2344,100)))
	:metallicity(f(1,2))
	:volcanicity(f(1,10))
	:atmos_density(f(1,1))
	:atmos_oxidizing(f(99,100))
	:ocean_cover(f(7,10))
	:ice_cover(f(5,10))
	:life(f(9,10))

local earth_ports = {
	CustomSystemBody:new('Test Base', 'STARPORT_SURFACE')
		:latitude(math.deg2rad(20))
		:longitude(math.deg2rad(30)),
}

-- much water, little land
local ocean = CustomSystemBody:new('Test Ocean', 'PLANET_TERRESTRIAL')
	:seed(731905)
	:radius(f(11,10))
	:mass(f(13,10))
	:temp(292)
	:semi_major_axis(f(11,10))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(50,1)))
	:rotation_period(f(1,1))
	:axial_tilt(fixed.deg2rad(f(15,1)))
	:metallicity(f(1,2))
	:volcanicity(f(2,10))
	:atmos_density(f(12,10))
	:atmos_oxidizing(f(99,100))
	:ocean_cover(f(95,100))
	:ice_cover(f(1,10))
	:life(f(5,10))

-- plate belts and old continents, no sea
local dry = CustomSystemBody:new('Test Dry', 'PLANET_TERRESTRIAL')
	:seed(482113)
	:radius(f(1,1))
	:mass(f(1,1))
	:temp(280)
	:semi_major_axis(f(125,100))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(100,1)))
	:rotation_period(f(1,1))
	:axial_tilt(fixed.deg2rad(f(20,1)))
	:metallicity(f(1,2))
	:volcanicity(f(2,10))
	:atmos_density(f(4,10))
	:atmos_oxidizing(f(5,10))
	:ocean_cover(f(0,1))
	:ice_cover(f(5,100))
	:life(f(0,1))

-- dust, sand in the basins, caps, dry lake floors. This seed gives it highlands and lowlands, and old
-- valleys: the two are a matter of chance on a world like this.
local mars = CustomSystemBody:new('Test Mars', 'PLANET_TERRESTRIAL')
	:seed(90220)
	:radius(f(533,1000))
	:mass(f(107,1000))
	:temp(240)
	:semi_major_axis(f(152,100))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(150,1)))
	:rotation_period(f(1027,1000))
	:axial_tilt(fixed.deg2rad(f(25,1)))
	:metallicity(f(7,10))
	:volcanicity(f(1,10))
	:atmos_density(f(4,100))
	:atmos_oxidizing(f(1,10))
	:ocean_cover(f(0,1))
	:ice_cover(f(3,10))
	:life(f(0,1))

-- a volcanic super-Earth: plates, sea and many volcanoes, some erupting
local volcanic = CustomSystemBody:new('Test Volcanic', 'PLANET_TERRESTRIAL')
	:seed(320771)
	:radius(f(16,10))
	:mass(f(5,1))
	:temp(285)
	:semi_major_axis(f(105,100))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(70,1)))
	:rotation_period(f(1,1))
	:axial_tilt(fixed.deg2rad(f(18,1)))
	:metallicity(f(1,2))
	:volcanicity(f(8,10))
	:atmos_density(f(1,1))
	:atmos_oxidizing(f(99,100))
	:ocean_cover(f(4,10))
	:ice_cover(f(2,10))
	:life(f(5,10))

-- volcanicity 1: lava seas in the lows, with the highest ground standing above them
local lavaWorld = CustomSystemBody:new('Test Lava', 'PLANET_TERRESTRIAL')
	:seed(884213)
	:radius(f(9,10))
	:mass(f(7,10))
	:temp(900)
	:semi_major_axis(f(6,10))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(120,1)))
	:rotation_period(f(3,1))
	:axial_tilt(fixed.deg2rad(f(5,1)))
	:metallicity(f(6,10))
	:volcanicity(f(1,1))
	:atmos_density(f(1,100))
	:atmos_oxidizing(f(1,10))
	:ocean_cover(f(0,1))
	:ice_cover(f(0,1))
	:life(f(0,1))

local venus = CustomSystemBody:new('Test Venus', 'PLANET_TERRESTRIAL')
	:seed(613440353)
	:radius(f(95,100))
	:mass(f(815,1000))
	:temp(735)
	:semi_major_axis(f(72,100))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(200,1)))
	:rotation_period(f(243,1))
	:axial_tilt(fixed.deg2rad(f(26,10)))
	:metallicity(f(1,2))
	:volcanicity(f(8,10))
	:atmos_density(f(53,1))
	:atmos_oxidizing(f(12,100))
	:ocean_cover(f(0,1))
	:ice_cover(f(0,1))
	:life(f(0,1))

local titan = CustomSystemBody:new('Test Titan', 'PLANET_TERRESTRIAL')
	:seed(70417)
	:radius(f(400,1000))
	:mass(f(225,10000))
	:temp(94)
	:semi_major_axis(f(95,10))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(250,1)))
	:rotation_period(f(15945,1000))
	:atmos_density(f(122,10))
	:atmos_oxidizing(f(6,10))
	:ocean_cover(f(0,1))
	:ice_cover(f(0,1))
	:life(f(0,1))

-- an Earth gone cold: a frozen sea
local frozen = CustomSystemBody:new('Test Frozen', 'PLANET_TERRESTRIAL')
	:seed(558821)
	:radius(f(1,1))
	:mass(f(1,1))
	:temp(230)
	:semi_major_axis(f(18,10))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(300,1)))
	:rotation_period(f(1,1))
	:axial_tilt(fixed.deg2rad(f(20,1)))
	:metallicity(f(1,2))
	:volcanicity(f(1,10))
	:atmos_density(f(1,1))
	:atmos_oxidizing(f(99,100))
	:ocean_cover(f(7,10))
	:ice_cover(f(1,1))
	:life(f(0,1))

-- hot enough to melt with no volcanoes to speak of: magma in the lows, among the crust
local magma = CustomSystemBody:new('Test Magma', 'PLANET_TERRESTRIAL')
	:seed(271828)
	:radius(f(1,1))
	:mass(f(1,1))
	:temp(1400)
	:semi_major_axis(f(3,10))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(30,1)))
	:rotation_period(f(5,1))
	:axial_tilt(fixed.deg2rad(f(3,1)))
	:metallicity(f(5,10))
	:volcanicity(f(2,10))
	:atmos_density(f(1,100))
	:atmos_oxidizing(f(1,10))
	:ocean_cover(f(0,1))
	:ice_cover(f(0,1))
	:life(f(0,1))

-- hotter still, close to its star: a sea of magma with rafts of crust. No tides, so the heat is the star's
local molten = CustomSystemBody:new('Test Molten', 'PLANET_TERRESTRIAL')
	:seed(314159)
	:radius(f(13,10))
	:mass(f(25,10))
	:temp(2600)
	:semi_major_axis(f(6,100))
	:eccentricity(f(0,1))
	:orbital_phase_at_start(fixed.deg2rad(f(160,1)))
	:rotation_period(f(5,1))
	:axial_tilt(fixed.deg2rad(f(1,1)))
	:metallicity(f(5,10))
	:volcanicity(f(2,10))
	:atmos_density(f(1,100))
	:atmos_oxidizing(f(1,10))
	:ocean_cover(f(0,1))
	:ice_cover(f(0,1))
	:life(f(0,1))

-- a small rocky body, about as big as Phobos
local asteroid = CustomSystemBody:new('Test Asteroid', 'PLANET_ASTEROID')
	:seed(161803)
	:radius(f(2,1000))
	:mass(f(2,1000000000))
	:temp(170)
	:semi_major_axis(f(24,10))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(210,1)))
	:rotation_period(f(1,2))
	:axial_tilt(fixed.deg2rad(f(30,1)))
	:metallicity(f(3,10))

-- rich in metal, which shows in patches and young craters
local metalAsteroid = CustomSystemBody:new('Test Metal Asteroid', 'PLANET_ASTEROID')
	:seed(141421)
	:radius(f(8,1000))
	:mass(f(3,10000000))
	:temp(165)
	:semi_major_axis(f(27,10))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(270,1)))
	:rotation_period(f(1,3))
	:axial_tilt(fixed.deg2rad(f(40,1)))
	:metallicity(f(95,100))

-- formed cold and so light it's mostly empty: a sponge of holes. Size and mass of Hyperion
local hyperion = CustomSystemBody:new('Test Hyperion', 'PLANET_ASTEROID')
	:seed(173205)
	:radius(f(212,10000))
	:mass(f(94,100000000))
	:temp(93)
	:semi_major_axis(f(96,10))
	:eccentricity(f(1,100))
	:orbital_phase_at_start(fixed.deg2rad(f(330,1)))
	:rotation_period(f(13,1))
	:axial_tilt(fixed.deg2rad(f(50,1)))
	:metallicity(f(2,10))

s:bodies(star, {
	magma,
	molten,
	asteroid,
	metalAsteroid,
	hyperion,
	earth,
		earth_ports,
	ocean,
	dry,
	mars,
	venus,
	titan,
	frozen,
	volcanic,
	lavaWorld,
})

s:add_to_sector(0,1,0,v(0.500,0.100,0.500))
