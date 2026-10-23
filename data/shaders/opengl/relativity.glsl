// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Special relativity helpers for the hyperspace sky: aberration, Doppler
// shift of blackbody colours, and the cosmic microwave background.
// Must match RelativityData in src/Background.cpp.

layout(std140) uniform RelativityData {
	vec4 u_axisBeta; // xyz: direction of travel in view space, w: beta
	vec4 u_lorentz;	 // x: gamma, y: 1 - beta, z: exposure, w: angular size of a pixel (rad)
	vec4 u_cmb;		 // x: CMB radiance -> pixel value, y: CMB temperature at rest, z: angular radius of the core disc, w: max sprite growth
	vec4 u_cmbCore;	 // rgb: pixel value of the CMB inside the core disc, w: unused
	vec4 u_glow;	 // rgb: light spread into the glow around the bright point (pixel units), w: unused
};

const vec3 CHANNEL_WAVELENGTH = vec3(610e-9, 550e-9, 465e-9);
const float HC_OVER_K = 1.438777e-2;
const float WHITE_TEMPERATURE = 5800.0;
const float HUE_TEMPERATURE_LIMIT = 40000.0;
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

// h c / (lambda k T) per channel
vec3 planckX(float T)
{
	return HC_OVER_K / (CHANNEL_WAVELENGTH * T);
}

// exp(x) - 1 that neither overflows nor loses precision for tiny x
vec3 expm1Safe(vec3 x)
{
	x = min(x, vec3(80.0));
	return mix(exp(x) - 1.0, x + 0.5 * x * x, lessThan(x, vec3(1e-3)));
}

// Planck spectral radiance per channel, in W / (m^2 sr nm)
vec3 planckRadiance(float T)
{
	return (1.191042972e-25 / pow(CHANNEL_WAVELENGTH, vec3(5.0))) / expm1Safe(planckX(max(T, 1e-3)));
}

// B(observedT) / B(restT) per channel
vec3 planckRatio(float observedT, float restT)
{
	return expm1Safe(planckX(restT)) / expm1Safe(planckX(max(observedT, 1e-3)));
}

// physical channel flux * whiteBalance() = display value; a 5800 K body is grey
vec3 whiteBalance()
{
	vec3 x = planckX(WHITE_TEMPERATURE);
	return pow(CHANNEL_WAVELENGTH / CHANNEL_WAVELENGTH.g, vec3(5.0)) * (exp(x) - 1.0) / (exp(x.g) - 1.0);
}

// Rest temperature of a blackbody with the same blue/red ratio as this linear colour
float estimateTemperature(vec3 linearColor)
{
	vec3 phys = linearColor / whiteBalance();
	float target = phys.b / max(phys.r, 1e-9);
	float lo = log(2000.0);
	float hi = log(50000.0);
	for (int i = 0; i < 14; i++) {
		float mid = 0.5 * (lo + hi);
		vec3 x = planckX(exp(mid));
		float ratio = pow(CHANNEL_WAVELENGTH.r / CHANNEL_WAVELENGTH.b, 5.0) * (exp(x.r) - 1.0) / (exp(x.b) - 1.0);
		if (ratio < target)
			lo = mid;
		else
			hi = mid;
	}
	return exp(0.5 * (lo + hi));
}

// Colour of a blackbody source of rest temperature T, seen with Doppler factor D.
// The hue stops changing above HUE_TEMPERATURE_LIMIT but the brightness keeps the true value.
vec3 dopplerShift(vec3 linearColor, float T, float D)
{
	float observedT = T * D;
	vec3 trueRatio = planckRatio(observedT, T);
	vec3 hueRatio = planckRatio(min(observedT, HUE_TEMPERATURE_LIMIT), T);
	float lumTrue = dot(linearColor * trueRatio, LUMA);
	float lumHue = dot(linearColor * hueRatio, LUMA);
	if (lumHue < 1e-30)
		return vec3(0.0);
	return linearColor * hueRatio * (lumTrue / lumHue);
}

// A source seen in rest-frame direction n by an observer moving along v.
// Returns the observed direction; doppler = observed / emitted frequency.
vec3 aberrate(vec3 n, vec3 v, float gamma, float oneMinusBeta, out float doppler)
{
	float beta = 1.0 - oneMinusBeta;
	float cosTheta = dot(n, v);
	vec3 perp = n - v * cosTheta;
	float sinTheta = length(perp);
	// 1 + cos and 1 - cos without cancellation
	float onePlusCos = 0.5 * dot(n + v, n + v);

	// D = gamma (1 + beta cos(theta)), written so that it stays accurate behind the ship
	doppler = max(gamma * (onePlusCos - oneMinusBeta * cosTheta), 1e-12);

	if (sinTheta < 1e-12)
		return n;

	// observed angle: sin' = sin / D, cos' = (cos + beta) / (1 + beta cos)
	float sinObs = sinTheta / doppler;
	float cosObs = (onePlusCos - oneMinusBeta) / (onePlusCos - oneMinusBeta * cosTheta);
	return normalize(v * cosObs + (perp / sinTheta) * sinObs);
}

// Identity for normal brightness, rolls off smoothly instead of clipping above it
vec3 softClip(vec3 x)
{
	const float knee = 0.8;
	vec3 over = max(x - knee, 0.0);
	return min(x, vec3(knee)) + (1.0 - knee) * (1.0 - exp(-over / (1.0 - knee)));
}
