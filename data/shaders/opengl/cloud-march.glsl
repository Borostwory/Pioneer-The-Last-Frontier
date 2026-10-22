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

// Phase k (0 = A, 1 = B) of a CloudPhase. Loops over the active phases use
// runtime bounds (cloudPhaseFirst .. cloudPhaseLast) so the compiler emits
// the expensive body once instead of unrolling it per phase.
void cloudPhaseK(CloudPhase ph, int k, out float t, out vec3 off, out float w)
{
	t = k == 0 ? ph.tA : ph.tB;
	off = k == 0 ? ph.offA : ph.offB;
	w = k == 0 ? ph.wA : 1.0 - ph.wA;
}

int cloudPhaseFirst(CloudPhase ph) { return ph.wA > 0.001 ? 0 : 1; }
int cloudPhaseLast(CloudPhase ph) { return ph.wA < 0.999 ? 1 : 0; }

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

// Rodrigues rotation of v about the unit axis k by angle ang
vec3 cloudRotAxis(vec3 v, vec3 k, float ang)
{
	float c = cos(ang), s = sin(ang);
	return v * c + cross(k, v) * s + k * dot(k, v) * (1.0 - c);
}

// Weather-noise domain: the large-scale flow organises cloud systems.
//  - extratropical cyclones in the storm tracks wind the cloud field into
//    spirals and comma clouds (cyclonic sense per hemisphere);
//  - the zonal flow stretches systems east-west into fronts and bands;
//  - a two-scale domain warp folds them into filaments.
// p: advected planet-fixed unit vector; returns the noise coordinate
// (1 unit = one weather system, wsize metres).
vec3 cloudWeatherDomain(CloudPlanet cp, vec3 p, float wsize, vec3 off)
{
	// cyclones: one candidate per cell of a 3D grid over the sphere, kept
	// with a probability that peaks in the storm track (poleward of the
	// subtropical jet); only planets with a strong Coriolis effect
	float spacing = CLOUD_CYCLONE_SPACING_M * (cp.a / 6.378e6);
	float strength = (1.0 - cp.wsr) * CLOUD_CYCLONE_SWIRL;
	if (strength > 0.01) {
		vec3 g = p * (cp.a / spacing);
		// candidates sit at 0.25..0.75 within their cell and reach at most
		// 0.75 cells, so only the 2x2x2 cells nearest to g can contribute
		vec3 g0 = floor(g - 0.5);
		vec3 seed = vec3(cloudTime.z, cloudTime.w, 0.0) * 0.013;
		vec3 disp = vec3(0.0);
		for (int z = 0; z <= 1; z++) {
			for (int y = 0; y <= 1; y++) {
				for (int x = 0; x <= 1; x++) {
					vec3 cell = g0 + vec3(float(x), float(y), float(z));
					vec3 h = cloudHash33(cell + seed);
					// candidates live in 3D grid space; the distance is measured
					// there too, so the field is continuous across cells
					vec3 cand = cell + 0.25 + 0.5 * h;
					vec3 dg = g - cand;
					float reach = CLOUD_CYCLONE_RADIUS * (0.6 + 0.4 * h.y); // cyclones differ in size
					float q = 1.0 - dot(dg, dg) / (reach * reach);
					if (q <= 0.0) continue;
					float fall = q * q * q; // compact bell, ~gaussian of radius reach / 2.2
					vec3 c = normalize(cand);
					float clat = abs(asin(clamp(c.y, -1.0, 1.0)));
					float track = exp(-cloudSq((clat - cp.phiJ - 5.0 * CLOUD_DEG) / (12.0 * CLOUD_DEG)));
					if (cloudHash13(cell * 1.7 + seed + vec3(5.3)) > CLOUD_CYCLONE_CHANCE * track) continue;
					// cyclonic: with the planet's spin, so opposite in the two
					// hemispheres; differential rotation (fastest near the centre)
					// winds any pattern into spiral arms (sampled back-rotated)
					float s = sign(c.y) * cp.spinSign * (0.6 + 0.8 * h.x);
					disp += cloudRotAxis(p, c, -strength * s * fall) - p;
				}
			}
		}
		// summed displacements: smooth where cyclones overlap
		p = normalize(p + disp);
	}
	vec3 x = p * (cp.a / wsize) + off * 0.001;
	// east-west elongation (the spin axis is y)
	x.y *= CLOUD_WEATHER_ZONAL;
	// fold into fronts and filaments
	vec3 w1 = vec3(cloudGradNoise(x * 0.6 + vec3(1.7, 9.2, 3.1)), cloudGradNoise(x * 0.6 + vec3(8.3, 2.8, 5.4)), cloudGradNoise(x * 0.6 + vec3(4.4, 7.1, 0.9)));
	x += CLOUD_WEATHER_WARP * w1;
	vec3 w2 = vec3(cloudGradNoise(x * 2.1 + vec3(3.3, 1.2, 7.7)), cloudGradNoise(x * 2.1 + vec3(6.1, 4.9, 2.2)), cloudGradNoise(x * 2.1 + vec3(0.7, 5.5, 8.8)));
	return x + 0.15 * CLOUD_WEATHER_WARP * w2;
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
	float W = cloudFbm(cloudWeatherDomain(cp, pAdv, wsize, off), oct, lastW > 1e-3 ? lastW : 1.0);
	float U = cloudUniformize(W, CLOUD_WEATHER_MEAN, CLOUD_WEATHER_SD);
	// total cover here, split into the fraction of the area inside cloud
	// systems (r) and the cloud fraction within a system (fill): r * fill = c
	float c = clamp(cp.L[li].coverage * cloudLatFactor(cp, li, lat, pfDir, sunPf) * CLOUD_COVERAGE_SCALE, 0.0, 1.0);
	float fill = max(cp.L[li].fillBase, c);
	float rg = c / max(fill, 1e-3);
	// soft edges: cloud fields thin out gradually toward their edges. The
	// edge stays inside 0..1 (e <= 2 rg and 2 (1 - rg)), so the mean of the
	// symmetric ramp, and thus the cover, is exact
	float e = min(0.6, 1.9 * min(rg, 1.0 - rg));
	float st = li == 0 ? max(smoothstep(0.85, 0.95, c), smoothstep(0.975, 0.995, U)) : 0.0;
	// cloud fraction across a system: overcast cores, broken toward the edges.
	// u is uniform over the system, so the linear ramp keeps the mean = fill
	float u = clamp((U - (1.0 - rg)) / max(rg, 1e-3), 0.0, 1.0);
	float ramp = min(1.0, 1.0 / max(fill, 1e-3) - 1.0);
	return vec3(smoothstep(1.0 - rg - 0.5 * e, 1.0 - rg + 0.5 * e, U), st, fill * (1.0 + ramp * (2.0 * u - 1.0)));
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
	// stratus: half the envelope, so twice the extinction keeps the optical
	// depth continuous with stratocumulus as cover approaches overcast
	if (li == 0) return mix(0.025, 0.05, st);
	if (li == 1) return 0.02;
	return 0.0002; // cirrus: semi-transparent veils, vertical optical depth ~0.05-0.3
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

// Large-scale cirrus structure: broad streak bands along the wind (~250 km
// across) seen from afar, and groups of wisps within them (~25 km across)
// closer in. Raw fBm value, roughly normal(0.5, 0.09). fp: pixel footprint [m].
float cloudCirrusWispField(CloudPlanet cp, vec3 q, float fp)
{
	float bands = cloudFbm(vec3(q.xy * 0.08, q.z * 0.006) + vec3(9.7), 2, 1.0);
	float groupAcross = CLOUD_CIRRUS_WIDTH_M * cp.cirrusScale / 0.06;
	float nearW = smoothstep(1.5, 4.0, groupAcross / max(fp, 1.0));
	if (nearW <= 0.0) return bands;
	float groups = cloudFbm(vec3(q.xy * 0.6, q.z * 0.06) + vec3(3.1), 2, 1.0);
	return mix(bands, 0.5 * (bands + groups), nearW);
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

// Scale of the erosion detail [m]: follows the cell size up to 4x the
// default, then stays fixed, so very large clouds keep a fine texture
float cloudDetailCell(CloudPlanet cp, int li)
{
	float base = li == 0 ? CLOUD_CELL_LOW_M : (li == 1 ? CLOUD_CELL_MID_M : CLOUD_CELL_HIGH_M);
	return min(cp.L[li].featureSize, 4.0 * base * cp.sH);
}

// Density of a field of unresolved cells (too small for the pixel): a
// uniform medium with the same mean transmittance as the broken field, so
// the visible amount of cloud does not change with distance or cell size.
// fLoc: local cloud area fraction, d: layer thickness [m].
float cloudUnresolvedRho(CloudPlanet cp, int li, float st, float fLoc, float prof, float d)
{
	float sig = cloudSigmaMax(li, st) * cp.L[li].sigmaScale;
	float colTau = sig * CLOUD_CELL_DENSITY * CLOUD_PROF_MEAN * d; // vertical optical depth through a cell
	float tEff = 1.0 - fLoc + fLoc * exp(-colTau);
	return prof * -log(max(tEff, 1e-4)) / max(sig * CLOUD_PROF_MEAN * d, 1e-6);
}

// Cell noise of the low and medium layers, advected with the wind and
// horizontal only, so a cell spans the whole column. Uniformly distributed:
// U > 1 - f covers exactly the fraction f of the area. Also returns the
// advected position p and the stratus sheet modulation.
float cloudCellU(CloudPlanet cp, int li, vec3 pfDir, float lat, float t, vec3 off, float st, float lod, out vec3 p, out float sheetMod)
{
	p = cloudAdvect(cp, li, pfDir, lat, t);
	vec3 q = p * (cp.a / cp.L[li].featureSize) + off;
	sheetMod = 1.0;
	if (li == 0) {
		// stratus: a continuous sheet with gentle density variation
		if (st > 0.001)
			sheetMod = mix(1.0, cloudFbm(q * 0.15, 2, lod) * 0.4 + 0.6, st);
		vec3 qs = q * vec3(1.0, 1.5, 1.0); // slight roll organisation along the zonal wind
		return st < 0.999 ? cloudUniformize(cloudPerlinWorley(qs, CLOUD_SHAPE_OCTAVES, lod), CLOUD_PW_MEAN, CLOUD_PW_SD) : 1.0;
	}
	// cell noise peaks at points (cell centres): thresholding it gives
	// round cells and a network of gaps; billow noise peaks along lines
	// and gave worm-like strands and holes
	vec3 qs = q * vec3(2.2, 1.8, 2.2); // rows across the wind
	return cloudUniformize(cloudPerlinWorley(qs, CLOUD_SHAPE_OCTAVES, lod), CLOUD_PW_MEAN, CLOUD_PW_SD);
}

// Density at a point given its column properties.
// Whether a cloud exists in a column is decided by a horizontal, uniformly
// distributed noise against the local cloud fraction, so the covered area
// equals the requested cover exactly; the height profile, domed tops and
// erosion only shape the clouds, they never remove them.
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
	// local cloud area fraction: inside-system weight x fraction within a system
	float fLoc = cLoc * mix(col.w, 1.0, st);
	// cheap path (slabs, shadows, debug): area fraction x profile
	if (!full) return vec3(fLoc * prof * floorF, st, h);

	if (li == 2) {
		// cirrus: fibrous streaks along the wind
		vec3 p = cloudAdvect(cp, li, pfDir, lat, t);
		// fall streaks: the lower, falling ice trails the wind (cirrus uncinus hooks)
		p = cloudRotY(p, (1.0 - h) * CLOUD_CIRRUS_FALL_M * cp.cirrusScale / cp.a);
		vec3 q = cloudCirrusDomain(cp, p, off);
		float wraw = cloudCirrusWispField(cp, q, fp);
		// unresolved fibres: a haze that keeps the wisp structure and the mean transmittance
		float rhoU = cloudUnresolvedRho(cp, li, st, fLoc, prof, d) * clamp(1.0 + 4.0 * (wraw - 0.5), 0.0, 2.0);
		float oct = cloudCirrusOctaves(cp, fp);
		if (oct <= 0.0) return vec3(rhoU * floorF, st, h);
		float U = cloudUniformize(cloudCirrusFibres(q, oct) + CLOUD_CIRRUS_WISP_WEIGHT * (wraw - 0.5), CLOUD_CIRRUS_G_MEAN, CLOUD_CIRRUS_G_SD);
		float thr = 1.0 - fLoc;
		float core = clamp((U - thr) / max(fLoc, 1e-3), 0.0, 1.0);
		float rhoR = smoothstep(thr - 0.03, thr + 0.03, U) * prof * (0.6 + 0.4 * core) / 0.8 * CLOUD_CELL_DENSITY;
		return vec3(mix(rhoU, rhoR, clamp(oct, 0.0, 1.0)) * floorF, st, h);
	}

	// cells smaller than a few pixels would only show as speckles: use the
	// transmittance-equivalent uniform medium instead
	float shapeFade = smoothstep(CLOUD_MIN_CELL_PIXELS, 3.0 * CLOUD_MIN_CELL_PIXELS, ly.featureSize / max(fp, 1.0));
	float rhoU = cloudUnresolvedRho(cp, li, st, fLoc, prof, d);
	if (shapeFade <= 0.0) return vec3(rhoU * floorF, st, h);

	vec3 p;
	float sheetMod;
	float U = cloudCellU(cp, li, pfDir, lat, t, off, st, lod, p, sheetMod);
	float ero = li == 0 ? mix(0.4, 0.1, st) : 0.45;

	// existence: U > 1 - fLoc covers exactly fLoc of the area
	float thr = 1.0 - fLoc;
	float edge = smoothstep(thr - 0.03, thr + 0.03, U);
	if (edge <= 0.0) return vec3(rhoU * (1.0 - shapeFade) * floorF, st, h);
	// rounded cells: toward the rim the top sinks and the base rises, and the
	// rim thins, so cells are lens-shaped rather than prisms; the full cell
	// area is kept at mid height, so the covered area is unchanged
	float core = clamp((U - thr) / max(fLoc, 1e-3), 0.0, 1.0);
	float sc = sqrt(core);
	float topH = mix(mix(0.5, 1.0, sc), 1.0, st);
	float botH = 0.4 * (1.0 - core) * (1.0 - st); // creases between merged cells
	float shape = (1.0 - smoothstep(topH - 0.2, topH, h)) * smoothstep(botH, botH + 0.1, h);
	float rim = mix(mix(0.25, 1.0, smoothstep(0.0, 0.3, core)), 1.0, st);
	float rho = edge * shape * rim * prof * sheetMod;
	if (detailW > 0.01) {
		// billowy detail roughens the cells, strongest toward their edges
		vec3 qd = (p * (cp.a / cloudDetailCell(cp, li)) + off) * 4.0 + vec3(3.1);
		float det = cloudUniformize(cloudBillowFbm(qd, CLOUD_DETAIL_OCTAVES, detailW), CLOUD_BILLOW_MEAN, CLOUD_BILLOW_SD);
		rho *= 1.0 - ero * detailW * det * (1.0 - core);
	}
	return vec3(mix(rhoU, rho, shapeFade) * floorF, st, h);
}

// Density blended over both time phases (column evaluated in place).
vec3 cloudDensity(CloudPlanet cp, int li, CloudPhase ph, vec3 pfDir, float alt, float lat, float hTerr,
	float lod, float detailW, bool full, vec3 sunPf, float fp)
{
	vec3 r = vec3(0.0);
	for (int k = cloudPhaseFirst(ph); k <= cloudPhaseLast(ph); k++) {
		float t, w;
		vec3 off;
		cloudPhaseK(ph, k, t, off, w);
		vec4 col = cloudColumn(cp, li, pfDir, alt, lat, t, off, sunPf, fp, 5.0);
		r += w * cloudShapeDensity(cp, li, pfDir, alt, lat, hTerr, t, off, col, lod, detailW, full, fp);
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

		// multiple scattering octaves (Wrenninge 2013); light is scattered more
		// than once only in optically thick cloud, so thin veils (cirrus) get
		// little more than the single-scattering octave
		float mu = dot(rayDir, L);
		vec3 ms = vec3(0.0);
		float msW = 1.0 - exp(-sigmaCol * d);
		float a = 1.0, b = 1.0, c = 1.0;
		for (int o = 0; o < 3; o++) {
			ms += (o == 0 ? 1.0 : msW) * a * exp(-tauSun * b) * cloudPhaseFn(ly, mu, c);
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
			// the weather column hardly changes across one crossing of the thin
			// layer: evaluate it once at the middle, for both phases
			vec4 cirCol[2];
			cirCol[0] = vec4(0.0);
			cirCol[1] = vec4(0.0);
			{
				float rm = length(midPos);
				vec3 mDir = cloudToPf(midPos) / rm;
				float mLat = asin(clamp(mDir.y, -1.0, 1.0));
				float fpMid = max(0.5 * (t0 + t1) * cp.a, 1.0) * pixelAngle;
				for (int k = cloudPhaseFirst(ph); k <= cloudPhaseLast(ph); k++) {
					float tk, wk;
					vec3 offk;
					cloudPhaseK(ph, k, tk, offk, wk);
					cirCol[k] = cloudColumn(cp, li, mDir, (rm - 1.0) * cp.a, mLat, tk, offk, sunPf, fpMid, 5.0);
				}
			}
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
				float hTerrC = cloudTerrainHeight(cp, pfDir);
				vec3 dens = vec3(0.0);
				for (int k = cloudPhaseFirst(ph); k <= cloudPhaseLast(ph); k++) {
					float tk, wk;
					vec3 offk;
					cloudPhaseK(ph, k, tk, offk, wk);
					dens += wk * cloudShapeDensity(cp, li, pfDir, alt, lat, hTerrC, tk, offk, cirCol[k], 1.0, 0.0, true, fp);
				}
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
				for (int k = cloudPhaseFirst(ph); k <= cloudPhaseLast(ph); k++) {
					float tk, wk;
					vec3 offk;
					cloudPhaseK(ph, k, tk, offk, wk);
					vec4 cc = cloudColumn(cp, li, cdir, calt, clat, tk, offk, sunPf, fpCol, 3.0);
					if (k == 0)
						colA[c] = cc;
					else
						colB[c] = cc;
				}
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
					float detailW = clamp(log2(cloudDetailCell(cp, li) * 0.25 / (2.0 * fp)), 0.0, 1.0);
					vec3 dens = vec3(0.0);
					for (int k = cloudPhaseFirst(ph); k <= cloudPhaseLast(ph); k++) {
						float tk, wk;
						vec3 offk;
						cloudPhaseK(ph, k, tk, offk, wk);
						dens += wk * cloudShapeDensity(cp, li, pfDir, alt, lat, hTerr, tk, offk, k == 0 ? cA : cB, lod, detailW, true, fp);
					}
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

// two-stream total (direct + diffuse) transmission, g = 0.85: thick cloud
// still passes ~30-40% of the light
float cloudTwoStream(float tau)
{
	return 1.0 / (1.0 + 0.75 * tau * (1.0 - 0.85));
}

// Light reaching the ground through one layer in one time phase.
// slant: 1 / cos of the sun zenith angle at the layer. A broken field passes
// (1 - f) + f * T(cell): averaging the optical depth instead would make
// distant shadows far too weak (or, spread as a uniform deck, far too dark).
float cloudShadowPhase(CloudPlanet cp, int li, vec3 pfDir, float alt, float lat, float t, vec3 off, vec3 sunPf, float fp, float slant)
{
	CloudLayer ly = cp.L[li];
	vec4 col = cloudColumn(cp, li, pfDir, alt, lat, t, off, sunPf, li == 2 ? max(fp, 20000.0) : fp, 3.0);
	if (col.x <= 0.0) return 1.0;
	float st = col.y;
	float sig = cloudSigmaMax(li, st) * ly.sigmaScale;
	float colD = CLOUD_PROF_MEAN * (ly.top - ly.base) * slant;
	float fLoc = col.x * mix(col.w, 1.0, st);
	float tUnres = 1.0 - fLoc + fLoc * cloudTwoStream(sig * CLOUD_CELL_DENSITY * colD);
	if (li == 2 || cloudTuneDensity.w < 1.5) return tUnres; // cirrus fibres are too thin to cast distinct shadows
	float sf = smoothstep(CLOUD_MIN_CELL_PIXELS, 3.0 * CLOUD_MIN_CELL_PIXELS, ly.featureSize / max(fp, 1.0));
	if (sf <= 0.0) return tUnres;
	// resolved: the footprint of each cell (the same noise as the clouds),
	// thicker toward the cell core like the domed cells themselves
	float lod = clamp(log2(ly.featureSize / (2.0 * fp)) - float(CLOUD_SHAPE_OCTAVES - 1), 0.0, 1.0);
	vec3 p;
	float sheetMod;
	float U = cloudCellU(cp, li, pfDir, lat, t, off, st, lod, p, sheetMod);
	float thr = 1.0 - fLoc;
	float edge = smoothstep(thr - 0.03, thr + 0.03, U);
	float core = clamp((U - thr) / max(fLoc, 1e-3), 0.0, 1.0);
	float thick = mix(mix(0.4, 1.2, sqrt(core)), 1.0, st) * sheetMod;
	float tRes = 1.0 - edge + edge * cloudTwoStream(sig * CLOUD_CELL_DENSITY * colD * thick);
	return mix(tUnres, tRes, sf);
}

// Cloud shadow factor on terrain at pos for light direction L.
// fp: footprint of the terrain pixel [m]. Near the camera the shadow follows
// the individual cells, using the same column and cell noise as the clouds
// so they line up; where cells are too small to see, the average of the
// broken field. Cost: one column (weather fBm) per layer, plus one cell
// noise per layer where cells are resolved.
float cloudShadowTerrain(CloudPlanet cp, vec3 pos, vec3 L, float fp)
{
	// cloudTuneDensity.w: shadow mode, 0 off, 1 soft (field average), 2 detailed
	if (cp.enabled < 0.5 || cloudTuneDensity.w < 0.5) return 1.0;
	float T = 1.0;
	vec3 sunPf = cloudToPf(L);
	for (int li = 0; li < 3; li++) {
		if (cp.L[li].active < 0.5) continue;
		CloudLayer ly = cp.L[li];
		float altMid = cloudSlabAlt(ly, li);
		vec2 hit = cloudSphereHit(pos, L, 1.0 + altMid / cp.a);
		if (hit.y <= 0.0) continue;
		vec3 lp = pos + L * max(hit.y, 0.0);
		vec3 pfDir = normalize(cloudToPf(lp));
		float lat = asin(clamp(pfDir.y, -1.0, 1.0));
		float slant = 1.0 / max(dot(normalize(lp), L), 0.1);
		CloudPhase ph = cloudPhases(ly.period, float(li));
		float tl = 0.0;
		for (int k = cloudPhaseFirst(ph); k <= cloudPhaseLast(ph); k++) {
			float tk, wk;
			vec3 offk;
			cloudPhaseK(ph, k, tk, offk, wk);
			tl += wk * cloudShadowPhase(cp, li, pfDir, altMid, lat, tk, offk, sunPf, fp, slant);
		}
		T *= tl;
	}
	return T;
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
