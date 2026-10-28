// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _EXCURSION_H
#define _EXCURSION_H

#include "vector3.h"

class Body;
class Game;
class Rover;
class Walker;

// The player outside the ship: getting out of a landed ship on foot, or out
// of a ship coasting in space (EVA), driving the surface rover, and boarding
// the ship again. The Player stays the ship (missions, cargo, Lua
// Game.player); off the ship a Walker or the Rover carries the camera, and
// GetControlledBody() is where the player is and looks from (sound listener,
// HUD markers).
//
// The rover is ship equipment (the surface_rover capability). Deployed from
// the landed ship with the player at the wheel, it stays out while the player
// gets in and out of it, and goes back into the hold when the player boards
// the ship, wherever it is parked.
// See docs/implementation_plans/on-foot-and-buggy_research.md
class Excursion {
public:
	enum class Mode {
		SHIP,
		ON_FOOT,
		EVA,
		ROVER
	};

	// what the interact key would do now
	enum class Action {
		NONE,
		BOARD,
		ENTER_ROVER,
		EXIT_ROVER
	};

	explicit Excursion(Game *game);

	Mode GetMode() const { return m_mode; }
	bool IsOffShip() const { return m_mode != Mode::SHIP; }
	Walker *GetWalker() const { return m_walker; }
	// the rover while it is out of the ship, driven or parked
	Rover *GetRover() const { return m_rover; }
	// the walker or the rover off the ship, the player's ship otherwise
	Body *GetControlledBody() const;

	// the ship is landed (not docked) on a planet with dry ground beside it,
	// or flying in space (outside any rotating frame: above the atmosphere and
	// the terrain) without the autopilot
	bool CanDisembark() const;
	bool Disembark();
	// the walker is within BOARD_DISTANCE of the ship's bounding box
	bool CanBoard() const;
	// boarding stows a deployed rover
	bool Board();

	// the ship is landed on a planet, has a rover aboard (the surface_rover
	// capability) and room beside it to set it down
	bool CanDeployRover() const;
	// sets the rover down beside the ship with the player at the wheel
	bool DeployRover();
	// on foot within ROVER_REACH of the rover
	bool CanEnterRover() const;
	bool EnterRover();
	// in the rover, with dry ground beside its door (or the other side's)
	bool CanExitRover() const;
	bool ExitRover();

	Action GetAvailableAction() const;
	// does what the interact key would do; the action done, NONE if nothing
	Action Interact();

	struct NavInfo {
		double heading; // the view's (on foot) or the rover's, degrees clockwise from north
		double shipBearing;
		double shipDistance; // metres
		bool hasRover; // on foot with the rover out: where it is
		double roverBearing;
		double roverDistance;
	};
	// for the HUD compass, on the surface
	bool GetNavInfo(NavInfo &info) const;

	// the ship has moved (Player::TimeStepUpdate) or been interpolated for
	// drawing (Player::UpdateInterpTransform): an EVA walker follows it
	void OnShipMoved();
	void OnShipInterpolated(double alpha);

	// where the hatch is in the ship's model space: its tag_hatch, or the
	// middle of the right side of its bounding box
	vector3d GetHatchPoint() const;

	static constexpr double BOARD_DISTANCE = 3.0;
	// from the rover's collision box, to get in
	static constexpr double ROVER_REACH = 2.0;

private:
	// where the walker stands after getting out, in the ship's frame, and the
	// direction away from the ship: the model's tag_hatch if it has one, else
	// beside the first side of its bounding box with dry ground clear of the hull
	bool FindExitPoint(vector3d &pos, vector3d &facing) const;
	// where the rover goes down beside the ship, and the way it faces
	bool FindRoverSpot(vector3d &pos, vector3d &forward) const;
	// where the walker stands getting out of the rover, and the way it faces
	bool FindRoverExit(vector3d &pos, vector3d &facing) const;
	// takes the rover back aboard
	void StowRover();
	// the world view's surface controller, looking ahead
	void ShowSurfaceView();
	bool CanStartEva() const;
	void StartEva();
	// the ship's flight controls take no input while the player is outside
	void SetShipControlsEnabled(bool enabled);

	Game *m_game;
	Mode m_mode = Mode::SHIP;
	Walker *m_walker = nullptr;
	Rover *m_rover = nullptr;
};

#endif /* _EXCURSION_H */
