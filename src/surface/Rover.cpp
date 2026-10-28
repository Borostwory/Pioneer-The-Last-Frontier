// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Rover.h"

#include "DynamicBody.h"
#include "Frame.h"
#include "GameSaveError.h"
#include "JsonUtils.h"
#include "Lang.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Quaternion.h"
#include "TerrainBody.h"
#include "Walker.h"
#include "collider/CollisionContact.h"
#include "collider/CollisionSpace.h"
#include "gameconsts.h"
#include "scenegraph/FindNodeVisitor.h"
#include "scenegraph/MatrixTransform.h"
#include "scenegraph/Model.h"
#include "scenegraph/Tag.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace {
	constexpr double MAX_SPEED = 20.0; // m/s, 72 km/h
	constexpr double MAX_REVERSE = 6.0;
	constexpr double ENGINE_ACCEL = 4.0; // m/s²
	constexpr double BRAKE_DECEL = 8.0;
	constexpr double ROLL_DECEL = 0.4;
	// the most the tyres push along or across the ground: GRIP times the
	// local gravity, at least MIN_GRIP so it still gets going on tiny moons
	constexpr double GRIP = 0.9;
	constexpr double MIN_GRIP = 0.8; // m/s²
	// slower than this with no throttle, it holds on its brakes
	constexpr double HOLD_SPEED = 0.3;
	// the steering lock, narrowing with speed: halved at STEER_FALLOFF
	constexpr double MAX_STEER = DEG2RAD(32.0);
	constexpr double STEER_FALLOFF = 8.0; // m/s
	constexpr double STEER_RATE = 1.5;	  // rad/s
	// suspension travel (m), and how fast (1/s) the wheels follow the ground
	constexpr double SUSP_UP = 0.25;
	constexpr double SUSP_DOWN = 0.25;
	constexpr double SUSP_RATE = 20.0;
	// how much of a landing's speed (m/s) the suspension takes, in metres
	constexpr double LANDING_SQUASH = 0.04;
	// the chassis turns to the ground at ORIENT_RATE (1/s), leans no more than
	// MAX_TILT, and in the air slowly levels at AIR_LEVEL_RATE
	constexpr double ORIENT_RATE = 10.0;
	constexpr double AIR_LEVEL_RATE = 0.5;
	constexpr double MAX_TILT = DEG2RAD(40.0);
	// a wheel climbs a step this high in one go; higher is a wall
	constexpr double WHEEL_STEP = 0.5;
	// the ground under a wheel: from this far above the wheel down to this far below
	constexpr double PROBE_ABOVE = 1.0;
	constexpr double PROBE_BELOW = 2.0;
	// the tyres' width, as a share of their radius
	constexpr double TYRE_WIDTH = 0.8;
	// thrown upwards no faster than this by bumps (m/s)
	constexpr double MAX_LAUNCH = 3.0;
	// walls: rays across the chassis (fractions of its half width) at a few
	// heights (fractions of its collision box), stopping this far short (m),
	// bouncing back this much of the speed into them
	constexpr double RAY_ACROSS[] = { -0.9, 0.0, 0.9 };
	constexpr double RAY_HEIGHTS[] = { 0.05, 0.4, 0.75 };
	constexpr double WALL_MARGIN = 0.15;
	constexpr double BOUNCE = 0.2;
	// in the air the wheels spin down at this rate (1/s)
	constexpr double WHEEL_SPIN_DOWN = 0.5;

	const char *const CORNERS[4] = { "fl", "fr", "rl", "rr" };
	// without the rig in the model: the hubs of tlf_rover
	const vector3d DEFAULT_HUBS[4] = {
		{ -1.4, 0.7, -1.6 }, { 1.4, 0.7, -1.6 }, { -1.4, 0.7, 1.6 }, { 1.4, 0.7, 1.6 }
	};

	struct ModelInfo {
		bool checked = false;
		bool available = false;
		Aabb aabb;
	};

	// looked up once: a missing model is reported once, not every frame
	const ModelInfo &GetModelInfo()
	{
		static ModelInfo info;
		if (!info.checked) {
			info.checked = true;
			SceneGraph::Model *model = Pi::FindModel(Rover::MODEL_NAME, false);
			if (model && model->GetCollisionMesh()) {
				info.available = true;
				info.aabb = model->GetCollisionMesh()->GetAabb();
			}
		}
		return info;
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

	// v turned to the right by angle radians about up (unit)
	vector3d TurnRight(const vector3d &v, const vector3d &up, double angle)
	{
		const double c = std::cos(angle);
		const double s = std::sin(angle);
		return v * c + v.Cross(up) * s + up * (up.Dot(v) * (1.0 - c));
	}

	// x moved towards target by at most step
	double Toward(double x, double target, double step)
	{
		return x < target ? std::min(x + step, target) : std::max(x - step, target);
	}

	vector3d TagPoint(const SceneGraph::Model *model, const char *name, const vector3d &fallback)
	{
		const SceneGraph::Tag *tag = model->FindTagByName(name);
		return tag ? vector3d(tag->GetGlobalTransform().GetTranslate()) : fallback;
	}
} // namespace

bool Rover::IsModelAvailable()
{
	return GetModelInfo().available;
}

Aabb Rover::GetModelAabb()
{
	return GetModelInfo().aabb;
}

Rover::Rover() :
	m_wheelRadius(0.7),
	m_wheelbase(3.2),
	m_halfTrack(1.7),
	m_vel(0.0),
	m_forward(0.0, 0.0, -1.0),
	m_up(0.0, 1.0, 0.0),
	m_oldPos(0.0),
	m_oldOrient(matrix3x3d::Identity)
{
	SetModel(MODEL_NAME);
	// its name on the HUD's markers and as the nav target
	SetLabel(Lang::SURFACE_ROVER);
	SetClipRadius(GetModel()->GetDrawClipRadius());
	FindRig();
}

Rover::Rover(const Json &jsonObj, Space *space) :
	ModelBody(jsonObj, space),
	m_wheelRadius(0.7),
	m_wheelbase(3.2),
	m_halfTrack(1.7),
	m_vel(0.0),
	m_forward(0.0, 0.0, -1.0),
	m_up(0.0, 1.0, 0.0),
	m_oldPos(0.0),
	m_oldOrient(matrix3x3d::Identity)
{
	SetLabel(Lang::SURFACE_ROVER);
	FindRig();

	try {
		const Json &roverObj = jsonObj["rover"];
		m_vel = roverObj["vel"];
		m_forward = roverObj["forward"];
		m_up = roverObj["up"];
		m_vUp = roverObj["v_up"];
		m_speed = roverObj["speed"];
		m_onGround = roverObj["on_ground"];
		m_steer = roverObj["steer"];
		m_spin = roverObj["spin"];
		m_wheelSpeed = roverObj["wheel_speed"];
		const Json &wheels = roverObj["wheels"];
		for (size_t i = 0; i < 4 && i < wheels.size(); i++) {
			m_wheels[i].contact = wheels[i]["contact"];
			m_wheels[i].offset = wheels[i]["offset"];
		}
	} catch (Json::type_error &) {
		throw SavedGameCorruptException();
	}

	ResetInterp();
}

void Rover::SaveToJson(Json &jsonObj, Space *space)
{
	ModelBody::SaveToJson(jsonObj, space);

	Json roverObj = Json::object();
	roverObj["vel"] = m_vel;
	roverObj["forward"] = m_forward;
	roverObj["up"] = m_up;
	roverObj["v_up"] = m_vUp;
	roverObj["speed"] = m_speed;
	roverObj["on_ground"] = m_onGround;
	roverObj["steer"] = m_steer;
	roverObj["spin"] = m_spin;
	roverObj["wheel_speed"] = m_wheelSpeed;
	Json wheels = Json::array();
	for (const Wheel &w : m_wheels) {
		Json wheelObj = Json::object();
		wheelObj["contact"] = w.contact;
		wheelObj["offset"] = w.offset;
		wheels.push_back(wheelObj);
	}
	roverObj["wheels"] = wheels;
	jsonObj["rover"] = roverObj;
}

void Rover::ResetInterp()
{
	m_oldSpin = m_spin;
	m_oldSteer = m_steer;
	double offsets[4];
	for (int i = 0; i < 4; i++) {
		m_wheels[i].oldOffset = m_wheels[i].offset;
		offsets[i] = m_wheels[i].offset;
	}
	m_oldPos = m_interpPos = GetPosition();
	m_oldOrient = m_interpOrient = GetOrient();
	PoseRig(m_spin, m_steer, offsets);
}

void Rover::FindRig()
{
	SceneGraph::Group *root = GetModel()->GetRoot().Get();
	const vector3f up(0.f, 1.f, 0.f);
	const vector3f right(1.f, 0.f, 0.f);

	// every node of that name (one per detail level), with the model axis in
	// its own space; the model's nodes are this rover's own copies
	auto collect = [root](const std::string &name, const vector3f &modelAxis, std::vector<RigNode> &out) {
		SceneGraph::FindNodeVisitor finder(SceneGraph::FindNodeVisitor::MATCH_NAME_FULL, name);
		root->Accept(finder);
		for (SceneGraph::Node *n : finder.GetResults()) {
			SceneGraph::MatrixTransform *mt = dynamic_cast<SceneGraph::MatrixTransform *>(n);
			if (!mt)
				continue;
			const matrix3x3f toModel = mt->CalcGlobalTransform().GetOrient();
			out.push_back({ mt, mt->GetTransform(), (toModel.Transpose() * modelAxis).Normalized() });
		}
	};

	double radius = 0.0;
	for (int i = 0; i < 4; i++) {
		Wheel &w = m_wheels[i];
		const std::string corner = CORNERS[i];
		collect("susp_" + corner, up, w.susp);
		collect("steer_" + corner, up, w.steer);
		collect("wheel_" + corner, right, w.spin);
		w.hub = w.spin.empty() ? DEFAULT_HUBS[i] : vector3d(w.spin.front().node->CalcGlobalTransform().GetTranslate());
		radius += 0.25 * w.hub.y;
	}
	// the model stands on its wheels at y = 0
	if (radius > 0.1)
		m_wheelRadius = radius;
	const double wheelbase = std::abs(0.5 * (m_wheels[2].hub.z + m_wheels[3].hub.z) - 0.5 * (m_wheels[0].hub.z + m_wheels[1].hub.z));
	if (wheelbase > 0.5)
		m_wheelbase = wheelbase;
	m_halfTrack = 0.0;
	for (const Wheel &w : m_wheels)
		m_halfTrack = std::max(m_halfTrack, std::abs(w.hub.x) + 0.5 * TYRE_WIDTH * m_wheelRadius);
}

void Rover::Render(Graphics::Renderer *r, const Camera *camera, const vector3d &viewCoords, const matrix4x4d &viewTransform)
{
	RenderModel(r, camera, viewCoords, viewTransform);
}

bool Rover::OnCollision(Body *o, Uint32 flags, double relVel)
{
	// a ship flying into it bounces off; a landed one is no business of the
	// collision response, the rover's own rays keep it off the hull
	return o->IsType(ObjectType::DYNAMICBODY) && static_cast<DynamicBody *>(o)->IsMoving();
}

void Rover::SetDriveInput(double throttle, double steer, bool brake)
{
	m_throttle = Clamp(throttle, -1.0, 1.0);
	m_steerInput = Clamp(steer, -1.0, 1.0);
	m_brake = brake;
}

void Rover::SetDriven(bool driven)
{
	m_driven = driven;
	if (!driven)
		SetDriveInput(0.0, 0.0, true);
}

vector3d Rover::GetDriverEye() const
{
	return TagPoint(GetModel(), "tag_seat_driver", vector3d(-0.5, 2.43, -1.85));
}

vector3d Rover::GetExitPoint(int side) const
{
	return side == 0 ? TagPoint(GetModel(), "tag_exit_l", vector3d(-2.75, 0.0, -1.45)) :
					   TagPoint(GetModel(), "tag_exit_r", vector3d(2.75, 0.0, -1.45));
}

matrix3x3d Rover::MakeOrient() const
{
	// model axes as for ships: x right, y up, looking along -z
	const vector3d forward = AlongGround(m_forward, m_up);
	return matrix3x3d::FromVectors(forward.Cross(m_up), m_up, -forward);
}

bool Rover::FindContacts(const Frame *frame, const vector3d &pos, const matrix3x3d &orient, double contacts[4]) const
{
	const TerrainBody *terrain = static_cast<const TerrainBody *>(frame->GetBody());
	bool ok = true;
	for (int i = 0; i < 4; i++) {
		// under the wheel, measured from where it last touched the ground, or
		// in the air from where it is (at rest on the chassis' ground plane)
		const Wheel &w = m_wheels[i];
		const vector3d up = (pos + orient * vector3d(w.hub.x, 0.0, w.hub.z)).Normalized();
		const double from = m_onGround ? w.contact : (pos + orient * vector3d(w.hub.x, 0.0, w.hub.z)).Length();
		const double ground = Walker::GroundRadius(frame, up * from, PROBE_ABOVE, PROBE_BELOW, nullptr, GetGeom());
		if (ground - from > WHEEL_STEP || Walker::IsSea(terrain, up, ground))
			ok = false;
		contacts[i] = ground;
	}
	return ok;
}

void Rover::FitGround(const vector3d &pos, const matrix3x3d &orient, const double contacts[4], vector3d &up, double &radius) const
{
	vector3d c[4];
	for (int i = 0; i < 4; i++) {
		const Wheel &w = m_wheels[i];
		c[i] = (pos + orient * vector3d(w.hub.x, 0.0, w.hub.z)).Normalized() * contacts[i];
	}
	const vector3d along = (c[0] + c[1]) - (c[2] + c[3]); // rear to front
	const vector3d across = (c[1] + c[3]) - (c[0] + c[2]); // left to right
	const vector3d planetUp = pos.Normalized();

	// right × forward is up
	up = across.Cross(along).NormalizedSafe();
	if (up.Dot(planetUp) < std::cos(MAX_TILT)) {
		// the same lean, no more than MAX_TILT of it
		const vector3d lean = up - planetUp * up.Dot(planetUp);
		up = lean.LengthSqr() > 1e-12 ? planetUp * std::cos(MAX_TILT) + lean.Normalized() * std::sin(MAX_TILT) : planetUp;
	}
	radius = (c[0] + c[1] + c[2] + c[3]).Length() * 0.25;
}

bool Rover::HitObstacle(const Frame *frame, const vector3d &pos, const matrix3x3d &orient, const vector3d &dir, double len, double &dist, vector3d &normal) const
{
	// the way it goes in model space, flat
	vector3d d = orient.Transpose() * dir;
	d.y = 0.0;
	if (d.LengthSqr() < 1e-12)
		return false;
	d = d.Normalized();
	const vector3d across(-d.z, 0.0, d.x);

	// the collision box, as wide as the tyres stick out
	const Aabb &box = GetAabb();
	vector3d centre = (box.min + box.max) * 0.5;
	vector3d half = (box.max - box.min) * 0.5;
	half.x = std::max(half.x, m_halfTrack + std::abs(centre.x));
	centre.x = 0.0;
	// from the middle to the face of the box ahead, and to its sides
	const double reach = std::abs(d.x) * half.x + std::abs(d.z) * half.z;
	const double width = std::abs(across.x) * half.x + std::abs(across.z) * half.z;

	double nearest = DBL_MAX;
	for (const double a : RAY_ACROSS) {
		for (const double h : RAY_HEIGHTS) {
			vector3d local = centre + across * (a * width);
			local.y = box.min.y + h * (box.max.y - box.min.y);
			CollisionContact c;
			frame->GetCollisionSpace()->TraceRay(pos + orient * local, dir, reach + len + WALL_MARGIN, &c, GetGeom());
			// the planet's own sphere is not a wall
			if (!c.userData1 || c.userData1 == frame->GetBody())
				continue;
			// already into it (bumped there): let it drive out
			if (c.distance < 0.5 * reach)
				continue;
			if (c.distance < nearest) {
				nearest = c.distance;
				normal = c.normal;
			}
		}
	}
	// how far the box can go before it touches
	dist = nearest - reach;
	return nearest < DBL_MAX;
}

void Rover::PlaceOnGround(const vector3d &pos, const vector3d &forward)
{
	m_vel = vector3d(0.0);
	m_vUp = 0.0;
	m_speed = 0.0;
	m_wheelSpeed = 0.0;
	m_oldSpin = m_spin;
	m_steer = m_oldSteer = 0.0;
	m_onGround = true;
	m_up = pos.Normalized();
	m_forward = AlongGround(forward, m_up);

	vector3d at = pos;
	const Frame *frame = Frame::GetFrame(GetFrame());
	if (frame && frame->GetBody() && frame->GetBody()->IsType(ObjectType::TERRAINBODY)) {
		// the wheels look for the ground from the terrain under them
		const TerrainBody *terrain = static_cast<const TerrainBody *>(frame->GetBody());
		const matrix3x3d orient = MakeOrient();
		for (Wheel &w : m_wheels)
			w.contact = terrain->GetDrawnTerrainHeight((pos + orient * vector3d(w.hub.x, 0.0, w.hub.z)).Normalized());
		double contacts[4];
		FindContacts(frame, pos, orient, contacts);
		double radius;
		FitGround(pos, orient, contacts, m_up, radius);
		m_forward = AlongGround(m_forward, m_up);
		at = pos.Normalized() * radius;
		for (int i = 0; i < 4; i++)
			m_wheels[i].contact = contacts[i];
	}

	for (Wheel &w : m_wheels)
		w.offset = 0.0;
	SetPosition(at);
	SetOrient(MakeOrient());
	ResetInterp();
}

void Rover::TimeStepUpdate(const float timeStep)
{
	ModelBody::TimeStepUpdate(timeStep);

	m_oldPos = GetPosition();
	m_oldOrient = GetOrient();
	m_oldSpin = m_spin;
	m_oldSteer = m_steer;
	for (Wheel &w : m_wheels)
		w.oldOffset = w.offset;

	const Frame *frame = Frame::GetFrame(GetFrame());
	if ((m_asleep && !m_driven) || !frame || !frame->GetBody() || !frame->GetBody()->IsType(ObjectType::TERRAINBODY))
		return;
	const TerrainBody *terrain = static_cast<const TerrainBody *>(frame->GetBody());
	const double dt = timeStep;

	const vector3d pos = GetPosition();
	const vector3d planetUp = pos.Normalized();
	const double g = G * terrain->GetMass() / pos.LengthSqr();
	const double grip = std::max(GRIP * g, MIN_GRIP);

	// parked, it holds on its brakes
	const double throttle = m_driven ? m_throttle : 0.0;
	const bool brake = !m_driven || m_brake;

	const double lock = MAX_STEER / (1.0 + std::abs(m_speed) / STEER_FALLOFF);
	const double wantSteer = (m_driven ? m_steerInput : 0.0) * lock;
	m_steer += Clamp(wantSteer - m_steer, -STEER_RATE * dt, STEER_RATE * dt);

	// the velocity along the ground, as speed ahead and sideways; the
	// heading before it turns, if the turn has to be taken back
	const vector3d oldForward = m_forward;
	vector3d fwdH = AlongGround(m_forward, planetUp);
	vector3d rightH = fwdH.Cross(planetUp);
	const vector3d vFlat0 = m_vel - planetUp * m_vel.Dot(planetUp);
	double speed = vFlat0.Dot(fwdH);
	double lateral = vFlat0.Dot(rightH);

	if (m_onGround) {
		// gravity down the slope it stands on
		const vector3d downhill = (m_up - planetUp * m_up.Dot(planetUp)) * g;
		speed += downhill.Dot(fwdH) * dt;
		lateral += downhill.Dot(rightH) * dt;

		const double brakeStep = std::min(BRAKE_DECEL, grip) * dt;
		if (brake || (throttle == 0.0 && std::abs(speed) < HOLD_SPEED)) {
			// held, unless the slope beats the grip
			speed = Toward(speed, 0.0, brakeStep);
		} else if (throttle * speed < 0.0 && std::abs(speed) > HOLD_SPEED) {
			// pulling against the way it rolls brakes first (but not against
			// the creep the slope gives it from standing, or it never climbs)
			speed = Toward(speed, 0.0, brakeStep * std::abs(throttle));
		} else if (throttle != 0.0) {
			// up to the top speed for the throttle, but not holding it back downhill
			const double top = std::abs(throttle) * (throttle > 0.0 ? MAX_SPEED : MAX_REVERSE);
			const double pushed = speed + throttle * std::min(ENGINE_ACCEL, grip) * dt;
			speed = throttle > 0.0 ? std::min(pushed, std::max(speed, top)) : std::max(pushed, std::min(speed, -top));
		} else {
			speed = Toward(speed, 0.0, ROLL_DECEL * dt);
		}
		// the tyres hold it from sliding sideways
		lateral = Toward(lateral, 0.0, grip * dt);

		// turning as a bicycle with its front wheels steered, no tighter than
		// the grip allows; the velocity turns with it
		const double maxYaw = grip / std::max(std::abs(speed), 1.0);
		const double yaw = Clamp(speed * std::tan(m_steer) / m_wheelbase, -maxYaw, maxYaw) * dt;
		m_forward = TurnRight(m_forward, m_up, yaw);
		fwdH = AlongGround(m_forward, planetUp);
		rightH = fwdH.Cross(planetUp);
	}
	vector3d vFlat = fwdH * speed + rightH * lateral;

	// along the ground: slide along walls, stop at a second one
	matrix3x3d orient = MakeOrient();
	vector3d move = vFlat * dt;
	double moveLen = move.Length();
	if (moveLen > 1e-9) {
		double dist;
		vector3d normal;
		if (HitObstacle(frame, pos, orient, move / moveLen, moveLen, dist, normal)) {
			vector3d n = normal - planetUp * planetUp.Dot(normal);
			if (n.LengthSqr() > 1e-12) {
				n = n.Normalized();
				if (n.Dot(move) > 0.0)
					n = -n;
				move -= n * move.Dot(n);
				const double into = vFlat.Dot(n);
				if (into < 0.0)
					vFlat -= n * (into * (1.0 + BOUNCE));
			} else {
				move = vector3d(0.0);
				vFlat = vector3d(0.0);
			}
			moveLen = move.Length();
			if (moveLen > 1e-9 && HitObstacle(frame, pos, orient, move / moveLen, moveLen, dist, normal)) {
				move = vector3d(0.0);
				vFlat = vector3d(0.0);
			}
		}
	}

	// the wheels on the ground there: into the sea, or up a step too high,
	// it stops and bounces back a little; if turning alone swings a wheel
	// there, it doesn't turn either (else, turning on a shore, it settled on
	// the sea bed under the water)
	vector3d next = pos + move;
	double contacts[4];
	if (!FindContacts(frame, next, orient, contacts)) {
		if (move.LengthSqr() > 0.0) {
			const vector3d d = move.Normalized();
			const double into = vFlat.Dot(d);
			if (into > 0.0)
				vFlat -= d * (into * (1.0 + BOUNCE));
			next = pos;
		}
		if (!FindContacts(frame, next, orient, contacts)) {
			m_forward = oldForward;
			orient = MakeOrient();
			FindContacts(frame, next, orient, contacts);
		}
	}
	vector3d groundUp;
	double groundRadius;
	FitGround(next, orient, contacts, groundUp, groundRadius);

	// up and down: on its wheels the chassis follows the ground, falling to
	// it no faster than gravity while the wheels reach down for it; thrown
	// off a crest or an edge it flies until it lands
	const double r = next.Length();
	double newRadius;
	if (m_onGround) {
		const double lowest = r + (m_vUp - g * dt) * dt;
		if (groundRadius >= lowest - SUSP_DOWN) {
			newRadius = std::max(groundRadius, lowest);
			m_vUp = std::min((newRadius - r) / dt, MAX_LAUNCH);
		} else {
			m_onGround = false;
			m_vUp -= g * dt;
			newRadius = r + m_vUp * dt;
		}
	} else {
		m_vUp -= g * dt;
		newRadius = r + m_vUp * dt;
		if (newRadius <= groundRadius) {
			const double squash = std::min(-m_vUp * LANDING_SQUASH, SUSP_UP);
			for (Wheel &w : m_wheels)
				w.offset = std::min(w.offset + squash, SUSP_UP);
			newRadius = groundRadius;
			m_vUp = 0.0;
			m_onGround = true;
		}
	}
	const vector3d nextUp = next.Normalized();
	const vector3d newPos = nextUp * newRadius;

	// the chassis turns to the ground, or slowly levels in the air
	const vector3d wantUp = m_onGround ? groundUp : nextUp;
	const double k = 1.0 - std::exp(-(m_onGround ? ORIENT_RATE : AIR_LEVEL_RATE) * dt);
	m_up = (m_up + (wantUp - m_up) * k).Normalized();
	m_forward = AlongGround(m_forward, m_up);
	const matrix3x3d newOrient = MakeOrient();

	// the wheels: down to the ground within their travel, spinning as it rolls
	const double follow = 1.0 - std::exp(-SUSP_RATE * dt);
	for (int i = 0; i < 4; i++) {
		Wheel &w = m_wheels[i];
		const double atRest = (newPos + newOrient * vector3d(w.hub.x, 0.0, w.hub.z)).Length();
		const double want = m_onGround ? Clamp(contacts[i] - atRest, -SUSP_DOWN, SUSP_UP) : -SUSP_DOWN;
		w.offset += (want - w.offset) * follow;
		w.contact = contacts[i];
	}
	m_speed = vFlat.Dot(AlongGround(m_forward, nextUp));
	if (m_onGround)
		m_wheelSpeed = brake && std::abs(m_speed) < HOLD_SPEED ? 0.0 : m_speed;
	else
		m_wheelSpeed *= brake ? 0.0 : 1.0 - WHEEL_SPIN_DOWN * dt;
	m_spin += m_wheelSpeed * dt / m_wheelRadius;
	if (std::abs(m_spin) > 2.0 * M_PI) {
		const double turns = 2.0 * M_PI * std::floor(m_spin / (2.0 * M_PI));
		m_spin -= turns;
		m_oldSpin -= turns;
	}

	m_vel = vFlat + nextUp * m_vUp;
	SetPosition(newPos);
	SetOrient(newOrient);
}

void Rover::UpdateInterpTransform(double alpha)
{
	m_interpPos = alpha * GetPosition() + (1.0 - alpha) * m_oldPos;
	const Quaterniond from = Quaterniond::FromMatrix3x3(m_oldOrient);
	const Quaterniond to = Quaterniond::FromMatrix3x3(GetOrient());
	// Slerp blends nearly equal orientations linearly without normalising:
	// the matrix of that quaternion is not a rotation. The driver's camera
	// takes its orientation from this, and the frames then invert it by
	// transposing, so the planet's centre, 6000 km off, came out metres out
	// of place while turning: the drawn ground rose and fell under the
	// rover (nearby bodies hardly moved)
	m_interpOrient = Quaterniond::Slerp(from, to, alpha).Normalized().ToMatrix3x3<double>();
	m_interpOrient.Renormalize();

	double offsets[4];
	for (int i = 0; i < 4; i++)
		offsets[i] = m_wheels[i].oldOffset + (m_wheels[i].offset - m_wheels[i].oldOffset) * alpha;
	PoseRig(m_oldSpin + (m_spin - m_oldSpin) * alpha, m_oldSteer + (m_steer - m_oldSteer) * alpha, offsets);
}

void Rover::PoseRig(double spin, double steer, const double offsets[4])
{
	for (int i = 0; i < 4; i++) {
		Wheel &w = m_wheels[i];
		for (RigNode &n : w.susp)
			n.node->SetTransform(n.rest * matrix4x4f::Translation(n.axis * float(offsets[i])));
		// the front wheels steer; to the right is clockwise seen from above
		const float steerAngle = i < 2 ? float(-steer) : 0.f;
		for (RigNode &n : w.steer)
			n.node->SetTransform(n.rest * matrix4x4f::RotateMatrix(steerAngle, n.axis.x, n.axis.y, n.axis.z));
		// rolling forward (-z) the top of the wheel goes forward: backwards about the axle (+x)
		for (RigNode &n : w.spin)
			n.node->SetTransform(n.rest * matrix4x4f::RotateMatrix(float(-spin), n.axis.x, n.axis.y, n.axis.z));
	}
}
