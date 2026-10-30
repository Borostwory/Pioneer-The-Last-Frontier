// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "SpaceStationType.h"
#include "FileSystem.h"
#include "JsonUtils.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Ship.h"
#include "StringF.h"
#include "scenegraph/Tag.h"
#include "scenegraph/Model.h"
#include "utils.h"
#include "EnumStrings.h"
#include "galaxy/SystemBody.h"

#include <algorithm>
#include <cmath>
#include <map>

// TODO: Fix the horrible control flow that makes this exception type necessary.
struct StationTypeLoadError {};

std::vector<SpaceStationType *> SpaceStationType::surfaceTypes;
std::vector<SpaceStationType *> SpaceStationType::orbitalTypes;

static std::map<std::string, SpaceStationType> s_allTypes;

// what a station type's "conditions" can test, on the body the station sits
// on or orbits
struct ConditionProperty {
	const char *name;
	double (*get)(const SystemBody *body);
};
static const ConditionProperty s_conditionProperties[] = {
	{ "oxidizing", [](const SystemBody *b) { return b->GetAtmosOxidizing(); } },
	{ "atmos_density", [](const SystemBody *b) { return b->GetVolatileGas(); } }, // kg/m^3, Earth 1.225
	{ "temperature", [](const SystemBody *b) { return double(b->GetAverageTemp()); } }, // K
	{ "gravity", [](const SystemBody *b) { return b->CalcSurfaceGravity() / 9.81; } }, // g
	{ "liquid", [](const SystemBody *b) { return b->GetVolatileLiquid(); } },
	{ "life", [](const SystemBody *b) { return b->GetLife(); } },
	{ "population", [](const SystemBody *b) { return b->GetPopulation(); } }, // billions
};

SpaceStationType::SpaceStationType(const std::string &id_, const std::string &path_) :
	id(id_),
	model(0),
	modelName(""),
	angVel(0.f),
	dockMethod(SURFACE),
	numDockingPorts(0),
	lastDockStage(DockStage::DOCK_ANIMATION_NONE),
	lastUndockStage(DockStage::UNDOCK_ANIMATION_NONE),
	parkingDistance(0),
	parkingGapSize(0),
	flattenRadius(0),
	customOnly(false),
	weight(1.0f)
{
	Json data = JsonUtils::LoadJsonDataFile(path_);
	if (data.is_null()) {
		Output("couldn't read station def '%s'\n", path_.c_str());
		throw StationTypeLoadError();
	}

	modelName = data.value("model", "");

	const std::string type = data.value("type", "");
	if (type == "surface")
		dockMethod = SURFACE;
	else if (type == "orbital")
		dockMethod = ORBITAL;
	else {
		Output("couldn't parse station def '%s': unknown type '%s'\n", path_.c_str(), type.c_str());
		throw StationTypeLoadError();
	}

	angVel = data.value("angular_velocity", 0.0f);

	parkingDistance = data.value("parking_distance", 0.0f);
	parkingGapSize = data.value("parking_gap_size", 0.0f);

	padOffset = data.value("pad_offset", 150.f);

	customOnly = data.value("custom_only", false);

	weight = std::max(0.0f, data.value("weight", 1.0f));

	const auto conds = data.find("conditions");
	if (conds != data.end() && conds->is_object()) {
		for (const auto &cond : conds->items()) {
			const std::string &key = cond.key();
			const bool isMin = starts_with(key, "min_");
			const bool isMax = starts_with(key, "max_");
			const auto prop = std::find_if(std::begin(s_conditionProperties), std::end(s_conditionProperties),
				[&](const ConditionProperty &p) { return (isMin || isMax) && key.compare(4, std::string::npos, p.name) == 0; });
			if (prop == std::end(s_conditionProperties) || !cond.value().is_number()) {
				Output("station def '%s': ignoring unknown condition '%s'\n", path_.c_str(), key.c_str());
				continue;
			}

			const int property = int(prop - std::begin(s_conditionProperties));
			auto c = std::find_if(conditions.begin(), conditions.end(), [&](const Condition &other) { return other.property == property; });
			if (c == conditions.end())
				c = conditions.insert(conditions.end(), Condition{ property, -HUGE_VAL, HUGE_VAL });
			(isMin ? c->min : c->max) = cond.value().get<double>();
		}
	}

	model = Pi::FindModel(modelName, /* allowPlaceholder = */ false);
	if (!model) {
		Output("couldn't initialize station type '%s' because the corresponding model ('%s') could not be found.\n", path_.c_str(), modelName.c_str());
		throw StationTypeLoadError();
	}

	// the ground levelled under a surface station (Terrain::ProtectSite): as
	// given, or the collision mesh's reach across the ground (x and z)
	flattenRadius = data.value("flatten_radius", 0.0f);
	if (flattenRadius <= 0.0f && model->GetCollisionMesh().Valid()) {
		const Aabb &aabb = model->GetCollisionMesh()->GetAabb();
		const double x = std::max(fabs(aabb.min.x), fabs(aabb.max.x));
		const double z = std::max(fabs(aabb.min.z), fabs(aabb.max.z));
		flattenRadius = float(sqrt(x * x + z * z));
	}

	OnSetupComplete();
}

void SpaceStationType::OnSetupComplete()
{
	// Since the model contains (almost) all of the docking information we have to extract that
	// and then generate any additional locators and information the station will need from it.

	// First we gather the MatrixTransforms that contain the location and orientation of the docking
	// locators/waypoints. We store some information within the name of these which needs parsing too.

	// Next we build the additional information required for docking ships with SPACE stations
	// on autopilot - this is the only option for docking with SPACE stations currently.
	// This mostly means offsetting from one locator to create the next in the sequence.

	// gather the tags
	std::vector<SceneGraph::Tag *> entrance_mts;
	std::vector<SceneGraph::Tag *> locator_mts;
	std::vector<SceneGraph::Tag *> exit_mts;
	model->FindTagsByStartOfName("entrance_", entrance_mts);
	model->FindTagsByStartOfName("loc_", locator_mts);
	model->FindTagsByStartOfName("exit_", exit_mts);

	Output("%s has:\n %lu entrances,\n %lu pads,\n %lu exits\n", modelName.c_str(), entrance_mts.size(), locator_mts.size(), exit_mts.size());

	// Add the partially initialised ports
	for (SceneGraph::Tag *tag : entrance_mts) {
		int portId;
		PiVerify(1 == sscanf(tag->GetName().c_str(), "entrance_port%d", &portId));
		PiVerify(portId > 0);

		const matrix4x4f &trans = tag->GetGlobalTransform();

		SPort new_port;
		new_port.portId = portId;
		new_port.name = tag->GetName();

		if (SURFACE == dockMethod) {
			const vector3f offDir = trans.Up().Normalized();
			new_port.m_approach[DockStage::APPROACH1] = trans;
			new_port.m_approach[DockStage::APPROACH1].SetTranslate(trans.GetTranslate() + (offDir * 500.0f));
		} else {
			const vector3f offDir = -trans.Back().Normalized();
			new_port.m_approach[DockStage::APPROACH1] = trans;
			new_port.m_approach[DockStage::APPROACH1].SetTranslate(trans.GetTranslate() + (offDir * 1500.0f));
		}
		new_port.m_approach[DockStage::APPROACH2] = trans;
		new_port.m_approach[DockStage::APPROACH1].Renormalize();
		new_port.m_approach[DockStage::APPROACH2].Renormalize();
		m_ports.push_back(new_port);
	}

	for (auto locIter : locator_mts) {
		int bay, portId;
		int minSize, maxSize;
		char padname[8];
		matrix4x4f locTransform = locIter->GetGlobalTransform();
		locTransform.Renormalize();

		// eg:loc_A001_p01_s0_500_b01
		PiVerify(5 == sscanf(locIter->GetName().c_str(), "loc_%4s_p%d_s%d_%d_b%d", &padname[0], &portId, &minSize, &maxSize, &bay));
		PiVerify(bay > 0 && portId > 0);

		// find the port and setup the rest of it's information
#ifndef NDEBUG
		bool bFoundPort = false;
#endif
		matrix4x4f approach1(0.0);
		matrix4x4f approach2(0.0);
		for (auto &rPort : m_ports) {
			if (rPort.portId == portId) {
				rPort.minShipSize = std::min(minSize, rPort.minShipSize);
				rPort.maxShipSize = std::max(maxSize, rPort.maxShipSize);
				rPort.bayIDs.push_back(std::make_pair(bay - 1, padname));
#ifndef NDEBUG
				bFoundPort = true;
#endif
				approach1 = rPort.m_approach[DockStage::APPROACH1];
				approach2 = rPort.m_approach[DockStage::APPROACH2];
				break;
			}
		}
		assert(bFoundPort);

		// now build the docking/leaving waypoints
		if (SURFACE == dockMethod) {
			// ground stations don't have leaving waypoints.
			m_bayPaths[bay].m_docking[DockStage::DOCKED] = locTransform; // final (docked)
			lastDockStage = DockStage::DOCK_ANIMATION_NONE;
			lastUndockStage = DockStage::UNDOCK_ANIMATION_NONE;
		} else {
			struct TPointLine {
				// for reference: http://paulbourke.net/geometry/pointlineplane/
				static bool ClosestPointOnLine(const vector3f &Point, const vector3f &LineStart, const vector3f &LineEnd, vector3f &Intersection)
				{
					const float LineMag = (LineStart - LineEnd).Length();

					const float U = (((Point.x - LineStart.x) * (LineEnd.x - LineStart.x)) +
										((Point.y - LineStart.y) * (LineEnd.y - LineStart.y)) +
										((Point.z - LineStart.z) * (LineEnd.z - LineStart.z))) /
						(LineMag * LineMag);

					if (U < 0.0f || U > 1.0f)
						return false; // closest point does not fall within the line segment

					Intersection.x = LineStart.x + U * (LineEnd.x - LineStart.x);
					Intersection.y = LineStart.y + U * (LineEnd.y - LineStart.y);
					Intersection.z = LineStart.z + U * (LineEnd.z - LineStart.z);

					return true;
				}
			};

			// create the docking locators

			// above the pad
			vector3f intersectionPos(0.0f);
			const vector3f approach1Pos = approach1.GetTranslate();
			const vector3f approach2Pos = approach2.GetTranslate();
			{
				const vector3f p0 = locTransform.GetTranslate();			   // plane position
				const vector3f l = (approach2Pos - approach1Pos).Normalized(); // ray direction
				const vector3f l0 = approach1Pos + (l * 10000.0f);

				if (!TPointLine::ClosestPointOnLine(p0, approach1Pos, l0, intersectionPos)) {
					Output("No point found on line segment");
				}
			}
			m_bayPaths[bay].m_docking[DockStage::DOCK_ANIMATION_1] = locTransform;
			m_bayPaths[bay].m_docking[DockStage::DOCK_ANIMATION_1].SetTranslate(intersectionPos);
			// final (docked)
			m_bayPaths[bay].m_docking[DockStage::DOCK_ANIMATION_2] = locTransform;
			lastDockStage = DockStage::DOCK_ANIMATION_2;

			m_bayPaths[bay].m_docking[DockStage::DOCKED] = locTransform;

			// create the leaving locators

			// above the pad
			m_bayPaths[bay].m_docking[DockStage::UNDOCK_ANIMATION_1] = locTransform;
			m_bayPaths[bay].m_docking[DockStage::UNDOCK_ANIMATION_1].SetTranslate(intersectionPos);
			lastUndockStage = DockStage::UNDOCK_ANIMATION_1;
		}
	}

	numDockingPorts = m_bayPaths.size();

	assert(!m_bayPaths.empty());
}

const SpaceStationType::SPort *SpaceStationType::FindPortByBay(const int zeroBaseBayID) const
{
	for (TPorts::const_iterator bayIter = m_ports.begin(), grpEnd = m_ports.end(); bayIter != grpEnd; ++bayIter) {
		for (auto idIter : (*bayIter).bayIDs) {
			if (idIter.first == zeroBaseBayID) {
				return &(*bayIter);
			}
		}
	}
	// is it safer to return that the bay is locked?
	return 0;
}

SpaceStationType::SPort *SpaceStationType::GetPortByBay(const int zeroBaseBayID)
{
	for (TPorts::iterator bayIter = m_ports.begin(), grpEnd = m_ports.end(); bayIter != grpEnd; ++bayIter) {
		for (auto idIter : (*bayIter).bayIDs) {
			if (idIter.first == zeroBaseBayID) {
				return &(*bayIter);
			}
		}
	}
	// is it safer to return that the bay is locked?
	return 0;
}

bool SpaceStationType::GetShipApproachWaypoints(const unsigned int port, DockStage stage, positionOrient_t &outPosOrient) const
{
	bool gotOrient = false;

	const SPort *pPort = FindPortByBay(port);
	if (pPort) {
		TMapBayIDMat::const_iterator stageDataIt = pPort->m_approach.find(stage);
		if (stageDataIt != pPort->m_approach.end()) {
			const matrix4x4f &mt = pPort->m_approach.at(stage);
			outPosOrient.pos = vector3d(mt.GetTranslate());
			outPosOrient.xaxis = vector3d(mt.GetOrient().VectorX());
			outPosOrient.yaxis = vector3d(mt.GetOrient().VectorY());
			outPosOrient.zaxis = vector3d(mt.GetOrient().VectorZ());
			outPosOrient.xaxis = outPosOrient.xaxis.Normalized();
			outPosOrient.yaxis = outPosOrient.yaxis.Normalized();
			outPosOrient.zaxis = outPosOrient.zaxis.Normalized();
			gotOrient = true;
		}
	}
	return gotOrient;
}
/*static*/
void SpaceStationType::Init()
{
	PROFILE_SCOPED()
	static bool isInitted = false;
	if (isInitted)
		return;
	isInitted = true;

	// load all station definitions
	namespace fs = FileSystem;
	for (fs::FileEnumerator files(fs::gameDataFiles, "stations", 0); !files.Finished(); files.Next()) {
		const fs::FileInfo &info = files.Current();
		if (ends_with_ci(info.GetPath(), ".json")) {

			const std::string id(info.GetName().substr(0, info.GetName().size() - 5));
			SpaceStationType *sst = nullptr;

			try {
				SpaceStationType st = SpaceStationType(id, info.GetPath());
				sst = &s_allTypes.try_emplace(st.id, std::move(st)).first->second;
			} catch (StationTypeLoadError) {
				// TODO: Actual error handling would be nice.
				Error("Error while loading Space Station data (check stdout/output.txt).\n");
			}

			if (!sst->customOnly) {
				switch (sst->dockMethod) {
				case SURFACE: surfaceTypes.push_back(sst); break;
				case ORBITAL: orbitalTypes.push_back(sst); break;
				}
			}

		}
	}
}

bool SpaceStationType::Suits(const SystemBody *sbody) const
{
	if (conditions.empty())
		return true;
	const SystemBody *body = sbody->GetParent();
	if (!body)
		return false;
	for (const Condition &c : conditions) {
		const double v = s_conditionProperties[c.property].get(body);
		if (v < c.min || v > c.max)
			return false;
	}
	return true;
}

/*static*/
const SpaceStationType *SpaceStationType::RandomStationType(Random &random, const SystemBody *sbody)
{
	const bool ground = sbody->GetType() != SystemBody::TYPE_STARPORT_ORBITAL;
	const std::vector<SpaceStationType *> &pool = ground ? surfaceTypes : orbitalTypes;

	// in the pool's order, so where the same types suit as before the pick
	// is the same as before
	std::vector<const SpaceStationType *> suited;
	for (const SpaceStationType *type : pool)
		if (type->weight > 0.0f && type->Suits(sbody))
			suited.push_back(type);
	if (suited.empty())
		suited.assign(pool.begin(), pool.end());

	const bool equalWeights = std::all_of(suited.begin(), suited.end(),
		[&](const SpaceStationType *type) { return type->weight == suited.front()->weight; });
	if (equalWeights)
		return suited[random.Int32(int(suited.size()))];

	double total = 0.0;
	for (const SpaceStationType *type : suited)
		total += type->weight;
	double pick = random.Double() * total;
	for (const SpaceStationType *type : suited) {
		pick -= type->weight;
		if (pick < 0.0)
			return type;
	}
	return suited.back();
}

/*static*/
const SpaceStationType *SpaceStationType::ForSystemBody(const SystemBody *sbody, Random &random)
{
	const std::string &custom = sbody->GetSpaceStationType();
	if (!custom.empty()) {
		if (const SpaceStationType *type = FindByName(custom))
			return type;
		Output("WARNING: SpaceStation::InitStation wants to initialize a custom station of type %s, but no station type with that id has been found.\n", custom.c_str());
	}

	const bool ground = sbody->GetType() != SystemBody::TYPE_STARPORT_ORBITAL;
	if ((ground ? surfaceTypes : orbitalTypes).empty())
		return nullptr;
	return RandomStationType(random, sbody);
}

/*static*/
const SpaceStationType *SpaceStationType::FindByName(const std::string &name)
{
	auto iter = s_allTypes.find(name);
	if (iter == s_allTypes.end())
		return nullptr;

	return &iter->second;
}

DockStage SpaceStationType::PivotStage(DockStage s) const {
	switch (s) {
		// at these stages, the position of the ship relative to the station has
		// already been calculated and is in the shipDocking_t data
		case DockStage::TOUCHDOWN:
		case DockStage::JUST_DOCK:
		case DockStage::LEVELING:
		case DockStage::REPOSITION:
			return DockStage::MANUAL;
		// at these stages, the station does not control the position of the ship
		case DockStage::CLEARANCE_GRANTED:
		case DockStage::LEAVE:
		case DockStage::APPROACH1:
		case DockStage::APPROACH2:
			return DockStage::NONE;
		default: return s;
	}
}

const char *SpaceStationType::DockStageName(DockStage s) const {
	return EnumStrings::GetString("DockStage", int(s));
}

matrix4x4f SpaceStationType::GetStageTransform(int bay, DockStage stage) const
{
	return m_bayPaths.at(bay + 1).m_docking.at(stage);
}
