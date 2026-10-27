// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Excursion.h"

#include "Frame.h"
#include "Game.h"
#include "MathUtil.h"
#include "Player.h"
#include "Space.h"
#include "SurfaceViewController.h"
#include "TerrainBody.h"
#include "Walker.h"
#include "WorldView.h"
#include "lua/LuaEvent.h"
#include "scenegraph/Model.h"
#include "scenegraph/Tag.h"
#include "ship/PlayerShipController.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
	// room between the hull's bounding box and the walker getting out
	constexpr double EXIT_CLEARANCE = 1.5;
	// the same in space, floating beside the hull
	constexpr double EVA_EXIT_CLEARANCE = 2.5;

	// the frame a landed ship sits in: a planet's rotating frame, or null
	const Frame *PlanetFrame(const Body *body)
	{
		const Frame *f = Frame::GetFrame(body->GetFrame());
		if (!f || !f->GetBody() || !f->GetBody()->IsType(ObjectType::PLANET) || f->GetBody()->GetFrame() != body->GetFrame())
			return nullptr;
		return f;
	}

	// degrees clockwise from north of v along the ground at up; north is
	// along the planet's spin axis, +y of its rotating frame
	double Bearing(const vector3d &v, const vector3d &up)
	{
		vector3d north = vector3d(0.0, 1.0, 0.0) - up * up.y;
		if (north.LengthSqr() < 1e-12) // at a pole
			north = vector3d(0.0, 0.0, -1.0) - up * up.z;
		north = north.Normalized();
		const vector3d east = north.Cross(up);
		const double deg = RAD2DEG(std::atan2(v.Dot(east), v.Dot(north)));
		return deg < 0.0 ? deg + 360.0 : deg;
	}
} // namespace

Excursion::Excursion(Game *game) :
	m_game(game)
{
}

Body *Excursion::GetControlledBody() const
{
	if (IsOffShip() && m_walker)
		return m_walker;
	return m_game->GetPlayer();
}

bool Excursion::FindExitPoint(vector3d &pos, vector3d &facing) const
{
	const Player *ship = m_game->GetPlayer();
	const Frame *frame = PlanetFrame(ship);
	if (!frame)
		return false;
	const TerrainBody *terrain = static_cast<const TerrainBody *>(frame->GetBody());

	const Aabb &aabb = ship->GetAabb();
	const vector3d centre = (aabb.min + aabb.max) * 0.5;
	const double clear = Walker::RADIUS + EXIT_CLEARANCE;

	std::vector<vector3d> spots; // in model space
	if (const SceneGraph::Tag *hatch = ship->GetModel()->FindTagByName("tag_hatch"))
		spots.push_back(vector3d(hatch->GetGlobalTransform().GetTranslate()));
	spots.push_back(vector3d(aabb.max.x + clear, centre.y, centre.z)); // right
	spots.push_back(vector3d(aabb.min.x - clear, centre.y, centre.z)); // left
	spots.push_back(vector3d(centre.x, centre.y, aabb.max.z + clear)); // behind
	spots.push_back(vector3d(centre.x, centre.y, aabb.min.z - clear)); // ahead (ships look along -z)

	const vector3d shipPos = ship->GetPosition();
	const double shipTop = shipPos.Length() + aabb.max.y;
	for (const vector3d &spot : spots) {
		const vector3d p = shipPos + ship->GetOrient() * spot;
		const vector3d up = p.Normalized();
		const vector3d onTerrain = up * terrain->GetTerrainHeight(up);

		// looking down from above the ship's top: if the ship is the first
		// thing hit, the spot is under the hull
		const double above = std::max(shipTop - onTerrain.Length() + 2.0, Walker::STEP_HEIGHT);
		const Body *hit = nullptr;
		const double ground = Walker::GroundRadius(frame, onTerrain, above, 1.0, &hit);
		if (hit == ship || Walker::IsSea(terrain, up, ground))
			continue;

		pos = up * ground;
		facing = p - shipPos;
		return true;
	}
	return false;
}

vector3d Excursion::GetHatchPoint() const
{
	const Player *ship = m_game->GetPlayer();
	if (const SceneGraph::Tag *hatch = ship->GetModel()->FindTagByName("tag_hatch"))
		return vector3d(hatch->GetGlobalTransform().GetTranslate());
	const Aabb &aabb = ship->GetAabb();
	return vector3d(aabb.max.x, 0.5 * (aabb.min.y + aabb.max.y), 0.5 * (aabb.min.z + aabb.max.z));
}

bool Excursion::CanStartEva() const
{
	const Player *player = m_game->GetPlayer();
	if (player->GetFlightState() != Ship::FLYING || player->AIIsActive())
		return false;
	const Frame *f = Frame::GetFrame(player->GetFrame());
	return f && !f->IsRotFrame();
}

bool Excursion::CanDisembark() const
{
	const Player *player = m_game->GetPlayer();
	if (IsOffShip() || !m_game->IsNormalSpace() || !player || player->IsDead() || player->IsHyperspaceActive())
		return false;
	if (CanStartEva())
		return true;
	if (player->GetFlightState() != Ship::LANDED || !PlanetFrame(player))
		return false;

	vector3d pos, facing;
	return FindExitPoint(pos, facing);
}

void Excursion::StartEva()
{
	Player *player = m_game->GetPlayer();

	// beside the hull, in root frame axes as the walker keeps them
	const Aabb &aabb = player->GetAabb();
	const vector3d centre = (aabb.min + aabb.max) * 0.5;
	const vector3d local(aabb.max.x + Walker::RADIUS + EVA_EXIT_CLEARANCE, centre.y, centre.z);
	const FrameId root = m_game->GetSpace()->GetRootFrame();
	const matrix3x3d shipToRoot = Frame::GetFrame(player->GetFrame())->GetOrientRelTo(root) * player->GetOrient();
	const vector3d relPos = shipToRoot * local;

	// looking back at the ship, upright with it
	const vector3d forward = (shipToRoot * centre - relPos).Normalized();
	vector3d up = shipToRoot.VectorY();
	up = (up - forward * forward.Dot(up)).Normalized();
	const matrix3x3d orient = matrix3x3d::FromVectors(forward.Cross(up), up, -forward);

	m_walker = new Walker();
	m_walker->SetFrame(player->GetFrame());
	m_walker->StartEva(player, relPos, orient, GetHatchPoint());

	// left alone, the ship coasts
	if (PlayerShipController *controller = player->GetPlayerController())
		controller->SetFlightControlState(CONTROL_MANUAL);
	player->ClearThrusterState();
}

bool Excursion::Disembark()
{
	if (!CanDisembark())
		return false;

	Player *player = m_game->GetPlayer();
	if (player->GetFlightState() == Ship::LANDED) {
		vector3d pos, facing;
		if (!FindExitPoint(pos, facing))
			return false;
		m_walker = new Walker();
		m_walker->SetFrame(player->GetFrame());
		m_walker->PlaceOnGround(pos, facing);
		m_mode = Mode::ON_FOOT;
	} else {
		StartEva();
		m_mode = Mode::EVA;
	}
	m_game->GetSpace()->AddBody(m_walker);

	// time acceleration stays at 1x off the ship (Game::UpdateTimeAccel)
	SetShipControlsEnabled(false);
	WorldView *worldView = m_game->GetWorldView();
	worldView->surfaceView->ResetLook();
	worldView->SetViewController(worldView->surfaceView.get());

	LuaEvent::Queue("onPlayerDisembark", player);
	return true;
}

bool Excursion::CanBoard() const
{
	if (!IsOffShip() || !m_walker)
		return false;
	const Player *ship = m_game->GetPlayer();
	if (!ship || ship->IsDead() || m_walker->GetFrame() != ship->GetFrame())
		return false;

	// the walker in the ship's model space, against its bounding box
	const vector3d local = ship->GetOrient().Transpose() * (m_walker->GetPosition() - ship->GetPosition());
	const Aabb &aabb = ship->GetAabb();
	const double d = BOARD_DISTANCE;
	return local.x > aabb.min.x - d && local.x < aabb.max.x + d &&
		local.y > aabb.min.y - d && local.y < aabb.max.y + d &&
		local.z > aabb.min.z - d && local.z < aabb.max.z + d;
}

bool Excursion::Board()
{
	if (!CanBoard())
		return false;

	Player *player = m_game->GetPlayer();
	m_mode = Mode::SHIP;
	WorldView *worldView = m_game->GetWorldView();
	worldView->SetViewController(worldView->shipView.get());
	SetShipControlsEnabled(true);

	m_game->GetSpace()->KillBody(m_walker);
	m_walker = nullptr;

	LuaEvent::Queue("onPlayerBoard", player);
	return true;
}

Excursion::Action Excursion::GetAvailableAction() const
{
	return CanBoard() ? Action::BOARD : Action::NONE;
}

bool Excursion::GetNavInfo(double &heading, double &shipBearing, double &shipDistance) const
{
	// no north in space
	if (m_mode != Mode::ON_FOOT || !m_walker)
		return false;

	const vector3d pos = m_walker->GetInterpPosition();
	const vector3d up = pos.Normalized();
	heading = Bearing(m_walker->GetForward(), up);

	const vector3d toShip = m_game->GetPlayer()->GetInterpPositionRelTo(m_walker->GetFrame()) - pos;
	shipDistance = toShip.Length();
	shipBearing = Bearing(toShip, up);
	return true;
}

void Excursion::OnShipMoved()
{
	if (m_mode == Mode::EVA && m_walker)
		m_walker->FollowShip();
}

void Excursion::OnShipInterpolated(double alpha)
{
	if (m_mode == Mode::EVA && m_walker)
		m_walker->FollowShipInterp(alpha);
}

void Excursion::SetShipControlsEnabled(bool enabled)
{
	if (PlayerShipController *controller = m_game->GetPlayer()->GetPlayerController())
		controller->SetInputEnabled(enabled);
}
