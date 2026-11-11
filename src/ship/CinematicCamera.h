// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#pragma once

#include "Camera.h"
#include "FrameId.h"
#include "Input.h"
#include "RefCounted.h"
#include "matrix3x3.h"
#include "vector3.h"

#include <string>

class Body;
class Ship;
class Space;

// A debug camera (The Last Frontier): it comes loose from the ship and stays
// where it is in space, and the keys and the mouse fly it about to film the
// ship and the world. It is never saved with the game.
//
// It keeps its own place: a frame (the patch of space it moves with, e.g. a
// planet's turning ground), a position and a facing in that frame. Like a
// ship, it moves into a planet's frame when it comes near and out again when
// it leaves, so a camera put down on the ground stays on the ground.
class CinematicCamera {
public:
	struct Settings {
		bool keysMoveCamera = true; // false: the camera holds still and the keys fly the ship again
		bool lookAtShip = false;	// keep turning to face the ship
		bool travelWithShip = false; // move along with the ship, keeping the same gap
		bool levelHorizon = true;	// no tilting sideways; turning left and right goes round "up"
		bool hideHud = false;		// hide the game's on-screen panels while it is on
		bool rideWithStation = true; // near a space station, turn and move with it as it spins
		bool stopAtGround = true;	// false: it goes through the ground too (it always goes through bases and ships)
		double speed = 20.0;		// metres a second, at full key press
		double smoothing = 0.4;		// seconds the camera takes to settle after a change; 0 is instant
		float fov = 0.0f;			// field of view, degrees (set from the game's camera on start)
	};

	CinematicCamera(RefCountedPtr<CameraContext> camera, const Ship *ship);
	~CinematicCamera();

	// start where the game's camera is right now
	void Start();
	void Stop();
	bool IsOn() const { return m_on; }

	// the view (world view) was shown or hidden: hand the keys back while it is hidden
	void OnActivated();
	void OnDeactivated();

	// moves the camera and places the game's camera there; false if it had to
	// stop (the ship left the star system)
	bool Update(float frameTime);

	// mouse wheel: faster or slower
	void ChangeSpeed(bool faster);

	Settings settings;

	// readouts for the debug tab
	double GetDistanceToShip() const;
	double GetCurrentSpeed() const { return m_vel.Length(); }
	std::string GetFrameLabel() const;

private:
	void UpdateInputFrame();
	void SetMouseLook(bool on);
	void SwitchToFrame(FrameId to);
	void UpdateFrame();
	vector3d LocalUp() const;
	void KeepAboveGround();
	void Level();
	void RideWithStation();

	RefCountedPtr<CameraContext> m_camera;
	const Ship *m_ship;
	Input::InputFrame m_keysFrame; // takes the keys from the ship while the camera flies
	bool m_on = false;
	bool m_shown = false; // the ship view is on screen (set by OnActivated)
	bool m_mouseLook = false;

	const Space *m_space = nullptr; // the star system it was started in
	FrameId m_frame;
	vector3d m_pos;
	matrix3x3d m_orient;
	vector3d m_vel;
	vector3d m_up;	// "up" where there is no planet to give one, in m_frame
	double m_pendingYaw = 0.0, m_pendingPitch = 0.0, m_pendingRoll = 0.0;

	vector3d m_lastShipPos;	// for travelling with the ship, in m_frame
	bool m_haveLastShipPos = false;

	// the space station the camera rides with: where it was last frame, in m_frame
	const Body *m_anchor = nullptr;
	matrix3x3d m_anchorOrient;
	vector3d m_anchorPos;

	// what this placed the game's camera at last frame, to see if anything
	// else moved it in between (written to output.txt)
	FrameId m_placedFrame;
	vector3d m_placedPos;
	matrix3x3d m_placedOrient;
	bool m_placed = false;
	int m_lastFlightState = -1;

	float m_savedFov = 0.0f;
	bool m_savedDrawGui = true;
	bool m_hidingHud = false;
};
