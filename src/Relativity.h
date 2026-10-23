// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _RELATIVITY_H
#define _RELATIVITY_H

#include "MathUtil.h"
#include "vector3.h"

#include <cmath>

/*
 * Special relativity helpers for the hyperspace transit.
 *
 * The transit is shown as a 1 g relativistic voyage compressed into the
 * length of the jump sound: the ship accelerates along its nose for
 * PEAK_PROPER_TIME years of ship time, flips, and decelerates for the same amount. Everything the player sees is computed for the true
 * speed at each instant; only the passage of time is compressed.
 */
namespace Relativity {

	// years of ship time spent accelerating (and again decelerating)
	constexpr double PEAK_PROPER_TIME = 11.0;
	// 1 g expressed in units of c per year (9.81 m/s^2 * 1 yr / c)
	constexpr double G_OVER_C = 1.0326;
	// temperature of the cosmic microwave background at rest, in K
	constexpr double CMB_TEMPERATURE = 2.725;

	// wavelengths (m) standing in for the red, green and blue display channels
	constexpr double CHANNEL_WAVELENGTH[3] = { 610e-9, 550e-9, 465e-9 };
	// h c / k, in m K
	constexpr double HC_OVER_K = 1.438777e-2;
	// 2 h c^2, in W m^2 / sr
	constexpr double TWO_HC2 = 1.191042972e-16;

	struct Kinematics {
		double beta = 0.0;		   // v / c
		double oneMinusBeta = 1.0; // 1 - beta, kept separately because beta rounds to 1 near the peak
		double gamma = 1.0;
		double rapidity = 0.0;
	};

	// Transit progress at which the ship flips over and starts to decelerate
	constexpr double FLIP_PROGRESS = 0.5;

	// Speed at the given transit progress (0 = departure, 1 = arrival)
	inline Kinematics FromTransitProgress(double progress)
	{
		Kinematics k;
		const double p = Clamp(progress, 0.0, 1.0);
		if (p <= 0.0)
			return k;

		// ship time spent accelerating so far, or still needed to come to rest after the flip
		const double properTime = (p <= FLIP_PROGRESS ? 2.0 * p : 2.0 - 2.0 * p) * PEAK_PROPER_TIME;
		k.rapidity = G_OVER_C * properTime;

		k.beta = std::tanh(k.rapidity);
		k.gamma = std::cosh(k.rapidity);
		// 1 - tanh(r), without cancellation
		const double e = std::exp(-2.0 * k.rapidity);
		k.oneMinusBeta = 2.0 * e / (1.0 + e);
		return k;
	}

	// Planck spectral radiance B_lambda(T), in W / (m^2 sr nm)
	inline double PlanckRadiance(double lambda, double temperature)
	{
		if (temperature <= 0.0)
			return 0.0;
		const double x = HC_OVER_K / (lambda * temperature);
		if (x > 700.0)
			return 0.0;
		return TWO_HC2 / std::pow(lambda, 5.0) / std::expm1(x) * 1e-9;
	}

	// B_lambda(observedT) / B_lambda(restT): how much brighter a blackbody looks
	// in the given channel once its temperature has been Doppler shifted
	inline double PlanckRatio(double lambda, double observedT, double restT)
	{
		if (observedT <= 0.0)
			return 0.0;
		const double x1 = HC_OVER_K / (lambda * observedT);
		if (x1 > 700.0)
			return 0.0;
		const double x0 = HC_OVER_K / (lambda * restT);
		return std::expm1(x0) / std::expm1(x1);
	}

	// Rest temperature of a blackbody whose blue/red channel ratio matches the
	// given linear colour (after white balance, so a 5800 K body is grey)
	double EstimateTemperature(const vector3f &linearColor);

	// Flux (W m^-2 nm^-1) per channel of the cosmic microwave background inside a
	// cone of the given half-angle around the direction of travel, as seen by
	// the moving observer
	vector3d CmbFluxInCone(const Kinematics &k, double halfAngle);

	// Per-channel multiplier that makes a 5800 K blackbody look white:
	// physical channel flux * WhiteBalance() = display value (green is unchanged)
	vector3d WhiteBalance();

} // namespace Relativity

#endif
