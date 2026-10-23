// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "SurfaceProfile.h"

#include "MathUtil.h"
#include "Random.h"
#include "core/Log.h"
#include "../galaxy/SystemBody.h"

#include <algorithm>
#include <cmath>

namespace {
	double SmoothStep(const double edge0, const double edge1, const double x)
	{
		const double t = Clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	const SystemBody *FindStar(const SystemBody *body)
	{
		for (const SystemBody *b = body->GetParent(); b; b = b->GetParent()) {
			if (b->GetSuperType() == SystemBody::SUPERTYPE_STAR)
				return b;
		}
		return nullptr;
	}

	// hue of photosynthetic pigments, in degrees: cooler stars favour darker,
	// redder pigments that absorb more of their light
	double VegetationHueForStar(const double starTempK)
	{
		if (starTempK < 3900.0) return 10.0; // M: maroon
		if (starTempK < 5300.0) return 55.0; // K: olive
		if (starTempK < 6000.0) return 95.0; // G: green
		if (starTempK < 7500.0) return 135.0; // F: blue-green
		return 165.0; // A and hotter: teal
	}

	// hue in degrees, saturation and value in [0, 1]
	vector3d HSVToRGB(double h, double s, double v)
	{
		h = std::fmod(h, 360.0);
		if (h < 0.0) h += 360.0;
		s = Clamp(s, 0.0, 1.0);
		v = Clamp(v, 0.0, 1.0);
		const double c = v * s;
		const double x = c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0));
		const double m = v - c;
		vector3d rgb;
		if (h < 60.0) rgb = vector3d(c, x, 0.0);
		else if (h < 120.0) rgb = vector3d(x, c, 0.0);
		else if (h < 180.0) rgb = vector3d(0.0, c, x);
		else if (h < 240.0) rgb = vector3d(0.0, x, c);
		else if (h < 300.0) rgb = vector3d(x, 0.0, c);
		else rgb = vector3d(c, 0.0, x);
		return rgb + vector3d(m);
	}

	// 8 colours around a hue, with jitter in hue and ranges for saturation and value
	void ColourFamily(Random &rand, vector3d out[8], const double hue, const double hueJitter,
		const double satMin, const double satMax, const double valMin, const double valMax)
	{
		for (int i = 0; i < 8; i++) {
			const double h = hue + rand.Double(-hueJitter, hueJitter);
			const double s = rand.Double(satMin, satMax);
			const double v = rand.Double(valMin, valMax);
			out[i] = HSVToRGB(h, s, v);
		}
	}
} // namespace

SurfaceProfile::SurfaceProfile(const SystemBody *body)
{
	const SystemBody *star = FindStar(body);
	if (star)
		starTempK = std::max(1000, star->GetAverageTemp());

	const double temp = body->GetAverageTemp();
	const double metallicity = Clamp(body->GetMetallicity(), 0.0, 1.0);
	const double liquid = Clamp(body->GetVolatileLiquid(), 0.0, 1.0);
	const double ices = Clamp(body->GetVolatileIces(), 0.0, 1.0);
	const double life = Clamp(body->GetLife(), 0.0, 1.0);
	const double volcanicity = Clamp(body->GetVolcanicity(), 0.0, 1.0);
	const double oxidizing = Clamp(body->GetAtmosOxidizing(), 0.0, 1.0);
	const bool hasAtmosphere = body->HasAtmosphere();
	const double atmosDensity = hasAtmosphere ? body->GetVolatileGas() : 0.0; // kg/m^3, Earth = 1.225

	// Equator to pole temperature spread: about 50 K for an Earth-like
	// atmosphere, more when there is little air to carry heat polewards.
	// Axial tilt also spreads heat towards the poles.
	const double heatTransport = atmosDensity / (atmosDensity + 1.0);
	const double spread = 80.0 * (1.0 - 0.5 * heatTransport) * (1.0 - 0.4 * std::sin(std::fabs(body->GetAxialTilt())));
	equatorTempK = temp + 0.35 * spread;
	poleTempK = temp - 0.65 * spread;

	// dry adiabatic lapse rate g / cp, as used by SystemBody::GetAtmAverageTemp
	lapseRateKperM = hasAtmosphere ? body->CalcSurfaceGravity() / 1000.5 : 0.0;
	snowLineM = (lapseRateKperM > 0.0 && equatorTempK > 273.0) ? (equatorTempK - 273.0) / lapseRateKperM : -1.0;
	iceCapLatitude = 90.0 * Clamp((equatorTempK - 263.0) / std::max(1.0, equatorTempK - poleTempK), 0.0, 1.0);
	moistureBase = liquid * SmoothStep(240.0, 280.0, temp) * (1.0 - SmoothStep(330.0, 380.0, temp));

	// iron oxides need both iron and an oxidising atmosphere (Mars)
	ironOxide = metallicity * SmoothStep(0.2, 0.7, oxidizing);
	icyCrust = ices > 0.3 && temp < 230.0;
	albedoBase = Clamp(0.12 + 0.15 * (1.0 - metallicity) + (icyCrust ? 0.35 : 0.0), 0.05, 0.6);

	// without much life the "plant" colours become lichen and soil tones
	vegetationCover = life;
	vegetationHue = 30.0 + (VegetationHueForStar(starTempK) - 30.0) * SmoothStep(0.0, 0.3, life);

	if (volcanicity < 0.1) {
		volcanism = VOLCANISM_NONE;
	} else if (icyCrust) {
		volcanism = VOLCANISM_CRYO;
	} else if (body->IsMoon() && body->GetParent() && body->GetParent()->GetSuperType() == SystemBody::SUPERTYPE_GAS_GIANT &&
		volcanicity > 0.5 && ices < 0.2) {
		// tidally heated moon of a gas giant (Io)
		volcanism = VOLCANISM_SULPHUR;
	} else {
		volcanism = VOLCANISM_SILICATE;
	}
	lavaCover = (volcanism == VOLCANISM_SILICATE || volcanism == VOLCANISM_SULPHUR) ? volcanicity * volcanicity : 0.0;
	// resurfaced by volcanism, worn down by liquid erosion
	surfaceAge = Clamp(1.0 - 0.8 * volcanicity - 0.3 * liquid, 0.05, 1.0);
}

void SurfaceProfile::MakePalette(const uint32_t bodySeed, Palette &out) const
{
	// separate generator so the terrain's own random sequence is unchanged
	Random rand(bodySeed ^ 0x51ab0f17u);

	double rockHue = 28.0 - 14.0 * ironOxide;
	double rockSat = 0.06 + 0.32 * ironOxide;
	if (icyCrust) {
		// cold icy crusts: bluish grey
		rockHue = 210.0;
		rockSat *= 0.5;
	}
	const double sandHue = 40.0 - 18.0 * ironOxide;
	const double vegSat = 0.35 + 0.4 * vegetationCover;
	// dark pigments around red dwarfs
	const double vegVal = (starTempK < 3900.0) ? 0.75 : 1.0;

	ColourFamily(rand, out.rock, rockHue, 8.0, rockSat * 0.6, rockSat * 1.2, 0.35, 0.8);
	ColourFamily(rand, out.darkrock, rockHue, 8.0, rockSat * 0.5, rockSat, 0.1, 0.32);
	ColourFamily(rand, out.plant, vegetationHue, 12.0, vegSat * 0.8, vegSat, 0.3 * vegVal, 0.6 * vegVal);
	ColourFamily(rand, out.darkplant, vegetationHue, 12.0, vegSat, vegSat * 1.2, 0.12 * vegVal, 0.3 * vegVal);
	ColourFamily(rand, out.sand, sandHue, 6.0, 0.25, 0.45, 0.6, 0.85);
	ColourFamily(rand, out.darksand, sandHue, 6.0, 0.3, 0.5, 0.2, 0.45);
	ColourFamily(rand, out.dirt, 30.0, 8.0, 0.25, 0.45, 0.35, 0.6);
	ColourFamily(rand, out.darkdirt, 30.0, 8.0, 0.3, 0.5, 0.1, 0.28);
}

void SurfaceProfile::Dump() const
{
	static const char *volcanismNames[] = { "none", "silicate", "sulphur", "cryo" };
	Output("  Surface profile:\n");
	Output("    star %.0f K, equator %.0f K, pole %.0f K, lapse %.4f K/m, snow line %.0f m, ice caps from %.0f deg\n",
		starTempK, equatorTempK, poleTempK, lapseRateKperM, snowLineM, iceCapLatitude);
	Output("    moisture %.2f, iron oxide %.2f, albedo %.2f, icy crust %d\n", moistureBase, ironOxide, albedoBase, icyCrust ? 1 : 0);
	Output("    vegetation %.2f (hue %.0f), volcanism %s (lava %.2f), surface age %.2f\n",
		vegetationCover, vegetationHue, volcanismNames[volcanism], lavaCover, surfaceAge);
}
