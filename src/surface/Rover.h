// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _ROVER_H
#define _ROVER_H

#include "ModelBody.h"
#include "matrix4x4.h"

#include <vector>

class Frame;

namespace SceneGraph {
	class MatrixTransform;
}

// The surface rover, bought as ship equipment and driven by the player out
// of a landed ship (see Excursion). It lives in the planet's rotating frame
// and moves itself, as the walker does: each wheel finds the ground under it
// (the terrain, or the first collision geom below), the chassis sits on the
// plane through the four contacts, and rays along the way stop it at ships
// and buildings. Its own collision geom lets the walker stand on it and bump
// into it, and flying ships bounce off it.
//
// The model (MODEL_NAME) is driven by node name: for each corner C in fl, fr,
// rl and rr, susp_C moves the wheel up and down, steer_C turns it and wheel_C
// spins it about the axle. Its tags: tag_seat_driver (the driver's eyes),
// tag_exit_l and tag_exit_r (where the player stands after getting out).
//
// Saved with the game as any other body, driven or parked (Excursion keeps
// which one it is, and whether it is driven). Parked in a system the player
// leaves, Excursion keeps where it is and puts it back on the player's return.
class Rover : public ModelBody {
public:
	OBJDEF(Rover, ModelBody, ROVER);

	static constexpr const char *MODEL_NAME = "tlf_rover";
	static constexpr double MASS = 8000.0;

	// the model loads, and its collision box in model space
	static bool IsModelAvailable();
	static Aabb GetModelAabb();

	Rover();
	Rover(const Json &jsonObj, Space *space);

	void Render(Graphics::Renderer *r, const Camera *camera, const vector3d &viewCoords, const matrix4x4d &viewTransform) override;
	void TimeStepUpdate(const float timeStep) override;
	void UpdateInterpTransform(double alpha) override;
	// flying ships and cargo bounce off it; landed ships are left alone
	bool OnCollision(Body *o, Uint32 flags, double relVel) override;
	vector3d GetVelocity() const override { return m_vel; }
	void SetVelocity(const vector3d &v) override { m_vel = v; }
	double GetMass() const override { return MASS; }

	// sets it down on the ground at pos (its frame must be set), facing
	// along forward (projected onto the ground), at rest with its brakes on
	void PlaceOnGround(const vector3d &pos, const vector3d &forward);

	// what the driver asks for: throttle (forward) and steering (right) from
	// -1 to 1, and the brake. Without a driver it holds still on its brakes.
	void SetDriveInput(double throttle, double steer, bool brake);
	void SetDriven(bool driven);
	// parked with no one near (Excursion::OnShipMoved): it holds still where
	// it is, whatever the ground does as it is drawn coarser far from the
	// camera, and settles on the ground again once woken
	void SetAsleep(bool asleep) { m_asleep = asleep; }

	// m/s along its heading, negative in reverse
	double GetSpeed() const { return m_speed; }
	// unit vector it faces, in its frame
	const vector3d &GetForward() const { return m_forward; }
	bool IsOnGround() const { return m_onGround; }

	// points of the model, in its model space (from its tags)
	vector3d GetDriverEye() const;
	// side: 0 left, 1 right
	vector3d GetExitPoint(int side) const;

protected:
	void SaveToJson(Json &jsonObj, Space *space) override;

private:
	// one node of the wheel rig, in one LOD: its pose in the model, and the
	// model axis it moves along or about, in its own space
	struct RigNode {
		SceneGraph::MatrixTransform *node;
		matrix4x4f rest;
		vector3f axis;
	};

	struct Wheel {
		vector3d hub; // model space, at rest
		std::vector<RigNode> susp; // along the model's up
		std::vector<RigNode> steer; // about the model's up
		std::vector<RigNode> spin; // about the model's right (the axle)
		double contact = 0.0; // distance of the ground under it from the planet centre
		double offset = 0.0; // suspension: up from rest (m)
		double oldOffset = 0.0;
	};

	void FindRig();
	// the previous step and the drawn pose as they are now (just placed or loaded)
	void ResetInterp();
	// the distance from the planet centre of the ground under each wheel, for
	// the rover at pos with orient; false into the sea, or up a step too high
	// for a wheel to climb
	bool FindContacts(const Frame *frame, const vector3d &pos, const matrix3x3d &orient, double contacts[4]) const;
	// the chassis on the plane through the wheels' contacts: its up and the
	// distance of its origin from the planet centre
	void FitGround(const vector3d &pos, const matrix3x3d &orient, const double contacts[4], vector3d &up, double &radius) const;
	// the move along the ground cut short at walls (ships, buildings): rays
	// from the chassis along it at a few heights and across its width
	bool HitObstacle(const Frame *frame, const vector3d &pos, const matrix3x3d &orient, const vector3d &dir, double len, double &dist, vector3d &normal) const;
	matrix3x3d MakeOrient() const;
	void PoseRig(double spin, double steer, const double offsets[4]);

	Wheel m_wheels[4]; // fl, fr, rl, rr
	double m_wheelRadius;
	double m_wheelbase;
	// from the middle to the outside of the tyres
	double m_halfTrack;

	vector3d m_vel;
	vector3d m_forward;
	vector3d m_up;
	double m_vUp = 0.0;
	double m_speed = 0.0;
	bool m_onGround = false;

	double m_throttle = 0.0;
	double m_steerInput = 0.0;
	bool m_brake = true;
	bool m_driven = false;
	bool m_asleep = false;

	double m_steer = 0.0; // radians, to the right
	double m_spin = 0.0; // wheel angle rolled forward, radians
	double m_oldSpin = 0.0;
	double m_wheelSpeed = 0.0; // m/s at the tyre
	double m_oldSteer = 0.0;

	vector3d m_oldPos;
	matrix3x3d m_oldOrient;
};

#endif /* _ROVER_H */
