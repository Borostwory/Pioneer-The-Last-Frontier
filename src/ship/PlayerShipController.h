// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#pragma once

#include "Input.h"
#include "ShipController.h"
#include "ConnectionTicket.h"

#include "vector3.h"
#include "matrix3x3.h"

#include <cstdint>

// autopilot AI + input
class PlayerShipController : public ShipController {
public:
	PlayerShipController();
	~PlayerShipController();

	Type GetType() override { return PLAYER; }
	void SaveToJson(Json &jsonObj, Space *s) override;
	void LoadFromJson(const Json &jsonObj) override;
	void PostLoadFixup(Space *s) override;
	void StaticUpdate(float timeStep) override;
	bool IsMouseActive() const { return m_mouseActive; }
	void SetDisableMouseFacing(bool disabled) { m_disableMouseFacing = disabled; }
	double GetCruiseSpeed() const override { return m_cruiseSpeed; }
	void ChangeCruiseSpeed(double delta) override { m_cruiseSpeed += delta; }
	FlightControlState GetFlightControlState() const override { return m_flightControlState; }
	vector3d GetMouseDir() const; // in local frame

	// Return the current mouse direction in camera space.
	vector3d GetMouseViewDir() const;
	void SetMouseForRearView(bool enable) { m_invertMouse = enable; }
	void SetFlightControlState(FlightControlState s) override;
	// plan-burn engine control: an independent, orthogonal enabled-flag, applied
	// on top of whatever orientation control (m_flightControlState) is active.
	// The FlightControlState machine is exclusive; this flag is not part of it.
	void SetPlanBurnEnabled(bool enabled);
	bool GetPlanBurnEnabled() const { return m_planBurnEnabled; }
	// the effective Auto Orientation direction lock: the normalized direction of
	// the active planned burn in the non-rotating planner frame, cached when
	// CONTROL_PLAN_HEADING acquired it. Zero when plan heading is not active, or
	// the active burn index or planner lock serial no longer match the cache.
	vector3d GetLockedManeuverDirection() const;
	// how close Auto Burn brings the orbit to the plan before a burn counts as
	// done: the largest error in periapsis / apoapsis, as a fraction of them
	// (and of the speed, out of the planned plane). Clamped to [0.001, 0.2]
	void SetPlanBurnAccuracy(double fraction);
	double GetPlanBurnAccuracy() const { return m_planBurnAccuracy; }
	float GetLowThrustPower() const { return m_lowThrustPower; }
	void SetLowThrustPower(float power);

	bool GetRotationDamping() const { return m_rotationDamping; }
	void SetRotationDamping(bool enabled);
	void ToggleRotationDamping();
	void FireMissile();
	void ToggleCruise();
	void SelectTarget();
	void CycleHostiles();

	//targeting
	//XXX AI should utilize one or more of these
	Body *GetCombatTarget() const;
	Body *GetNavTarget() const;
	Body *GetFollowTarget() const override;
	void SetCombatTarget(Body *const target, bool setFollowTo = false);
	void SetNavTarget(Body *const target);
	void SetFollowTarget(Body *const target);

	sigc::signal<void> onRotationDampingChanged;
	sigc::signal<void, Body *> onChangeTarget;
	sigc::signal<void> onChangeFlightControlState;

	enum CruiseDirection { // <enum scope='PlayerShipController' name=CruiseDirection public>
		CRUISE_FWD,
		CRUISE_UP
	};

	enum FollowMode { // <enum scope='PlayerShipController' name=FollowMode public>
		FOLLOW_POS,
		FOLLOW_ORI
	};

	static constexpr double maxFollowDistance[] = {
		1000000.0, // FOLLOW_POS
		10000.0    // FOLLOW_ORI
	};

	static constexpr double maxFollowDistanceSqr[] = {
		maxFollowDistance[0] * maxFollowDistance[0],
		maxFollowDistance[1] * maxFollowDistance[1]
	};

	void SetCruiseDirection(CruiseDirection mode);
	CruiseDirection GetCruiseDirection() const { return m_cruiseDirection; }
	void SetFollowMode(FollowMode mode) { m_followMode = mode; }
	FollowMode GetFollowMode() const { return m_followMode; }
	void SetSpeedLimit(double limit) { m_speedLimit = limit; }
	double GetSpeedLimit() { return m_speedLimit; }
	void SetSpeedLimiterActive(bool active) { m_speedLimiterActive = active; }
	bool IsSpeedLimiterActive() const { return m_speedLimiterActive; }
	bool IsShipDrifting(); // 'setted' speed is very different from real speed

private:
	struct InputBinding : public Input::InputFrame {
		using InputFrame::InputFrame;

		// Weapons
		Action *targetObject;
		Action *cycleHostiles;
		Action *primaryFire;
		Action *secondaryFire;

		// Flight
		Axis *pitch;
		Axis *yaw;
		Axis *roll;
		Action *killRot;
		Action *toggleRotationDamping;

		// Manual Control
		Axis *thrustForward;
		Axis *thrustUp;
		Axis *thrustLeft;
		Action *thrustLowPower;

		// Speed Control
		Axis *speedControl;
		Action *toggleCruise;
		Action *toggleSpeedLimiter;

		// Landing Controls
		Action *toggleLandingGear;
		Axis *controlLandingGear;

		void RegisterBindings() override;
	} InputBindings;

	// cumulative action from the input and flight assistance
	struct TotalDesiredAction;
	// static functions with access to private members
	struct Util;

	// Poll controls, set gun states
	void PollControls(float timeStep, int *mouseMotion, TotalDesiredAction &outParams);
	// desired speed of linear and rotary movement, calculated by flight control assistance
	void FlightAssist(const float timeStep, TotalDesiredAction &outParams);
	// send a control request to propulsion
	void ApplyTotalAction(const TotalDesiredAction &params);
	// shared by the plan-heading orientation control and the plan-burn engine
	// control: the unit direction of the active planned burn in the ship's
	// current frame, which the attitude helpers take as input. returns the burn
	// index, or -1 when there is no burn to point at, in which case the flight
	// control state is released to manual. The direction comes from the
	// Auto Orientation lock, acquired once per active burn and steered along the
	// closed-loop residual once the burn is under way
	int ResolvePlanBurnDirection(vector3d &outWorldDir);
	// aims the Auto Orientation lock along what is still to gain, once Auto
	// Burn has ignited the burn; holds it near the end of the burn
	void SteerPlanBurnLock();
	// the ship's position and velocity relative to the planner frame (the
	// non-rotating frame of the body it orbits)
	bool GetPlannerState(vector3d &pos, vector3d &vel) const;
	// keep m_planBurnTarget on burn idx: follows the plan until ignition, then
	// holds it. False when the burn has no target orbit
	bool SyncPlanBurnTarget(int idx);
	// the velocity still to gain to be on the target orbit, planner frame
	vector3d PlanBurnVelocityToGain(const vector3d &pos, const vector3d &vel) const;
	// the ship's orbit matches the target within m_planBurnAccuracy
	bool IsPlanBurnOnTarget(const vector3d &pos, const vector3d &vel, const vector3d &toGain) const;
	// transform a planner-frame (non-rotating system frame) direction into the
	// ship's current frame; recomputed every call, never cached
	vector3d PlannerToShipFrame(const vector3d &plannerDir) const;
	// Auto Orientation direction lock lifecycle (derived runtime state only)
	void InvalidatePlanBurnDirectionLock();
	// acquire the lock for burn idx from the planner's offset velocity; returns
	// false (lock left invalid) when the burn has no direction
	bool AcquirePlanBurnDirectionLock(int idx);
	// keep the lock in step with the planner's active burn: re-acquire when the
	// active index or planner lock serial changed. Returns the active burn index
	// with a valid lock, or -1 when there is no direction to point at
	int SyncPlanBurnDirectionLock();
	// true when the cache is valid for the planner's current active burn/serial
	bool IsPlanBurnDirectionLockCurrent() const;
	// the plan-burn engine control law, run from StaticUpdate after the
	// orientation switch: fires the main engine for the active planned burn
	// while the ship is aligned with the burn direction, on top of whatever
	// orientation control is active. never touches m_flightControlState.
	void UpdatePlanBurnThrust();

	void OnToggleLandingGear();
	void UpdateLandingGear();

	// FIXME: separate the propusion controller from the input system, pass in wanted velocity correction directly.
	friend class Propulsion;

	double m_speedLimit = 0.0;
	bool m_speedLimiterActive = false;
	bool IsAnyAngularThrusterKeyDown();
	bool IsAnyLinearThrusterKeyDown();
	//do a variety of checks to see if input is allowed
	bool AreControlsLocked();
	Body *m_combatTarget;
	Body *m_navTarget;
	Body *m_followTarget;
	bool m_invertMouse; // used for rear view, *not* for invert Y-axis option (which is Pi::input->IsMouseYInvert)
	bool m_mouseActive;
	bool m_disableMouseFacing;
	bool m_rotationDamping;
	bool m_stickySpeedKey = false; // helps cruise speed sticks to 0 when it crosses it
	matrix3x3d m_followTargetPrevOrient;
	vector3d m_followTargetPrevVel;
	double m_mouseX;
	double m_mouseY;
	double m_cruiseSpeed;
	FlightControlState m_flightControlState;
	// plan-burn engine control enable: orthogonal to m_flightControlState
	bool m_planBurnEnabled = false;
	// Auto Orientation direction lock: derived runtime state, never serialized.
	// unit direction in the non-rotating planner frame, not the ship frame
	vector3d m_lockedPlanBurnDir = vector3d(0.0, 0.0, 0.0);
	int m_lockedPlanBurnIdx = -1;
	uint32_t m_lockedPlanBurnSerial = 0;
	bool m_lockedPlanBurnValid = false;
	// closed-loop guidance of the active planned burn: the orbit the plan
	// expects after it (planner frame), held from ignition. Derived runtime
	// state, never serialized
	struct PlanBurnTarget {
		bool valid = false;
		int idx = -1;
		uint32_t serial = 0;
		double mu = 0.0;							   // of the central body
		double bodyRadius = 0.0;					   // of the central body
		vector3d normal = vector3d(0.0);			   // unit angular momentum
		vector3d periapsisDir = vector3d(0.0);		   // unit; zero for a circle
		double a = 0.0;								   // semi-major axis, negative on escape
		double p = 0.0;								   // semi-latus rectum
		double rp = 0.0;							   // periapsis radius
		double ra = 0.0;							   // apoapsis radius, 0 on escape
		double plannedDv = 0.0;
		bool ignited = false;
		double ignitionTime = 0.0;
	};
	PlanBurnTarget m_planBurnTarget;
	// how close to the planned periapsis / apoapsis a burn must end, as a fraction
	double m_planBurnAccuracy = 0.01;
	float m_fovY; //for mouse acceleration adjustment
	float m_joystickDeadzone;
	float m_lowThrustPower;
	float m_aimingSens;
	int m_combatTargetIndex; //for PostLoadFixUp
	int m_navTargetIndex;
	int m_followTargetIndex;
	vector3d m_mouseDir;

	FollowMode m_followMode = FOLLOW_POS;
	CruiseDirection m_cruiseDirection = CRUISE_FWD;

	ConnectionTicket m_connRotationDampingToggleKey;
	ConnectionTicket m_fireMissileKey;
	ConnectionTicket m_toggleCruise;
	ConnectionTicket m_toggleSpeedLimiter;
	ConnectionTicket m_selectTarget;
	ConnectionTicket m_cycleHostiles;
	ConnectionTicket m_toggleLandingGear;
};
