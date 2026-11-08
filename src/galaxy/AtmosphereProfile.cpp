// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "AtmosphereProfile.h"

#include <algorithm>

namespace AtmosphereProfile {

	// Scattering per kilogram: the per-molecule Rayleigh cross-section from each gas's
	// refractivity (air 293, N2 298, CO2 449, CH4 444, NH3 376, H2 139, He 35, in 1e-6 at
	// STP), squared, with its King factor, divided by the molar mass relative to air's.
	// The absorption values are first guesses, to be tuned in game.
	static const GasInfo s_gases[GAS_COUNT] = {
		// AIR must keep the literal 0.02897 and 1000.5 (GetMolarMass / GetSpecificHeat in
		// SystemBody.cpp) and exactly 1 for scattering, or Earth's sky and clouds change
		{ "air", "Nitrogen, oxygen (air)", 0.02897, 1000.5, 1.0f, { 0.0f, 0.0f, 0.0f } },
		{ "co2", "Carbon dioxide", 0.04401, 1100.0, 1.71f, { 0.0f, 0.0f, 0.0f } },
		{ "n2_ch4", "Nitrogen, methane", 0.02741, 1040.0, 1.14f, { 1.5f, 0.2f, 0.0f } },
		{ "ch4", "Methane", 0.01604, 2220.0, 3.97f, { 60.0f, 8.0f, 0.0f } },
		{ "n2_nh3", "Nitrogen, ammonia", 0.02691, 1100.0, 1.17f, { 0.0f, 0.0f, 0.0f } },
		// as GetMolarMass / GetSpecificHeat give gas giants, so their air doesn't move
		{ "h2_he", "Hydrogen, helium", 0.0023139903, 12950.0, 2.41f, { 0.0f, 0.0f, 0.0f } },
		{ "h2_he_ch4", "Hydrogen, helium, methane", 0.002642, 10500.0, 2.58f, { 25.0f, 4.0f, 0.0f } },
	};

	// HAZE_NONE's values are Earth's: the shader's betaM and g, and the "thins 6.66 times
	// faster than the air" of SystemBody::CalcAtmosphereParams. The others are first
	// guesses, to be tuned in game.
	static const HazeInfo s_hazes[HAZE_COUNT] = {
		{ "none", "None (thin haze)", 0.0f, 6.66f, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 0.76f, 0.76f, 0.76f } },
		// a deck between the 370 K and 230 K heights, like the sulphuric acid clouds
		{ "acid_deck", "Sulphuric acid deck", 3.5f, 0.0f, { 1.0f, 0.97f, 0.85f }, { 0.0f, 0.03f, 0.15f }, { 0.7f, 0.7f, 0.7f } },
		{ "dust", "Dust", 0.6f, 1.0f, { 0.95f, 0.85f, 0.65f }, { 0.05f, 0.15f, 0.35f }, { 0.6f, 0.7f, 0.85f } },
		// (its glow is less forward than real smog's 0.65: light bounces many times in it, which
		// spreads it out, and the shader follows one bounce; at 0.65 Titan seen with the sun
		// behind the viewer came out nearly black)
		// colour fitted to Cassini's natural-colour full disc: golden orange, about 1 : 0.7 : 0.35
		{ "smog", "Orange smog", 3.5f, 0.5f, { 0.97f, 0.88f, 0.65f }, { 0.03f, 0.12f, 0.35f }, { 0.35f, 0.35f, 0.35f } },
		{ "ice", "Ice haze", 0.3f, 1.0f, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 0.7f, 0.7f, 0.7f } },
	};

	const GasInfo &GetGas(Gas gas)
	{
		return s_gases[std::clamp(int(gas), 0, int(GAS_COUNT) - 1)];
	}

	const HazeInfo &GetHaze(Haze haze)
	{
		return s_hazes[std::clamp(int(haze), 0, int(HAZE_COUNT) - 1)];
	}

	double SpecificHeat(Gas gas, double temperature)
	{
		// carbon dioxide's rises from about 850 at 300 K to 1100 at 700 K
		if (gas == GAS_CO2)
			return std::clamp(846.0 + 0.635 * (temperature - 300.0), 735.0, 1200.0);
		return GetGas(gas).specificHeat;
	}

	int GasFromName(const std::string &name)
	{
		for (int i = 0; i < GAS_COUNT; i++)
			if (name == s_gases[i].name)
				return i;
		return -1;
	}

	int HazeFromName(const std::string &name)
	{
		for (int i = 0; i < HAZE_COUNT; i++)
			if (name == s_hazes[i].name)
				return i;
		return -1;
	}

	const char *CloudSpeciesName(CloudSpecies species)
	{
		switch (species) {
		case CLOUD_H2SO4: return "H2SO4";
		case CLOUD_H2O: return "H2O";
		case CLOUD_CH4: return "CH4";
		case CLOUD_NH3: return "NH3";
		case CLOUD_CO2: return "CO2";
		default: return "none";
		}
	}

	CloudSpecies CloudSpeciesFor(float averageTemp, float surfacePressure, float oceanCover, float iceCover,
		float oxidizing, float volcanicity)
	{
		// as cloudDerive: cp.T0 = max(cloudAtmos.x, 1.0), w = liq + 0.2 * ice
		const float T0 = std::max(averageTemp, 1.0f);
		const float P0 = surfacePressure;
		const float w = oceanCover + 0.2f * iceCover;
		if (T0 >= 500.0f && P0 >= 10.0f && volcanicity >= 0.3f)
			return CLOUD_H2SO4;
		else if (w >= 0.02f && T0 >= 230.0f && T0 <= 373.0f)
			return CLOUD_H2O;
		else if (T0 <= 120.0f && P0 >= 0.5f)
			return CLOUD_CH4;
		else if (T0 > 120.0f && T0 <= 240.0f && oxidizing < 0.3f && P0 >= 0.3f)
			return CLOUD_NH3;
		else if (T0 < 250.0f && oxidizing >= 0.5f)
			return CLOUD_CO2;
		return CLOUD_NONE;
	}

} // namespace AtmosphereProfile
