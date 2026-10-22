// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Cloud density, lighting and raymarching.
// Requires (in this order, before this file): attributes.glsl, lib.glsl,
// a uniform block providing atmosColor, cloud-config.glsl,
// cloud-uniforms.glsl, cloud-noise.glsl, cloud-derive.glsl.
//
// Conventions: positions are relative to the planet centre, in planet radii,
// in view-space orientation. "pf" vectors are planet-fixed (rotate with the
// planet, +Y = spin axis).

#define CLOUD_AU 149598000000.0

#if (NUM_LIGHTS > 0)
#if (NUM_LIGHTS < CLOUD_MAX_LIGHTS)
#define CLOUD_NL NUM_LIGHTS
#else
#define CLOUD_NL CLOUD_MAX_LIGHTS
#endif
#else
#define CLOUD_NL 0
#endif

// vertical relief of the layer envelope, as fractions of the layer thickness
#define CLOUD_BASE_RELIEF 0.25
#define CLOUD_TOP_RELIEF 0.35

struct CloudPhase {
	float tA;  // time inside phase A [s]
	float tB;  // time inside phase B [s]
	float wA;  // weight of phase A (B gets 1 - wA)
	vec3 offA; // noise offsets (re-seeded every cycle)
	vec3 offB;
};

struct CloudResult {
	vec3 inscatter; // linear radiance, already premultiplied
	float transmittance;
};

vec3 cloudToPf(vec3 v)
{
	return vec3(dot(v, cloudAxisX.xyz), dot(v, cloudAxisY.xyz), dot(v, cloudAxisZ.xyz));
}

// rotate about the planet's +Y axis, increasing longitude atan(x, z) by ang
vec3 cloudRotY(vec3 v, float ang)
{
	float c = cos(ang), s = sin(ang);
	return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

vec2 cloudSphereHit(vec3 o, vec3 d, float r)
{
	float b = dot(o, d);
	float c = dot(o, o) - r * r;
	float h = b * b - c;
	if (h < 0.0) return vec2(-1.0, -2.0);
	h = sqrt(h);
	return vec2(-b - h, -b + h);
}

// Dual-phase cross-fade. Each phase lives 0.6 of a period and is re-seeded
// while its weight is zero; both are evaluated only during the 20% of the
// time when they overlap. The cloud clock (C++) never runs faster than 100x
// real time, so high time acceleration stays smooth.
CloudPhase cloudPhases(float period, float layer)
{
	CloudPhase ph;
	float seedLo = cloudTime.z, seedHi = cloudTime.w;
	float cyclesPerWrap = 1048576.0 / period;
	float p = cloudTime.x / period;
	float f = fract(p);
	float cycA = cloudTime.y * cyclesPerWrap + floor(p);
	float cycB = cloudTime.y * cyclesPerWrap + floor(p + 0.5);
	ph.tA = f * period;
	ph.tB = fract(p + 0.5) * period;

	const float k = 0.1;
	float wA = f < k ? f / k : (f < 0.5 ? 1.0 : (f < 0.5 + k ? 1.0 - (f - 0.5) / k : 0.0));
	ph.wA = smoothstep(0.0, 1.0, wA);

	ph.offA = (cloudHash33(vec3(mod(cycA, 4096.0) + 0.37, seedLo + layer * 7.0, seedHi + floor(cycA / 4096.0))) - 0.5) * 2000.0;
	ph.offB = (cloudHash33(vec3(mod(cycB, 4096.0) + 0.37, seedLo + layer * 7.0, seedHi + floor(cycB / 4096.0))) - 0.5) * 2000.0;
	return ph;
}

float cloudTerrainHeight(CloudPlanet cp, vec3 pfDir)
{
	if (cloudAxisX.w > 0.75) {
		float lon = atan(pfDir.x, pfDir.z);
		float lat = asin(clamp(pfDir.y, -1.0, 1.0));
		vec2 uv = vec2(lon / (2.0 * PI) + 0.5, clamp(lat / PI + 0.5, 0.5 / 256.0, 1.0 - 0.5 / 256.0));
		return textureLod(cloudHeightTex, uv, 0.0).r;
	}
	// still baking: stay conservative; far away (no map): sea level
	return cloudAxisX.w > 0.25 ? cp.hMax : 0.0;
}

// The noise functions are close to normal(mean, sd) (measured on the GPU);
// mapping them to a uniform variable makes coverage thresholds exact.
float cloudUniformize(float x, float mean, float sd)
{
	return 1.0 / (1.0 + exp(-1.702 * (x - mean) / sd));
}

// Local coverage of a layer at an (advected) planet-fixed position.
// fp: pixel footprint [m]; weather octaves are added down to ~2 pixels, up
// to 3 + maxExtra octaves (raymarched samples get their fine structure from
// the shape noise, single-sample slabs need more weather octaves).
// Returns x = inside-a-cloud-system weight 0..1, y = stratus weight (low
// layer: overcast regimes, plus the densest few percent of broken cloud
// fields), z = cloud fraction within a system.
vec3 cloudCoverage(CloudPlanet cp, int li, vec3 pAdv, vec3 pfDir, float lat, vec3 off, vec3 sunPf, float fp, float maxExtra)
{
	float wsize = 1.2e6 * (cp.a / 6.378e6) * cp.L[li].systemSize;
	float n = 3.0 + clamp(log2((wsize / 8.0) / max(2.0 * fp, 1.0)), 0.0, maxExtra);
	float lastW = n - floor(n);
	int oct = int(ceil(n - 1e-3));
	float W = cloudFbm(pAdv * (cp.a / wsize) + off * 0.001, oct, lastW > 1e-3 ? lastW : 1.0);
	float U = cloudUniformize(W, 0.5, 0.09);
	// total cover here, split into the fraction of the area inside cloud
	// systems (r) and the cloud fraction within a system (fill): r * fill = c
	float c = clamp(cp.L[li].coverage * cloudLatFactor(cp, li, lat, pfDir, sunPf) * CLOUD_COVERAGE_SCALE, 0.0, 1.0);
	float fill = max(cp.L[li].fillBase, c);
	float rg = c / max(fill, 1e-3);
	// soft edges, narrowing as the layer approaches overcast
	float e = 0.3 * clamp(2.0 * (1.0 - rg) + 0.1, 0.1, 1.0);
	float st = li == 0 ? max(smoothstep(0.85, 0.95, c), smoothstep(0.975, 0.995, U)) : 0.0;
	return vec3(smoothstep(1.0 - rg - 0.5 * e, 1.0 - rg + 0.5 * e, U), st, fill);
}

// Height profiles per genus. st = stratus weight (low layer only).
float cloudProfile(int li, float h, float st)
{
	if (li == 0) {
		float hs = h * 2.0; // stratus occupies the lower half of the envelope
		float pSt = smoothstep(0.0, 0.15, hs) * (1.0 - smoothstep(0.75, 1.0, hs));
		// stratocumulus: dense lower part, rounded upper part
		float pSc = smoothstep(0.0, 0.12, h) * (1.0 - smoothstep(0.35, 1.0, h));
		return mix(pSc, pSt, st);
	} else if (li == 1) {
		return smoothstep(0.0, 0.25, h) * (1.0 - smoothstep(0.45, 1.0, h));
	}
	return smoothstep(0.0, 0.4, h) * (1.0 - smoothstep(0.85, 1.0, h));
}

// representative altitude for single-sample (slab) evaluation of a layer:
// the low layer's stratus profile lives in the lower half of its envelope
float cloudSlabAlt(CloudLayer ly, int li)
{
	return li == 0 ? ly.base + 0.3 * (ly.top - ly.base) : 0.5 * (ly.base + ly.top);
}

float cloudSigmaMax(int li, float st)
{
	if (li == 0) return mix(0.025, 0.10, st);
	if (li == 1) return 0.02;
	return 0.0005; // cirrus: mostly translucent veils, vertical optical depth ~0.1-0.6
}

// Wind displacement of a whole layer: every altitude inside a layer moves
// with the wind at the layer's mid-height. Per-altitude winds would shear the
// 3D noise by tens of km per layer over one evolution cycle, which shows up
// as long streaks.
vec3 cloudAdvect(CloudPlanet cp, int li, vec3 pfDir, float lat, float t)
{
	CloudLayer ly = cp.L[li];
	float u = cloudWind(cp, lat, 0.5 * (ly.base + ly.top));
	float theta = u * t / (cp.a * max(cos(lat), 0.1)) * cp.spinSign * CLOUD_WIND_SIGN;
	return cloudRotY(pfDir, -theta);
}

// Large-scale properties of a layer column for one time phase: the
// expensive, slowly varying part (weather coverage and base undulation).
// Returns x = inside-a-system weight, y = stratus weight, z = base noise
// -1..1, w = cloud fraction within a system.
vec4 cloudColumn(CloudPlanet cp, int li, vec3 pfDir, float alt, float lat, float t, vec3 off, vec3 sunPf, float fp, float maxExtra)
{
	vec3 p = cloudAdvect(cp, li, pfDir, lat, t);
	vec3 cov = cloudCoverage(cp, li, p, pfDir, lat, off, sunPf, fp, maxExtra);
	float baseN = cov.x > 0.0 ? cloudGradNoise(p * (cp.a / (15000.0 * cp.sH)) + off * 0.37) : 0.0;
	return vec4(cov.xy, baseN, cov.z);
}

// Cirrus noise domain: horizontal planet coordinates scaled by the
// along-wind fibre length, latitude arc by the across-wind spacing. The
// noise is thus stretched along latitude circles - the direction of the
// zonal wind - without a seam at any longitude.
vec3 cloudCirrusDomain(CloudPlanet cp, vec3 p, vec3 off)
{
	float lat = asin(clamp(p.y, -1.0, 1.0));
	float len = CLOUD_CIRRUS_LENGTH_M * cp.cirrusScale;
	float wid = CLOUD_CIRRUS_WIDTH_M * cp.cirrusScale;
	vec3 q = vec3(p.x * (cp.a / len), p.z * (cp.a / len), lat * (cp.a / wid)) + off;
	// fibres meander and fan out: bend the across-wind coordinate with a
	// broad and a finer noise
	q.z += 3.0 * cloudGradNoise(vec3(q.xy * 0.25, q.z * 0.03) + vec3(7.3));
	q.z += 1.2 * cloudGradNoise(vec3(q.xy * 1.3, q.z * 0.15) + vec3(1.9));
	return q;
}

// Large-scale cirrus structure, 0..1: broad streak bands along the wind
// (~250 km across) seen from afar, and groups of wisps within them
// (~25 km across) closer in. fp: pixel footprint [m].
float cloudCirrusWisps(CloudPlanet cp, vec3 q, float fp)
{
	// clustering 0.5 = default; higher = fewer, denser wisp groups
	float shift = (clamp(cloudTuneCluster.z, 0.0, 1.0) - 0.5) * 0.3;
	float bands = smoothstep(0.35 + shift, 0.65 + shift, cloudFbm(vec3(q.xy * 0.08, q.z * 0.006) + vec3(9.7), 2, 1.0));
	float groupAcross = CLOUD_CIRRUS_WIDTH_M * cp.cirrusScale / 0.06;
	float nearW = smoothstep(1.5, 4.0, groupAcross / max(fp, 1.0));
	if (nearW <= 0.0) return bands;
	float groups = smoothstep(0.38 + shift, 0.62 + shift, cloudFbm(vec3(q.xy * 0.6, q.z * 0.06) + vec3(3.1), 2, 1.0));
	return mix(bands, clamp(bands * groups * 2.0, 0.0, 1.0), nearW);
}

// number of resolvable fibre octaves for a pixel footprint fp [m] (0..3):
// a fibre's bright crest is ~20% of the fibre spacing, and each octave is
// ~2x finer; octaves whose crest is under ~2 pixels fade out (no moire)
float cloudCirrusOctaves(CloudPlanet cp, float fp)
{
	float crest = 0.2 * CLOUD_CIRRUS_WIDTH_M * cp.cirrusScale;
	return clamp(log2(crest / (2.0 * max(fp, 1.0))) + 1.0, 0.0, 3.0);
}

// Fibres: ridged noise across the wind gives thin filaments along it.
// Octaves thinner than ~2 pixels fade out.
float cloudCirrusFibres(vec3 q, float oct)
{
	vec3 q0 = q;
	float sum = 0.0, norm = 0.0, amp = 0.6;
	for (int i = 0; i < 3; i++) {
		float w = clamp(oct - float(i), 0.0, 1.0);
		if (w <= 0.0) break;
		float r = max(1.0 - abs(cloudGradNoise(q)), 0.0);
		sum += w * amp * pow(r, 6.0); // sharp crests: thin filaments
		norm += w * amp;
		amp *= 0.5;
		q = q * vec3(1.6, 1.6, 2.2) + vec3(11.3, 5.7, 2.9);
	}
	// bundles: fibres fade in and out along their length (~15 km segments)
	float bundle = smoothstep(0.25, 0.75, 0.5 + 0.5 * cloudGradNoise(q0 * vec3(0.7, 0.7, 0.12) + vec3(4.1)));
	return norm > 0.0 ? sum / norm * (0.35 + 0.65 * bundle) : 0.0;
}

// Density at a point given its column properties.
// Returns x = density 0..1, y = stratus weight, z = height in layer 0..1.
vec3 cloudShapeDensity(CloudPlanet cp, int li, vec3 pfDir, float alt, float lat, float hTerr,
	float t, vec3 off, vec4 col, float lod, float detailW, bool full, float fp)
{
	CloudLayer ly = cp.L[li];
	float cLoc = col.x;
	float st = col.y;
	if (cLoc <= 0.0) return vec3(0.0);

	// undulating base; tops rise where the cloud field is denser
	float d = ly.top - ly.base;
	float base = ly.base + CLOUD_BASE_RELIEF * d * col.z;
	float top = ly.top + CLOUD_TOP_RELIEF * d * (2.0 * cLoc - 1.0);
	float h = (alt - base) / max(top - base, 1.0);
	if (h <= 0.0 || h >= 1.0) return vec3(0.0, st, h);

	// hard terrain floor
	float floorZ = hTerr + CLOUD_FLOOR_M;
	float floorF = smoothstep(floorZ, floorZ + 300.0, alt);
	if (floorF <= 0.0) return vec3(0.0, st, h);

	float prof = cloudProfile(li, h, st);

	if (li == 2) {
		// cirrus: fibrous streaks along the wind, with their own distance LOD
		vec3 p = cloudAdvect(cp, li, pfDir, lat, t);
		// fall streaks: the lower, falling ice trails the wind (cirrus uncinus hooks)
		p = cloudRotY(p, (1.0 - h) * CLOUD_CIRRUS_FALL_M * cp.cirrusScale / cp.a);
		vec3 q = cloudCirrusDomain(cp, p, off);
		float wisps = cloudCirrusWisps(cp, q, fp);
		// unresolved fibres (far away, slabs, shadows) use their mean fill
		float oct = full ? cloudCirrusOctaves(cp, fp) : 0.0;
		float fill = CLOUD_CIRRUS_FAR_FILL;
		if (oct > 0.0)
			fill = mix(CLOUD_CIRRUS_FAR_FILL, smoothstep(CLOUD_CIRRUS_FIB_LO, CLOUD_CIRRUS_FIB_HI, cloudCirrusFibres(q, oct) + 0.25 * (wisps - 0.5)), clamp(oct, 0.0, 1.0));
		return vec3(cLoc * prof * wisps * fill * floorF, st, h);
	}

	// cells smaller than a few pixels would only show as speckles: fade the
	// shape noise to the mean density of the field instead
	float shapeFade = full ? smoothstep(CLOUD_MIN_CELL_PIXELS, 3.0 * CLOUD_MIN_CELL_PIXELS, ly.featureSize / max(fp, 1.0)) : 0.0;
	// cheap / unresolved density: the cloud fraction times the mean in-cloud density
	float cellFill = mix(col.w, 1.0, st);
	float meanRho = cLoc * cellFill * prof * 0.7;
	if (shapeFade <= 0.0) return vec3((full ? meanRho : cLoc * cellFill * prof) * floorF, st, h);

	// shape noise per genus, advected with the wind
	vec3 p = cloudAdvect(cp, li, pfDir, lat, t);
	vec3 q = p * (cp.a / ly.featureSize) + off;
	float shape;
	float ero;
	if (li == 0) {
		vec3 qs = q * vec3(1.0, 1.5, 1.0); // slight roll organisation along the zonal wind
		float sc = st < 0.999 ? cloudUniformize(cloudPerlinWorley(qs, CLOUD_SHAPE_OCTAVES, lod), CLOUD_PW_MEAN, CLOUD_PW_SD) : 0.0;
		float sts = st > 0.001 ? cloudFbm(q * 0.15, 2, lod) * 0.4 + 0.6 : 0.0;
		shape = mix(sc, sts, st);
		ero = mix(0.4, 0.1, st);
	} else {
		vec3 qs = q * vec3(1.0, 0.8, 1.0); // rows across the wind
		shape = cloudUniformize(cloudBillowFbm(qs, 3, lod), CLOUD_BILLOW_MEAN, CLOUD_BILLOW_SD);
		ero = 0.45;
	}

	// inside a cloud system the cells fill the clustering fraction of the
	// area (stratus fills all of it)
	float cShape = cLoc * cellFill;
	float rho = clamp(cloudRemap(shape * prof, 1.0 - cShape, 1.0, 0.0, 1.0), 0.0, 1.0) * cLoc;
	if (rho > 0.0 && detailW > 0.01) {
		// billowy detail erodes the edges into rounded lumps
		float det = cloudUniformize(cloudBillowFbm(q * 4.0 + vec3(3.1), CLOUD_DETAIL_OCTAVES, detailW), CLOUD_BILLOW_MEAN, CLOUD_BILLOW_SD);
		rho = clamp(cloudRemap(rho, det * ero * detailW, 1.0, 0.0, 1.0), 0.0, 1.0);
	}
	return vec3(mix(meanRho, rho, shapeFade) * floorF, st, h);
}

// Density blended over both time phases (column evaluated in place).
vec3 cloudDensity(CloudPlanet cp, int li, CloudPhase ph, vec3 pfDir, float alt, float lat, float hTerr,
	float lod, float detailW, bool full, vec3 sunPf, float fp)
{
	vec3 r = vec3(0.0);
	if (ph.wA > 0.001) {
		vec4 col = cloudColumn(cp, li, pfDir, alt, lat, ph.tA, ph.offA, sunPf, fp, 5.0);
		r += ph.wA * cloudShapeDensity(cp, li, pfDir, alt, lat, hTerr, ph.tA, ph.offA, col, lod, detailW, full, fp);
	}
	if (ph.wA < 0.999) {
		vec4 col = cloudColumn(cp, li, pfDir, alt, lat, ph.tB, ph.offB, sunPf, fp, 5.0);
		r += (1.0 - ph.wA) * cloudShapeDensity(cp, li, pfDir, alt, lat, hTerr, ph.tB, ph.offB, col, lod, detailW, full, fp);
	}
	return r;
}

// Henyey-Greenstein, normalised so that the isotropic case is 1 (x 4 pi)
float cloudHG(float g, float mu)
{
	float g2 = g * g;
	return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5);
}

float cloudPhaseFn(CloudLayer ly, float mu, float gScale)
{
	return mix(cloudHG(ly.g2 * gScale, mu), cloudHG(ly.g1 * gScale, mu), ly.lobeMix);
}

// Sunlight arriving at pos, excluding the cloud self-shadow: planet shadow
// and Rayleigh-like reddening along the air mass (Kasten-Young), scaled by
// the surface pressure. White at noon, orange near the horizon.
vec3 cloudSunAt(vec3 pos, vec3 L, vec3 lightCol)
{
	// planet shadow, soft over ~0.2% of the radius
	float dp = dot(pos, L);
	float lit = 1.0;
	if (dp < 0.0)
		lit = smoothstep(0.998, 1.002, length(pos - dp * L));

	vec3 up = normalize(pos);
	float cz = dot(up, L);
	float zen = degrees(acos(clamp(cz, -1.0, 1.0)));
	float airmass = 1.0 / (max(cz, 0.0) + 0.50572 * pow(max(96.07995 - zen, 0.1), -1.6364));
	vec3 tAtm = exp(-vec3(0.03, 0.06, 0.13) * min(airmass, 40.0) * clamp(cloudAtmos.y, 0.0, 3.0));
	return lightCol * tAtm * lit;
}

// Lighting and integration of one sample into the running result.
// slabDepth: depth below the layer top [m] for slab samples, < 0 for
// raymarch samples (their height in the layer is used instead).
void cloudShadeSample(CloudPlanet cp, int li, vec3 pos, vec3 dens, float ds, vec3 rayDir, vec3 albedo, float slabDepth, inout CloudResult res)
{
	CloudLayer ly = cp.L[li];
	float sigmaE = cloudSigmaMax(li, dens.y) * ly.sigmaScale * dens.x;
	if (sigmaE <= 0.0) return;

	vec3 up = pos / max(length(pos), 1e-6);
	vec3 sunLight = vec3(0.0);
	vec3 ambient = vec3(0.0);
	float d = ly.top - ly.base;
	float depth = slabDepth >= 0.0 ? slabDepth : (1.0 - clamp(dens.z, 0.0, 1.0)) * d;
	// column extinction above the sample (analytic self-shadow, no extra noise
	// evaluations): a slab knows its mean density; a raymarch sample assumes
	// at least moderately dense cloud continues up to the top
	float sigmaCol = slabDepth >= 0.0 ? sigmaE : cloudSigmaMax(li, dens.y) * ly.sigmaScale * max(dens.x, 0.3);

#if (CLOUD_NL > 0)
	// sky light: the atmosphere colour, pulled towards Rayleigh blue-white
	vec3 sky = mix(toLinear(atmosColor.rgb), vec3(0.55, 0.7, 1.0), 0.5) * CLOUD_AMBIENT;
	for (int i = 0; i < CLOUD_NL; i++) {
		vec3 L = normalize(uLight[i].position.xyz);
		vec3 lightCol = toLinear(uLight[i].diffuse.rgb) / max(length(uLight[i].position.xyz / CLOUD_AU), 0.05);
		vec3 sun = cloudSunAt(pos, L, lightCol);

		// light enters through the top, or through the side for a low sun
		float tauSun = sigmaCol * min(depth / max(dot(up, L), 0.05), 4.0 * d);

		// multiple scattering octaves (Wrenninge 2013)
		float mu = dot(rayDir, L);
		vec3 ms = vec3(0.0);
		float a = 1.0, b = 1.0, c = 1.0;
		for (int o = 0; o < 3; o++) {
			ms += a * exp(-tauSun * b) * cloudPhaseFn(ly, mu, c);
			a *= 0.5;
			b *= 0.5;
			c *= 0.5;
		}
		// diffusely transmitted light deep inside thick cloud (two-stream,
		// g = 0.85): why overcast undersides are grey rather than black
		ms += 0.5 * (1.0 / (1.0 + 0.75 * tauSun * (1.0 - 0.85)) - exp(-tauSun));
		// powder (Schneider 2015): mild darkening of thin, sun-facing wisps,
		// only for raymarched samples; on far slabs every patch edge is thin
		// and the term would draw dark outlines
		float powder = 1.0;
		if (slabDepth < 0.0)
			powder = mix(1.0, max(1.0 - exp(-2.0 * sigmaE * 150.0 * cp.sH), 0.5), 0.5 - 0.5 * mu);
		sunLight += sun * ms * powder;

		// sky light: from above, darker underneath (vertical gradient)
		float dayside = clamp(dot(up, L) + 0.2, 0.0, 1.2) / 1.2;
		ambient += sky * lightCol * dayside * mix(0.35, 1.0, clamp(dens.z, 0.0, 1.0));
	}
#endif

	vec3 S = (sunLight + ambient) * albedo * ly.omega;
	float ext = exp(-sigmaE * ds);
	res.inscatter += res.transmittance * S * (1.0 - ext);
	res.transmittance *= ext;
}

// cloud-to-cloud tint
vec3 cloudTint(CloudPlanet cp, int li, vec3 pfDir)
{
	vec3 q = pfDir * (cp.a / 50000.0);
	float n1 = cloudGradNoise(q + vec3(float(li) * 13.1));
	float n2 = cloudGradNoise(q * 0.7 + vec3(41.3));
	vec3 warmCool = mix(vec3(0.98, 1.0, 1.02), vec3(1.02, 1.0, 0.97), 0.5 + 0.5 * n2);
	return cp.L[li].albedo * (1.0 + CLOUD_TINT_AMPLITUDE * n1) * mix(vec3(1.0), warmCool, 0.5);
}

void cloudSortIntervals(inout vec4 iv[6], int n)
{
	for (int i = 0; i < 5; i++) {
		for (int j = 0; j < 5 - i; j++) {
			if (j + 1 < n && iv[j + 1].x < iv[j].x) {
				vec4 tmp = iv[j];
				iv[j] = iv[j + 1];
				iv[j + 1] = tmp;
			}
		}
	}
}

// Raymarch the cloud layers along a view ray.
// camPos: camera relative to the planet centre [planet radii]; rayDir: unit,
// view space; tMax: ray end [planet radii]; pixelAngle: radians per pixel.
CloudResult cloudMarchView(CloudPlanet cp, vec3 camPos, vec3 rayDir, float tMax, float pixelAngle, float jitter)
{
	CloudResult res;
	res.inscatter = vec3(0.0);
	res.transmittance = 1.0;
	if (cp.enabled < 0.5) return res;

	vec3 sunPf = vec3(0.0, 1.0, 0.0);
#if (NUM_LIGHTS > 0)
	sunPf = cloudToPf(normalize(uLight[0].position.xyz));
#endif

	// collect the ray intervals inside each layer's envelope
	vec4 iv[6]; // x = t0, y = t1, z = layer
	for (int i = 0; i < 6; i++)
		iv[i] = vec4(0.0);
	int n = 0;
	float topAll = 0.0;
	for (int li = 0; li < 3; li++) {
		if (cp.L[li].active < 0.5) continue;
		float d = cp.L[li].top - cp.L[li].base;
		topAll = max(topAll, cp.L[li].top + CLOUD_TOP_RELIEF * d);
		float rIn = 1.0 + max(cp.L[li].base - CLOUD_BASE_RELIEF * d, CLOUD_FLOOR_M) / cp.a;
		float rOut = 1.0 + (cp.L[li].top + CLOUD_TOP_RELIEF * d) / cp.a;
		vec2 o = cloudSphereHit(camPos, rayDir, rOut);
		if (o.y <= 0.0) continue;
		float t0 = max(o.x, 0.0), t1 = min(o.y, tMax);
		vec2 inr = cloudSphereHit(camPos, rayDir, rIn);
		if (inr.y < inr.x) {
			if (t1 > t0) iv[n++] = vec4(t0, t1, float(li), 0.0);
		} else {
			float a0 = t0, a1 = min(inr.x, t1);
			if (a1 > a0) iv[n++] = vec4(a0, a1, float(li), 0.0);
			float b0 = max(inr.y, t0), b1 = t1;
			if (b1 > b0) iv[n++] = vec4(b0, b1, float(li), 0.0);
		}
	}
	if (n == 0) return res;
	cloudSortIntervals(iv, n);

	// aerial perspective: from inside the cloud region, distant clouds fade
	// into the (already rendered) hazy sky; visibility grows in thinner air
	float camAlt = (length(camPos) - 1.0) * cp.a;
	float hazeDist = 1e30;
	if (camAlt < topAll + 10000.0)
		hazeDist = CLOUD_VISIBILITY / clamp(cloudPressAt(cp, max(camAlt, 0.0)), 0.05, 3.0);

	for (int k = 0; k < 6; k++) {
		if (k >= n || res.transmittance < CLOUD_MIN_TRANSMITTANCE) break;
		int li = int(iv[k].z + 0.5);
		CloudLayer ly = cp.L[li];
		CloudPhase ph = cloudPhases(ly.period, float(li));
		float d = ly.top - ly.base;

		float t0 = iv[k].x, t1 = iv[k].y;
		float segLen = (t1 - t0) * cp.a;
		float distStart = t0 * cp.a;
		if (distStart > 4.0 * hazeDist) break; // too far to be visible
		float f0 = max(distStart, 1.0) * pixelAngle;
		float slabW = smoothstep(0.5, 2.0, f0 / max(d, 1.0));

		vec3 midPos = camPos + rayDir * (0.5 * (t0 + t1));
		vec3 albedo = cloudTint(cp, li, cloudToPf(normalize(midPos)));
		CloudResult before = res;

		// cirrus is a thin, translucent sheet whose pattern hardly changes with
		// height: a few jittered samples through the layer replace the raymarch
		// (a raymarch would never terminate early in it)
		if (li == 2) {
			float ds = segLen / float(CLOUD_CIRRUS_SAMPLES);
			for (int i = 0; i < 4; i++) {
				if (i >= CLOUD_CIRRUS_SAMPLES) break;
				// fixed (not jittered) sample heights: the fall-streak offset varies
				// with height, so jitter would scramble the fibres into a moire
				float t = t0 + (float(i) + 0.5) * (t1 - t0) / float(CLOUD_CIRRUS_SAMPLES);
				vec3 pos = camPos + rayDir * t;
				float r = length(pos);
				vec3 pfDir = cloudToPf(pos) / r;
				float alt = (r - 1.0) * cp.a;
				float lat = asin(clamp(pfDir.y, -1.0, 1.0));
				float fp = max(t * cp.a, 1.0) * pixelAngle;
				vec3 dens = cloudDensity(cp, li, ph, pfDir, alt, lat, cloudTerrainHeight(cp, pfDir), 1.0, 0.0, true, sunPf, fp);
				cloudShadeSample(cp, li, pos, dens, ds, rayDir, albedo, -1.0, res);
			}
			float fadeC = exp(-(0.5 * (t0 + t1) * cp.a) / hazeDist);
			res.inscatter = mix(before.inscatter, res.inscatter, fadeC);
			res.transmittance = mix(before.transmittance, res.transmittance, fadeC);
			continue;
		}

		// near: raymarch the first CLOUD_MAX_MARCH_DIST of the segment
		float marchLen = slabW < 0.99 ? min(segLen, CLOUD_MAX_MARCH_DIST) : 0.0;
		if (marchLen > 0.0) {
			// the column (coverage, base noise) varies on >= 15 km scales:
			// evaluate it every ~6 km along the segment and interpolate
			float spanT = marchLen / cp.a;
			int nCol = int(clamp(marchLen / 6000.0 + 2.0, 2.0, 8.0));
			float colStep = 1.0 / float(nCol - 1);
			float fpCol = max(distStart, 1.0) * pixelAngle;
			vec4 colA[8];
			vec4 colB[8];
			float anyCover = 0.0;
			for (int c = 0; c < 8; c++) {
				colA[c] = vec4(0.0);
				colB[c] = vec4(0.0);
				if (c >= nCol) continue;
				vec3 cpos = camPos + rayDir * (t0 + spanT * float(c) * colStep);
				float cr = length(cpos);
				vec3 cdir = cloudToPf(cpos) / cr;
				float clat = asin(clamp(cdir.y, -1.0, 1.0));
				float calt = (cr - 1.0) * cp.a;
				if (ph.wA > 0.001) colA[c] = cloudColumn(cp, li, cdir, calt, clat, ph.tA, ph.offA, sunPf, fpCol, 3.0);
				if (ph.wA < 0.999) colB[c] = cloudColumn(cp, li, cdir, calt, clat, ph.tB, ph.offB, sunPf, fpCol, 3.0);
				anyCover = max(anyCover, max(colA[c].x, colB[c].x));
			}

			if (anyCover > 0.0) {
				CloudResult m = res;
				// enough steps to resolve the layer geometry and its optical depth
				float tauEst = cloudSigmaMax(li, 1.0) * ly.sigmaScale * marchLen;
				int steps = int(clamp(max(marchLen / (0.15 * max(d, 1.0)), 1.5 * tauEst), 8.0, float(CLOUD_STEPS)));
				float ds = marchLen / float(steps);
				float dt = ds / cp.a;
				for (int i = 0; i < CLOUD_STEPS; i++) {
					if (i >= steps || m.transmittance < CLOUD_MIN_TRANSMITTANCE) break;
					float t = t0 + (float(i) + jitter) * dt;
					float fc = clamp((t - t0) / spanT, 0.0, 1.0) * float(nCol - 1);
					int c0 = int(min(floor(fc), float(nCol - 2)));
					float fw = fc - float(c0);
					vec4 cA = mix(colA[c0], colA[c0 + 1], fw);
					vec4 cB = mix(colB[c0], colB[c0 + 1], fw);
					if (max(cA.x, cB.x) <= 0.0) continue;

					vec3 pos = camPos + rayDir * t;
					float r = length(pos);
					vec3 pfDir = cloudToPf(pos) / r;
					float alt = (r - 1.0) * cp.a;
					float lat = asin(clamp(pfDir.y, -1.0, 1.0));
					float hTerr = cloudTerrainHeight(cp, pfDir);

					float fp = max(t * cp.a, 1.0) * pixelAngle;
					float lod = clamp(log2(ly.featureSize / (2.0 * fp)) - float(CLOUD_SHAPE_OCTAVES - 1), 0.0, 1.0);
					float detailW = clamp(log2(ly.featureSize * 0.25 / (2.0 * fp)), 0.0, 1.0);
					vec3 dens = vec3(0.0);
					if (ph.wA > 0.001)
						dens += ph.wA * cloudShapeDensity(cp, li, pfDir, alt, lat, hTerr, ph.tA, ph.offA, cA, lod, detailW, true, fp);
					if (ph.wA < 0.999)
						dens += (1.0 - ph.wA) * cloudShapeDensity(cp, li, pfDir, alt, lat, hTerr, ph.tB, ph.offB, cB, lod, detailW, true, fp);
					cloudShadeSample(cp, li, pos, dens, ds, rayDir, albedo, -1.0, m);
				}
				res.inscatter = mix(m.inscatter, res.inscatter, slabW);
				res.transmittance = mix(m.transmittance, res.transmittance, slabW);
			}
		}

		// far: the rest (or all) of the segment as one slab
		float slabLen = segLen - marchLen * (1.0 - slabW);
		if (slabLen > 1.0) {
			float tm = t0 + (marchLen * (1.0 - slabW) / cp.a) + 0.5 * slabLen / cp.a;
			vec3 pos = camPos + rayDir * tm;
			float r = length(pos);
			vec3 pfDir = cloudToPf(pos) / r;
			float lat = asin(clamp(pfDir.y, -1.0, 1.0));
			float altMid = cloudSlabAlt(ly, li);
			float hTerr = cloudTerrainHeight(cp, pfDir);
			float fpSlab = max(tm * cp.a, 1.0) * pixelAngle;
			vec3 env = cloudDensity(cp, li, ph, pfDir, altMid, lat, hTerr, 1.0, 0.0, false, sunPf, fpSlab);
			// A broken cloud field seen from afar is clouds plus gaps: the
			// covered area fraction f has the in-cloud optical depth, the
			// rest is clear, so T = (1 - f) + f * exp(-tau) rather than
			// exp(-f * tau). env.x = cloud fraction x profile.
			float f = clamp(env.x, 0.0, 1.0);
			if (f > 0.001) {
				vec3 dens = vec3(0.7, env.y, env.z); // mean in-cloud density
				// light the slab at the depth the viewer actually sees (optical depth ~1
				// from the near side): sunlit tops from above, shaded bases from below
				float sigmaSlab = cloudSigmaMax(li, dens.y) * ly.sigmaScale * dens.x;
				float fv = min(1.0, 1.0 / max(sigmaSlab * slabLen, 1e-3));
				bool fromAbove = camAlt > altMid;
				float slabDepth = fromAbove ? 0.5 * fv * d : d * (1.0 - 0.5 * fv);
				dens.z = 1.0 - slabDepth / max(d, 1.0);
				vec3 posMid = normalize(pos) * (1.0 + altMid / cp.a);
				CloudResult cloudPart;
				cloudPart.inscatter = vec3(0.0);
				cloudPart.transmittance = 1.0;
				cloudShadeSample(cp, li, posMid, dens, slabLen, rayDir, albedo, slabDepth, cloudPart);
				res.inscatter += res.transmittance * f * cloudPart.inscatter;
				res.transmittance *= 1.0 - f + f * cloudPart.transmittance;
			}
		}

		// aerial perspective for this segment
		float fade = exp(-(0.5 * (t0 + t1) * cp.a) / hazeDist);
		res.inscatter = mix(before.inscatter, res.inscatter, fade);
		res.transmittance = mix(before.transmittance, res.transmittance, fade);
	}
	return res;
}

// Cloud shadow factor on terrain at pos for light direction L
float cloudShadowTerrain(CloudPlanet cp, vec3 pos, vec3 L)
{
	if (cp.enabled < 0.5) return 1.0;
	float tau = 0.0;
	vec3 sunPf = cloudToPf(L);
	for (int li = 0; li < 3; li++) {
		if (cp.L[li].active < 0.5) continue;
		CloudLayer ly = cp.L[li];
		float altMid = cloudSlabAlt(ly, li);
		vec2 hit = cloudSphereHit(pos, L, 1.0 + altMid / cp.a);
		if (hit.y <= 0.0) continue;
		vec3 lp = pos + L * max(hit.y, 0.0);
		vec3 pfDir = normalize(cloudToPf(lp));
		CloudPhase ph = cloudPhases(ly.period, float(li));
		vec3 dens = cloudDensity(cp, li, ph, pfDir, altMid, asin(clamp(pfDir.y, -1.0, 1.0)), 0.0, 1.0, 0.0, false, sunPf, 150000.0);
		tau += cloudSigmaMax(li, dens.y) * ly.sigmaScale * dens.x * 0.7 * (ly.top - ly.base) / max(dot(normalize(lp), L), 0.1);
	}
	// two-stream total (direct + diffuse) transmission, g = 0.85: overcast
	// skies still pass ~30% of the light
	return 1.0 / (1.0 + 0.75 * tau * (1.0 - 0.85));
}

vec3 cloudToneMap(vec3 c)
{
	return toSRGB(1.0 - exp(-c * CLOUD_EXPOSURE));
}

// per-pixel raymarch offset: interleaved gradient noise (Jimenez 2014), which
// spreads the sampling error more evenly than white noise
float cloudJitter()
{
	return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
}

vec3 cloudHeat(float x)
{
	x = clamp(x, 0.0, 1.0);
	return clamp(vec3(3.0 * x - 2.0, 1.0 - abs(3.0 * x - 1.5) * 1.2, 1.0 - 3.0 * x), 0.0, 1.0) * smoothstep(0.0, 0.05, x);
}

// Debug views (CLOUD_DEBUG in cloud-config.glsl). pfDir: planet-fixed unit vector.
// Returns rgb and the amount to blend over the normal image.
vec4 cloudDebugColor(CloudPlanet cp, vec3 pfDir)
{
	float lat = asin(clamp(pfDir.y, -1.0, 1.0));
	float lon = atan(pfDir.x, pfDir.z);
	vec3 sunPf = vec3(0.0, 1.0, 0.0);
#if (NUM_LIGHTS > 0)
	sunPf = cloudToPf(normalize(uLight[0].position.xyz));
#endif
#if (CLOUD_DEBUG == 1)
	float latD = degrees(lat), lonD = degrees(lon);
	float wl = 0.15;
	vec3 c = vec3(0.0);
	float a = 0.0;
	if (abs(fract(latD / 10.0 + 0.5) - 0.5) * 10.0 < wl || abs(fract(lonD / 10.0 + 0.5) - 0.5) * 10.0 < wl) { c = vec3(1.0); a = 0.8; }
	if (abs(latD) < wl * 2.0) { c = vec3(1.0, 0.0, 0.0); a = 1.0; }
	if (abs(lonD) < wl * 2.0) { c = vec3(0.0, 1.0, 0.0); a = 1.0; }
	if (abs(latD - 30.0) < wl * 2.0) { c = vec3(0.0, 0.3, 1.0); a = 1.0; }
	return vec4(c, a);
#elif (CLOUD_DEBUG == 2)
	if (cp.enabled < 0.5) return vec4(0.0, 0.0, 0.0, 0.8);
	vec3 c = vec3(0.0);
	for (int li = 0; li < 3; li++) {
		if (cp.L[li].active < 0.5) continue;
		CloudPhase ph = cloudPhases(cp.L[li].period, float(li));
		vec3 cv = cloudCoverage(cp, li, cloudAdvect(cp, li, pfDir, lat, ph.tA), pfDir, lat, ph.offA, sunPf, 20000.0, 5.0);
		c[li] = cv.x * cv.z;
	}
	return vec4(c, 0.9);
#elif (CLOUD_DEBUG == 3)
	vec3 c = vec3(0.0);
	if (cp.species == CLOUD_SPECIES_H2SO4) c = vec3(1.0, 0.9, 0.2);
	else if (cp.species == CLOUD_SPECIES_H2O) c = vec3(0.2, 0.4, 1.0);
	else if (cp.species == CLOUD_SPECIES_CH4) c = vec3(1.0, 0.5, 0.1);
	else if (cp.species == CLOUD_SPECIES_NH3) c = vec3(0.2, 1.0, 0.3);
	else if (cp.species == CLOUD_SPECIES_CO2) c = vec3(1.0);
	return vec4(c, 0.9);
#elif (CLOUD_DEBUG == 4)
	if (cp.enabled < 0.5) return vec4(0.0, 0.0, 0.0, 0.9);
	float tau = 0.0;
	for (int li = 0; li < 3; li++) {
		if (cp.L[li].active < 0.5) continue;
		CloudLayer ly = cp.L[li];
		CloudPhase ph = cloudPhases(ly.period, float(li));
		vec3 dn = cloudDensity(cp, li, ph, pfDir, cloudSlabAlt(ly, li), lat, 0.0, 1.0, 0.0, false, sunPf, 20000.0);
		tau += cloudSigmaMax(li, dn.y) * ly.sigmaScale * dn.x * 0.7 * (ly.top - ly.base);
	}
	return vec4(cloudHeat(tau / 40.0), 0.9);
#elif (CLOUD_DEBUG == 5)
	if (cp.enabled < 0.5 || cp.L[0].active < 0.5) return vec4(0.0, 0.0, 0.0, 0.8);
	CloudPhase ph = cloudPhases(cp.L[0].period, 0.0);
	vec3 dn = cloudDensity(cp, 0, ph, pfDir, cp.L[0].base + 0.25 * (cp.L[0].top - cp.L[0].base), lat, 0.0, 1.0, 0.0, false, sunPf, 20000.0);
	if (dn.x <= 0.0) return vec4(0.0, 0.0, 0.0, 0.8);
	return vec4(mix(vec3(1.0, 0.1, 0.1), vec3(1.0), dn.y), 0.9);
#elif (CLOUD_DEBUG == 6)
	if (cp.enabled < 0.5) return vec4(0.0, 0.0, 0.0, 0.8);
	float u = cloudWind(cp, lat, 0.5 * (cp.L[1].base + cp.L[1].top)) * cp.spinSign * CLOUD_WIND_SIGN;
	vec3 c = (u >= 0.0 ? vec3(0.0, 1.0, 1.0) : vec3(1.0, 0.0, 1.0)) * clamp(abs(u) / 100.0, 0.05, 1.0);
	return vec4(c, 0.9);
#elif (CLOUD_DEBUG == 7)
	float hT = cloudTerrainHeight(cp, pfDir);
	vec3 c = vec3(clamp(hT / 9000.0, 0.0, 1.0)) * 0.5;
	for (int li = 0; li < 3; li++) {
		if (cp.L[li].active > 0.5 && hT + CLOUD_FLOOR_M > cp.L[li].base - CLOUD_BASE_RELIEF * (cp.L[li].top - cp.L[li].base))
			c = vec3(1.0, 0.0, 0.0);
	}
	return vec4(c, 0.85);
#else
	return vec4(0.0);
#endif
}
