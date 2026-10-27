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
class Walker;

// The world view while the player is out of the ship (Excursion): a
// first-person camera at the walker's eyes, mouse look, and the on-foot
// controls. Its input frame replaces the ship's flight and camera controls.
class SurfaceViewController : public ViewController {
public:
	SurfaceViewController(WorldView *v);
	~SurfaceViewController();

	void Activated() override;
	void Deactivated() override;
	void Update() override;
	void Draw(Camera *camera) override;

	// level the view (on getting out of the ship)
	void ResetLook() { m_pitch = 0.0; }

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

		void RegisterBindings() override;
	} InputBindings;

private:
	void OnJump() { m_jumpRequested = true; }
	void OnInteract() { m_interactRequested = true; }
	// the mouse is captured for looking around, and freed while the game is
	// paused, the console is open, or Alt is held (to click the HUD)
	void UpdateMouseCapture();
	void SetMouseCaptured(bool captured);
	void PlaceCamera(const Walker *walker);
	void UpdateJetSound(const Walker *walker);
	void UpdateFootsteps(const Walker *walker);
	// the line to the ship on EVA, after the camera is placed
	void UpdateTether(const Walker *walker);

	double m_pitch = 0.0;
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
};
