// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _PLAYER_H
#define _PLAYER_H

#include "Ship.h"
#include "ShipCockpit.h"

class PlayerShipController;

namespace Graphics {
	class Renderer;
}

class Player : public Ship {
public:
	OBJDEF(Player, Ship, PLAYER);
	Player() = delete;
	Player(const Json &jsonObj, Space *space);
	Player(const ShipType::Id &shipId);

	void OnDocked(SpaceStation *, int) override;
	void OnUndocked(SpaceStation *, int) override;

	virtual bool DoDamage(float kgDamage) override final; // overloaded to add "crush" audio
	virtual bool OnDamage(Body *attacker, float kgDamage, const CollisionContact &contactData) override;
	virtual bool SetWheelState(bool down) override; // returns success of state change, NOT state itself
	virtual Missile *SpawnMissile(const MissileDef &, Body *) override;
	virtual void SetAlertState(Ship::AlertState as) override;
	virtual void NotifyRemoved(const Body *const removedBody) override;
	virtual bool ManualDocking() const override { return !AIIsActive(); }

	void DoFixspeedTakeoff(SpaceStation *from = nullptr);

	virtual void SetShipType(const ShipType::Id &shipId) override;

	PlayerShipController *GetPlayerController() const;
	//XXX temporary things to avoid causing too many changes right now
	Body *GetCombatTarget() const;
	Body *GetNavTarget() const;
	Body *GetFollowTarget() const;
	void SetCombatTarget(Body *const target, bool setFollowTo = false);
	void SetNavTarget(Body *const target);
	void SetFollowTarget(Body *const target);
	void ChangeCruiseSpeed(double delta);

	virtual Ship::HyperjumpStatus InitiateHyperjumpTo(const SystemPath &dest, int warmup_time, double duration, const HyperdriveSoundsTable &sounds, LuaRef checks) override;
	virtual void AbortHyperjump() override;

	// XXX cockpit is here for now because it has a physics component
	void InitCockpit();
	ShipCockpit *GetCockpit() const { return m_cockpit.get(); }
	void OnCockpitActivated();

	virtual void StaticUpdate(const float timeStep) override;
	virtual void TimeStepUpdate(const float timeStep) override;
	virtual vector3d GetManeuverVelocity() const;
	virtual int GetManeuverTime() const;
	// per-burn variants; the no-argument forms above resolve the active burn
	int GetManeuverTime(int idx) const;
	vector3d GetManeuverVelocity(int idx) const;
	// index of the burn the planner is currently working towards, -1 when the
	// plan is empty or every planned burn has been completed
	int GetActiveBurnIndex() const;

	// plan-burn engine control enable: a thin forwarder to the player's ship
	// controller, orthogonal to the flight control state
	void SetPlanBurnEnabled(bool enabled);
	bool GetPlanBurnEnabled() const;
	// how close Auto Burn brings the orbit to the plan, as a fraction of the
	// planned periapsis / apoapsis: a forwarder to the ship controller
	void SetPlanBurnAccuracy(double fraction);
	double GetPlanBurnAccuracy() const;
	// effective Auto Orientation direction lock: normalized, in the non-rotating
	// planner frame; zero when no valid lock is active. Read-only forwarder
	vector3d GetLockedManeuverDirection() const;

protected:
	virtual void SaveToJson(Json &jsonObj, Space *space) override;

	virtual void OnEnterSystem() override;
	virtual void OnBeforeEnterHyperspace() override;
	virtual void OnEnterHyperspace() override;

private:
	std::unique_ptr<ShipCockpit> m_cockpit;
	Sound::Event m_creakSound;
	vector3d m_atmosAccel;
	vector3d m_atmosJerk;
};

#endif /* _PLAYER_H */
