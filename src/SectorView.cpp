// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "SectorView.h"

#include "AnimationCurves.h"
#include "Game.h"
#include "GameSaveError.h"
#include "Input.h"
#include "Json.h"
#include "JsonUtils.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Player.h"
#include "RefCounted.h"
#include "SectorMap.h"
#include "SectorMapContext.h"
#include "Space.h"
#include "core/Log.h"
#include "galaxy/Galaxy.h"
#include "galaxy/Sector.h"
#include "galaxy/StarSystem.h"
#include "imgui/imgui.h"
#include "lua/LuaObject.h"
#include "lua/LuaRef.h"
#include "lua/LuaTable.h"
#include "matrix4x4.h"

#include "profiler/Profiler.h"

#include <cassert>
#include <cmath>

SectorView::~SectorView() {}

using namespace Graphics;

enum DetailSelection {
	DETAILBOX_NONE = 0,
	DETAILBOX_INFO = 1,
	DETAILBOX_FACTION = 2
};

REGISTER_INPUT_BINDING(SectorView)
{
	using namespace InputBindings;
	auto *mapView = input->GetBindingPage("MapControls");
	Input::BindingGroup *group;

#define BINDING_GROUP(n) group = mapView->GetBindingGroup(#n);
#define KEY_BINDING(name, k1, k2) \
	input->AddActionBinding(name, group, InputBindings::Action({ k1 }, { k2 }));
#define AXIS_BINDING(name, k1, k2) \
	input->AddAxisBinding(name, group, InputBindings::Axis({}, { k1 }, { k2 }));

	BINDING_GROUP(SectorMapViewControls)
	KEY_BINDING("BindMapToggleSelectionFollowView", SDLK_RETURN, SDLK_KP_ENTER)
	KEY_BINDING("BindMapWarpToCurrentSystem", SDLK_c, 0)
	KEY_BINDING("BindMapWarpToSelectedSystem", SDLK_g, 0)

#undef KEY_BINDING
#undef AXIS_BINDING
#undef BINDING_GROUP
}

void SectorView::InputBinding::RegisterBindings()
{
	mapViewReset = AddAction("BindResetOrientationAndZoom");
	mapToggleSelectionFollowView = AddAction("BindMapToggleSelectionFollowView");
	mapWarpToCurrent = AddAction("BindMapWarpToCurrentSystem");
	mapWarpToSelected = AddAction("BindMapWarpToSelectedSystem");
}

// callbacks for the SectorMap
class SectorView::SectorMapCallbacks : public SectorMapContext::Callbacks {
	SectorView &sv;

public:
	SectorMapCallbacks(SectorView &sv) :
		sv(sv) {}

	void OnClickLabel(const SystemPath &clickedLabel) override
	{
		sv.SwitchToPath(clickedLabel);
	}

	SectorMapContext::DisplayMode GetDisplayMode(const SystemPath &system) override
	{
		if (system.IsSameSystem(sv.m_selected)) return DisplayModes::ALWAYS;
		if (system.IsSameSystem(sv.m_current)) return DisplayModes::ALWAYS;
		// always show systems that are in route
		if (std::find_if(sv.m_route.begin(), sv.m_route.end(), [system](const SystemPath &a) { return system.IsSameSystem(a); }) != sv.m_route.end())
			return DisplayModes::ALWAYS;

		// with the route highlighted, everything else is just a small grey star
		if (sv.m_routeFocus && !sv.m_route.empty())
			return DisplayModes::HIDE_LABEL | DisplayModes::DIM;

		// hide labels outside of hyperjump range if option is set
		RefCountedPtr<const Sector> sec1 = sv.m_game.GetGalaxy()->GetSector(system);
		RefCountedPtr<const Sector> sec2 = sv.m_game.GetGalaxy()->GetSector(sv.m_current);
		float dist = Sector::DistanceBetween(sec1, system.systemIndex, sec2, sv.m_current.systemIndex);

		if (dist > sv.m_playerHyperspaceRange) {
			if (sv.m_drawOutRangeLabels) {
				return DisplayModes::SHADOW_LABEL;
			} else {
				return DisplayModes::HIDE_LABEL;
			}
		}
		return DisplayModes::DEFAULT;
	}
};

SectorMapContext SectorView::CreateMapContext()
{
	SectorMapContext context;
	context.galaxy = m_game.GetGalaxy();
	context.input = Pi::input;
	context.config = Pi::config;
	context.renderer = Pi::renderer;
	context.callbacks = std::make_unique<SectorView::SectorMapCallbacks>(*this);
	context.pigui = Pi::pigui;
	return context;
}

SectorView::SectorView(Game *game) :
	View("SectorView"),
	InputBindings(Pi::input),
	m_game(*game),
	m_map(new SectorMap(CreateMapContext()))
{
	InitDefaults();

	m_inSystem = true;

	RefCountedPtr<StarSystem> system = game->GetSpace()->GetStarSystem();
	m_current = system->GetPath();
	assert(!m_current.IsSectorPath());
	m_current = m_current.SystemOnly();
	m_selected = m_hyperspaceTarget = system->GetStars()[0]->GetPath();

	m_map->GotoSystem(m_current);

	m_automaticSystemSelection = true;
	m_detailBoxVisible = DETAILBOX_INFO;

	InitObject();
}

SectorView::SectorView(const Json &jsonObj, Game *game) :
	View("SectorView"),
	InputBindings(Pi::input),
	m_game(*game)
{
	InitDefaults();

	try {
		Json sectorViewObj = jsonObj["sector_view"];

		m_inSystem = sectorViewObj["in_system"];
		m_current = SystemPath::FromJson(sectorViewObj["current"]);
		m_selected = SystemPath::FromJson(sectorViewObj["selected"]);
		m_hyperspaceTarget = SystemPath::FromJson(sectorViewObj["hyperspace"]);
		m_automaticSystemSelection = sectorViewObj["automatic_system_selection"];
		m_map.reset(new SectorMap(sectorViewObj["map"], CreateMapContext()));
	} catch (Json::type_error &) {
		throw SavedGameCorruptException();
	}

	InitObject();
}

void SectorView::InitDefaults()
{
	InputBindings.RegisterBindings();
	m_drawRouteLines = true; // where should this go?!
	m_route = std::vector<SystemPath>();
}

void SectorView::SetDrawRouteLines(bool value)
{
	m_drawRouteLines = value;
	m_setupLines = true;
}

void SectorView::InitObject()
{
	// single keystroke handlers
	m_onToggleSelectionFollowView =
		InputBindings.mapToggleSelectionFollowView->onPressed.connect([&]() {
			m_automaticSystemSelection = !m_automaticSystemSelection;
		});
	m_onWarpToCurrent =
		InputBindings.mapWarpToCurrent->onPressed.connect([&]() {
			m_map->GotoSystem(m_current);
		});
	m_onWarpToSelected =
		InputBindings.mapWarpToSelected->onPressed.connect([&]() {
			m_map->GotoSystem(m_selected);
		});
	m_onViewReset =
		InputBindings.mapViewReset->onPressed.connect([&]() {
			m_map->ResetView();
		});
}

void SectorView::SaveToJson(Json &jsonObj)
{
	Json sectorViewObj({}); // Create JSON object to contain sector view data.

	sectorViewObj["in_system"] = m_inSystem;

	Json currentSystemObj({}); // Create JSON object to contain current system data.
	m_current.ToJson(currentSystemObj);
	sectorViewObj["current"] = currentSystemObj; // Add current system object to sector view object.

	Json selectedSystemObj({}); // Create JSON object to contain selected system data.
	m_selected.ToJson(selectedSystemObj);
	sectorViewObj["selected"] = selectedSystemObj; // Add selected system object to sector view object.

	Json hyperspaceSystemObj({}); // Create JSON object to contain hyperspace system data.
	m_hyperspaceTarget.ToJson(hyperspaceSystemObj);
	sectorViewObj["hyperspace"] = hyperspaceSystemObj; // Add hyperspace system object to sector view object.
	sectorViewObj["automatic_system_selection"] = m_automaticSystemSelection;

	Json sectorMapObj{};
	m_map->SaveToJson(sectorMapObj);
	sectorViewObj["map"] = sectorMapObj;

	jsonObj["sector_view"] = sectorViewObj; // Add sector view object to supplied object.
}

void SectorView::Draw3D()
{
	PROFILE_SCOPED()

	matrix4x4f modelview = m_map->PointOfView();
	modelview.Translate(-m_map->GetPosition() * Sector::SIZE);

	// calculate the player's location (it will be interpolated between systems during a hyperjump)
	vector3f playerPos;
	float currentStarSize;
	GetPlayerPosAndStarSize(playerPos, currentStarSize);

	// player location indicator
	matrix4x4f trans = modelview * matrix4x4f::Translation(playerPos);
	// prior to modelview transformation, rotate in the opposite direction so
	// that the billboard is always facing the camera
	matrix4x4f rot = modelview;
	rot.ClearToRotOnly();
	rot = rot.Inverse();
	// move this disk 0.03 light years further so that it does not overlap the star, and selected indicator and hyperspace target indicator
	m_map->AddStarBillboard(trans * rot, vector3f(0.f, 0.f, -0.03f), Color(0, 0, 204), 1.5f);

	// selected indicator
	trans = modelview * matrix4x4f::Translation(m_map->GetSystemPosition(m_selected));
	// move this disk 0.01 light years further so that it does not overlap the star
	m_map->AddStarBillboard(trans * rot, vector3f(0.f, 0.f, -0.01), Color(0, 204, 0), 1.0);

	// hyperspace target indicator
	if (m_hyperspaceTarget != m_selected && (!m_inSystem || m_hyperspaceTarget != m_current)) {
		trans = modelview * matrix4x4f::Translation(m_map->GetSystemPosition(m_hyperspaceTarget));
		// move this disk 0.02 light years further so that it does not overlap the star, and selected indicator
		m_map->AddStarBillboard(trans * rot, vector3f(0.f, 0.f, -0.02), Color(77, 77, 77), 1.0);
	}

	// all the lines
	if (m_setupLines) {
		SetupLines(playerPos, modelview);
	}

	// hyperspace range sphere
	if (m_playerHyperspaceRange > 0.0f) {
		const matrix4x4f sphTrans = modelview * matrix4x4f::Translation(playerPos);
		m_map->AddSphere(sphTrans * matrix4x4f::ScaleMatrix(m_playerHyperspaceRange));
	}

	// actually rendering
	m_map->Draw3D();
}

void SectorView::DrawUIContents()
{
	m_map->DrawLabels(ImGui::IsWindowHovered());
}

void SectorView::SetHyperspaceTarget(const SystemPath &path)
{
	if (m_hyperspaceTarget != path) {
		m_hyperspaceTarget = path;
		onHyperspaceTargetChanged.emit();
	}
}

void SectorView::ResetHyperspaceTarget()
{
	SystemPath old = m_hyperspaceTarget;
	m_hyperspaceTarget = m_game.GetSpace()->GetStarSystem()->GetStars()[0]->GetPath();

	if (!old.IsSameSystem(m_hyperspaceTarget)) {
		onHyperspaceTargetChanged.emit();
	}
}

void SectorView::SetSelectedPath(const SystemPath &path)
{
	if (path.IsBodyPath())
		m_selected = path;
	else if (path.IsSystemPath()) {
		RefCountedPtr<StarSystem> system = m_game.GetGalaxy()->GetStarSystem(path);
		m_selected = CheckPathInRoute(system->GetStars()[0]->GetPath());
	}
	m_setupLines = true;
}

void SectorView::SwitchToPath(const SystemPath &path)
{
	SetSelectedPath(path);
	if (m_automaticSystemSelection)
		m_map->GotoSystem(path);
}

// check the route, maybe this system is there, then we'll take the path from the route
const SystemPath &SectorView::CheckPathInRoute(const SystemPath &path)
{
	for (const auto &p : m_route)
		if (p.IsSameSystem(path))
			return p;
	return path;
}

bool SectorView::MoveRouteItemUp(const std::vector<SystemPath>::size_type element)
{
	if (element == 0 || element >= m_route.size()) return false;

	std::swap(m_route[element - 1], m_route[element]);

	m_setupLines = true;
	return true;
}

bool SectorView::MoveRouteItemDown(const std::vector<SystemPath>::size_type element)
{
	if (element >= m_route.size() - 1) return false;

	std::swap(m_route[element + 1], m_route[element]);

	m_setupLines = true;
	return true;
}

void SectorView::UpdateRouteItem(const std::vector<SystemPath>::size_type element, const SystemPath &path)
{
	m_route[element] = path;
	m_setupLines = true;
}

void SectorView::AddToRoute(const SystemPath &path)
{
	m_route.push_back(path);
	m_setupLines = true;
}

bool SectorView::RemoveRouteItem(const std::vector<SystemPath>::size_type element)
{
	if (element < m_route.size()) {
		m_route.erase(m_route.begin() + element);
		m_setupLines = true;
		return true;
	} else {
		return false;
	}
}

void SectorView::ClearRoute()
{
	m_route.clear();
	m_setupLines = true;
}

const std::vector<SystemPath> &SectorView::GetRoute() const
{
	return m_route;
}

const std::string SectorView::AutoRoute(const SystemPath &start, const SystemPath &target, std::vector<SystemPath> &outRoute) const
{
	PROFILE_SCOPED()
	LuaRef try_hdrive = LuaObject<Player>::CallMethod<LuaRef>(Pi::player, "GetInstalledHyperdrive");
	if (try_hdrive.IsNil())
		return "NO_DRIVE";
	const ScopedTable hyperdrive = ScopedTable(try_hdrive);
	const float max_range = hyperdrive.CallMethod<float>("GetMaximumRange", Pi::player);

	// A jump's duration (HyperdriveType:GetDuration) is distance^1.6 times a
	// factor that is the same for every jump of this ship, so the route with
	// the least total distance^1.6 is also the quickest one.
	std::vector<SystemPath> route;
	if (!m_game.GetGalaxy()->FindJumpRoute(start, target, max_range, route))
		return "NO_VALID_ROUTE";

	outRoute.reserve(route.size());
	for (const SystemPath &path : route)
		outRoute.push_back(m_game.GetGalaxy()->GetStarSystem(path)->GetStars()[0]->GetPath());
	// end at the given body in multistar systems
	if (!outRoute.empty())
		outRoute.back().bodyIndex = target.bodyIndex;
	return "OKAY";
}

void SectorView::SetupLines(const vector3f &playerAbsPos, const matrix4x4f &trans)
{
	assert(m_setupLines);
	m_setupLines = false;

	m_map->ClearLineVerts();

	// line to selected system
	if (!m_current.IsSameSystem(m_selected)) {
		m_map->AddLineVert(m_map->GetSystemPosition(m_selected), Color::BLANK);
		m_map->AddLineVert(playerAbsPos, Color::WHITE);
	}

	if (!m_drawRouteLines || m_route.empty()) return;

	// line from the end of the route to the selected
	const SystemPath &routeBack = m_route.back();
	if (!routeBack.IsSameSystem(m_selected)) {
		m_map->AddLineVert(m_map->GetSystemPosition(m_selected), Color::BLANK);
		m_map->AddLineVert(m_map->GetSystemPosition(routeBack), Color::WHITE);
	}

	vector3f startPos = playerAbsPos;
	for (std::vector<SystemPath>::size_type i = 0; i < m_route.size(); ++i) {
		const vector3f jumpAbsPos = m_map->GetSystemPosition(m_route[i]);
		if (i > 0) {
			startPos = m_map->GetSystemPosition(m_route[i - 1]);
		}

		m_map->AddLineVert(startPos, Color(20, 20, 0, 127));
		m_map->AddLineVert(jumpAbsPos, Color(255, 255, 0, 255));
	}
}

void SectorView::GetPlayerPosAndStarSize(vector3f &playerPosOut, float &currentStarSizeOut)
{
	// calculate the player's location (it will be interpolated between systems during a hyperjump)
	const Sector::System currentSystem = m_map->GetCached(m_current)->m_systems[m_current.systemIndex];
	playerPosOut = Sector::SIZE * vector3f(float(m_current.sectorX), float(m_current.sectorY), float(m_current.sectorZ)) + currentSystem.GetPosition();
	currentStarSizeOut = StarSystem::starScale[currentSystem.GetStarType(0)];
	if (!m_inSystem) {
		// we are in hyperspace, interpolate
		// m_current has been set to the destination path
		// destination system data:
		const SystemPath dstPath = m_game.GetHyperspaceDest();
		const Sector::System dstSystem = m_map->GetCached(dstPath)->m_systems[dstPath.systemIndex];
		const vector3f dstPos = Sector::SIZE * vector3f(float(dstPath.sectorX), float(dstPath.sectorY), float(dstPath.sectorZ)) + dstSystem.GetPosition();
		// jumpProcess: 0.0f (at source) .. 1.0f (at destination)
		// smoothing speed at the ends with cubic interpolation
		const float jumpProcess = AnimationCurves::InOutCubicEasing(m_game.GetHyperspaceProgress() / m_game.GetHyperspaceDuration());
		// player location indicator's size of the star, on which it is drawn
		// so we interpolate it too
		const float dstStarSize = StarSystem::starScale[dstSystem.GetStarType(0)];
		// interpolating player location indicator's position and size
		playerPosOut = playerPosOut.Lerp(dstPos, jumpProcess);
		currentStarSizeOut = MathUtil::Lerp(currentStarSizeOut, dstStarSize, jumpProcess);
	}
}

void SectorView::OnSwitchTo()
{
	InputBindings.manager->AddInputFrame(&InputBindings);
	InputBindings.manager->AddInputFrame(&m_map->InputBindings);
	Update();
}

void SectorView::OnSwitchFrom()
{
	InputBindings.manager->RemoveInputFrame(&InputBindings);
	InputBindings.manager->RemoveInputFrame(&m_map->InputBindings);
}

void SectorView::Update()
{
	PROFILE_SCOPED()
	SystemPath last_current = m_current;

	m_map->Update(Pi::GetFrameTime());

	if (m_game.IsNormalSpace()) {
		m_inSystem = true;
		m_current = m_game.GetSpace()->GetStarSystem()->GetPath();
	} else {
		m_inSystem = false;
		m_current = m_game.GetHyperspaceSource();
	}

	if (m_automaticSystemSelection && m_map->IsManualMove()) {
		SystemPath new_selected = m_map->NearestSystemToPos(m_map->GetPosition());
		if (new_selected.IsSystemPath() && !m_selected.IsSameSystem(new_selected)) {
			RefCountedPtr<StarSystem> system = m_game.GetGalaxy()->GetStarSystem(new_selected);
			SetSelectedPath(CheckPathInRoute(system->GetStars()[0]->GetPath()));
		}
	}

	m_playerHyperspaceRange = LuaObject<Player>::CallMethod<float>(Pi::player, "GetHyperspaceRange");
}

void SectorView::ResetView()
{
	SwitchToPath(m_current);
	m_map->ResetView();
}

void SectorView::GotoCurrentSystem() { m_map->GotoSystem(m_current); }
void SectorView::GotoSelectedSystem() { m_map->GotoSystem(m_selected); }
void SectorView::GotoHyperspaceTarget() { m_map->GotoSystem(m_hyperspaceTarget); }
