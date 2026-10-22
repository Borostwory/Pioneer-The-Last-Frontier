// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Closest-approach planning for the transfer planner.
//
// A target that orbits the planner's central body is modelled as a track that
// answers its position and velocity at any game time. PlanApproach searches
// departure time x flight time with a Lambert solver for the transfer that
// meets the target for the least delta-v, and appends it as one burn
// (intercept) or two (rendezvous). GetClosestApproach scans the planned
// trajectory against the same track.
//
// Everything is two-body, around the planner frame's body, like the rest of
// the planner: a moon's own gravity, or leaving the frame, is not modelled.

#include "TransferPlanner.h"

#include "Body.h"
#include "FloatComparison.h"
#include "Frame.h"
#include "Game.h"
#include "Orbit.h"
#include "Pi.h"
#include "Player.h"
#include "galaxy/SystemBody.h"
#include "gameconsts.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

// the earliest departure after the plan's end, so there is time to turn the ship
constexpr double kDepartureLead = 120.0;
// the departure search spans at most this many revolutions of the slower orbit
constexpr double kMaxWaitRevolutions = 10.0;
// flight times searched, as fractions of the shorter / longer orbital period
constexpr double kMinFlightFraction = 0.05;
constexpr double kMaxFlightFraction = 0.6;
// coarse search grid: departure times x flight times
constexpr int kDepartureSteps = 96;
constexpr int kFlightSteps = 64;
// a transfer may not pass closer than this to the central body, in its radii,
// unless it starts or ends lower (see DipsBelow)
constexpr double kMinRadiusFactor = 1.05;
// Lambert transfers whose angle has a smaller sine are refused (see SolveLambert)
constexpr double kMinTransferSine = 2e-3;
// a planet or moon is passed this many of its radii from its centre
constexpr double kBodyAimFactor = 2.0;
// closest-approach scan: samples over the span, which is capped
constexpr int kApproachSamples = 1000;
constexpr double kMaxApproachSpan = 365.25 * 24 * 3600;
// the fallback span when no orbit involved is periodic
constexpr double kDefaultSpan = 24 * 3600;

//
// Where a target is at a game time, relative to the planner frame. A system
// body (moon, orbital station) rides its orbit rails, which is exact; a ship
// is assumed to coast on the orbit of its current state.
//
struct TargetTrack {
	const SystemBody *rails = nullptr;
	Orbit orbit;
	double epoch = 0.0;
	double mass = 0.0;	 // of the central body
	double radius = 0.0; // a planet's or moon's, 0 for ships and stations
	double period = 0.0; // 0 when not periodic
	// a stand-off: the track runs this many seconds behind the target, so it
	// follows the target's own orbit at a distance behind it
	double lag = 0.0;

	vector3d PosAt(double t) const
	{
		// the rails are what Frame::UpdateOrbitRails places the body's frame by
		const double when = t - lag;
		return rails ? rails->GetOrbit().OrbitalPosAtTime(when) : orbit.OrbitalPosAtTime(when - epoch);
	}

	vector3d VelAt(double t) const
	{
		if (rails)
			return (PosAt(t + 1.0) - PosAt(t - 1.0)) * 0.5;
		return orbit.OrbitalVelocityAtTime(mass, t - lag - epoch);
	}
};

double PeriodOf(const Orbit &orbit)
{
	if (is_zero_general(orbit.GetSemiMajorAxis()) || orbit.GetEccentricity() >= 1.0)
		return 0.0;
	return orbit.Period();
}

bool MakeTargetTrack(const Body *target, const Frame *frame, TargetTrack &track, std::string &error)
{
	const SystemBody *centre = frame->GetSystemBody();
	track.mass = centre->GetMass();

	if (!target) {
		error = "No target: select one on the map or set a navigation target";
		return false;
	}
	if (target == frame->GetBody() || target == Pi::player) {
		error = "Choose a target other than your ship and the body you orbit";
		return false;
	}

	// moons and orbital stations of the central body ride their rails
	const SystemBody *sbody = target->GetSystemBody();
	if (sbody && sbody->GetParent() == centre) {
		if (sbody->GetType() == SystemBody::TYPE_STARPORT_SURFACE) {
			error = "A surface port turns with the planet and cannot be met in orbit";
			return false;
		}
		track.rails = sbody;
		track.period = PeriodOf(sbody->GetOrbit());
		const SystemBody::BodySuperType superType = sbody->GetSuperType();
		if (superType == SystemBody::SUPERTYPE_ROCKY_PLANET || superType == SystemBody::SUPERTYPE_GAS_GIANT ||
			superType == SystemBody::SUPERTYPE_STAR)
			track.radius = sbody->GetRadius();
		return true;
	}

	// ships flying in the planner's frame coast on their current orbit
	const Frame *targetFrame = Frame::GetFrame(target->GetFrame());
	if (!sbody && targetFrame && targetFrame->GetNonRotFrame() == frame->GetId()) {
		track.epoch = Pi::game->GetTime();
		track.orbit = Orbit::FromBodyState(target->GetPositionRelTo(frame->GetId()),
			target->GetVelocityRelTo(frame->GetId()), track.mass);
		if (is_zero_general(track.orbit.GetSemiMajorAxis())) {
			error = "The target is not in orbit";
			return false;
		}
		track.period = PeriodOf(track.orbit);
		return true;
	}

	error = "The target must orbit " + centre->GetName() + ", the body you orbit";
	return false;
}

// Stumpff functions C(z) and S(z) of the universal-variable formulation
double StumpffC(double z)
{
	if (z > 1e-6)
		return (1.0 - std::cos(std::sqrt(z))) / z;
	if (z < -1e-6)
		return (std::cosh(std::sqrt(-z)) - 1.0) / -z;
	return 0.5 - z / 24.0 + z * z / 720.0;
}

double StumpffS(double z)
{
	if (z > 1e-6) {
		const double s = std::sqrt(z);
		return (s - std::sin(s)) / (s * s * s);
	}
	if (z < -1e-6) {
		const double s = std::sqrt(-z);
		return (std::sinh(s) - s) / (s * s * s);
	}
	return 1.0 / 6.0 - z / 120.0 + z * z / 5040.0;
}

//
// Lambert's problem, single revolution, universal variables (Curtis,
// Orbital Mechanics for Engineering Students, algorithm 5.2), solved by
// bisection on z, along which the flight time grows monotonically. The
// transfer turns the same way as hRef, the departure orbit's angular
// momentum. transferAngle is the angle swept on the way, in [0, 2pi).
//
bool SolveLambertImpl(const vector3d &r1, const vector3d &r2, double tof, double mu, const vector3d &hRef,
	vector3d &v1, vector3d &v2, double &transferAngle)
{
	const double r1n = r1.Length();
	const double r2n = r2.Length();
	const double cosAngle = std::min(1.0, std::max(-1.0, r1.Dot(r2) / (r1n * r2n)));
	transferAngle = std::acos(cosAngle);
	if (r1.Cross(r2).Dot(hRef) < 0.0)
		transferAngle = 2.0 * M_PI - transferAngle;
	// The formulation is singular where the transfer plane is undefined: ends
	// in line with the body (0 or 180 degrees apart). Closer than about 0.1
	// degree to that its answer misses the target badly, and a search for the
	// least delta-v would home in on exactly that, so those are refused - the
	// cheapest real transfer lies just beside them.
	if (1.0 - cosAngle < 1e-12 || std::fabs(std::sin(transferAngle)) < kMinTransferSine)
		return false;

	const double A = std::sin(transferAngle) * std::sqrt(r1n * r2n / (1.0 - cosAngle));

	const double sqrtMu = std::sqrt(mu);
	const auto y = [&](double z) {
		return r1n + r2n + A * (z * StumpffS(z) - 1.0) / std::sqrt(StumpffC(z));
	};
	// flight-time error at z; a z with y < 0 is unreachable and counts as too short
	const auto timeError = [&](double z) {
		const double yz = y(z);
		if (yz < 0.0)
			return -sqrtMu * tof;
		return std::pow(yz / StumpffC(z), 1.5) * StumpffS(z) + A * std::sqrt(yz) - sqrtMu * tof;
	};

	double lo = -4.0 * M_PI * M_PI;
	double hi = 4.0 * M_PI * M_PI - 1e-6;
	while (timeError(lo) > 0.0) {
		lo *= 2.0;
		if (lo < -1e6)
			return false;
	}
	if (timeError(hi) < 0.0)
		return false;
	for (int i = 0; i < 200 && hi - lo > 1e-10; ++i) {
		const double mid = 0.5 * (lo + hi);
		if (timeError(mid) < 0.0)
			lo = mid;
		else
			hi = mid;
	}

	const double z = 0.5 * (lo + hi);
	const double yz = y(z);
	if (yz <= 0.0)
		return false;
	const double f = 1.0 - yz / r1n;
	const double g = A * std::sqrt(yz / mu);
	const double gDot = 1.0 - yz / r2n;
	if (std::fabs(g) < 1e-12)
		return false;
	v1 = (r2 - f * r1) / g;
	v2 = (gDot * r2 - r1) / g;
	return std::isfinite(v1.x) && std::isfinite(v1.y) && std::isfinite(v1.z);
}

// Whether the arc from r1 to r2 (leaving at v1, sweeping transferAngle)
// passes through a periapsis too close to the central body. The limit is a
// margin over the surface, but never above the arc's own ends: a ship already
// in a low orbit may leave it, and the periapsis of a transfer that starts at
// its periapsis is the departure point itself.
bool DipsBelow(const vector3d &r1, const vector3d &v1, const vector3d &r2, double transferAngle, double mu,
	double bodyRadius)
{
	const double ends = std::min(r1.Length(), r2.Length()) * (1.0 - 1e-6);
	const double minRadius = std::max(bodyRadius, std::min(bodyRadius * kMinRadiusFactor, ends));

	const vector3d h = r1.Cross(v1);
	const vector3d eccentricity = v1.Cross(h) / mu - r1.Normalized();
	const double e = eccentricity.Length();
	const double periapsis = h.LengthSqr() / mu / (1.0 + e);
	if (periapsis >= minRadius)
		return false;
	if (e < 1e-9)
		return true;
	// the true anomaly at departure; the arc holds the periapsis when it
	// sweeps past anomaly 2pi
	double anomaly = std::atan2(eccentricity.Cross(r1).Dot(h.Normalized()), eccentricity.Dot(r1));
	if (anomaly < 0.0)
		anomaly += 2.0 * M_PI;
	return anomaly + transferAngle >= 2.0 * M_PI;
}

} // namespace

bool TransferPlanner::SolveLambert(const vector3d &r1, const vector3d &r2, double tof, double mu, const vector3d &hRef,
	vector3d &v1, vector3d &v2, double &transferAngle)
{
	return SolveLambertImpl(r1, r2, tof, mu, hRef, v1, v2, transferAngle);
}

bool TransferPlanner::GetFinalOrbit(Orbit &orbit, vector3d &pos, vector3d &vel, double &segmentTime) const
{
	Frame *frame = PlannerFrame();
	if (!frame || !frame->GetSystemBody())
		return false;

	// chained as RecomputeChain chains it: the outgoing state of the last
	// burn still to fly, at its start time (never before now), or the live
	// state now; completed burns are already in the live state
	segmentTime = Pi::game->GetTime();
	const Burn *last = nullptr;
	for (const Burn &burn : m_burns) {
		if (burn.status == COMPLETED)
			continue;
		segmentTime = std::max(segmentTime, burn.startTime);
		last = &burn;
	}
	if (!last) {
		pos = Pi::player->GetPositionRelTo(frame->GetId());
		vel = Pi::player->GetVelocityRelTo(frame->GetId());
	} else {
		pos = last->pos;
		vel = last->vel + GetOffsetVel(*last);
	}

	orbit = Orbit::FromBodyState(pos, vel, frame->GetSystemBody()->GetMass());
	return !is_zero_general(orbit.GetSemiMajorAxis());
}

bool TransferPlanner::PlanApproach(const Body *target, int burns, double standOff, std::string &error)
{
	burns = std::min(std::max(burns, 1), 2);
	if (m_burns.size() + burns > MAX_BURNS) {
		error = "Not enough room: a plan holds at most " + std::to_string(MAX_BURNS) + " burns";
		return false;
	}

	// plan against the chain as it stands now
	RecomputeChain();

	Frame *frame = PlannerFrame();
	if (!frame || !frame->GetSystemBody()) {
		error = "No body to orbit here";
		return false;
	}
	TargetTrack track;
	if (!MakeTargetTrack(target, frame, track, error))
		return false;
	// a ship or station is met standOff behind it on its own orbit, where a
	// matched velocity keeps the gap; a planet or moon is passed at a
	// distance from its centre instead (kBodyAimFactor)
	if (track.radius <= 0.0 && standOff > 0.0) {
		const double speed = track.VelAt(Pi::game->GetTime()).Length();
		if (speed > 0.0)
			track.lag = standOff / speed;
	}

	Orbit departure;
	vector3d pos0, vel0;
	double segmentTime;
	if (!GetFinalOrbit(departure, pos0, vel0, segmentTime)) {
		error = "The ship is not in orbit";
		return false;
	}

	const double mu = G * track.mass;
	const double bodyRadius = frame->GetSystemBody()->GetRadius();
	const vector3d hRef = pos0.Cross(vel0);
	const double earliest = segmentTime + kDepartureLead;

	// the search box: wait up to a synodic period (so every phasing comes
	// round once, but at most kMaxWaitRevolutions of the slower orbit, and at
	// least one revolution of the ship), and fly for a share of the periods
	const double shipPeriod = PeriodOf(departure);
	double shortest = std::min(shipPeriod > 0.0 ? shipPeriod : kInfinity, track.period > 0.0 ? track.period : kInfinity);
	double longest = std::max(shipPeriod, track.period);
	if (!std::isfinite(shortest)) {
		shortest = kDefaultSpan;
		longest = kDefaultSpan;
	}
	double window = shortest;
	if (shipPeriod > 0.0 && track.period > 0.0) {
		const double rate = std::fabs(1.0 / shipPeriod - 1.0 / track.period);
		const double synodic = rate > 0.0 ? 1.0 / rate : kInfinity;
		window = std::max(std::min(synodic, kMaxWaitRevolutions * longest), shipPeriod);
	}
	const double tofMin = kMinFlightFraction * shortest;
	const double tofMax = std::max(kMaxFlightFraction * longest, 2.0 * tofMin);

	// total delta-v of departing at depart and flying for tof; infinite for a
	// transfer that cannot be solved or would hit the central body
	const auto cost = [&](double depart, double tof, vector3d &dv1, vector3d &dv2) {
		if (depart < earliest || tof < 0.5 * tofMin)
			return kInfinity;
		const double tau = depart - segmentTime;
		const vector3d r1 = departure.OrbitalPosAtTime(tau);
		const vector3d shipVel = departure.OrbitalVelocityAtTime(track.mass, tau);
		const double arrive = depart + tof;
		vector3d r2 = track.PosAt(arrive);
		if (track.radius > 0.0)
			r2 += r2.Normalized() * (track.radius * kBodyAimFactor);

		vector3d v1, v2;
		double transferAngle;
		if (!SolveLambert(r1, r2, tof, mu, hRef, v1, v2, transferAngle))
			return kInfinity;
		if (DipsBelow(r1, v1, r2, transferAngle, mu, bodyRadius))
			return kInfinity;

		dv1 = v1 - shipVel;
		dv2 = (burns > 1) ? track.VelAt(arrive) - v2 : vector3d(0.0);
		return dv1.Length() + dv2.Length();
	};

	// coarse grid, then a pattern search around its best point
	double bestDepart = earliest, bestTof = tofMin, best = kInfinity;
	vector3d dv1, dv2;
	for (int i = 0; i < kDepartureSteps; ++i) {
		const double depart = earliest + window * double(i) / double(kDepartureSteps - 1);
		for (int j = 0; j < kFlightSteps; ++j) {
			const double tof = tofMin + (tofMax - tofMin) * double(j) / double(kFlightSteps - 1);
			const double c = cost(depart, tof, dv1, dv2);
			if (c < best) {
				best = c;
				bestDepart = depart;
				bestTof = tof;
			}
		}
	}
	if (!std::isfinite(best)) {
		error = "No transfer to the target was found that clears " + frame->GetSystemBody()->GetName();
		return false;
	}

	double stepDepart = window / double(kDepartureSteps - 1);
	double stepTof = (tofMax - tofMin) / double(kFlightSteps - 1);
	for (int i = 0; i < 400 && (stepDepart > 0.01 || stepTof > 0.01); ++i) {
		bool improved = false;
		const double moves[4][2] = { { stepDepart, 0.0 }, { -stepDepart, 0.0 }, { 0.0, stepTof }, { 0.0, -stepTof } };
		for (const auto &move : moves) {
			const double c = cost(bestDepart + move[0], bestTof + move[1], dv1, dv2);
			if (c < best) {
				best = c;
				bestDepart += move[0];
				bestTof += move[1];
				improved = true;
			}
		}
		if (!improved) {
			stepDepart *= 0.5;
			stepTof *= 0.5;
		}
	}
	cost(bestDepart, bestTof, dv1, dv2);

	// the departure burn, split onto the planner's axes at the state the
	// chain will compute for it
	const double tau = bestDepart - segmentTime;
	const vector3d r1 = departure.OrbitalPosAtTime(tau);
	const vector3d shipVel = departure.OrbitalVelocityAtTime(track.mass, tau);
	Burn departBurn;
	departBurn.startTime = bestDepart;
	if (!DecomposeDv(r1, shipVel, dv1, departBurn.dv)) {
		error = "The departure burn could not be set up";
		return false;
	}

	// the arrival burn matches the target's velocity, from the arrival state
	// as the chain will propagate the transfer
	Burn arriveBurn;
	if (burns > 1) {
		const Orbit transfer = Orbit::FromBodyState(r1, shipVel + dv1, track.mass);
		const vector3d r2 = transfer.OrbitalPosAtTime(bestTof);
		const vector3d v2 = transfer.OrbitalVelocityAtTime(track.mass, bestTof);
		arriveBurn.startTime = bestDepart + bestTof;
		if (!DecomposeDv(r2, v2, track.VelAt(arriveBurn.startTime) - v2, arriveBurn.dv)) {
			error = "The arrival burn could not be set up";
			return false;
		}
	}

	m_burns.push_back(departBurn);
	if (burns > 1)
		m_burns.push_back(arriveBurn);

	EnforceOrder();
	RecomputeChain();
	// new burns now occupy indices
	AdvanceDirectionLockSerial();
	return true;
}

bool TransferPlanner::GetClosestApproach(const Body *target, ClosestApproach &result, std::string &error) const
{
	Frame *frame = PlannerFrame();
	if (!frame || !frame->GetSystemBody()) {
		error = "No body to orbit here";
		return false;
	}
	TargetTrack track;
	if (!MakeTargetTrack(target, frame, track, error))
		return false;
	const double mass = track.mass;

	// the planned trajectory as consecutive conics: the live orbit from now,
	// then each burn's from its start time, chained as RecomputeChain does
	struct Leg {
		double start;
		Orbit orbit;
	};
	std::vector<Leg> legs;
	const double now = Pi::game->GetTime();
	legs.push_back({ now, Orbit::FromBodyState(Pi::player->GetPositionRelTo(frame->GetId()),
							  Pi::player->GetVelocityRelTo(frame->GetId()), mass) });
	double legStart = now;
	for (const Burn &burn : m_burns) {
		// completed burns are already in the live state
		if (burn.status == COMPLETED)
			continue;
		legStart = std::max(legStart, burn.startTime);
		legs.push_back({ legStart, Orbit::FromBodyState(burn.pos, burn.vel + GetOffsetVel(burn), mass) });
	}

	const auto shipState = [&](double t, vector3d &pos, vector3d &vel) {
		size_t i = legs.size() - 1;
		while (i > 0 && legs[i].start > t)
			--i;
		const double tau = t - legs[i].start;
		pos = legs[i].orbit.OrbitalPosAtTime(tau);
		vel = legs[i].orbit.OrbitalVelocityAtTime(mass, tau);
	};
	const auto distanceAt = [&](double t) {
		vector3d pos, vel;
		shipState(t, pos, vel);
		return (pos - track.PosAt(t)).Length();
	};

	// scan up to a revolution (of the slower orbit) past the last burn
	double span = std::max(PeriodOf(legs.back().orbit), track.period);
	if (span <= 0.0)
		span = kDefaultSpan;
	const double end = std::min(legs.back().start + span, now + kMaxApproachSpan);

	double bestTime = now, bestDistance = kInfinity;
	const double step = (end - now) / double(kApproachSamples);
	for (int i = 0; i <= kApproachSamples; ++i) {
		const double t = now + step * double(i);
		const double d = distanceAt(t);
		if (d < bestDistance) {
			bestDistance = d;
			bestTime = t;
		}
	}
	if (!std::isfinite(bestDistance)) {
		error = "No approach could be computed";
		return false;
	}

	// golden-section refinement in the samples around the best one
	double lo = std::max(now, bestTime - step), hi = std::min(end, bestTime + step);
	const double ratio = 0.5 * (std::sqrt(5.0) - 1.0);
	double a = hi - ratio * (hi - lo), b = lo + ratio * (hi - lo);
	double da = distanceAt(a), db = distanceAt(b);
	for (int i = 0; i < 80 && hi - lo > 1e-3; ++i) {
		if (da < db) {
			hi = b;
			b = a;
			db = da;
			a = hi - ratio * (hi - lo);
			da = distanceAt(a);
		} else {
			lo = a;
			a = b;
			da = db;
			b = lo + ratio * (hi - lo);
			db = distanceAt(b);
		}
	}
	const double refined = 0.5 * (lo + hi);
	if (distanceAt(refined) < bestDistance)
		bestTime = refined;

	vector3d pos, vel;
	shipState(bestTime, pos, vel);
	result.time = bestTime;
	result.distance = (pos - track.PosAt(bestTime)).Length();
	result.relativeSpeed = (vel - track.VelAt(bestTime)).Length();
	result.targetRadius = track.radius;
	return true;
}
