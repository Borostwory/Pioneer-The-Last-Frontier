// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Written by tools/material-preview/material_tool.py.
// Each texture layer, in the order of TerrainMaterialTable::Layer: x turns (0 none, 1 right angles only, 2 all eight),
// y an area with one turn is 2^y tiles across (-1 to 4), z how far turns blend into each other (0.01 sharp to 0.5)
// (plain comparisons: looking these up in a const array costs far more on the GPU)
vec3 materialRotation(int layer)
{
	if (layer == 0) return vec3(2.0, 4.0, 0.50); // rock
	if (layer == 1) return vec3(2.0, 4.0, 0.50); // gravel
	if (layer == 2) return vec3(2.0, 4.0, 0.50); // sand
	if (layer == 3) return vec3(2.0, 2.0, 0.50); // soil
	if (layer == 4) return vec3(2.0, 0.0, 0.25); // grass
	if (layer == 5) return vec3(2.0, 0.0, 0.25); // forest
	if (layer == 6) return vec3(2.0, 0.0, 0.50); // snow
	if (layer == 7) return vec3(2.0, 2.0, 0.50); // ice
	if (layer == 8) return vec3(2.0, 2.0, 0.25); // basalt
	if (layer == 9) return vec3(2.0, 0.0, 0.25); // lava
	if (layer == 10) return vec3(2.0, 2.0, 0.25); // ash
	if (layer == 11) return vec3(2.0, 2.0, 0.50); // regolith
	if (layer == 12) return vec3(2.0, 0.0, 0.25); // salt
	if (layer == 13) return vec3(2.0, 0.0, 0.25); // tundra
	if (layer == 14) return vec3(2.0, 1.0, 0.50); // dirty_ice
	if (layer == 15) return vec3(2.0, 2.0, 0.25); // lava_flow
	if (layer == 16) return vec3(0.0, 0.0, 0.25); // cracked_mud
	if (layer == 17) return vec3(2.0, 1.0, 0.50); // dust
	if (layer == 18) return vec3(2.0, 3.0, 0.50); // metal
	return vec3(2.0, 4.0, 0.25); // forest_far
}
