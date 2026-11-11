// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "CinematicCamera.h"

#include "Frame.h"
#include "Game.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Quaternion.h"
#include "Ship.h"
#include "TerrainBody.h"
#include "core/Log.h"

#include <SDL_keycode.h>
#include <SDL_mouse.h>

#include <algorithm>
#include <cmath>

namespace {
	// radians the view turns per pixel the mouse moves
	constexpr double LOOK_PER_PIXEL = 0.0025;
	// radians a second for tilting with Q / E
	constexpr double ROLL_SPEED = 1.0;
	// how much faster with Shift held
	constexpr double BOOST = 10.0;
	// metres the camera keeps above the ground as drawn
	constexpr double GROUND_CLEARANCE = 1.0;

	constexpr double MIN_SPEED = 0.1;
	constexpr double MAX_SPEED = 1e9;
} // namespace

CinematicCamera::CinematicCamera(RefCountedPtr<CameraContext> camera, const Ship *ship) :
	m_camera(camera),
	m_ship(ship),
	m_keysFrame(Pi::input, true, "CinematicCamera"),
	m_pos(0.0),
	m_orient(matrix3x3d::Identity),
	m_vel(0.0),
	m_up(0.0, 1.0, 0.0),
	m_lastShipPos(0.0)
{
}

CinematicCamera::~CinematicCamera()
{
	// the game is closing: the ship may be gone already, so only let go of
	// the keys, the mouse and the panels
	if (!m_on)
		return;
	m_on = false;
	UpdateInputFrame();
	SetMouseLook(false);
	if (m_hidingHud)
		Pi::DrawGUI = m_savedDrawGui;
}

void CinematicCamera::Start()
{
	if (m_on)
		return;

	m_space = Pi::game ? Pi::game->GetSpace() : nullptr;
	m_frame = m_camera->GetCameraFrame();
	if (!m_frame.valid())
		m_frame = m_ship->GetFrame();
	m_pos = m_camera->GetCameraPos();
	m_orient = m_camera->GetCameraOrient();
	m_orient.Renormalize();
	m_vel = vector3d(0.0);
	m_up = m_orient.VectorY();
	m_pendingYaw = m_pendingPitch = m_pendingRoll = 0.0;
	m_haveLastShipPos = false;
	m_placed = false;
	m_anchor = nullptr;
	m_lastFlightState = int(m_ship->GetFlightState());

	m_savedFov = m_camera->GetFovAng();
	settings.fov = m_savedFov;

	m_on = true;
	UpdateInputFrame();

	Log::Info("Cinematic camera: detached in frame {}, {:.1f} m from the ship, ship flight state {}",
		GetFrameLabel(), GetDistanceToShip(), int(m_ship->GetFlightState()));
}

void CinematicCamera::Stop()
{
	if (!m_on)
		return;
	Log::Info("Cinematic camera: back on the ship (was {:.1f} m from it, in frame {})", GetDistanceToShip(), GetFrameLabel());
	m_on = false;

	UpdateInputFrame();
	SetMouseLook(false);

	if (m_camera->GetFovAng() != m_savedFov)
		m_camera->SetFovAng(m_savedFov);

	if (m_hidingHud) {
		Pi::DrawGUI = m_savedDrawGui;
		m_hidingHud = false;
	}
}

void CinematicCamera::OnActivated()
{
	if (m_on)
		Log::Info("Cinematic camera: the ship's 3D view is shown again");
	m_shown = true;
	UpdateInputFrame();
}

void CinematicCamera::OnDeactivated()
{
	if (m_on)
		Log::Info("Cinematic camera: the ship's 3D view is hidden (another screen is up)");
	m_shown = false;
	UpdateInputFrame();
	SetMouseLook(false);
}

void CinematicCamera::UpdateInputFrame()
{
	const bool want = m_on && m_shown && settings.keysMoveCamera;
	const bool has = Pi::input->HasInputFrame(&m_keysFrame);
	if (want && !has)
		Pi::input->AddInputFrame(&m_keysFrame);
	else if (!want && has)
		Pi::input->RemoveInputFrame(&m_keysFrame);
}

void CinematicCamera::SetMouseLook(bool on)
{
	if (on == m_mouseLook)
		return;
	m_mouseLook = on;
	Pi::input->SetCapturingMouse(on);
}

void CinematicCamera::ChangeSpeed(bool faster)
{
	settings.speed = Clamp(settings.speed * (faster ? 1.25 : 0.8), MIN_SPEED, MAX_SPEED);
}

double CinematicCamera::GetDistanceToShip() const
{
	if (!m_on || !m_frame.valid())
		return 0.0;
	return (m_ship->GetInterpPositionRelTo(m_frame) - m_pos).Length();
}

std::string CinematicCamera::GetFrameLabel() const
{
	const Frame *frame = m_on ? Frame::GetFrame(m_frame) : nullptr;
	return frame ? frame->GetLabel() : std::string();
}

void CinematicCamera::SwitchToFrame(FrameId to)
{
	const Frame *frame = Frame::GetFrame(m_frame);
	const matrix3x3d orient = frame->GetInterpOrientRelTo(to);
	const vector3d pos = frame->GetInterpPositionRelTo(to);

	m_pos = orient * m_pos + pos;
	m_orient = orient * m_orient;
	m_vel = orient * m_vel;
	m_up = orient * m_up;
	m_lastShipPos = orient * m_lastShipPos + pos;
	m_frame = to;
	m_anchor = nullptr;

	Log::Info("Cinematic camera: moved from frame {} into frame {}, {:.1f} m from the ship",
		frame->GetLabel(), GetFrameLabel(), GetDistanceToShip());
}

// the same rule a ship follows: out of a frame past its edge, into a child
// frame once inside it
void CinematicCamera::UpdateFrame()
{
	const Frame *frame = Frame::GetFrame(m_frame);

	if (frame->GetRadius() < m_pos.Length() && frame->GetParent().valid()) {
		SwitchToFrame(frame->GetParent());
		return;
	}

	for (FrameId kid : frame->GetChildren()) {
		const Frame *kidFrame = Frame::GetFrame(kid);
		const vector3d pos = frame->GetInterpOrientRelTo(kid) * m_pos + frame->GetInterpPositionRelTo(kid);
		if (pos.Length() < kidFrame->GetRadius()) {
			SwitchToFrame(kid);
			return;
		}
	}
}

// "up" near a planet or star is away from its middle; elsewhere it is the
// camera's own up from when it was started
vector3d CinematicCamera::LocalUp() const
{
	const Frame *frame = Frame::GetFrame(m_frame);
	const Body *body = frame ? frame->GetBody() : nullptr;
	if (body && body->IsType(ObjectType::TERRAINBODY) && m_pos.LengthSqr() > 1.0)
		return m_pos.Normalized();
	return m_up.NormalizedSafe();
}

// the ground isn't in the collision space, so it is checked here
void CinematicCamera::KeepAboveGround()
{
	if (!settings.stopAtGround)
		return;
	const Frame *frame = Frame::GetFrame(m_frame);
	if (!frame || !frame->IsRotFrame() || !frame->GetBody() || !frame->GetBody()->IsType(ObjectType::TERRAINBODY))
		return;
	const TerrainBody *terrain = static_cast<const TerrainBody *>(frame->GetBody());

	const double r = m_pos.Length();
	if (r > terrain->GetMaxFeatureRadius() + 1000.0 || r < 1.0)
		return;

	const vector3d up = m_pos / r;
	const double ground = terrain->GetDrawnTerrainHeight(up) + GROUND_CLEARANCE;
	if (r >= ground)
		return;
	if (ground - r > 5.0)
		Log::Info("Cinematic camera: lifted {:.1f} m out of the ground", ground - r);
	m_pos = up * ground;
	// stop sinking into it
	const double down = m_vel.Dot(up);
	if (down < 0.0)
		m_vel -= up * down;
}

// A space station's area of space holds still against the stars, but the
// station spins in it (and the area moves along the station's orbit with it).
// Near one, the camera is carried round with the station by as much as it
// turned and moved since last frame, so it keeps its place in or by the station.
void CinematicCamera::RideWithStation()
{
	const Frame *frame = Frame::GetFrame(m_frame);
	const Body *station = frame ? frame->GetBody() : nullptr;
	if (!settings.rideWithStation || !station || !station->IsType(ObjectType::SPACESTATION)) {
		m_anchor = nullptr;
		return;
	}

	const matrix3x3d orient = station->GetInterpOrientRelTo(m_frame);
	const vector3d pos = station->GetInterpPositionRelTo(m_frame);
	if (m_anchor == station) {
		const matrix3x3d turn = orient * m_anchorOrient.Transpose();
		m_pos = turn * (m_pos - m_anchorPos) + pos;
		m_orient = turn * m_orient;
		m_vel = turn * m_vel;
		m_up = turn * m_up;
		// so travelling with the ship only adds how the ship moved against the station
		m_lastShipPos = turn * (m_lastShipPos - m_anchorPos) + pos;
	}
	m_anchor = station;
	m_anchorOrient = orient;
	m_anchorPos = pos;
}

// turn the camera so its sides are level, keeping where it looks
void CinematicCamera::Level()
{
	const vector3d up = LocalUp();
	const vector3d forward = -m_orient.VectorZ();
	vector3d right = forward.Cross(up);
	if (right.LengthSqr() < 1e-8)
		return; // looking straight up or down
	right = right.Normalized();
	m_orient = matrix3x3d::FromVectors(right, right.Cross(forward), -forward);
}

bool CinematicCamera::Update(float frameTime)
{
	if (!m_on)
		return true;

	// a new star system: the frames it was in are gone
	if (!Pi::game || Pi::game->IsHyperspace() || Pi::game->GetSpace() != m_space || !Frame::GetFrame(m_frame)) {
		Log::Info("Cinematic camera: the ship left the star system");
		Stop();
		return false;
	}

	UpdateInputFrame();

	// ---- checks for the log: did anything else move the game's camera, and did the ship dock or undock
	if (m_placed) {
		const bool otherFrame = m_camera->GetCameraFrame() != m_placedFrame;
		const double moved = (m_camera->GetCameraPos() - m_placedPos).Length();
		const double turned = (m_camera->GetCameraOrient().VectorZ() - m_placedOrient.VectorZ()).Length();
		if (otherFrame || moved > 0.01 || turned > 0.001) {
			const Frame *f = Frame::GetFrame(m_camera->GetCameraFrame());
			Log::Info("Cinematic camera: something else moved the game's camera: frame {} -> {}, moved {:.2f} m, turned {:.3f}",
				GetFrameLabel(), f ? f->GetLabel() : std::string("none"), moved, turned);
		}
	}
	if (int(m_ship->GetFlightState()) != m_lastFlightState) {
		m_lastFlightState = int(m_ship->GetFlightState());
		const Frame *shipFrame = Frame::GetFrame(m_ship->GetFrame());
		Log::Info("Cinematic camera: ship flight state now {} (ship in frame {}), camera {:.1f} m from it in frame {}",
			m_lastFlightState, shipFrame ? shipFrame->GetLabel() : std::string("none"), GetDistanceToShip(), GetFrameLabel());
	}

	const double dt = Clamp(double(frameTime), 0.0, 0.25);
	// how far to settle toward what is wanted this frame
	const double settle = settings.smoothing > 0.0 ? 1.0 - std::exp(-dt / settings.smoothing) : 1.0;
	const bool keys = settings.keysMoveCamera && m_shown;

	// ---- the keys: W/S forward and back, A/D sideways, R/F up and down, Q/E tilt
	vector3d move(0.0);
	double roll = 0.0;
	if (keys) {
		if (Pi::input->IsKeyDown(SDLK_w)) move.z -= 1.0;
		if (Pi::input->IsKeyDown(SDLK_s)) move.z += 1.0;
		if (Pi::input->IsKeyDown(SDLK_d)) move.x += 1.0;
		if (Pi::input->IsKeyDown(SDLK_a)) move.x -= 1.0;
		if (Pi::input->IsKeyDown(SDLK_r)) move.y += 1.0;
		if (Pi::input->IsKeyDown(SDLK_f)) move.y -= 1.0;
		if (Pi::input->IsKeyDown(SDLK_q)) roll += 1.0;
		if (Pi::input->IsKeyDown(SDLK_e)) roll -= 1.0;
	}

	// ---- the mouse: hold the right button to look about
	SetMouseLook(keys && Pi::input->MouseButtonState(SDL_BUTTON_RIGHT));
	if (m_mouseLook && !settings.lookAtShip) {
		int motion[2];
		Pi::input->GetMouseMotion(motion);
		m_pendingYaw -= motion[0] * LOOK_PER_PIXEL;
		m_pendingPitch -= motion[1] * LOOK_PER_PIXEL;
	}
	if (!settings.levelHorizon)
		m_pendingRoll += roll * ROLL_SPEED * dt;

	// ---- near a space station, go round with it
	RideWithStation();

	// ---- turn, easing in the mouse and keys
	{
		const double yaw = m_pendingYaw * settle;
		const double pitch = m_pendingPitch * settle;
		const double tilt = m_pendingRoll * settle;
		m_pendingYaw -= yaw;
		m_pendingPitch -= pitch;
		m_pendingRoll -= tilt;

		const vector3d yawAxis = settings.levelHorizon ? LocalUp() : m_orient.VectorY();
		if (yaw != 0.0)
			m_orient = matrix3x3d::Rotate(yaw, yawAxis) * m_orient;
		if (pitch != 0.0) {
			const matrix3x3d before = m_orient;
			m_orient = matrix3x3d::Rotate(pitch, m_orient.VectorX()) * m_orient;
			// level: no turning over the top
			if (settings.levelHorizon && std::abs(m_orient.VectorZ().Dot(LocalUp())) > 0.995) {
				m_orient = before;
				m_pendingPitch = 0.0;
			}
		}
		if (tilt != 0.0)
			m_orient = matrix3x3d::Rotate(tilt, m_orient.VectorZ()) * m_orient;
	}

	// ---- move
	const vector3d startPos = m_pos;
	const FrameId startFrame = m_frame;
	vector3d wantVel(0.0);
	if (move.LengthSqr() > 0.0) {
		const vector3d up = settings.levelHorizon ? LocalUp() : m_orient.VectorY();
		const vector3d dir = m_orient.VectorX() * move.x + up * move.y + m_orient.VectorZ() * move.z;
		const bool boost = Pi::input->KeyState(SDLK_LSHIFT) || Pi::input->KeyState(SDLK_RSHIFT);
		wantVel = dir.NormalizedSafe() * settings.speed * (boost ? BOOST : 1.0);
	}
	m_vel += (wantVel - m_vel) * settle;
	m_pos += m_vel * dt;

	// ---- travel with the ship: move by as much as it moved
	const vector3d shipPos = m_ship->GetInterpPositionRelTo(m_frame);
	if (settings.travelWithShip && m_haveLastShipPos)
		m_pos += shipPos - m_lastShipPos;
	m_lastShipPos = shipPos;
	m_haveLastShipPos = true;

	UpdateFrame();
	KeepAboveGround();

	// for the log: a big step in one frame that the keys didn't ask for
	if (m_frame == startFrame && !settings.travelWithShip) {
		const double step = (m_pos - startPos).Length();
		if (step > std::max(100.0, 3.0 * settings.speed * BOOST * dt))
			Log::Info("Cinematic camera: jumped {:.1f} m in one frame in frame {}", step, GetFrameLabel());
	}

	// ---- face the ship
	if (settings.lookAtShip) {
		const vector3d target = m_ship->GetInterpPositionRelTo(m_frame);
		if ((target - m_pos).LengthSqr() > 1e-6) {
			const vector3d up = settings.levelHorizon ? LocalUp() : m_orient.VectorY();
			const matrix3x3d want = MathUtil::LookAt(m_pos, target, up);
			const Quaterniond from = Quaterniond::FromMatrix3x3(m_orient);
			const Quaterniond to = Quaterniond::FromMatrix3x3(want);
			m_orient = Quaterniond::Slerp(from, to, settle).ToMatrix3x3<double>();
		}
	} else if (settings.levelHorizon) {
		Level();
	}
	m_orient.Renormalize();

	// ---- place the game's camera
	m_camera->SetCameraFrame(m_frame);
	m_camera->SetCameraPosition(m_pos);
	m_camera->SetCameraOrient(m_orient);
	m_placedFrame = m_frame;
	m_placedPos = m_pos;
	m_placedOrient = m_orient;
	m_placed = true;
	settings.fov = Clamp(settings.fov, 5.0f, 150.0f);
	if (m_camera->GetFovAng() != settings.fov)
		m_camera->SetFovAng(settings.fov);

	// ---- the on-screen panels
	if (settings.hideHud && !m_hidingHud) {
		m_savedDrawGui = Pi::DrawGUI;
		m_hidingHud = true;
	}
	if (m_hidingHud) {
		if (settings.hideHud) {
			Pi::DrawGUI = false;
		} else {
			Pi::DrawGUI = m_savedDrawGui;
			m_hidingHud = false;
		}
	}

	return true;
}
