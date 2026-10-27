// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _EXCURSION_H
#define _EXCURSION_H

#include "vector3.h"

class Body;
class Game;
class Walker;

// The player outside the ship: getting out of a landed ship on foot, or out
// of a ship coasting in space (EVA), and boarding it again. The Player stays
// the ship (missions, cargo, Lua Game.player); off the ship a Walker carries
// the camera, and GetControlledBody() is where the player is and looks from
// (sound listener, HUD markers).
// See docs/implementation_plans/on-foot-and-buggy_research.md
class Excursion {
public:
	enum class Mode {
		SHIP,
		ON_FOOT,
		EVA
	};

	// what the interact key would do now
	enum class Action {
		NONE,
		BOARD
	};

	explicit Excursion(Game *game);

	Mode GetMode() const { return m_mode; }
	bool IsOffShip() const { return m_mode != Mode::SHIP; }
	Walker *GetWalker() const { return m_walker; }
	// the walker off the ship, the player's ship otherwise
	Body *GetControlledBody() const;

	// the ship is landed (not docked) on a planet with dry ground beside it,
	// or flying in space (outside any rotating frame: above the atmosphere and
	// the terrain) without the autopilot
	bool CanDisembark() const;
	bool Disembark();
	// the walker is within BOARD_DISTANCE of the ship's bounding box
	bool CanBoard() const;
	bool Board();
	Action GetAvailableAction() const;

	// for the HUD compass: the view's heading and the bearing to the ship in
	// degrees clockwise from north, and the distance to the ship in metres
	bool GetNavInfo(double &heading, double &shipBearing, double &shipDistance) const;

	// the ship has moved (Player::TimeStepUpdate) or been interpolated for
	// drawing (Player::UpdateInterpTransform): an EVA walker follows it
	void OnShipMoved();
	void OnShipInterpolated(double alpha);

	// where the hatch is in the ship's model space: its tag_hatch, or the
	// middle of the right side of its bounding box
	vector3d GetHatchPoint() const;

	static constexpr double BOARD_DISTANCE = 3.0;

private:
	// where the walker stands after getting out, in the ship's frame, and the
	// direction away from the ship: the model's tag_hatch if it has one, else
	// beside the first side of its bounding box with dry ground clear of the hull
	bool FindExitPoint(vector3d &pos, vector3d &facing) const;
	bool CanStartEva() const;
	void StartEva();
	// the ship's flight controls take no input while the player is outside
	void SetShipControlsEnabled(bool enabled);

	Game *m_game;
	Mode m_mode = Mode::SHIP;
	Walker *m_walker = nullptr;
};

#endif /* _EXCURSION_H */
