// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _SPACESTATION_H
#define _SPACESTATION_H

#include "ModelBody.h"
#include "Quaternion.h"
#include "SpaceStationType.h"
#include "sound/Sound.h"

#include <deque>
#define MAX_DOCKING_PORTS 240 //256-(0x10), 0x10 is used because the collision surfaces use it as an identifying flag
#define DOCKING_CLEARANCE_TIMEOUT (10.0 * 60.0)  // (10 min)

class Body;
class Camera;
class CityOnPlanet;
class Frame;
class NavLights;
class Ship;
class Space;
class SystemBody;

namespace Graphics {
	class Renderer;
}

namespace SceneGraph {
	class Animation;
	class MatrixTransform;
}

class SpaceStation : public ModelBody {
public:
	OBJDEF(SpaceStation, ModelBody, SPACESTATION);
	static void Init();

	enum class DockingRefusedReason { // <enum scope='SpaceStation::DockingRefusedReason' name='DockingRefusedReason' public>
		ClearanceAlreadyGranted,
		TooFarFromStation,
		NoBaysAvailable
	};

	SpaceStation() = delete;
	// Should point to SystemBody in Pi::currentSystem
	SpaceStation(const SystemBody *);
	SpaceStation(const Json &jsonObj, Space *space);

	virtual ~SpaceStation();
	vector3d GetAngVelocity() const override { return vector3d(0, m_type->AngVel(), 0); }
	bool OnCollision(Body *b, Uint32 flags, double relVel) override;
	bool DoShipDamage(Ship *s, Uint32 flags, double relVel);
	void Render(Graphics::Renderer *r, const Camera *camera, const vector3d &viewCoords, const matrix4x4d &viewTransform) override;
	void RenderShadowCaster(Graphics::Renderer *r, const matrix4x4d &frameToLight, const Aabb &lightBox) override;
	void StaticUpdate(const float timeStep) override;
	void TimeStepUpdate(const float timeStep) override;

	const SystemBody *GetSystemBody() const override { return m_sbody; }
	void PostLoadFixup(Space *space) override;
	void NotifyRemoved(const Body *const removedBody) override;

	void SetLabel(const std::string &label) override;

	// should call Ship::Undock and Ship::SetDockedWith instead
	// Returns true on success, false if permission denied
	bool LaunchShip(Ship *ship, const int port);
	void SetDocked(Ship *ship, const int port);
	void SwapDockedShipsPort(const int oldPort, const int newPort);

	int GetNearbyTraffic(double radius);

	bool GetDockingClearance(Ship *s);
	int GetDockingPortCount() const { return m_type->NumDockingPorts(); }
	int GetFreeDockingPort(const Ship *s) const; // returns -1 if none free
	int GetMyDockingPort(const Ship *s) const;
	int NumShipsDocked() const;
	// the ship docked, docking or leaving at a bay (from 0), or nullptr
	Ship *GetShipInBay(int bay) const { return bay >= 0 && bay < int(m_shipDocking.size()) ? m_shipDocking[bay].ship : nullptr; }

	const SpaceStationType *GetStationType() const { return m_type; }
	bool IsGroundStation() const;
	// the city around a ground station, once it has been close enough to be drawn
	const CityOnPlanet *GetAdjacentCity() const { return m_adjacentCity; }
	CityOnPlanet *GetAdjacentCity() { return m_adjacentCity; }

	bool AllocateStaticSlot(int &slot);

	// use docking bay position, if player has been granted permission
	vector3d GetTargetIndicatorPosition() const override;

	// need this now because stations rotate in their frame
	void UpdateInterpTransform(double alpha) override;

	// Return true if any lightning changes were applied, only do so if body is inside
	// or near enough to the station
	bool CalcInteriorLighting(const Body *b, Color4ub& sLight, double& sIntensity) const;

	// A station with a turning floor under one shaft (SpaceStationType::Revolver):
	// the ships take turns. Whether it is the ship's turn (its spot is under the
	// shaft: come over the shaft and lower the gear, and the lid opens); whether
	// the lid is open for it, to come down now; and where (in the station's
	// frame) a ship with clearance waits for its turn.
	bool IsRevolver() const { return m_type->IsRevolver(); }
	// a bay's docked transform in station space, where it is now (a
	// revolver station's spots turn with its floor)
	matrix4x4d GetBayLocalTransform(int bay) const;
	bool IsShaftReadyFor(const Ship *ship) const;
	bool IsShaftOpenFor(const Ship *ship) const;
	vector3d GetRevolverHoldPosition(int bay) const;
	// what the floor and the lid are doing, in words (log, debug window); empty if not a revolver
	std::string GetRevolverStatus() const;


protected:
	void SaveToJson(Json &jsonObj, Space *space) override;

private:
	void DockingUpdate(const double timeStep);
	void PositionDockingShip(Ship *ship, int port) const;
	void PositionDockedShip(Ship *ship, int port) const;
	bool LevelShip(Ship *ship, int port, const float timeStep);
	void DoLawAndOrder(const double timeStep);
	bool IsPortLocked(const int bay) const;
	void LockPort(const int bay, const bool lockIt);
	double GetDockAnimStageDuration(const int bay, const DockStage stage) const;
	double GetUndockAnimStageDuration(const int bay, const DockStage stage) const;

	struct shipDocking_t {
		shipDocking_t() :
			ship(0),
			shipIndex(0),
			stage(DockStage::NONE),
			stagePos(0),
			fromPos(0.0),
			fromRot(1.0, 0.0, 0.0, 0.0),
			maxOffset(0)
		{}

		Ship *ship;
		int shipIndex; // deserialisation
		DockStage stage;
		double stagePos;  // 0 -> 1.0
		vector3d fromPos; // in station model coords
		Quaterniond fromRot;
		double maxOffset;
	};
	typedef std::vector<shipDocking_t>::const_iterator constShipDockingIter;
	typedef std::vector<shipDocking_t>::iterator shipDockingIter;
	std::vector<shipDocking_t> m_shipDocking;

	SpaceStationType::TPorts m_ports;

	double m_oldAngDisplacement;

	void SwitchToStage(Uint32 bay, DockStage stage);
	matrix4x4d GetBayTransform(Uint32 bay) const;

	void InitStation();
	const SpaceStationType *m_type;
	const SystemBody *m_sbody;
	CityOnPlanet *m_adjacentCity;
	enum { NUM_STATIC_SLOTS = 4 };
	bool m_staticSlot[NUM_STATIC_SLOTS];

	SceneGraph::Animation *m_doorAnimation;
	double m_doorAnimationStep;
	double m_doorAnimationState;

	// the turning floor and its lid (IsRevolver): one job at a time, in turn
	enum class RevolverPhase {
		IDLE,	 // lid closed, nothing to do
		TURNING, // the job's spot is turning under the shaft
		OPENING, // the lid opens
		OPEN,	 // the ship comes down, or is lifted out
		CLOSING, // the lid closes
		WAITING	 // the spot is under the shaft: waiting for the arriving ship over it, gear down
				 // (last, so the numbers of the others in saved games stay the same)
	};
	// whether the ship of an arriving job hovers over the shaft with its gear down
	bool RevolverShipOverShaft() const;
	struct RevolverJob {
		int bay = -1;
		bool leaving = false;
	};
	RevolverPhase m_revPhase = RevolverPhase::IDLE;
	RevolverPhase m_revLoggedPhase = RevolverPhase::IDLE;
	RevolverJob m_revJob;
	std::deque<RevolverJob> m_revQueue;
	double m_floorAngle = 0.0; // radians, the floor's turn from the model's pose
	SceneGraph::Animation *m_turnAnimation = nullptr;
	// the spots' pad lights (navlight_pad*): the model keeps every navlight
	// where it was made (Loader::CreateNavlight), so they are turned with the
	// floor here; with their place at rest
	std::vector<std::pair<SceneGraph::MatrixTransform *, matrix4x4f>> m_padLights;
	double m_padLightsAngle = -1.0;

	// the lid's and the floor's sounds (data/sounds/Station/RevolverLid,
	// RevolverTurn), heard from the base while they move
	Sound::Event m_lidSound;
	Sound::Event m_turnSound;
	bool m_lidSoundOn = false;
	bool m_turnSoundOn = false;
	// startFx / stopFx: played once as the movement starts / stops (RevolverTurnStart:
	// the floor's motor engaging, RevolverTurnStop: winding down)
	void RevolverSound(Sound::Event &sound, bool &on, const char *fx, bool moving,
		const char *startFx = nullptr, const char *stopFx = nullptr);
	bool m_revGeomsMoved = false; // the lid moved this step: its collision follows

	void RevolverUpdate(const double timeStep);
	void RevolverEnqueue(int bay, bool leaving);
	bool RevolverJobValid(const RevolverJob &job) const;
	void RevolverSave(Json &spaceStationObj) const;
	void RevolverLoad(const Json &spaceStationObj);

	std::unique_ptr<NavLights> m_navLights;
};

#endif /* _SPACESTATION_H */
