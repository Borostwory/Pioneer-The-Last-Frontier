// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Screen-space cloud pass parameters, filled by PlanetClouds::RenderCloudPasses
// (src/PlanetClouds.cpp). Must match CloudPassBlock.
layout(std140) uniform CloudPassData {
	vec4 cloudPassCenter;	  // xyz: planet centre in view space [planet radii]
	vec4 cloudPassProj;		  // x: P[0], y: P[5] (1 / tan of the half fov), z: near plane [m], w: composite divisor
	vec4 cloudPassSize;		  // xy: size of the buffer the composite reads, zw: full size [pixels]
	vec4 cloudPassAtmos;	  // rgb: atmosphere colour, w: softness 0..2
	vec4 cloudPassMarch;	  // xy: march buffer size, z: full pixels per history pixel, w: sample footprint [full pixels]
	vec4 cloudPassFrame;	  // xy: checkerboard offset in history pixels (0.5 = block centre), z: history valid (0/1), w: apply cloud shadows (0/1)
	vec4 cloudPassPrevCenter; // previous frame: planet centre in view space [planet radii]
	vec4 cloudPassPrevAxisX;  // previous frame: planet axes in view space
	vec4 cloudPassPrevAxisY;
	vec4 cloudPassPrevAxisZ;
	vec4 cloudPassPrevProj;	  // previous frame: x P[0], y P[5]
	vec4 cloudPassHistSize;	  // xy: history buffer size, zw: shadow buffer size
};
