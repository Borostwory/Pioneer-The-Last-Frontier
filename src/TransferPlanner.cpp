// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TransferPlanner.h"

#include "Frame.h"
#include "Game.h"
#include "Orbit.h"
#include "Pi.h"
#include "Player.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace {
// a burn whose startTime is effectively unset; seeds at Pi::game->GetTime() on first touch
bool IsUnsetStartTime(double t) { return std::fabs(t) < 1.; }

// sane cap for a plan; more burns than this is fiddling, not planning
constexpr size_t kMaxBurns = 8;
} // namespace

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

	// legacy behaviour: a plan dragged back to (or before) the present is cleared
	if (HasPlan() && GetStartTime(0) <= Pi::game->GetTime())
		m_burns.clear();
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
	for (Burn &burn : m_burns)
		burn.startTime = gameTime;

	// identical start times collapse the chain; EnforceOrder re-spreads them
	// at >= prev + 1s so the m_burns-by-startTime invariant holds on this path too
	EnforceOrder();
	RecomputeChain();
}

int TransferPlanner::InsertBurn(int afterIdx)
{
	// hard cap: a plan is a plan, not a wishlist
	if (m_burns.size() >= kMaxBurns)
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
	return newIdx;
}

void TransferPlanner::RemoveBurn(int idx)
{
	if (idx < 0 || idx >= int(m_burns.size()))
		return;

	m_burns.erase(m_burns.begin() + idx);
	RecomputeChain();
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

	RecomputeChain();
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

	RecomputeChain();
}

void TransferPlanner::ResetDv()
{
	m_burns.clear();
}

void TransferPlanner::SetPosition(const vector3d &position)
{
	Burn &burn = EnsureBurn(0);
	burn.pos = position;
	RecomputeChain();
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

	std::sort(m_burns.begin(), m_burns.end(),
		[](const Burn &a, const Burn &b) { return a.startTime < b.startTime; });
}

void TransferPlanner::RecomputeChain()
{
	// an empty plan has no chain to compute; every accessor guards on this too
	if (m_burns.empty())
		return;

	Frame *frame = Frame::GetFrame(Frame::GetFrame(Pi::player->GetFrame())->GetNonRotFrame());
	if (!frame)
		return;

	const double mass = frame->GetSystemBody()->GetMass();
	const double gameTime = Pi::game->GetTime();

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
		const double deltaT = std::max(0.0, burn.startTime - gameTime);

		burn.pos = segment.OrbitalPosAtTime(deltaT);
		burn.vel = segment.OrbitalVelocityAtTime(mass, deltaT);

		// the burn's offset velocity bends the next segment
		segment = Orbit::FromBodyState(burn.pos, burn.vel + GetOffsetVel(burn), mass);
	}
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
