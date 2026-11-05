// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainPhysicalCraters.h"

#include "MathUtil.h"
#include "SphereCells.h"

#include "../Random.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
	// lattices take over from the list below this share of the radius
	constexpr double LATTICE_BELOW_RADIUS = 1.0 / 16.0;
	// how far a crater reaches, in crater radii; the youngest throw rays further
	constexpr double REACH = 2.8;
	constexpr double REACH_RAYS = 7.5;
	constexpr double RAYS_FROM = 0.93;

	std::atomic<uint64_t> s_nextId(1);

	double Clamp01(const double x)
	{
		return std::clamp(x, 0.0, 1.0);
	}

	double SmoothEdge(const double t)
	{
		return t * t * (3.0 - 2.0 * t);
	}

	double Soft(const double x, const double low, const double width)
	{
		return SmoothEdge(Clamp01((x - low) / width));
	}

	// Craters per square km of at least this diameter, in km: the Hartmann saturation line
	double Saturated(const double diameter)
	{
		return 0.046 * std::pow(diameter, -1.83);
	}

	// An irregular, partly polygonal outline, as real rims are: slumps and scallops
	double Outline(const double theta, const double seed, const double amount)
	{
		const double k = seed * 2.0 * M_PI;
		const double w = 0.55 * std::sin(2.0 * theta + k * 3.1) + 0.5 * std::sin(3.0 * theta + k * 7.3) + 0.4 * std::sin(5.0 * theta + k * 11.9) +
			0.22 * std::sin(9.0 * theta + k * 17.7) + 0.12 * std::sin(17.0 * theta + k * 29.3);
		return 1.0 + amount * w;
	}

	// Star-shaped debris: uneven lobes around the crater
	double Spokes(const double theta, const double seed, const int n)
	{
		const double k = seed * 2.0 * M_PI;
		const double a = std::abs(std::sin(theta * n / 2.0 + k * 5.7 + 0.8 * std::sin(theta * 3.0 + k * 9.0)));
		const double b = std::abs(std::sin(theta * (n * 2 + 1) / 2.0 + k * 13.1));
		const double c = std::abs(std::sin(theta * (n * 5 + 2) / 2.0 + k * 3.3));
		return std::clamp(0.55 * std::pow(a, 1.4) + 0.3 * b * b + 0.12 * c * c + 0.22 * std::sin(theta * 2.0 + k), 0.0, 1.1);
	}

	vector3d RandomDirection(Random &rng)
	{
		const double y = 2.0 * rng.Double() - 1.0;
		const double angle = 2.0 * M_PI * rng.Double();
		const double r = std::sqrt(std::max(0.0, 1.0 - y * y));
		return vector3d(r * std::cos(angle), y, r * std::sin(angle));
	}
} // namespace

void TerrainCraters::Build(const PlanetPhysics::Core &core, const PlanetPhysics::Interior &it, const PlanetPhysics::Surface &st, const uint32_t noiseSeed)
{
	m_id = s_nextId++;
	m_seed = noiseSeed;
	m_radiusKm = core.radius * 6378.135;
	m_transition = std::max(it.transitionDiameter, 0.01);
	m_reliefLimit = std::max(it.reliefLimit, 1.0);
	m_wearLarge = st.wearLarge;
	m_wearSmall = st.wearSmall;
	m_gullies = st.wear;
	m_flatten = it.plates ? 0.15 : 1.0;
	if (it.cratering <= 0.0 && !it.basin)
		return;

	Random rng(core.seed ^ 0xA5A5u);
	const double area = 4.0 * M_PI * m_radiusKm * m_radiusKm;
	const double smallestListed = m_radiusKm * LATTICE_BELOW_RADIUS;
	// wet worlds lose most of their small craters
	auto survives = [&](const double diameter, const double seed) {
		const double chance = 1.0 - 0.97 * m_wearSmall * (1.0 - Soft(diameter, 30.0, 40.0));
		return std::fmod(seed * 9973.0, 1.0) < chance;
	};
	auto randomCrater = [&](const double diameter) {
		Crater c;
		c.centre = RandomDirection(rng);
		c.diameter = diameter / 2.0 * (1.0 + rng.Double());
		const double u = rng.Double();
		c.fresh = u * u * u;
		c.seed = rng.Double();
		const double angle = rng.Double() * 2.0 * M_PI;
		c.oblique = rng.Double() < 0.1 ? angle : -1.0;
		return c;
	};

	double diameter = std::min(2.0 * m_radiusKm * 0.45, 2500.0);
	if (it.cratering > 0.0) {
		while (diameter > smallestListed) {
			const double expected = (Saturated(diameter / 2.0) - Saturated(diameter)) * area * it.cratering;
			const int count = int(std::min(expected, 3000.0) + rng.Double());
			for (int i = 0; i < count; i++) {
				const Crater c = randomCrater(diameter);
				if (survives(c.diameter, c.seed))
					m_listed.push_back(c);
			}
			diameter /= 2.0;
		}
		int index = 0;
		while (diameter > SMALLEST_KM) {
			Size size;
			size.diameter = diameter;
			size.density = (Saturated(diameter / 2.0) - Saturated(diameter)) * it.cratering;
			size.cell = std::max(1.5 * diameter, std::sqrt(1.5 / std::max(size.density, 1e-12)));
			size.seed = noiseSeed + 7000u + uint32_t(index++) * 100u;
			m_sizes.push_back(size);
			diameter /= 2.0;
		}
	}
	// one great basin, on bodies whose old crust has one
	if (it.basin) {
		Crater c;
		c.centre = RandomDirection(rng);
		c.diameter = m_radiusKm * (0.5 + 0.4 * rng.Double());
		c.fresh = 0.3;
		c.seed = rng.Double();
		c.oblique = -1.0;
		m_listed.push_back(c);
	}
}

void TerrainCraters::AddSite(const Crater &crater, const double spacing, std::vector<Site> &out) const
{
	const double angle = crater.diameter / 2.0 / m_radiusKm;
	double fade = 1.0;
	if (spacing > 0.0) {
		fade = Clamp01((crater.diameter / m_radiusKm / spacing - 4.0) / 4.0);
		if (fade <= 0.0)
			return;
	}

	Site s;
	s.centre = crater.centre;
	const vector3d helper = std::abs(crater.centre.y) < 0.9 ? vector3d(0.0, 1.0, 0.0) : vector3d(1.0, 0.0, 0.0);
	s.axisA = crater.centre.Cross(helper).Normalized();
	s.axisB = crater.centre.Cross(s.axisA);
	s.angle = angle;
	s.fade = fade;
	s.diameter = crater.diameter;
	s.fresh = crater.fresh;
	s.seed = crater.seed;
	s.oblique = crater.oblique;
	s.ratio = crater.diameter / m_transition;
	s.large = crater.diameter > 0.05 * m_radiusKm;
	s.reach = crater.fresh > RAYS_FROM ? REACH_RAYS : REACH;
	s.reachAngle = std::min(s.reach * angle, M_PI);
	s.cosReach = std::cos(s.reachAngle);
	s.sinReach = std::sin(s.reachAngle);

	// old craters are shallow and broken even with no air; wear from air and water adds to that
	const double age = std::pow(1.0 - crater.fresh, 2.0) * std::min(1.0, 0.35 + 8.0 / std::max(crater.diameter, 1.0)) * 0.75;
	s.wear = 1.0 - (1.0 - age) * (1.0 - (s.large ? m_wearLarge : m_wearSmall));
	const double metres = crater.diameter * 1000.0;
	const double depth = std::min(s.ratio <= 1.0 ? 0.2 * metres : 0.2 * metres * std::pow(s.ratio, -0.65), 0.7 * m_reliefLimit);
	s.depth = depth;
	s.wornDepth = depth * (1.0 - 0.72 * s.wear);
	s.rim = (s.ratio <= 1.0 ? 0.18 : 0.13) * depth * (1.0 - 0.85 * s.wear);
	out.push_back(s);
}

void TerrainCraters::Gather(const vector3d &centre, const double radius, const double spacing, std::vector<Site> &out) const
{
	out.clear();
	for (const Crater &c : m_listed) {
		const double reach = radius + (c.fresh > RAYS_FROM ? REACH_RAYS : REACH) * c.diameter / 2.0 / m_radiusKm;
		if (reach >= M_PI || centre.Dot(c.centre) >= std::cos(reach))
			AddSite(c, spacing, out);
	}

	const vector3d place = centre * m_radiusKm;
	for (const Size &size : m_sizes) {
		// (none of this size can show at this spacing, nor can smaller ones)
		if (spacing > 0.0 && size.diameter / m_radiusKm / spacing < 4.0)
			break;
		const double cell = size.cell;
		const double around = radius * m_radiusKm + REACH_RAYS * size.diameter / 2.0 + cell;
		const int64_t x0 = int64_t(std::floor((place.x - around) / cell)), x1 = int64_t(std::floor((place.x + around) / cell));
		const int64_t y0 = int64_t(std::floor((place.y - around) / cell)), y1 = int64_t(std::floor((place.y + around) / cell));
		const int64_t z0 = int64_t(std::floor((place.z - around) / cell)), z1 = int64_t(std::floor((place.z + around) / cell));
		const double chance = size.density * cell * cell;
		for (int64_t x = x0; x <= x1; x++) {
			for (int64_t y = y0; y <= y1; y++) {
				for (int64_t z = z0; z <= z1; z++) {
					// one site for each cell the surface passes through
					const vector3d middle((double(x) + 0.5) * cell, (double(y) + 0.5) * cell, (double(z) + 0.5) * cell);
					if (std::abs(middle.Length() - m_radiusKm) > 0.5 * cell)
						continue;
					auto draw = [&](const uint32_t k) { return SphereCells::Hash(x, y, z, size.seed + k) * 0.5 + 0.5; };
					if (draw(0) >= chance)
						continue;
					Crater c;
					const vector3d jitter(SphereCells::Hash(x, y, z, size.seed + 11), SphereCells::Hash(x, y, z, size.seed + 12), SphereCells::Hash(x, y, z, size.seed + 13));
					c.centre = (middle + jitter * (0.45 * cell)).Normalized();
					c.diameter = size.diameter / 2.0 * (1.0 + draw(1));
					const double u = draw(2);
					c.fresh = u * u * u;
					c.seed = draw(3);
					// wet worlds lose most of their small craters
					if (std::fmod(c.seed * 9973.0, 1.0) >= 1.0 - 0.97 * m_wearSmall * (1.0 - Soft(c.diameter, 30.0, 40.0)))
						continue;
					c.oblique = draw(5) < 0.1 ? draw(6) * 2.0 * M_PI : -1.0;
					const double reach = radius + (c.fresh > RAYS_FROM ? REACH_RAYS : REACH) * c.diameter / 2.0 / m_radiusKm;
					if (centre.Dot(c.centre) >= std::cos(std::min(reach, M_PI)))
						AddSite(c, spacing, out);
				}
			}
		}
	}
}

void TerrainCraters::Accumulate(const Site &s, const vector3d &p, Sample &out) const
{
	const double cosine = p.Dot(s.centre);
	if (cosine < s.cosReach)
		return;
	const double t0 = std::acos(std::min(cosine, 1.0)) / s.angle;
	const double theta = std::atan2(p.Dot(s.axisB), p.Dot(s.axisA));

	// The outline is rounder when fresh and small, more scalloped when large or old. It bends the rim
	// and the ground beyond it only: towards the middle it would fan out into star lines
	const double amount = 0.045 + 0.07 * std::min(s.ratio / 8.0, 1.0) + 0.10 * s.wear;
	const double outward = Clamp01(t0 / 0.6);
	double t = t0 / (1.0 + (Outline(theta, s.seed, amount) - 1.0) * outward);
	if (s.oblique >= 0.0)
		t /= 1.0 + 0.28 * std::cos(2.0 * (theta - s.oblique)) * outward;

	const double wear = s.wear;
	const double rimShape = std::clamp(1.15 + (0.25 + 0.8 * wear) * std::sin(3.0 * theta + s.seed * 40.0) + 0.15 * std::sin(7.0 * theta + s.seed * 90.0), 0.0, 2.0);
	const double rim = s.rim * rimShape;
	double h;
	if (t >= 1.0) {
		// the outer slope reaches exactly zero at the end of the crater's reach
		const double tail = SmoothEdge(Clamp01((s.reach - t0) / (0.35 * s.reach)));
		// the rim's unevenness stays on its crest: carried down the outer slope, it draws radial ribs
		const double beyond = t - 1.0;
		h = s.rim * (1.0 + (rimShape - 1.0) * std::exp(-beyond * 6.0)) * std::exp(-beyond * 3.4) * tail;
	} else if (s.ratio <= 1.0) {
		h = rim - (s.wornDepth + rim) * (1.0 - t * t);
	} else {
		const double floorEdge = std::min(0.72, 0.42 + 0.08 * std::log2(std::max(s.ratio, 1.0)));
		double q = Clamp01((1.0 - t) / (1.0 - floorEdge));
		if (s.ratio > 2.0) {
			// terraces, stronger when fresh
			const double gate = Clamp01(0.6 + std::sin(theta * 3.0 + s.seed * 20.0) + 0.6 * std::sin(theta * 7.0 + s.seed * 50.0));
			q += (1.0 - 0.8 * wear) * 0.035 * std::sin(q * 2.0 * M_PI * (2 + int(s.seed * 2.0))) * gate * Clamp01((0.75 - q) / 0.1);
		}
		h = rim - (s.wornDepth + rim) * SmoothEdge(Clamp01(q));
		if (s.ratio < 7.0) {
			// a central peak: lumps fixed in space, as anything shaped by the angle meets at the middle as a star
			if (t < 0.4) {
				const double lumps = SphereCells::Noise(p * (16.0 / s.angle), uint32_t(s.seed * 1e6));
				h += std::exp(-(t / 0.13) * (t / 0.13)) * (0.55 + 0.3 * lumps) * std::min(0.6, 0.25 * std::pow(s.ratio, 0.35)) * s.depth * (1.0 - 0.6 * wear);
			}
		} else if (s.ratio < 15.0) {
			// peak ring
			const double e = (t - 0.5) / 0.05;
			h += std::exp(-e * e) * (0.6 + 0.4 * std::sin(7.0 * theta + s.seed * 30.0)) * 0.28 * s.depth * (1.0 - 0.6 * wear);
		}
	}
	if (s.ratio >= 15.0) {
		// rings outside the basin
		// (wide, low and broken: thin unbroken ones read as contour lines from orbit)
		const double e1 = (t - 1.4) / 0.09, e2 = (t - 1.95) / 0.12;
		const double ring = 0.1 * std::exp(-e1 * e1) + 0.07 * std::exp(-e2 * e2);
		const double gaps = Clamp01(0.5 + std::sin(2.0 * theta + s.seed * 50.0) + 0.6 * std::sin(5.0 * theta + s.seed * 77.0));
		h += ring * gaps * s.depth * (1.0 - 0.6 * wear);
	}
	if (m_gullies > 0.0 && std::abs(t - 0.85) < 0.25) {
		// wear cuts radial gullies into the walls, unevenly, in groups with gaps, so they don't read as a comb
		const double g = std::abs(std::sin(theta * (18 + int(s.seed * 14.0)) + 3.0 * std::sin(theta * 3.0 + s.seed * 9.0) + 2.2 * std::sin(theta * 11.0 + s.seed * 37.0)));
		const double groups = Clamp01(0.45 + 0.9 * std::sin(theta * 4.0 + s.seed * 23.0) + 0.5 * std::sin(theta * 9.0 + s.seed * 61.0));
		h -= m_gullies * s.depth * 0.12 * std::min(1.0, std::sqrt(60.0 / std::max(s.diameter, 1.0))) * g * g * g * groups * (1.0 - std::abs(t - 0.85) / 0.25);
	}
	h *= s.fade * m_flatten;
	if (s.large)
		out.large += h;
	else
		out.small += h;

	// old floors gather dust
	if (s.fresh < 0.1 && s.diameter < 0.15 * m_radiusKm)
		out.floor = std::max(out.floor, Soft((s.ratio > 1.0 ? 0.62 : 0.4) - t, 0.0, 0.18) * s.fade);
	// star-shaped debris near the crater; fresh ones throw long rays
	if (s.fresh > 0.7) {
		const int lobes = 4 + int(s.seed * 6.0) + (s.fresh > 0.8 ? 3 : 0);
		double spokes = Spokes(theta, s.seed, lobes);
		// none uprange, wings to the sides
		if (s.oblique >= 0.0)
			spokes *= Clamp01(0.2 + std::cos(theta - s.oblique));
		const double edge = 1.2 + 1.1 * spokes * (0.4 + s.fresh);
		double debris = Clamp01((edge - t0) / std::max(edge - 1.0, 0.05)) * Clamp01((t0 - 0.8) / 0.3) * std::min(1.0, (s.fresh - 0.7) * 3.3);
		if (debris > 0.0)
			debris *= Clamp01(0.7 + 0.5 * SphereCells::Noise(p * (5.0 / s.angle), uint32_t(s.seed * 1e6) + 1u));
		out.ejecta = std::max(out.ejecta, debris * s.fade);
		if (s.fresh > RAYS_FROM) {
			const double ray = std::pow(Spokes(theta, s.seed * 0.7 + 0.1, 7 + int(s.seed * 8.0)), 5.0);
			const double tail = SmoothEdge(Clamp01((s.reach - t0) / (0.35 * s.reach)));
			if (ray > 0.01) {
				const double broken = Clamp01(0.5 + 0.7 * SphereCells::Noise(p * (3.0 / s.angle), uint32_t(s.seed * 1e6) + 2u));
				out.rays = std::max(out.rays, ray * broken * std::exp(-(t0 - 1.0) / 2.0) * Clamp01((t0 - 0.9) / 0.3) * (s.fresh - RAYS_FROM) * 14.0 * tail * s.fade);
			}
		}
	}
}

TerrainCraters::Sample TerrainCraters::At(const vector3d &p) const
{
	Sample out;
	if (m_listed.empty() && m_sizes.empty())
		return out;

	// Queries come in runs of nearby points, so the craters around them are kept per thread, for the few
	// band limits a patch uses in turn. The radius covers a whole grid of two patches from its corner.
	thread_local Nearby views[3];
	thread_local uint32_t clock = 0;
	const double band = SphereCells::RoundedBand(TerrainNoise::s_maxFrequency);
	Nearby *view = nullptr;
	for (Nearby &n : views) {
		if (n.owner == m_id && n.band == band && p.Dot(n.centre) >= n.cosRadius) {
			view = &n;
			break;
		}
	}
	if (!view) {
		view = &views[0];
		for (Nearby &n : views) {
			if (n.used < view->used)
				view = &n;
		}
		const double spacing = band > 0.0 ? 0.5 / band : 0.0;
		const double radius = std::min(std::max(100.0 * spacing, 4.0 * SMALLEST_KM / m_radiusKm), M_PI);
		Gather(p, radius, spacing, view->sites);
		view->owner = m_id;
		view->band = band;
		view->centre = p;
		view->cosRadius = std::cos(radius);
		view->cosMargin = 2.0;
		view->margin = radius / 8.0;
	}
	view->used = ++clock;
	if (p.Dot(view->activeCentre) < view->cosMargin) {
		const double cosMargin = std::cos(view->margin), sinMargin = std::sin(view->margin);
		view->active.clear();
		for (const Site &site : view->sites) {
			if (site.reachAngle + view->margin >= M_PI || p.Dot(site.centre) >= site.cosReach * cosMargin - site.sinReach * sinMargin)
				view->active.push_back(&site);
		}
		view->activeCentre = p;
		view->cosMargin = cosMargin;
	}
	for (const Site *site : view->active)
		Accumulate(*site, p, out);
	return out;
}
