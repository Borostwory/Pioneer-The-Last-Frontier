// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Player.h"

#include "Frame.h"
#include "Game.h"
#include "GameConfig.h"
#include "GameLog.h"
#include "HyperspaceCloud.h"
#include "Lang.h"
#include "Pi.h"
#include "SectorView.h"
#include "Sfx.h"
#include "SpaceStation.h"
#include "StringF.h"
#include "SystemView.h" // for the transfer planner
#include "TransferPlanner.h"
#include "WorldView.h"
#include "lua/LuaEvent.h"
#include "lua/LuaObject.h"
#include "pigui/LuaPiGui.h"
#include "ship/PlayerShipController.h"
#include "ship/ShipController.h"
#include "sound/Sound.h"

//Some player specific sounds
static Sound::Event s_soundUndercarriage;
static Sound::Event s_soundHyperdrive;

Player::Player(const ShipType::Id &shipId) :
	Ship(shipId)
{
	SetController(new PlayerShipController());
	InitCockpit();
	m_atmosAccel = vector3d(0.0f, 0.0f, 0.0f);

	static_cast<PlayerShipController *>(m_controller)->onChangeTarget.connect([=](Body *target) {
		LuaEvent::Queue(PiGui::GetEventQueue(), "onPlayerTargetChanged", target);
	});
}

Player::Player(const Json &jsonObj, Space *space) :
	Ship(jsonObj, space)
{
	InitCockpit();

	static_cast<PlayerShipController *>(m_controller)->onChangeTarget.connect([=](Body *target) {
		LuaEvent::Queue(PiGui::GetEventQueue(), "onPlayerTargetChanged", target);
	});
}

void Player::SetShipType(const ShipType::Id &shipId)
{
	Ship::SetShipType(shipId);
	InitCockpit();
}

void Player::SaveToJson(Json &jsonObj, Space *space)
{
	Ship::SaveToJson(jsonObj, space);
}

void Player::InitCockpit()
{
	delete m_cockpit.release();
	if (!Pi::config->Int("EnableCockpit"))
		return;

	// XXX select a cockpit model. this is all quite skanky because we want a
	// fallback if the name is not found, which means having to actually try to
	// load the model. but ModelBody (on which ShipCockpit is currently based)
	// requires a model name, not a model object. it won't hurt much because it
	// all stays in the model cache anyway, its just awkward. the fix is to fix
	// ShipCockpit so its not a ModelBody and thus does its model work
	// directly, but we're not there yet
	std::string cockpitModelName;
	if (!GetShipType()->cockpitName.empty()) {
		if (Pi::FindModel(GetShipType()->cockpitName, false))
			cockpitModelName = GetShipType()->cockpitName;
	}
	if (cockpitModelName.empty()) {
		if (Pi::FindModel("default_cockpit", false))
			cockpitModelName = "default_cockpit";
	}
	if (!cockpitModelName.empty())
		m_cockpit.reset(new ShipCockpit(cockpitModelName, this));

	OnCockpitActivated();
}

bool Player::DoDamage(float kgDamage)
{
	bool r = Ship::DoDamage(kgDamage);

	// Don't fire audio on EVERY iteration (aka every 16ms, or 60fps), only when exceeds a value randomly
	const float dam = kgDamage * 0.01f;
	if (Pi::rng.Double() < dam && m_lastDamageSound > k_damageSoundInterval) {
		if (!IsDead() && (GetPercentHull() < 25.0f)) {
			Sound::BodyMakeNoise(this, "warning", .5f);
		}
		if (dam < (0.01 * float(GetShipType()->hullMass)))
			Sound::BodyMakeNoise(this, "Hull_hit_Small", 1.0f);
		else
			Sound::BodyMakeNoise(this, "Hull_Hit_Medium", 1.0f);

		m_lastDamageSound = 0.f;
	}
	return r;
}

//XXX perhaps remove this, the sound is very annoying
bool Player::OnDamage(Body *attacker, float kgDamage, const CollisionContact &contactData)
{
	bool r = Ship::OnDamage(attacker, kgDamage, contactData);
	if (!IsDead() && (GetPercentHull() < 25.0f)) {
		Sound::BodyMakeNoise(this, "warning", .5f);
	}
	return r;
}

void Player::OnDocked(SpaceStation *s, int port)
{
	Ship::OnDocked(s, port);

	LuaEvent::Queue("onPlayerDocked", this, s, port);
}

void Player::OnUndocked(SpaceStation *s, int port)
{
	Ship::OnUndocked(s, port);

	LuaEvent::Queue("onPlayerUndocked", this, s, port);
}

//XXX all ships should make this sound
bool Player::SetWheelState(bool down)
{
	bool did = Ship::SetWheelState(down);
	if (did) {
		s_soundUndercarriage.Play(down ? "UC_out" : "UC_in", 1.0f, 1.0f, 0);
		GetPlayerController()->SetCruiseDirection(down ? PlayerShipController::CRUISE_UP : PlayerShipController::CRUISE_FWD);
	}
	return did;
}

//XXX all ships should make this sound
Missile *Player::SpawnMissile(const MissileDef &def, Body *target)
{
	Missile *m = Ship::SpawnMissile(def, target);
	if (m)
		Sound::PlaySfx("Missile_launch", 1.0f, 1.0f, 0);
	return m;
}

//XXX do in lua, or use the alert concept for all ships
void Player::SetAlertState(Ship::AlertState as)
{
	Ship::AlertState prev = GetAlertState();

	switch (as) {
	case ALERT_NONE:
		if (prev != ALERT_NONE)
			Pi::game->log->Add(Lang::ALERT_CANCELLED);
		break;

	case ALERT_SHIP_NEARBY:
		if (prev == ALERT_NONE)
			Pi::game->log->Add(Lang::SHIP_DETECTED_NEARBY);
		else
			Pi::game->log->Add(Lang::DOWNGRADING_ALERT_STATUS);
		Sound::PlaySfx("OK");
		break;

	case ALERT_SHIP_FIRING:
		Pi::game->log->Add(Lang::LASER_FIRE_DETECTED);
		Sound::PlaySfx("warning", 0.2f, 0.2f, 0);
		break;

	case ALERT_MISSILE_DETECTED:
		Pi::game->log->Add(Lang::MISSILE_DETECTED);
		Sound::PlaySfx("warning", 0.2f, 0.2f, 0);
		break;
	}

	Ship::SetAlertState(as);
}

void Player::NotifyRemoved(const Body *const removedBody)
{
	if (GetNavTarget() == removedBody)
		SetNavTarget(0);

	if (GetCombatTarget() == removedBody) {
		SetCombatTarget(0);

		if (!GetNavTarget() && removedBody->IsType(ObjectType::SHIP))
			SetNavTarget(static_cast<const Ship *>(removedBody)->GetHyperspaceCloud());
	}

	if (GetFollowTarget() == removedBody) {
		SetFollowTarget(0);
		// turn on manual mode, otherwise the ship will start to drop speed to 0
		// relative to the frame
		if (m_controller->GetFlightControlState() == CONTROL_FIXSPEED) {
			m_controller->SetFlightControlState(CONTROL_MANUAL);
		}
	}

	Ship::NotifyRemoved(removedBody);
}

void Player::OnBeforeEnterHyperspace()
{
	Ship::OnBeforeEnterHyperspace();

	LuaEvent::Queue("onLeaveSystem", this);
}

//XXX ui stuff
void Player::OnEnterHyperspace()
{
	s_soundHyperdrive.Play(m_hyperspace.sounds.jump_sound.c_str());
	SetNavTarget(0);
	SetCombatTarget(0);
	SetFollowTarget(0);

	m_controller->SetFlightControlState(CONTROL_MANUAL); //could set CONTROL_HYPERDRIVE
	ClearThrusterState();
	Pi::game->WantHyperspace();
}

void Player::OnEnterSystem()
{
	m_controller->SetFlightControlState(CONTROL_MANUAL);
}

//temporary targeting stuff
PlayerShipController *Player::GetPlayerController() const
{
	return static_cast<PlayerShipController *>(GetController());
}

Body *Player::GetCombatTarget() const
{
	return static_cast<PlayerShipController *>(m_controller)->GetCombatTarget();
}

Body *Player::GetNavTarget() const
{
	return static_cast<PlayerShipController *>(m_controller)->GetNavTarget();
}

Body *Player::GetFollowTarget() const
{
	return static_cast<PlayerShipController *>(m_controller)->GetFollowTarget();
}

void Player::SetCombatTarget(Body *const target, bool setFollowTo)
{
	static_cast<PlayerShipController *>(m_controller)->SetCombatTarget(target, setFollowTo);
}

void Player::SetNavTarget(Body *const target)
{
	static_cast<PlayerShipController *>(m_controller)->SetNavTarget(target);
}

void Player::SetFollowTarget(Body *const target)
{
	static_cast<PlayerShipController *>(m_controller)->SetFollowTarget(target);
}

void Player::ChangeCruiseSpeed(double delta)
{
	static_cast<PlayerShipController *>(m_controller)->ChangeCruiseSpeed(delta);
}

//temporary targeting stuff ends

Ship::HyperjumpStatus Player::InitiateHyperjumpTo(const SystemPath &dest, int warmup_time, double duration, const HyperdriveSoundsTable &sounds, LuaRef checks)
{
	HyperjumpStatus status = Ship::InitiateHyperjumpTo(dest, warmup_time, duration, sounds, checks);

	if (status == HYPERJUMP_OK)
		s_soundHyperdrive.Play(m_hyperspace.sounds.warmup_sound.c_str());

	return status;
}

void Player::AbortHyperjump()
{
	s_soundHyperdrive.Play(m_hyperspace.sounds.abort_sound.c_str());
	Ship::AbortHyperjump();
}

void Player::OnCockpitActivated()
{
	if (m_cockpit)
		m_cockpit->OnActivated(this);
}

void Player::StaticUpdate(const float timeStep)
{
	Ship::StaticUpdate(timeStep);

	// now insert the latest value
	vector3d current_atmosAccel = GetAtmosForce() * (1.0 / GetMass());
	m_atmosJerk = (current_atmosAccel - m_atmosAccel) * Pi::game->GetInvTimeAccelRate();
	bool playCreak = false;

	// play creaking sfx if the acceleration along the Y axis (up/down directions) is higher than 1g
	// and the rate of change of acceleration is more than 2.5 m s-3
	if ((abs(m_atmosAccel.Dot(Player::m_interpOrient.VectorY()))) > 10 && abs(m_atmosJerk.Dot(Player::m_interpOrient.VectorY())) > 2.5) {
		playCreak = true;
	}

	if (playCreak) {
		if (!m_creakSound.IsPlaying()) {
			float creakVol = fmax(0.f, fmin(float((m_atmosJerk.Length() - 50) * 0.05f), 0.3f));
			m_creakSound.Play("metal_creaking", creakVol, creakVol, Sound::OP_REPEAT);
			m_creakSound.VolumeAnimate(creakVol, creakVol, 0.3f, 0.3f);
		}
	} else if (m_creakSound.IsPlaying()) {
		m_creakSound.FadeOut(1.5f);
	}
	m_atmosAccel = current_atmosAccel;

	// XXX even when not on screen. hacky, but really cockpit shouldn't be here
	// anyway so this will do for now
	if (m_cockpit)
		m_cockpit->Update(this, timeStep);
}

void Player::TimeStepUpdate(const float timeStep)
{
	Ship::TimeStepUpdate(timeStep);

	// executed delta-v accounting: project the thrust the ship actually applied
	// this tick (resolved by Ship::TimeStepUpdate above) onto the planned
	// direction of the active burn, and let the planner accumulate it
	if (!Pi::planner->HasPlan())
		return;
	// Auto Burn flies the burn closed-loop and sets the remainder itself
	if (GetPlanBurnEnabled())
		return;

	const int activeIdx = Pi::planner->GetActiveIndex();
	if (activeIdx < 0) // every planned burn is complete: nothing to accumulate into
		return;

	const double mass = GetMass();
	if (mass <= 0.0)
		return;

	// the plan's offset velocity lives in the burn's non-rotating system frame,
	// while GetActualLinThrust() is in ship axes
	const vector3d planDv = Pi::planner->GetOffsetVel(activeIdx);
	if (planDv.LengthSqr() < 1e-18) // an inert plan has no direction to count progress along
		return;

	matrix3x3d rot = Frame::GetFrame(Frame::GetFrame(GetFrame())->GetNonRotFrame())->GetOrientRelTo(GetFrame());
	const vector3d planDir = (rot * planDv).NormalizedSafe() * GetOrient();

	const vector3d accel = GetPropulsion()->GetActualLinThrust() * (1.0 / mass);
	Pi::planner->AddBurnProgress(activeIdx, accel.Dot(planDir) * Pi::game->GetTimeStep());
}

int Player::GetManeuverTime() const
{
	// burns are stored strictly ordered by startTime, so the active burn is the
	// earliest one that has not been completed; -1 (no plan, or every planned
	// burn complete) keeps reading as "no plan"
	return GetManeuverTime(GetActiveBurnIndex());
}

int Player::GetManeuverTime(int idx) const
{
	if (idx < 0) { // no burn to target, which reads as "no plan" here
		return 0;
	}
	return Pi::planner->GetStartTime(idx);
}

vector3d Player::GetManeuverVelocity() const
{
	return GetManeuverVelocity(GetActiveBurnIndex());
}

vector3d Player::GetManeuverVelocity(int idx) const
{
	Frame *frame = Frame::GetFrame(GetFrame());
	if (frame->IsRotFrame())
		frame = Frame::GetFrame(frame->GetNonRotFrame());
	const SystemBody *systemBody = frame->GetSystemBody();

	if (idx < 0) {
		return vector3d::Zero;
	} else if (systemBody) {
		Orbit playerOrbit = ComputeOrbit();
		if (!is_zero_exact(playerOrbit.GetSemiMajorAxis())) {
			double mass = systemBody->GetMass();
			// XXX The best solution would be to store the mass(es) on Orbit
			const vector3d velocity = (Pi::planner->GetVel(idx) - playerOrbit.OrbitalVelocityAtTime(mass, playerOrbit.OrbitalTimeAtPos(Pi::planner->GetPos(idx), mass)));
			return velocity;
		}
	}
	return vector3d::Zero;
}

int Player::GetActiveBurnIndex() const
{
	return Pi::planner->HasPlan() ? Pi::planner->GetActiveIndex() : -1;
}

// thin forwarders to the player's ship controller - no logic here; the plan-
// burn engine enable is orthogonal to the flight control state
void Player::SetPlanBurnEnabled(bool enabled)
{
	auto con = GetPlayerController();
	if (con) {
		con->SetPlanBurnEnabled(enabled);
	}
}

bool Player::GetPlanBurnEnabled() const
{
	auto con = GetPlayerController();
	return con ? con->GetPlanBurnEnabled() : false;
}

void Player::SetPlanBurnAccuracy(double fraction)
{
	auto con = GetPlayerController();
	if (con) {
		con->SetPlanBurnAccuracy(fraction);
	}
}

double Player::GetPlanBurnAccuracy() const
{
	auto con = GetPlayerController();
	return con ? con->GetPlanBurnAccuracy() : 0.0;
}

vector3d Player::GetLockedManeuverDirection() const
{
	auto con = GetPlayerController();
	return con ? con->GetLockedManeuverDirection() : vector3d(0.0, 0.0, 0.0);
}

void Player::DoFixspeedTakeoff(SpaceStation *from)
{
	auto con = GetPlayerController();
	con->SetCruiseDirection(PlayerShipController::CRUISE_UP);
	SetFlightState(Ship::FLYING);
	GetPlayerController()->SetFlightControlState(CONTROL_FIXSPEED);
	double curSpeed = con->GetCruiseSpeed();
	double wantSpeed = 2; // m/s
	con->ChangeCruiseSpeed(wantSpeed - curSpeed);

	// special preparations for launch in a rotating orbital
	if (from && !from->IsGroundStation()) {
		SetFollowTarget(from);
		con->SetFollowMode(PlayerShipController::FOLLOW_ORI);
		auto pPos = GetPosition();
		SetAngVelocity(from->GetAngVelocity());

		// some actions to avoid collision with the pad at start with a margin
		SetPosition(GetPosition() + GetOrient().VectorY() * 0.01);
		auto tangent = from->GetAngVelocity().Cross(pPos);
		auto radial = GetOrient().VectorY() * 0.05;
		SetVelocity(tangent + radial);
	}
}
