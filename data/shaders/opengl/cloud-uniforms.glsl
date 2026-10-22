// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Per-planet cloud input data, filled by PlanetClouds::FillCloudData (src/PlanetClouds.cpp).
// Must match PlanetClouds::CloudDataBlock.
layout(std140) uniform CloudData {
	vec4 cloudAxisX; // xyz: planet +X axis in view space; w: height map state (0 none, 0.5 baking, 1 ready)
	vec4 cloudAxisY; // xyz: planet +Y (spin) axis in view space
	vec4 cloudAxisZ; // xyz: planet +Z axis in view space
	vec4 cloudAtmos; // average temp [K], surface pressure [atm], surface density [kg/m^3], gravity [m/s^2]
	vec4 cloudGeom;	 // radius [m], tropopause [m], atmosphere top [m], max terrain height [m]
	vec4 cloudComp;	 // volatileLiquid, volatileIces, atmosOxidizing, volcanicity
	vec4 cloudSpin;	 // rotation period [days], orbit period around a star [days] (0 for moons), time accel rate, enabled (0/1)
	vec4 cloudTime;	 // fmod(t, 2^20) [s], floor(t / 2^20), seed & 0xFFFF, seed >> 16
	vec4 cloudTuneDensity; // xyz: user amount multiplier for the low, medium, high layer
	vec4 cloudTuneSize;	   // xyz: user cloud size multiplier for the low, medium, high layer
};

// Equirectangular terrain height in metres (u = longitude, v = latitude)
uniform sampler2D cloudHeightTex;
