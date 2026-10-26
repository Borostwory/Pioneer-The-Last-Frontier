// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#pragma once

#include "vector3.h"

class Frame;

//
// Two-body building blocks the transfer planner chains its legs from. They
// follow the game's physics: a body's non-rotating frame is a sphere inside
// which only that body's gravity acts (DynamicBody::CalcExternalForce), the
// frame rides the body's orbit rails in its parent (Frame::UpdateOrbitRails),
// and a coasting ship follows its conic exactly (Orbit::PropagateState).
//
namespace PlannerConics {

	// a two-body conic: a state at epoch around mu (G * the central mass)
	struct Conic {
		vector3d pos = vector3d(0.0);
		vector3d vel = vector3d(0.0);
		double epoch = 0.0;
		double mu = 0.0;

		// the state at time t; false if it cannot be propagated
		bool At(double t, vector3d &p, vector3d &v) const;
		vector3d PosAt(double t) const;
	};

	// G * the mass of the frame's body, and the body's radius; 0 without one
	double FrameMu(const Frame *frame);
	double FrameBodyRadius(const Frame *frame);
	// where the frame is in its parent at time t, from the body's orbit rails;
	// zero for a frame without a parent
	vector3d FramePos(const Frame *frame, double t);
	vector3d FrameVel(const Frame *frame, double t);

	// The first time from `from` on (searched up to `from + horizon`) that the
	// conic - in frame - is outside frame's sphere, and the state then in the
	// parent frame. False if it stays inside.
	bool FindExit(const Conic &conic, const Frame *frame, double from, double horizon,
		double &exit, vector3d &pos, vector3d &vel);

	// The first time between from and to that the conic - in child's parent
	// frame - is inside child's sphere, and the state then relative to child.
	// False if it does not get in.
	bool FindEntry(const Conic &conic, const Frame *child, double from, double to,
		double &entry, vector3d &relPos, vector3d &relVel);

	// The first time between from and to that the conic comes down to radius
	// - the surface of its frame's body - on its way in. False if it does not
	// get there, or starts below it.
	bool FindImpact(const Conic &conic, double radius, double from, double to, double &time);

	// the first periapsis (or apoapsis) of the conic after from, within
	// horizon seconds; false if none comes
	bool NextApsis(const Conic &conic, bool periapsis, double from, double horizon, double &time);

	// the velocity change that leaves a body at pos / vel on a circular orbit
	// through pos around mu, in the plane it is flying
	vector3d CircularizeDv(const vector3d &pos, const vector3d &vel, double mu);

	// the periapsis and apoapsis radii of the conic of pos / vel around mu;
	// apoapsis is infinite on an escape trajectory
	void Apsides(const vector3d &pos, const vector3d &vel, double mu, double &periapsis, double &apoapsis);

} // namespace PlannerConics
