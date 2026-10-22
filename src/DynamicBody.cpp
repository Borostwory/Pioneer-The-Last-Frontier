// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "DynamicBody.h"

#include "Frame.h"
#include "GameSaveError.h"
#include "JsonUtils.h"
#include "Planet.h"
#include "Space.h"
#include "collider/CollisionContact.h"
#include "gameconsts.h"
#include "ship/Propulsion.h"

#include <algorithm>
#include <cmath>

static const float KINETIC_ENERGY_MULT = 0.00001f;
const double DynamicBody::DEFAULT_DRAG_COEFF = 0.1; // 'smooth sphere'

namespace {
// Stumpff functions C(z) and S(z) of the universal-variable formulation
double StumpffC(double z)
{
	if (z > 1e-6)
		return (1.0 - std::cos(std::sqrt(z))) / z;
	if (z < -1e-6)
		return (std::cosh(std::sqrt(-z)) - 1.0) / -z;
	return 0.5 - z / 24.0 + z * z / 720.0;
}

double StumpffS(double z)
{
	if (z > 1e-6) {
		const double s = std::sqrt(z);
		return (s - std::sin(s)) / (s * s * s);
	}
	if (z < -1e-6) {
		const double s = std::sqrt(-z);
		return (std::sinh(s) - s) / (s * s * s);
	}
	return 1.0 / 6.0 - z / 120.0 + z * z / 5040.0;
}

//
// Moves pos / vel along their two-body conic around a point mass mu for dt
// seconds, exactly for any dt: universal-variable Kepler propagation with the
// Lagrange f and g coefficients (Curtis, Orbital Mechanics for Engineering
// Students, algorithms 3.3 and 3.4). Leaves them untouched and returns false
// when the solver does not converge (a near-radial fall, say), so the caller
// can fall back to a plain integration step.
//
bool KeplerDrift(vector3d &pos, vector3d &vel, double mu, double dt)
{
	const double r0 = pos.Length();
	if (r0 <= 0.0 || mu <= 0.0)
		return false;
	const double sqrtMu = std::sqrt(mu);
	const double vr0 = pos.Dot(vel) / r0;
	const double alpha = 2.0 / r0 - vel.LengthSqr() / mu; // 1 / semi-major axis

	// Newton on the universal anomaly chi
	double chi = sqrtMu * std::fabs(alpha) * dt;
	if (!(std::fabs(chi) > 0.0))
		chi = sqrtMu * dt / r0;
	bool converged = false;
	for (int i = 0; i < 50; ++i) {
		const double z = alpha * chi * chi;
		const double C = StumpffC(z), S = StumpffS(z);
		const double F = r0 * vr0 / sqrtMu * chi * chi * C + (1.0 - alpha * r0) * chi * chi * chi * S + r0 * chi - sqrtMu * dt;
		const double dF = r0 * vr0 / sqrtMu * chi * (1.0 - alpha * chi * chi * S) + (1.0 - alpha * r0) * chi * chi * C + r0;
		if (!std::isfinite(F) || !std::isfinite(dF) || dF == 0.0)
			return false;
		const double step = F / dF;
		chi -= step;
		if (std::fabs(step) <= 1e-12 * std::max(1.0, std::fabs(chi))) {
			converged = true;
			break;
		}
	}
	if (!converged)
		return false;

	const double z = alpha * chi * chi;
	const double C = StumpffC(z), S = StumpffS(z);
	const double f = 1.0 - chi * chi / r0 * C;
	const double g = dt - chi * chi * chi / sqrtMu * S;
	const vector3d newPos = f * pos + g * vel;
	const double r = newPos.Length();
	if (!(r > 0.0) || !std::isfinite(r))
		return false;
	const double fDot = sqrtMu / (r * r0) * (alpha * chi * chi * chi * S - chi);
	const double gDot = 1.0 - chi * chi / r * C;
	const vector3d newVel = fDot * pos + gDot * vel;
	if (!std::isfinite(newVel.x) || !std::isfinite(newVel.y) || !std::isfinite(newVel.z))
		return false;

	pos = newPos;
	vel = newVel;
	return true;
}
} // namespace

DynamicBody::DynamicBody() :
	ModelBody()
{
	m_dragCoeff = DEFAULT_DRAG_COEFF;
	m_flags = Body::FLAG_CAN_MOVE_FRAME;
	m_oldPos = GetPosition();
	m_oldAngDisplacement = vector3d(0.0);
	m_force = vector3d(0.0);
	m_torque = vector3d(0.0);
	m_vel = vector3d(0.0);
	m_angVel = vector3d(0.0);
	m_mass = 1;
	m_angInertia = 1;
	m_massRadius = 1;
	m_isMoving = true;
	m_atmosForce = vector3d(0.0);
	m_gravityForce = vector3d(0.0);
	m_externalForce = vector3d(0.0); // do external forces calc instead?
	m_lastForce = vector3d(0.0);
	m_lastTorque = vector3d(0.0);
	m_aiMessage = AIError::AIERROR_NONE;
	m_decelerating = false;
}

DynamicBody::DynamicBody(const Json &jsonObj, Space *space) :
	ModelBody(jsonObj, space),
	m_dragCoeff(DEFAULT_DRAG_COEFF),
	m_externalForce(vector3d(0.0)),
	m_atmosForce(vector3d(0.0)),
	m_gravityForce(vector3d(0.0)),
	m_lastForce(vector3d(0.0)),
	m_lastTorque(vector3d(0.0))
{
	m_flags = Body::FLAG_CAN_MOVE_FRAME;
	m_oldPos = GetPosition();
	m_oldAngDisplacement = vector3d(0.0);

	try {
		Json dynamicBodyObj = jsonObj["dynamic_body"];

		m_force = dynamicBodyObj["force"];
		m_torque = dynamicBodyObj["torque"];
		m_vel = dynamicBodyObj["vel"];
		m_angVel = dynamicBodyObj["ang_vel"];
		m_mass = dynamicBodyObj["mass"];
		m_massRadius = dynamicBodyObj["mass_radius"];
		m_angInertia = dynamicBodyObj["ang_inertia"];
		SetMoving(dynamicBodyObj["is_moving"]);
	} catch (Json::type_error &) {
		throw SavedGameCorruptException();
	}

	m_aiMessage = AIError::AIERROR_NONE;
	m_decelerating = false;
}

void DynamicBody::SetMoving(bool isMoving)
{
	m_isMoving = isMoving;

	if (!m_isMoving) {
		m_vel = vector3d(0.0);
		m_angVel = vector3d(0.0);
		m_force = vector3d(0.0);
		m_torque = vector3d(0.0);
	}
}

void DynamicBody::SaveToJson(Json &jsonObj, Space *space)
{
	ModelBody::SaveToJson(jsonObj, space);

	Json dynamicBodyObj = Json::object(); // Create JSON object to contain dynamic body data.

	dynamicBodyObj["force"] = m_force;
	dynamicBodyObj["torque"] = m_torque;
	dynamicBodyObj["vel"] = m_vel;
	dynamicBodyObj["ang_vel"] = m_angVel;
	dynamicBodyObj["mass"] = m_mass;
	dynamicBodyObj["mass_radius"] = m_massRadius;
	dynamicBodyObj["ang_inertia"] = m_angInertia;
	dynamicBodyObj["is_moving"] = m_isMoving;

	jsonObj["dynamic_body"] = dynamicBodyObj; // Add dynamic body object to supplied object.
}

void DynamicBody::GetCurrentAtmosphericState(double &pressure, double &density) const
{
	Frame *f = Frame::GetFrame(GetFrame());
	Body *body = f->GetBody();
	if (!body || !f->IsRotFrame() || !body->IsType(ObjectType::PLANET)) {
		pressure = density = 0;
		return;
	}
	Planet *planet = static_cast<Planet *>(body);
	planet->GetAtmosphericState(GetPosition().Length(), &pressure, &density);
}

void DynamicBody::PostLoadFixup(Space *space)
{
	Body::PostLoadFixup(space);
	m_oldPos = GetPosition();
	//	CalcExternalForce();		// too dangerous
}

DynamicBody::~DynamicBody()
{
}

void DynamicBody::SetForce(const vector3d &f)
{
	m_force = f;
}

void DynamicBody::AddForce(const vector3d &f)
{
	m_force += f;
}

void DynamicBody::AddTorque(const vector3d &t)
{
	m_torque += t;
}

void DynamicBody::AddRelForce(const vector3d &f)
{
	m_force += GetOrient() * f;
}

void DynamicBody::AddRelTorque(const vector3d &t)
{
	m_torque += GetOrient() * t;
}

void DynamicBody::SetTorque(const vector3d &t)
{
	m_torque = t;
}

void DynamicBody::SetMass(double mass)
{
	m_mass = mass;
	// This is solid sphere mass distribution, my friend
	m_angInertia = (2 / 5.0) * m_mass * m_massRadius * m_massRadius;
}

void DynamicBody::SetFrame(FrameId fId)
{
	ModelBody::SetFrame(fId);
	// external forces will be wrong after frame transition
	m_externalForce = m_gravityForce = m_atmosForce = vector3d(0.0);
}

double DynamicBody::CalcAtmosphericDrag(double velSqr, double area, double coeff) const
{
	double pressure, density;
	GetCurrentAtmosphericState(pressure, density);

	// Simplified calculation of atmospheric drag/lift force.
	return density > 0 ? 0.5 * density * velSqr * area * coeff : 0;
}

vector3d DynamicBody::CalcAtmosphericForce() const
{
	vector3d dragDir = -m_vel.NormalizedSafe();

	// We assume the object is a perfect sphere in the size of the clip radius.
	// Most things are /not/ using the default DynamicBody code, but this is still better than before.
	return CalcAtmosphericDrag(m_vel.LengthSqr(), GetClipRadius() * GetClipRadius() * M_PI, m_dragCoeff) * dragDir;
}

void DynamicBody::CalcExternalForce()
{
	// gravity
	Frame *f = Frame::GetFrame(GetFrame());
	if (!f) return; // no external force if not in a frame
	Body *body = f->GetBody();
	if (body && !body->IsType(ObjectType::SPACESTATION)) { // they ought to have mass though...
		vector3d b1b2 = GetPosition();
		double m1m2 = GetMass() * body->GetMass();
		double invrsqr = 1.0 / b1b2.LengthSqr();
		double force = G * m1m2 * invrsqr;
		m_externalForce = -b1b2 * sqrt(invrsqr) * force;
	} else
		m_externalForce = vector3d(0.0);
	m_gravityForce = m_externalForce;

	// atmospheric drag
	if (body && f->IsRotFrame() && body->IsType(ObjectType::PLANET)) {
		vector3d fAtmoForce = CalcAtmosphericForce();

		// make this a bit less daft at high time accel
		// only allow atmosForce to increase by .1g per frame
		// TODO: clamp fAtmoForce instead.
		vector3d f1g = m_atmosForce + fAtmoForce.NormalizedSafe() * GetMass();
		if (fAtmoForce.LengthSqr() > f1g.LengthSqr())
			m_atmosForce = f1g;
		else
			m_atmosForce = fAtmoForce;

		m_externalForce += m_atmosForce;
	} else
		m_atmosForce = vector3d(0.0);

	// centrifugal and coriolis forces for rotating frames
	if (f->IsRotFrame()) {
		vector3d angRot(0, f->GetAngSpeed(), 0);
		m_externalForce -= m_mass * angRot.Cross(angRot.Cross(GetPosition())); // centrifugal
		m_externalForce -= 2 * m_mass * angRot.Cross(GetVelocity());		   // coriolis
	}
}

void DynamicBody::TimeStepUpdate(const float timeStep)
{
	m_oldPos = GetPosition();
	if (m_isMoving) {
		// In a non-rotating frame around a gravitating body the only external
		// force is that body's gravity (CalcExternalForce), and the motion it
		// causes is a two-body conic, which KeplerDrift follows exactly. The
		// plain step below drifts off the conic by an amount that grows with
		// the step - kilometres per orbit under time acceleration - while the
		// transfer planner predicts exact conics. So there the step is split:
		// every other force (thrust) kicks the velocity, then the body drifts
		// along the conic. Rotating frames (fictitious forces, atmosphere)
		// and station frames (no gravity) keep the plain step.
		const double gravityMu = GetFrameGravityMu();
		if (gravityMu > 0.0) {
			const vector3d nonGravity = m_force + (m_externalForce - m_gravityForce);
			vector3d pos = GetPosition();
			vector3d vel = m_vel + double(timeStep) * nonGravity * (1.0 / m_mass);
			if (KeplerDrift(pos, vel, gravityMu, double(timeStep))) {
				m_force += m_externalForce;
				m_vel = vel;
				UpdateAngularMotion(timeStep);
				SetPosition(pos);

				m_lastForce = m_force;
				m_lastTorque = m_torque;
				m_force = vector3d(0.0);
				m_torque = vector3d(0.0);
				CalcExternalForce(); // regenerate for new pos/vel
				ModelBody::TimeStepUpdate(timeStep);
				return;
			}
			// the solver gave up: take the plain step this time
		}

		m_force += m_externalForce;

		m_vel += double(timeStep) * m_force * (1.0 / m_mass);
		UpdateAngularMotion(timeStep);

		SetPosition(GetPosition() + m_vel * double(timeStep));

		//if (this->IsType(ObjectType::PLAYER))
		//Output("pos = %.1f,%.1f,%.1f, vel = %.1f,%.1f,%.1f, force = %.1f,%.1f,%.1f, external = %.1f,%.1f,%.1f\n",
		//	pos.x, pos.y, pos.z, m_vel.x, m_vel.y, m_vel.z, m_force.x, m_force.y, m_force.z,
		//	m_externalForce.x, m_externalForce.y, m_externalForce.z);

		m_lastForce = m_force;
		m_lastTorque = m_torque;
		m_force = vector3d(0.0);
		m_torque = vector3d(0.0);
		CalcExternalForce(); // regenerate for new pos/vel
	} else {
		m_oldAngDisplacement = vector3d(0.0);
	}

	ModelBody::TimeStepUpdate(timeStep);
}

double DynamicBody::GetFrameGravityMu() const
{
	// the same body CalcExternalForce takes gravity from, in a non-rotating frame
	const Frame *f = Frame::GetFrame(GetFrame());
	if (!f || f->IsRotFrame())
		return 0.0;
	const Body *body = f->GetBody();
	if (!body || body->IsType(ObjectType::SPACESTATION) || body->GetMass() <= 0.0)
		return 0.0;
	return G * body->GetMass();
}

void DynamicBody::UpdateAngularMotion(const float timeStep)
{
	m_angVel += double(timeStep) * m_torque * (1.0 / m_angInertia);

	double len = m_angVel.Length();
	if (len > 1e-16) {
		vector3d axis = m_angVel * (1.0 / len);
		matrix3x3d r = matrix3x3d::Rotate(len * timeStep, axis);
		SetOrient(r * GetOrient());
	}
	m_oldAngDisplacement = m_angVel * timeStep;
}

void DynamicBody::UpdateInterpTransform(double alpha)
{
	m_interpPos = alpha * GetPosition() + (1.0 - alpha) * m_oldPos;

	double len = m_oldAngDisplacement.Length() * (1.0 - alpha);
	if (len > 1e-16) {
		vector3d axis = m_oldAngDisplacement.Normalized();
		matrix3x3d rot = matrix3x3d::Rotate(-len, axis); // rotate backwards
		m_interpOrient = rot * GetOrient();
	} else
		m_interpOrient = GetOrient();
}

void DynamicBody::SetMassDistributionFromModel()
{
	CollMesh *m = GetCollMesh();
	// XXX totally arbitrarily pick to distribute mass over a half
	// bounding sphere area
	m_massRadius = m->GetRadius() * 0.5f;
	SetMass(m_mass);
}

vector3d DynamicBody::GetAngularMomentum() const
{
	return m_angInertia * m_angVel;
}

vector3d DynamicBody::GetVelocity() const
{
	return m_vel;
}

void DynamicBody::SetVelocity(const vector3d &v)
{
	m_vel = v;
}

vector3d DynamicBody::GetAngVelocity() const
{
	return m_angVel;
}

void DynamicBody::SetAngVelocity(const vector3d &v)
{
	m_angVel = v;
}

bool DynamicBody::OnCollision(Body *o, Uint32 flags, double relVel)
{
	// don't bother doing collision damage from a missile that will now explode, or may have already
	// also avoids an occasional race condition where destruction event of this could be queued twice
	// returning true to ensure that the missile can react to the collision
	if (o->IsType(ObjectType::MISSILE)) return true;

	double kineticEnergy = 0;
	if (o->IsType(ObjectType::DYNAMICBODY)) {
		kineticEnergy = KINETIC_ENERGY_MULT * static_cast<DynamicBody *>(o)->GetMass() * relVel * relVel;
	} else {
		kineticEnergy = KINETIC_ENERGY_MULT * m_mass * relVel * relVel;
	}

	// damage (kineticEnergy is being passed as a damage value) is measured in kilograms
	// ignore damage less than a gram except for cargo, which is very fragile.
	CollisionContact dummy;
	if (this->IsType(ObjectType::CARGOBODY)) {
		OnDamage(o, float(kineticEnergy), dummy);
	} else if (kineticEnergy > 1e-3) {
		OnDamage(o, float(kineticEnergy), dummy);
	}

	return true;
}

// return parameters for orbit of any body, gives both elliptic and hyperbolic trajectories
Orbit DynamicBody::ComputeOrbit() const
{
	auto f = Frame::GetFrame(GetFrame());
	// if we are in a rotating frame, then dynamic body currently under the
	// influence of a rotational frame, therefore getting the orbital parameters
	// is not appropriate, return the orbit as a fixed point
	if (f->IsRotFrame()) return Orbit::ForStaticBody(GetPosition());
	FrameId nrFrameId = f->GetId();
	const Frame *nrFrame = Frame::GetFrame(nrFrameId);
	const double mass = nrFrame->GetSystemBody()->GetMass();

	// current velocity and position with respect to non-rotating frame
	const vector3d vel = GetVelocityRelTo(nrFrameId);
	const vector3d pos = GetPositionRelTo(nrFrameId);

	return Orbit::FromBodyState(pos, vel, mass);
}
