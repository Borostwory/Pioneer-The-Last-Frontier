int _iSamples = 16;
int _oSamples = 8;

// ---- The body's scattering (basesphere_uniforms.glsl) ----
// Earth's values. A body whose values are all Earth's (atmosRayleigh.w) takes exactly the
// sums this file always did; the rest scale them by their gas, their star's light and their
// haze. See docs/implementation_plans/atmosphere-types_plan.md.
#define ATMOS_BETA_R0 (1e-6 * vec3(3.8, 13.5, 33.1))
#define ATMOS_BETA_M0 (1e-6 * vec3(21.0))
#define ATMOS_MIE_G0 0.76f

bool atmosEarthValues()
{
	return atmosRayleigh.w > 0.5;
}

// Rayleigh scattering
vec3 atmosBetaR()
{
	return atmosEarthValues() ? ATMOS_BETA_R0 : ATMOS_BETA_R0 * atmosRayleigh.rgb;
}

// haze scattering (its extinction is in atmosViewTransmittance)
vec3 atmosBetaM()
{
	return atmosEarthValues() ? ATMOS_BETA_M0 : ATMOS_BETA_M0 * atmosHazeScatter.rgb;
}

// the haze's phase function, per colour (a haze can glow bluer around the sun, like dust)
vec3 atmosMiePhase(const in float mu)
{
	if (atmosEarthValues())
		return vec3(miePhaseFunction(ATMOS_MIE_G0, mu));
	return vec3(miePhaseFunction(atmosHazeGlow.r, mu), miePhaseFunction(atmosHazeGlow.g, mu), miePhaseFunction(atmosHazeGlow.b, mu));
}

// How much of a straight beam gets through air and haze of the given density along a path
// (x: air, y: haze, as in the density tables): what is scattered or absorbed is gone from
// it. Used for what is seen through the air (the view ray).
vec3 atmosViewTransmittance(const in vec2 density)
{
	if (atmosEarthValues())
		return exp(-(ATMOS_BETA_R0 * density.x + ATMOS_BETA_M0 * density.y));

	vec3 tauR = ATMOS_BETA_R0 * atmosRayleigh.rgb * density.x;
	vec3 tauMe = ATMOS_BETA_M0 * atmosHazeExtinction.rgb * density.y;
	vec3 tauGas = 1e-6 * atmosGasAbsorb.rgb * density.x;
	return exp(-(tauR + tauMe + tauGas));
}

// How much sunlight reaches a point through the given density. In thick air
// (atmosHazeScatter.w) light that is scattered again and again still gets there, about as
// 1 / (1 + 3/4 (1 - g) tau) rather than exp(-tau): it keeps some green and yellow where a
// single bounce leaves only red. Absorbed light is lost either way. (Only for the light
// arriving: the view ray takes atmosViewTransmittance, or the sky would count that bounced
// light again at every step and burn out white.)
vec3 atmosSunTransmittance(const in vec2 density)
{
	if (atmosEarthValues())
		return exp(-(ATMOS_BETA_R0 * density.x + ATMOS_BETA_M0 * density.y));

	vec3 direct = atmosViewTransmittance(density);
	float thick = atmosHazeScatter.w;
	if (thick <= 0.0)
		return direct;

	vec3 tauR = ATMOS_BETA_R0 * atmosRayleigh.rgb * density.x;
	vec3 tauMs = ATMOS_BETA_M0 * atmosHazeScatter.rgb * density.y;
	vec3 tauMe = ATMOS_BETA_M0 * atmosHazeExtinction.rgb * density.y;
	vec3 tauA = 1e-6 * atmosGasAbsorb.rgb * density.x + (tauMe - tauMs);
	vec3 tauS = tauR + tauMs * (1.0 - atmosHazeGlow.rgb);
	vec3 bounced = exp(-tauA) / (1.0 + 0.75 * tauS);
	return max(direct, thick * bounced);
}

// The share of a sample step's scattering that reaches the end of the step: (1 - e^-tau) / tau
// of its own dimming tau. The sky adds up only 16 steps; in very thick air one step can be
// tens of optical depths, and counting all its scattering as if nothing in the step were in
// the way makes the sky burn out white. 1 for Earth's values, where it was never needed.
vec3 atmosStepShare(const in vec2 deltaDensity)
{
	if (atmosEarthValues())
		return vec3(1.0);
	vec3 tau = -log(max(atmosViewTransmittance(deltaDensity), vec3(1e-30)));
	return mix((1.0 - exp(-tau)) / max(tau, vec3(1e-6)), 1.0 - 0.5 * tau, lessThan(tau, vec3(1e-3)));
}

// get density at given point
vec2 getDensityAtPoint(const in vec3 rayStart, const in sampler2D densityLUT)
{
	float height = length(rayStart) - geosphereRadius; // in meters
	float maxHeight = (geosphereAtmosTopRad - 1.f) * geosphereRadius;

	float ratio = height / maxHeight;

	return exp(texture(densityLUT, vec2(0.0, ratio)).xy);
}

// replace (a, b) by (b, a) if a > b
vec2 sortAscending(const in vec2 segment)
{
	return (segment.x > segment.y) ? vec2(segment.y, segment.x) : segment;
}

// given a and b segments, return c = a \ b
vec4 segmentSubtraction(const in vec2 a, const in vec2 b)
{
	vec2 as = sortAscending(a);
	vec2 bs = sortAscending(b);

	// b could be inside a, leaving segments at both sides
	vec4 c;
	c.x = a.x;
	c.w = a.y;

	c.y = min(a.y, max(a.x, b.x));
	c.z = max(a.x, min(a.y, b.y));
	return c;
}

/*
 * given:
 * sunDirection - direction from camera to light source, normalized
 * dir - direction from camera to pixel being rendered, normalized
 * center - position of planet relative to camera, in absolute scale
 *
 * splits given ray into parts by planet shadow:
 * - xy: ray before shadow
 * - yz: ray inside shadow
 * - zw: ray after shadow
 */
vec4 getRaySegment(const in vec3 sunDirection, const in vec3 rayDirection, const in vec3 rayStart)
{
	// solve Cylinder entry/exit dist
	vec2 cylinder_intersect = rayCylinderIntersect(rayDirection, -rayStart, sunDirection, geosphereRadius);
	bool hasIntersect = cylinder_intersect.x != 0 || cylinder_intersect.y != 0;

	vec3 cylinder_near = rayStart + rayDirection * cylinder_intersect.x;
	vec3 cylinder_far  = rayStart + rayDirection * cylinder_intersect.y;

	// test if ray passes through shadow
	float a = dot(cylinder_near, sunDirection);
	float b = dot(cylinder_far , sunDirection);
	bool intersectsShadow = hasIntersect && (a < 0.f || b < 0.f);

	vec2 ground_intersect = raySphereIntersect(-rayStart, rayDirection, geosphereRadius);
	bool shadowVisible = intersectsShadow && ground_intersect.x == 0.f;

	/*
	* We have three options:
	* 1) Ray does not intersect shadow
	*    Do nothing
	* 2) Ray intersects shadow, starts inside
	*    (cylinder_intersect.y, tmax)
	* 3) Ray intersects shadow, starts outside
	*    (tmin, cylinder_intersect.x) + (cylinder_intersect.y, tmax)
	*/

	vec2 atmosphere_intersect = raySphereIntersect(-rayStart, rayDirection, geosphereRadius * geosphereAtmosTopRad);
	vec4 atmosphere_minus_shadow = shadowVisible ? segmentSubtraction(atmosphere_intersect, cylinder_intersect) : vec4(atmosphere_intersect.x, atmosphere_intersect.y, atmosphere_intersect.y, atmosphere_intersect.y);

	if (ground_intersect.x > 0.f) {
		atmosphere_minus_shadow.yzw = min(atmosphere_minus_shadow.yzw, ground_intersect.x);
	}
	if (ground_intersect.y < 0.f) {
		atmosphere_minus_shadow.xyz = max(atmosphere_minus_shadow.xyz, ground_intersect.y);
	}

	return atmosphere_minus_shadow;
}

vec3 directLight2dLUT(const in vec3 a, const in vec3 b, const in sampler2D scatterLUT) {
	float height = length(a) - geosphereRadius;
	float maxHeight = geosphereAtmosTopRad * geosphereRadius - geosphereRadius;

	float height_scaled = height / maxHeight;

	float cos_phi = dot(normalize(b - a), normalize(a));
	vec2 density = exp(texture(scatterLUT, vec2(acos(cos_phi) / PI, height_scaled)).xy);

	return atmosSunTransmittance(density);
}


vec3 directLight1dLUT(const in vec3 a, const in vec3 b, const in sampler2D densityLUT, const int samples) {

	float segmentLength = length(b - a) / samples;

	vec2 density = vec2(0.f);
	for (int i = 0; i < samples; ++i) {
		vec3 c = mix(a, b, vec3(i + 0.5) / samples);

		density += segmentLength * getDensityAtPoint(c, densityLUT);
	}

	return atmosSunTransmittance(density);
}


vec3 calculateDirectLight(const in vec3 a, const in vec3 b, const in sampler2D densityLUT, const int samples, const in sampler2D scatterLUT)
{
    //return directLight1dLUT(a, b, densityLUT, samples);
    return directLight2dLUT(a, b, scatterLUT);
}

void processLight(const in vec3 a, const in vec3 b, inout vec2 iDensity, inout vec3 color, const in vec3 sunDirection, const in vec3 rayDirection, const in vec4 diffuse, const in float uneclipsed, const in sampler2D densityLUT, const in sampler2D scatterLUT) {
	if (a == b)
		return;

	float mu = dot(rayDirection, sunDirection);
	float phaseR = rayleighPhaseFunction(mu);
	vec3 phaseM = atmosMiePhase(mu);
	vec3 betaR = atmosBetaR();
	vec3 betaM = atmosBetaM();

	int iSamples = _iSamples;
	int oSamples = _oSamples;
	float iSegmentLength = length(b - a) / iSamples;
	if (iSegmentLength == 0.0) {
		return;
	}

	for (int i = 0; i < iSamples; ++i) {
		vec3 c = mix(a, b, vec3(i + 0.5f) / iSamples);

		vec2 deltaDensity = iSegmentLength * getDensityAtPoint(c, densityLUT);

		vec2 intersect = raySphereIntersect(-c, sunDirection, geosphereRadius);

		float d_scal = raySphereIntersect(-c, sunDirection, geosphereRadius * geosphereAtmosTopRad).y;
		vec3 d = c + sunDirection * d_scal;

		// before scattering
		vec3 color0 = diffuse.xyz * calculateDirectLight(c, d, densityLUT, oSamples, scatterLUT);

		// during scattering
		vec3 color1R = color0 * betaR * phaseR * deltaDensity.x;
		vec3 color1M = color0 * betaM * phaseM * deltaDensity.y;
		vec3 color1 = (color1R + color1M) * atmosStepShare(deltaDensity);

		// after scattering
		vec3 color2 = color1 * atmosViewTransmittance(iDensity);

		iDensity += deltaDensity;
		color += (intersect.y > 0.f) ? vec3(0.f) : color2;
	}
}

vec3 calculateIncidentLightAB(const in vec3 sunDirection, const in vec3 rayStart, const in vec3 rayEnd, const in vec4 diffuse, const in float uneclipsed, const in sampler2D densityLUT, const in sampler2D scatterLUT)
{
    vec3 rayDirection = normalize(rayEnd - rayStart);
    float rayLength   = length(rayEnd - rayStart);

	vec4 segment = getRaySegment(sunDirection, rayDirection, rayStart);

	/*
	 * rayStart -> enterAtm -> enterShadow -> exitShadow -> exitAtm -> rayFinish
	 */
	float enterAtm    = min(rayLength, segment.x);
	float enterShadow = min(rayLength, segment.y);
	float exitShadow  = min(rayLength, segment.z);
	float exitAtm     = min(rayLength, segment.w);

	vec3 a = rayStart + rayDirection * enterAtm;
	vec3 b = rayStart + rayDirection * enterShadow;
	vec3 c = rayStart + rayDirection * exitShadow;
	vec3 d = rayStart + rayDirection * exitAtm;

	vec2 density = vec2(0.0);
	vec3 color = vec3(0.f);

	processLight(a, b, density, color, sunDirection, rayDirection, diffuse, uneclipsed, densityLUT, scatterLUT);
	processLight(b, c, density, color, sunDirection, rayDirection, diffuse, uneclipsed, densityLUT, scatterLUT);
	processLight(c, d, density, color, sunDirection, rayDirection, diffuse, uneclipsed, densityLUT, scatterLUT);

	return color;
}

// The many-bounce glow of a thick haze (atmosHazeExtinction.w; Titan's smog), as
// rayleigh_geosphere_terrain.vert gives it to the ground seen through it, for a sky ray:
// along the planet's edge it carries the disc's glow out until the haze thins, so the
// edge is smooth. hazeDensity: the haze along the ray; at: a point on it; divided by the
// sky's x20 exposure so it matches the ground.
vec3 atmosHazeGlowSky(const in float hazeDensity, const in vec3 at, const in vec3 rayDirection, const in vec3 sunDirection, const in vec3 lightColor)
{
	if (atmosEarthValues() || atmosHazeExtinction.w <= 0.0)
		return vec3(0.0);
	vec3 tauHaze = ATMOS_BETA_M0 * atmosHazeExtinction.rgb * hazeDensity;
	vec3 scatterShare = atmosHazeScatter.rgb / max(atmosHazeExtinction.rgb, vec3(1e-9));
	vec3 gam = sqrt(max(1.0 - scatterShare, vec3(0.0)));
	vec3 layerAlbedo = (1.0 - gam) / (1.0 + gam);
	vec3 up = normalize(at);
	float muS = max(dot(up, sunDirection), 0.0);
	float muV = max(dot(up, -rayDirection), 0.0);
	return atmosHazeExtinction.w * layerAlbedo * lightColor * muS * (0.6 + 0.4 * muV) * (1.0 - exp(-tauHaze)) / 20.0;
}

// Seen from high above, the sky around a planet's edge at the ground's x4 rather than the sky's
// x20 (rayleigh_geosphere_terrain.vert shows the same air over the disc at x4): otherwise the
// edge of the air stands out against the disc as a bright, sharp ring. From the ground the sky
// keeps its x20; in between it changes smoothly with height. Bodies with Earth's values keep
// the old sky.
float atmosEdgeScale(const in vec3 rayStart)
{
	if (atmosEarthValues())
		return 1.0;
	float atmosHeight = (geosphereAtmosTopRad - 1.0) * geosphereRadius;
	float camAlt = length(rayStart) - geosphereRadius;
	return mix(1.0, 4.0 / 20.0, smoothstep(0.2, 1.0, camAlt / max(atmosHeight, 1.0)));
}

// How much a thick haze (atmosHazeExtinction.w) hides along a path with this much haze: there
// its many-bounce glow takes the place of the single bounce, which the sky shows at x20 and
// the ground at x4, so the two only agree where the glow has taken over. 0 without one.
float atmosHazeCover(const in float hazeDensity)
{
	if (atmosEarthValues() || atmosHazeExtinction.w <= 0.0)
		return 0.0;
	return atmosHazeExtinction.w * (1.0 - exp(-ATMOS_BETA_M0.g * atmosHazeExtinction.g * hazeDensity));
}

vec3 calculateIncidentLight(const in vec3 sunDirection, const in vec3 rayDirection, const in vec3 rayStart, const in vec4 diffuse, const in float uneclipsed, const in sampler2D densityLUT, const in sampler2D scatterLUT)
{
	vec4 segment = getRaySegment(sunDirection, rayDirection, rayStart);

	/*
	 * rayStart -> enterAtm -> enterShadow -> exitShadow -> exitAtm -> rayFinish
	 */
	float enterAtm    = segment.x;
	float enterShadow = segment.y;
	float exitShadow  = segment.z;
	float exitAtm     = segment.w;

	vec3 a = rayStart + rayDirection * enterAtm;
	vec3 b = rayStart + rayDirection * enterShadow;
	vec3 c = rayStart + rayDirection * exitShadow;
	vec3 d = rayStart + rayDirection * exitAtm;

	vec2 density = vec2(0.0);
	vec3 color = vec3(0.f);

	processLight(a, b, density, color, sunDirection, rayDirection, diffuse, uneclipsed, densityLUT, scatterLUT);
	processLight(b, c, density, color, sunDirection, rayDirection, diffuse, uneclipsed, densityLUT, scatterLUT);
	processLight(c, d, density, color, sunDirection, rayDirection, diffuse, uneclipsed, densityLUT, scatterLUT);

	color *= atmosEdgeScale(rayStart);

	// a thick haze's many-bounce glow, carried out along the planet's edge (Titan), in place of
	// the single bounce where the haze hides the ray
	float cover = atmosHazeCover(density.y);
	if (cover > 0.0) {
		// Past the planet's edge the glow fades with the height the ray passes at, from the
		// disc's own brightness at the edge outwards; the haze stays thick along such rays far
		// out, so without this the glow would stay flat and then stop short, a hard ring.
		// (From the ground the ray passes at the camera's height, so the sky there keeps it.)
		float tClosest = max(-dot(rayStart, rayDirection), 0.0);
		float passHeight = max(length(rayStart + rayDirection * tClosest) - geosphereRadius, 0.0);
		float fadeHeight = 0.15 * (geosphereAtmosTopRad - 1.0) * geosphereRadius;
		float edgeFade = exp(-passHeight / max(fadeHeight, 1.0));
		color = color * (1.0 - cover) + atmosHazeGlowSky(density.y, 0.5 * (a + d), rayDirection, sunDirection, diffuse.xyz) * uneclipsed * edgeFade;
	}

	return color;
}

vec3 calculateTerrainLight(const in vec3 sunDirection, const in vec3 rayDirection, const in vec3 camera, const in sampler2D densityLUT, const int samples, const in sampler2D scatterLUT)
{
	// get terrain intersection
	vec2 sectTerrain = raySphereIntersect(camera, rayDirection, geosphereRadius);
	vec3 rayStart = rayDirection * sectTerrain.x + camera;

	vec2 sectAtm = raySphereIntersect(-rayStart, sunDirection, geosphereRadius * geosphereAtmosTopRad);
	vec3 rayFinish = sunDirection * sectAtm.y + rayStart;

	vec3 light = calculateDirectLight(rayStart, rayFinish, densityLUT, samples, scatterLUT);

	return light;
}

vec3 calculateTerrainColor(const in vec4 planet, const in vec4 atmosphere, const in vec4 lightColor, const in vec3 lightDir, const in vec3 rayStart, const in vec3 rayDir, const in float uneclipsed, const in sampler2D densityLUT, const in sampler2D scatterLUT)
{
	// rayStart is already multiplied by planet radius
	vec3 planetPosition = planet.xyz * planet.w + rayStart;

	vec3 atmospherePosition = atmosphere.xyz * atmosphere.w;

	vec3 light = calculateTerrainLight(lightDir, rayDir, planetPosition, densityLUT, _iSamples, scatterLUT);

	return light;
}

vec3 calculateAtmosphereColor(const in vec4 planet, const in vec4 atmosphere, const in vec4 lightColor, const in vec3 lightDir, const in vec3 rayStart, const in vec3 rayDir, const in float uneclipsed, const in sampler2D densityLUT, const in sampler2D scatterLUT)
{
    // rayStart is already multiplied by planet radius
    vec3 planetPosition = planet.xyz * planet.w + rayStart;

    vec3 atmospherePosition = atmosphere.xyz * atmosphere.w;

    return calculateIncidentLight(lightDir, rayDir, -planetPosition, lightColor, uneclipsed, densityLUT, scatterLUT);
}

vec3 calculateAtmosphereColorAB(const in vec4 planet, const in vec4 atmosphere, const in vec3 rayStart, const in vec3 rayEnd, const in vec4 lightColor, const in vec3 lightDir, const in float uneclipsed, const in sampler2D densityLUT, const in sampler2D scatterLUT)
{
    // rayStart is already multiplied by planet radius
    vec3 planetPositionStart = planet.xyz * planet.w + rayStart;
    vec3 planetPositionEnd = planet.xyz * planet.w + rayEnd;

    vec3 atmospherePosition = atmosphere.xyz * atmosphere.w;
    return calculateIncidentLightAB(lightDir, -planetPositionStart, -planetPositionEnd, lightColor, uneclipsed, densityLUT, scatterLUT);
}

// Compute the per-wavelength (RGB) optical transmittance of an atmospheric
// medium over the given optical depth. This is a convenience function.
vec3 calcTransmittance(const in vec2 opticalDepth)
{
	return atmosViewTransmittance(opticalDepth);
}

// Compute the atmospheric transmittance along a ray from a point within the atmosphere to a point on the edge of the atmosphere.
// NOTE: rayStart is relative to geosphereCenter
// NOTE: rayDir points "out" of the atmosphere
vec3 calculateRayTransmittance(const in vec3 rayStart, const in vec3 rayDir, const in sampler2D scatterLUT)
{
	// Compute the height of the sampled point in the atmosphere in 0..1
	float height_scaled = clamp((length(rayStart) - 1) / (geosphereAtmosTopRad - 1), 0, 1);

	// To compute the density of the atmosphere along the ray, we need to know
	// the angle of the ray relative to vertical.
	float cos_phi = dot(rayDir, normalize(rayStart)); //* 0.5 + 0.5;
	float alpha = acos(cos_phi) / PI;

	// Density is stored to the texture in log scale
	vec2 density = exp(texture(scatterLUT, vec2(alpha, height_scaled)).xy);

	// Compute the transmittance factor for the three color wavelengths we are rendering.
	return atmosSunTransmittance(density);
}

// Compute the in-scattering of light along a ray from a point in the atmosphere
// until its exit. RayDir points in the direction the light being in-scattered will travel.
//
// The parameterization is backwards because it is much easier to express a
// *surface shader* in terms of the surface being shaded.
vec3 calculateRayInscattering(inout vec2 opticalDepth, const in vec3 rayStart, const in vec3 rayDir, const in float rayLength, const in vec3 lightDir, const in vec3 lightColor, const in sampler2D densityLUT, const in sampler2D scatterLUT)
{
	vec3 color = vec3(0);

	vec2 atmos = raySphereIntersect(-rayStart, rayDir, geosphereAtmosTopRad);
	if (atmos.y == 0)
		return color; // early-out if we're not in an atmosphere

	float atmosDist = min(atmos.y, rayLength);
	vec3 atmosExit = rayStart + rayDir * atmosDist;

	// This phase function uses the convention that the ray points from the
	// camera to the surface so we need to invert the ray dir here.
	float mu = dot(-rayDir, lightDir);
	float phaseR = rayleighPhaseFunction(mu);
	vec3 phaseM = atmosMiePhase(mu);
	vec3 betaR = atmosBetaR();
	vec3 betaM = atmosBetaM();

	float iSegmentLength = geosphereRadius * atmosDist / _iSamples;
	if (iSegmentLength == 0.0) {
		return color;
	}

	for (int i = 0; i < _iSamples; ++i) {
		// Trace backwards from the exit to the start of the ray to accumulate
		// optical depth over the sample ray.
		vec3 c = mix(atmosExit, rayStart, vec3(i + 0.5f) / _iSamples);

		float heightRatio = (length(c) - 1) / (geosphereAtmosTopRad - 1);
		vec2 avgDensity = exp(texture(densityLUT, vec2(0.0, heightRatio)).xy);

		vec2 deltaDensity = iSegmentLength * avgDensity;

		// find if this ray intersects the terrain
		vec2 intersect = raySphereIntersect(-c, lightDir, 1);

		// outscattering of light reaching this point
		vec3 color0 = lightColor * calculateRayTransmittance(c, lightDir, scatterLUT);

		// evaluate the phase function to find the wavelength-dependent
		// proportion of light being scattered in the camera direction
		vec3 color1R = color0 * betaR * phaseR * deltaDensity.x;
		vec3 color1M = color0 * betaM * phaseM * deltaDensity.y;
		vec3 color1 = (color1R + color1M) * atmosStepShare(deltaDensity);

		// outscattering of this new light on the way to the camera
		vec3 color2 = color1 * calcTransmittance(opticalDepth);

		opticalDepth += deltaDensity;
		color += step(0, intersect.y) * color2;
	}

	return color;
}
