// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _EXCURSION_H
#define _EXCURSION_H

#include "JsonFwd.h"
#include "galaxy/SystemPath.h"
#include "vector3.h"

#include <memory>

class Body;
class Game;
class Rover;
class Space;
class SurfaceTracks;
class Walker;

// The player outside the ship: getting out of a landed ship on foot, or out
// of a ship coasting in space (EVA), driving the surface rover, and boarding
// the ship again. The Player stays the ship (missions, cargo, Lua
// Game.player); off the ship a Walker or the Rover carries the camera, and
// GetControlledBody() is where the player is and looks from (sound listener,
// HUD markers).
//
// The rover is ship equipment (the surface_rover capability). Deployed from
// the landed ship with the player at the wheel, it stays where it is parked
// while the player gets in and out of it, boards the ship and gets out again,
// and when the ship takes off without it: it waits on the planet, in this
// system or another, until the player comes back, drives it up to the ship
// and stows it. Until then no other rover comes out of the hold.
//
// Saved games keep the mode, the walker and the rover (both bodies in Space;
// the rover in another system as its planet, position and heading) and load
// straight back into the surface view.
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
		EXIT_ROVER,
		STOW_ROVER
	};

	explicit Excursion(Game *game);
	~Excursion();

	Mode GetMode() const { return m_mode; }
	bool IsOffShip() const { return m_mode != Mode::SHIP; }
	Walker *GetWalker() const { return m_walker; }
	// the rover while it is out of the ship in this system, driven or parked
	Rover *GetRover() const { return m_rover; }
	// the tracks its wheels and the walker's boots leave
	SurfaceTracks *GetTracks() const { return m_tracks.get(); }
	// out of the ship, here or parked in another system
	bool IsRoverOut() const { return m_rover || m_roverAway; }
	// the planet the rover is out on; false while it is in the hold
	bool GetRoverPlanet(SystemPath &path) const;
	// the walker or the rover off the ship, the player's ship otherwise
	Body *GetControlledBody() const;

	// the ship is landed (not docked) on a planet with dry ground beside it,
	// or flying in space (outside any rotating frame: above the atmosphere and
	// the terrain) without the autopilot
	bool CanDisembark() const;
	bool Disembark();
	// the walker is within BOARD_DISTANCE of the ship's bounding box
	bool CanBoard() const;
	// a parked rover stays where it is
	bool Board();

	// the ship is landed on a planet, has a rover aboard (the surface_rover
	// capability, and none out anywhere) and room beside it to set it down
	bool CanDeployRover() const;
	// sets the rover down beside the ship with the player at the wheel
	bool DeployRover();
	// on foot within ROVER_REACH of the rover
	bool CanEnterRover() const;
	bool EnterRover();
	// in the rover, with dry ground beside its door (or the other side's)
	bool CanExitRover() const;
	bool ExitRover();
	// in the rover, slow, within STOW_REACH of the ship's bounding box
	bool CanStowRover() const;
	// drives it into the hold, and the player boards the ship
	bool StowRover();

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
	// drawing (Player::UpdateInterpTransform): an EVA walker follows it, the
	// parked rover sleeps with no one within ROVER_WAKE_DISTANCE, and the
	// rover's wheels and the walker's boots press their tracks
	void OnShipMoved();
	void OnShipInterpolated(double alpha);

	// where the hatch is in the ship's model space: its tag_hatch, or the
	// middle of the right side of its bounding box
	vector3d GetHatchPoint() const;

	// the mode and the walker and the rover (by body index; Space saves the
	// bodies; the rover in another system by where it is), in the game's JSON
	// (Game::ToJson, after Space)
	void SaveToJson(Json &jsonObj, Space *space) const;
	void LoadFromJson(const Json &jsonObj, Space *space);
	// once the views are made: back to the surface view if off the ship
	void OnGameLoaded();
	// the bodies are about to go with the space (into hyperspace): back
	// aboard, and where the rover is parked kept for coming back
	void OnSpaceDestroyed();
	// out of hyperspace: the rover parked in this system is back on its planet
	void OnSpaceCreated();

	static constexpr double BOARD_DISTANCE = 3.0;
	// from the rover's collision box, to get in
	static constexpr double ROVER_REACH = 2.0;
	// from the ship's bounding box to the rover's, to stow it
	static constexpr double STOW_REACH = 2.0;
	// from the parked rover to the walker or the ship, for it to wake (m)
	static constexpr double ROVER_WAKE_DISTANCE = 10000.0;

private:
	// where the walker stands after getting out, in the ship's frame, and the
	// direction away from the ship: the model's tag_hatch if it has one, else
	// beside the first side of its bounding box with dry ground clear of the hull
	bool FindExitPoint(vector3d &pos, vector3d &facing) const;
	// where the rover goes down beside the ship, and the way it faces
	bool FindRoverSpot(vector3d &pos, vector3d &forward) const;
	// where the walker stands getting out of the rover, and the way it faces
	bool FindRoverExit(vector3d &pos, vector3d &facing) const;
	// the world view's surface controller, looking ahead
	void ShowSurfaceView();
	// back to the ship's view and controls
	void ShowShipView();
	bool CanStartEva() const;
	void StartEva();
	// the rover parked in another system, back on its planet if that is in
	// this space
	void BringBackRover(Space *space);
	// the wheels' tracks from where they last pressed them to where they are
	void PressTracks();
	// on foot, a boot's print at each step
	void PressFootprints();
	// the ship's flight controls take no input while the player is outside
	void SetShipControlsEnabled(bool enabled);

	Game *m_game;
	Mode m_mode = Mode::SHIP;
	Walker *m_walker = nullptr;
	Rover *m_rover = nullptr;

	// the rover parked in another system: its planet, and its position and
	// heading in the planet's rotating frame
	bool m_roverAway = false;
	SystemPath m_roverPlanet;
	vector3d m_roverPos;
	vector3d m_roverForward;

	std::unique_ptr<SurfaceTracks> m_tracks;
	// where each wheel last pressed its track, for this rover
	const Rover *m_trackRover = nullptr;
	vector3d m_wheelTracks[4];
	bool m_wheelTracked[4] = {};
	// the walker's last position, how far it has gone since its last
	// print, and which foot that was, for this walker
	const Walker *m_trackWalker = nullptr;
	vector3d m_footLast = vector3d(0.0);
	double m_footTravel = 0.0;
	bool m_footTracked = false;
	bool m_footLeft = false;
};

#endif /* _EXCURSION_H */
