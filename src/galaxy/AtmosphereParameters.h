#ifndef ATMOSPHEREPARAMETERS_H_INCLUDED
#define ATMOSPHEREPARAMETERS_H_INCLUDED

#include "Color.h"
#include "vector3.h"

#define DENSITY_STEPS 255

struct AtmosphereParameters {
	float atmosRadius;
	float atmosInvScaleHeight;
	float atmosDensity;
	float planetRadius;
	Color atmosCol;
	vector3d center;
	vector2f logDensityMap[DENSITY_STEPS + 1];
	float logDensityMapR[(DENSITY_STEPS + 1)*(DENSITY_STEPS + 1)];
	float logDensityMapM[(DENSITY_STEPS + 1)*(DENSITY_STEPS + 1)];

	// The body's own scattering, as multiples of the Earth values in rayleigh-lib.glsl
	// (betaR, betaM, the haze's g). The defaults are Earth's. See
	// SystemBody::CalcAtmosphereParams and docs/implementation_plans/atmosphere-types_plan.md.
	float rayleighScale[3] = { 1.0f, 1.0f, 1.0f };	 // the gas, and its star's light
	float hazeScatter[3] = { 1.0f, 1.0f, 1.0f };	 // the haze's scattering
	float hazeExtinction[3] = { 1.0f, 1.0f, 1.0f };	 // its scattering and absorption
	float hazeGlow[3] = { 0.76f, 0.76f, 0.76f };	 // its forward scattering (the shader's g)
	float gasAbsorb[3] = { 0.0f, 0.0f, 0.0f };		 // colour the gas absorbs, in 1e-6 like betaR
	float thickAir = 0.0f;							 // 0..1: light bouncing many times in thick air
	float hazeGlowFromAbove = 0.0f;					 // 0..1: a thick haze's many-bounce glow seen from above (smog)
	bool earthValues = true;						 // all of the above are Earth's exactly
};

#endif // ATMOSPHEREPARAMETERS_H_INCLUDED
