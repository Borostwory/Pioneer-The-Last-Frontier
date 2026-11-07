// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "DeathView.h"

#include "Camera.h"
#include "Frame.h"
#include "Game.h"
#include "GameConfig.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Player.h"
#include "TerrainBody.h"
#include "graphics/Graphics.h"
#include "graphics/Renderer.h"

#include "profiler/Profiler.h"

#include <cmath>

namespace {
	// on a planet: backing off this far above the horizon, and kept this
	// many metres above the ground as drawn
	constexpr double DEATH_CAMERA_ELEVATION = DEG2RAD(30.0);
	constexpr double DEATH_CAMERA_GROUND_CLEARANCE = 2.0;
} // namespace

DeathView::DeathView(Game *game) :
	View("DeathView"),
	m_game(game)
{
	float znear;
	float zfar;
	Pi::renderer->GetNearFarRange(znear, zfar);

	const float fovY = Pi::config->Float("FOVVertical");
	m_cameraContext.Reset(new CameraContext(Pi::renderer->GetWindowWidth(), Pi::renderer->GetWindowHeight(), fovY, znear, zfar));
	m_camera.reset(new Camera(m_cameraContext, Pi::renderer));
}

DeathView::~DeathView() {}

void DeathView::Init()
{
	// the ship, or the player out of it (Excursion: killed on foot or in EVA)
	const Body *body = m_game->GetControlledBody();
	m_cameraDist = body->GetClipRadius() * 5.0;
	m_cameraContext->SetCameraFrame(body->GetFrame());
	m_cameraContext->SetCameraOrient(matrix3x3d::Identity);

	// in space: off along the frame's z, looking back down it. On a planet
	// that would be into the ground on half of it, so off and up from the
	// ground there instead, the same way round the planet as z
	m_cameraDir = vector3d(0.0, 0.0, 1.0);
	m_terrain = nullptr;
	const Frame *frame = Frame::GetFrame(body->GetFrame());
	if (frame && frame->IsRotFrame() && frame->GetBody() && frame->GetBody()->IsType(ObjectType::TERRAINBODY)) {
		m_terrain = static_cast<const TerrainBody *>(frame->GetBody());
		const vector3d up = body->GetInterpPosition().NormalizedSafe();
		vector3d level = m_cameraDir - up * up.Dot(m_cameraDir);
		if (level.LengthSqr() < 1e-6)
			level = vector3d(1.0, 0.0, 0.0) - up * up.x;
		level = level.Normalized();
		m_cameraDir = up * std::sin(DEATH_CAMERA_ELEVATION) + level * std::cos(DEATH_CAMERA_ELEVATION);
	}

	PlaceCamera();
}

void DeathView::PlaceCamera()
{
	const vector3d target = m_game->GetControlledBody()->GetInterpPosition();
	vector3d pos = target + m_cameraDir * double(m_cameraDist);
	if (!m_terrain) {
		m_cameraContext->SetCameraPosition(pos);
		return;
	}

	// kept above the ground; well above the highest ground there is, no need
	const vector3d up = pos.NormalizedSafe();
	if (pos.Length() < m_terrain->GetMaxFeatureRadius() + 1000.0) {
		const double ground = m_terrain->GetDrawnTerrainHeight(up) + DEATH_CAMERA_GROUND_CLEARANCE;
		if (pos.Length() < ground)
			pos = up * ground;
	}
	m_cameraContext->SetCameraPosition(pos);

	// looking at the body, upright with the planet; straight above it, as
	// it was turned before
	const vector3d forward = (target - pos).NormalizedSafe();
	const vector3d right = forward.Cross(up);
	if (right.LengthSqr() < 1e-12)
		return;
	const vector3d r = right.Normalized();
	m_cameraContext->SetCameraOrient(matrix3x3d::FromVectors(r, r.Cross(forward), -forward));
}

void DeathView::OnSwitchTo()
{
}

void DeathView::Update()
{
	assert(Pi::player->IsDead());

	m_cameraDist += 160.0 * Pi::GetFrameTime();
	PlaceCamera();
	m_cameraContext->BeginFrame();
	m_camera->Update();
}

void DeathView::Draw3D()
{
	PROFILE_SCOPED()
	m_cameraContext->ApplyDrawTransforms(Pi::renderer);
	m_camera->Draw();
	m_cameraContext->EndFrame();
}
