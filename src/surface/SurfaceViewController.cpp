// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "SurfaceViewController.h"

#include "Camera.h"
#include "Excursion.h"
#include "Game.h"
#include "GameConfig.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Walker.h"
#include "WorldView.h"
#include "sound/Sound.h"

#include <SDL_keycode.h>
#include <algorithm>
#include <cmath>

namespace {
	constexpr double MOUSE_LOOK_SPEED = 0.0025; // radians per pixel
	constexpr double STICK_LOOK_SPEED = 2.0;	// radians per second at full deflection
	constexpr double MAX_PITCH = DEG2RAD(89.0);
	constexpr float JET_SOUND_VOLUME = 0.4f;
	constexpr float JET_SOUND_FADE = 4.0f; // volume per second
	// footsteps: a looped recording of walking at about WALK_PACE m/s, played
	// faster (and higher) for running, from STEP_MIN_SPEED
	constexpr float STEPS_VOLUME = 0.5f;
	constexpr float STEPS_FADE = 6.0f;
	constexpr double WALK_PACE = 1.6;
	constexpr double RUN_PACE = 4.5;
	constexpr double RUN_PITCH = 1.4;
	constexpr double STEP_MIN_SPEED = 0.3;
} // namespace

REGISTER_INPUT_BINDING(SurfaceViewController)
{
	using namespace InputBindings;
	Input::BindingPage *page = input->GetBindingPage("OnFoot");

	Input::BindingGroup *movement = page->GetBindingGroup("Movement");
	input->AddAxisBinding("BindWalkForward", movement, Axis({}, { SDLK_w }, { SDLK_s }));
	input->AddAxisBinding("BindWalkRight", movement, Axis({}, { SDLK_d }, { SDLK_a }));
	input->AddActionBinding("BindWalkRun", movement, Action({ SDLK_LSHIFT }));
	input->AddActionBinding("BindWalkJump", movement, Action({ SDLK_SPACE }));
	input->AddAxisBinding("BindWalkLookYaw", movement, Axis());
	input->AddAxisBinding("BindWalkLookPitch", movement, Axis());
	// as the ship's up / down thrust; forward and right steer it in the air
	input->AddAxisBinding("BindJetpackUp", movement, Axis({}, { SDLK_r }, { SDLK_f }));

	Input::BindingGroup *interaction = page->GetBindingGroup("Interaction");
	input->AddActionBinding("BindInteract", interaction, Action({ SDLK_e }));
}

void SurfaceViewController::InputBinding::RegisterBindings()
{
	moveForward = AddAxis("BindWalkForward");
	moveRight = AddAxis("BindWalkRight");
	lookYaw = AddAxis("BindWalkLookYaw");
	lookPitch = AddAxis("BindWalkLookPitch");
	jetpackUp = AddAxis("BindJetpackUp");
	run = AddAction("BindWalkRun");
	jump = AddAction("BindWalkJump");
	interact = AddAction("BindInteract");
}

SurfaceViewController::SurfaceViewController(WorldView *v) :
	ViewController(v),
	InputBindings(Pi::input)
{
	InputBindings.RegisterBindings();
	m_onJumpCon = InputBindings.jump->onPressed.connect(sigc::mem_fun(this, &SurfaceViewController::OnJump));
	m_onInteractCon = InputBindings.interact->onPressed.connect(sigc::mem_fun(this, &SurfaceViewController::OnInteract));
}

SurfaceViewController::~SurfaceViewController()
{
	m_jetSound.Stop();
	m_footsteps.Stop();
	SetMouseCaptured(false);
	Pi::input->RemoveInputFrame(&InputBindings);
}

void SurfaceViewController::Activated()
{
	Pi::input->AddInputFrame(&InputBindings);
	m_parentView->GetCameraContext()->SetFovAng(Pi::config->Float("FOVVertical"));
	m_jumpRequested = false;
	m_interactRequested = false;
}

void SurfaceViewController::Deactivated()
{
	Pi::input->RemoveInputFrame(&InputBindings);
	SetMouseCaptured(false);
	m_jetSound.Stop();
	m_footsteps.Stop();
	if (Walker *walker = Pi::game->GetExcursion()->GetWalker())
		walker->SetMoveInput(0.0, 0.0, 0.0, false);
}

void SurfaceViewController::SetMouseCaptured(bool captured)
{
	// checked against the input manager too, in case something else freed it
	if (captured != Pi::input->IsCapturingMouse() && (captured || m_mouseCaptured)) {
		Pi::input->SetCapturingMouse(captured);
		// relative mode can report a jump on the frame it starts
		m_skipMouseMotion = captured;
	}
	m_mouseCaptured = captured;
}

void SurfaceViewController::UpdateMouseCapture()
{
	const bool altHeld = Pi::input->KeyState(SDLK_LALT) || Pi::input->KeyState(SDLK_RALT);
	SetMouseCaptured(InputBindings.active && !Pi::game->IsPaused() && !altHeld);
}

void SurfaceViewController::Update()
{
	Excursion *excursion = Pi::game->GetExcursion();
	Walker *walker = excursion->GetWalker();
	if (!walker)
		return;

	UpdateMouseCapture();

	if (InputBindings.active) {
		const double frameTime = Pi::GetFrameTime();
		double yaw = InputBindings.lookYaw->GetValue() * STICK_LOOK_SPEED * frameTime;
		double pitch = InputBindings.lookPitch->GetValue() * STICK_LOOK_SPEED * frameTime;

		int motion[2];
		Pi::input->GetMouseMotion(motion);
		if (m_mouseCaptured && !m_skipMouseMotion) {
			const double speed = MOUSE_LOOK_SPEED * Pi::config->Float("OnFootMouseSensitivity", 1.0f);
			yaw += motion[0] * speed;
			pitch += (Pi::input->IsMouseYInvert() ? motion[1] : -motion[1]) * speed;
		}
		m_skipMouseMotion = false;

		if (walker->IsEva()) {
			// floating free: turn about its own axes
			walker->LookEva(yaw, pitch);
		} else {
			walker->Turn(yaw);
			m_pitch = Clamp(m_pitch + pitch, -MAX_PITCH, MAX_PITCH);
		}

		walker->SetMoveInput(InputBindings.moveForward->GetValue(), InputBindings.moveRight->GetValue(),
			InputBindings.jetpackUp->GetValue(), InputBindings.run->IsActive());
		if (m_jumpRequested)
			walker->RequestJump();
	} else {
		walker->SetMoveInput(0.0, 0.0, 0.0, false);
	}
	m_jumpRequested = false;

	UpdateJetSound(walker);
	UpdateFootsteps(walker);

	if (m_interactRequested) {
		m_interactRequested = false;
		// boarding switches the world view back to the ship's controller
		if (excursion->GetAvailableAction() == Excursion::Action::BOARD && excursion->Board())
			return;
	}

	PlaceCamera(walker);
}

void SurfaceViewController::UpdateJetSound(const Walker *walker)
{
	// a looped roar that follows the thrust, a little higher at full thrust
	const double thrust = walker->GetJetThrust();
	const float volume = Pi::game->IsPaused() ? 0.0f : JET_SOUND_VOLUME * float(thrust);
	if (!m_jetSound.VolumeAnimate(volume, volume, JET_SOUND_FADE, JET_SOUND_FADE) && volume > 0.0f) {
		m_jetSound.Play("Jetpack_Thrust", 0.0f, 0.0f, Sound::OP_REPEAT);
		m_jetSound.VolumeAnimate(volume, volume, JET_SOUND_FADE, JET_SOUND_FADE);
	}
	if (thrust > 0.0)
		m_jetSound.SetPitch(float(0.9 + 0.15 * thrust));
}

void SurfaceViewController::UpdateFootsteps(const Walker *walker)
{
	const double speed = walker->IsOnGround() ? walker->GetVelocity().Length() : 0.0;
	const bool walking = speed > STEP_MIN_SPEED && !Pi::game->IsPaused();
	const float volume = walking ? STEPS_VOLUME * float(std::min(speed / WALK_PACE, 1.0) * (0.8 + 0.2 * std::min(speed / RUN_PACE, 1.0))) : 0.0f;
	if (!m_footsteps.VolumeAnimate(volume, volume, STEPS_FADE, STEPS_FADE) && volume > 0.0f) {
		m_footsteps.Play("footsteps", 0.0f, 0.0f, Sound::OP_REPEAT);
		m_footsteps.VolumeAnimate(volume, volume, STEPS_FADE, STEPS_FADE);
	}
	if (walking) {
		const double pitch = 1.0 + (speed - WALK_PACE) / (RUN_PACE - WALK_PACE) * (RUN_PITCH - 1.0);
		m_footsteps.SetPitch(float(Clamp(pitch, 0.8, RUN_PITCH)));
	}
}

void SurfaceViewController::PlaceCamera(const Walker *walker)
{
	RefCountedPtr<CameraContext> camera = m_parentView->GetCameraContext();
	camera->SetCameraFrame(walker->GetFrame());

	// in EVA the walker is all eyes
	if (walker->IsEva()) {
		camera->SetCameraPosition(walker->GetInterpPosition());
		camera->SetCameraOrient(walker->GetEvaOrient());
		return;
	}

	const vector3d feet = walker->GetInterpPosition();
	const vector3d up = feet.Normalized();
	vector3d forward = walker->GetForward();
	forward = (forward - up * up.Dot(forward)).NormalizedSafe();
	const vector3d right = forward.Cross(up);

	// pitch turns the view about its right axis
	const double c = std::cos(m_pitch);
	const double s = std::sin(m_pitch);
	const vector3d viewForward = forward * c + up * s;
	const vector3d viewUp = up * c - forward * s;

	camera->SetCameraPosition(feet + up * Walker::EYE_HEIGHT);
	camera->SetCameraOrient(matrix3x3d::FromVectors(right, viewUp, -viewForward));
}
