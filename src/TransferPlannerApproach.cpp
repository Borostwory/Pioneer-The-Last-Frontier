// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Closest-approach planning for the transfer planner: the approach leg and
// the closest-approach readout.
//
// A target that orbits a frame's body is modelled as a track that answers its
// position and velocity at any game time. AppendApproachLeg searches
// departure time x flight time with a Lambert solver for the transfer that
// meets the target for the least delta-v from a given state, and appends it
// as one burn (intercept) or two (rendezvous; a capture for a planet or
// moon). Routes across frames are put together from legs in
// TransferPlannerRoute.cpp. GetClosestApproach follows the planned
// trajectory through the frames it crosses and scans it against the target.

#include "TransferPlanner.h"

#include "Body.h"
#include "FloatComparison.h"
#include "Frame.h"
#include "Game.h"
#include "Orbit.h"
#include "Pi.h"
#include "Player.h"
#include "TransferPlannerConics.h"
#include "galaxy/SystemBody.h"
#include "gameconsts.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

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
// the coarse search aims this many radii from a planet's or moon's centre;
// the flyby search then sets the real aim from the flyby altitude
constexpr double kBodyAimFactor = 2.0;
// closest-approach scan: samples over the span, which is capped
constexpr int kApproachSamples = 1000;
constexpr double kMaxApproachSpan = 365.25 * 24 * 3600;
// the fallback span when no orbit involved is periodic
constexpr double kDefaultSpan = 24 * 3600;
// samples of a trajectory searched for its entry into a target's sphere
constexpr int kSphereSamples = 2000;
// flyby altitude search: bisection steps on the aim offset, which is at most
// this share of the target's sphere
constexpr int kFlybySteps = 40;
constexpr double kMaxFlybyAimShare = 0.9;

//
// Where a target is at a game time, relative to the planner frame. A system
// body (moon, orbital station) rides its orbit rails, which is exact; a ship
// is assumed to coast on the orbit of its current state.
//
struct TargetTrack {
	const SystemBody *rails = nullptr;
	PlannerConics::Conic conic; // a ship's coasting conic, from its state now
	double mass = 0.0;	 // of the central body
	double radius = 0.0; // a planet's or moon's, 0 for ships and stations
	double period = 0.0; // 0 when not periodic
	// a stand-off: the track runs this many seconds behind the target, so it
	// follows the target's own orbit at a distance behind it
	double lag = 0.0;
	// a planet or moon with a frame of its own: inside sphere (that frame's
	// radius) the game applies only the target's gravity, of bodyMass
	double sphere = 0.0;
	double bodyMass = 0.0;
	FrameId sphereFrame;

	vector3d PosAt(double t) const
	{
		// the rails are what Frame::UpdateOrbitRails places the body's frame by
		const double when = t - lag;
		return rails ? rails->GetOrbit().OrbitalPosAtTime(when) : conic.PosAt(when);
	}

	vector3d VelAt(double t) const
	{
		if (rails)
			return (PosAt(t + 1.0) - PosAt(t - 1.0)) * 0.5;
		vector3d p, v;
		conic.At(t - lag, p, v);
		return v;
	}
};

double PeriodOf(const Orbit &orbit)
{
	if (is_zero_general(orbit.GetSemiMajorAxis()) || orbit.GetEccentricity() >= 1.0)
		return 0.0;
	return orbit.Period();
}

// a conic's period, 0 on an escape trajectory
double ConicPeriod(const PlannerConics::Conic &conic)
{
	const double energy = 0.5 * conic.vel.LengthSqr() - conic.mu / conic.pos.Length();
	if (!(energy < 0.0) || !(conic.mu > 0.0))
		return 0.0;
	const double a = -conic.mu / (2.0 * energy);
	return 2.0 * M_PI * std::sqrt(a * a * a / conic.mu);
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
			superType == SystemBody::SUPERTYPE_STAR) {
			track.radius = sbody->GetRadius();
			// its non-rotating frame, where DynamicBody::CalcExternalForce takes
			// gravity from it alone (Space::MakeFramesFor sizes it to the Hill
			// radius)
			const Frame *bodyFrame = Frame::GetFrame(target->GetFrame());
			const Frame *ownFrame = bodyFrame ? Frame::GetFrame(bodyFrame->GetNonRotFrame()) : nullptr;
			if (ownFrame && ownFrame->GetBody() == target && ownFrame->GetRadius() > track.radius) {
				track.sphere = ownFrame->GetRadius();
				track.bodyMass = sbody->GetMass();
				track.sphereFrame = ownFrame->GetId();
			}
		}
		return true;
	}

	// ships flying in the frame (or in a station's zone in it) coast on their
	// current orbit
	const Frame *targetFrame = TransferPlanner::GravityFrame(target->GetFrame());
	if (!sbody && targetFrame && targetFrame->GetId() == frame->GetId()) {
		track.conic.pos = target->GetPositionRelTo(frame->GetId());
		track.conic.vel = target->GetVelocityRelTo(frame->GetId());
		track.conic.epoch = Pi::game->GetTime();
		track.conic.mu = G * track.mass;
		if (is_zero_general(track.conic.pos.Length())) {
			error = "The target is not in orbit";
			return false;
		}
		track.period = ConicPeriod(track.conic);
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

//
// The first time a ship flying shipState(t, pos, vel) - a state relative to
// the planner frame - is inside the target's sphere, between from and to, and
// its target-relative state then. Sampled, then bisected to the boundary.
// False when it never gets inside.
//
template <class ShipState>
bool FindSphereEntry(const ShipState &shipState, const TargetTrack &track, double from, double to,
	double &entry, vector3d &relPos, vector3d &relVel)
{
	const auto inside = [&](double t) {
		vector3d pos, vel;
		shipState(t, pos, vel);
		return (pos - track.PosAt(t)).Length() < track.sphere;
	};

	double before = from, after = -1.0;
	if (inside(from)) {
		after = from;
	} else {
		for (int i = 1; i <= kSphereSamples; ++i) {
			const double t = from + (to - from) * double(i) / double(kSphereSamples);
			if (inside(t)) {
				after = t;
				break;
			}
			before = t;
		}
		if (after < 0.0)
			return false;
		for (int i = 0; i < 60 && after - before > 1e-3; ++i) {
			const double mid = 0.5 * (before + after);
			if (inside(mid))
				after = mid;
			else
				before = mid;
		}
	}

	vector3d pos, vel;
	shipState(after, pos, vel);
	entry = after;
	relPos = pos - track.PosAt(after);
	relVel = vel - track.VelAt(after);
	return true;
}

// The time from the pass's epoch (its sphere entry) on when the
// target-relative pass is closest to the target: its next periapsis while that
// is still ahead - within four sphere-crossings at the entry speed - else the
// entry itself.
double ClosestOnPass(const PlannerConics::Conic &pass, double sphere)
{
	const double horizon = 4.0 * sphere / std::max(pass.vel.Length(), 1.0);
	double t;
	if (pass.pos.Dot(pass.vel) < 0.0 && PlannerConics::NextApsis(pass, true, pass.epoch, horizon, t))
		return t;
	return pass.epoch;
}

// the closest a target-relative conic comes to the target from relPos on:
// its periapsis while that is still ahead, else where it is now
double PeriapsisAhead(const vector3d &relPos, const vector3d &relVel, double bodyMass)
{
	const double mu = G * bodyMass;
	const double r = relPos.Length();
	if (relPos.Dot(relVel) >= 0.0)
		return r;
	const double hSqr = relPos.Cross(relVel).LengthSqr();
	const double energy = 0.5 * relVel.LengthSqr() - mu / r;
	const double e = std::sqrt(std::max(0.0, 1.0 + 2.0 * energy * hSqr / (mu * mu)));
	return hSqr / mu / (1.0 + e);
}

// The orbit normal a capture around sbody should share: the way its moons go
// round - the route on from it - or, with none, the way it goes round itself.
// A parking orbit the other way would meet its moons head-on.
vector3d CaptureNormal(const SystemBody *sbody, double t)
{
	const SystemBody *ref = sbody;
	for (const SystemBody *kid : sbody->GetChildren()) {
		const SystemBody::BodySuperType superType = kid->GetSuperType();
		if (superType == SystemBody::SUPERTYPE_ROCKY_PLANET || superType == SystemBody::SUPERTYPE_GAS_GIANT) {
			ref = kid;
			break;
		}
	}
	const Orbit &orbit = ref->GetOrbit();
	const vector3d p = orbit.OrbitalPosAtTime(t);
	const vector3d v = (orbit.OrbitalPosAtTime(t + 1.0) - orbit.OrbitalPosAtTime(t - 1.0)) * 0.5;
	return p.Cross(v).NormalizedSafe();
}

// the frame a planet's or moon's own sphere is (its non-rotating frame), or
// nothing for a ship, a station or a body without one
const Frame *OwnFrameOf(const Body *body)
{
	if (!body)
		return nullptr;
	const Frame *bodyFrame = Frame::GetFrame(body->GetFrame());
	const Frame *ownFrame = bodyFrame ? Frame::GetFrame(bodyFrame->GetNonRotFrame()) : nullptr;
	return (ownFrame && ownFrame->GetBody() == body) ? ownFrame : nullptr;
}

} // namespace

bool TransferPlanner::SolveLambert(const vector3d &r1, const vector3d &r2, double tof, double mu, const vector3d &hRef,
	vector3d &v1, vector3d &v2, double &transferAngle)
{
	return SolveLambertImpl(r1, r2, tof, mu, hRef, v1, v2, transferAngle);
}

bool TransferPlanner::PlanApproach(const Body *target, int burns, double standOff, std::string &error)
{
	// The approach is a fresh plan, from the ship's orbit now: whatever was
	// planned before goes, but only once a route is found - a failed attempt
	// leaves it as it was. The legs append their burns; the chain is then
	// recomputed through every frame they cross.
	std::vector<Burn> previous;
	previous.swap(m_burns);
	if (PlanRoute(target, burns, standOff, error)) {
		if (m_burns.size() <= MAX_BURNS) {
			EnforceOrder();
			RecomputeChain();
			// new burns now occupy indices
			AdvanceDirectionLockSerial();
			return true;
		}
		error = "The trip needs " + std::to_string(m_burns.size()) + " burns, more than the " +
			std::to_string(MAX_BURNS) + " a plan holds";
	}
	m_burns.swap(previous);
	return false;
}

bool TransferPlanner::AppendApproachLeg(const LegState &start, const LegOptions &options, const Body *target,
	int burns, double standOff, LegState *end, std::string &error)
{
	using namespace PlannerConics;
	burns = std::min(std::max(burns, 1), 2);

	const Frame *frame = start.frame;
	if (!frame || !frame->GetSystemBody()) {
		error = "No body to orbit here";
		return false;
	}
	TargetTrack track;
	if (!MakeTargetTrack(target, frame, track, error))
		return false;
	// a ship or station is met standOff behind it on its own orbit, where a
	// matched velocity keeps the gap; a planet or moon is flown past with its
	// periapsis standOff above the surface (see the flyby search below), and
	// a rendezvous with it is a capture into a circular orbit there
	if (track.radius <= 0.0 && standOff > 0.0) {
		const double speed = track.VelAt(start.time).Length();
		if (speed > 0.0)
			track.lag = standOff / speed;
	}

	Conic departure;
	departure.pos = start.pos;
	departure.vel = start.vel;
	departure.epoch = start.time;
	departure.mu = FrameMu(frame);
	const double mu = departure.mu;
	const double bodyRadius = FrameBodyRadius(frame);
	const vector3d hRef = start.pos.Cross(start.vel);
	const double earliest = std::max(options.earliest, start.time);

	// the search box: wait up to a synodic period (so every phasing comes
	// round once, but at most kMaxWaitRevolutions of the slower orbit, and at
	// least one revolution of the ship), and fly for a share of the periods -
	// unless the leg's options set them
	const double shipPeriod = ConicPeriod(departure);
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
	if (options.window > 0.0)
		window = options.window;
	double tofMin = kMinFlightFraction * shortest;
	double tofMax = std::max(kMaxFlightFraction * longest, 2.0 * tofMin);
	if (options.tofMax > options.tofMin && options.tofMin > 0.0) {
		tofMin = options.tofMin;
		tofMax = options.tofMax;
	}

	// total delta-v of departing at depart and flying for tof; infinite for a
	// transfer that cannot be solved or would hit the central body
	const auto cost = [&](double depart, double tof, vector3d &dv1, vector3d &dv2) {
		if (depart < earliest || tof < 0.5 * tofMin)
			return kInfinity;
		vector3d r1, shipVel;
		if (!departure.At(depart, r1, shipVel))
			return kInfinity;
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
		error = "No transfer to " + target->GetLabel() + " was found that clears " + frame->GetSystemBody()->GetName();
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

	vector3d r1, shipVel;
	departure.At(bestDepart, r1, shipVel);
	const double arrive = bestDepart + bestTof;
	LegState legEnd;

	// the capture burn of a rendezvous with a planet or moon (see below)
	Burn captureBurn;
	bool hasCapture = false;

	// A planet or moon with a sphere of its own: inside it the game flies the
	// ship around the target alone, which bends the pass inwards - aimed at a
	// fixed distance in this plan's two-body frame, the ship would hit it.
	// So the aim is moved sideways, square to the arrival direction, until the
	// target-relative pass flown from the sphere's edge - exactly as the game
	// flies it - has its periapsis standOff above the surface. The pass rises
	// with the offset, so the offset is bisected. A flyby passes on the side
	// away from the central body; a capture on the side that makes its orbit
	// go round the way the target's moons do.
	if (track.sphere > 0.0) {
		const vector3d centre = track.PosAt(arrive);
		const double wanted = track.radius + std::max(standOff, 0.0);
		const double bodyMu = G * track.bodyMass;

		vector3d v1, v2;
		double transferAngle;
		if (!SolveLambert(r1, centre, bestTof, mu, hRef, v1, v2, transferAngle) &&
			!SolveLambert(r1, centre + centre.Normalized() * (track.radius * kBodyAimFactor), bestTof, mu, hRef, v1, v2, transferAngle)) {
			error = "No transfer to " + target->GetLabel() + " was found";
			return false;
		}
		const vector3d arrival = (v2 - track.VelAt(arrive)).NormalizedSafe();
		vector3d side;
		if (burns > 1)
			side = arrival.Cross(CaptureNormal(track.rails, arrive));
		if (side.Length() < 1e-6)
			side = centre.Normalized() - arrival * arrival.Dot(centre.Normalized());
		if (side.Length() < 1e-6)
			side = arrival.Cross(hRef);
		side = side.NormalizedSafe();

		// the periapsis of the pass for an aim offset, and the departure
		// delta-v it takes; a pass that misses the sphere counts as high
		const auto periapsisFor = [&](double offset, vector3d &departDv) {
			vector3d w1, w2;
			double angle;
			if (!SolveLambert(r1, centre + side * offset, bestTof, mu, hRef, w1, w2, angle))
				return -1.0;
			departDv = w1 - shipVel;
			Conic transfer;
			transfer.pos = r1;
			transfer.vel = w1;
			transfer.epoch = bestDepart;
			transfer.mu = mu;
			const auto legState = [&](double t, vector3d &pos, vector3d &vel) { transfer.At(t, pos, vel); };
			double entry;
			vector3d relPos, relVel;
			if (!FindSphereEntry(legState, track, bestDepart, arrive + bestTof, entry, relPos, relVel))
				return kInfinity;
			return PeriapsisAhead(relPos, relVel, track.bodyMass);
		};

		double lo = 0.0, hi = kMaxFlybyAimShare * track.sphere;
		vector3d flybyDv;
		if (periapsisFor(hi, flybyDv) < wanted) {
			error = "No pass of " + target->GetLabel() + " that high was found: lower the altitude";
			return false;
		}
		for (int i = 0; i < kFlybySteps; ++i) {
			const double mid = 0.5 * (lo + hi);
			vector3d dv;
			const double rp = periapsisFor(mid, dv);
			if (rp >= 0.0 && rp < wanted)
				lo = mid;
			else
				hi = mid;
		}
		if (periapsisFor(hi, flybyDv) < 0.0) {
			error = "No transfer to " + target->GetLabel() + " was found";
			return false;
		}
		dv1 = flybyDv;

		// the pass inside the sphere, as the chain will fly it
		Conic transfer;
		transfer.pos = r1;
		transfer.vel = shipVel + dv1;
		transfer.epoch = bestDepart;
		transfer.mu = mu;
		const auto legState = [&](double t, vector3d &pos, vector3d &vel) { transfer.At(t, pos, vel); };
		double entry;
		vector3d relPos, relVel;
		if (!FindSphereEntry(legState, track, bestDepart, arrive + bestTof, entry, relPos, relVel)) {
			error = "The transfer does not reach " + target->GetLabel() + "'s sphere of influence";
			return false;
		}
		Conic pass;
		pass.pos = relPos;
		pass.vel = relVel;
		pass.epoch = entry;
		pass.mu = bodyMu;
		const double tp = ClosestOnPass(pass, track.sphere);
		vector3d p, v;
		pass.At(tp, p, v);
		legEnd.frame = Frame::GetFrame(track.sphereFrame);
		legEnd.time = tp;
		legEnd.pos = p;
		legEnd.vel = v;

		// A rendezvous with a planet or moon is a capture: a retro burn at the
		// pass's periapsis, in the target's own frame, that leaves the ship on
		// a circular orbit at the pass altitude. RecomputeChain carries the
		// chain into that frame at the sphere's edge.
		if (burns > 1) {
			const vector3d capture = CircularizeDv(p, v, bodyMu);
			captureBurn.startTime = tp;
			captureBurn.frame = track.sphereFrame;
			if (!DecomposeDv(p, v, capture, captureBurn.dv)) {
				error = "The capture burn could not be set up";
				return false;
			}
			hasCapture = true;
			legEnd.vel = v + capture;
		}
	}

	// the departure burn, split onto the planner's axes at the state the
	// chain will compute for it
	Burn departBurn;
	departBurn.startTime = bestDepart;
	departBurn.frame = frame->GetId();
	if (!DecomposeDv(r1, shipVel, dv1, departBurn.dv)) {
		error = "The departure burn could not be set up";
		return false;
	}

	// a ship or station: the arrival, as the chain will propagate the
	// transfer, and for a rendezvous the burn that matches its velocity there
	Burn arriveBurn;
	if (track.sphere <= 0.0) {
		Conic transfer;
		transfer.pos = r1;
		transfer.vel = shipVel + dv1;
		transfer.epoch = bestDepart;
		transfer.mu = mu;
		vector3d r2, v2;
		transfer.At(arrive, r2, v2);
		legEnd.frame = frame;
		legEnd.time = arrive;
		legEnd.pos = r2;
		legEnd.vel = v2;
		if (burns > 1) {
			arriveBurn.startTime = arrive;
			arriveBurn.frame = frame->GetId();
			if (!DecomposeDv(r2, v2, track.VelAt(arrive) - v2, arriveBurn.dv)) {
				error = "The arrival burn could not be set up";
				return false;
			}
			legEnd.vel = track.VelAt(arrive);
		}
	}

	m_burns.push_back(departBurn);
	if (hasCapture)
		m_burns.push_back(captureBurn);
	else if (burns > 1 && track.sphere <= 0.0)
		m_burns.push_back(arriveBurn);
	if (end)
		*end = legEnd;
	return true;
}

bool TransferPlanner::GetClosestApproach(const Body *target, ClosestApproach &result, std::string &error) const
{
	using namespace PlannerConics;
	Frame *frame = PlannerFrame();
	if (!frame || !frame->GetSystemBody()) {
		error = "No body to orbit here";
		return false;
	}
	if (!target) {
		error = "No target: select one on the map or set a navigation target";
		return false;
	}
	if (target == Pi::player || target == frame->GetBody()) {
		error = "Choose a target other than your ship and the body you orbit";
		return false;
	}

	// where the target is: a planet's or moon's own frame - which the plan may
	// get into, its centre at the origin - and the frame it moves in
	const Frame *ownFrame = OwnFrameOf(target);
	const Frame *homeFrame = ownFrame ? Frame::GetFrame(ownFrame->GetParent()) : GravityFrame(target->GetFrame());
	if (!homeFrame && !ownFrame) {
		error = "The target's whereabouts are unknown";
		return false;
	}

	// The planned trajectory, frame by frame, as the chain flies it: the live
	// orbit, every burn still to fly, and the spheres crossed on the way;
	// after the last burn it carries on into the target's frame where it can
	struct Leg {
		const Frame *frame;
		Conic conic;
		double end;
	};
	std::vector<Leg> legs;
	const double now = Pi::game->GetTime();
	const Frame *segFrame = frame;
	Conic segment;
	segment.pos = Pi::player->GetPositionRelTo(frame->GetId());
	segment.vel = Pi::player->GetVelocityRelTo(frame->GetId());
	segment.epoch = now;
	segment.mu = FrameMu(frame);
	const auto close = [&](double t) {
		if (!legs.empty())
			legs.back().end = t;
	};
	const auto open = [&]() { legs.push_back({ segFrame, segment, kInfinity }); };
	open();
	std::vector<std::pair<const Frame *, Conic>> trace;
	for (const Burn &burn : m_burns) {
		if (burn.status == COMPLETED)
			continue;
		const double at = std::max(burn.startTime, segment.epoch);
		if (burn.frame.valid() && burn.frame != segFrame->GetId()) {
			trace.clear();
			if (!TransitionTo(segment, segFrame, burn.frame, at, true, &trace))
				break;
			for (const auto &step : trace) {
				close(step.second.epoch);
				legs.push_back({ step.first, step.second, kInfinity });
			}
		}
		vector3d p, v;
		if (!segment.At(at, p, v))
			break;
		close(at);
		segment.pos = p;
		segment.vel = v + GetOffsetVel(burn);
		segment.epoch = at;
		open();
	}

	// how far past the last burn to look: a revolution of the slower of the
	// last orbit and the target's, capped
	double targetPeriod = 0.0;
	if (const SystemBody *sbody = target->GetSystemBody())
		targetPeriod = PeriodOf(sbody->GetOrbit());
	double span = std::max(ConicPeriod(segment), targetPeriod);
	if (span <= 0.0)
		span = kDefaultSpan;
	const double end = std::min(segment.epoch + span, now + kMaxApproachSpan);

	// on into the target's own frame, or else the frame it moves in
	trace.clear();
	const Frame *lastFrame = segFrame;
	Conic last = segment;
	bool reached = false;
	if (ownFrame && ownFrame != segFrame)
		reached = TransitionTo(last, lastFrame, ownFrame->GetId(), end, false, &trace);
	if (!reached && homeFrame && homeFrame != segFrame) {
		trace.clear();
		lastFrame = segFrame;
		last = segment;
		TransitionTo(last, lastFrame, homeFrame->GetId(), end, false, &trace);
	}
	for (const auto &step : trace) {
		close(step.second.epoch);
		legs.push_back({ step.first, step.second, kInfinity });
	}
	close(end);
	for (Leg &leg : legs)
		leg.end = std::min(leg.end, end);

	// Inside the target's own frame the closest approach is the lowest point
	// of the legs flown there - the target is at the origin. Distances within
	// a metre tie, and the earliest wins: on a circular orbit every point is
	// as close, and the first is the one that matters.
	const auto closer = [](double d, double than) { return d < than - 1.0; };
	double bestTime = 0.0, bestDistance = kInfinity;
	vector3d bestRelVel;
	const auto scan = [&](const Leg &leg, const std::function<double(double, vector3d &)> &distanceAt) {
		if (leg.end <= leg.conic.epoch)
			return;
		const double step = (leg.end - leg.conic.epoch) / double(kApproachSamples);
		double localBest = kInfinity, localTime = leg.conic.epoch;
		vector3d relVel;
		for (int i = 0; i <= kApproachSamples; ++i) {
			const double t = leg.conic.epoch + step * double(i);
			const double d = distanceAt(t, relVel);
			if (closer(d, localBest)) {
				localBest = d;
				localTime = t;
			}
		}
		// golden-section refinement around the best sample
		double lo = std::max(leg.conic.epoch, localTime - step), hi = std::min(leg.end, localTime + step);
		const double ratio = 0.5 * (std::sqrt(5.0) - 1.0);
		for (int i = 0; i < 80 && hi - lo > 1e-3; ++i) {
			const double a = hi - ratio * (hi - lo), b = lo + ratio * (hi - lo);
			vector3d dummy;
			if (distanceAt(a, dummy) < distanceAt(b, dummy))
				hi = b;
			else
				lo = a;
		}
		const double refined = 0.5 * (lo + hi);
		if (closer(distanceAt(refined, relVel), localBest))
			localTime = refined;
		const double d = distanceAt(localTime, relVel);
		if (closer(d, bestDistance)) {
			bestDistance = d;
			bestTime = localTime;
			bestRelVel = relVel;
		}
	};

	if (ownFrame) {
		for (const Leg &leg : legs) {
			if (leg.frame != ownFrame)
				continue;
			scan(leg, [&](double t, vector3d &relVel) {
				vector3d p;
				if (!leg.conic.At(t, p, relVel))
					return kInfinity;
				return p.Length();
			});
		}
		if (std::isfinite(bestDistance)) {
			result.time = bestTime;
			result.distance = bestDistance;
			result.relativeSpeed = bestRelVel.Length();
			result.targetRadius = FrameBodyRadius(ownFrame);
			return true;
		}
	}

	if (!homeFrame) {
		error = "The plan does not reach " + target->GetLabel() + "'s neighbourhood";
		return false;
	}

	// Otherwise in the frame the target moves in, against its track; a planet
	// or moon whose sphere the plan enters is passed on the target-relative
	// conic from the sphere's edge, which its gravity bends inwards - often
	// into the surface, where a two-body scan would show a clean pass.
	TargetTrack track;
	if (!MakeTargetTrack(target, homeFrame, track, error))
		return false;
	for (const Leg &leg : legs) {
		if (leg.frame != homeFrame)
			continue;
		if (track.sphere > 0.0) {
			const auto legState = [&](double t, vector3d &pos, vector3d &vel) { leg.conic.At(t, pos, vel); };
			double entry;
			vector3d relPos, relVel;
			if (FindSphereEntry(legState, track, leg.conic.epoch, leg.end, entry, relPos, relVel)) {
				Conic pass;
				pass.pos = relPos;
				pass.vel = relVel;
				pass.epoch = entry;
				pass.mu = G * track.bodyMass;
				const double tp = ClosestOnPass(pass, track.sphere);
				vector3d p, v;
				pass.At(tp, p, v);
				result.time = tp;
				result.distance = p.Length();
				result.relativeSpeed = v.Length();
				result.targetRadius = track.radius;
				return true;
			}
		}
		scan(leg, [&](double t, vector3d &relVel) {
			vector3d p, v;
			if (!leg.conic.At(t, p, v))
				return kInfinity;
			relVel = v - track.VelAt(t);
			return (p - track.PosAt(t)).Length();
		});
	}
	if (!std::isfinite(bestDistance)) {
		error = "The plan does not reach " + target->GetLabel() + "'s neighbourhood";
		return false;
	}
	result.time = bestTime;
	result.distance = bestDistance;
	result.relativeSpeed = bestRelVel.Length();
	result.targetRadius = track.radius;
	return true;
}
