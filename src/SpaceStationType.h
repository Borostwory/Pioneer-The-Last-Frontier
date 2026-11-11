// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _SPACESTATIONTYPE_H
#define _SPACESTATIONTYPE_H

#include "Random.h"
#include "matrix4x4.h"
#include "vector3.h"

#include <map>
#include <vector>
#include <string>

//Space station definition, loaded from data/stations

class Ship;
class SystemBody;
namespace SceneGraph {
	class Model;
}

enum class DockStage { // <enum scope='DockStage' name=DockStage public>

	NONE,
	MANUAL,

	DOCK_STAGES_BEGIN,

	CLEARANCE_GRANTED,

	DOCK_ANIMATION_NONE,
	DOCK_ANIMATION_1,
	DOCK_ANIMATION_2,
	DOCK_ANIMATION_3,
	DOCK_ANIMATION_MAX,

	TOUCHDOWN,
	LEVELING,
	REPOSITION,
	JUST_DOCK,

	DOCK_STAGES_END,

	DOCKED,

	UNDOCK_STAGES_BEGIN,

	UNDOCK_BEGIN,

	UNDOCK_ANIMATION_NONE,
	UNDOCK_ANIMATION_1,
	UNDOCK_ANIMATION_2,
	UNDOCK_ANIMATION_3,
	UNDOCK_ANIMATION_MAX,

	UNDOCK_END,

	LEAVE,

	UNDOCK_STAGES_END,

	// used not in BayPath, but in Port
	APPROACH1,
	APPROACH2

};

class SpaceStationType {
public:
	typedef std::map<DockStage, matrix4x4f> TMapBayIDMat;
	struct BayPath {
		TMapBayIDMat m_docking;
		TMapBayIDMat m_leaving;
	};
	typedef std::map<Uint32, BayPath> BayPathMap;

	struct SPort {
		static const int BAD_PORT_ID = -1;
		SPort() :
			portId(BAD_PORT_ID),
			minShipSize(5000),
			maxShipSize(-1),
			inUse(false) {}
		int portId;
		int minShipSize, maxShipSize;
		bool inUse;
		std::vector<std::pair<int, std::string>> bayIDs;
		std::string name;
		TMapBayIDMat m_approach;
	};
	typedef std::vector<SPort> TPorts;

	struct positionOrient_t {
		vector3d pos;
		vector3d xaxis;
		vector3d yaxis;
		vector3d zaxis;
	};

	DockStage PivotStage(DockStage s) const;

	// A ground station whose landing spots sit on a turning floor under one
	// shaft, closed by a lid (the model's "doors" animation): "revolver" in the
	// station definition. One ship at a time uses the shaft: the floor turns
	// the ship's spot under it, the lid opens, the ship lands, or is lifted
	// up the shaft by the station when it leaves (SpaceStation's Revolver*).
	struct Revolver {
		bool enabled = false;
		float turnSpeed = 9.0f;		 // degrees per second
		float liftSpeed = 20.0f;	 // m/s, the average speed of a ship lifted out
		float liftHeight = 345.0f;	 // m from the pad up to where a lifted ship is let go
		float lidTime = 3.3f;		 // seconds the lid takes to open, or to close
		float holdDistance = 800.0f; // m out from the shaft where ships wait for their turn
		float holdHeight = 500.0f;	 // m above the station's origin where they wait
		float holeRadius = 0.0f;	 // m, the hole cut in the ground over the shaft (none: 0)
		float holeDepth = 0.0f;		 // m, how far below the ground that hole goes for collisions
		vector3d hub;				 // the floor's centre (tag_turntable), station space
		vector3d shaft;				 // the pad point under the shaft (entrance_port01), station space
		std::vector<double> bayAngles; // per bay (from 0): the floor's angle that puts it under the shaft
	};

private:
	std::string id;
	SceneGraph::Model *model;
	std::string modelName;
	float angVel;
	enum DOCKMETHOD {
		SURFACE,
		ORBITAL
	} dockMethod;
	unsigned int numDockingPorts;
	DockStage lastDockStage;
	DockStage lastUndockStage;
	float parkingDistance;
	float parkingGapSize;
	BayPathMap m_bayPaths;
	TPorts m_ports;
	float padOffset;
	float flattenRadius;
	bool customOnly;
	// share of the random pick among the types a body suits
	float weight;
	// "conditions": a property of the body the station sits on or orbits, and
	// the range (from min_<name>, max_<name>) it must be in for a random pick
	struct Condition {
		int property;
		double min, max;
	};
	std::vector<Condition> conditions;
	Revolver m_revolver;

	static std::vector<SpaceStationType *> surfaceTypes;
	static std::vector<SpaceStationType *> orbitalTypes;

public:
	SpaceStationType(const std::string &id, const std::string &path);

	static bool IsDockStage(DockStage s) {
		return
			int(s) > int(DockStage::DOCK_STAGES_BEGIN) &&
			int(s) < int(DockStage::DOCK_STAGES_END);
	}

	static bool IsUndockStage(DockStage s) {
		return
			int(s) > int(DockStage::UNDOCK_STAGES_BEGIN) &&
			int(s) < int(DockStage::UNDOCK_STAGES_END);
	}

	static DockStage NextAnimStage(DockStage s) {
		return DockStage(int(s) + 1);
	}

	const char *DockStageName(DockStage s) const;

	void OnSetupComplete();
	void SetUpRevolver();
	const SPort *FindPortByBay(const int zeroBaseBayID) const;
	SPort *GetPortByBay(const int zeroBaseBayID);

	// Call functions in the station .lua
	bool GetShipApproachWaypoints(const unsigned int port, DockStage stage, positionOrient_t &outPosOrient) const;

	matrix4x4f GetStageTransform(int bay, DockStage stage) const;

	const std::string &ModelName() const { return modelName; }
	float AngVel() const { return angVel; }
	bool IsSurfaceStation() const { return (SURFACE == dockMethod); }
	bool IsOrbitalStation() const { return (ORBITAL == dockMethod); }
	unsigned int NumDockingPorts() const { return numDockingPorts; }
	int NumDockingStages() const { return int(lastDockStage) - int(DockStage::DOCK_ANIMATION_NONE); }
	int NumUndockStages() const { return int(lastUndockStage) - int(DockStage::UNDOCK_ANIMATION_NONE); }
	DockStage LastDockStage() const { return lastDockStage; }
	DockStage LastUndockStage() const { return lastUndockStage; }
	float ParkingDistance() const { return parkingDistance; }
	float ParkingGapSize() const { return parkingGapSize; }
	// metres from the model's origin the ground is levelled for a surface station
	float FlattenRadius() const { return flattenRadius; }
	const TPorts &Ports() const { return m_ports; }
	bool IsRevolver() const { return m_revolver.enabled; }
	const Revolver &GetRevolver() const { return m_revolver; }
	// whether sbody meets the conditions for a random pick of this type
	bool Suits(const SystemBody *sbody) const;

	static void Init();

	// a weighted pick among the non-custom types of sbody's kind it suits (all
	// of them if it suits none); equal weights pick as a plain uniform choice
	static const SpaceStationType *RandomStationType(Random &random, const SystemBody *sbody);
	static const SpaceStationType *FindByName(const std::string &name);
	// the type of the station at sbody: its custom type, or one picked with
	// random (seeded from sbody); nullptr before Init or without any types
	static const SpaceStationType *ForSystemBody(const SystemBody *sbody, Random &random);
};

#endif
