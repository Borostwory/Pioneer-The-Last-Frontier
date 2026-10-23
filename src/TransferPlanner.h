// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#pragma once

#include "FrameId.h"
#include "vector3.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Body;
class Frame;
class Orbit;

class TransferPlanner {
public:
	// sane cap for a plan; more burns than this is fiddling, not planning
	static constexpr size_t MAX_BURNS = 8;

	// the frame the planner works in for a body in frameId: its non-rotating
	// frame, or - inside an orbital station's gravity-free frame - the frame
	// of the body the station orbits. Every planner vector is relative to it.
	static Frame *GravityFrame(FrameId frameId);

	// Lambert's problem, single revolution: the velocities leaving r1 and
	// arriving at r2 tof seconds later around mu, turning the same way as
	// hRef; transferAngle is the angle swept. False where it has no reliable
	// answer, within about 0.1 degree of the ends being in line with the body.
	static bool SolveLambert(const vector3d &r1, const vector3d &r2, double tof, double mu, const vector3d &hRef,
		vector3d &v1, vector3d &v2, double &transferAngle);

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

	// orbital events a burn's node time can be snapped to, found on the orbit
	// leading into the burn: the live orbit for burn 0, otherwise the orbit
	// left behind by the previous burn
	enum SnapEvent {
		SNAP_PERIAPSIS,
		SNAP_APOAPSIS,
		SNAP_ASCENDING_NODE,
		SNAP_DESCENDING_NODE,
	};

	// a single planned burn; dv[] indices map to BurnDirection values
	struct Burn {
		double startTime = 0.0;
		double dv[3] = {0.0, 0.0, 0.0};
		vector3d pos = vector3d(0., 0., 0.);
		vector3d vel = vector3d(0., 0., 0.);
		int status = PENDING;
		double appliedDeltaV = 0.0; // executed delta-v, accumulated along the burn direction
		// the gravity frame pos / vel / dv are in: the frame of the body the
		// ship orbits at the burn - the planner's frame, or a moon's own frame
		// the chain enters on the way (RecomputeChain). Unset until chained
		FrameId frame;
		// the conic leading into the burn, in that frame: its state at segStart
		double segStart = 0.0;
		vector3d segPos = vector3d(0., 0., 0.);
		vector3d segVel = vector3d(0., 0., 0.);
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
	// resets a single axis of one burn, leaving its other axes untouched
	void ResetDv(int idx, BurnDirection d);
	// resets only the start time of one burn, leaving its neighbours untouched
	void ResetStartTime(int idx);
	double GetStartTime(int idx) const;
	void AddStartTime(int idx, double timeStep);
	vector3d GetPos(int idx) const;
	vector3d GetVel(int idx) const;
	vector3d GetOffsetVel(int idx) const;
	bool HasPlan() const { return !m_burns.empty(); }
	// the gravity frame burn idx is expressed in (see Burn::frame); invalid
	// for an unknown index
	FrameId GetBurnFrame(int idx) const;

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
	// sets the executed delta-v from what is still left of the burn, for a
	// closed-loop executor that measures the remainder directly
	void SetBurnRemainingDeltaV(int idx, double remaining);

	// snapping: the node time a snap of burn idx to the given event would set.
	// The first occurrence after the previous burn (or now) is chosen; a burn
	// already sitting on an occurrence moves on to the next orbit's. False when
	// the orbit has no such event ahead: no apoapsis on an escape trajectory, a
	// periapsis already passed on one, no nodes for an orbit in the reference
	// plane.
	bool GetSnapTime(int idx, SnapEvent event, double &time) const;
	bool SnapStartTime(int idx, SnapEvent event);
	// the plane the nodes are measured against: the navigation target's orbit
	// when the target circles the same body, otherwise that body's equator.
	// name is the target's or the body's label.
	static bool GetNodeReference(vector3d &normal, std::string &name, bool &isTarget);

	// circularizing: where burn idx should go and the prograde / normal /
	// radial delta-v that leaves it on a circular orbit there, in the plane it
	// is already flying. The place is the periapsis or apoapsis the burn
	// already sits on, otherwise the next one ahead on the orbit leading into
	// it (apsis = SNAP_PERIAPSIS / SNAP_APOAPSIS), or the burn's own time when
	// that orbit has neither ahead (apsis = -1). magnitude is the delta-v's
	// size. False for an inert burn or a purely radial trajectory.
	bool GetCircularizePlan(int idx, double &time, int &apsis, double dv[3], double &magnitude) const;
	// moves burn idx and replaces its delta-v as GetCircularizePlan says
	bool Circularize(int idx);

	// closest-approach planning (TransferPlannerApproach.cpp), against a
	// target orbiting the body the player orbits: a ship or station flying in
	// the planner's frame, or a moon / orbital station on rails around it.
	//
	// PlanApproach replaces the plan with burns from the ship's current orbit:
	// one that puts the ship on a transfer meeting the target (burns = 1,
	// intercept), and with burns = 2 a second one on arrival that matches the
	// target's velocity (rendezvous). The old plan is kept when it fails.
	// Departure and flight time are chosen for the least total delta-v. A ship
	// or station is met standOff metres behind it on its own orbit, so a
	// rendezvous parks the ship there instead of running into it. A planet or
	// moon is flown past (intercept only): the aim is set so that the pass
	// inside its sphere of influence, bent by its own gravity as the game
	// flies it, has its periapsis standOff metres above the surface. On
	// failure error says why.
	bool PlanApproach(const Body *target, int burns, double standOff, std::string &error);

	struct ClosestApproach {
		double time = 0.0;			// game time of the closest approach
		double distance = 0.0;		// between the ship and the target's centre
		double relativeSpeed = 0.0;
		double targetRadius = 0.0; // a planet's or moon's radius, 0 for ships and stations
	};
	// the closest approach of the planned trajectory (the live orbit, then
	// each burn's) to the target, from now until a revolution after the last
	// burn. On failure error says why.
	bool GetClosestApproach(const Body *target, ClosestApproach &result, std::string &error) const;

	// runtime-only lock-lifecycle serial: advances whenever a burn completes,
	// is reset, or may be replaced at an existing index (insert/remove/reorder,
	// plan resets). Consumers caching a burn direction compare it for equality
	// only, so wraparound is harmless. Ordinary delta-v edits do not advance it.
	// Never serialized.
	uint32_t GetDirectionLockSerial() const { return m_directionLockSerial; }

private:
	// the non-rotating frame of the body the player orbits: every planner
	// vector is relative to it
	static Frame *PlannerFrame();
	// splits a velocity change at a burn point onto GetOffsetVel's axes:
	// prograde (along vel), normal (along pos x vel), radial (along pos)
	static bool DecomposeDv(const vector3d &pos, const vector3d &vel, const vector3d &change, double dv[3]);
	vector3d GetOffsetVel(const Burn &burn) const;
	// The first time the conic seg (in a frame, its t = 0 at segStart, around
	// mass) enters the sphere of child, a moon's own frame inside it, between
	// from and to, and the state relative to child then. False if it does not.
	static bool FindChildFrameEntry(const Orbit &seg, double segStart, double mass, const Frame *child,
		double from, double to, double &entry, vector3d &relPos, vector3d &relVel);
	// the orbit the plan ends on - after its last burn, or the live one - with
	// its state at t = 0 and the game time t = 0 corresponds to
	bool GetFinalOrbit(Orbit &orbit, vector3d &pos, vector3d &vel, double &segmentTime) const;
	// PlanApproach's search, appending its burns after the plan's last burn
	bool AppendApproach(const Body *target, int burns, double standOff, std::string &error);
	// the orbit flown into burn idx, its state vector at t = 0 and the game
	// time t = 0 corresponds to, as RecomputeChain chains it
	bool GetIncomingOrbit(int idx, Orbit &orbit, vector3d &pos, vector3d &vel, double &segmentTime) const;
	// the first occurrence of an event after the previous burn (or now) on the
	// orbit leading into burn idx, and its period (0 on an escape trajectory)
	bool FindEventTime(int idx, SnapEvent event, double &time, double &period) const;
	void RecomputeChain();
	void EnforceOrder();
	Burn &EnsureBurn(int idx) const;
	// the single mutation point for m_directionLockSerial
	void AdvanceDirectionLockSerial() { ++m_directionLockSerial; }

	// mutable: read accessors auto-create burn 0 on first touch, mirroring the
	// legacy lazy-seeding behaviour
	mutable std::vector<Burn> m_burns; // strictly ordered by startTime
	double m_factor;				   // dv multiplier
	const double m_factorFactor = 5.0; // m_factor multiplier
	uint32_t m_directionLockSerial = 0; // derived runtime state, not serialized
};
