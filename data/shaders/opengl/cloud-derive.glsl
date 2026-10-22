// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Derives the cloud layers of a planet from the CloudData uniforms.
// See docs/implementation_plans/volumetric-clouds_plan.md, section 4 (R1-R7).
// Requires: cloud-config.glsl, cloud-uniforms.glsl

#define CLOUD_GAS_R 8.3144621
#define CLOUD_MOLAR_MASS 0.02897
#define CLOUD_CP 1000.5
#define CLOUD_DEG (PI / 180.0)

#define CLOUD_SPECIES_NONE 0.0
#define CLOUD_SPECIES_H2SO4 1.0
#define CLOUD_SPECIES_H2O 2.0
#define CLOUD_SPECIES_CH4 3.0
#define CLOUD_SPECIES_NH3 4.0
#define CLOUD_SPECIES_CO2 5.0

#define CLOUD_GENUS_LOW 0.0  // stratus / stratocumulus, chosen per location
#define CLOUD_GENUS_ALTOCUMULUS 1.0
#define CLOUD_GENUS_CIRRUS 2.0

struct CloudLayer {
	float active;      // 0/1
	float base;        // nominal base altitude [m above sea level]
	float top;         // nominal top altitude [m]
	float coverage;    // global coverage 0..1
	float sigmaScale;  // species/phase factor q divided by s_H
	float featureSize; // cell size [m]
	float phase;       // 0 liquid, 1 mixed, 2 ice (H2O only; other species 3)
	vec3 albedo;       // single-scattering tint (linear)
	float omega;       // single-scattering albedo
	float g1;          // forward HG lobe
	float g2;          // backward HG lobe
	float lobeMix;     // weight of the forward lobe
	float period;      // evolution period [s], divides 2^20
	float fNorm;       // area-weighted mean of the latitude factor
};

struct CloudPlanet {
	float enabled;
	float species;
	float a;       // planet radius [m]
	float T0;      // surface temperature [K]
	float P0;      // surface pressure [atm]
	float g;       // gravity [m/s^2]
	float gamma;   // lapse rate [K/m]
	float zt;      // tropopause [m] (0 = isothermal engine model)
	float ztEff;   // tropopause, or 2H for isothermal atmospheres [m]
	float H;       // scale height [m]
	float sH;      // H / 8.4 km, clamped
	float hMax;    // max terrain height [m]
	float phiH;    // Hadley cell edge [rad]
	float phiJ;    // jet latitude [rad]
	float wsr;     // 0 = Earth-like jets, 1 = super-rotation
	float ujet;    // jet speed [m/s]
	float usr;     // super-rotation speed [m/s]
	float spinSign;
	float tidalLock;
	CloudLayer L[3];
};

float cloudSq(float x) { return x * x; }

// engine atmosphere model, SystemBody::GetAtmAverageTemp / GetAtmPressure
float cloudTempAt(CloudPlanet cp, float z)
{
	return cp.T0 - cp.gamma * min(max(z, 0.0), cp.zt);
}

float cloudPressAt(CloudPlanet cp, float z)
{
	float M = CLOUD_MOLAR_MASS;
	if (cp.zt <= 0.0) // isothermal
		return cp.P0 * exp(-max(z, 0.0) / cp.H);
	if (z <= cp.zt)
		return cp.P0 * pow(max(1.0 - cp.gamma * max(z, 0.0) / cp.T0, 1e-4), CLOUD_CP * M / CLOUD_GAS_R);
	float Tt = cloudTempAt(cp, cp.zt);
	float Pt = cp.P0 * pow(max(1.0 - cp.gamma * cp.zt / cp.T0, 1e-4), CLOUD_CP * M / CLOUD_GAS_R);
	return Pt * exp(-cp.g * M * (z - cp.zt) / (CLOUD_GAS_R * max(Tt, 1.0)));
}

// altitude at which temperature T is reached (0 if already colder at the surface, zt if never)
float cloudAltOfTemp(CloudPlanet cp, float T)
{
	if (T >= cp.T0) return 0.0;
	return min((cp.T0 - T) / max(cp.gamma, 1e-9), cp.zt);
}

// Clausius-Clapeyron from the methane triple point (90.69 K, 11696 Pa)
float cloudPsatCH4(float T)
{
	return 11696.0 * exp(-(8190.0 / 8.314) * (1.0 / max(T, 1.0) - 1.0 / 90.69));
}

// Mars CO2 frost point fit, p in Pa
float cloudFrostCO2(float p)
{
	return 3167.8 / (23.23 - log(max(0.01 * p, 1e-6)));
}

// latitude structure of coverage (plan R4.4.2), not normalised
float cloudLatFactorRaw(CloudPlanet cp, float alat, float layer)
{
	float fj = 1.0 + 0.6 * exp(-cloudSq(alat / (8.0 * CLOUD_DEG)))
		- 0.5 * exp(-cloudSq((alat - cp.phiH) / (10.0 * CLOUD_DEG)))
		+ 0.4 * exp(-cloudSq((alat - cp.phiJ - 5.0 * CLOUD_DEG) / (12.0 * CLOUD_DEG)));
	if (layer < 0.5)
		fj += 0.2 * smoothstep(60.0 * CLOUD_DEG, 80.0 * CLOUD_DEG, alat);
	return max(mix(fj, 1.0, cp.wsr), 0.0);
}

void cloudSetLayer(inout CloudLayer l, float base, float top, float coverage, float sigmaScale, float featureSize, float period)
{
	l.active = (coverage > 0.001 && top > base) ? 1.0 : 0.0;
	l.base = base;
	l.top = top;
	l.coverage = min(coverage, 1.0);
	l.sigmaScale = sigmaScale;
	l.featureSize = featureSize;
	l.period = period;
}

void cloudSetOptics(inout CloudLayer l, vec3 albedo, float omega, float g1, float g2, float lobeMix)
{
	l.albedo = albedo;
	l.omega = omega;
	l.g1 = g1;
	l.g2 = g2;
	l.lobeMix = lobeMix;
}

CloudPlanet cloudDerive()
{
	CloudPlanet cp;
	cp.enabled = 0.0;
	cp.species = CLOUD_SPECIES_NONE;
	cp.a = max(cloudGeom.x, 1.0);
	cp.T0 = max(cloudAtmos.x, 1.0);
	cp.P0 = cloudAtmos.y;
	cp.g = max(cloudAtmos.w, 1e-3);
	cp.gamma = cp.g / CLOUD_CP;
	cp.zt = max(cloudGeom.y, 0.0);
	cp.H = CLOUD_GAS_R * cp.T0 / (CLOUD_MOLAR_MASS * cp.g);
	cp.ztEff = cp.zt > 0.0 ? cp.zt : 2.0 * cp.H;
	cp.sH = clamp(cp.H / 8400.0, 0.5, 4.0);
	cp.hMax = cloudGeom.w;
	cp.phiH = 0.5 * PI;
	cp.phiJ = 0.5 * PI;
	cp.wsr = 1.0;
	cp.ujet = 0.0;
	cp.usr = 0.0;
	cp.spinSign = 1.0;
	cp.tidalLock = 0.0;
	for (int i = 0; i < 3; i++) {
		cp.L[i].active = 0.0;
		cp.L[i].base = 0.0;
		cp.L[i].top = 0.0;
		cp.L[i].coverage = 0.0;
		cp.L[i].sigmaScale = 0.0;
		cp.L[i].featureSize = 1000.0;
		cp.L[i].phase = 3.0;
		cp.L[i].albedo = vec3(1.0);
		cp.L[i].omega = 0.9999;
		cp.L[i].g1 = 0.8;
		cp.L[i].g2 = -0.3;
		cp.L[i].lobeMix = 0.7;
		cp.L[i].period = 32768.0;
		cp.L[i].fNorm = 1.0;
	}

	float rho0 = cloudAtmos.z;
	float zAtm = cloudGeom.z;

	// R1: scope (C++ sets enabled only for terrestrial planets), atmosphere, pressure
	if (cloudSpin.w < 0.5 || rho0 <= 0.01 || zAtm <= 0.0 || cp.P0 < 0.005)
		return cp;

	float liq = cloudComp.x;
	float ice = cloudComp.y;
	float ox = cloudComp.z;
	float volc = cloudComp.w;
	float w = liq + 0.2 * ice;
	float m = clamp(w / 0.8, 0.0, 1.25);

	// R2: primary species, first eligible wins (plan 4.2.1)
	float s = CLOUD_SPECIES_NONE;
	if (cp.T0 >= 500.0 && cp.P0 >= 10.0 && volc >= 0.3)
		s = CLOUD_SPECIES_H2SO4;
	else if (w >= 0.02 && cp.T0 >= 230.0 && cp.T0 <= 373.0)
		s = CLOUD_SPECIES_H2O;
	else if (cp.T0 <= 120.0 && cp.P0 >= 0.5)
		s = CLOUD_SPECIES_CH4;
	else if (cp.T0 > 120.0 && cp.T0 <= 240.0 && ox < 0.3 && cp.P0 >= 0.3)
		s = CLOUD_SPECIES_NH3;
	else if (cp.T0 < 250.0 && ox >= 0.5)
		s = CLOUD_SPECIES_CO2;
	if (s == CLOUD_SPECIES_NONE)
		return cp;

	// condensation window (plan 4.2.2)
	bool isothermal = cp.zt <= 0.0;
	bool highOnly = (s == CLOUD_SPECIES_CO2);
	float zb = 0.0;
	float ztop = cp.zt;
	if (isothermal) {
		if (s != CLOUD_SPECIES_H2O && s != CLOUD_SPECIES_CO2)
			return cp;
		zb = CLOUD_FLOOR_M;
		ztop = 2.0 * cp.H;
		highOnly = true;
	} else if (s == CLOUD_SPECIES_H2SO4) {
		zb = cloudAltOfTemp(cp, 370.0);
		ztop = min(cp.zt, cloudAltOfTemp(cp, 230.0));
	} else if (s == CLOUD_SPECIES_H2O) {
		// lifting condensation level (plan 4.3.1)
		float RH = min(0.45 + 0.35 * m, 0.95);
		float Td = 1.0 / (1.0 / cp.T0 - (461.5 / 2.5e6) * log(RH));
		zb = (cp.T0 - Td) / max(cp.gamma * 0.82, 1e-9);
		ztop = cp.zt;
	} else if (s == CLOUD_SPECIES_CH4) {
		zb = -1.0;
		for (int i = 0; i < 32; i++) {
			float z = float(i) * cp.zt / 32.0;
			if (0.05 * cloudPressAt(cp, z) * 101325.0 >= cloudPsatCH4(cloudTempAt(cp, z))) {
				zb = z;
				break;
			}
		}
		if (zb < 0.0)
			return cp;
		ztop = min(cp.zt, cloudAltOfTemp(cp, 60.0));
	} else if (s == CLOUD_SPECIES_NH3) {
		zb = cloudAltOfTemp(cp, 190.0);
		ztop = min(cp.zt, cloudAltOfTemp(cp, 110.0));
	} else { // CO2
		zb = -1.0;
		for (int i = 0; i < 32; i++) {
			float z = float(i) * cp.zt / 32.0;
			float pCO2 = ox * cloudPressAt(cp, z) * 101325.0;
			if (cloudTempAt(cp, z) < cloudFrostCO2(pCO2)) {
				zb = z;
				break;
			}
		}
		if (zb < 0.0)
			return cp;
	}
	zb = max(zb, CLOUD_FLOOR_M);
	ztop = min(ztop, zAtm - 1000.0);
	float span = ztop - zb;
	if (span < 1500.0)
		return cp;

	cp.species = s;
	cp.enabled = 1.0;

	// R7: circulation regime (plan 4.7.1)
	float rotDays = cloudSpin.x;
	float orbDays = cloudSpin.y;
	cp.spinSign = rotDays < 0.0 ? -1.0 : 1.0;
	float omega = abs(rotDays) > 1e-6 ? 2.0 * PI / (abs(rotDays) * 86400.0) : 1e-9;
	float hh = 5.0 * 0.25 * cp.g * cp.ztEff / (3.0 * omega * omega * cp.a * cp.a);
	cp.phiH = min(sqrt(hh), 0.5 * PI);
	cp.wsr = smoothstep(45.0 * CLOUD_DEG, 75.0 * CLOUD_DEG, cp.phiH);
	if (orbDays > 0.0 && abs(abs(rotDays) - orbDays) / orbDays < 0.01) {
		cp.tidalLock = 1.0;
		cp.wsr = 1.0;
	}
	cp.phiJ = min(cp.phiH + 15.0 * CLOUD_DEG, 70.0 * CLOUD_DEG);
	if (cp.phiH < 89.0 * CLOUD_DEG)
		cp.ujet = min(0.3 * omega * cp.a * cloudSq(sin(cp.phiH)) / cos(cp.phiH), 150.0);
	cp.usr = 0.25 * sqrt(cp.g * cp.ztEff * 0.25);

	// R2: slots (plan 4.2.3) and R3 geometry (plan 4.3.2)
	float sH = cp.sH;
	float lowBot = zb, lowTop = zb + 0.14 * span;
	float midBot = zb + 0.18 * span, midTop = zb + 0.45 * span;
	float hiBot = zb + 0.52 * span, hiTop = zb + 0.95 * span;
	if (highOnly) {
		hiBot = zb;
		hiTop = zb + 0.95 * span;
	}

	float dLow = min(800.0 * sH, lowTop - lowBot);
	float dMid = min(500.0 * sH, midTop - midBot);
	float dHigh = min(1500.0 * sH, hiTop - hiBot);
	float midCentre = 0.5 * (midBot + midTop);

	float lowBase = lowBot, lowTopA = lowBot + dLow;
	float midBase = midCentre - 0.5 * dMid, midTopA = midCentre + 0.5 * dMid;
	float hiBase = hiTop - dHigh, hiTopA = hiTop;

	// R4: coverage per species (plan 4.4.1) and optics (plan 4.4.4, 4.5.3)
	float cLow = 0.0, cMid = 0.0, cHigh = 0.0;
	float qLow = 1.0, qMid = 1.0, qHigh = 1.0;
	if (s == CLOUD_SPECIES_H2O) {
		cLow = 0.40 * m;
		cMid = 0.25 * m;
		cHigh = 0.30 * sqrt(m);
		float qW = clamp(m, 0.2, 1.5);
		float temps[3];
		temps[0] = cloudTempAt(cp, 0.5 * (lowBase + lowTopA));
		temps[1] = cloudTempAt(cp, midCentre);
		temps[2] = cloudTempAt(cp, 0.5 * (hiBase + hiTopA));
		for (int i = 0; i < 3; i++) {
			float ph = temps[i] > 253.0 ? 0.0 : (temps[i] > 235.0 ? 1.0 : 2.0);
			cp.L[i].phase = ph;
			if (ph < 0.5)
				cloudSetOptics(cp.L[i], vec3(1.0, 1.0, 1.0), 0.9999, 0.8, -0.3, 0.7);
			else if (ph < 1.5)
				cloudSetOptics(cp.L[i], vec3(0.985, 0.99, 1.0), 0.9999, 0.78, -0.3, 0.75);
			else
				cloudSetOptics(cp.L[i], vec3(0.97, 0.98, 1.0), 0.9999, 0.75, -0.2, 0.85);
		}
		qLow = qW * (cp.L[0].phase < 0.5 ? 1.0 : (cp.L[0].phase < 1.5 ? 0.8 : 0.6));
		qMid = qW * (cp.L[1].phase < 0.5 ? 1.0 : (cp.L[1].phase < 1.5 ? 0.8 : 0.6));
		qHigh = qW * (cp.L[2].phase < 0.5 ? 1.0 : (cp.L[2].phase < 1.5 ? 0.8 : 0.6));
	} else if (s == CLOUD_SPECIES_H2SO4) {
		float v = clamp(1.25 * volc, 0.0, 1.0);
		cLow = v;
		cMid = 0.8 * v;
		cHigh = 0.6 * v;
		for (int i = 0; i < 3; i++)
			cloudSetOptics(cp.L[i], vec3(1.0, 0.95, 0.78), 0.998, 0.7, -0.2, 0.9);
	} else if (s == CLOUD_SPECIES_CH4) {
		cLow = clamp(cp.P0 / 3.0, 0.3, 1.0);
		cMid = 0.10;
		cHigh = 0.08;
		qLow = 0.4;
		qMid = 0.8;
		qHigh = 0.8;
		cloudSetOptics(cp.L[0], vec3(0.85, 0.55, 0.25), 0.95, 0.7, -0.2, 0.9);
		cloudSetOptics(cp.L[1], vec3(0.95, 0.93, 0.90), 0.9999, 0.8, -0.3, 0.7);
		cloudSetOptics(cp.L[2], vec3(0.95, 0.93, 0.90), 0.9999, 0.75, -0.2, 0.85);
	} else if (s == CLOUD_SPECIES_NH3) {
		float k = clamp(cp.P0, 0.3, 1.0);
		cLow = 0.5 * k;
		cMid = 0.35 * k;
		cHigh = 0.3 * k;
		qLow = qMid = qHigh = 0.7;
		for (int i = 0; i < 3; i++)
			cloudSetOptics(cp.L[i], vec3(1.0, 0.98, 0.92), 0.9999, 0.75, -0.2, 0.85);
	} else { // CO2
		cHigh = 0.15 * ox;
		qHigh = 0.3;
		cloudSetOptics(cp.L[2], vec3(1.0), 0.9999, 0.75, -0.2, 0.85);
	}
	if (highOnly) {
		cLow = 0.0;
		cMid = 0.0;
	}

	cloudSetLayer(cp.L[0], lowBase, lowTopA, cLow, qLow / sH, 2000.0 * sH, 32768.0);
	cloudSetLayer(cp.L[1], midBase, midTopA, cMid, qMid / sH, 700.0 * sH, 32768.0);
	cloudSetLayer(cp.L[2], hiBase, hiTopA, cHigh, qHigh / sH, 2500.0 * sH, 65536.0);

	// normalise the latitude factor to an area-weighted mean of 1 (9-point quadrature)
	for (int l = 0; l < 3; l++) {
		float sum = 0.0, wsum = 0.0;
		for (int i = 0; i < 9; i++) {
			float lat = (float(i) + 0.5) / 9.0 * 0.5 * PI;
			float wgt = cos(lat);
			sum += wgt * cloudLatFactorRaw(cp, lat, float(l));
			wsum += wgt;
		}
		cp.L[l].fNorm = max(sum / wsum, 1e-3);
	}

	return cp;
}

// R7: zonal wind [m/s], positive = prograde (plan 4.7.2)
float cloudWind(CloudPlanet cp, float lat, float z)
{
	float alat = abs(lat);
	float trades = alat < cp.phiH ? 0.15 * cp.ujet * cloudSq(cos(PI * alat / (2.0 * cp.phiH))) : 0.0;
	float uj = cp.ujet * exp(-cloudSq((alat - cp.phiJ) / (12.0 * CLOUD_DEG))) - trades;
	float zr = clamp(z / cp.ztEff, 0.0, 1.0);
	float fj = 0.25 + 0.75 * zr;
	float us = cp.usr * cos(lat);
	return mix(uj * fj, us * zr, cp.wsr) * smoothstep(90.0 * CLOUD_DEG, 75.0 * CLOUD_DEG, alat);
}

// R4: local coverage multiplier from latitude and the substellar point
float cloudLatFactor(CloudPlanet cp, int layer, float lat, vec3 pfDir, vec3 sunPf)
{
	float f = cloudLatFactorRaw(cp, abs(lat), float(layer)) / cp.L[layer].fNorm;
	if (cp.tidalLock > 0.5)
		f *= 0.2 + 1.5 * max(0.0, dot(pfDir, sunPf));
	return f;
}
