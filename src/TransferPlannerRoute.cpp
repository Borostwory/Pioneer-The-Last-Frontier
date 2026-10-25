// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Route planning for the transfer planner: a trip from the ship's orbit to a
// target anywhere in the system, staged through parking orbits.
//
// The game's frames nest - moons inside planets inside the star - and inside
// each the body's gravity alone acts. A trip is put together from legs:
//
//   up      out of a body's sphere onto an orbit around the body above, with
//           an apsis at a chosen radius, circularized there
//   across  out of a body's sphere and over to another body circling the
//           same body above: a Lambert transfer there sets the departure, the
//           escape burn is solved to leave the sphere at its velocity, and the
//           approach leg - from the state after the sphere's edge, its first
//           burn the course correction - flies by or captures at the far body
//   down    the approach leg from a parking orbit to a body or craft below
//
// Between levels the ship parks in a circular orbit at half the distance of
// the next (or last) moon on the way: high, so the stages stay cheap, and
// clear of that moon's sphere.

#include "TransferPlanner.h"

#include "Body.h"
#include "Frame.h"
#include "Game.h"
#include "Pi.h"
#include "Player.h"
#include "TransferPlannerConics.h"
#include "galaxy/SystemBody.h"
#include "gameconsts.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {
using namespace PlannerConics;

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kDay = 86400.0;

// the first burn's earliest time after now, so there is time to turn the ship
constexpr double kLegLead = 120.0;
// a leg starts at least this long after the last burn of the one before, and
// at least a quarter of the orbit it leaves the ship on: a finite burn takes
// time
constexpr double kLegGap = 600.0;
// parking orbits on the way: at this share of the distance of the next (or
// last) moon on the way, and at least this many radii of the body
constexpr double kParkingShare = 0.5;
constexpr double kMinParkingRadii = 2.0;
// the furthest a climb out of a sphere is followed
constexpr double kExitHorizon = 2.0 * 365.25 * kDay;

// up leg: burn points over a parking orbit, and escape delta-v scanned in
// steps above the bare escape before a bisection
constexpr int kUpBurnPoints = 72;
constexpr int kUpDvSteps = 60;
constexpr double kUpDvStep = 50.0; // m/s

// across leg: the Lambert grid in the frame above, the escape burn points
// and Newton iterations, and the course correction after the sphere's edge
constexpr int kAcrossDepartSteps = 60;
constexpr int kAcrossFlightSteps = 40;
constexpr double kMaxWaitRevolutions = 10.0;
constexpr double kMinFlightFraction = 0.05;
constexpr double kMaxFlightFraction = 0.6;
constexpr int kEscapePoints = 36;
constexpr int kEscapeIterations = 12;
constexpr double kEscapeStep = 0.1;		  // m/s, the Newton difference step
constexpr double kEscapeTolerance = 1e-3; // m/s
constexpr double kEscapeMaxCorrection = 2000.0; // m/s per Newton step
// the slowest climb out of a sphere assumed when picking the departure date
constexpr double kClimbSpeed = 2000.0; // m/s
constexpr double kCorrectionDelay = kDay;
constexpr double kCorrectionWindow = 10.0 * kDay;
constexpr double kFlightSlack = 0.3;

double PeriodOf(const Conic &conic)
{
	const double energy = 0.5 * conic.vel.LengthSqr() - conic.mu / conic.pos.Length();
	if (!(energy < 0.0) || !(conic.mu > 0.0))
		return 0.0;
	const double a = -conic.mu / (2.0 * energy);
	return 2.0 * M_PI * std::sqrt(a * a * a / conic.mu);
}

// the period of a frame's body around the one above
double RailsPeriod(const Frame *frame)
{
	const SystemBody *sbody = frame ? frame->GetSystemBody() : nullptr;
	if (!sbody || !sbody->GetParent())
		return 0.0;
	const Orbit &orbit = sbody->GetOrbit();
	if (orbit.GetEccentricity() >= 1.0 || !(orbit.GetSemiMajorAxis() > 0.0))
		return 0.0;
	return orbit.Period();
}

const Frame *ParentOf(const Frame *frame)
{
	return frame ? Frame::GetFrame(frame->GetParent()) : nullptr;
}

// the frame a planet's or moon's own sphere is, or nothing
const Frame *OwnFrame(const Body *body)
{
	if (!body)
		return nullptr;
	const Frame *bodyFrame = Frame::GetFrame(body->GetFrame());
	const Frame *ownFrame = bodyFrame ? Frame::GetFrame(bodyFrame->GetNonRotFrame()) : nullptr;
	return (ownFrame && ownFrame->GetBody() == body) ? ownFrame : nullptr;
}

std::string NameOf(const Frame *frame)
{
	return (frame && frame->GetSystemBody()) ? frame->GetSystemBody()->GetName() : std::string("it");
}

// solves the 3x3 system m x = b (m by columns); false when singular
bool Solve3(const vector3d m[3], const vector3d &b, vector3d &x)
{
	const auto det = [](const vector3d &c0, const vector3d &c1, const vector3d &c2) {
		return c0.Dot(c1.Cross(c2));
	};
	const double d = det(m[0], m[1], m[2]);
	if (std::fabs(d) < 1e-30)
		return false;
	x = vector3d(det(b, m[1], m[2]), det(m[0], b, m[2]), det(m[0], m[1], b)) / d;
	return std::isfinite(x.x) && std::isfinite(x.y) && std::isfinite(x.z);
}

//
// The escape burn: from the parking orbit (in frame), a burn whose climb
// leaves the sphere with the body-relative velocity vinf. Burn points are
// tried over a parking orbit around the time the climb has to start for the
// sphere's edge at depart; at each, Newton on the burn's velocity change,
// started from a prograde burn of the speed that energy takes. The cheapest
// burn that converges wins.
//
bool EscapeToVinf(const Conic &orbit, const Frame *frame, double earliest, double depart, const vector3d &vinf,
	double &burnTime, vector3d &burnPos, vector3d &burnVel, vector3d &burnDv)
{
	const double mu = orbit.mu;
	const double sphere = frame->GetRadius();
	const double period = PeriodOf(orbit);
	if (period <= 0.0)
		return false;

	// the body-relative velocity at the sphere's edge after a burn dv at t
	const auto exitVelocity = [&](double t, const vector3d &r, const vector3d &v, const vector3d &dv, vector3d &out) {
		Conic climb;
		climb.pos = r;
		climb.vel = v + dv;
		climb.epoch = t;
		climb.mu = mu;
		double exit;
		vector3d pos, vel;
		if (!FindExit(climb, frame, t, kExitHorizon, exit, pos, vel))
			return -1.0;
		out = vel - FrameVel(frame, exit);
		return exit;
	};
	const auto prograde = [&](const vector3d &r, const vector3d &v) {
		const double needed = std::sqrt(vinf.LengthSqr() + 2.0 * mu / r.Length() - 2.0 * mu / sphere);
		return v.NormalizedSafe() * (needed - v.Length());
	};

	// how long the climb takes, to centre the burn points on its start
	vector3d r, v, out;
	const double probeTime = std::max(earliest, depart);
	if (!orbit.At(probeTime, r, v))
		return false;
	const double probeExit = exitVelocity(probeTime, r, v, prograde(r, v), out);
	const double climb = probeExit > 0.0 ? probeExit - probeTime : 0.0;

	double best = kInfinity;
	for (int i = 0; i < kEscapePoints; ++i) {
		const double t = std::max(earliest, depart - climb + period * (double(i) / kEscapePoints - 0.5));
		if (!orbit.At(t, r, v))
			continue;
		vector3d dv = prograde(r, v);
		bool converged = false;
		for (int k = 0; k < kEscapeIterations; ++k) {
			vector3d got;
			if (exitVelocity(t, r, v, dv, got) < 0.0)
				break;
			const vector3d miss = got - vinf;
			if (miss.Length() < kEscapeTolerance) {
				converged = true;
				break;
			}
			vector3d columns[3];
			bool ok = true;
			for (int c = 0; c < 3 && ok; ++c) {
				vector3d probe = dv;
				(c == 0 ? probe.x : c == 1 ? probe.y : probe.z) += kEscapeStep;
				vector3d moved;
				ok = exitVelocity(t, r, v, probe, moved) >= 0.0;
				columns[c] = (moved - got) / kEscapeStep;
			}
			vector3d step;
			if (!ok || !Solve3(columns, -miss, step))
				break;
			if (step.Length() > kEscapeMaxCorrection)
				step = step.Normalized() * kEscapeMaxCorrection;
			dv += step;
		}
		if (converged && dv.Length() < best) {
			best = dv.Length();
			burnTime = t;
			burnPos = r;
			burnVel = v;
			burnDv = dv;
		}
	}
	return std::isfinite(best);
}

} // namespace

bool TransferPlanner::AppendUpLeg(const LegState &start, double earliest, double targetRadius, bool capture,
	LegState &end, std::string &error)
{
	const Frame *frame = start.frame;
	const Frame *parent = ParentOf(frame);
	if (!frame || !frame->GetSystemBody() || !parent || !parent->GetSystemBody()) {
		error = "There is nothing above " + NameOf(frame) + " to go to";
		return false;
	}
	const double mu = FrameMu(frame);
	const double parentMu = FrameMu(parent);

	Conic orbit;
	orbit.pos = start.pos;
	orbit.vel = start.vel;
	orbit.epoch = start.time;
	orbit.mu = mu;
	const double period = PeriodOf(orbit);
	if (period <= 0.0) {
		error = "The ship must be in orbit around " + NameOf(frame) + " to leave it";
		return false;
	}
	// going lower than the body's own orbit puts the target radius at the
	// periapsis of the orbit above, going higher at its apoapsis
	const double bodyDistance = FramePos(frame, start.time).Length();
	const bool lower = targetRadius < bodyDistance;

	// the apsis the orbit above gets from a prograde burn of m at r / v, at t
	const auto apsisFor = [&](double t, const vector3d &r, const vector3d &v, double m) {
		Conic climb;
		climb.pos = r;
		climb.vel = v + v.NormalizedSafe() * m;
		climb.epoch = t;
		climb.mu = mu;
		double exit;
		vector3d pos, vel;
		if (!FindExit(climb, frame, t, kExitHorizon, exit, pos, vel))
			return std::numeric_limits<double>::quiet_NaN();
		double periapsis, apoapsis;
		Apsides(pos, vel, parentMu, periapsis, apoapsis);
		return lower ? periapsis : apoapsis;
	};

	// burn points over a parking orbit; at each the burn sizes are scanned
	// above the bare escape for a pair that brackets the target radius, which
	// a bisection then meets. The smallest burn wins.
	double bestDv = kInfinity, bestTime = 0.0;
	vector3d bestPos, bestVel;
	for (int i = 0; i < kUpBurnPoints; ++i) {
		const double t = earliest + period * double(i) / double(kUpBurnPoints);
		vector3d r, v;
		if (!orbit.At(t, r, v))
			continue;
		const double escape = std::sqrt(2.0 * mu / r.Length()) - v.Length();
		double prevM = escape * 1.001 + 1e-3;
		double prev = apsisFor(t, r, v, prevM);
		for (int k = 1; k < kUpDvSteps; ++k) {
			const double m = escape * 1.001 + 1e-3 + kUpDvStep * double(k);
			if (m >= bestDv)
				break;
			const double cur = apsisFor(t, r, v, m);
			if (!std::isnan(prev) && !std::isnan(cur) && (prev - targetRadius) * (cur - targetRadius) <= 0.0) {
				double lo = prevM, hi = m, aLo = prev;
				for (int b = 0; b < 50; ++b) {
					const double mid = 0.5 * (lo + hi);
					const double a = apsisFor(t, r, v, mid);
					if (std::isnan(a))
						break;
					if ((aLo - targetRadius) * (a - targetRadius) <= 0.0) {
						hi = mid;
					} else {
						lo = mid;
						aLo = a;
					}
				}
				if (hi < bestDv) {
					bestDv = hi;
					bestTime = t;
					bestPos = r;
					bestVel = v;
				}
				break;
			}
			prevM = m;
			prev = cur;
		}
	}
	if (!std::isfinite(bestDv)) {
		error = "No way out of " + NameOf(frame) + "'s sphere of influence to that height around " + NameOf(parent) + " was found";
		return false;
	}

	// the escape burn, then the climb to the sphere's edge and on to the apsis
	const vector3d dv = bestVel.NormalizedSafe() * bestDv;
	Burn escapeBurn;
	escapeBurn.startTime = bestTime;
	escapeBurn.frame = frame->GetId();
	if (!DecomposeDv(bestPos, bestVel, dv, escapeBurn.dv)) {
		error = "The escape burn could not be set up";
		return false;
	}
	Conic climb;
	climb.pos = bestPos;
	climb.vel = bestVel + dv;
	climb.epoch = bestTime;
	climb.mu = mu;
	double exit;
	vector3d pos, vel;
	if (!FindExit(climb, frame, bestTime, kExitHorizon, exit, pos, vel)) {
		error = "The escape does not leave " + NameOf(frame) + "'s sphere of influence";
		return false;
	}
	Conic above;
	above.pos = pos;
	above.vel = vel;
	above.epoch = exit;
	above.mu = parentMu;
	const double abovePeriod = PeriodOf(above);
	double apsisTime;
	if (!NextApsis(above, lower, exit, abovePeriod > 0.0 ? abovePeriod : kExitHorizon, apsisTime)) {
		error = "The orbit around " + NameOf(parent) + " has no apsis to circularize at";
		return false;
	}
	vector3d apsisPos, apsisVel;
	above.At(apsisTime, apsisPos, apsisVel);

	m_burns.push_back(escapeBurn);
	end.frame = parent;
	end.time = apsisTime;
	end.pos = apsisPos;
	end.vel = apsisVel;
	if (capture) {
		const vector3d circularize = CircularizeDv(apsisPos, apsisVel, parentMu);
		Burn captureBurn;
		captureBurn.startTime = apsisTime;
		captureBurn.frame = parent->GetId();
		if (!DecomposeDv(apsisPos, apsisVel, circularize, captureBurn.dv)) {
			error = "The circularizing burn could not be set up";
			return false;
		}
		m_burns.push_back(captureBurn);
		end.vel = apsisVel + circularize;
	}
	return true;
}

bool TransferPlanner::AppendAcrossLeg(const LegState &start, double earliest, const Body *destination,
	double altitude, bool capture, LegState &end, std::string &error)
{
	const Frame *frame = start.frame;
	const Frame *parent = ParentOf(frame);
	const Frame *destFrame = OwnFrame(destination);
	if (!frame || !parent || !destFrame || ParentOf(destFrame) != parent) {
		error = "No route across to " + (destination ? destination->GetLabel() : std::string("it")) + " was found";
		return false;
	}
	const double mu = FrameMu(frame);
	const double parentMu = FrameMu(parent);
	const double destMu = FrameMu(destFrame);
	const double destRadius = FrameBodyRadius(destFrame);

	Conic orbit;
	orbit.pos = start.pos;
	orbit.vel = start.vel;
	orbit.epoch = start.time;
	orbit.mu = mu;
	const double parkPeriod = PeriodOf(orbit);
	if (parkPeriod <= 0.0) {
		error = "The ship must be in orbit around " + NameOf(frame) + " to leave it";
		return false;
	}
	const double parkRadius = start.pos.Length();
	const double parkSpeed = std::sqrt(mu / parkRadius);
	const vector3d parkNormal = start.pos.Cross(start.vel).NormalizedSafe();

	// The departure date and flight time, from a Lambert transfer between the
	// two bodies in the frame above. The cost estimates the escape from the
	// parking orbit - plus turning out of its plane, which at escape speed
	// costs a great deal - and the capture at the far end. No departure
	// before the climb out of the sphere and two parking orbits to pick the
	// burn point from.
	const double homePeriod = RailsPeriod(frame);
	const double destPeriod = RailsPeriod(destFrame);
	if (homePeriod <= 0.0 || destPeriod <= 0.0) {
		error = "No route across to " + destination->GetLabel() + " was found";
		return false;
	}
	const double rate = std::fabs(1.0 / homePeriod - 1.0 / destPeriod);
	const double longest = std::max(homePeriod, destPeriod);
	const double window = std::min(rate > 0.0 ? 1.0 / rate : kInfinity, kMaxWaitRevolutions * longest);
	const double tofMin = kMinFlightFraction * std::min(homePeriod, destPeriod);
	const double tofMax = kMaxFlightFraction * longest;
	const double departMin = std::max(earliest, start.time) + frame->GetRadius() / kClimbSpeed + 2.0 * parkPeriod;
	const double captureRadius = destRadius + std::max(altitude, 0.0);

	double bestCost = kInfinity, bestDepart = 0.0, bestTof = 0.0;
	vector3d bestVinf;
	for (int i = 0; i < kAcrossDepartSteps; ++i) {
		const double depart = departMin + window * double(i) / double(kAcrossDepartSteps - 1);
		const vector3d r1 = FramePos(frame, depart);
		const vector3d bodyVel = FrameVel(frame, depart);
		for (int j = 0; j < kAcrossFlightSteps; ++j) {
			const double tof = tofMin + (tofMax - tofMin) * double(j) / double(kAcrossFlightSteps - 1);
			const vector3d r2 = FramePos(destFrame, depart + tof);
			vector3d v1, v2;
			double angle;
			if (!SolveLambert(r1, r2, tof, parentMu, r1.Cross(bodyVel), v1, v2, angle))
				continue;
			const vector3d vinf = v1 - bodyVel;
			const double burnSpeed = std::sqrt(vinf.LengthSqr() + 2.0 * mu / parkRadius);
			const double declination = std::asin(std::min(1.0, std::fabs(vinf.NormalizedSafe().Dot(parkNormal))));
			double c = burnSpeed - parkSpeed + burnSpeed * declination;
			if (capture) {
				const double arrival = (v2 - FrameVel(destFrame, depart + tof)).Length();
				c += std::sqrt(arrival * arrival + 2.0 * destMu / captureRadius) - std::sqrt(destMu / captureRadius);
			}
			if (c < bestCost) {
				bestCost = c;
				bestDepart = depart;
				bestTof = tof;
				bestVinf = vinf;
			}
		}
	}
	if (!std::isfinite(bestCost)) {
		error = "No transfer to " + destination->GetLabel() + " was found";
		return false;
	}

	// the escape burn that leaves the sphere at that velocity
	double burnTime;
	vector3d burnPos, burnVel, burnDv;
	if (!EscapeToVinf(orbit, frame, std::max(earliest, start.time), bestDepart, bestVinf, burnTime, burnPos, burnVel, burnDv)) {
		error = "No escape burn from " + NameOf(frame) + " towards " + destination->GetLabel() + " was found";
		return false;
	}
	Burn escapeBurn;
	escapeBurn.startTime = burnTime;
	escapeBurn.frame = frame->GetId();
	if (!DecomposeDv(burnPos, burnVel, burnDv, escapeBurn.dv)) {
		error = "The escape burn could not be set up";
		return false;
	}
	Conic climb;
	climb.pos = burnPos;
	climb.vel = burnVel + burnDv;
	climb.epoch = burnTime;
	climb.mu = mu;
	LegState out;
	out.frame = parent;
	if (!FindExit(climb, frame, burnTime, kExitHorizon, out.time, out.pos, out.vel)) {
		error = "The escape does not leave " + NameOf(frame) + "'s sphere of influence";
		return false;
	}
	m_burns.push_back(escapeBurn);

	// the course correction and the arrival: the approach leg from the state
	// after the sphere's edge, flight times around the Lambert one
	const double remaining = bestDepart + bestTof - out.time;
	LegOptions options;
	options.earliest = out.time + kCorrectionDelay;
	options.window = kCorrectionWindow;
	options.tofMin = std::max(remaining * (1.0 - kFlightSlack), kDay);
	options.tofMax = std::max(remaining * (1.0 + kFlightSlack), 2.0 * kDay);
	return AppendApproachLeg(out, options, destination, capture ? 2 : 1, altitude, &end, error);
}

bool TransferPlanner::PlanRoute(const Body *target, int burns, double standOff, std::string &error)
{
	const Frame *startFrame = PlannerFrame();
	if (!startFrame || !startFrame->GetSystemBody()) {
		error = "No body to orbit here";
		return false;
	}
	if (!target) {
		error = "No target: select one on the map or set a navigation target";
		return false;
	}
	if (target == Pi::player) {
		error = "Choose a target other than your ship";
		return false;
	}

	// where the target is: a planet's or moon's own frame, and the frame it
	// moves in (a ship's or station's gravity frame)
	const Frame *ownFrame = OwnFrame(target);
	const Frame *homeFrame = ownFrame ? ParentOf(ownFrame) : GravityFrame(target->GetFrame());
	if (ownFrame == startFrame) {
		error = "You are already orbiting " + target->GetLabel();
		return false;
	}
	// a star at the root of the system has no frame above it: only climbing
	// into it can reach it, which is decided below before the frame above is
	// needed
	if (!homeFrame && !(ownFrame && IsAncestorOrSelf(ownFrame->GetId(), startFrame->GetId()))) {
		error = "The target's whereabouts are unknown";
		return false;
	}

	LegState state;
	state.frame = startFrame;
	state.time = Pi::game->GetTime();
	state.pos = Pi::player->GetPositionRelTo(startFrame->GetId());
	state.vel = Pi::player->GetVelocityRelTo(startFrame->GetId());
	double earliest = state.time + kLegLead;
	const bool capture = burns > 1;

	// each leg starts where the one before ends, a gap after its last burn
	const auto advance = [&](const LegState &end) {
		state = end;
		Conic orbit;
		orbit.pos = end.pos;
		orbit.vel = end.vel;
		orbit.epoch = end.time;
		orbit.mu = FrameMu(end.frame);
		earliest = end.time + std::max(kLegGap, 0.25 * PeriodOf(orbit));
	};
	// a parking orbit around frame's body, on the way to (or from) something
	// at `distance` from it
	const auto parkingRadius = [&](const Frame *frame, double distance) {
		return std::max(kParkingShare * distance, kMinParkingRadii * FrameBodyRadius(frame));
	};
	const auto distanceIn = [&](const Frame *frame, const Frame *child) {
		return FramePos(child, state.time).Length();
	};

	LegState end;
	// the target is a body the ship is inside the sphere of: climb into it,
	// parking around each body on the way up
	if (ownFrame && IsAncestorOrSelf(ownFrame->GetId(), startFrame->GetId())) {
		const Frame *frame = startFrame;
		while (ParentOf(frame) != ownFrame) {
			const Frame *up = ParentOf(frame);
			if (!AppendUpLeg(state, earliest, parkingRadius(up, distanceIn(up, frame)), true, end, error))
				return false;
			advance(end);
			frame = up;
		}
		return AppendUpLeg(state, earliest, FrameBodyRadius(ownFrame) + std::max(standOff, 0.0), capture, end, error);
	}

	// the target circles the body the ship orbits: one approach leg
	if (homeFrame == startFrame) {
		LegOptions options;
		options.earliest = earliest;
		return AppendApproachLeg(state, options, target, burns, standOff, nullptr, error);
	}

	// the lowest frame holding both the ship and the target's frame
	const Frame *common = startFrame;
	while (common && !IsAncestorOrSelf(common->GetId(), homeFrame->GetId()))
		common = ParentOf(common);
	if (!common) {
		error = "No route to " + target->GetLabel() + " was found";
		return false;
	}

	// up to the child of the common frame that holds the ship, parking around
	// each body on the way
	const Frame *frame = startFrame;
	while (frame != common && ParentOf(frame) != common) {
		const Frame *up = ParentOf(frame);
		if (!AppendUpLeg(state, earliest, parkingRadius(up, distanceIn(up, frame)), true, end, error))
			return false;
		advance(end);
		frame = up;
	}

	// the frames from below the common frame down to the target's frame
	std::vector<const Frame *> path;
	for (const Frame *f = homeFrame; f && f != common; f = ParentOf(f))
		path.insert(path.begin(), f);

	// the parking radius around the body of path[k]: half way to the next
	// frame down, or to the target in the last one
	const auto parkingFor = [&](size_t k) {
		const Frame *around = path[k];
		const double distance = (k + 1 < path.size()) ? distanceIn(around, path[k + 1]) :
			(ownFrame ? distanceIn(around, ownFrame) : target->GetPositionRelTo(around->GetId()).Length());
		return parkingRadius(around, distance);
	};

	size_t k = 0;
	if (frame != common) {
		if (path.empty()) {
			// the target circles the common body, the ship one of its moons
			if (ownFrame)
				return AppendAcrossLeg(state, earliest, target, standOff, capture, end, error);
			// a ship or station there: park near its distance, then approach it
			const double distance = target->GetPositionRelTo(common->GetId()).Length();
			if (!AppendUpLeg(state, earliest, parkingRadius(common, 2.0 * distance * 0.9), true, end, error))
				return false;
			advance(end);
			LegOptions options;
			options.earliest = earliest;
			return AppendApproachLeg(state, options, target, burns, standOff, nullptr, error);
		}
		// across to the next body down, parking around it
		const Frame *next = path[0];
		if (!AppendAcrossLeg(state, earliest, next->GetBody(), parkingFor(0) - FrameBodyRadius(next), true, end, error))
			return false;
		advance(end);
		k = 1;
	}

	// down, parking around each body on the way
	for (; k < path.size(); ++k) {
		const Frame *next = path[k];
		LegOptions options;
		options.earliest = earliest;
		if (!AppendApproachLeg(state, options, next->GetBody(), 2, parkingFor(k) - FrameBodyRadius(next), &end, error))
			return false;
		advance(end);
	}

	// and the target itself, from the frame it moves in
	LegOptions options;
	options.earliest = earliest;
	return AppendApproachLeg(state, options, target, burns, standOff, nullptr, error);
}
