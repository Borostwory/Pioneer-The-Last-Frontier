// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TransferPlannerConics.h"

#include "Frame.h"
#include "Orbit.h"
#include "galaxy/SystemBody.h"
#include "gameconsts.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
// samples of a conic searched for its entry into a sphere, or for an apsis
constexpr int kEntrySamples = 2000;
constexpr int kApsisSamples = 720;
// bisection steps to a boundary or an apsis
constexpr int kRefineSteps = 60;
} // namespace

namespace PlannerConics {

	bool Conic::At(double t, vector3d &p, vector3d &v) const
	{
		p = pos;
		v = vel;
		return Orbit::PropagateState(p, v, mu, t - epoch);
	}

	vector3d Conic::PosAt(double t) const
	{
		vector3d p, v;
		At(t, p, v);
		return p;
	}

	double FrameMu(const Frame *frame)
	{
		return (frame && frame->GetSystemBody()) ? G * frame->GetSystemBody()->GetMass() : 0.0;
	}

	double FrameBodyRadius(const Frame *frame)
	{
		return (frame && frame->GetSystemBody()) ? frame->GetSystemBody()->GetRadius() : 0.0;
	}

	vector3d FramePos(const Frame *frame, double t)
	{
		if (!frame || !frame->GetSystemBody() || !frame->GetParent().valid())
			return vector3d(0.0);
		return frame->GetSystemBody()->GetOrbit().OrbitalPosAtTime(t);
	}

	vector3d FrameVel(const Frame *frame, double t)
	{
		return (FramePos(frame, t + 1.0) - FramePos(frame, t - 1.0)) * 0.5;
	}

	bool FindExit(const Conic &conic, const Frame *frame, double from, double horizon,
		double &exit, vector3d &pos, vector3d &vel)
	{
		if (!frame || !frame->GetParent().valid())
			return false;
		const double radius = frame->GetRadius();
		const auto outside = [&](double t) {
			vector3d p, v;
			return conic.At(t, p, v) && p.Length() >= radius;
		};

		// widen the window until the conic is out, then bisect to the edge
		double lo = from, hi = from;
		if (!outside(from)) {
			double step = 60.0;
			while (true) {
				hi = std::min(from + step, from + horizon);
				if (outside(hi))
					break;
				if (hi >= from + horizon)
					return false;
				lo = hi;
				step *= 1.6;
			}
			for (int i = 0; i < kRefineSteps && hi - lo > 1e-3; ++i) {
				const double mid = 0.5 * (lo + hi);
				if (outside(mid))
					hi = mid;
				else
					lo = mid;
			}
		}

		vector3d p, v;
		if (!conic.At(hi, p, v))
			return false;
		exit = hi;
		pos = p + FramePos(frame, hi);
		vel = v + FrameVel(frame, hi);
		return true;
	}

	bool FindEntry(const Conic &conic, const Frame *child, double from, double to,
		double &entry, vector3d &relPos, vector3d &relVel)
	{
		if (!child || !child->GetSystemBody() || to < from)
			return false;
		const double radius = child->GetRadius();
		const auto inside = [&](double t) {
			vector3d p, v;
			return conic.At(t, p, v) && (p - FramePos(child, t)).Length() < radius;
		};

		double before = from, after = -1.0;
		if (inside(from)) {
			after = from;
		} else {
			for (int i = 1; i <= kEntrySamples; ++i) {
				const double t = from + (to - from) * double(i) / double(kEntrySamples);
				if (inside(t)) {
					after = t;
					break;
				}
				before = t;
			}
			if (after < 0.0)
				return false;
			for (int i = 0; i < kRefineSteps && after - before > 1e-3; ++i) {
				const double mid = 0.5 * (before + after);
				if (inside(mid))
					after = mid;
				else
					before = mid;
			}
		}

		vector3d p, v;
		if (!conic.At(after, p, v))
			return false;
		entry = after;
		relPos = p - FramePos(child, after);
		relVel = v - FrameVel(child, after);
		return true;
	}

	bool NextApsis(const Conic &conic, bool periapsis, double from, double horizon, double &time)
	{
		// an apsis is where the radial velocity changes sign: from inwards to
		// outwards at periapsis, the other way round at apoapsis
		const auto radial = [&](double t) {
			vector3d p, v;
			if (!conic.At(t, p, v))
				return std::numeric_limits<double>::quiet_NaN();
			return p.Dot(v);
		};
		const auto crossed = [&](double a, double b) {
			return periapsis ? (a < 0.0 && b >= 0.0) : (a > 0.0 && b <= 0.0);
		};

		double prevT = from, prev = radial(from);
		if (std::isnan(prev))
			return false;
		for (int i = 1; i <= kApsisSamples; ++i) {
			const double t = from + horizon * double(i) / double(kApsisSamples);
			const double cur = radial(t);
			if (std::isnan(cur))
				return false;
			if (crossed(prev, cur)) {
				double lo = prevT, hi = t;
				for (int k = 0; k < kRefineSteps && hi - lo > 1e-3; ++k) {
					const double mid = 0.5 * (lo + hi);
					const double rm = radial(mid);
					if (crossed(prev, rm))
						hi = mid;
					else
						lo = mid;
				}
				time = hi;
				return true;
			}
			prevT = t;
			prev = cur;
		}
		return false;
	}

	vector3d CircularizeDv(const vector3d &pos, const vector3d &vel, double mu)
	{
		const vector3d radialDir = pos.NormalizedSafe();
		const vector3d horizontal = (vel - radialDir * vel.Dot(radialDir)).NormalizedSafe();
		return horizontal * std::sqrt(mu / pos.Length()) - vel;
	}

	void Apsides(const vector3d &pos, const vector3d &vel, double mu, double &periapsis, double &apoapsis)
	{
		const double hSqr = pos.Cross(vel).LengthSqr();
		const double energy = 0.5 * vel.LengthSqr() - mu / pos.Length();
		const double e = std::sqrt(std::max(0.0, 1.0 + 2.0 * energy * hSqr / (mu * mu)));
		const double p = hSqr / mu;
		periapsis = p / (1.0 + e);
		apoapsis = (e < 1.0) ? p / (1.0 - e) : std::numeric_limits<double>::infinity();
	}

} // namespace PlannerConics
