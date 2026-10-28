// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#pragma once

#include "ConnectionTicket.h"
#include "Input.h"
#include "InputBindings.h"
#include "ViewController.h"
#include "matrix3x3.h"
#include "sound/Sound.h"
#include "vector3.h"

#include <memory>

class EvaTether;
class Rover;
class Walker;

// The world view while the player is out of the ship (Excursion): a
// first-person camera at the walker's eyes (bobbing as it walks, unless the
// OnFootHeadBob setting is off) or in the rover's driving seat, mouse look,
// and the on-foot controls, which also drive the rover (forward and right for
// throttle and steering, jump for the brake). Its input frame replaces the
// ship's flight and camera controls. Getting into the rover, the view is the
// RoverDefaultView setting's: 0 the cab, 1 without the rover, 2 from outside.
class SurfaceViewController : public ViewController {
public:
	SurfaceViewController(WorldView *v);
	~SurfaceViewController();

	void Activated() override;
	void Deactivated() override;
	void Update() override;
	void Draw(Camera *camera) override;

	// level the view, looking ahead, and the rover's view the default one
	// (on getting out of the ship, or in or out of the rover)
	void ResetLook();

	struct InputBinding : public Input::InputFrame {
		using InputFrame::InputFrame;

		Axis *moveForward;
		Axis *moveRight;
		Axis *lookYaw;
		Axis *lookPitch;
		Axis *jetpackUp;
		Action *run;
		Action *jump;
		Action *interact;
		Action *roverView;

		void RegisterBindings() override;
	} InputBindings;

private:
	void OnJump() { m_jumpRequested = true; }
	void OnInteract() { m_interactRequested = true; }
	// the rover's views in turn: the cab, without the rover, from outside
	void OnRoverView();
	void OnMouseWheel(bool up);
	// the rover's model is drawn unless the driver has hidden it
	void ShowRoverModel(bool show);
	// the mouse is captured for looking around, and freed while the game is
	// paused, the console is open, or Alt is held (to click the HUD)
	void UpdateMouseCapture();
	void SetMouseCaptured(bool captured);
	void PlaceCamera(const Walker *walker);
	void PlaceRoverCamera(const Rover *rover);
	void PlaceRoverExternalCamera(const Rover *rover);
	void UpdateJetSound(const Walker *walker);
	void UpdateFootsteps(const Walker *walker);
	// how far through its stride the walker is, and how much the head bobs
	void UpdateHeadBob(const Walker *walker);
	void SilenceWalker();
	// the line to the ship on EVA, after the camera is placed
	void UpdateTether(const Walker *walker);

	double m_pitch = 0.0;
	// in the rover, looking round from its heading, to the right
	double m_roverYaw = 0.0;
	enum class RoverView {
		CAB,
		NO_MODEL, // as the ship's internal view: the driver's eyes, no rover drawn
		EXTERNAL  // circling the rover, as the ship's external view
	};
	RoverView m_roverView = RoverView::CAB;
	// the external view: round from behind the rover (to its right), up, and
	// how far from it (m)
	double m_extYaw = 0.0;
	double m_extPitch;
	double m_extDistance;
	// the head bob: radians through the stride (pi a step), and the height of
	// the bob now (m), easing in and out as the walker starts and stops
	double m_bobPhase = 0.0;
	double m_bobHeight = 0.0;
	bool m_mouseCaptured = false;
	bool m_skipMouseMotion = false;
	bool m_jumpRequested = false;
	bool m_interactRequested = false;

	// where PlaceCamera put the camera, in the walker's frame
	vector3d m_camPos;
	matrix3x3d m_camOrient;

	std::unique_ptr<EvaTether> m_tether;
	matrix3x3d m_tetherRootToView;
	vector3d m_tetherShipInView;

	Sound::Event m_jetSound;
	Sound::Event m_footsteps;

	ConnectionTicket m_onJumpCon;
	ConnectionTicket m_onInteractCon;
	ConnectionTicket m_onRoverViewCon;
	ConnectionTicket m_onMouseWheelCon;
};
