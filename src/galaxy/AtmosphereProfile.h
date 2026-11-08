// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef ATMOSPHEREPROFILE_H_INCLUDED
#define ATMOSPHEREPROFILE_H_INCLUDED

#include <string>

// What a body's air is made of, and what floats in it: its main gas, its haze and its
// clouds. Worked out once per body (SystemBody::SetAtmFromParameters), from the body's own
// values or from the atmosGas / atmosHaze / atmosHazeAmount fields of a custom system.
// The sky, the ground light and the clouds all read it, so they agree.
// See docs/implementation_plans/atmosphere-types_plan.md.
namespace AtmosphereProfile {

	enum Gas : int {
		GAS_AIR = 0,   // nitrogen and oxygen: Earth's air, exactly
		GAS_CO2,	   // carbon dioxide (Venus)
		GAS_N2_CH4,	   // nitrogen with a little methane (Titan)
		GAS_CH4,	   // mostly methane
		GAS_N2_NH3,	   // nitrogen with ammonia
		GAS_H2_HE,	   // hydrogen and helium (Jupiter, Saturn)
		GAS_H2_HE_CH4, // hydrogen and helium with methane (Uranus, Neptune)
		GAS_COUNT
	};

	enum Haze : int {
		HAZE_NONE = 0, // Earth's thin haze, exactly
		HAZE_ACID_DECK, // a sulphuric acid deck (Venus)
		HAZE_DUST,		// dust lifted off dry ground
		HAZE_SMOG,		// orange smog made from methane by sunlight (Titan)
		HAZE_ICE,		// thin ice-crystal haze (giants, methane and ammonia worlds)
		HAZE_COUNT
	};

	// the same numbers as CLOUD_SPECIES_* in data/shaders/opengl/cloud-derive.glsl
	enum CloudSpecies : int {
		CLOUD_NONE = 0,
		CLOUD_H2SO4 = 1,
		CLOUD_H2O = 2,
		CLOUD_CH4 = 3,
		CLOUD_NH3 = 4,
		CLOUD_CO2 = 5
	};

	struct GasInfo {
		const char *name;	  // in system data
		const char *label;	  // in the editor
		double molarMass;	  // kg/mol
		double specificHeat;  // J/(kg K), at constant pressure (see SpecificHeat for CO2)
		float scatterPerKg;	  // Rayleigh scattering per kilogram of gas, relative to air
		float absorb[3];	  // colour absorbed (red, green, blue), in units of 1e-6 like the shader's betaR
	};

	struct HazeInfo {
		const char *name;
		const char *label;
		float opticalDepth;	 // straight up through it, in green light, at amount 1
		float heightFactor;	 // haze thins this many times faster than the air with height
		float scatter[3];	 // share of the haze's dimming that is scattering, per colour
		float absorb[3];	 // and that is absorption: together they give the extinction
		float glow[3];		 // forward scattering (the glow around the sun), per colour
	};

	const GasInfo &GetGas(Gas gas);
	const HazeInfo &GetHaze(Haze haze);

	// the specific heat of a gas at a temperature (only carbon dioxide's changes much)
	double SpecificHeat(Gas gas, double temperature);

	// -1 when the name is empty or unknown
	int GasFromName(const std::string &name);
	int HazeFromName(const std::string &name);

	const char *CloudSpeciesName(CloudSpecies species);

	// R2 of cloud-derive.glsl: the cloud species, first eligible wins. Takes the values the
	// cloud shader gets (CloudDataBlock holds floats) and does the shader's float maths, so
	// a body sitting right on a limit gets the same answer the shader used to.
	CloudSpecies CloudSpeciesFor(float averageTemp, float surfacePressure, float oceanCover, float iceCover,
		float oxidizing, float volcanicity);

	struct Profile {
		Gas gas = GAS_AIR;
		Haze haze = HAZE_NONE;
		float hazeAmount = 0.0f; // 0..1
		CloudSpecies cloudSpecies = CLOUD_NONE;
		bool gasFromData = false;
		bool hazeFromData = false;
	};

} // namespace AtmosphereProfile

#endif // ATMOSPHEREPROFILE_H_INCLUDED
