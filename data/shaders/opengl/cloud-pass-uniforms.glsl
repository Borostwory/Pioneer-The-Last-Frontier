// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Screen-space cloud pass parameters, filled by PlanetClouds::RenderCloudPasses
// (src/PlanetClouds.cpp). Must match CloudPassBlock.
layout(std140) uniform CloudPassData {
	vec4 cloudPassCenter; // xyz: planet centre in view space [planet radii]
	vec4 cloudPassProj;	  // x: P[0], y: P[5] (1 / tan of the half fov), z: near plane [m], w: resolution divisor
	vec4 cloudPassSize;	  // xy: cloud buffer size, zw: full size [pixels]
	vec4 cloudPassAtmos;  // rgb: atmosphere colour, w: softness 0..2
};
