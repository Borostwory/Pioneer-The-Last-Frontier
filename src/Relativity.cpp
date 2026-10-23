// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Relativity.h"

#include <algorithm>

namespace Relativity {

	// temperature that is displayed as pure white
	static constexpr double WHITE_TEMPERATURE = 5800.0;

	vector3d WhiteBalance()
	{
		vector3d w;
		const double green = PlanckRadiance(CHANNEL_WAVELENGTH[1], WHITE_TEMPERATURE);
		for (int c = 0; c < 3; c++)
			w[c] = green / PlanckRadiance(CHANNEL_WAVELENGTH[c], WHITE_TEMPERATURE);
		return w;
	}

	double EstimateTemperature(const vector3f &linearColor)
	{
		const vector3d w = WhiteBalance();
		const double red = std::max(double(linearColor.x) / w.x, 1e-9);
		const double blue = double(linearColor.z) / w.z;
		const double target = blue / red;

		// the blue/red ratio of a blackbody grows with temperature: bisect in log(T)
		double lo = std::log(2000.0);
		double hi = std::log(50000.0);
		for (int i = 0; i < 20; i++) {
			const double mid = 0.5 * (lo + hi);
			const double T = std::exp(mid);
			const double ratio = PlanckRadiance(CHANNEL_WAVELENGTH[2], T) / PlanckRadiance(CHANNEL_WAVELENGTH[0], T);
			if (ratio < target)
				lo = mid;
			else
				hi = mid;
		}
		return std::exp(0.5 * (lo + hi));
	}

	vector3d CmbFluxInCone(const Kinematics &k, double halfAngle)
	{
		vector3d flux(0.0);
		const double s = std::sin(0.5 * halfAngle);
		const double oneMinusCos = 2.0 * s * s;

		if (k.rapidity < 1e-6) {
			const double solidAngle = 2.0 * M_PI * oneMinusCos;
			for (int c = 0; c < 3; c++)
				flux[c] = solidAngle * PlanckRadiance(CHANNEL_WAVELENGTH[c], CMB_TEMPERATURE);
			return flux;
		}

		// Seen from the ship, light arriving from angle theta to the direction of travel
		// has Doppler factor D = 1 / (gamma (1 - beta cos(theta))): e^rapidity on the
		// axis, falling towards the edge of the cone. The CMB there is a blackbody of
		// temperature CMB_TEMPERATURE * D, and dOmega = 2 pi dD / (gamma beta D^2).
		// Integrate over u = ln(D).
		const double gammaBeta = std::sinh(k.rapidity);
		const double dEdge = 1.0 / (k.gamma * (k.oneMinusBeta + k.beta * oneMinusCos));
		const double uMin = std::log(dEdge);
		const double uMax = k.rapidity;
		if (uMax <= uMin)
			return flux;

		constexpr int STEPS = 256;
		const double du = (uMax - uMin) / STEPS;
		for (int i = 0; i <= STEPS; i++) {
			const double D = std::exp(uMin + i * du);
			const double weight = (i == 0 || i == STEPS) ? 0.5 : 1.0;
			for (int c = 0; c < 3; c++)
				flux[c] += weight * PlanckRadiance(CHANNEL_WAVELENGTH[c], CMB_TEMPERATURE * D) / D;
		}
		return flux * (2.0 * M_PI * du / gammaBeta);
	}

} // namespace Relativity
