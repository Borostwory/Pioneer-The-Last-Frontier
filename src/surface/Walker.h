// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _WALKER_H
#define _WALKER_H

#include "Body.h"

class Frame;
class Geom;
class TerrainBody;

// The player on foot, outside the landed ship (see Excursion). A first-person
// avatar: no model, no collision geom and never drawn. It lives in the
// planet's rotating frame and moves itself: the terrain height and
// CollisionSpace::TraceRay against ships and buildings give the ground and
// the walls. Its position is the feet; the camera sits EYE_HEIGHT above.
//
// In EVA it floats beside the ship in space instead (StartEva): its position
// is then the eyes, it turns freely about its own axes, and the jetpack
// pushes it along them as a ship's thrusters.
//
// Saved with the game as any other body (Excursion keeps which one it is).
class Walker : public Body {
public:
	OBJDEF(Walker, Body, WALKER);

	static constexpr double EYE_HEIGHT = 1.7;
	static constexpr double RADIUS = 0.3;
	static constexpr double STEP_HEIGHT = 0.45;
	static constexpr double MASS = 100.0; // with the suit

	Walker();
	Walker(const Json &jsonObj, Space *space);
	void PostLoadFixup(Space *space) override;

	void Render(Graphics::Renderer *r, const Camera *camera, const vector3d &viewCoords, const matrix4x4d &viewTransform) override {}
	void TimeStepUpdate(const float timeStep) override;
	void UpdateInterpTransform(double alpha) override;
	void NotifyRemoved(const Body *const removedBody) override;
	vector3d GetVelocity() const override { return m_vel; }
	void SetVelocity(const vector3d &v) override { m_vel = v; }
	double GetMass() const override { return MASS; }

	// stands the walker at pos (its frame must be set, pos on the ground),
	// facing along forward (projected onto the ground)
	void PlaceOnGround(const vector3d &pos, const vector3d &forward);

	// what the player asks for: forward, right and jetpack up from -1 to 1
	void SetMoveInput(double forward, double right, double up, bool run);
	void RequestJump() { m_jumpRequested = true; }
	// turns the heading to the right by angle radians
	void Turn(double angle);

	// unit vector along the ground the walker faces, in its frame
	const vector3d &GetForward() const { return m_forward; }
	bool IsOnGround() const { return m_onGround; }

	// Jetpack: up (R) lifts off, down (F) and forward/right push in the air,
	// as a ship's thrusters. It burns for JET_BURN_TIME seconds at full
	// thrust, then refills over JET_REFILL_TIME once it has been idle for
	// JET_REFILL_DELAY. Burnt out, it relights at JET_RELIGHT of a full tank.
	static constexpr double JET_BURN_TIME = 8.0;
	static constexpr double JET_REFILL_TIME = 12.0;
	static constexpr double JET_REFILL_DELAY = 1.5;
	static constexpr double JET_RELIGHT = 0.2;
	// 0 to 1
	double GetJetFuel() const { return m_jetFuel / JET_BURN_TIME; }
	// 0 to 1, this step's thrust
	double GetJetThrust() const { return m_jetThrust; }
	bool IsJetBurntOut() const { return m_jetBurntOut; }

	// EVA: the walker keeps its position, velocity and orientation relative
	// to the ship, in root frame axes, and the ship carries it along (Player
	// calls FollowShip and FollowShipInterp after it has moved), so both share
	// the ship's orbit exactly. relPos: from the ship; orient: x right, y up,
	// looking along -z, in root frame axes; hatch: where the line is tied, in
	// the ship's model space
	void StartEva(const Body *ship, const vector3d &relPos, const matrix3x3d &orient, const vector3d &hatch);
	bool IsEva() const { return m_evaShip != nullptr; }
	// turns the view about its own axes: yaw to the right, pitch up (radians)
	void LookEva(double yaw, double pitch);
	// the view's orientation in the walker's frame
	matrix3x3d GetEvaOrient() const;
	void FollowShip();
	void FollowShipInterp(double alpha);

	// the farthest the line lets it drift from the hatch, metres
	static constexpr double EVA_TETHER = 300.0;
	// the line is holding it back (at EVA_TETHER, give or take a little)
	bool IsEvaTetherTaut() const { return m_evaTaut; }

	// Distance from the planet centre of the ground under pos: the terrain or,
	// if higher, the first collision geom below a ray from `above` metres over
	// pos down to `below` metres under it. hitBody: the body hit, if any;
	// ignore: a geom the ray passes through (the rover's own)
	static double GroundRadius(const Frame *frame, const vector3d &pos, double above, double below, const Body **hitBody = nullptr, const Geom *ignore = nullptr);
	// whether the ground at radius groundRadius in direction up is sea
	static bool IsSea(const TerrainBody *terrain, const vector3d &up, double groundRadius);

protected:
	void SaveToJson(Json &jsonObj, Space *space) override;

private:
	// a wall along the ground: rays at knee and chest height from feet along
	// dir (unit) for len plus the walker's radius; the hit normal if any
	bool HitWall(const Frame *frame, const vector3d &feet, const vector3d &up, const vector3d &dir, double len, vector3d &normal) const;
	// the jetpack's acceleration this step (empty without fuel), and burns or refills it
	vector3d UpdateJetpack(const vector3d &up, const vector3d &right, double dt);
	// burns thrust (0 to 1) for dt, or refills when idle
	void BurnJetpack(double thrust, double dt);
	void EvaStep(double dt);
	// a ray against the EVA ship's collision mesh, from `from` (relative to
	// the ship, root axes) along unit dir for len: the distance and the
	// surface normal (root axes) of the nearest hit
	bool HullHit(const vector3d &from, const vector3d &dir, double len, double &dist, vector3d &normal) const;
	void UpdateOrient();

	vector3d m_vel;
	vector3d m_oldPos;
	vector3d m_forward;
	double m_groundRadius = 0.0;
	bool m_onGround = false;

	double m_moveForward = 0.0;
	double m_moveRight = 0.0;
	double m_moveUp = 0.0;
	bool m_run = false;
	bool m_jumpRequested = false;

	double m_jetFuel = JET_BURN_TIME; // seconds of full thrust left
	double m_jetIdle = 0.0;			  // seconds since it last burnt
	double m_jetThrust = 0.0;
	bool m_jetBurntOut = false;

	const Body *m_evaShip = nullptr;
	Uint32 m_evaShipIndex = 0; // loading: m_evaShip's, until PostLoadFixup
	vector3d m_relPos;
	vector3d m_oldRelPos;
	vector3d m_relVel;
	matrix3x3d m_evaOrient;
	vector3d m_evaHatch;
	bool m_evaTaut = false;
};

#endif /* _WALKER_H */
