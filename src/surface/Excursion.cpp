// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Excursion.h"

#include "Frame.h"
#include "Game.h"
#include "MathUtil.h"
#include "Player.h"
#include "Rover.h"
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
#include <cfloat>
#include <cmath>
#include <vector>

namespace {
	// room between the hull's bounding box and the walker getting out
	constexpr double EXIT_CLEARANCE = 1.5;
	// the same in space, floating beside the hull
	constexpr double EVA_EXIT_CLEARANCE = 2.5;
	// room between the hull's bounding box and the rover set down beside it
	constexpr double ROVER_CLEARANCE = 3.0;
	// the ground under the rover's footprint rises no more than this (m)
	constexpr double ROVER_SPOT_RISE = 2.0;
	// getting out of the rover: no faster than this (m/s), onto ground no
	// more than this above or below it (m)
	constexpr double ROVER_EXIT_SPEED = 3.0;
	constexpr double ROVER_EXIT_DROP = 2.5;
	// where the walker reaches from, above its feet
	constexpr double REACH_HEIGHT = 1.0;

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

	// v along the ground (perpendicular to up), unit length
	vector3d AlongGround(const vector3d &v, const vector3d &up)
	{
		const vector3d flat = v - up * up.Dot(v);
		if (flat.LengthSqr() > 1e-12)
			return flat.Normalized();
		const vector3d axis = std::abs(up.y) < 0.9 ? vector3d(0.0, 1.0, 0.0) : vector3d(1.0, 0.0, 0.0);
		return (axis - up * up.Dot(axis)).Normalized();
	}

	// how far p (in body's frame) is from body's collision box, 0 inside
	double DistanceToBox(const ModelBody *body, const vector3d &p)
	{
		const vector3d local = body->GetOrient().Transpose() * (p - body->GetPosition());
		const Aabb &box = body->GetAabb();
		const vector3d out(
			std::max({ box.min.x - local.x, 0.0, local.x - box.max.x }),
			std::max({ box.min.y - local.y, 0.0, local.y - box.max.y }),
			std::max({ box.min.z - local.z, 0.0, local.z - box.max.z }));
		return out.Length();
	}
} // namespace

Excursion::Excursion(Game *game) :
	m_game(game)
{
}

Body *Excursion::GetControlledBody() const
{
	if (m_mode == Mode::ROVER && m_rover)
		return m_rover;
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
	ShowSurfaceView();

	LuaEvent::Queue("onPlayerDisembark", player);
	return true;
}

void Excursion::ShowSurfaceView()
{
	// time acceleration stays at 1x off the ship (Game::UpdateTimeAccel)
	SetShipControlsEnabled(false);
	WorldView *worldView = m_game->GetWorldView();
	worldView->surfaceView->ResetLook();
	worldView->SetViewController(worldView->surfaceView.get());
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
	StowRover();

	LuaEvent::Queue("onPlayerBoard", player);
	return true;
}

void Excursion::StowRover()
{
	if (!m_rover)
		return;
	m_game->GetSpace()->KillBody(m_rover);
	m_rover = nullptr;
}

bool Excursion::FindRoverSpot(vector3d &pos, vector3d &forward) const
{
	const Player *ship = m_game->GetPlayer();
	const Frame *frame = PlanetFrame(ship);
	if (!frame || !Rover::IsModelAvailable())
		return false;
	const TerrainBody *terrain = static_cast<const TerrainBody *>(frame->GetBody());

	const Aabb &aabb = ship->GetAabb();
	const vector3d centre = (aabb.min + aabb.max) * 0.5;
	// the rover's footprint about its origin, which is not its middle
	const Aabb rover = Rover::GetModelAabb();
	const double halfWidth = std::max(-rover.min.x, rover.max.x);
	const double halfLength = std::max(-rover.min.z, rover.max.z);

	// in the ship's model space: alongside it right or left, or behind or
	// ahead of it facing away
	struct Spot {
		vector3d at;
		vector3d forward;
	};
	const Spot spots[] = {
		{ vector3d(aabb.max.x + ROVER_CLEARANCE + halfWidth, centre.y, centre.z), vector3d(0.0, 0.0, -1.0) },
		{ vector3d(aabb.min.x - ROVER_CLEARANCE - halfWidth, centre.y, centre.z), vector3d(0.0, 0.0, -1.0) },
		{ vector3d(centre.x, centre.y, aabb.max.z + ROVER_CLEARANCE + halfLength), vector3d(0.0, 0.0, 1.0) },
		{ vector3d(centre.x, centre.y, aabb.min.z - ROVER_CLEARANCE - halfLength), vector3d(0.0, 0.0, -1.0) },
	};

	const vector3d shipPos = ship->GetPosition();
	const double shipTop = shipPos.Length() + aabb.max.y;
	for (const Spot &spot : spots) {
		const vector3d p = shipPos + ship->GetOrient() * spot.at;
		const vector3d up = p.Normalized();
		const vector3d ahead = AlongGround(ship->GetOrient() * spot.forward, up);
		const vector3d right = ahead.Cross(up);

		// the corners of its footprint on dry, open ground (nothing over them
		// from above the ship's top down), and not too uneven
		bool clear = true;
		double low = DBL_MAX;
		double high = -DBL_MAX;
		for (const double across : { -halfWidth, halfWidth }) {
			for (const double along : { -halfLength, halfLength }) {
				const vector3d cornerUp = (p + right * across + ahead * along).Normalized();
				const vector3d onTerrain = cornerUp * terrain->GetTerrainHeight(cornerUp);
				const double above = std::max(shipTop - onTerrain.Length() + 2.0, 3.0);
				const Body *hit = nullptr;
				const double ground = Walker::GroundRadius(frame, onTerrain, above, 1.0, &hit);
				if (hit || Walker::IsSea(terrain, cornerUp, ground))
					clear = false;
				low = std::min(low, ground);
				high = std::max(high, ground);
			}
		}
		if (!clear || high - low > ROVER_SPOT_RISE)
			continue;

		pos = up * terrain->GetTerrainHeight(up);
		forward = ahead;
		return true;
	}
	return false;
}

bool Excursion::CanDeployRover() const
{
	const Player *player = m_game->GetPlayer();
	if (IsOffShip() || !m_game->IsNormalSpace() || !player || player->IsDead() || player->GetFlightState() != Ship::LANDED)
		return false;
	if (player->Properties().Get("surface_rover_cap").get_integer() < 1)
		return false;

	vector3d pos, forward;
	return FindRoverSpot(pos, forward);
}

bool Excursion::DeployRover()
{
	vector3d pos, forward;
	if (!CanDeployRover() || !FindRoverSpot(pos, forward))
		return false;

	Player *player = m_game->GetPlayer();
	m_rover = new Rover();
	m_rover->SetFrame(player->GetFrame());
	m_rover->PlaceOnGround(pos, forward);
	m_rover->SetDriven(true);
	m_game->GetSpace()->AddBody(m_rover);
	m_mode = Mode::ROVER;
	ShowSurfaceView();

	LuaEvent::Queue("onPlayerDisembark", player);
	return true;
}

bool Excursion::CanEnterRover() const
{
	if (m_mode != Mode::ON_FOOT || !m_walker || !m_rover || m_walker->GetFrame() != m_rover->GetFrame())
		return false;
	const vector3d feet = m_walker->GetPosition();
	return DistanceToBox(m_rover, feet + feet.Normalized() * REACH_HEIGHT) <= ROVER_REACH;
}

bool Excursion::EnterRover()
{
	if (!CanEnterRover())
		return false;

	m_game->GetSpace()->KillBody(m_walker);
	m_walker = nullptr;
	m_rover->SetDriven(true);
	m_mode = Mode::ROVER;
	m_game->GetWorldView()->surfaceView->ResetLook();
	return true;
}

bool Excursion::FindRoverExit(vector3d &pos, vector3d &facing) const
{
	if (!m_rover)
		return false;
	const Frame *frame = Frame::GetFrame(m_rover->GetFrame());
	if (!frame || !frame->GetBody() || !frame->GetBody()->IsType(ObjectType::TERRAINBODY))
		return false;
	const TerrainBody *terrain = static_cast<const TerrainBody *>(frame->GetBody());

	// beside the door, else the other side: dry ground, not under the rover
	// or anything else, and not far above or below it
	const vector3d roverPos = m_rover->GetPosition();
	const double roverGround = roverPos.Length();
	for (int side = 0; side < 2; side++) {
		const vector3d p = roverPos + m_rover->GetOrient() * m_rover->GetExitPoint(side);
		const vector3d up = p.Normalized();
		const vector3d onTerrain = up * terrain->GetTerrainHeight(up);
		const double above = std::max(roverGround + m_rover->GetAabb().max.y - onTerrain.Length(), Walker::STEP_HEIGHT);
		const Body *hit = nullptr;
		const double ground = Walker::GroundRadius(frame, onTerrain, above, 1.0, &hit);
		if (hit == m_rover || Walker::IsSea(terrain, up, ground) || std::abs(ground - roverGround) > ROVER_EXIT_DROP)
			continue;

		pos = up * ground;
		facing = p - roverPos;
		return true;
	}
	return false;
}

bool Excursion::CanExitRover() const
{
	if (m_mode != Mode::ROVER || !m_rover || std::abs(m_rover->GetSpeed()) > ROVER_EXIT_SPEED)
		return false;
	vector3d pos, facing;
	return FindRoverExit(pos, facing);
}

bool Excursion::ExitRover()
{
	vector3d pos, facing;
	if (!CanExitRover() || !FindRoverExit(pos, facing))
		return false;

	m_rover->SetDriven(false);
	m_walker = new Walker();
	m_walker->SetFrame(m_rover->GetFrame());
	m_walker->PlaceOnGround(pos, facing);
	m_game->GetSpace()->AddBody(m_walker);
	m_mode = Mode::ON_FOOT;
	m_game->GetWorldView()->surfaceView->ResetLook();
	return true;
}

Excursion::Action Excursion::GetAvailableAction() const
{
	if (m_mode == Mode::ROVER)
		return CanExitRover() ? Action::EXIT_ROVER : Action::NONE;

	const bool board = CanBoard();
	const bool enter = CanEnterRover();
	if (board && enter) {
		// the nearer of the two
		const vector3d feet = m_walker->GetPosition();
		const vector3d chest = feet + feet.Normalized() * REACH_HEIGHT;
		return DistanceToBox(m_game->GetPlayer(), chest) <= DistanceToBox(m_rover, chest) ? Action::BOARD : Action::ENTER_ROVER;
	}
	if (board)
		return Action::BOARD;
	return enter ? Action::ENTER_ROVER : Action::NONE;
}

Excursion::Action Excursion::Interact()
{
	const Action action = GetAvailableAction();
	bool done = false;
	switch (action) {
	case Action::BOARD: done = Board(); break;
	case Action::ENTER_ROVER: done = EnterRover(); break;
	case Action::EXIT_ROVER: done = ExitRover(); break;
	case Action::NONE: break;
	}
	return done ? action : Action::NONE;
}

bool Excursion::GetNavInfo(NavInfo &info) const
{
	// on the surface, on foot or driving; no north in space
	const Body *me = nullptr;
	vector3d forward;
	if (m_mode == Mode::ON_FOOT && m_walker) {
		me = m_walker;
		forward = m_walker->GetForward();
	} else if (m_mode == Mode::ROVER && m_rover) {
		me = m_rover;
		forward = m_rover->GetForward();
	} else {
		return false;
	}

	const vector3d pos = me->GetInterpPosition();
	const vector3d up = pos.Normalized();
	info.heading = Bearing(forward, up);

	const vector3d toShip = m_game->GetPlayer()->GetInterpPositionRelTo(me->GetFrame()) - pos;
	info.shipDistance = toShip.Length();
	info.shipBearing = Bearing(toShip, up);

	info.hasRover = m_mode == Mode::ON_FOOT && m_rover;
	info.roverBearing = info.roverDistance = 0.0;
	if (info.hasRover) {
		const vector3d toRover = m_rover->GetInterpPositionRelTo(me->GetFrame()) - pos;
		info.roverDistance = toRover.Length();
		info.roverBearing = Bearing(toRover, up);
	}
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
