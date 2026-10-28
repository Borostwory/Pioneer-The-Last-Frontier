// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Ship.h"

#include "CargoBody.h"
#include "EnumStrings.h"
#include "Frame.h"
#include "Game.h"
#include "GameSaveError.h"
#include "HyperspaceCloud.h"
#include "JsonUtils.h"
#include "Missile.h"
#include "NavLights.h"
#include "Pi.h"
#include "Planet.h"
#include "galaxy/SystemBody.h"
#include "Player.h" // <-- Here only for 1 occurrence of "Pi::player" in Ship::Explode
#include "Sensors.h"
#include "Sfx.h"
#include "Shields.h"
#include "ShipAICmd.h"
#include "Space.h"
#include "SpaceStation.h"
#include "WorldView.h"
#include "collider/CollisionContact.h"
#include "collider/CollisionSpace.h"
#include "collider/GeomTree.h"
#include "gameconsts.h"
#include "graphics/RenderState.h"
#include "graphics/Renderer.h"
#include "graphics/TextureBuilder.h"
#include "graphics/Types.h"
#include "graphics/VertexBuffer.h"
#include "lua/LuaEvent.h"
#include "lua/LuaObject.h"
#include "scenegraph/Animation.h"
#include "scenegraph/MatrixTransform.h"
#include "scenegraph/Model.h"
#include "scenegraph/Tag.h"
#include "ThrusterExhaust.h"
#include "scenegraph/CollisionGeometry.h"
#include "scenegraph/NodeVisitor.h"
#include "scenegraph/StaticGeometry.h"
#include "ship/GunManager.h"
#include "ship/PlayerShipController.h"
#include "surface/Excursion.h"
#include "surface/Walker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

static const float TONS_HULL_PER_SHIELD = 10.f;
// A landed ship eases onto the drawn ground no faster than this (m/s) and
// turns to its slope no faster than this (radians per second), leaning no
// more than LANDED_MAX_TILT. Its contacts are the collision mesh's vertices
// within LANDED_CONTACT_BAND of its bottom, the farthest one in each of
// LANDED_CONTACT_SECTORS directions round the footprint.
static const double LANDED_SETTLE_SPEED = 2.0;
static const double LANDED_TURN_SPEED = 0.5;
static const double LANDED_MAX_TILT = 0.35; // 20 degrees
static const double LANDED_CONTACT_BAND = 0.3;
static const int LANDED_CONTACT_SECTORS = 8;
// the ground under a contact is looked for from this far above it (m), down
// to this far below
static const double LANDED_PROBE_ABOVE = 2.0;
static const double LANDED_PROBE_BELOW = 10.0;
// it lands only with the ground or a structure this close under its bottom
// (m): not on touching a wall in flight
static const double LANDED_STRUCTURE_GAP = 2.0;
const float Ship::DEFAULT_SHIELD_COOLDOWN_TIME = 1.0f;
const double Ship::DEFAULT_LIFT_TO_DRAG_RATIO = 0.001;

namespace {
	static constexpr size_t s_heatingNormalParam = "heatingNormal"_hash;
	static constexpr size_t s_heatGradientTexParam = "heatGradient"_hash;

	static constexpr size_t s_reentryFlightParam = "ReentryFlight"_hash;
	static constexpr size_t s_reentryTimeParam = "ReentryTime"_hash;

	static RefCountedPtr<Graphics::Material> s_reentryGlowTemplate;

	bool TryGetLandingPadDustOverride(const Ship *ship, double *outGroundRadius, Color *outDustTint)
	{
		PROFILE_SCOPED()

		if (!ship || !outGroundRadius || !outDustTint || !Pi::game) return false;
		Space *space = Pi::game->GetSpace();
		if (!space) return false;

		// Keep this local query cheap - we only need stations close enough to plausibly be beneath the ship.
		constexpr double PAD_QUERY_RADIUS = 2000.0;
		constexpr double PAD_MAX_VERTICAL_ABOVE_M = 350.0;
		constexpr double PAD_MIN_VERTICAL_BELOW_M = -20.0;
		constexpr double PAD_BASE_LATERAL_RADIUS_M = 120.0;

		const Space::BodyNearList nearby = space->GetBodiesMaybeNear(ship, PAD_QUERY_RADIUS);
		bool foundPad = false;
		double bestPadDistSq = std::numeric_limits<double>::max();
		double bestPadRadius = 0.0;
		for (Body *body : nearby) {
			if (!body || !body->IsType(ObjectType::SPACESTATION)) continue;
			SpaceStation *station = static_cast<SpaceStation *>(body);
			if (!station->IsGroundStation()) continue;
			if (station->GetFrame() != ship->GetFrame()) continue;

			const SpaceStationType *stationType = station->GetStationType();
			if (!stationType) continue;

			for (Uint32 bay = 0; bay < stationType->NumDockingPorts(); ++bay) {
				const matrix4x4f bayDocked = stationType->GetStageTransform(int(bay), DockStage::DOCKED);
				const vector3d bayLocal = vector3d(bayDocked.GetTranslate());
				const vector3d bayWorld = station->GetPosition() + station->GetOrient() * bayLocal;
				const vector3d padUp = bayWorld.NormalizedSafe();

				const vector3d shipRel = ship->GetPosition() - bayWorld;
				const double vertical = shipRel.Dot(padUp);
				if (vertical < PAD_MIN_VERTICAL_BELOW_M || vertical > PAD_MAX_VERTICAL_ABOVE_M)
					continue;

				const vector3d lateralVec = shipRel - padUp * vertical;
				double padLateralRadius = PAD_BASE_LATERAL_RADIUS_M;
				if (const SpaceStationType::SPort *port = stationType->FindPortByBay(int(bay)))
					padLateralRadius = std::max(padLateralRadius, 0.5 * double(port->maxShipSize));

				const double lateralDistSq = lateralVec.LengthSqr();
				if (lateralDistSq > padLateralRadius * padLateralRadius)
					continue;

				if (lateralDistSq < bestPadDistSq) {
					bestPadDistSq = lateralDistSq;
					bestPadRadius = bayWorld.Length();
					foundPad = true;
				}
			}
		}
		if (!foundPad) return false;

		*outGroundRadius = bestPadRadius;
		// Concrete or landing pad dust colour. We make this lower alpha because it's less intense than a dusty planet surface.
		*outDustTint = Color(128, 128, 128, 63);
		return true;
	}
} // namespace

struct ReentryGlowSetupVisitor : SceneGraph::NodeVisitor {
	Ship *ship;
	Graphics::Renderer *renderer;
	ReentryGlowSetupVisitor(Ship *s, Graphics::Renderer *r) :
		ship(s), renderer(r) {}
	void ApplyStaticGeometry(SceneGraph::StaticGeometry &sg) override
	{
		ship->m_reentryGlowGeoms.push_back(&sg);
		sg.SetNodeMask(SceneGraph::NODE_TRANSPARENT);
		for (Uint32 i = 0; i < sg.GetNumMeshes(); ++i) {
			SceneGraph::StaticGeometry::Mesh &mesh = sg.GetMeshAt(i);
			mesh.material = ship->FindOrCreateReentryGlowMats(renderer, mesh.meshObject->GetFormat())->frontShellPass;
		}
	}
};

Ship::Ship(const ShipType::Id &shipId) :
	DynamicBody(),
	m_controller(0),
	m_flightState(FLYING),
	m_alertState(ALERT_NONE),
	m_landingGearAnimation(nullptr)
{
	/*
		THIS CODE DOES NOT RUN WHEN LOADING SAVEGAMES!!
	*/
	m_propulsion = AddComponent<Propulsion>();
	m_shields = AddComponent<Shields>();
	m_gunManager = AddComponent<GunManager>();
	Properties().Set("flightState", EnumStrings::GetString("ShipFlightState", m_flightState));
	Properties().Set("alertStatus", EnumStrings::GetString("ShipAlertStatus", m_alertState));

	SetFuel(1.0);
	SetFuelReserve(0.0);
	m_latestSpawnTime = 0.0;
	m_lastAlertUpdate = 0.0;
	m_lastFiringAlert = 0.0;
	m_shipNear = false;
	m_shipFiring = false;
	m_missileDetected = false;
	m_lastDamageSound = 0.f;

	m_testLanded = false;
	m_launchLockTimeout = 0;
	m_wheelTransition = 0;
	m_wheelState = 0;
	m_forceWheelUpdate = false;
	m_dockedWith = nullptr;
	m_dockedWithPort = 0;
	SetShipId(shipId);
	ClearAngThrusterState();
	ClearLinThrusterState();

	m_hyperspace.countdown = 0;
	m_hyperspace.now = false;
	m_ecmRecharge = 0;
	m_shieldCooldown = 0.0f;
	m_curAICmd = 0;
	m_aiMessage = AIERROR_NONE;
	m_decelerating = false;

	SetModel(m_type->modelName.c_str());
	SetupShields();

	// Setting thrusters colors
	if (m_type->isGlobalColorDefined) GetModel()->SetThrusterColor(m_type->globalThrusterColor);
	for (int i = 0; i < THRUSTER_MAX; i++) {
		if (!m_type->isDirectionColorDefined[i]) continue;
		vector3f dir;
		switch (i) {
		case THRUSTER_FORWARD: dir = vector3f(0.0, 0.0, 1.0); break;
		case THRUSTER_REVERSE: dir = vector3f(0.0, 0.0, -1.0); break;
		case THRUSTER_LEFT: dir = vector3f(1.0, 0.0, 0.0); break;
		case THRUSTER_RIGHT: dir = vector3f(-1.0, 0.0, 0.0); break;
		case THRUSTER_UP: dir = vector3f(1.0, 0.0, 0.0); break;
		case THRUSTER_DOWN: dir = vector3f(-1.0, 0.0, 0.0); break;
		}
		GetModel()->SetThrusterColor(dir, m_type->directionThrusterColor[i]);
	}
	SetLabel("UNLABELED_SHIP");
	m_skin.SetRandomColors(Pi::rng);
	m_skin.SetDecal(m_type->manufacturer);
	m_skin.Apply(GetModel());
	if (GetModel()->SupportsPatterns())
		GetModel()->SetPattern(Pi::rng.Int32(0, GetModel()->GetNumPatterns() - 1));

	Init();
	SetController(new ShipController());
}

Ship::Ship(const Json &jsonObj, Space *space) :
	DynamicBody(jsonObj, space)
{
	m_propulsion = AddComponent<Propulsion>();
	m_shields = AddComponent<Shields>();
	m_gunManager = AddComponent<GunManager>();

	try {
		Json shipObj = jsonObj["ship"];

		m_propulsion->LoadFromJson(shipObj, space);

		SetShipId(shipObj["ship_type_id"]); // XXX handle missing thirdparty ship
		m_propulsion->SetFuelTankMass(GetShipType()->fuelTankMass);
		m_stats.fuel_tank_mass_left = m_propulsion->FuelTankMassLeft();

		m_skin.LoadFromJson(shipObj);
		m_skin.Apply(GetModel());
		// needs fixups
		m_wheelTransition = shipObj["wheel_transition"];
		m_wheelState = shipObj["wheel_state"];
		m_forceWheelUpdate = true;
		SetLightsLevel(shipObj.value("lights_level", 0));
		m_launchLockTimeout = shipObj["launch_lock_timeout"];
		m_testLanded = shipObj["test_landed"];
		m_flightState = shipObj["flight_state"];

		m_lastAlertUpdate = 0.0;   // alertstate check cache timer
		m_shipNear = false;		   // alertstate check cache value
		m_shipFiring = false;	   // alertstate check cache value
		m_missileDetected = false; // alertstate check cache value
		m_lastDamageSound = 0.f;

		m_alertState = shipObj["alert_state"];
		m_lastFiringAlert = shipObj["last_firing_alert"];

		Json hyperspaceDestObj = shipObj["hyperspace_destination"];
		m_hyperspace.dest = SystemPath::FromJson(hyperspaceDestObj);
		m_hyperspace.countdown = shipObj["hyperspace_countdown"];
		m_hyperspace.duration = 0;
		m_hyperspace.sounds.warmup_sound = shipObj.value("hyperspace_warmup_sound", "");
		m_hyperspace.sounds.abort_sound = shipObj.value("hyperspace_abort_sound", "");
		m_hyperspace.sounds.jump_sound = shipObj.value("hyperspace_jump_sound", "");
		m_hyperspace.sounds.travel_sound = shipObj.value("hyperspace_travel_sound", "Hyperspace_Travel");
		m_hyperspace.sounds.exit_sound = shipObj.value("hyperspace_exit_sound", "Hyperspace_Exit");

		m_ecmRecharge = shipObj["ecm_recharge"];
		SetShipId(shipObj["ship_type_id"]); // XXX handle missing thirdparty ship
		m_dockedWithPort = shipObj["docked_with_port"];
		m_dockedWithIndex = shipObj["index_for_body_docked_with"];
		Init();
		m_stats.hull_mass_left = shipObj["hull_mass_left"]; // must be after Init()...
		m_stats.shield_mass_left = shipObj["shield_mass_left"];
		m_shieldCooldown = shipObj["shield_cooldown"];
		m_curAICmd = 0;
		m_curAICmd = AICommand::LoadFromJson(shipObj);
		m_aiMessage = AIError(shipObj["ai_message"]);

		m_latestSpawnTime = 0.0;

		// NOTE: needs to happen before shield data is loaded from JSON
		SetupShields();

		PropertyMap &p = Properties();
		Properties().Set("flightState", EnumStrings::GetString("ShipFlightState", m_flightState));
		Properties().Set("alertStatus", EnumStrings::GetString("ShipAlertStatus", m_alertState));

		p.Set("hullMassLeft", m_stats.hull_mass_left);
		p.Set("hullPercent", 100.0f * (m_stats.hull_mass_left / float(m_type->hullMass)));
		p.Set("shieldMassLeft", m_stats.shield_mass_left);
		p.Set("fuelMassLeft", m_stats.fuel_tank_mass_left);

		m_controller = 0;
		const ShipController::Type ctype = shipObj["controller_type"];
		if (ctype == ShipController::PLAYER)
			SetController(new PlayerShipController());
		else
			SetController(new ShipController());
		m_controller->LoadFromJson(shipObj);

		m_navLights->LoadFromJson(shipObj);

		m_shields->LoadFromJson(shipObj["shields"]);

		m_shipName = shipObj["name"].get<std::string>();
		Properties().Set("shipName", m_shipName);
	} catch (Json::type_error &) {
		throw SavedGameCorruptException();
	}
}

Ship::~Ship()
{
	if (m_curAICmd) delete m_curAICmd;
	delete m_controller;
}

void Ship::Init()
{
	m_invulnerable = false;

	m_sensors.reset(new Sensors(this));

	m_navLights.reset(new NavLights(GetModel()));
	m_navLights->SetEnabled(true);

	SetMassDistributionFromModel();
	UpdateEquipStats();
	m_stats.hull_mass_left = float(m_type->hullMass);
	m_stats.shield_mass_left = 0;

	PropertyMap &p = Properties();
	p.Set("hullMassLeft", m_stats.hull_mass_left);
	p.Set("hullPercent", 100.0f * (m_stats.hull_mass_left / float(m_type->hullMass)));
	p.Set("shieldMassLeft", m_stats.shield_mass_left);
	p.Set("fuelMassLeft", m_stats.fuel_tank_mass_left);

	// Init of GunManager
	m_gunManager->Init(this);

	// Init of Propulsion:
	m_propulsion->Init(this, GetModel(), m_type->fuelTankMass, m_type->effectiveExhaustVelocity, m_type->linThrust, m_type->angThrust, m_type->linAccelerationCap);

	p.Set("shipName", m_shipName);

	m_hyperspace.now = false; // TODO: move this on next savegame change, maybe
	m_hyperspaceCloud = 0;

	m_landingGearAnimation = GetModel()->FindAnimation("gear_down");
	m_forceWheelUpdate = true;
	m_groundContacts.clear(); // found again for the new model

	// If we've got the tag_landing set then use it for an offset
	// otherwise use zero so that it will dock but look clearly incorrect
	const SceneGraph::Tag *tagNode = GetModel()->FindTagByName("tag_landing");
	if (tagNode) {
		m_landingMinOffset = tagNode->GetGlobalTransform().GetTranslate().y;
	} else {
		m_landingMinOffset = 0.0; // GetAabb().min.y;
	}

	InitMaterials();
	InitLightMounts();

	m_exhaustSpawner.RefreshMounts(GetModel());
}

void Ship::SetLightsLevel(int level)
{
	m_lightsLevel = Clamp(level, 0, MAX_LIGHTS_LEVEL);
}

// The lamps: the model's tag_light_* tags, shining along their +z (as a
// thruster's exhaust), or for models without them a pair under the nose,
// shining ahead and down (ships look along -z)
void Ship::InitLightMounts()
{
	m_lightMounts.clear();

	std::vector<SceneGraph::Tag *> tags;
	GetModel()->FindTagsByStartOfName("tag_light_", tags);
	for (const SceneGraph::Tag *tag : tags) {
		const matrix4x4f &m = tag->GetGlobalTransform();
		m_lightMounts.push_back({ m.GetTranslate(), m.ApplyRotationOnly(vector3f(0.f, 0.f, 1.f)).NormalizedSafe() });
	}
	if (!m_lightMounts.empty())
		return;

	const Aabb &aabb = GetAabb();
	const vector3f size(aabb.max - aabb.min);
	const float x = 0.5f * float(aabb.min.x + aabb.max.x);
	const float y = float(aabb.min.y) + 0.25f * size.y;
	const float z = float(aabb.min.z) + 0.15f * size.z;
	const float down = DEG2RAD(20.f);
	const vector3f direction(0.f, -std::sin(down), -std::cos(down));
	m_lightMounts.push_back({ vector3f(x - 0.2f * size.x, y, z), direction });
	m_lightMounts.push_back({ vector3f(x + 0.2f * size.x, y, z), direction });
}

void Ship::PostLoadFixup(Space *space)
{
	DynamicBody::PostLoadFixup(space);
	m_dockedWith = static_cast<SpaceStation *>(space->GetBodyByIndex(m_dockedWithIndex));
	if (m_curAICmd) m_curAICmd->PostLoadFixup(space);
	m_controller->PostLoadFixup(space);
	m_gunManager->PostLoadFixup(space);
}

void Ship::SaveToJson(Json &jsonObj, Space *space)
{
	DynamicBody::SaveToJson(jsonObj, space);

	Json shipObj({}); // Create JSON object to contain ship data.

	m_propulsion->SaveToJson(shipObj, space);

	m_skin.SaveToJson(shipObj);
	shipObj["wheel_transition"] = m_wheelTransition;
	shipObj["wheel_state"] = m_wheelState;
	shipObj["lights_level"] = m_lightsLevel;
	shipObj["launch_lock_timeout"] = m_launchLockTimeout;
	shipObj["test_landed"] = m_testLanded;
	shipObj["flight_state"] = int(m_flightState);
	shipObj["alert_state"] = int(m_alertState);
	shipObj["last_firing_alert"] = m_lastFiringAlert;

	// XXX make sure all hyperspace attrs and the cloud get saved
	Json hyperspaceDestObj({}); // Create JSON object to contain hyperspace destination data.
	m_hyperspace.dest.ToJson(hyperspaceDestObj);
	shipObj["hyperspace_destination"] = hyperspaceDestObj; // Add hyperspace destination object to ship object.
	shipObj["hyperspace_countdown"] = m_hyperspace.countdown;
	shipObj["hyperspace_warmup_sound"] = m_hyperspace.sounds.warmup_sound;
	shipObj["hyperspace_abort_sound"] = m_hyperspace.sounds.abort_sound;
	shipObj["hyperspace_jump_sound"] = m_hyperspace.sounds.jump_sound;
	shipObj["hyperspace_travel_sound"] = m_hyperspace.sounds.travel_sound;
	shipObj["hyperspace_exit_sound"] = m_hyperspace.sounds.exit_sound;

	shipObj["ecm_recharge"] = m_ecmRecharge;
	shipObj["ship_type_id"] = m_type->id;
	shipObj["docked_with_port"] = m_dockedWithPort;
	shipObj["index_for_body_docked_with"] = space->GetIndexForBody(m_dockedWith);
	shipObj["hull_mass_left"] = m_stats.hull_mass_left;
	shipObj["shield_mass_left"] = m_stats.shield_mass_left;
	shipObj["shield_cooldown"] = m_shieldCooldown;
	if (m_curAICmd) m_curAICmd->SaveToJson(shipObj);
	shipObj["ai_message"] = int(m_aiMessage);

	shipObj["controller_type"] = static_cast<int>(m_controller->GetType());
	m_controller->SaveToJson(shipObj, space);

	m_navLights->SaveToJson(shipObj);

	m_shields->SaveToJson(shipObj["shields"]);

	shipObj["name"] = m_shipName;

	jsonObj["ship"] = shipObj; // Add ship object to supplied object.
}

void Ship::InitMaterials()
{
	SceneGraph::Model *pModel = GetModel();
	assert(pModel);

	Graphics::Texture *tex = Graphics::TextureBuilder::Decal("textures/heat_gradient.dds").GetOrCreateTexture(Pi::renderer, "model");
	const Uint32 numMats = pModel->GetNumMaterials();
	for (Uint32 m = 0; m < numMats; m++) {
		RefCountedPtr<Graphics::Material> mat = pModel->GetMaterialByIndex(m);
		mat->SetTexture(s_heatGradientTexParam, tex);
	}
}

void Ship::SetController(ShipController *c)
{
	assert(c != 0);
	if (m_controller) delete m_controller;
	m_controller = c;
	m_controller->m_ship = this;
}

float Ship::GetPercentHull() const
{
	return 100.0f * (m_stats.hull_mass_left / float(m_type->hullMass));
}

float Ship::GetPercentShields() const
{
	if (m_stats.shield_mass <= 0)
		return 100.0f;
	else
		return 100.0f * (m_stats.shield_mass_left / m_stats.shield_mass);
}

float Ship::GetAtmosphericPressureLimit() const
{
	return m_type->atmosphericPressureLimit * std::max(m_stats.atmo_shield_cap, 1); //default to base limit if no shield installed
}

void Ship::SetPercentHull(float p)
{
	m_stats.hull_mass_left = 0.01f * Clamp(p, 0.0f, 100.0f) * float(m_type->hullMass);
	Properties().Set("hullMassLeft", m_stats.hull_mass_left);
	Properties().Set("hullPercent", 100.0f * (m_stats.hull_mass_left / float(m_type->hullMass)));
}

void Ship::UpdateMass()
{
	SetMass(((double)m_stats.static_mass + m_propulsion->FuelTankMassLeft()) * 1000);
}

template <typename T>
inline int sign(T num)
{
	return (num >= 0) - (num < 0);
}

vector3d Ship::CalcAtmosphericForce() const
{
	// Data from ship.
	const double topCrossSec = GetShipType()->topCrossSection;
	const double sideCrossSec = GetShipType()->sideCrossSection;
	const double frontCrossSec = GetShipType()->frontCrossSection;

	// TODO: vary drag coefficient based on Reynolds number, specifically by
	// atmospheric composition (viscosity) and airspeed (mach number).
	const double topDragCoeff = GetShipType()->topDragCoeff;
	const double sideDragCoeff = GetShipType()->sideDragCoeff;
	const double frontDragCoeff = GetShipType()->frontDragCoeff;

	const double shipLiftCoeff = GetShipType()->shipLiftCoefficient;

	// By converting the velocity into local space, we can apply the drag individually to each component.
	const vector3d localVel = GetVelocity() * GetOrient();

	// The drag forces applied to the craft, in local space.
	// TODO: verify dimensional accuracy and that we're not generating more drag than physically possible.
	// TODO: use a different drag constant for each side (front, back, etc).
	// This also handles (most of) the lift due to wing deflection.

	// Get current atmosphere parameters
	double pressure, density;
	GetCurrentAtmosphericState(pressure, density);
	if (is_zero_exact(density))
		return vector3d(0.0);

	// Precalculate common part of drag components calculation (rho / 2)
	const auto rho_2 = density / 2.;
	// Vector, which components are square of local airspeed vector components
	const vector3d lv2(localVel * localVel);
	const vector3d fAtmosDrag(
		-sign(localVel.x) * rho_2 * lv2.x * sideCrossSec * sideDragCoeff,
		-sign(localVel.y) * rho_2 * lv2.y * topCrossSec * topDragCoeff,
		-sign(localVel.z) * rho_2 * lv2.z * frontCrossSec * frontDragCoeff);

	// The amount of lift produced by air pressure differential across the top and bottom of the lifting surfaces.
	vector3d fAtmosLift(0.0);

	double AoAMultiplier = localVel.NormalizedSafe().y;

	// There's no lift produced once the wing hits the stall angle.
	if (std::abs(AoAMultiplier) < 0.61) {
		// Pioneer simulates non-cambered wings, with equal air displacement on either side of AoA.

		// Generated lift peaks at around 20 degrees here, and falls off fully at 35-ish.
		// TODO: handle AoA better / more gracefully with an actual angle- and curve-based implementation.
		AoAMultiplier = cos((std::abs(AoAMultiplier) - 0.31) * 5.0) * sign(AoAMultiplier);

		// TODO: verify dimensional accuracy and that we're not generating more lift than physically possible.
		// We scale down the lift contribution because fAtmosDrag handles deflection-based lift.
		fAtmosLift.y = CalcAtmosphericDrag(pow(localVel.z, 2), topCrossSec, shipLiftCoeff) * -AoAMultiplier * 0.2;
	}

	return GetOrient() * (fAtmosDrag + fAtmosLift);
}

//calculates torque to force the spacecraft go nose-first in atmosphere
vector3d Ship::CalcAtmoTorque() const
{
	double m_topCrossSec = GetShipType()->topCrossSection;
	double m_sideCrossSec = GetShipType()->sideCrossSection;
	double m_frontCrossSec = GetShipType()->frontCrossSection;
	double m_aeroStabilityMultiplier = GetShipType()->atmoStability;

	vector3d forward = GetOrient().VectorZ();
	vector3d m_vel = GetVelocity().NormalizedSafe();
	vector3d m_torqueDir = -m_vel.Cross(-forward); // <--- This is correct

	// TODO: evaluate this function and properly implement based upon ship cross-section.
	double m_drag = CalcAtmosphericDrag(GetVelocity().LengthSqr(), m_topCrossSec, DEFAULT_DRAG_COEFF);
	vector3d fAtmoTorque = vector3d(0.0);

	if (GetVelocity().Length() > 100) { //don't apply torque at minimal speeds
		fAtmoTorque = m_drag * m_torqueDir * ((m_topCrossSec + m_sideCrossSec) / (m_frontCrossSec * 4)) * 0.3 * m_aeroStabilityMultiplier * Pi::game->GetInvTimeAccelRate();
	}

	return fAtmoTorque;
}

bool Ship::OnDamage(Body *attacker, float kgDamage, const CollisionContact &contactData)
{
	if (m_invulnerable) {
		return true;
	}

	if (!IsDead()) {
		float dam = kgDamage * 0.001f;
		if (m_stats.shield_mass_left > 0.0f) {
			if (m_stats.shield_mass_left > dam) {
				m_stats.shield_mass_left -= dam;
				dam = 0;
			} else {
				dam -= m_stats.shield_mass_left;
				m_stats.shield_mass_left = 0;
			}
			Properties().Set("shieldMassLeft", m_stats.shield_mass_left);
		}

		m_shieldCooldown = DEFAULT_SHIELD_COOLDOWN_TIME;
		// transform the collision location into the models local space (from world space) and add it as a hit.
		matrix4x4d mtx = GetOrient();
		mtx.SetTranslate(GetPosition());
		const matrix4x4d invmtx = mtx.Inverse();
		const vector3d localPos = invmtx * contactData.pos;
		m_shields->AddHit(localPos);

		m_stats.hull_mass_left -= dam;
		Properties().Set("hullMassLeft", m_stats.hull_mass_left);
		Properties().Set("hullPercent", 100.0f * (m_stats.hull_mass_left / float(m_type->hullMass)));
		if (m_stats.hull_mass_left < 0) {
			if (attacker) {
				LuaEvent::Queue("onShipDestroyed", this, attacker);
			}

			Explode();
		} else {
			if (Pi::rng.Double() < kgDamage)
				SfxManager::Add(this, TYPE_DAMAGE);

			if (dam > float(GetShipType()->hullMass / 1000.) && m_lastDamageSound > k_damageSoundInterval) {
				if (dam < 0.01 * float(GetShipType()->hullMass))
					Sound::BodyMakeNoise(this, "Hull_hit_Small", 1.0f);
				else
					Sound::BodyMakeNoise(this, "Hull_Hit_Medium", 1.0f);

				m_lastDamageSound = 0.f;
			}
		}
	}

	//Output("Ouch! %s took %.1f kilos of damage from %s! (%.1f t hull left)\n", GetLabel().c_str(), kgDamage, attacker->GetLabel().c_str(), m_stats.hull_mass_left);
	return true;
}

bool Ship::OnCollision(Body *b, Uint32 flags, double relVel)
{
	// Collision with SpaceStation docking or entrance surface is
	// completely handled by SpaceStations, you only
	// need to return a "true" value in order to trigger
	// a bounce in Space::OnCollision
	if (b->IsType(ObjectType::SPACESTATION) && (flags & (SceneGraph::CollisionGeometry::DOCKING | SceneGraph::CollisionGeometry::ENTRANCE))) {
		// a surface starport's pad: nothing stops a ship without clearance
		// touching down there, and it lands as on the ground; with clearance
		// the starport docks it (or bounces it off to try again)
		const SpaceStation *station = static_cast<SpaceStation *>(b);
		if ((flags & SceneGraph::CollisionGeometry::DOCKING) && station->IsGroundStation() && station->GetMyDockingPort(this) < 0 &&
			m_flightState == FLYING && GetVelocity().Length() < MAX_LANDING_SPEED)
			m_testLanded = true;
		return true;
	}

	// hitting cargo scoop surface shouldn't do damage
	int cargoscoop_cap = Properties().Get("cargo_scoop_cap");
	if (cargoscoop_cap > 0 && b->IsType(ObjectType::CARGOBODY) && !b->IsDead()) {
		bool scooped = LuaObject<Ship>::CallMethod<bool>(this, "OnScoopCargo",
			static_cast<CargoBody *>(b)->GetCargoType(), static_cast<CargoBody *>(b)->GetCargoQuantity());

		if (scooped) {
			Pi::game->GetSpace()->KillBody(b);
			return true;
		}
	}


	if (b->IsType(ObjectType::PLANET)) {
		// geoms still enabled when landed
		if (m_flightState != FLYING)
			return false;
		else {
			if (GetVelocity().Length() < MAX_LANDING_SPEED) {
				m_testLanded = true;
				return true;
			}
		}
	}

	// a surface starport's other surfaces (its docking ones are handled
	// above): it can land there as on the ground (TestLanded checks the
	// structure is under it), then bounces off as usual
	if (b->IsType(ObjectType::SPACESTATION) && m_flightState == FLYING && GetVelocity().Length() < MAX_LANDING_SPEED &&
		static_cast<SpaceStation *>(b)->IsGroundStation())
		m_testLanded = true;

	if (b->IsType(ObjectType::SHIP) ||
		b->IsType(ObjectType::PLAYER) ||
		b->IsType(ObjectType::SPACESTATION) ||
		b->IsType(ObjectType::PLANET) ||
		b->IsType(ObjectType::STAR) ||
		b->IsType(ObjectType::CARGOBODY)) {
		LuaEvent::Queue("onShipCollided", this, b);
	}

	return DynamicBody::OnCollision(b, flags, relVel);
}

//destroy ship in an explosion
void Ship::Explode()
{
	if (m_invulnerable) return;

	Pi::game->GetSpace()->KillBody(this);
	if (this->GetFrame() == Pi::player->GetFrame()) {
		SfxManager::AddExplosion(this);
		Sound::BodyMakeNoise(this, "Explosion_1", 1.0f);
	}
	ClearThrusterState();
}

bool Ship::DoDamage(float kgDamage)
{
	PROFILE_SCOPED()
	if (m_invulnerable) {
		return true;
	}

	if (!IsDead()) {
		float dam = kgDamage * 0.01f;
		if (m_stats.shield_mass_left > 0.0f) {
			if (m_stats.shield_mass_left > dam) {
				m_stats.shield_mass_left -= dam;
				dam = 0;
			} else {
				dam -= m_stats.shield_mass_left;
				m_stats.shield_mass_left = 0;
			}
			Properties().Set("shieldMassLeft", m_stats.shield_mass_left);
		}

		m_shieldCooldown = DEFAULT_SHIELD_COOLDOWN_TIME;
		// create a collision location in the models local space and add it as a hit.
		Random rnd;
		rnd.seed(time(0));
		const vector3d randPos(
			rnd.Double() * 2.0 - 1.0,
			rnd.Double() * 2.0 - 1.0,
			rnd.Double() * 2.0 - 1.0);
		m_shields->AddHit(randPos * (GetPhysRadius() * 0.75));

		m_stats.hull_mass_left -= dam;
		Properties().Set("hullMassLeft", m_stats.hull_mass_left);
		Properties().Set("hullPercent", 100.0f * (m_stats.hull_mass_left / float(m_type->hullMass)));
		if (m_stats.hull_mass_left < 0) {
			Explode();
		} else {
			if (Pi::rng.Double() < dam)
				SfxManager::Add(this, TYPE_DAMAGE);
		}
	}

	//Output("Ouch! %s took %.1f kilos of damage from %s! (%.1f t hull left)\n", GetLabel().c_str(), kgDamage, attacker->GetLabel().c_str(), m_stats.hull_mass_left);
	return true;
}

void Ship::UpdateEquipStats()
{
	PropertyMap &p = Properties();

	m_stats.loaded_mass = p.Get("mass_cap");
	m_stats.static_mass = m_stats.loaded_mass + m_type->hullMass;

	m_stats.used_cargo = p.Get("usedCargo").get_integer();
	m_stats.free_cargo = p.Get("cargo_cap").get_integer() - m_stats.used_cargo;

	p.Set("loadedMass", m_stats.loaded_mass);
	p.Set("staticMass", m_stats.static_mass);

	float shield_cap = p.Get("shield_cap");
	m_stats.shield_mass = TONS_HULL_PER_SHIELD * shield_cap;
	p.Set("shieldMass", m_stats.shield_mass);

	UpdateFuelStats();

	unsigned int thruster_power_cap = p.Get("thruster_power_cap");
	const double power_mul = m_type->thrusterUpgrades[Clamp(thruster_power_cap, 0U, 3U)];
	m_propulsion->SetThrustPowerMult(power_mul, m_type->linThrust, m_type->angThrust);

	m_stats.hyperspace_range = m_stats.hyperspace_range_max = 0;
	p.Set("hyperspaceRange", m_stats.hyperspace_range);
	p.Set("maxHyperspaceRange", m_stats.hyperspace_range_max);

	m_stats.atmo_shield_cap = p.Get("atmo_shield_cap");
	m_stats.radar_cap = p.Get("radar_cap");
	m_stats.fuel_scoop_cap = p.Get("fuel_scoop_cap");
	m_stats.cargo_life_support_cap = p.Get("cargo_life_support_cap");
	m_stats.hull_autorepair_cap = p.Get("hull_autorepair_cap");
}

void Ship::UpdateLuaStats()
{
	// This code cannot be in UpdateEquipStats itself because *Equip* needs to be
	// called in Init(), which is itself called in the constructor, but we absolutely
	// cannot use LuaObject<Ship>::* in a constructor, or else we'd fix the type of the
	// object to Ship forever, even though it could very well be a Player.
	// Updates at Gen 2019: Indeed, this function has been removed from
	// loading because loading is now a ctor: see Body.cpp
	UpdateEquipStats();
	PropertyMap &p = Properties();
	m_stats.hyperspace_range = m_stats.hyperspace_range_max = 0;
	int hyperclass = p.Get("hyperclass_cap");
	if (hyperclass) {
		std::tie(m_stats.hyperspace_range, m_stats.hyperspace_range_max) =
			LuaObject<Ship>::CallMethod<double, double>(this, "GetHyperspaceRange");
	}

	p.Set("hyperspaceRange", m_stats.hyperspace_range);
	p.Set("maxHyperspaceRange", m_stats.hyperspace_range_max);
}

void Ship::UpdateFuelStats()
{
	m_stats.fuel_tank_mass_left = m_propulsion->FuelTankMassLeft();
	Properties().Set("fuelMassLeft", m_stats.fuel_tank_mass_left);

	UpdateMass();
}

Ship::HyperjumpStatus Ship::CheckHyperjumpCapability() const
{
	if (GetFlightState() == HYPERSPACE)
		return HYPERJUMP_DRIVE_ACTIVE;

	if (GetFlightState() != FLYING && GetFlightState() != JUMPING)
		return HYPERJUMP_SAFETY_LOCKOUT;

	return HYPERJUMP_OK;
}

Ship::HyperjumpStatus Ship::InitiateHyperjumpTo(const SystemPath &dest, int warmup_time, double duration, const HyperdriveSoundsTable &sounds, LuaRef checks)
{
	if (!dest.HasValidSystem() || GetFlightState() != FLYING || warmup_time < 1)
		return HYPERJUMP_SAFETY_LOCKOUT;
	StarSystem *s = Pi::game->GetSpace()->GetStarSystem().Get();
	if (s && s->GetPath().IsSameSystem(dest))
		return HYPERJUMP_CURRENT_SYSTEM;

	m_hyperspace.dest = dest;
	m_hyperspace.countdown = warmup_time;
	m_hyperspace.now = false;
	m_hyperspace.duration = duration;
	m_hyperspace.checks = checks;
	m_hyperspace.sounds = sounds;

	return Ship::HYPERJUMP_OK;
}

void Ship::AbortHyperjump()
{
	m_hyperspace.countdown = 0;
	m_hyperspace.now = false;
	m_hyperspace.duration = 0;
	m_hyperspace.checks = LuaRef();
}

float Ship::GetECMRechargeTime()
{
	float ecm_recharge_cap = Properties().Get("ecm_recharge_cap");
	return ecm_recharge_cap;
}

Ship::ECMResult Ship::UseECM()
{
	int ecm_power_cap = Properties().Get("ecm_power_cap");
	if (m_ecmRecharge > 0.0f) return ECM_RECHARGING;

	if (ecm_power_cap > 0) {
		Sound::BodyMakeNoise(this, "ECM", 1.0f);
		m_ecmRecharge = GetECMRechargeTime();

		// damage neaby missiles
		const float ECM_RADIUS = 4000.0f;

		Space::BodyNearList nearby = Pi::game->GetSpace()->GetBodiesMaybeNear(this, ECM_RADIUS);
		for (Body *body : nearby) {
			if (body->GetFrame() != GetFrame()) continue;

			double dist = (body->GetPosition() - GetPosition()).Length();
			if (body->IsType(ObjectType::MISSILE)) {
				if (dist < ECM_RADIUS) {
					// increasing chance of destroying it with proximity
					if (Pi::rng.Double() > (dist / ECM_RADIUS)) {
						static_cast<Missile *>(body)->ECMAttack(ecm_power_cap);
					}
				}
			}
			else if (body->IsType(ObjectType::SPACESTATION) && dist < ECM_RADIUS) {
				LuaEvent::Queue("unlawfulDischargeECM", this);
			}
			else { continue; }
		}
		return ECM_ACTIVATED;
	} else
		return ECM_NOT_INSTALLED;
}

Missile *Ship::SpawnMissile(const MissileDef &missileStats, Body *target)
{
	if (GetFlightState() != FLYING)
		return 0;

	Missile *missile = new Missile(this, missileStats);
	missile->SetOrient(GetOrient());
	missile->SetFrame(GetFrame());
	const vector3d pos = GetOrient() * vector3d(0, GetAabb().min.y - 10, GetAabb().min.z);
	const vector3d vel = -40.0 * GetOrient().VectorZ();
	missile->SetPosition(GetPosition() + pos);
	missile->SetVelocity(GetVelocity() + vel);
	Pi::game->GetSpace()->AddBody(missile);

	if (target) {
		missile->AIKamikaze(target);
	}

	return missile;
}

void Ship::SetFlightState(Ship::FlightState newState)
{
	if (m_flightState == newState) return;
	if (IsHyperspaceActive() && (newState != FLYING))
		AbortHyperjump();

	if (newState == FLYING) {
		m_testLanded = false;
		if (m_flightState == DOCKING || m_flightState == DOCKED)
			onUndock.emit();

		m_dockedWith = nullptr;

		// lock thrusters on for amount of time needed to push us out of station
		static const double MASS_LOCK_REFERENCE(40000.0); // based purely on experimentation
		// limit the time to between 2.0 and 20.0 seconds of thrust, the player can override
		m_launchLockTimeout = std::min(std::max(2.0, 2.0 * (GetMass() / MASS_LOCK_REFERENCE)), 20.0);
	}

	if (newState == DOCKED) {
		m_launchLockTimeout = 0.0;
		ClearLinThrusterState();
		ClearAngThrusterState();
	}

	m_flightState = newState;
	Properties().Set("flightState", EnumStrings::GetString("ShipFlightState", m_flightState));

	switch (m_flightState) {
	case FLYING:
		SetMoving(true);
		SetColliding(true);
		SetStatic(false);
		break;
	case DOCKING:
		SetMoving(false);
		SetColliding(false);
		SetStatic(false);
		break;
	case UNDOCKING:
		SetMoving(false);
		SetColliding(false);
		SetStatic(false);
		break;
		// TODO: set collision index? dynamic stations... use landed for open-air?
	case DOCKED:
		SetMoving(false);
		SetColliding(false);
		SetStatic(false);
		break;
	case LANDED:
		SetMoving(false);
		SetColliding(true);
		SetStatic(true);
		break;
	case JUMPING:
		SetMoving(true);
		SetColliding(false);
		SetStatic(false);
		break;
	case HYPERSPACE:
		SetMoving(false);
		SetColliding(false);
		SetStatic(false);
		break;
	}
}

void Ship::Blastoff()
{
	if (m_flightState != LANDED) return;
	// not without the player aboard
	if (IsType(ObjectType::PLAYER) && Pi::game->GetExcursion()->IsOffShip()) return;

	vector3d up = GetPosition().Normalized();

	Frame *f = Frame::GetFrame(GetFrame());

	assert(f->GetBody()->IsType(ObjectType::PLANET));

	if (ManualDocking()) {
		if (!IsType(ObjectType::PLAYER)) {
			Log::Warning("It wasn't the player's ship that tried to take off without an autopilot!");
			return;
		}
		// level again before lifting off itself (landed, it leans with the slope)
		const vector3d right = up.Cross(GetOrient().VectorZ()).NormalizedSafe();
		SetOrient(matrix3x3d::FromVectors(right, up));
		auto p = static_cast<Player*>(this);
		p->DoFixspeedTakeoff();
	} else {
		// level again (landed, it leans with the slope: SettleOnDrawnGround),
		// and 2 m up from where it rests (on the ground or a structure), and
		// no lower than that over the exact terrain
		const Planet *planet = static_cast<Planet *>(f->GetBody());
		const double planetRadius = 2.0 + std::max(GetPosition().Length() + GetAabb().min.y, planet->GetTerrainHeight(up));
		SetVelocity(vector3d::Zero);
		SetAngVelocity(vector3d::Zero);
		SetFlightState(FLYING);

		const vector3d right = up.Cross(GetOrient().VectorZ()).NormalizedSafe();
		SetOrient(matrix3x3d::FromVectors(right, up));
		SetPosition(up * planetRadius - GetAabb().min.y * up);
		SetThrusterState(1, 1.0); // thrust upwards
	}

	OnTakeoff(f->GetBody());
}

void Ship::TestLanded()
{
	PROFILE_SCOPED()
	m_testLanded = false;
	if (m_flightState != FLYING) return; // docking, say
	if (m_launchLockTimeout > 0.0f) return;
	if (m_wheelState < 1.0f) return;

	Frame *f = Frame::GetFrame(GetFrame());

	if (f->GetBody()->IsType(ObjectType::PLANET)) {
		double speed = GetVelocity().Length();
		vector3d up = GetPosition().Normalized();

		// what it rests on: the drawn terrain, or a structure (a starport's
		// apron, a city building, its roads) just under its bottom, not one
		// it brushed in flight (SettleOnDrawnGround takes over from here)
		const double bottom = GetPosition().Length() + GetAabb().min.y;
		const Body *structure = nullptr;
		const double ground = Walker::GroundRadius(f, up * bottom, -GetAabb().min.y, LANDED_PROBE_BELOW, &structure, GetGeom());
		const double terrain = static_cast<Planet *>(f->GetBody())->GetTerrainHeight(up);
		if (bottom - (structure ? ground : std::min(ground, terrain)) > LANDED_STRUCTURE_GAP)
			return;
		const double planetRadius = structure ? ground : terrain;

		if (speed < MAX_LANDING_SPEED) {
			// check player is sortof sensibly oriented for landing
			if (GetOrient().VectorY().Dot(up) > 0.99) {
				// position at zero altitude
				SetPosition(up * (planetRadius - GetAabb().min.y));

				// position facing in roughly the same direction
				vector3d right = up.Cross(GetOrient().VectorZ()).Normalized();
				SetOrient(matrix3x3d::FromVectors(right, up));

				SetVelocity(vector3d::Zero);
				SetAngVelocity(vector3d::Zero);
				ClearThrusterState();
				SetFlightState(LANDED);
				Sound::BodyMakeNoise(this, "Rough_Landing", 1.0f);
				OnLanded(f->GetBody());
				onLanded.emit();
			}
		}
	}
}

// A landed ship rests on the ground as drawn, as the walker and the rover do:
// the terrain mesh's vertices are tens of metres apart, and between them it
// lies metres above or below the exact height the ship touched down at
// (flight collision keeps the exact height), and on landing pads, roads and
// other structures under it. It finds the ground under each contact of its
// collision mesh, leans with the plane through them (up to LANDED_MAX_TILT)
// with none of them in the ground, and eases there; it keeps following the
// mesh as it is refined around the camera.
void Ship::SettleOnDrawnGround(const float timeStep)
{
	Frame *f = Frame::GetFrame(GetFrame());
	if (!f || !f->GetBody() || !f->GetBody()->IsType(ObjectType::PLANET))
		return;

	if (m_groundContacts.empty()) {
		// the bottom of the collision mesh, the farthest point round it in each direction
		const Aabb &box = GetAabb();
		const vector3d centre = (box.min + box.max) * 0.5;
		std::vector<std::pair<double, vector3d>> farthest(LANDED_CONTACT_SECTORS, { -1.0, vector3d(0.0) });
		if (GetCollMesh() && GetCollMesh()->GetGeomTree()) {
			for (const vector3f &v : GetCollMesh()->GetGeomTree()->GetVertices()) {
				if (v.y > box.min.y + LANDED_CONTACT_BAND)
					continue;
				const double dx = v.x - centre.x;
				const double dz = v.z - centre.z;
				const int sector = int(std::floor((std::atan2(dz, dx) + M_PI) / (2.0 * M_PI) * LANDED_CONTACT_SECTORS)) % LANDED_CONTACT_SECTORS;
				const double distSqr = dx * dx + dz * dz;
				if (distSqr > farthest[sector].first)
					farthest[sector] = { distSqr, vector3d(v.x, box.min.y, v.z) };
			}
		}
		for (const auto &entry : farthest) {
			if (entry.first >= 0.0)
				m_groundContacts.push_back(entry.second);
		}
		// too few to lean on: the middle of the footprint's sides
		if (m_groundContacts.size() < 3) {
			m_groundContacts.clear();
			const vector3d quarter = (box.max - box.min) * 0.25;
			for (const double x : { -quarter.x, quarter.x })
				for (const double z : { -quarter.z, quarter.z })
					m_groundContacts.push_back(vector3d(centre.x + x, box.min.y, centre.z + z));
		}
	}

	// the ground under each contact, in a frame along the planet's surface at
	// the ship: (east, north) across it, height above the ship's radius
	const vector3d pos = GetPosition();
	const double radius = pos.Length();
	const vector3d up = pos / radius;
	const vector3d axis = std::abs(up.y) < 0.9 ? vector3d(0.0, 1.0, 0.0) : vector3d(1.0, 0.0, 0.0);
	const vector3d across = (axis - up * up.Dot(axis)).Normalized();
	const vector3d along = up.Cross(across);
	const size_t n = m_groundContacts.size();
	std::vector<double> u(n), v(n), h(n);
	for (size_t i = 0; i < n; i++) {
		const vector3d c = GetOrient() * m_groundContacts[i];
		u[i] = c.Dot(across);
		v[i] = c.Dot(along);
		// from a little above where the contact is now, ignoring its own hull
		const vector3d at = pos + c;
		const double from = at.Length() + LANDED_PROBE_ABOVE;
		const double ground = Walker::GroundRadius(f, at.Normalized() * from, 0.0, LANDED_PROBE_ABOVE + LANDED_PROBE_BELOW, nullptr, GetGeom());
		h[i] = ground - radius;
	}

	// least-squares plane h = a + b u + c v through them
	double su = 0.0, sv = 0.0, sh = 0.0, suu = 0.0, svv = 0.0, suv = 0.0, suh = 0.0, svh = 0.0;
	for (size_t i = 0; i < n; i++) {
		su += u[i];
		sv += v[i];
		sh += h[i];
		suu += u[i] * u[i];
		svv += v[i] * v[i];
		suv += u[i] * v[i];
		suh += u[i] * h[i];
		svh += v[i] * h[i];
	}
	const double cuu = suu - su * su / n, cvv = svv - sv * sv / n, cuv = suv - su * sv / n;
	const double cuh = suh - su * sh / n, cvh = svh - sv * sh / n;
	const double det = cuu * cvv - cuv * cuv;
	double b = 0.0, c = 0.0;
	if (std::abs(det) > 1e-9) {
		b = (cuh * cvv - cvh * cuv) / det;
		c = (cvh * cuu - cuh * cuv) / det;
	}

	// it leans with the plane, no more than LANDED_MAX_TILT, keeping its heading
	vector3d wantUp = (up - across * b - along * c).Normalized();
	if (wantUp.Dot(up) < std::cos(LANDED_MAX_TILT)) {
		const vector3d lean = (wantUp - up * wantUp.Dot(up)).Normalized();
		wantUp = up * std::cos(LANDED_MAX_TILT) + lean * std::sin(LANDED_MAX_TILT);
	}
	const vector3d oldUp = GetOrient().VectorY();
	const double angle = std::acos(Clamp(oldUp.Dot(wantUp), -1.0, 1.0));
	const double turn = std::min(angle, LANDED_TURN_SPEED * timeStep);
	vector3d newUp = oldUp;
	if (angle > 1e-6) {
		const vector3d axisOfTurn = oldUp.Cross(wantUp).NormalizedSafe();
		newUp = (oldUp * std::cos(turn) + axisOfTurn.Cross(oldUp) * std::sin(turn)).Normalized();
	}
	const vector3d back = GetOrient().VectorZ();
	const vector3d newRight = newUp.Cross(back).NormalizedSafe();
	const matrix3x3d newOrient = matrix3x3d::FromVectors(newRight, newUp);

	// high enough that no contact is in the ground
	double lift = -std::numeric_limits<double>::max();
	for (size_t i = 0; i < n; i++) {
		const vector3d contact = pos + newOrient * m_groundContacts[i];
		lift = std::max(lift, (radius + h[i]) - contact.Length());
	}
	const double step = LANDED_SETTLE_SPEED * timeStep;
	const double rise = Clamp(lift, -step, step);
	if (std::abs(rise) < 0.001 && turn < 1e-5)
		return;

	SetOrient(newOrient);
	SetPosition(up * (radius + rise));
	// its geom is static while landed: re-indexed where it has moved to
	if (IsStatic())
		f->GetCollisionSpace()->FlagRebuildObjectTrees();
}

void Ship::SetLandedOn(Planet *p, float latitude, float longitude)
{
	m_wheelTransition = 0;
	m_wheelState = 1.0f;
	m_forceWheelUpdate = true;
	Frame *f_non_rot = Frame::GetFrame(p->GetFrame());
	SetFrame(f_non_rot->GetRotFrame());

	vector3d up = vector3d(cos(latitude) * sin(longitude), sin(latitude), cos(latitude) * cos(longitude));
	const double planetRadius = p->GetTerrainHeight(up);
	SetPosition(up * (planetRadius - GetAabb().min.y));
	vector3d right = up.Cross(vector3d(0, 0, 1)).Normalized();
	SetOrient(matrix3x3d::FromVectors(right, up));
	SetVelocity(vector3d::Zero);
	SetAngVelocity(vector3d::Zero);
	ClearThrusterState();
	SetFlightState(LANDED);
	OnLanded(p);
	onLanded.emit();
}

void Ship::SetFrame(FrameId fId)
{
	if (fId != GetFrame()) {
		m_exhaustSpawner.ClearChannelState();
	}
	DynamicBody::SetFrame(fId);
}

void Ship::TimeStepUpdate(const float timeStep)
{
	PROFILE_SCOPED()

	m_lastDamageSound += timeStep;

	// If docked, station is responsible for updating position/orient of ship
	// but we call this crap anyway and hope it doesn't do anything bad

	const vector3d thrust = m_propulsion->GetActualLinThrust();
	AddRelForce(thrust);
	AddRelTorque(m_propulsion->GetActualAngThrust());

	//apply extra atmospheric flight forces
	AddTorque(CalcAtmoTorque());

	if (m_landingGearAnimation) {
		m_landingGearAnimation->SetProgress(m_wheelState);
		if (m_forceWheelUpdate) {
			m_landingGearAnimation->Interpolate();
			m_forceWheelUpdate = false;
		}
	}

	if (!m_cargoSpawnQueue.empty()) {
		ProcessSpawnQueue();
	}

	m_dragCoeff = DynamicBody::DEFAULT_DRAG_COEFF * (1.0 + 0.25 * m_wheelState);
	DynamicBody::TimeStepUpdate(timeStep);
	if (m_flightState == LANDED)
		SettleOnDrawnGround(timeStep);

	// fuel use decreases mass, so do this as the last thing in the frame
	UpdateFuel(timeStep);

	m_navLights->SetEnabled(m_wheelState > 0.01f);
	m_navLights->Update(timeStep);
	if (m_sensors.get()) m_sensors->Update(timeStep);
}

void Ship::DoThrusterSounds() const
{
	// XXX any ship being the current camera body should emit sounds
	// also, ship sounds could be split to internal and external sounds

	// XXX sound logic could be part of a bigger class (ship internal sounds)
	/* Ship engine noise. less loud inside */
	float v_env = (Pi::game->GetWorldView()->shipView->IsExteriorView() ? 1.0f : 0.5f);
	static Sound::Event sndev;
	float volBoth = 0.0f;
	volBoth += 0.5f * fabs(m_propulsion->GetLinThrusterState().y);
	volBoth += 0.5f * fabs(m_propulsion->GetLinThrusterState().z);

	float targetVol[2] = { volBoth, volBoth };
	if (m_propulsion->GetLinThrusterState().x > 0.0)
		targetVol[0] += 0.5f * float(m_propulsion->GetLinThrusterState().x);
	else
		targetVol[1] += -0.5f * float(m_propulsion->GetLinThrusterState().x);

	targetVol[0] = v_env * Clamp(targetVol[0], 0.0f, 1.0f);
	targetVol[1] = v_env * Clamp(targetVol[1], 0.0f, 1.0f);
	float dv_dt[2] = { 4.0f, 4.0f };
	if (!sndev.VolumeAnimate(targetVol, dv_dt)) {
		sndev.Play("Thruster_large", 0.0f, 0.0f, Sound::OP_REPEAT);
		sndev.VolumeAnimate(targetVol, dv_dt);
	}
	float angthrust = 0.1f * v_env * float(m_propulsion->GetAngThrusterState().Length());

	static Sound::Event angThrustSnd;
	if (!angThrustSnd.VolumeAnimate(angthrust, angthrust, 5.0f, 5.0f)) {
		angThrustSnd.Play("Thruster_Small", 0.0f, 0.0f, Sound::OP_REPEAT);
		angThrustSnd.VolumeAnimate(angthrust, angthrust, 5.0f, 5.0f);
	}
}

// for timestep changes, to stop autopilot overshoot
// either adds half of current accel if decelerating
void Ship::TimeAccelAdjust(const float timeStep)
{
	if (!AIIsActive()) return;
#ifdef DEBUG_AUTOPILOT
	if (this->IsType(ObjectType::PLAYER))
		Output("Time accel adjustment, step = %.1f, decel = %s\n", double(timeStep),
			m_decelerating ? "true" : "false");
#endif
	vector3d vdiff = double(timeStep) * GetLastForce() * (1.0 / GetMass());
	if (!m_decelerating) vdiff = -2.0 * vdiff;
	SetVelocity(GetVelocity() + vdiff);
}

double Ship::GetHullTemperature() const
{
	// TODO: fix this to calculate appropriate skin friction and heating.
	//const double dragCoeff = DynamicBody::DEFAULT_DRAG_COEFF * 1.25;
	//const double dragGs = CalcAtmosphericDrag(GetVelocity().LengthSqr(), GetClipRadius(), dragCoeff) / (GetMass() * 9.81);
	//return dragGs / 25.0;
	// TODO: fix this to properly account for heating due to air friction instead of G-force.
	double dragGs = GetAtmosForce().Length() / (GetMass() * 9.81);
	return dragGs / (15.0 * (1.0 + m_stats.atmo_shield_cap + (2.0 * (1.0 - m_wheelState))));
}

void Ship::SetAlertState(AlertState as)
{
	m_alertState = as;
	Properties().Set("alertStatus", EnumStrings::GetString("ShipAlertStatus", as));
}

void Ship::UpdateAlertState()
{
	// no alerts if no radar
	if (m_stats.radar_cap <= 0) {
		// clear existing alert state if there was one
		if (GetAlertState() != ALERT_NONE) {
			SetAlertState(ALERT_NONE);
			LuaEvent::Queue("onShipAlertChanged", this, EnumStrings::GetString("ShipAlertStatus", ALERT_NONE));
		}
		return;
	}

	bool ship_is_near = false, ship_is_firing = false, missile_detected = false;

	// sanity check: m_lastAlertUpdate should not be in the future.
	// reset and re-check if it is.
	if (m_lastAlertUpdate > Pi::game->GetTime()) {
		m_lastAlertUpdate = 0;
		m_shipNear = false;
		m_shipFiring = false;
		m_missileDetected = false;
	}

	if (m_lastAlertUpdate + 1.0 <= Pi::game->GetTime()) {
		// time to update the list again, once per second should suffice
		m_lastAlertUpdate = Pi::game->GetTime();

		static const double ALERT_DISTANCE = 100000.0; // 100km
		Space::BodyNearList nearbyBodies = Pi::game->GetSpace()->GetBodiesMaybeNear(this, ALERT_DISTANCE);

		// handle the results
		for (auto i : nearbyBodies) {
			if ((i) == this) continue;

			if ((i)->IsType(ObjectType::SHIP)) {
				// TODO: Here there were a const on Ship*, now it cannot remain because of ship->firing and so, this open a breach...
				// A solution is to put a member on ship: true if is firing, false if is not
				Ship *ship = static_cast<Ship *>(i);

				if (ship->GetShipType()->tag == ShipType::TAG_STATIC_SHIP) continue;
				if (ship->GetFlightState() == LANDED || ship->GetFlightState() == DOCKED) continue;

				if (GetPositionRelTo(ship).LengthSqr() < ALERT_DISTANCE * ALERT_DISTANCE) {
					ship_is_near = true;

					Uint32 gunstate = ship->m_gunManager->IsFiring();
					if (gunstate) {
						ship_is_firing = true;
						break;
					}
				}
			} else if ((i)->IsType(ObjectType::MISSILE)) {
				Missile *missile = static_cast<Missile *>(i);

				if (missile->GetOwner() != this) {
					if (GetPositionRelTo(missile).LengthSqr() < ALERT_DISTANCE * ALERT_DISTANCE) {
						missile_detected = true;
						break;
					}
				}
			}
		}

		// store
		m_shipNear = ship_is_near;
		m_shipFiring = ship_is_firing;
		m_missileDetected = missile_detected;
	} else {
		ship_is_near = m_shipNear;
		ship_is_firing = m_shipFiring;
		missile_detected = m_missileDetected;
	}

	bool changed = false;
	switch (m_alertState) {
	case ALERT_NONE:
		if (ship_is_near) {
			SetAlertState(ALERT_SHIP_NEARBY);
			changed = true;
		}
		if (ship_is_firing) {
			m_lastFiringAlert = Pi::game->GetTime();
			SetAlertState(ALERT_SHIP_FIRING);
			changed = true;
		}
		if (missile_detected) {
			m_lastFiringAlert = Pi::game->GetTime();
			SetAlertState(ALERT_MISSILE_DETECTED);
			changed = true;
		}
		break;

	case ALERT_SHIP_NEARBY:
		if (!ship_is_near && !missile_detected) {
			SetAlertState(ALERT_NONE);
			changed = true;
		} else if (ship_is_firing) {
			m_lastFiringAlert = Pi::game->GetTime();
			SetAlertState(ALERT_SHIP_FIRING);
			changed = true;
		} else if (missile_detected) {
			m_lastFiringAlert = Pi::game->GetTime();
			SetAlertState(ALERT_MISSILE_DETECTED);
			changed = true;
		}
		break;

	case ALERT_SHIP_FIRING:
	case ALERT_MISSILE_DETECTED:
		if (!ship_is_near && !missile_detected) {
			SetAlertState(ALERT_NONE);
			changed = true;
		} else if (ship_is_firing || missile_detected) {
			m_lastFiringAlert = Pi::game->GetTime();
		} else if (m_lastFiringAlert + 60.0 <= Pi::game->GetTime()) {
			SetAlertState(ALERT_SHIP_NEARBY);
			changed = true;
		}
		break;
	}

	if (changed)
		LuaEvent::Queue("onShipAlertChanged", this, EnumStrings::GetString("ShipAlertStatus", GetAlertState()));
}

void Ship::UpdateFuel(const float timeStep)
{
	m_propulsion->UpdateFuel(timeStep);
	UpdateFuelStats();
	Properties().Set("fuel", GetFuel() * 100); // XXX to match SetFuelPercent

	if (m_propulsion->IsFuelStateChanged())
		LuaEvent::Queue("onShipFuelChanged", this, EnumStrings::GetString("PropulsionFuelStatus", m_propulsion->GetFuelState()));
}

void Ship::StaticUpdate(const float timeStep)
{
	PROFILE_SCOPED()
	// do player sounds before dead check, so they also turn off
	if (IsType(ObjectType::PLAYER)) DoThrusterSounds();

	if (IsDead()) return;

	if (m_controller) m_controller->StaticUpdate(timeStep);

	const double hullTemp = GetHullTemperature();
	if (hullTemp > 1.0) {
		DoDamage(hullTemp);
	}

	if (m_flightState == FLYING) {
		Frame *frame = Frame::GetFrame(GetFrame());
		Body *astro = frame->GetBody();
		if (astro && astro->IsType(ObjectType::PLANET)) {
			Planet *p = static_cast<Planet *>(astro);
			double dist = GetPosition().Length();
			double pressure, density;
			p->GetAtmosphericState(dist, &pressure, &density);

			int atmo_shield_cap = std::max(m_stats.atmo_shield_cap, 1); // needs to have some shielding by default
			if (pressure > (m_type->atmosphericPressureLimit * atmo_shield_cap)) {
				float damage = float(pressure - m_type->atmosphericPressureLimit);
				DoDamage(damage);
			}

			SpawnThrusterExhaustParticles(timeStep);
		}
	}

	UpdateAlertState();

	/* FUEL SCOOPING!!!!!!!!! */
	if (m_flightState == FLYING && m_stats.fuel_scoop_cap > 0) {
		// TODO: this isn't the cleanest, but at least it's performant and the
		// fiddly bits are delegated to Lua
		Frame *frame = Frame::GetFrame(GetFrame());
		Body *astro = frame->GetBody();
		if (astro && astro->IsType(ObjectType::PLANET)) {
			Planet *p = static_cast<Planet *>(astro);
			if (p->GetSystemBody()->IsScoopable()) {
				const double dist = GetPosition().Length();
				double pressure, density;
				p->GetAtmosphericState(dist, &pressure, &density);

				const double speed = GetVelocity().Length();
				const vector3d vdir = GetVelocity().Normalized();
				const vector3d pdir = -GetOrient().VectorZ();
				const double dot = vdir.Dot(pdir);
				const double speed_times_density = speed * density;
				/*
				 * speed = m/s, density = g/cm^3 -> T/m^3, pressure = Pa -> N/m^2 -> kg/(m*s^2)
				 * m    T      kg         m*kg^2           kg^2
				 * - * --- * ----- = 1000 ------- = 1000 -------
				 * s   m^3   m*s^2        m^4*s^3        m^3*s^3
				 *
				 * fuel_scoop_cap = area, m^2. rate = kg^2/(m*s^3) = (Pa*kg)/s^2
				 */
				const double hydrogen_density = 0.0002;
				if ((m_stats.free_cargo > 0) && (dot > 0.90) && speed_times_density > (100.0 * 0.3)) {
					const double rate = speed_times_density * hydrogen_density * double(m_stats.fuel_scoop_cap);
					m_hydrogenScoopedAccumulator += rate * timeStep;
					if (m_hydrogenScoopedAccumulator > 1) {
						const double scoopedTons = floor(m_hydrogenScoopedAccumulator);
						LuaEvent::Queue("onShipScoopFuel", this, p, scoopedTons);
						m_hydrogenScoopedAccumulator -= scoopedTons;
					}
				}
			}
		}
	}

	if (m_flightState == FLYING)
		m_launchLockTimeout -= timeStep;
	if (m_launchLockTimeout < 0) m_launchLockTimeout = 0;
	if (m_flightState == JUMPING || m_flightState == HYPERSPACE)
		m_launchLockTimeout = 0;

	// lasers
	GetComponent<GunManager>()->StaticUpdate(timeStep);

	// TODO: this is abominable.
	// It will lead to multiple sound cutouts with more than a single beam laser
	// Unfortunately, I don't have the time or inclination to completely rewrite the sound system at this juncture

	GunManager::WeaponIndexSet firedGuns = m_gunManager->GetGunsFiredThisFrame();
	GunManager::WeaponIndexSet stoppedGuns = m_gunManager->GetGunsStoppedThisFrame();

	for (size_t i = 0; i < m_gunManager->GetNumWeapons(); i++) {
		const GunManager::WeaponState *ws = m_gunManager->GetWeaponState(i);
		if (ws->data.projectileType == GunManager::PROJECTILE_BEAM) {
			if (m_gunManager->GetWeaponGroups()[ws->group].firing) {
				float vl, vr;
				Sound::CalculateStereo(this, 1.0f, &vl, &vr);
				if (firedGuns[i]) {
					m_beamLaser[i % 2].Play("Beam_laser", vl, vr, Sound::OP_REPEAT);
				} else {
					// update volume
					m_beamLaser[i % 2].SetVolume(vl, vr);
				}
			} else if (stoppedGuns[i]) {
				m_beamLaser[i % 2].Stop();
			}
		} else if (firedGuns[i]) {
			Sound::BodyMakeNoise(this, "Pulse_Laser", 1.0f);
		}
	}

	if (firedGuns.any())
		LuaEvent::Queue("onShipFiring", this);

	if (m_ecmRecharge > 0.0f) {
		m_ecmRecharge = std::max(0.0f, m_ecmRecharge - timeStep);
	}

	if (m_shieldCooldown > 0.0f) {
		m_shieldCooldown = std::max(0.0f, m_shieldCooldown - timeStep);
	}

	if (m_stats.shield_mass_left < m_stats.shield_mass) {
		// 250 second recharge
		float recharge_rate = 0.004f;
		float booster = Properties().Get("shield_energy_booster_cap");
		recharge_rate *= booster ? booster : 1.0;
		m_stats.shield_mass_left = Clamp(m_stats.shield_mass_left + m_stats.shield_mass * recharge_rate * timeStep, 0.0f, m_stats.shield_mass);
		Properties().Set("shieldMassLeft", m_stats.shield_mass_left);
	}

	if (m_wheelTransition) {
		m_wheelState += m_wheelTransition * 0.3f * timeStep;
		m_wheelState = Clamp(m_wheelState, 0.0f, 1.0f);
		if (is_equal_exact(m_wheelState, 0.0f) || is_equal_exact(m_wheelState, 1.0f)) {
			m_wheelTransition = 0;
			// TODO: this really needs to be driven by the animation; work around it by forcing an update for the last frame of the animation
			m_forceWheelUpdate = true;
			if (m_landingGearAnimation)
				GetModel()->SetAnimationActive(GetModel()->FindAnimationIndex(m_landingGearAnimation), false);
		}
	}

	if (m_testLanded) TestLanded();

	if (m_stats.hull_autorepair_cap && m_stats.hull_mass_left < float(m_type->hullMass)) {
		m_stats.hull_mass_left = std::min(m_stats.hull_mass_left + 0.1f * timeStep, float(m_type->hullMass));
		Properties().Set("hullMassLeft", m_stats.hull_mass_left);
		Properties().Set("hullPercent", 100.0f * (m_stats.hull_mass_left / float(m_type->hullMass)));
	}

	// After calling StartHyperspaceTo this Ship must not spawn objects
	// holding references to it (eg missiles), as StartHyperspaceTo
	// removes the ship from Space::bodies and so the missile will not
	// have references to this cleared by NotifyRemoved()
	if (m_hyperspace.now) {
		m_hyperspace.now = false;
		EnterHyperspace();
	}

	if (m_hyperspace.countdown > 0.0f) {
		// Check the Lua function
		bool abort = false;
		lua_State *l = m_hyperspace.checks.GetLua();
		if (l) {
			m_hyperspace.checks.PushCopyToStack();
			if (lua_isfunction(l, -1)) {
				lua_call(l, 0, 1);
				abort = !lua_toboolean(l, -1);
				lua_pop(l, 1);
			}
		}
		if (abort) {
			AbortHyperjump();
		} else {
			m_hyperspace.countdown = m_hyperspace.countdown - timeStep;
			if (!abort && m_hyperspace.countdown <= 0.0f && (is_equal_exact(m_wheelState, 0.0f))) {
				m_hyperspace.countdown = 0;
				m_hyperspace.now = true;
				SetFlightState(JUMPING);

				// We have to fire it here, because the event isn't actually fired until
				// after the whole physics update, which means the flight state on next
				// step would be HYPERSPACE, thus breaking quite a few things.
				OnBeforeEnterHyperspace();
			} else if (!(is_equal_exact(m_wheelState, 0.0f)) && this->IsType(ObjectType::PLAYER)) {
				AbortHyperjump();
				Sound::BodyMakeNoise(this, "Missile_Inbound", 1.0f);
			}
		}
	}
}

void Ship::SpawnThrusterExhaustParticles(float timeStep)
{
	if (IsDead()) return;
	if (m_flightState != FLYING) return;

	Frame *frame = Frame::GetFrame(GetFrame());
	Body *astro = frame->GetBody();
	if (!astro || !astro->IsType(ObjectType::PLANET)) return;

	Planet *p = static_cast<Planet *>(astro);
	const double dist = GetPosition().Length();
	double pressureAtDist, density;
	p->GetAtmosphericState(dist, &pressureAtDist, &density);
	(void)pressureAtDist;

	const SystemBody *sbody = p->GetSystemBody();
	// Terrestrial worlds / asteroids / moons with terrain (exclude gas giants and non-planet types).
	const bool landableNaturalWorld = sbody && (sbody->GetSuperType() == SystemBodyType::SUPERTYPE_ROCKY_PLANET);
	const double altitudeM = sbody ? (dist - sbody->GetRadius()) : 0.0;
	const double plumeAltitudeCapM = sbody ? std::max(sbody->GetAtmRadius(), 1000.0) : 0.0;
	double groundRadiusAtShip = landableNaturalWorld ? p->GetTerrainHeight(GetPosition().NormalizedSafe()) : 0.0;
	Color dustTint = landableNaturalWorld ? p->GetDustColor(GetPosition().NormalizedSafe()) : Color(128, 128, 128, 255);
	const bool overPad = TryGetLandingPadDustOverride(this, &groundRadiusAtShip, &dustTint);

	// Slight per-spawn dust colour variation so dust clouds are less uniform.
	const float shade = float(Pi::rng.Double(0.95, 1.05));
	dustTint.r = Uint8(Clamp(int(dustTint.r * shade), 0, 255));
	dustTint.g = Uint8(Clamp(int(dustTint.g * shade), 0, 255));
	dustTint.b = Uint8(Clamp(int(dustTint.b * shade), 0, 255));

	// Exhaust plumes shown if the ship is within the atmosphere height, or within 1 km of the surface for ground-hugging / surface dust.
	const bool lowAltitudePlumes = landableNaturalWorld && ((altitudeM <= plumeAltitudeCapM) || (dist <= groundRadiusAtShip + 1000.0));

	// Atmospheric exhaust plume - spawn only for ships close enough to the player.
	constexpr double EXHAUST_MAX_PLAYER_DISTANCE_SQR = SfxParams::EXHAUST_MAX_PLAYER_DISTANCE * SfxParams::EXHAUST_MAX_PLAYER_DISTANCE;
	const bool spawnExhaustForShip = !Pi::player || this == Pi::player || (GetPositionRelTo(Pi::player).LengthSqr() <= EXHAUST_MAX_PLAYER_DISTANCE_SQR);

	if (!spawnExhaustForShip || !lowAltitudePlumes) {
		// We're not adding any exhaust this tick. Clear the thruster exhaust channels so there is no stale state.
		m_exhaustSpawner.ClearChannelState();
		return;
	}

	// Simple ambient wind model: fixed-speed direction tangent to local surface.
	const vector3f localUp = vector3f(GetPosition().NormalizedSafe());
	vector3f windDir = vector3f(0.f, 1.f, 0.f).Cross(localUp);
	if (windDir.LengthSqr() < 1e-8f) windDir = vector3f(1.f, 0.f, 0.f).Cross(localUp);
	windDir = windDir.NormalizedSafe();
	const vector3f windVel = windDir * (SfxParams::EXHAUST_WIND_SPEED * float(Clamp(density, 0.0, 1.0)));

	ExhaustEnvironment env;
	env.density = density;
	env.opacityAtmosphereFactor = float(density / 1.225);
	env.windVel = windVel;
	env.groundRadius = groundRadiusAtShip;
	env.dustTint = dustTint;

	m_exhaustSpawner.Spawn(this, m_propulsion, timeStep, env, SfxParams::EXHAUST_PARTICLES_PER_SHIP_PER_SEC);

	// (paving gives up no dust)
	if (!overPad)
		SpawnGroundDust(timeStep, p, density, groundRadiusAtShip);
}

// Downwash: the jets holding a ship up near the ground blow the dust off it
// under the ship, out in a ring and up into a billowing cloud in air (as a
// helicopter's rotor does), or in fast low sheets without it. As much as the
// ship's thrust upwards against its weight, fading out by a few times its own
// size above the ground, and as the ground is dusty
// (SystemBody::CalcSurfaceDustiness). Puffs: SfxManager::AddDust.
void Ship::SpawnGroundDust(float timeStep, const Planet *planet, double density, double groundRadius)
{
	// puffs a second at full strength (fewer, larger and longer lived in air);
	// the reach above the ground: this many times the ship's size, and more
	constexpr double VACUUM_PUFFS = 1200.0;
	constexpr double AIR_PUFFS = 250.0;
	constexpr double REACH_SIZES = 10.0;
	constexpr double REACH_EXTRA = 60.0; // m
	// how fast the dust is blown out along the ground (m/s) at full strength
	constexpr double VACUUM_BLAST = 80.0;
	constexpr double AIR_BLAST = 40.0;
	// the wind in air as thick as Earth's (m/s)
	constexpr double DUST_WIND = 3.0;
	// without air the dust flies out in sheets: each puff streaked back along
	// as far as it goes in this long (s)
	constexpr double VACUUM_STREAK = 0.2;

	const SystemBody *sb = planet->GetSystemBody();
	const double dustiness = sb ? sb->CalcSurfaceDustiness() : 0.0;
	if (dustiness <= 0.0 || groundRadius <= 0.0) {
		m_groundDustAccum = 0.0;
		return;
	}

	// how hard the jets push up, against the ship's weight, and how near the ground they are
	const vector3d pos = GetPosition();
	const vector3d up = pos.NormalizedSafe();
	const double g = G * planet->GetMass() / (groundRadius * groundRadius);
	const double lift = (GetOrient() * m_propulsion->GetActualLinThrust()).Dot(up);
	const double weight = std::max(GetMass() * g, 1.0);
	const double size = GetAabb().radius;
	const double height = pos.Length() - groundRadius;
	const double reach = REACH_SIZES * size + REACH_EXTRA;
	const double nearness = Clamp(1.0 - height / reach, 0.0, 1.0);
	// (full blast from about half the ship's weight held up by the jets)
	const double strength = Clamp(std::sqrt(std::max(lift, 0.0) / weight) * 1.4, 0.0, 1.5) * nearness;
	if (strength <= 0.01) {
		m_groundDustAccum = 0.0;
		return;
	}

	const double air = Clamp(std::cbrt(density / 1.225) * 1.2, 0.0, 1.0);
	const auto mix = [air](double vacuum, double inAir) { return vacuum + (inAir - vacuum) * air; };

	// the ground's dust, as the exhaust's own
	Color dust = planet->GetDustColor(up);
	dust.a = Uint8(255.0 * std::min(dustiness * 1.5, 1.0));

	// the ground's axes under the ship
	const vector3d ref = std::abs(up.y) < 0.9 ? vector3d(0.0, 1.0, 0.0) : vector3d(1.0, 0.0, 0.0);
	const vector3d east = ref.Cross(up).Normalized();
	const vector3d north = up.Cross(east);
	vector3d windDir = vector3d(0.0, 1.0, 0.0).Cross(up);
	windDir = windDir.LengthSqr() > 1e-12 ? windDir.Normalized() : east;
	const vector3f wind(windDir * (DUST_WIND * air));
	const vector3d centre = up * groundRadius;

	// (time accelerated: no more than the stream of a tenth of a second)
	m_groundDustAccum += mix(VACUUM_PUFFS, AIR_PUFFS) * strength * dustiness * std::min(double(timeStep), 0.1);
	for (; m_groundDustAccum >= 1.0; m_groundDustAccum -= 1.0) {
		// from a ring under the ship, out along the ground
		const double angle = Pi::rng.Double(0.0, 2.0 * M_PI);
		const vector3d out = east * std::cos(angle) + north * std::sin(angle);
		const double from = size * Pi::rng.Double(0.3, 1.5);
		const vector3d at = centre + out * from + up * Pi::rng.Double(0.5, 0.2 * size + 1.0);
		// (most of it slow, so it is thickest under the ship, thinning out)
		const double fast = Pi::rng.Double(0.15, 1.0);
		const double blast = mix(VACUUM_BLAST, AIR_BLAST) * std::sqrt(strength) * fast * fast;
		const vector3d thrown = GetVelocity() * 0.2 + out * blast +
			up * (mix(Pi::rng.Double(0.02, 0.15) * blast, Pi::rng.Double(2.0, 10.0)));
		// (in air a wall of it: the puffs as big as the ship and more)
		const float startSize = float(mix(1.5 + 0.05 * size, 0.4 * size + 3.0));
		const float endSize = float(mix(3.0 + 0.25 * size, 1.5 * size + 15.0) * Pi::rng.Double(0.7, 1.3));
		const float life = float(mix(4.0, 18.0) * Pi::rng.Double(0.8, 1.2));
		// (a little lighter or darker puff by puff)
		Color tint = dust;
		const double shade = Pi::rng.Double(0.9, 1.1);
		tint.r = Uint8(Clamp(tint.r * shade, 0.0, 255.0));
		tint.g = Uint8(Clamp(tint.g * shade, 0.0, 255.0));
		tint.b = Uint8(Clamp(tint.b * shade, 0.0, 255.0));
		SfxManager::AddDust(GetFrame(), at, thrown, startSize, endSize, life, tint,
			float(mix(0.0, 1.2)), wind, float(g * (1.0 - 0.97 * air)), groundRadius, float(VACUUM_STREAK * (1.0 - air)));
	}
}

void Ship::NotifyRemoved(const Body *const removedBody)
{
	if (m_curAICmd) m_curAICmd->OnDeleted(removedBody);
}

void Ship::OnDocked(SpaceStation *s, int port)
{
	LuaEvent::Queue("onShipDocked", this, s, port);
}

void Ship::OnUndocked(SpaceStation *s, int port)
{
	LuaEvent::Queue("onShipUndocked", this, s, port);
}

void Ship::OnLanded(Body *b)
{
	LuaEvent::Queue("onShipLanded", this, b);
}

void Ship::OnTakeoff(Body *b)
{
	LuaEvent::Queue("onShipTakeOff", this, b);
}

bool Ship::IsOnSurface() const
{
	if (IsLanded())
		return true;
	if (IsDocked()) {
		const SpaceStation *station = GetDockedWith();
		return station && station->IsGroundStation();
	}
	return false;
}

bool Ship::Undock()
{
	return (m_dockedWith && m_dockedWith->LaunchShip(this, m_dockedWithPort));
}

void Ship::SetDockedWith(SpaceStation *s, int port)
{
	if (s) {
		m_dockedWith = s;
		m_dockedWithPort = port;
		m_wheelTransition = 0;
		m_wheelState = 1.0f;
		m_forceWheelUpdate = true;
		// hand position/state responsibility over to station
		m_dockedWith->SetDocked(this, port);
		onDock.emit();
	} else {
		Undock();
	}
}

void Ship::SetGunState(int idx, int state)
{
	if (m_flightState != FLYING)
		return;

	m_gunManager->SetGroupFiring(idx, state);
}

bool Ship::SetWheelState(bool down)
{
	if (m_flightState != FLYING) return false;
	if (is_equal_exact(m_wheelState, down ? 1.0f : 0.0f)) return false;
	int newWheelTransition = (down ? 1 : -1);
	if (newWheelTransition == m_wheelTransition) return false;
	m_wheelTransition = newWheelTransition;
	if (m_landingGearAnimation)
		GetModel()->SetAnimationActive(GetModel()->FindAnimationIndex(m_landingGearAnimation), true);
	return true;
}

void Ship::Render(Graphics::Renderer *renderer, const Camera *camera, const vector3d &viewCoords, const matrix4x4d &viewTransform)
{
	if (IsDead()) return;

	m_propulsion->Render(renderer, camera, viewCoords, viewTransform);

	// transpose the interpolated orient to convert velocity into shipspace, then into view space
	// FIXME: this produces a vector oriented to the top of the ship when velocity is forward.
	vector3f heatingNormal = matrix3x3f(viewTransform.GetOrient()).Inverse().Transpose() * vector3f(GetVelocity().Normalized());
	float heatingAmount = Clamp(GetHullTemperature(), 0.0, 1.0);

	for (Uint32 m = 0; m < GetModel()->GetNumMaterials(); m++) {
		GetModel()->GetMaterialByIndex(m)->SetPushConstant(s_heatingNormalParam, heatingNormal, heatingAmount);
	}

	// This has to be done per-model with a shield and just before it's rendered
	const bool shieldsVisible = m_shieldCooldown > 0.01f && m_stats.shield_mass_left > (m_stats.shield_mass / 100.0f);
	m_shields->SetEnabled(shieldsVisible);
	m_shields->Update(m_shieldCooldown, 0.01f * GetPercentShields());

	//strncpy(params.pText[0], GetLabel().c_str(), sizeof(params.pText));
	RenderModel(renderer, camera, viewCoords, viewTransform);
	m_navLights->Render(renderer);

	if (m_shieldModel && shieldsVisible)
		m_shieldModel->Render(matrix4x4f(viewTransform * GetInterpMatrix()));

	// Atmospheric reentry / high-speed glow (shield ellipsoid, alpha-blended; two-pass sort)
	vector3d vel = GetVelocity();
	if (m_reentryGlowModel && (m_flightState == FLYING) && (vel.LengthSqr() > 1.0)) {
		const float glow = CalcReentryGlowIntensity();
		const Uint32 glowMask = (glow > 0.02f) ? SceneGraph::NODE_TRANSPARENT : 0u;
		for (SceneGraph::StaticGeometry *geom : m_reentryGlowGeoms)
			geom->SetNodeMask(glowMask);
		if (glow > 0.02f) {
			vector3f flight = matrix3x3f(viewTransform.GetOrient()).Inverse().Transpose() * vector3f(vel);
			flight = flight.Normalized();
			// Use Game::GetTime() as the streak seed to achieve a highly variable random streak distribution effect.
			// If the game is paused we don't draw streaks at all - pass in -1 to signify this.
			const double simT = Pi::game->GetTime();
			const float reentryPhaseT = Pi::game->IsPaused() ? -1.f	: float(std::fmod(simT, 2048.0));
			for (ReentryGlowMatPair &mats : m_reentryGlowMaterials) {
				mats.backShellPass->SetPushConstant(s_reentryFlightParam, flight, glow);
				mats.backShellPass->SetPushConstant(s_reentryTimeParam, reentryPhaseT);
				mats.frontShellPass->SetPushConstant(s_reentryFlightParam, flight, glow);
				mats.frontShellPass->SetPushConstant(s_reentryTimeParam, reentryPhaseT);
			}
			m_reentryGlowModel->SetRenderTime(simT);
			const matrix4x4f glowMtx(matrix4x4f(viewTransform * GetInterpMatrix()));
			// Convex translucent shell: draw back faces first, then front
			BindReentryGlowMaterials(true);
			m_reentryGlowModel->Render(glowMtx);
			BindReentryGlowMaterials(false);
			m_reentryGlowModel->Render(glowMtx);
		}
	}

	renderer->GetStats().AddToStatCount(Graphics::Stats::STAT_SHIPS, 1);

	if (m_ecmRecharge > 0.0f) {
		constexpr uint32_t NUM_ECM_PARTICLES = 100;
		// ECM effect: a cloud of particles for a sparkly effect
		Graphics::VertexArray particles(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_NORMAL, NUM_ECM_PARTICLES);
		for (uint32_t i = 0; i < NUM_ECM_PARTICLES; i++) {
			const double r1 = Pi::rng.Double() - 0.5;
			const double r2 = Pi::rng.Double() - 0.5;
			const double r3 = Pi::rng.Double() - 0.5;
			vector3f pos(GetPhysRadius() * vector3d(r1, r2, r3).NormalizedSafe());
			particles.Add(pos, vector3f(0.f, 0.f, 50.f));
		}

		Color c(128, 128, 255, 255);
		float totalRechargeTime = GetECMRechargeTime();
		if (totalRechargeTime >= 0.0f) {
			c.a = (m_ecmRecharge / totalRechargeTime) * 255;
		}

		SfxManager::ecmParticle->diffuse = c;

		matrix4x4f t(viewTransform);
		t.SetTranslate(vector3f(viewCoords));

		renderer->SetTransform(t);
		renderer->DrawBuffer(&particles, SfxManager::ecmParticle.get());
	}
}

void Ship::RenderShadowCaster(Graphics::Renderer *r, const matrix4x4d &frameToLight, const Aabb &lightBox)
{
	if (IsDead() || m_flightState == HYPERSPACE)
		return;
	ModelBody::RenderShadowCaster(r, frameToLight, lightBox);
}

bool Ship::SpawnCargo(CargoBody *c_body)
{
	if (m_flightState != FLYING) {
		// cleanup the allocated CargoBody
		delete c_body;
		return false;
	}

	// don't actually spawn the cargo just yet, add it to a queue for spawning later
	m_cargoSpawnQueue.push_back(c_body);
	return true;
}

void Ship::ProcessSpawnQueue()
{
	// check we're due to spawn something
	const double latestTime = Pi::game->GetTime();
	if (m_latestSpawnTime > latestTime)
		m_latestSpawnTime = 0.0; // don't allow time to be in the future, reset it

	if (m_latestSpawnTime + 0.2 <= latestTime) {
		// time to spawn again
		m_latestSpawnTime = latestTime;
	} else {
		return; // not ready to spawn
	}

	// add one CargoBody to space at a time per timestep, this will happen over several steps
	// and spread out the bodies in physical space
	CargoBody *c_body = m_cargoSpawnQueue.front();
	m_cargoSpawnQueue.pop_front();
	if (!c_body)
		return; // sanity check

	const double r1 = (Pi::rng.Double() - 0.5);
	const double r2 = (Pi::rng.Double() - 0.5);
	const double r3 = (Pi::rng.Double() - 0.5);
	vector3d pos = GetOrient() * vector3d(0., GetAabb().min.y - 5., 0.);
	c_body->SetFrame(GetFrame());
	c_body->SetPosition(GetPosition() + pos);
	c_body->SetVelocity(GetVelocity() + GetOrient() * vector3d(r1, -10. + r2, r3));
	Pi::game->GetSpace()->AddBody(c_body);
}

void Ship::EnterHyperspace()
{
	assert(GetFlightState() != Ship::HYPERSPACE);

	// Is it still a good idea, with the onLeaveSystem moved elsewhere?
	Ship::HyperjumpStatus status = CheckHyperjumpCapability();
	if (status != HYPERJUMP_OK && status != HYPERJUMP_INITIATED) {
		if (m_flightState == JUMPING)
			SetFlightState(FLYING);
		return;
	}
	AIClearInstructions();
	SetFlightState(Ship::HYPERSPACE);

	// virtual call, do class-specific things
	OnEnterHyperspace();
}

void Ship::OnBeforeEnterHyperspace()
{
	LuaEvent::Queue("onShipLeaveSystem", this);
}

void Ship::OnEnterHyperspace()
{
	Sound::BodyMakeNoise(this, m_hyperspace.sounds.jump_sound.c_str(), 1.f);
	m_hyperspaceCloud = new HyperspaceCloud(this, Pi::game->GetTime() + m_hyperspace.duration, false);
	m_hyperspaceCloud->SetFrame(GetFrame());
	m_hyperspaceCloud->SetPosition(GetPosition());

	Space *space = Pi::game->GetSpace();

	space->RemoveBody(this);
	space->AddBody(m_hyperspaceCloud);
}

void Ship::EnterSystem()
{
	PROFILE_SCOPED()
	assert(GetFlightState() == Ship::HYPERSPACE);

	// virtual call, do class-specific things
	OnEnterSystem();

	SetFlightState(Ship::FLYING);

	LuaEvent::Queue("onShipEnterSystem", this);
}

void Ship::OnEnterSystem()
{
	m_hyperspaceCloud = 0;
}

void Ship::BindReentryGlowMaterials(bool backShellPass)
{
	for (SceneGraph::StaticGeometry *geom : m_reentryGlowGeoms) {
		for (Uint32 i = 0; i < geom->GetNumMeshes(); ++i) {
			SceneGraph::StaticGeometry::Mesh &mesh = geom->GetMeshAt(i);
			const uint64_t h = mesh.meshObject->GetFormat().Hash();
			for (ReentryGlowMatPair &mats : m_reentryGlowMaterials) {
				if (mats.formatHash != h)
					continue;
				mesh.material = backShellPass ? mats.backShellPass : mats.frontShellPass;
				break;
			}
		}
	}
}

Ship::ReentryGlowMatPair *Ship::FindOrCreateReentryGlowMats(Graphics::Renderer *r, const Graphics::VertexFormatDesc &vfmt)
{
	// We draw the shield re-entry glow in two passes - first all the back faces, then the front ones. If we didn't do this
	// then the faces would be drawn in mesh order and from some angles the rear of the shield bubble would be drawn over
	// the top of the front and look wrong.

	if (!s_reentryGlowTemplate.Valid()) {
		Graphics::MaterialDescriptor desc;
		desc.textures = 0;
		desc.lighting = false;
		Graphics::RenderStateDesc rsd;
		rsd.blendMode = Graphics::BLEND_ALPHA;
		rsd.depthWrite = false;
		rsd.depthTest = true;
		rsd.cullMode = Graphics::CULL_BACK;
		const auto vtxFormat = Graphics::VertexFormatDesc::FromAttribSet(
			Graphics::ATTRIB_POSITION | Graphics::ATTRIB_NORMAL | Graphics::ATTRIB_UV0 | Graphics::ATTRIB_TANGENT);
		s_reentryGlowTemplate.Reset(r->CreateMaterial("reentry_glow", desc, rsd, vtxFormat));
	}

	const uint64_t h = vfmt.Hash();
	for (ReentryGlowMatPair &mats : m_reentryGlowMaterials) {
		if (mats.formatHash == h)
			return &mats;
	}
	Graphics::Material *tpl = s_reentryGlowTemplate.Get();
	const Graphics::RenderStateDesc rsdBase = r->GetMaterialRenderState(tpl);

	Graphics::RenderStateDesc rsdBack = rsdBase;
	rsdBack.cullMode = Graphics::CULL_FRONT;
	RefCountedPtr<Graphics::Material> backMat(r->CloneMaterial(tpl, tpl->GetDescriptor(), rsdBack, vfmt));

	Graphics::RenderStateDesc rsdFront = rsdBase;
	rsdFront.cullMode = Graphics::CULL_BACK;
	RefCountedPtr<Graphics::Material> frontMat(r->CloneMaterial(tpl, tpl->GetDescriptor(), rsdFront, vfmt));

	ReentryGlowMatPair pair;
	pair.formatHash = h;
	pair.backShellPass = backMat;
	pair.frontShellPass = frontMat;
	m_reentryGlowMaterials.push_back(std::move(pair));
	return &m_reentryGlowMaterials.back();
}

float Ship::CalcReentryGlowIntensity() const
{
	if (m_flightState != FLYING)
		return 0.0f;
	double pressure, density;
	GetCurrentAtmosphericState(pressure, density);
	if (density <= 0.01)
		return 0.0f;

	// Thinner atmosphere translates to a higher required speed before re-entry glow is seen
	const double vel = GetVelocity().Length();
	const double vel_adj = vel * std::pow(density, 0.5);

	if (vel_adj < SfxParams::REENTRY_GLOW_MIN_SPEED) return 0.0f;
	if (vel_adj > SfxParams::REENTRY_GLOW_MAX_SPEED) return 1.0f;
	return (vel_adj - SfxParams::REENTRY_GLOW_MIN_SPEED) / (SfxParams::REENTRY_GLOW_MAX_SPEED - SfxParams::REENTRY_GLOW_MIN_SPEED);
}

void Ship::SetupShields()
{
	SceneGraph::Model *sm = Pi::FindModel(m_type->shieldName, false);

	if (sm) {
		m_shieldModel.reset(sm->MakeInstance());
		m_shields->ApplyModel(m_shieldModel.get());
		m_reentryGlowModel.reset(sm->MakeInstance());
		m_reentryGlowMaterials.clear();
		m_reentryGlowGeoms.clear();
		ReentryGlowSetupVisitor visitor(this, m_reentryGlowModel->GetRenderer());
		m_reentryGlowModel->GetRoot()->Accept(visitor);
	} else {
		m_shieldModel.reset();
		m_shields->ClearModel();
		m_reentryGlowModel.reset();
		m_reentryGlowMaterials.clear();
		m_reentryGlowGeoms.clear();
	}
}

void Ship::SetShipId(const ShipType::Id &shipId)
{
	m_type = &ShipType::types[shipId];

	Properties().Set("shipId", shipId);
}

void Ship::SetShipType(const ShipType::Id &shipId)
{
	SetShipId(shipId);
	SetModel(m_type->modelName.c_str());
	SetupShields();

	// Recreate our GunManager for the new ship type
	// Its init method will be called later
	RemoveComponent<GunManager>();
	m_gunManager = AddComponent<GunManager>();

	m_skin.SetDecal(m_type->manufacturer);
	m_skin.Apply(GetModel());
	Init();
	onFlavourChanged.emit();
	if (IsType(ObjectType::PLAYER))
		Pi::game->GetWorldView()->shipView->GetCameraController()->Reset();

	LuaObject<Ship>::CallMethod(this, "OnShipTypeChanged");

	LuaEvent::Queue("onShipTypeChanged", this);
}

void Ship::SetLabel(const std::string &label)
{
	DynamicBody::SetLabel(label);
	m_skin.SetLabel(label);
	m_skin.Apply(GetModel());
}

void Ship::SetShipName(const std::string &shipName)
{
	m_shipName = shipName;
	Properties().Set("shipName", shipName);
}

void Ship::SetSkin(const SceneGraph::ModelSkin &skin)
{
	m_skin = skin;
	m_skin.Apply(GetModel());
}

void Ship::SetPattern(const unsigned int num)
{
	GetModel()->SetPattern(num);
}

Uint8 Ship::GetRelations(Body *other) const
{
	auto it = m_relationsMap.find(other);
	if (it != m_relationsMap.end())
		return it->second;

	return 50;
}

void Ship::SetRelations(Body *other, Uint8 percent)
{
	m_relationsMap[other] = percent;
	if (m_sensors.get()) m_sensors->UpdateIFF(other);
}
