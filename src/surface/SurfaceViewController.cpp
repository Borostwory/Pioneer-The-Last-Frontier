// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "SurfaceViewController.h"

#include "Camera.h"
#include "EvaTether.h"
#include "Excursion.h"
#include "Frame.h"
#include "Game.h"
#include "GameConfig.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Player.h"
#include "Rover.h"
#include "Space.h"
#include "TerrainBody.h"
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
	// looking round from the rover's driving seat
	constexpr double MAX_ROVER_YAW = DEG2RAD(160.0);
	constexpr double MAX_ROVER_PITCH = DEG2RAD(70.0);
	// the external view of the rover: looking at this point of it (model
	// space), from above the ground, no nearer or farther than these (m)
	const vector3d EXTERNAL_TARGET(0.0, 1.8, -0.35);
	constexpr double EXTERNAL_PITCH = DEG2RAD(20.0);
	constexpr double EXTERNAL_MIN_PITCH = DEG2RAD(-5.0);
	constexpr double EXTERNAL_MAX_PITCH = DEG2RAD(85.0);
	constexpr double EXTERNAL_DISTANCE = 14.0;
	constexpr double EXTERNAL_MIN_DISTANCE = 7.0;
	constexpr double EXTERNAL_MAX_DISTANCE = 80.0;
	constexpr double EXTERNAL_ZOOM_STEP = 1.15; // per wheel click
	constexpr double EXTERNAL_GROUND_CLEARANCE = 0.5;
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
	// the head bob: down at each footfall and up between, swaying over to the
	// foot it stands on; walking or running, this high (m), steps this long
	// (m), and easing to a new height at BOB_FADE (1/s)
	constexpr double BOB_WALK_HEIGHT = 0.03;
	constexpr double BOB_RUN_HEIGHT = 0.05;
	constexpr double BOB_WALK_STEP = 0.75;
	constexpr double BOB_RUN_STEP = 1.3;
	constexpr double BOB_SWAY = 0.5; // of the height
	constexpr double BOB_FADE = 8.0;
	// where the line is clipped on: the walker's belt, below and ahead of the eyes
	const vector3d TETHER_BELT(0.15, -0.6, -0.2);

	// the rover's motor, recorded from an electric car: pulling away from
	// standing and slowing to a stop (played once, from their start), and a
	// loop at the car's top speed. The car's speed through each one-shot, as a
	// fraction of that, every ACCEL_STEP or DECEL_STEP (s) of the recording.
	// The motor turns with the wheels, so its speed is theirs (a fraction of
	// Rover::MAX_SPEED; in the air they spin up with the throttle, down
	// without it and stop with the brake). Each one-shot plays pitched by that
	// over the car's speed at that point, so the motor's tone follows the
	// wheels and the one-shot runs faster or slower with them; the loop,
	// pitched by the speed, takes over (faded) when the driver stops doing
	// what the one-shot recorded, the wheels outrun it beyond the pitch
	// range, or it ends. The one-shots begin and end as the loop does, and at
	// its pitch.
	constexpr double ACCEL_SPEEDS[] = { 0.0, 0.20, 0.255, 0.295, 0.375, 0.435, 0.496, 0.556, 0.615, 0.668,
		0.717, 0.763, 0.811, 0.853, 0.891, 0.922, 0.948, 0.975, 0.996, 1.0 };
	constexpr double ACCEL_STEP = 1.0;
	constexpr double ACCEL_LENGTH = 18.3; // s
	constexpr double DECEL_SPEEDS[] = { 1.0, 0.98, 0.93, 0.89, 0.86, 0.82, 0.76, 0.73, 0.66, 0.60, 0.52,
		0.46, 0.38, 0.30, 0.22, 0.15, 0.08, 0.03, 0.0 };
	constexpr double DECEL_STEP = 0.5;
	constexpr double DECEL_LENGTH = 10.8;
	// pulling away from below this speed plays the acceleration; letting go
	// above this one the slowing down; at the top speed, the loop
	constexpr double ACCEL_FROM = 0.15;
	constexpr double DECEL_FROM = 0.7;
	constexpr double TOP_SPEED = 0.97;
	// near standing the one-shots play as recorded
	constexpr double MOTOR_SLOWEST = 0.15;
	// the pitch range the sound backend plays (all OpenAL guarantees)
	constexpr double MOTOR_MIN_PITCH = 0.5;
	constexpr double MOTOR_MAX_PITCH = 2.0;
	// the loop is quieter slow, at full volume from this speed
	constexpr double CRUISE_FULL_SPEED = 0.3;
	// a one-shot hands over to the loop this long before its end (s)
	constexpr double HANDOVER = 0.3;
	constexpr float MOTOR_VOLUME = 0.6f;
	// coasting or braking, the motor this much as loud as at full throttle
	constexpr float MOTOR_COAST_VOLUME = 0.75f;
	constexpr float MOTOR_FADE = 1.5f; // volume per second: a crossfade of 0.4 s
	constexpr float MOTOR_FADE_IN = 6.0f;
	// the tyres rolling on the ground, recorded at about this speed (a
	// fraction of the top speed): pitched by the speed over it, louder up to
	// it, and with the wheels on the ground (silent in the air)
	constexpr double ROLL_RECORDED_SPEED = 0.75;
	constexpr float ROLL_VOLUME = 1.0f;
	constexpr float ROLL_FADE = 10.0f;

	// the car's speed a time into a one-shot, from its table
	template <size_t N>
	double RecordedSpeed(const double (&speeds)[N], double step, double time)
	{
		const double i = Clamp(time / step, 0.0, double(N - 1));
		const size_t a = std::min(size_t(i), N - 2);
		return speeds[a] + (speeds[a + 1] - speeds[a]) * (i - double(a));
	}
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
	// as the ship's camera mode key: the rover's views in turn
	input->AddActionBinding("BindRoverCycleView", interaction, Action({ SDLK_F1 }));
	// the rover's lamps: off, each level brighter, off again
	input->AddActionBinding("BindRoverLights", interaction, Action({ SDLK_l }));
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
	roverView = AddAction("BindRoverCycleView");
	roverLights = AddAction("BindRoverLights");
}

SurfaceViewController::SurfaceViewController(WorldView *v) :
	ViewController(v),
	InputBindings(Pi::input),
	m_extPitch(EXTERNAL_PITCH),
	m_extDistance(EXTERNAL_DISTANCE),
	m_camPos(0.0),
	m_camOrient(matrix3x3d::Identity),
	m_tether(new EvaTether()),
	m_tetherRootToView(matrix3x3d::Identity),
	m_tetherShipInView(0.0)
{
	InputBindings.RegisterBindings();
	m_onJumpCon = InputBindings.jump->onPressed.connect(sigc::mem_fun(this, &SurfaceViewController::OnJump));
	m_onInteractCon = InputBindings.interact->onPressed.connect(sigc::mem_fun(this, &SurfaceViewController::OnInteract));
	m_onRoverViewCon = InputBindings.roverView->onPressed.connect(sigc::mem_fun(this, &SurfaceViewController::OnRoverView));
	m_onRoverLightsCon = InputBindings.roverLights->onPressed.connect(sigc::mem_fun(this, &SurfaceViewController::OnRoverLights));
}

SurfaceViewController::~SurfaceViewController()
{
	m_jetSound.Stop();
	m_footsteps.Stop();
	m_roverAccel.Stop();
	m_roverCruise.Stop();
	m_roverDecel.Stop();
	m_roverRolling.Stop();
	SetMouseCaptured(false);
	Pi::input->RemoveInputFrame(&InputBindings);
}

void SurfaceViewController::Activated()
{
	Pi::input->AddInputFrame(&InputBindings);
	m_parentView->GetCameraContext()->SetFovAng(Pi::config->Float("FOVVertical"));
	m_jumpRequested = false;
	m_interactRequested = false;
	m_onMouseWheelCon = Pi::input->onMouseWheel.connect(sigc::mem_fun(this, &SurfaceViewController::OnMouseWheel));
}

void SurfaceViewController::Deactivated()
{
	Pi::input->RemoveInputFrame(&InputBindings);
	m_onMouseWheelCon.disconnect();
	SetMouseCaptured(false);
	m_jetSound.Stop();
	m_footsteps.Stop();
	m_roverAccel.Stop();
	m_roverCruise.Stop();
	m_roverDecel.Stop();
	m_roverRolling.Stop();
	m_roverAccelTime = m_roverDecelTime = -1.0;
	m_roverPushed = false;
	m_tether->Clear();
	if (Walker *walker = Pi::game->GetExcursion()->GetWalker())
		walker->SetMoveInput(0.0, 0.0, 0.0, false);
	if (Rover *rover = Pi::game->GetExcursion()->GetRover())
		rover->SetDriveInput(0.0, 0.0, true);
	ShowRoverModel(true);
}

void SurfaceViewController::ResetLook()
{
	m_pitch = 0.0;
	m_roverYaw = 0.0;
	m_extYaw = 0.0;
	m_extPitch = EXTERNAL_PITCH;
	m_bobHeight = 0.0;
	switch (Pi::config->Int("RoverDefaultView")) {
	case 1: m_roverView = RoverView::NO_MODEL; break;
	case 2: m_roverView = RoverView::EXTERNAL; break;
	default: m_roverView = RoverView::CAB; break;
	}
}

void SurfaceViewController::OnRoverView()
{
	switch (m_roverView) {
	case RoverView::CAB: m_roverView = RoverView::NO_MODEL; break;
	case RoverView::NO_MODEL: m_roverView = RoverView::EXTERNAL; break;
	case RoverView::EXTERNAL: m_roverView = RoverView::CAB; break;
	}
}

void SurfaceViewController::OnRoverLights()
{
	Excursion *excursion = Pi::game->GetExcursion();
	if (excursion->GetMode() == Excursion::Mode::ROVER && excursion->GetRover())
		excursion->GetRover()->CycleLights();
}

void SurfaceViewController::OnMouseWheel(bool up)
{
	if (m_roverView != RoverView::EXTERNAL || Pi::game->GetExcursion()->GetMode() != Excursion::Mode::ROVER)
		return;
	m_extDistance = Clamp(up ? m_extDistance / EXTERNAL_ZOOM_STEP : m_extDistance * EXTERNAL_ZOOM_STEP,
		EXTERNAL_MIN_DISTANCE, EXTERNAL_MAX_DISTANCE);
}

void SurfaceViewController::ShowRoverModel(bool show)
{
	if (Rover *rover = Pi::game->GetExcursion()->GetRover())
		rover->SetFlag(Body::FLAG_DRAW_EXCLUDE, !show);
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
	Rover *rover = excursion->GetMode() == Excursion::Mode::ROVER ? excursion->GetRover() : nullptr;
	if (!walker && !rover)
		return;

	UpdateMouseCapture();
	double throttle = 0.0;
	bool brake = true;

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

		if (rover) {
			// looking round in the seat, or circling the rover from outside;
			// the jump key is the brake
			if (m_roverView == RoverView::EXTERNAL) {
				m_extYaw = std::remainder(m_extYaw + yaw, 2.0 * M_PI);
				m_extPitch = Clamp(m_extPitch - pitch, EXTERNAL_MIN_PITCH, EXTERNAL_MAX_PITCH);
			} else {
				m_roverYaw = Clamp(m_roverYaw + yaw, -MAX_ROVER_YAW, MAX_ROVER_YAW);
				m_pitch = Clamp(m_pitch + pitch, -MAX_ROVER_PITCH, MAX_ROVER_PITCH);
			}
			throttle = InputBindings.moveForward->GetValue();
			brake = InputBindings.jump->IsActive();
			rover->SetDriveInput(throttle, InputBindings.moveRight->GetValue(), brake);
		} else if (walker->IsEva()) {
			// floating free: turn about its own axes
			walker->LookEva(yaw, pitch);
		} else {
			walker->Turn(yaw);
			m_pitch = Clamp(m_pitch + pitch, -MAX_PITCH, MAX_PITCH);
		}

		if (walker) {
			walker->SetMoveInput(InputBindings.moveForward->GetValue(), InputBindings.moveRight->GetValue(),
				InputBindings.jetpackUp->GetValue(), InputBindings.run->IsActive());
			if (m_jumpRequested)
				walker->RequestJump();
		}
	} else if (rover) {
		rover->SetDriveInput(0.0, 0.0, true);
	} else {
		walker->SetMoveInput(0.0, 0.0, 0.0, false);
	}
	m_jumpRequested = false;

	if (m_interactRequested) {
		m_interactRequested = false;
		// boarding switches the world view back to the ship's controller;
		// getting in or out of the rover changes who carries the camera
		const Excursion::Action done = excursion->Interact();
		if (done == Excursion::Action::BOARD || done == Excursion::Action::STOW_ROVER)
			return;
		if (done != Excursion::Action::NONE) {
			walker = excursion->GetWalker();
			rover = excursion->GetMode() == Excursion::Mode::ROVER ? excursion->GetRover() : nullptr;
		}
	}

	// hidden only from its driver's seat
	ShowRoverModel(!rover || m_roverView != RoverView::NO_MODEL);

	if (rover) {
		SilenceWalker();
		UpdateRoverSound(rover, throttle, brake);
		m_tether->Clear();
		if (m_roverView == RoverView::EXTERNAL)
			PlaceRoverExternalCamera(rover);
		else
			PlaceRoverCamera(rover);
		return;
	}

	SilenceRover();
	UpdateJetSound(walker);
	UpdateFootsteps(walker);
	UpdateHeadBob(walker);
	PlaceCamera(walker);
	if (walker->IsEva())
		UpdateTether(walker);
	else
		m_tether->Clear();
}

void SurfaceViewController::SilenceWalker()
{
	m_jetSound.VolumeAnimate(0.0f, 0.0f, JET_SOUND_FADE, JET_SOUND_FADE);
	m_footsteps.VolumeAnimate(0.0f, 0.0f, STEPS_FADE, STEPS_FADE);
}

void SurfaceViewController::UpdateRoverSound(const Rover *rover, double throttle, bool brake)
{
	const double dt = Pi::game->IsPaused() ? 0.0 : Pi::GetFrameTime();
	const double v = rover->GetWheelSpeed();
	const double speed = std::abs(v) / Rover::MAX_SPEED;
	// the throttle on the way the wheels turn, or away from standing
	const bool pushing = !brake && std::abs(throttle) > 0.1 && (std::abs(v) < 0.5 || throttle * v > 0.0);
	// louder with the throttle
	const float load = pushing ? MOTOR_COAST_VOLUME + (1.0f - MOTOR_COAST_VOLUME) * float(std::min(std::abs(throttle), 1.0)) : MOTOR_COAST_VOLUME;

	if (pushing && !m_roverPushed && speed < ACCEL_FROM && m_roverAccelTime < 0.0) {
		m_roverAccel.Play("rover_accel", 0.0f, 0.0f, 0);
		m_roverAccelTime = 0.0;
	}
	if (!pushing && m_roverPushed && speed > DECEL_FROM && m_roverDecelTime < 0.0) {
		m_roverDecel.Play("rover_decel", 0.0f, 0.0f, 0);
		m_roverDecelTime = 0.0;
	}
	m_roverPushed = pushing;

	// a one-shot on, at the pitch that turns the car's tone there into the
	// rover's; false once it has handed over to the loop
	auto playOneShot = [&](Sound::Event &sound, double &time, double recorded, double length, bool wanted, float volume) {
		if (time < 0.0)
			return false;
		const double pitch = std::max(speed, MOTOR_SLOWEST) / std::max(recorded, MOTOR_SLOWEST);
		if (!wanted || pitch < MOTOR_MIN_PITCH || pitch > MOTOR_MAX_PITCH || time > length - HANDOVER || !sound.IsPlaying()) {
			sound.FadeOut(MOTOR_FADE);
			time = -1.0;
			return false;
		}
		sound.VolumeAnimate(volume, volume, MOTOR_FADE_IN, MOTOR_FADE_IN);
		sound.SetPitch(float(pitch));
		time += dt * pitch;
		return true;
	};
	const bool accel = playOneShot(m_roverAccel, m_roverAccelTime,
		RecordedSpeed(ACCEL_SPEEDS, ACCEL_STEP, m_roverAccelTime), ACCEL_LENGTH, pushing && speed < TOP_SPEED, MOTOR_VOLUME * load);
	const bool decel = playOneShot(m_roverDecel, m_roverDecelTime,
		RecordedSpeed(DECEL_SPEEDS, DECEL_STEP, m_roverDecelTime), DECEL_LENGTH, !pushing, MOTOR_VOLUME * MOTOR_COAST_VOLUME);

	// the loop otherwise, silent with the wheels still
	const float volume = accel || decel ? 0.0f : MOTOR_VOLUME * load * float(std::min(speed / CRUISE_FULL_SPEED, 1.0));
	if (!m_roverCruise.VolumeAnimate(volume, volume, MOTOR_FADE, MOTOR_FADE) && volume > 0.0f) {
		m_roverCruise.Play("rover_cruise", 0.0f, 0.0f, Sound::OP_REPEAT);
		m_roverCruise.VolumeAnimate(volume, volume, MOTOR_FADE, MOTOR_FADE);
	}
	m_roverCruise.SetPitch(float(Clamp(speed, MOTOR_MIN_PITCH, MOTOR_MAX_PITCH)));

	// and the tyres on the ground, at its speed, whatever the motor does
	const double roll = std::abs(rover->GetSpeed()) / Rover::MAX_SPEED / ROLL_RECORDED_SPEED;
	const float rollVolume = ROLL_VOLUME * float(std::min(roll, 1.0)) * float(rover->GetWheelsOnGround()) / 4.0f;
	if (!m_roverRolling.VolumeAnimate(rollVolume, rollVolume, ROLL_FADE, ROLL_FADE) && rollVolume > 0.0f) {
		m_roverRolling.Play("rover_rolling", 0.0f, 0.0f, Sound::OP_REPEAT);
		m_roverRolling.VolumeAnimate(rollVolume, rollVolume, ROLL_FADE, ROLL_FADE);
	}
	m_roverRolling.SetPitch(float(Clamp(roll, MOTOR_MIN_PITCH, MOTOR_MAX_PITCH)));
}

void SurfaceViewController::SilenceRover()
{
	m_roverAccel.FadeOut(MOTOR_FADE);
	m_roverDecel.FadeOut(MOTOR_FADE);
	m_roverCruise.VolumeAnimate(0.0f, 0.0f, MOTOR_FADE, MOTOR_FADE);
	m_roverRolling.VolumeAnimate(0.0f, 0.0f, ROLL_FADE, ROLL_FADE);
	m_roverAccelTime = m_roverDecelTime = -1.0;
	m_roverPushed = false;
}

void SurfaceViewController::PlaceRoverCamera(const Rover *rover)
{
	RefCountedPtr<CameraContext> camera = m_parentView->GetCameraContext();
	camera->SetCameraFrame(rover->GetFrame());

	// in the driving seat, looking round from the rover's heading: the view
	// in the rover's model axes (x right, y up, looking along -z)
	const double cy = std::cos(m_roverYaw);
	const double sy = std::sin(m_roverYaw);
	const double cp = std::cos(m_pitch);
	const double sp = std::sin(m_pitch);
	const vector3d forward(sy * cp, sp, -cy * cp);
	const vector3d right(cy, 0.0, sy);
	const vector3d up = right.Cross(forward);

	const matrix3x3d &orient = rover->GetInterpOrient();
	m_camPos = rover->GetInterpPosition() + orient * rover->GetDriverEye();
	m_camOrient = orient * matrix3x3d::FromVectors(right, up, -forward);
	// a rotation exactly, as the camera's frame is inverted by transposing
	m_camOrient.Renormalize();
	camera->SetCameraPosition(m_camPos);
	camera->SetCameraOrient(m_camOrient);
}

void SurfaceViewController::UpdateTether(const Walker *walker)
{
	const Player *ship = Pi::game->GetPlayer();
	const FrameId root = Pi::game->GetSpace()->GetRootFrame();
	const matrix3x3d toRoot = Frame::GetFrame(walker->GetFrame())->GetInterpOrientRelTo(root);

	// both ends relative to the ship, in root frame axes, as the rope keeps them
	const vector3d shipPos = ship->GetInterpPosition();
	const vector3d anchor = toRoot * (ship->GetInterpOrient() * Pi::game->GetExcursion()->GetHatchPoint());
	const vector3d end = toRoot * (walker->GetInterpPosition() - shipPos + m_camOrient * TETHER_BELT);
	m_tether->Update(anchor, end, Pi::game->IsPaused() ? 0.0 : Pi::GetFrameTime(), walker->IsEvaTetherTaut());

	m_tetherRootToView = m_camOrient.Transpose() * toRoot.Transpose();
	m_tetherShipInView = m_camOrient.Transpose() * (shipPos - m_camPos);
}

void SurfaceViewController::Draw(Camera *camera)
{
	if (!m_tether->IsActive())
		return;
	// the camera's projection, in case drawing the world changed it
	m_parentView->GetCameraContext()->ApplyDrawTransforms(Pi::renderer);
	m_tether->Draw(Pi::renderer, m_tetherRootToView, m_tetherShipInView);
}

void SurfaceViewController::PlaceRoverExternalCamera(const Rover *rover)
{
	RefCountedPtr<CameraContext> camera = m_parentView->GetCameraContext();
	camera->SetCameraFrame(rover->GetFrame());

	// in the rover's model axes: from behind it (+z), round to its right and up
	const double cy = std::cos(m_extYaw);
	const double sy = std::sin(m_extYaw);
	const double cp = std::cos(m_extPitch);
	const double sp = std::sin(m_extPitch);
	const vector3d out(sy * cp, sp, cy * cp);

	const matrix3x3d &orient = rover->GetInterpOrient();
	const vector3d target = rover->GetInterpPosition() + orient * EXTERNAL_TARGET;
	vector3d pos = target + orient * (out * m_extDistance);

	// kept above the terrain
	const Frame *frame = Frame::GetFrame(rover->GetFrame());
	if (frame && frame->GetBody() && frame->GetBody()->IsType(ObjectType::TERRAINBODY)) {
		const vector3d up = pos.Normalized();
		const double ground = static_cast<const TerrainBody *>(frame->GetBody())->GetDrawnTerrainHeight(up) + EXTERNAL_GROUND_CLEARANCE;
		if (pos.Length() < ground)
			pos = up * ground;
	}

	// looking at the rover, upright with the planet
	const vector3d forward = (target - pos).Normalized();
	const vector3d right = forward.Cross(pos.Normalized()).NormalizedSafe();
	const vector3d up = right.Cross(forward);
	m_camPos = pos;
	m_camOrient = matrix3x3d::FromVectors(right, up, -forward);
	camera->SetCameraPosition(m_camPos);
	camera->SetCameraOrient(m_camOrient);
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

void SurfaceViewController::UpdateHeadBob(const Walker *walker)
{
	const double dt = Pi::game->IsPaused() ? 0.0 : Pi::GetFrameTime();
	const double speed = walker->IsOnGround() && !walker->IsEva() ? walker->GetVelocity().Length() : 0.0;
	const double run = Clamp((speed - WALK_PACE) / (RUN_PACE - WALK_PACE), 0.0, 1.0);

	const double step = BOB_WALK_STEP + (BOB_RUN_STEP - BOB_WALK_STEP) * run;
	m_bobPhase = std::fmod(m_bobPhase + speed * dt / step * M_PI, 2.0 * M_PI);

	double height = 0.0;
	if (Pi::config->Int("OnFootHeadBob") != 0 && speed > STEP_MIN_SPEED)
		height = (BOB_WALK_HEIGHT + (BOB_RUN_HEIGHT - BOB_WALK_HEIGHT) * run) * std::min(speed / WALK_PACE, 1.0);
	m_bobHeight += (height - m_bobHeight) * (1.0 - std::exp(-BOB_FADE * dt));
}

void SurfaceViewController::PlaceCamera(const Walker *walker)
{
	RefCountedPtr<CameraContext> camera = m_parentView->GetCameraContext();
	camera->SetCameraFrame(walker->GetFrame());

	// in EVA the walker is all eyes
	if (walker->IsEva()) {
		m_camPos = walker->GetInterpPosition();
		m_camOrient = walker->GetEvaOrient();
		camera->SetCameraPosition(m_camPos);
		camera->SetCameraOrient(m_camOrient);
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

	// a footfall at every pi of the stride
	const vector3d bob = up * (-m_bobHeight * std::cos(2.0 * m_bobPhase)) + right * (BOB_SWAY * m_bobHeight * std::sin(m_bobPhase));

	camera->SetCameraPosition(feet + up * Walker::EYE_HEIGHT + bob);
	camera->SetCameraOrient(matrix3x3d::FromVectors(right, viewUp, -viewForward));
}
