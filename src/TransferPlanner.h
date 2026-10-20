// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#pragma once

#include "vector3.h"

#include <cstddef>
#include <vector>

class TransferPlanner {
public:
	enum BurnDirection {
		PROGRADE,
		NORMAL,
		RADIAL,
	};

	// completion state of a planned burn; PENDING/ACTIVE for display is derived
	// by the UI (start time vs. game time), only CompleteBurn() sets COMPLETED
	enum BurnStatus {
		PENDING = 0,
		ACTIVE = 1,
		COMPLETED = 2,
	};

	// a single planned burn; dv[] indices map to BurnDirection values
	struct Burn {
		double startTime = 0.0;
		double dv[3] = {0.0, 0.0, 0.0};
		vector3d pos = vector3d(0., 0., 0.);
		vector3d vel = vector3d(0., 0., 0.);
		int status = PENDING;
		double appliedDeltaV = 0.0; // executed delta-v, accumulated along the burn direction
	};

	TransferPlanner();

	// legacy no-index API: operates on burn 0, auto-creating it when the plan is empty
	vector3d GetVel() const;
	vector3d GetOffsetVel() const;
	vector3d GetPosition() const;
	double GetStartTime() const;
	void SetPosition(const vector3d &position);
	void IncreaseFactor(), ResetFactor(), DecreaseFactor();
	void AddStartTime(double timeStep);
	void ResetStartTime();
	double GetFactor() const { return m_factor; }
	void AddDv(BurnDirection d, double dv);
	double GetDv(BurnDirection d);
	void ResetDv(BurnDirection d);
	void ResetDv();

	// per-burn API
	size_t GetBurnCount() const { return m_burns.size(); }
	int InsertBurn(int afterIdx);
	void RemoveBurn(int idx);
	double GetDv(int idx, BurnDirection d) const;
	void AddDv(int idx, BurnDirection d, double dv);
	void ResetDv(int idx);
	double GetStartTime(int idx) const;
	void AddStartTime(int idx, double timeStep);
	vector3d GetPos(int idx) const;
	vector3d GetVel(int idx) const;
	vector3d GetOffsetVel(int idx) const;
	bool HasPlan() const { return !m_burns.empty(); }

	// burn status / executed-delta-v API; the "active" burn is the first one in
	// start-time order that has not been completed, so completing a burn advances
	// the plan without any of the other burns moving
	int GetActiveIndex() const;
	int GetBurnStatus(int idx) const;
	void CompleteBurn(int idx);
	void ResetBurnStatus(int idx);
	double GetBurnDeltaV(int idx) const;
	double GetBurnRemainingDeltaV(int idx) const;
	void AddBurnProgress(int idx, double dv);

private:
	vector3d GetOffsetVel(const Burn &burn) const;
	void RecomputeChain();
	void EnforceOrder();
	Burn &EnsureBurn(int idx) const;

	// mutable: read accessors auto-create burn 0 on first touch, mirroring the
	// legacy lazy-seeding behaviour
	mutable std::vector<Burn> m_burns; // strictly ordered by startTime
	double m_factor;				   // dv multiplier
	const double m_factorFactor = 5.0; // m_factor multiplier
};
