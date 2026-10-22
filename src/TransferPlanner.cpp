// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TransferPlanner.h"

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
#include <sstream>

namespace {
// a burn whose startTime is effectively unset; seeds at Pi::game->GetTime() on first touch
bool IsUnsetStartTime(double t) { return std::fabs(t) < 1.; }

// a burn within this many seconds of a snap event is "on" it, so snapping
// again moves it to the next orbit's occurrence
constexpr double kSnapTolerance = 1.0;
// the furthest an escape trajectory is searched for an event, in seconds
constexpr double kSnapHyperbolicHorizon = 1e10;

// the occurrence of an event (first at time, then every period; 0 = once)
// that a burn at current sits on, or -1 when it sits on none
double OccurrenceAt(double current, double time, double period)
{
	const double k = (period > 0.0) ? std::floor((current - time) / period + 0.5) : 0.0;
	if (k < 0.0 || std::fabs(current - (time + k * period)) >= kSnapTolerance)
		return -1.0;
	return k;
}

// angle swept from a to b around axis (the orbit's angular momentum), in [0, 2pi)
double SweptAngle(const vector3d &a, const vector3d &b, const vector3d &axis)
{
	double angle = std::atan2(a.Cross(b).Dot(axis), a.Dot(b));
	if (angle < 0.0)
		angle += 2.0 * M_PI;
	return angle;
}
} // namespace

Frame *TransferPlanner::PlannerFrame()
{
	if (!Pi::player)
		return nullptr;
	const Frame *playerFrame = Frame::GetFrame(Pi::player->GetFrame());
	if (!playerFrame)
		return nullptr;
	return Frame::GetFrame(playerFrame->GetNonRotFrame());
}

bool TransferPlanner::DecomposeDv(const vector3d &pos, const vector3d &vel, const vector3d &change, double dv[3])
{
	if (is_zero_general(pos.Length()) || is_zero_general(vel.Length()))
		return false;

	// prograde (along vel) and radial (along pos) both lie in the orbital
	// plane but are not orthogonal off a circle, so the in-plane part is split
	// by solving the 2x2 system; normal is orthogonal to both. At an apsis the
	// two in-plane axes are orthogonal and this is a plain projection.
	const vector3d progradeDir = vel.Normalized();
	const vector3d radialDir = pos.Normalized();
	const vector3d normal = pos.Cross(vel);
	if (is_zero_general(normal.Length()))
		return false;
	const double k = progradeDir.Dot(radialDir);
	const double det = 1.0 - k * k;
	if (det < 1e-9)
		return false;
	const double alongPrograde = change.Dot(progradeDir);
	const double alongRadial = change.Dot(radialDir);
	dv[PROGRADE] = (alongPrograde - k * alongRadial) / det;
	dv[RADIAL] = (alongRadial - k * alongPrograde) / det;
	dv[NORMAL] = change.Dot(normal.Normalized());
	return true;
}

TransferPlanner::TransferPlanner()
{
	m_factor = 1;
}

vector3d TransferPlanner::GetVel() const { return GetVel(0); }

vector3d TransferPlanner::GetVel(int idx) const
{
	const Burn &burn = EnsureBurn(idx);
	if (idx < 0)
		return vector3d(0., 0., 0.);

	return burn.vel + GetOffsetVel(burn);
}

vector3d TransferPlanner::GetOffsetVel() const
{
	if (!HasPlan())
		return vector3d(0., 0., 0.);

	return GetOffsetVel(EnsureBurn(0));
}

vector3d TransferPlanner::GetOffsetVel(int idx) const
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return vector3d(0., 0., 0.);

	return GetOffsetVel(m_burns[idx]);
}

vector3d TransferPlanner::GetOffsetVel(const Burn &burn) const
{
	// a plan with no position is inert
	if (burn.pos.ExactlyEqual(vector3d(0., 0., 0.)))
		return vector3d(0., 0., 0.);

	const vector3d pNormal = burn.pos.Cross(burn.vel);

	return burn.dv[PROGRADE] * burn.vel.Normalized() +
		burn.dv[NORMAL] * pNormal.Normalized() +
		burn.dv[RADIAL] * burn.pos.Normalized();
}

vector3d TransferPlanner::GetPosition() const { return GetPos(0); }

vector3d TransferPlanner::GetPos(int idx) const
{
	if (idx < 0)
		return vector3d(0., 0., 0.);
	return EnsureBurn(idx).pos;
}

double TransferPlanner::GetStartTime() const { return GetStartTime(0); }

double TransferPlanner::GetStartTime(int idx) const
{
	if (idx < 0)
		return 0.0;
	const double t = EnsureBurn(idx).startTime;
	return t < 0.0 ? 0.0 : t;
}

void TransferPlanner::AddStartTime(double timeStep)
{
	AddStartTime(0, timeStep);

	// legacy behaviour: a plan dragged back to (or before) the present is cleared.
	// Dropping the plan drops its completion state with it: m_burns.clear()
	// destroys every Burn struct, so no status or appliedDeltaV survives it.
	if (HasPlan() && GetStartTime(0) <= Pi::game->GetTime()) {
		m_burns.clear();
		AdvanceDirectionLockSerial();
	}
}

void TransferPlanner::AddStartTime(int idx, double timeStep)
{
	Burn &burn = EnsureBurn(idx);
	if (idx < 0)
		return;

	// first-touch seeding, preserved for burn 0 (and any burn not yet anchored)
	if (IsUnsetStartTime(burn.startTime))
		burn.startTime = Pi::game->GetTime();

	burn.startTime += m_factor * timeStep;

	EnforceOrder();
	RecomputeChain();
}

void TransferPlanner::ResetStartTime()
{
	const double gameTime = Pi::game->GetTime();
	for (Burn &burn : m_burns) {
		burn.startTime = gameTime;
		// the full-plan reset is a new plan: flown progress and completion
		// belong to the old one
		burn.status = PENDING;
		burn.appliedDeltaV = 0.0;
	}

	// identical start times collapse the chain; EnforceOrder re-spreads them
	// at >= prev + 1s so the m_burns-by-startTime invariant holds on this path too
	EnforceOrder();
	RecomputeChain();
	// every burn's status was reset: any cached burn direction is stale
	AdvanceDirectionLockSerial();
}

int TransferPlanner::InsertBurn(int afterIdx)
{
	// hard cap: a plan is a plan, not a wishlist
	if (m_burns.size() >= MAX_BURNS)
		return -1;

	Burn burn;
	if (m_burns.empty()) {
		// an empty plan seeds at the present so the first burn is anchored, not epoch 0
		burn.startTime = Pi::game->GetTime();
	} else {
		const int anchor = (afterIdx < 0) ? int(m_burns.size()) - 1
										  : std::min(afterIdx, int(m_burns.size()) - 1);
		// seed at the anchor's start time never before the present: appending seeds
		// one hour later, inserting at the anchor (EnforceOrder spreads collisions).
		// clamping to GetTime() keeps any new burn strictly after now, so its
		// start-time stepper is immediately adjustable instead of stuck at NOW
		burn.startTime = std::max(m_burns[anchor].startTime, Pi::game->GetTime()) +
			(afterIdx < 0 ? 3600.0 : 0.0);
	}

	m_burns.push_back(burn);
	int newIdx = int(m_burns.size()) - 1;

	EnforceOrder();
	RecomputeChain();
	// the new burn may now occupy an index another burn held before
	AdvanceDirectionLockSerial();
	return newIdx;
}

void TransferPlanner::RemoveBurn(int idx)
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return;

	m_burns.erase(m_burns.begin() + idx);
	RecomputeChain();
	// a later burn now occupies idx (and every index after it)
	AdvanceDirectionLockSerial();
}

double TransferPlanner::GetDv(BurnDirection d)
{
	return GetDv(0, d);
}

double TransferPlanner::GetDv(int idx, BurnDirection d) const
{
	if (idx < 0)
		return 0.0;
	return EnsureBurn(idx).dv[d];
}

void TransferPlanner::AddDv(BurnDirection d, double dv)
{
	AddDv(0, d, dv);
}

void TransferPlanner::AddDv(int idx, BurnDirection d, double dv)
{
	Burn &burn = EnsureBurn(idx);
	if (idx < 0)
		return;

	burn.dv[d] += m_factor * dv;

	RecomputeChain();
}

void TransferPlanner::ResetDv(BurnDirection d)
{
	if (m_burns.empty())
		return;

	m_burns[0].dv[d] = 0.0;

	// legacy behaviour: an inert, unanchored plan drops out
	if (IsUnsetStartTime(m_burns[0].startTime) &&
		GetOffsetVel(m_burns[0]).ExactlyEqual(vector3d(0., 0., 0.))) {
		RemoveBurn(0);
		return;
	}

	// same rule as ResetDv(int): a reset axis has flown nothing. No-op for a
	// plan with nothing completed, so the legacy index-less path is unchanged
	m_burns[0].status = PENDING;
	m_burns[0].appliedDeltaV = 0.0;

	RecomputeChain();
	AdvanceDirectionLockSerial();
}

void TransferPlanner::ResetDv(int idx)
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return;

	Burn &burn = m_burns[idx];
	for (int i = 0; i < 3; ++i)
		burn.dv[i] = 0.0;

	// legacy behaviour: an inert, unanchored burn drops out of the plan
	if (IsUnsetStartTime(burn.startTime) &&
		GetOffsetVel(burn).ExactlyEqual(vector3d(0., 0., 0.))) {
		RemoveBurn(idx);
		return;
	}

	// a burn whose planned delta-v is reset has flown nothing: its executed
	// progress is bookkeeping for the old plan, and completion is decided by the
	// player, so a reset drops it along with the values the burn was made of
	burn.status = PENDING;
	burn.appliedDeltaV = 0.0;

	RecomputeChain();
	AdvanceDirectionLockSerial();
}

void TransferPlanner::ResetDv(int idx, BurnDirection d)
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return;

	// targeted reset: only the selected axis changes; status, progress and
	// the burn's identity are preserved
	m_burns[idx].dv[d] = 0.0;

	RecomputeChain();
	AdvanceDirectionLockSerial();
}

void TransferPlanner::ResetStartTime(int idx)
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return;

	// clamp "now" into the slot between the neighbours so the ordering
	// invariant holds without moving any other burn
	double t = Pi::game->GetTime();
	if (idx > 0)
		t = std::max(t, m_burns[idx - 1].startTime + 1.0);
	if (idx + 1 < int(m_burns.size()))
		t = std::min(t, m_burns[idx + 1].startTime - 1.0);

	m_burns[idx].startTime = t;
	m_burns[idx].status = PENDING;
	m_burns[idx].appliedDeltaV = 0.0;

	RecomputeChain();
	AdvanceDirectionLockSerial();
}

void TransferPlanner::ResetDv()
{
	// drops the whole plan, statuses and executed delta-v included
	m_burns.clear();
	AdvanceDirectionLockSerial();
}

void TransferPlanner::SetPosition(const vector3d &position)
{
	Burn &burn = EnsureBurn(0);
	burn.pos = position;
	RecomputeChain();
}

int TransferPlanner::GetActiveIndex() const
{
	// m_burns is strictly ordered by startTime, so the earliest burn that has not
	// been completed is the first non-completed entry; -1 when every burn is flown
	for (size_t i = 0; i < m_burns.size(); ++i) {
		if (m_burns[i].status != COMPLETED)
			return int(i);
	}
	return -1;
}

int TransferPlanner::GetBurnStatus(int idx) const
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return PENDING;
	return m_burns[idx].status;
}

void TransferPlanner::CompleteBurn(int idx)
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return;

	// deliberately no EnforceOrder()/RecomputeChain(): completing a burn is
	// bookkeeping only, so it cannot move any other burn's geometry or start time
	m_burns[idx].status = COMPLETED;
	AdvanceDirectionLockSerial();
}

void TransferPlanner::ResetBurnStatus(int idx)
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return;

	m_burns[idx].status = PENDING;
	m_burns[idx].appliedDeltaV = 0.0;
	AdvanceDirectionLockSerial();
}

double TransferPlanner::GetBurnDeltaV(int idx) const
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return 0.0;
	return GetOffsetVel(m_burns[idx]).Length();
}

double TransferPlanner::GetBurnRemainingDeltaV(int idx) const
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return 0.0;
	// appliedDeltaV is accumulated as the projection of actual acceleration onto
	// the burn's unit vector, so the projected magnitude is that scalar itself
	return std::max(0.0, GetBurnDeltaV(idx) - m_burns[idx].appliedDeltaV);
}

void TransferPlanner::AddBurnProgress(int idx, double dv)
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return;

	Burn &burn = m_burns[idx];
	// a completed burn is closed: late thrust belongs to the next one, not this
	if (burn.status == COMPLETED)
		return;

	// keep progress within [0, planned delta-v]; thrust against the planned
	// direction claws progress back but never below "nothing flown"
	burn.appliedDeltaV = std::min(std::max(burn.appliedDeltaV + dv, 0.0), GetBurnDeltaV(idx));
}

TransferPlanner::Burn &TransferPlanner::EnsureBurn(int idx) const
{
	if (idx == 0 && m_burns.empty() && Pi::game)
		m_burns.push_back(Burn());
	if (idx < 0 || idx >= int(m_burns.size())) {
		static Burn s_empty;
		return s_empty;
	}
	return m_burns[idx];
}

void TransferPlanner::EnforceOrder()
{
	// keep burns strictly ordered by startTime, each at least 1s after the previous
	for (size_t i = 1; i < m_burns.size(); ++i) {
		const double minTime = m_burns[i - 1].startTime + 1.0;
		if (m_burns[i].startTime < minTime)
			m_burns[i].startTime = minTime;
	}

	// sorts whole Burn structs, so a burn's status and appliedDeltaV move with
	// it: re-anchoring start times can reorder the plan, never split a burn from
	// its completion state
	const auto byStartTime = [](const Burn &a, const Burn &b) { return a.startTime < b.startTime; };
	if (!std::is_sorted(m_burns.begin(), m_burns.end(), byStartTime)) {
		std::sort(m_burns.begin(), m_burns.end(), byStartTime);
		// an actual reorder moves burns between indices
		AdvanceDirectionLockSerial();
	}
}

void TransferPlanner::RecomputeChain()
{
	// an empty plan has no chain to compute; every accessor guards on this too
	if (m_burns.empty())
		return;

	Frame *frame = PlannerFrame();
	if (!frame)
		return;

	const double mass = frame->GetSystemBody()->GetMass();
	const double gameTime = Pi::game->GetTime();
	double segmentTime = gameTime;

	// seed the first segment from the live player state
	Orbit segment = Orbit::FromBodyState(
		Pi::player->GetPositionRelTo(frame->GetId()),
		Pi::player->GetVelocityRelTo(frame->GetId()),
		mass);

	for (Burn &burn : m_burns) {
		// hyperbolic legs chain exactly like elliptic ones: OrbitalPosAtTime /
		// OrbitalVelocityAtTime branch on eccentricity internally (the same
		// hyperbolic solver as Orbit.cpp's MeanAnomalyFromTrueAnomaly branch),
		// so no special casing is needed here - same mass, non-negative deltaT
		// only, which std::max(0.0, ...) guarantees.
		const double deltaT = std::max(0.0, burn.startTime - segmentTime);

		burn.pos = segment.OrbitalPosAtTime(deltaT);
		burn.vel = segment.OrbitalVelocityAtTime(mass, deltaT);
		segmentTime += deltaT;

		// the burn's offset velocity bends the next segment
		segment = Orbit::FromBodyState(burn.pos, burn.vel + GetOffsetVel(burn), mass);
	}
}

bool TransferPlanner::GetIncomingOrbit(int idx, Orbit &orbit, vector3d &pos, vector3d &vel, double &segmentTime) const
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return false;

	Frame *frame = PlannerFrame();
	if (!frame || !frame->GetSystemBody())
		return false;

	// the same chaining as RecomputeChain: burn 0 flies the live orbit from
	// now, every later burn the orbit the previous burn leaves behind, from
	// that burn's start time (never before now)
	segmentTime = Pi::game->GetTime();
	if (idx == 0) {
		pos = Pi::player->GetPositionRelTo(frame->GetId());
		vel = Pi::player->GetVelocityRelTo(frame->GetId());
	} else {
		for (int i = 0; i < idx; ++i)
			segmentTime = std::max(segmentTime, m_burns[i].startTime);
		const Burn &prev = m_burns[idx - 1];
		pos = prev.pos;
		vel = prev.vel + GetOffsetVel(prev);
	}

	orbit = Orbit::FromBodyState(pos, vel, frame->GetSystemBody()->GetMass());
	// FromBodyState falls back to a static body for a degenerate state
	return !is_zero_general(orbit.GetSemiMajorAxis());
}

bool TransferPlanner::GetNodeReference(vector3d &normal, std::string &name, bool &isTarget)
{
	Frame *frame = PlannerFrame();
	if (!frame || !frame->GetSystemBody())
		return false;

	// the navigation target's orbital plane, when it circles the same body:
	// a ship or station flying in this frame, or a moon of this body. Surface
	// stations ride the rotating frame and are left to the equator below.
	if (const Body *target = Pi::player->GetNavTarget()) {
		const SystemBody *targetBody = target->GetSystemBody();
		const bool orbitsCentre = target->GetFrame() == frame->GetId() ||
			(targetBody && targetBody->GetParent() == frame->GetSystemBody());
		if (orbitsCentre) {
			const vector3d h = target->GetPositionRelTo(frame->GetId()).Cross(target->GetVelocityRelTo(frame->GetId()));
			if (h.Length() > 0.0) {
				normal = h.Normalized();
				name = target->GetLabel();
				isTarget = true;
				return true;
			}
		}
	}

	// the body's equator: its rotation axis is the rotating frame's +y
	// (Frame::UpdateOrbitRails), a frame without one falls back to the
	// system's reference plane, which is the same axis
	const Frame *rotFrame = Frame::GetFrame(frame->GetRotFrame());
	normal = (rotFrame ? rotFrame->GetOrient() : matrix3x3d::Identity) * vector3d(0.0, 1.0, 0.0);
	name = frame->GetSystemBody()->GetName();
	isTarget = false;
	return true;
}

bool TransferPlanner::FindEventTime(int idx, SnapEvent event, double &time, double &period) const
{
	Orbit orbit;
	vector3d pos, vel;
	double segmentTime;
	if (!GetIncomingOrbit(idx, orbit, pos, vel, segmentTime))
		return false;

	const vector3d h = pos.Cross(vel);
	if (is_zero_general(h.Length()))
		return false;
	const vector3d axis = h.Normalized();
	const bool elliptic = orbit.GetEccentricity() < 1.0;

	// the direction from the body to the event, in the orbital plane
	vector3d eventDir;
	switch (event) {
	case SNAP_PERIAPSIS:
		eventDir = orbit.Perigeum();
		break;
	case SNAP_APOAPSIS:
		if (!elliptic)
			return false;
		eventDir = orbit.Apogeum();
		break;
	case SNAP_ASCENDING_NODE:
	case SNAP_DESCENDING_NODE: {
		vector3d normal;
		std::string name;
		bool isTarget;
		if (!GetNodeReference(normal, name, isTarget))
			return false;
		// the line of nodes points at the ascending node (k x h)
		const vector3d nodeLine = normal.Cross(axis);
		if (nodeLine.Length() < 1e-6)
			return false;
		eventDir = (event == SNAP_ASCENDING_NODE) ? nodeLine : -nodeLine;
		break;
	}
	}
	if (is_zero_general(eventDir.Length()))
		return false;

	// The burn must stay at least 1 s after the previous one (EnforceOrder),
	// so the search starts there. The angle swept from that point grows
	// monotonically with time, from 0 to 2pi over one period of an ellipse,
	// and towards the asymptote on an escape trajectory, so the event time is
	// found by bisection on the swept angle. Only OrbitalPosAtTime is used,
	// the same propagation the chain and the map draw with.
	const double tau0 = 1.0;
	const vector3d from = orbit.OrbitalPosAtTime(tau0);
	const double target = SweptAngle(from, eventDir, axis);
	const auto swept = [&](double tau) { return SweptAngle(from, orbit.OrbitalPosAtTime(tau), axis); };

	double lo = tau0, hi;
	if (elliptic) {
		hi = tau0 + orbit.Period();
	} else {
		// widen the window until it reaches the event; an event beyond the
		// asymptote (a passed periapsis, a node never crossed) is never reached
		hi = tau0 + 60.0;
		while (true) {
			const double s = swept(hi);
			if (std::isnan(s) || hi > kSnapHyperbolicHorizon)
				return false;
			if (s >= target)
				break;
			lo = hi;
			hi *= 2.0;
		}
	}

	for (int i = 0; i < 100 && hi - lo > 1e-3; ++i) {
		const double mid = 0.5 * (lo + hi);
		if (swept(mid) < target)
			lo = mid;
		else
			hi = mid;
	}
	time = segmentTime + hi;
	period = elliptic ? orbit.Period() : 0.0;
	return true;
}

bool TransferPlanner::GetSnapTime(int idx, SnapEvent event, double &time) const
{
	double period;
	if (!FindEventTime(idx, event, time, period))
		return false;

	// a burn already on an occurrence moves on to the next orbit's
	const double k = OccurrenceAt(m_burns[idx].startTime, time, period);
	if (period > 0.0 && k >= 0.0)
		time += (k + 1.0) * period;
	return true;
}

bool TransferPlanner::SnapStartTime(int idx, SnapEvent event)
{
	// snap against the chain as it stands now, not as it was last edited
	RecomputeChain();

	double time;
	if (!GetSnapTime(idx, event, time))
		return false;

	m_burns[idx].startTime = time;

	// a snap past later burns pushes them along, as the time stepper does
	EnforceOrder();
	RecomputeChain();
	return true;
}

bool TransferPlanner::GetCircularizePlan(int idx, double &time, int &apsis, double dv[3], double &magnitude) const
{
	Orbit orbit;
	vector3d pos, vel;
	double segmentTime;
	if (!GetIncomingOrbit(idx, orbit, pos, vel, segmentTime))
		return false;
	const double mass = PlannerFrame()->GetSystemBody()->GetMass();

	// where to burn: the apsis the burn already sits on (so a periapsis or
	// apoapsis picked by hand is kept), otherwise the next apsis ahead, and
	// the burn's own time when the orbit has neither ahead
	const double current = m_burns[idx].startTime;
	int onApsis = -1, nextApsis = -1;
	double nextTime = 0.0;
	for (const SnapEvent event : { SNAP_PERIAPSIS, SNAP_APOAPSIS }) {
		double t, period;
		if (!FindEventTime(idx, event, t, period))
			continue;
		if (OccurrenceAt(current, t, period) >= 0.0)
			onApsis = event;
		if (nextApsis < 0 || t < nextTime) {
			nextApsis = event;
			nextTime = t;
		}
	}
	if (onApsis >= 0) {
		time = current;
		apsis = onApsis;
	} else if (nextApsis >= 0) {
		time = nextTime;
		apsis = nextApsis;
	} else {
		time = current;
		apsis = -1;
	}

	// the state there, propagated exactly as RecomputeChain will
	const double tau = std::max(0.0, time - segmentTime);
	pos = orbit.OrbitalPosAtTime(tau);
	vel = orbit.OrbitalVelocityAtTime(mass, tau);
	const double r = pos.Length();
	if (is_zero_general(r) || is_zero_general(vel.Length()))
		return false;

	// the velocity wanted after the burn: circular speed along the horizontal
	// direction of the current motion, which keeps the orbital plane
	const vector3d radialDir = pos / r;
	const vector3d horizontal = vel - radialDir * vel.Dot(radialDir);
	if (horizontal.Length() < 1e-6 * vel.Length())
		return false;
	const vector3d change = horizontal.Normalized() * std::sqrt(G * mass / r) - vel;
	magnitude = change.Length();

	// the change lies in the orbital plane, so its normal part is zero
	return DecomposeDv(pos, vel, change, dv);
}

bool TransferPlanner::Circularize(int idx)
{
	// circularize against the chain as it stands now
	RecomputeChain();

	double time, magnitude, dv[3];
	int apsis;
	if (!GetCircularizePlan(idx, time, apsis, dv, magnitude))
		return false;

	Burn &burn = m_burns[idx];
	burn.startTime = time;
	for (int i = 0; i < 3; ++i)
		burn.dv[i] = dv[i];
	// new delta-v values make a new burn, as a reset does: nothing of it is
	// flown yet, and any cached burn direction is stale
	burn.status = PENDING;
	burn.appliedDeltaV = 0.0;

	// a move past later burns pushes them along, as a snap does
	EnforceOrder();
	RecomputeChain();
	AdvanceDirectionLockSerial();
	return true;
}

void TransferPlanner::IncreaseFactor(void)
{
	if (m_factor > 1000) return;
	m_factor *= m_factorFactor;
}
void TransferPlanner::ResetFactor(void) { m_factor = 1; }

void TransferPlanner::DecreaseFactor(void)
{
	if (m_factor < 0.0002) return;
	m_factor /= m_factorFactor;
}
