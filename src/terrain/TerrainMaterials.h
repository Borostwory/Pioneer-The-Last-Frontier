// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINMATERIALS_H
#define _TERRAINMATERIALS_H

#include "../vector3.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

// Surface materials of a body for the terrain shaders
// (data/shaders/opengl/terrain-materials.glsl).
//
// Each rocky colour fractal lists which colours it paints for which material
// (the "families", mostly its palette arrays). A body uses at most eight
// materials, its slots; every vertex stores a weight per slot, from how close
// its colour is to each family. The shader maps slots to texture layers.
class TerrainMaterialTable {
public:
	// texture layers in data/textures/terrain, in the order of the
	// MATERIAL_* indices in terrain-materials.glsl
	enum Layer : uint8_t {
		ROCK,
		GRAVEL,
		SAND,
		SOIL,
		GRASS,
		FOREST,
		SNOW,
		ICE,
		BASALT,
		LAVA,
		ASH,
		REGOLITH,
		SALT,
		LAYER_COUNT
	};

	static constexpr int SLOT_COUNT = 8;

	// Adds colours the colour fractal paints for a material (none: a slot for
	// materials only the shader places, like rock on steep slopes).
	void Add(Layer layer, std::initializer_list<vector3d> colours = {});
	void Add(Layer layer, const vector3d *colours, int count);

	bool Empty() const { return m_slotCount == 0; }
	int SlotCount() const { return m_slotCount; }
	// texture layer of a slot
	Layer SlotLayer(int slot) const { return m_layers[slot]; }
	// slot of a layer, or -1
	int SlotOf(Layer layer) const;

	// slots the shader moves weight to: bare rock on steep slopes (rock, else
	// basalt), beaches (sand, else salt), glowing lava; -1 where the body has none
	int RockSlot() const;
	int BeachSlot() const;
	int LavaSlot() const { return SlotOf(LAVA); }

	// slot weights (0..255, adding up to 255) for a vertex colour
	void Classify(const vector3d &colour, uint8_t weightsOut[SLOT_COUNT]) const;
	// all weight in one slot (water vertices: the beach)
	void Single(int slot, uint8_t weightsOut[SLOT_COUNT]) const;

private:
	int AddSlot(Layer layer);

	struct Family {
		vector3d feature; // see Feature()
		int slot;
	};
	static vector3d Feature(const vector3d &colour);

	Layer m_layers[SLOT_COUNT] = {};
	int m_slotCount = 0;
	std::vector<Family> m_families;
};

#endif // _TERRAINMATERIALS_H
