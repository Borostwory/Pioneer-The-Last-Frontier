// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainMaterials.h"

#include <algorithm>
#include <cassert>
#include <cmath>

int TerrainMaterialTable::AddSlot(const Layer layer)
{
	const int slot = SlotOf(layer);
	if (slot >= 0)
		return slot;
	assert(m_slotCount < SLOT_COUNT);
	if (m_slotCount >= SLOT_COUNT)
		return m_slotCount - 1;
	m_layers[m_slotCount] = layer;
	return m_slotCount++;
}

void TerrainMaterialTable::Add(const Layer layer, std::initializer_list<vector3d> colours)
{
	const int slot = AddSlot(layer);
	for (const vector3d &c : colours)
		m_families.push_back({ Feature(c), slot });
}

void TerrainMaterialTable::Add(const Layer layer, const vector3d *colours, const int count)
{
	const int slot = AddSlot(layer);
	for (int i = 0; i < count; i++)
		m_families.push_back({ Feature(colours[i]), slot });
}

int TerrainMaterialTable::SlotOf(const Layer layer) const
{
	for (int i = 0; i < m_slotCount; i++) {
		if (m_layers[i] == layer)
			return i;
	}
	return -1;
}

int TerrainMaterialTable::RockSlot() const
{
	const int rock = SlotOf(ROCK);
	return rock >= 0 ? rock : SlotOf(BASALT);
}

int TerrainMaterialTable::BeachSlot() const
{
	const int sand = SlotOf(SAND);
	return sand >= 0 ? sand : SlotOf(SALT);
}

// brightness and two colour-difference axes, the colour differences weighted
// up so hue and saturation count about as much as brightness
vector3d TerrainMaterialTable::Feature(const vector3d &colour)
{
	const double r = std::clamp(colour.x, 0.0, 1.0);
	const double g = std::clamp(colour.y, 0.0, 1.0);
	const double b = std::clamp(colour.z, 0.0, 1.0);
	return vector3d(0.3 * r + 0.59 * g + 0.11 * b, 1.5 * (r - g), 0.75 * (r + g - 2.0 * b));
}

void TerrainMaterialTable::Classify(const vector3d &colour, uint8_t weightsOut[SLOT_COUNT]) const
{
	std::fill(weightsOut, weightsOut + SLOT_COUNT, 0);
	if (m_families.empty()) {
		if (m_slotCount > 0)
			weightsOut[0] = 255;
		return;
	}

	// soft nearest family: weights fall off with the distance beyond the
	// nearest one, so a colour blended from two families gets both
	const vector3d f = Feature(colour);
	double nearest = 1e30;
	for (const Family &fam : m_families)
		nearest = std::min(nearest, (fam.feature - f).LengthSqr());

	const double invSigma2 = 1.0 / (0.05 * 0.05);
	double slotWeight[SLOT_COUNT] = {};
	for (const Family &fam : m_families) {
		const double x = ((fam.feature - f).LengthSqr() - nearest) * invSigma2;
		if (x < 9.0) // below that the weight is under 1e-4
			slotWeight[fam.slot] = std::max(slotWeight[fam.slot], std::exp(-x));
	}

	double total = 0.0;
	for (int i = 0; i < m_slotCount; i++)
		total += slotWeight[i];

	// round to bytes adding up to 255
	int sum = 0, largest = 0;
	for (int i = 0; i < m_slotCount; i++) {
		weightsOut[i] = static_cast<uint8_t>(std::floor(255.0 * slotWeight[i] / total + 0.5));
		sum += weightsOut[i];
		if (slotWeight[i] > slotWeight[largest])
			largest = i;
	}
	weightsOut[largest] = static_cast<uint8_t>(std::clamp(int(weightsOut[largest]) + 255 - sum, 0, 255));
}

void TerrainMaterialTable::Single(const int slot, uint8_t weightsOut[SLOT_COUNT]) const
{
	std::fill(weightsOut, weightsOut + SLOT_COUNT, 0);
	if (slot >= 0 && slot < m_slotCount)
		weightsOut[slot] = 255;
	else if (m_slotCount > 0)
		weightsOut[0] = 255;
}
