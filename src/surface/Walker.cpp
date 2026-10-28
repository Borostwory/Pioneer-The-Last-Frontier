// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Walker.h"

#include "Frame.h"
#include "Game.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Space.h"
#include "TerrainBody.h"
#include "ModelBody.h"
#include "collider/CollisionContact.h"
#include "collider/CollisionSpace.h"
#include "collider/Geom.h"
#include "collider/GeomTree.h"
#include "galaxy/SystemBody.h"
#include "gameconsts.h"

#include <algorithm>
#include <cmath>

namespace {
	constexpr double WALK_SPEED = 1.6; // m/s
	constexpr double RUN_SPEED = 4.5;
	// ground acceleration: traction times the local gravity, so low gravity
	// feels floaty and high gravity heavy; at least MIN_GROUND_ACCEL so the
	// walker still gets going on tiny moons
	constexpr double TRACTION = 1.2;
	constexpr double MIN_GROUND_ACCEL = 1.0; // m/s²
	constexpr double AIR_CONTROL = 0.1;		 // of the ground acceleration
	// the same push everywhere: about 0.5 m high on Earth, 3 m on the Moon
	constexpr double JUMP_SPEED = 3.1;
	// follows the ground down this far per step before falling
	constexpr double SNAP_DOWN = 0.5;
	// steepest walkable ground, tan(45°), give or take SLOPE_TOLERANCE metres a step
	constexpr double MAX_SLOPE = 1.0;
	constexpr double SLOPE_TOLERANCE = 0.01;
	// wall rays, above the step height
	constexpr double WALL_RAY_HEIGHTS[] = { 0.7, 1.5 };
	// terrain this close to sea level is sea
	constexpr double SEA_MARGIN = 0.05;
	// jetpack accelerations (m/s²): up beats Earth's gravity by about half
	constexpr double JET_ACCEL_UP = 15.0;
	constexpr double JET_ACCEL_DOWN = 6.0;
	constexpr double JET_ACCEL_SIDE = 6.0;
	// it pushes no faster than this, up and along the ground (m/s)
	constexpr double JET_MAX_CLIMB = 12.0;
	constexpr double JET_MAX_SPEED = 20.0;
	// room kept between the head and a ceiling
	constexpr double HEAD_CLEARANCE = 0.15;
	// EVA: the jetpack's push in every direction (m/s²), the fastest it goes
	// from the ship (m/s), and how much of its speed into the hull bounces back
	constexpr double EVA_ACCEL = 3.0;
	constexpr double EVA_MAX_SPEED = 10.0;
	constexpr double EVA_BOUNCE = 0.2;
	// within this of its length the line counts as taut (m)
	constexpr double EVA_TAUT_MARGIN = 0.25;

	// the orientation of a frame's axes in the root frame's
	matrix3x3d ToRoot(FrameId frame)
	{
		return Frame::GetFrame(frame)->GetOrientRelTo(Pi::game->GetSpace()->GetRootFrame());
	}

	// any unit vector perpendicular to up
	vector3d Perpendicular(const vector3d &up)
	{
		const vector3d axis = std::abs(up.y) < 0.9 ? vector3d(0.0, 1.0, 0.0) : vector3d(1.0, 0.0, 0.0);
		return (axis - up * up.Dot(axis)).Normalized();
	}

	// v along the ground (perpendicular to up), unit length
	vector3d AlongGround(const vector3d &v, const vector3d &up)
	{
		const vector3d flat = v - up * up.Dot(v);
		return flat.LengthSqr() > 1e-12 ? flat.Normalized() : Perpendicular(up);
	}
} // namespace

Walker::Walker() :
	m_vel(0.0),
	m_oldPos(0.0),
	m_forward(0.0, 0.0, -1.0),
	m_relPos(0.0),
	m_oldRelPos(0.0),
	m_relVel(0.0),
	m_evaOrient(matrix3x3d::Identity),
	m_evaHatch(0.0)
{
	SetLabel("Walker");
	SetPhysRadius(RADIUS);
	SetClipRadius(EYE_HEIGHT);
	// the camera is inside it
	SetFlag(FLAG_DRAW_EXCLUDE, true);
}

void Walker::PlaceOnGround(const vector3d &pos, const vector3d &forward)
{
	SetPosition(pos);
	m_oldPos = pos;
	m_interpPos = pos;
	m_vel = vector3d(0.0);
	m_groundRadius = pos.Length();
	m_onGround = true;
	m_forward = AlongGround(forward, pos.Normalized());
	UpdateOrient();
	m_interpOrient = GetOrient();
}

void Walker::SetMoveInput(double forward, double right, double up, bool run)
{
	m_moveForward = Clamp(forward, -1.0, 1.0);
	m_moveRight = Clamp(right, -1.0, 1.0);
	m_moveUp = Clamp(up, -1.0, 1.0);
	m_run = run;
}

vector3d Walker::UpdateJetpack(const vector3d &up, const vector3d &right, double dt)
{
	vector3d accel(0.0);
	double thrust = 0.0;
	if (!m_jetBurntOut && m_jetFuel > 0.0) {
		// up lifts off the ground; down and sideways push only in the air
		if (m_moveUp > 0.0 || (!m_onGround && m_moveUp < 0.0)) {
			accel += up * (m_moveUp * (m_moveUp > 0.0 ? JET_ACCEL_UP : JET_ACCEL_DOWN));
			thrust = std::abs(m_moveUp);
		}
		if (!m_onGround || m_moveUp > 0.0) {
			vector3d side = m_forward * m_moveForward + right * m_moveRight;
			const double len = side.Length();
			if (len > 1.0)
				side = side / len;
			if (len > 0.0) {
				accel += side * JET_ACCEL_SIDE;
				thrust = std::max(thrust, std::min(len, 1.0));
			}
		}
	}

	BurnJetpack(thrust, dt);
	return accel;
}

void Walker::BurnJetpack(double thrust, double dt)
{
	m_jetThrust = thrust;
	if (thrust > 0.0) {
		m_jetFuel = std::max(m_jetFuel - thrust * dt, 0.0);
		m_jetIdle = 0.0;
		if (m_jetFuel <= 0.0)
			m_jetBurntOut = true;
	} else {
		m_jetIdle += dt;
		if (m_jetIdle >= JET_REFILL_DELAY)
			m_jetFuel = std::min(m_jetFuel + JET_BURN_TIME / JET_REFILL_TIME * dt, JET_BURN_TIME);
		if (m_jetBurntOut && m_jetFuel >= JET_RELIGHT * JET_BURN_TIME)
			m_jetBurntOut = false;
	}
}

void Walker::NotifyRemoved(const Body *const removedBody)
{
	if (removedBody == m_evaShip)
		m_evaShip = nullptr;
}

void Walker::StartEva(const Body *ship, const vector3d &relPos, const matrix3x3d &orient, const vector3d &hatch)
{
	m_evaShip = ship;
	m_evaHatch = hatch;
	m_evaTaut = false;
	m_relPos = relPos;
	m_oldRelPos = relPos;
	m_relVel = vector3d(0.0);
	m_evaOrient = orient;
	m_onGround = false;
	FollowShip();
	m_oldPos = GetPosition();
	m_interpPos = GetPosition();
	m_interpOrient = GetOrient();
}

void Walker::LookEva(double yaw, double pitch)
{
	const vector3d right = m_evaOrient.VectorX();
	const vector3d up = m_evaOrient.VectorY();
	const vector3d forward = -m_evaOrient.VectorZ();

	// yaw about its up, then pitch about its new right
	const vector3d yawForward = forward * std::cos(yaw) + right * std::sin(yaw);
	const vector3d yawRight = right * std::cos(yaw) - forward * std::sin(yaw);
	const vector3d newForward = yawForward * std::cos(pitch) + up * std::sin(pitch);
	const vector3d newUp = up * std::cos(pitch) - yawForward * std::sin(pitch);
	m_evaOrient = matrix3x3d::FromVectors(yawRight, newUp, -newForward);
	m_evaOrient.Renormalize();
}

matrix3x3d Walker::GetEvaOrient() const
{
	return ToRoot(GetFrame()).Transpose() * m_evaOrient;
}

void Walker::FollowShip()
{
	if (!m_evaShip)
		return;
	if (GetFrame() != m_evaShip->GetFrame())
		SetFrame(m_evaShip->GetFrame());

	const matrix3x3d fromRoot = ToRoot(GetFrame()).Transpose();
	SetPosition(m_evaShip->GetPosition() + fromRoot * m_relPos);
	SetOrient(fromRoot * m_evaOrient);
	m_vel = m_evaShip->GetVelocity() + fromRoot * m_relVel;
}

void Walker::FollowShipInterp(double alpha)
{
	if (!m_evaShip)
		return;
	const FrameId root = Pi::game->GetSpace()->GetRootFrame();
	const matrix3x3d fromRoot = Frame::GetFrame(GetFrame())->GetInterpOrientRelTo(root).Transpose();
	m_interpPos = m_evaShip->GetInterpPosition() + fromRoot * (alpha * m_relPos + (1.0 - alpha) * m_oldRelPos);
	m_interpOrient = fromRoot * m_evaOrient;
}

bool Walker::HullHit(const vector3d &from, const vector3d &dir, double len, double &dist, vector3d &normal) const
{
	if (!m_evaShip || !m_evaShip->IsType(ObjectType::MODELBODY))
		return false;
	const Geom *geom = static_cast<const ModelBody *>(m_evaShip)->GetGeom();
	if (!geom)
		return false;
	const GeomTree *tree = geom->GetGeomTree();

	// root axes to the ship's model space
	const matrix3x3d shipToRoot = ToRoot(m_evaShip->GetFrame()) * m_evaShip->GetOrient();
	const matrix3x3d rootToShip = shipToRoot.Transpose();
	isect_t isect;
	isect.triIdx = -1;
	isect.dist = float(len);
	tree->TraceRay(vector3f(rootToShip * from), vector3f(rootToShip * dir), &isect);
	if (isect.triIdx == -1)
		return false;

	dist = isect.dist;
	normal = (shipToRoot * vector3d(tree->GetTriNormal(isect.triIdx))).NormalizedSafe();
	return true;
}

void Walker::EvaStep(double dt)
{
	// where the ship is now, consistent with its collision mesh
	FollowShip();
	m_oldRelPos = m_relPos;

	// the jetpack along its own axes, as a ship's thrusters
	const vector3d forward = -m_evaOrient.VectorZ();
	vector3d dir = forward * m_moveForward + m_evaOrient.VectorX() * m_moveRight + m_evaOrient.VectorY() * m_moveUp;
	const double len = dir.Length();
	if (len > 1.0)
		dir = dir / len;
	double thrust = 0.0;
	if (!m_jetBurntOut && m_jetFuel > 0.0 && len > 0.0) {
		m_relVel += dir * (EVA_ACCEL * dt);
		thrust = std::min(len, 1.0);
	}
	BurnJetpack(thrust, dt);
	if (m_relVel.LengthSqr() > EVA_MAX_SPEED * EVA_MAX_SPEED)
		m_relVel = m_relVel.Normalized() * EVA_MAX_SPEED;

	// The hull: tested against the ship's own collision mesh in its model
	// space. The collision space's trees are built before the ship moves, so
	// at orbital speed (a hundred metres a step) their rays miss the hull;
	// relative to the ship it doesn't matter how fast it goes or turns.
	// Slide along it, bouncing a little.
	vector3d move = m_relVel * dt;
	const double moveLen = move.Length();
	if (moveLen > 1e-9) {
		double dist;
		vector3d n;
		if (HullHit(m_relPos, move / moveLen, moveLen + RADIUS, dist, n)) {
			if (n.Dot(move) > 0.0)
				n = -n;
			move -= n * move.Dot(n);
			const double into = m_relVel.Dot(n);
			if (into < 0.0)
				m_relVel -= n * (into * (1.0 + EVA_BOUNCE));
		}
	}
	m_relPos += move;

	// kept RADIUS off the hull, where it came at the walker (the ship
	// turning) or an edge slipped past the ray above
	static const vector3d PROBES[] = {
		{ 1.0, 0.0, 0.0 }, { -1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 },
		{ 0.0, -1.0, 0.0 }, { 0.0, 0.0, 1.0 }, { 0.0, 0.0, -1.0 }
	};
	for (const vector3d &probe : PROBES) {
		double dist;
		vector3d n;
		if (HullHit(m_relPos, probe, RADIUS, dist, n)) {
			m_relPos -= probe * (RADIUS - dist);
			const double into = m_relVel.Dot(probe);
			if (into > 0.0)
				m_relVel -= probe * into;
		}
	}

	// the line to the hatch: held at its length, and taut within a hand's
	// breadth of it (the line is drawn straight then, see EvaTether)
	const vector3d hatch = ToRoot(GetFrame()) * (m_evaShip->GetOrient() * m_evaHatch);
	const vector3d fromHatch = m_relPos - hatch;
	const double dist = fromHatch.Length();
	m_evaTaut = dist > EVA_TETHER - EVA_TAUT_MARGIN;
	if (dist > EVA_TETHER) {
		const vector3d out = fromHatch / dist;
		m_relPos = hatch + out * EVA_TETHER;
		const double away = m_relVel.Dot(out);
		if (away > 0.0)
			m_relVel -= out * away;
	}

	FollowShip();
}

void Walker::Turn(double angle)
{
	const vector3d up = GetPosition().Normalized();
	const vector3d right = m_forward.Cross(up);
	m_forward = AlongGround(m_forward * std::cos(angle) + right * std::sin(angle), up);
	UpdateOrient();
}

void Walker::UpdateOrient()
{
	// model axes as for ships: x right, y up, looking along -z
	const vector3d up = GetPosition().Normalized();
	m_forward = AlongGround(m_forward, up);
	const vector3d right = m_forward.Cross(up);
	SetOrient(matrix3x3d::FromVectors(right, up, -m_forward));
}

void Walker::UpdateInterpTransform(double alpha)
{
	// in EVA the ship sets it, after its own (FollowShipInterp)
	if (IsEva())
		return;
	m_interpPos = alpha * GetPosition() + (1.0 - alpha) * m_oldPos;
	m_interpOrient = GetOrient();
}

double Walker::GroundRadius(const Frame *frame, const vector3d &pos, double above, double below, const Body **hitBody, const Geom *ignore)
{
	// the ground as drawn, not the terrain's exact height, which between the
	// mesh's vertices can be metres above or below it
	const vector3d up = pos.Normalized();
	const TerrainBody *terrain = static_cast<const TerrainBody *>(frame->GetBody());
	double ground = terrain->GetDrawnTerrainHeight(up);
	if (hitBody)
		*hitBody = nullptr;

	CollisionContact c;
	frame->GetCollisionSpace()->TraceRay(pos + up * above, -up, above + below, &c, ignore);
	if (c.userData1) {
		const double hit = c.pos.Length();
		if (hit > ground) {
			ground = hit;
			if (hitBody)
				*hitBody = static_cast<const Body *>(c.userData1);
		}
	}
	return ground;
}

bool Walker::IsSea(const TerrainBody *terrain, const vector3d &up, double groundRadius)
{
	if (!terrain->HasSea())
		return false;
	const double sea = terrain->GetSystemBody()->GetRadius() + SEA_MARGIN;
	// on a pier or a ship over the sea is not in it
	return terrain->GetTerrainHeight(up) <= sea && groundRadius <= sea;
}

bool Walker::HitWall(const Frame *frame, const vector3d &feet, const vector3d &up, const vector3d &dir, double len, vector3d &normal) const
{
	for (const double height : WALL_RAY_HEIGHTS) {
		CollisionContact c;
		frame->GetCollisionSpace()->TraceRay(feet + up * height, dir, len + RADIUS, &c);
		if (c.userData1) {
			normal = c.normal;
			return true;
		}
	}
	return false;
}

void Walker::TimeStepUpdate(const float timeStep)
{
	if (IsEva()) {
		EvaStep(timeStep);
		return;
	}
	m_oldPos = GetPosition();

	const Frame *frame = Frame::GetFrame(GetFrame());
	if (!frame || !frame->GetBody() || !frame->GetBody()->IsType(ObjectType::TERRAINBODY))
		return;
	const TerrainBody *terrain = static_cast<const TerrainBody *>(frame->GetBody());
	const double dt = timeStep;

	const vector3d pos = GetPosition();
	const vector3d up = pos.Normalized();
	m_forward = AlongGround(m_forward, up);
	const vector3d right = m_forward.Cross(up);
	const double g = G * terrain->GetMass() / pos.LengthSqr();

	double vUp = m_vel.Dot(up);
	vector3d vFlat = m_vel - up * vUp;

	const vector3d jet = UpdateJetpack(up, right, dt);
	const double jetUp = jet.Dot(up);
	const vector3d jetFlat = jet - up * jetUp;
	const bool jetSideways = jetFlat.LengthSqr() > 0.0;

	// steer the velocity along the ground towards the one asked for, unless
	// the jetpack is pushing in the air
	if (m_onGround || !jetSideways) {
		vector3d wish = m_forward * m_moveForward + right * m_moveRight;
		if (wish.LengthSqr() > 1.0)
			wish = wish.Normalized();
		wish = wish * (m_run ? RUN_SPEED : WALK_SPEED);

		double accel = std::max(TRACTION * g, MIN_GROUND_ACCEL);
		if (!m_onGround)
			accel *= AIR_CONTROL;
		vector3d dv = wish - vFlat;
		const double maxDv = accel * dt;
		if (dv.LengthSqr() > maxDv * maxDv)
			dv = dv.Normalized() * maxDv;
		vFlat += dv;
	}

	if (m_onGround) {
		vUp = 0.0;
		if (m_jumpRequested)
			vUp = JUMP_SPEED;
		// a jump, or the jetpack beating gravity, leaves the ground
		m_onGround = !m_jumpRequested && jetUp <= g;
	}
	if (!m_onGround) {
		// the jetpack climbs no faster than JET_MAX_CLIMB, and pushes along
		// the ground no faster than JET_MAX_SPEED
		const double climb = vUp < JET_MAX_CLIMB ? jetUp : std::min(jetUp, 0.0);
		vUp += (climb - g) * dt;
		if (jetSideways) {
			const double before = vFlat.Length();
			vFlat += jetFlat * dt;
			const double after = vFlat.Length();
			if (after > JET_MAX_SPEED && after > before)
				vFlat = vFlat * (std::max(before, JET_MAX_SPEED) / after);
		}
	}
	m_jumpRequested = false;

	// along the ground: slide along walls, stop at a second one
	vector3d move = vFlat * dt;
	double moveLen = move.Length();
	if (moveLen > 1e-9) {
		vector3d normal;
		if (HitWall(frame, pos, up, move / moveLen, moveLen, normal)) {
			vector3d n = normal - up * up.Dot(normal);
			if (n.LengthSqr() > 1e-12) {
				n = n.Normalized();
				if (n.Dot(move) > 0.0)
					n = -n;
				move -= n * move.Dot(n);
				vFlat -= n * vFlat.Dot(n);
			} else {
				move = vector3d(0.0);
			}
			moveLen = move.Length();
			if (moveLen > 1e-9 && HitWall(frame, pos, up, move / moveLen, moveLen, normal)) {
				move = vector3d(0.0);
				moveLen = 0.0;
			}
			if (moveLen <= 1e-9)
				vFlat = vector3d(0.0);
		}
	}

	vector3d next = pos + move + up * (vUp * dt);

	// the head against a ceiling (a hull over the jetpack)
	if (vUp > 0.0) {
		CollisionContact c;
		frame->GetCollisionSpace()->TraceRay(pos + up * EYE_HEIGHT, up, vUp * dt + HEAD_CLEARANCE, &c);
		if (c.userData1) {
			vUp = 0.0;
			next = pos + move;
		}
	}
	vector3d nextUp = next.Normalized();
	const double below = SNAP_DOWN + std::abs(vUp) * dt + 0.1;
	const Body *groundBody = nullptr;
	double ground = GroundRadius(frame, pos + move, STEP_HEIGHT, below, &groundBody);

	// no walking into the sea, up steps higher than STEP_HEIGHT or up terrain
	// steeper than MAX_SLOPE (steps on ships and buildings are fine); in the
	// air, ground ahead higher than a step is a cliff face, not a landing
	if (moveLen > 1e-9) {
		const double rise = ground - (m_onGround ? m_groundRadius : pos.Length());
		const bool tooHigh = rise > STEP_HEIGHT;
		const bool tooSteep = m_onGround && !groundBody && rise > MAX_SLOPE * moveLen + SLOPE_TOLERANCE;
		const bool intoSea = m_onGround && IsSea(terrain, nextUp, ground);
		if (intoSea || tooHigh || tooSteep) {
			next = pos + up * (vUp * dt);
			nextUp = next.Normalized();
			vFlat = vector3d(0.0);
			ground = GroundRadius(frame, pos, STEP_HEIGHT, below);
		}
	}

	// on the ground, or falling
	const double feet = next.Length();
	if (m_onGround) {
		if (ground >= feet - SNAP_DOWN)
			next = nextUp * ground;
		else
			m_onGround = false; // walked off an edge
	} else if (feet <= ground) {
		// never below the ground; coming down, it is a landing
		next = nextUp * ground;
		if (vUp <= 0.0) {
			vUp = 0.0;
			m_onGround = true;
		}
	}

	m_groundRadius = ground;
	m_vel = m_onGround ? vFlat : vFlat + up * vUp;
	SetPosition(next);
	UpdateOrient();
}
