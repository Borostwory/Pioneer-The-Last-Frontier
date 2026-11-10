// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Terrain.h"
#include "TerrainErosion.h"
#include "TerrainNoise.h"
#include "TerrainPhysical.h"
#include "TerrainPhysicalHydro.h"

#include "FileSystem.h"
#include "FloatComparison.h"
#include "GameConfig.h"
#include "MathUtil.h"
#include "perlin.h"
#include "SpaceStationType.h"
#include "core/macros.h"
#include "core/Log.h"
#include "../galaxy/SystemBody.h"

#include <atomic>
#include <chrono>
#include <iterator>
#include <map>
#include <thread>

namespace {
	// ground levelled beyond a starport's footprint (SpaceStationType::FlattenRadius)
	constexpr double SITE_MARGIN_M = 30.0;
	// the slope back to the natural ground: as wide as the level disc's radius, at least this
	constexpr double SITE_BLEND_MIN_M = 300.0;
	// asteroids' relief at most this fraction of their radius: the height
	// formula reaches the whole radius below ~170 km, and a height field that
	// tall gave spiky blobs
	constexpr double ASTEROID_RELIEF_PER_RADIUS = 0.4;
} // namespace

static int s_generator = 0;

void Terrain::SetGenerator(const int generator)
{
	s_generator = generator;
}

void Terrain::GetSurface(const vector3d &p, const double height, const vector3d &normal, vector3d &colourOut, uint8_t weightsOut[TerrainMaterialTable::SLOT_COUNT]) const
{
	colourOut = GetColor(p, height, normal);
	m_materials.Classify(colourOut, weightsOut);
}

// static instancer. selects the best height and color classes for the body
Terrain *Terrain::InstanceTerrain(const SystemBody *body)
{
	// special case for heightmaps
	// XXX this is terrible but will do for now until we get a unified
	// heightmap setup. if you add another height fractal, remember to change
	// the check in CustomSystem::l_height_map / SystemBodyData::LoadFromJson
	if (!body->GetHeightMapFilename().empty()) {
		const GeneratorInstancer choices[] = {
			InstanceGenerator<TerrainHeightMapped, TerrainColorEarthLikeHeightmapped>,
			InstanceGenerator<TerrainHeightMapped2, TerrainColorRock2>
		};
		assert(body->GetHeightMapFractal() < COUNTOF(choices));
		Terrain *terrain = choices[body->GetHeightMapFractal()](body);
		terrain->AddStarportSites(body);
		return terrain;
	}

	Random rand(body->GetSeed());

	GeneratorInstancer gi = 0;

	switch (body->GetType()) {

	case SystemBody::TYPE_BROWN_DWARF:
		gi = InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarBrownDwarf>;
		break;

	case SystemBody::TYPE_WHITE_DWARF:
		gi = InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarWhiteDwarf>;
		break;

	case SystemBody::TYPE_STAR_M:
	case SystemBody::TYPE_STAR_M_GIANT:
	case SystemBody::TYPE_STAR_M_SUPER_GIANT:
	case SystemBody::TYPE_STAR_M_HYPER_GIANT: {
		const GeneratorInstancer choices[] = {
			InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarM>,
			InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarM>,
			InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarK>,
			InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarG>
		};
		gi = choices[rand.Int32(COUNTOF(choices))];
		break;
	}

	case SystemBody::TYPE_STAR_K:
	case SystemBody::TYPE_STAR_K_GIANT:
	case SystemBody::TYPE_STAR_K_SUPER_GIANT:
	case SystemBody::TYPE_STAR_K_HYPER_GIANT: {
		const GeneratorInstancer choices[] = {
			InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarM>,
			InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarK>,
			InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarK>,
			InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarG>
		};
		gi = choices[rand.Int32(COUNTOF(choices))];
		break;
	}

	case SystemBody::TYPE_STAR_G:
	case SystemBody::TYPE_STAR_G_GIANT:
	case SystemBody::TYPE_STAR_G_SUPER_GIANT:
	case SystemBody::TYPE_STAR_G_HYPER_GIANT: {
		const GeneratorInstancer choices[] = {
			InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarWhiteDwarf>,
			InstanceGenerator<TerrainHeightEllipsoid, TerrainColorStarG>
		};
		gi = choices[rand.Int32(COUNTOF(choices))];
		break;
	}

	case SystemBody::TYPE_STAR_F:
	case SystemBody::TYPE_STAR_F_GIANT:
	case SystemBody::TYPE_STAR_F_HYPER_GIANT:
	case SystemBody::TYPE_STAR_F_SUPER_GIANT:
	case SystemBody::TYPE_STAR_A:
	case SystemBody::TYPE_STAR_A_GIANT:
	case SystemBody::TYPE_STAR_A_HYPER_GIANT:
	case SystemBody::TYPE_STAR_A_SUPER_GIANT:
	case SystemBody::TYPE_STAR_B:
	case SystemBody::TYPE_STAR_B_GIANT:
	case SystemBody::TYPE_STAR_B_SUPER_GIANT:
	case SystemBody::TYPE_STAR_B_WF:
	case SystemBody::TYPE_STAR_O:
	case SystemBody::TYPE_STAR_O_GIANT:
	case SystemBody::TYPE_STAR_O_HYPER_GIANT:
	case SystemBody::TYPE_STAR_O_SUPER_GIANT:
	case SystemBody::TYPE_STAR_O_WF:
		gi = InstanceGenerator<TerrainHeightEllipsoid, TerrainColorWhite>;
		break;

	case SystemBody::TYPE_STAR_S_BH:
	case SystemBody::TYPE_STAR_IM_BH:
	case SystemBody::TYPE_STAR_SM_BH:
		gi = InstanceGenerator<TerrainHeightEllipsoid, TerrainColorBlack>;
		break;

	case SystemBody::TYPE_PLANET_GAS_GIANT: {
		const GeneratorInstancer choices[] = {
			InstanceGenerator<TerrainHeightFlat, TerrainColorGGJupiter>,
			InstanceGenerator<TerrainHeightFlat, TerrainColorGGSaturn>,
			InstanceGenerator<TerrainHeightFlat, TerrainColorGGSaturn2>,
			InstanceGenerator<TerrainHeightFlat, TerrainColorGGNeptune>,
			InstanceGenerator<TerrainHeightFlat, TerrainColorGGNeptune2>,
			InstanceGenerator<TerrainHeightFlat, TerrainColorGGUranus>,
			InstanceGenerator<TerrainHeightFlat, TerrainColorGGSaturn>
		};
		gi = choices[rand.Int32(COUNTOF(choices))];
		break;
	}

	case SystemBody::TYPE_PLANET_ASTEROID: {
		if (s_generator == 1) {
			Terrain *terrain = new TerrainPhysical(body);
			terrain->AddStarportSites(body);
			return terrain;
		}

		const GeneratorInstancer choices[] = {
			InstanceGenerator<TerrainHeightAsteroid, TerrainColorAsteroid>,
			InstanceGenerator<TerrainHeightAsteroid2, TerrainColorAsteroid>,
			InstanceGenerator<TerrainHeightAsteroid3, TerrainColorAsteroid>,
			InstanceGenerator<TerrainHeightAsteroid4, TerrainColorAsteroid>,
			InstanceGenerator<TerrainHeightAsteroid, TerrainColorRock>,
			InstanceGenerator<TerrainHeightAsteroid2, TerrainColorBandedRock>,
			InstanceGenerator<TerrainHeightAsteroid3, TerrainColorRock>,
			InstanceGenerator<TerrainHeightAsteroid4, TerrainColorBandedRock>
		};
		gi = choices[rand.Int32(COUNTOF(choices))];
		break;
	}

	case SystemBody::TYPE_PLANET_TERRESTRIAL: {

		if (s_generator == 1) {
			Terrain *terrain = new TerrainPhysical(body);
			terrain->AddStarportSites(body);
			return terrain;
		}

		//Over-ride:
		//gi = InstanceGenerator<TerrainHeightAsteroid3,TerrainColorRock>;
		//break;
		// Earth-like world

		if ((body->GetLifeAsFixed() > fixed(7, 10)) && (body->GetVolatileGasAsFixed() > fixed(2, 10))) {
			// There would be no life on the surface without atmosphere

			if (body->GetAverageTemp() > 240) {
				const GeneratorInstancer choices[] = {
					InstanceGenerator<TerrainHeightHillsRidged, TerrainColorEarthLike>,
					InstanceGenerator<TerrainHeightHillsRivers, TerrainColorEarthLike>,
					InstanceGenerator<TerrainHeightHillsDunes, TerrainColorEarthLike>,
					InstanceGenerator<TerrainHeightMountainsRidged, TerrainColorEarthLike>,
					InstanceGenerator<TerrainHeightMountainsNormal, TerrainColorEarthLike>,
					InstanceGenerator<TerrainHeightMountainsRivers, TerrainColorEarthLike>,
					InstanceGenerator<TerrainHeightMountainsVolcano, TerrainColorEarthLike>,
					InstanceGenerator<TerrainHeightMountainsRiversVolcano, TerrainColorEarthLike>
				};
				gi = choices[rand.Int32(COUNTOF(choices))];
				break;
			}

			// desert-ice planets
			const GeneratorInstancer choices[] = {
				InstanceGenerator<TerrainHeightHillsRidged, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightHillsRivers, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightHillsDunes, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightMountainsRidged, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightMountainsNormal, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightMountainsRivers, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightMountainsVolcano, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightMountainsRiversVolcano, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightBarrenRock, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightBarrenRock2, TerrainColorDesert> //,
				//InstanceGenerator<TerrainHeightBarrenRock3,TerrainColorTFGood>
			};
			gi = choices[rand.Int32(COUNTOF(choices))];
			break;
		}

		// Harsh, habitable world
		if ((body->GetVolatileGasAsFixed() > fixed(2, 10)) && (body->GetLifeAsFixed() > fixed(4, 10))) {

			if (body->GetAverageTemp() > 240) {
				const GeneratorInstancer choices[] = {
					InstanceGenerator<TerrainHeightHillsRidged, TerrainColorTFGood>,
					InstanceGenerator<TerrainHeightHillsRivers, TerrainColorTFGood>,
					InstanceGenerator<TerrainHeightHillsDunes, TerrainColorTFGood>,
					InstanceGenerator<TerrainHeightHillsNormal, TerrainColorTFGood>,
					InstanceGenerator<TerrainHeightMountainsNormal, TerrainColorTFGood>,
					InstanceGenerator<TerrainHeightMountainsRidged, TerrainColorTFGood>,
					InstanceGenerator<TerrainHeightMountainsVolcano, TerrainColorTFGood>,
					InstanceGenerator<TerrainHeightMountainsRiversVolcano, TerrainColorTFGood>,
					InstanceGenerator<TerrainHeightMountainsRivers, TerrainColorTFGood>,
					InstanceGenerator<TerrainHeightRuggedDesert, TerrainColorTFGood>,
					InstanceGenerator<TerrainHeightBarrenRock, TerrainColorTFGood>,
					InstanceGenerator<TerrainHeightBarrenRock2, TerrainColorTFGood>
					//InstanceGenerator<TerrainHeightBarrenRock3,TerrainColorTFGood>
				};
				gi = choices[rand.Int32(COUNTOF(choices))];
				break;
			}

			// ice planets
			const GeneratorInstancer choices[] = {
				InstanceGenerator<TerrainHeightHillsRidged, TerrainColorIce>,
				InstanceGenerator<TerrainHeightHillsRivers, TerrainColorIce>,
				InstanceGenerator<TerrainHeightHillsDunes, TerrainColorIce>,
				InstanceGenerator<TerrainHeightHillsNormal, TerrainColorIce>,
				InstanceGenerator<TerrainHeightMountainsNormal, TerrainColorIce>,
				InstanceGenerator<TerrainHeightMountainsRidged, TerrainColorIce>,
				InstanceGenerator<TerrainHeightMountainsVolcano, TerrainColorIce>,
				InstanceGenerator<TerrainHeightMountainsRiversVolcano, TerrainColorIce>,
				InstanceGenerator<TerrainHeightMountainsRivers, TerrainColorIce>,
				InstanceGenerator<TerrainHeightRuggedDesert, TerrainColorIce>,
				InstanceGenerator<TerrainHeightBarrenRock, TerrainColorIce>,
				InstanceGenerator<TerrainHeightBarrenRock2, TerrainColorIce>,
				InstanceGenerator<TerrainHeightBarrenRock3, TerrainColorIce>
			};
			gi = choices[rand.Int32(COUNTOF(choices))];
			break;
		}

		// Marginally habitable world/ verging on mars like :)
		else if ((body->GetVolatileGasAsFixed() > fixed(1, 10)) && (body->GetLifeAsFixed() > fixed(1, 10))) {

			if (body->GetAverageTemp() > 240) {
				const GeneratorInstancer choices[] = {
					InstanceGenerator<TerrainHeightHillsRidged, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightHillsRivers, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightHillsDunes, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightHillsNormal, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightMountainsNormal, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightMountainsRidged, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightMountainsVolcano, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightMountainsRiversVolcano, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightMountainsRivers, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightRuggedDesert, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightBarrenRock, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightBarrenRock2, TerrainColorTFPoor>,
					InstanceGenerator<TerrainHeightBarrenRock3, TerrainColorTFPoor>
				};
				gi = choices[rand.Int32(COUNTOF(choices))];
				break;
			}

			// ice planets
			const GeneratorInstancer choices[] = {
				InstanceGenerator<TerrainHeightHillsRidged, TerrainColorIce>,
				InstanceGenerator<TerrainHeightHillsRivers, TerrainColorIce>,
				InstanceGenerator<TerrainHeightHillsDunes, TerrainColorIce>,
				InstanceGenerator<TerrainHeightHillsNormal, TerrainColorIce>,
				InstanceGenerator<TerrainHeightMountainsNormal, TerrainColorIce>,
				InstanceGenerator<TerrainHeightMountainsRidged, TerrainColorIce>,
				InstanceGenerator<TerrainHeightMountainsVolcano, TerrainColorIce>,
				InstanceGenerator<TerrainHeightMountainsRiversVolcano, TerrainColorIce>,
				InstanceGenerator<TerrainHeightMountainsRivers, TerrainColorIce>,
				InstanceGenerator<TerrainHeightRuggedDesert, TerrainColorIce>,
				InstanceGenerator<TerrainHeightBarrenRock, TerrainColorIce>,
				InstanceGenerator<TerrainHeightBarrenRock2, TerrainColorIce>,
				InstanceGenerator<TerrainHeightBarrenRock3, TerrainColorIce>
			};
			gi = choices[rand.Int32(COUNTOF(choices))];
			break;
		}

		// Desert-like world, Mars -like.
		if ((body->GetVolatileLiquidAsFixed() < fixed(1, 10)) && (body->GetVolatileGasAsFixed() > fixed(1, 5))) {
			const GeneratorInstancer choices[] = {
				InstanceGenerator<TerrainHeightHillsDunes, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightWaterSolid, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightRuggedDesert, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightRuggedLava, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightMountainsVolcano, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightMountainsRiversVolcano, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightBarrenRock, TerrainColorDesert>,
				InstanceGenerator<TerrainHeightBarrenRock2, TerrainColorDesert>
			};
			gi = choices[rand.Int32(COUNTOF(choices))];
			break;
		}

		// Frozen world
		if ((body->GetVolatileIcesAsFixed() > fixed(8, 10)) && (body->GetAverageTemp() < 250)) {
			const GeneratorInstancer choices[] = {
				InstanceGenerator<TerrainHeightHillsDunes, TerrainColorIce>,
				InstanceGenerator<TerrainHeightHillsCraters, TerrainColorIce>,
				InstanceGenerator<TerrainHeightMountainsCraters, TerrainColorIce>,
				InstanceGenerator<TerrainHeightWaterSolid, TerrainColorIce>,
				InstanceGenerator<TerrainHeightWaterSolidCanyons, TerrainColorIce>,
				InstanceGenerator<TerrainHeightRuggedDesert, TerrainColorIce>,
				InstanceGenerator<TerrainHeightBarrenRock, TerrainColorIce>,
				InstanceGenerator<TerrainHeightBarrenRock2, TerrainColorIce>,
				InstanceGenerator<TerrainHeightBarrenRock3, TerrainColorIce>
			};
			gi = choices[rand.Int32(COUNTOF(choices))];
			break;
		}

		// Volcanic world
		if (body->GetVolcanicityAsFixed() > fixed(7, 10)) {

			if (body->GetLifeAsFixed() > fixed(5, 10)) // life on a volcanic world ;)
				gi = InstanceGenerator<TerrainHeightRuggedLava, TerrainColorTFGood>;
			else if (body->GetLifeAsFixed() > fixed(2, 10))
				gi = InstanceGenerator<TerrainHeightRuggedLava, TerrainColorTFPoor>;
			else
				gi = InstanceGenerator<TerrainHeightRuggedLava, TerrainColorVolcanic>;
			break;
		}

		//Below might not be needed.
		//Alien life world:
		if (body->GetLifeAsFixed() > fixed(1, 10)) {
			const GeneratorInstancer choices[] = {
				InstanceGenerator<TerrainHeightHillsDunes, TerrainColorTFPoor>,
				InstanceGenerator<TerrainHeightHillsRidged, TerrainColorTFPoor>,
				InstanceGenerator<TerrainHeightHillsRivers, TerrainColorTFPoor>,
				InstanceGenerator<TerrainHeightMountainsNormal, TerrainColorTFPoor>,
				InstanceGenerator<TerrainHeightMountainsRidged, TerrainColorTFPoor>,
				InstanceGenerator<TerrainHeightMountainsVolcano, TerrainColorTFPoor>,
				InstanceGenerator<TerrainHeightMountainsRiversVolcano, TerrainColorTFPoor>,
				InstanceGenerator<TerrainHeightMountainsRivers, TerrainColorTFPoor>,
				InstanceGenerator<TerrainHeightWaterSolid, TerrainColorTFPoor>,
				InstanceGenerator<TerrainHeightRuggedLava, TerrainColorTFPoor>,
				InstanceGenerator<TerrainHeightRuggedDesert, TerrainColorTFPoor>,
				InstanceGenerator<TerrainHeightBarrenRock, TerrainColorIce>,
				InstanceGenerator<TerrainHeightBarrenRock2, TerrainColorIce>,
				InstanceGenerator<TerrainHeightBarrenRock3, TerrainColorIce>
			};
			gi = choices[rand.Int32(COUNTOF(choices))];
			break;
		};

		if (body->GetVolatileGasAsFixed() > fixed(1, 10)) {
			const GeneratorInstancer choices[] = {
				InstanceGenerator<TerrainHeightHillsNormal, TerrainColorRock>,
				InstanceGenerator<TerrainHeightMountainsNormal, TerrainColorRock>,
				InstanceGenerator<TerrainHeightRuggedDesert, TerrainColorRock>,
				InstanceGenerator<TerrainHeightBarrenRock, TerrainColorRock>,
				InstanceGenerator<TerrainHeightBarrenRock2, TerrainColorRock>,
				InstanceGenerator<TerrainHeightBarrenRock3, TerrainColorRock>
			};
			gi = choices[rand.Int32(COUNTOF(choices))];
			break;
		}

		const GeneratorInstancer choices[] = {
			InstanceGenerator<TerrainHeightHillsCraters2, TerrainColorRock>,
			InstanceGenerator<TerrainHeightMountainsCraters2, TerrainColorRock>,
			InstanceGenerator<TerrainHeightBarrenRock3, TerrainColorRock>
		};
		gi = choices[rand.Int32(COUNTOF(choices))];
		break;
	}

	default:
		gi = InstanceGenerator<TerrainHeightFlat, TerrainColorWhite>;
		break;
	}

	Terrain *terrain = gi(body);
	terrain->AddStarportSites(body);
	return terrain;
}

namespace {
	std::mutex s_prebuiltLock;
	std::map<const SystemBody *, Terrain *> s_prebuilt;
} // namespace

//static
void Terrain::Prebuild(const std::vector<const SystemBody *> &bodies)
{
	// a body from before may have the address of one of these
	DiscardPrebuilt();
	if (bodies.empty())
		return;

	const auto started = std::chrono::steady_clock::now();
	std::vector<Terrain *> built(bodies.size(), nullptr);
	std::atomic<size_t> next(0);
	auto work = [&]() {
		for (size_t i = next++; i < bodies.size(); i = next++) {
			ScopedSharedCores share;
			try {
				built[i] = InstanceTerrain(bodies[i]);
			} catch (...) {
				// left to PrebuiltOrNew, which builds it again on the main thread, where the error shows as it did
				built[i] = nullptr;
			}
		}
	};
	// on a single core this is the same as before: one body after another
	const int count = std::clamp(int(std::thread::hardware_concurrency()), 1, int(bodies.size()));
	std::vector<std::thread> threads;
	for (int i = 1; i < count; i++)
		threads.emplace_back(work);
	work();
	for (std::thread &t : threads)
		t.join();

	std::lock_guard<std::mutex> lock(s_prebuiltLock);
	for (size_t i = 0; i < bodies.size(); i++) {
		if (built[i])
			s_prebuilt[bodies[i]] = built[i];
	}
	Log::Info("Terrain: built {} bodies on {} threads in {:.0f} ms\n", bodies.size(), count,
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

//static
Terrain *Terrain::PrebuiltOrNew(const SystemBody *body)
{
	{
		std::lock_guard<std::mutex> lock(s_prebuiltLock);
		auto it = s_prebuilt.find(body);
		if (it != s_prebuilt.end()) {
			Terrain *terrain = it->second;
			s_prebuilt.erase(it);
			return terrain;
		}
	}
	return InstanceTerrain(body);
}

//static
void Terrain::DiscardPrebuilt()
{
	std::lock_guard<std::mutex> lock(s_prebuiltLock);
	for (auto &entry : s_prebuilt)
		delete entry.second;
	s_prebuilt.clear();
}

static size_t bufread_or_die(void *ptr, size_t size, size_t nmemb, ByteRange &buf)
{
	size_t read_count = buf.read(static_cast<char *>(ptr), size, nmemb);
	if (read_count < nmemb) {
		Output("Error: failed to read file (truncated)\n");
		abort();
	}
	return read_count;
}

// XXX this sucks, but there isn't a reliable cross-platform way to get them
#ifndef INT16_MIN
#define INT16_MIN (-32767 - 1)
#endif
#ifndef INT16_MAX
#define INT16_MAX (32767)
#endif
#ifndef UINT16_MAX
#define UINT16_MAX (65535)
#endif

Terrain::Terrain(const SystemBody *body) :
	m_seed(body->GetSeed()),
	m_rand(body->GetSeed()),
	m_heightScaling(0),
	m_minh(0),
	m_minBody(body),
	m_surfaceProfile(body)
{
	m_airless = body->GetVolatileGas() < 0.05;

	// load the heightmap
	if (!body->GetHeightMapFilename().empty()) {
		RefCountedPtr<FileSystem::FileData> fdata = FileSystem::gameDataFiles.ReadFile(body->GetHeightMapFilename());
		if (!fdata) {
			Output("Error: could not open file '%s'\n", body->GetHeightMapFilename().c_str());
			abort();
		}

		ByteRange databuf = fdata->AsByteRange();

		Sint16 minHMap = INT16_MAX, maxHMap = INT16_MIN;
		Uint16 minHMapScld = UINT16_MAX, maxHMapScld = 0;

		// XXX unify heightmap types
		switch (body->GetHeightMapFractal()) {
		case 0: {
			Uint16 v;
			bufread_or_die(&v, 2, 1, databuf);
			m_heightMapSizeX = v;
			bufread_or_die(&v, 2, 1, databuf);
			m_heightMapSizeY = v;
			const Uint32 heightmapPixelArea = (m_heightMapSizeX * m_heightMapSizeY);

			std::unique_ptr<Sint16[]> heightMap(new Sint16[heightmapPixelArea]);
			bufread_or_die(heightMap.get(), sizeof(Sint16), heightmapPixelArea, databuf);
			m_heightMap.reset(new double[heightmapPixelArea]);
			double *pHeightMap = m_heightMap.get();
			for (Uint32 i = 0; i < heightmapPixelArea; i++) {
				const Sint16 val = heightMap.get()[i];
				minHMap = std::min(minHMap, val);
				maxHMap = std::max(maxHMap, val);
				// store then increment pointer, in metres
				(*pHeightMap) = double(val) * body->GetHeightMapScale() + body->GetHeightMapOffset();
				++pHeightMap;
			}
			assert(pHeightMap == &m_heightMap[heightmapPixelArea]);
			//Output("minHMap = (%hd), maxHMap = (%hd)\n", minHMap, maxHMap);
			break;
		}

		case 1: {
			Uint16 v;
			// XXX x and y reversed from above *sigh*
			bufread_or_die(&v, 2, 1, databuf);
			m_heightMapSizeY = v;
			bufread_or_die(&v, 2, 1, databuf);
			m_heightMapSizeX = v;
			const Uint32 heightmapPixelArea = (m_heightMapSizeX * m_heightMapSizeY);

			// read height scaling and min height which are doubles
			double te;
			bufread_or_die(&te, 8, 1, databuf);
			m_heightScaling = te;
			bufread_or_die(&te, 8, 1, databuf);
			m_minh = te;

			std::unique_ptr<Uint16[]> heightMapScaled(new Uint16[heightmapPixelArea]);
			bufread_or_die(heightMapScaled.get(), sizeof(Uint16), heightmapPixelArea, databuf);
			m_heightMap.reset(new double[heightmapPixelArea]);
			double *pHeightMap = m_heightMap.get();
			for (Uint32 i = 0; i < heightmapPixelArea; i++) {
				const Uint16 val = heightMapScaled[i];
				minHMapScld = std::min(minHMapScld, val);
				maxHMapScld = std::max(maxHMapScld, val);
				// store then increment pointer
				// pre-multiply and offset the height value
				(*pHeightMap) = (double(val) * m_heightScaling + m_minh) * body->GetHeightMapScale() + body->GetHeightMapOffset();
				++pHeightMap;
			}
			assert(pHeightMap == &m_heightMap[heightmapPixelArea]);
			//Output("minHMapScld = (%hu), maxHMapScld = (%hu)\n", minHMapScld, maxHMapScld);
			break;
		}

		default:
			assert(0);
		}
	}

	m_sealevel = Clamp(body->GetVolatileLiquid(), 0.0, 1.0);
	m_icyness = Clamp(body->GetVolatileIces(), 0.0, 1.0);
	m_volcanic = Clamp(body->GetVolcanicity(), 0.0, 1.0); // height scales with volcanicity as well
	m_surfaceEffects = 0;

	const double rad = m_minBody.m_radius;

	// calculate max height
	if (!body->GetHeightMapFilename().empty() && body->GetHeightMapFractal() > 1) { // if scaled heightmap
		m_maxHeightInMeters = 1.1 * pow(2.0, 16.0) * m_heightScaling; // no min height required as it's added to radius in lua
	} else {
		// max mountain height for earth-like planet (same mass, radius)
		m_maxHeightInMeters = std::max(100.0, (9000.0 * rad * rad * (m_volcanic + 0.5)) / (body->GetMass() * 6.64e-12));
		m_maxHeightInMeters = std::min(rad, m_maxHeightInMeters); // small asteroid case
		if (body->GetType() == SystemBody::TYPE_PLANET_ASTEROID)
			m_maxHeightInMeters = std::min(m_maxHeightInMeters, ASTEROID_RELIEF_PER_RADIUS * rad);
	}
	// and then in sphere normalized jizz
	m_maxHeight = m_maxHeightInMeters / rad;
	//Output("%s: max terrain height: %fm [%f]\n", m_minBody.name.c_str(), m_maxHeightInMeters, m_maxHeight);
	m_invMaxHeight = 1.0 / m_maxHeight;
	m_planetRadius = rad;
	m_invPlanetRadius = 1.0 / rad;
	m_planetEarthRadii = rad / EARTH_RADIUS;

	// colour noise; deterministic and without m_rand, so the random sequence
	// the height fractals draw from stays unchanged
	SetColorFracDef(0, 10e3, 10.0);
	SetColorFracDef(1, 25e3, 25.0);
	SetColorFracDef(2, 2e3, 20.0);
	SetColorFracDef(3, 1000e3, 10e3);
	SetColorFracDef(4, 100e3, 1e3);
	SetColorFracDef(5, 50e3, 50.0);
	SetColorFracDef(6, 5e3, 50.0);
	SetColorFracDef(7, 20e3, 200.0);
	SetColorFracDef(8, 300e3, 3e3);
	SetColorFracDef(COLOR_FRACDEF_CLIMATE, 2000e3, 20e3);

	// NB: I don't know what this does but only the 1st entry was ever used, and only for Neptune and Jupiter colours
	m_entropy = m_rand.Double();

	// Pick some colors, mainly reds and greens
	for (int i = 0; i < int(COUNTOF(m_rockColor)); i++) {
		double r, g, b;
		r = m_rand.Double(0.3, 1.0);
		g = m_rand.Double(0.3, r);
		b = m_rand.Double(0.3, g);
		r = std::max(b, r * body->GetMetallicity());
		g = std::max(b, g * body->GetMetallicity());
		m_rockColor[i] = vector3d(r, g, b);
	}

	// Pick some darker colours mainly reds and greens
	for (int i = 0; i < int(COUNTOF(m_darkrockColor)); i++) {
		double r, g, b;
		r = m_rand.Double(0.05, 0.3);
		g = m_rand.Double(0.05, r);
		b = m_rand.Double(0.05, g);
		r = std::max(b, r * body->GetMetallicity());
		g = std::max(b, g * body->GetMetallicity());
		m_darkrockColor[i] = vector3d(r, g, b);
	}

	// grey colours, in case you simply must have a grey colour on a world with high metallicity
	for (int i = 0; i < int(COUNTOF(m_greyrockColor)); i++) {
		double g;
		g = m_rand.Double(0.3, 0.9);
		m_greyrockColor[i] = vector3d(g, g, g);
	}

	// Pick some plant colours, mainly greens
	// TODO take star class into account
	for (int i = 0; i < int(COUNTOF(m_plantColor)); i++) {
		double r, g, b;
		g = m_rand.Double(0.3, 1.0);
		r = m_rand.Double(0.3, g);
		b = m_rand.Double(0.2, r);
		g = std::max(r, g * body->GetLife());
		b *= (1.0 - body->GetLife());
		m_plantColor[i] = vector3d(r, g, b);
	}

	// Pick some darker plant colours mainly greens
	// TODO take star class into account
	for (int i = 0; i < int(COUNTOF(m_darkplantColor)); i++) {
		double r, g, b;
		g = m_rand.Double(0.05, 0.3);
		r = m_rand.Double(0.00, g);
		b = m_rand.Double(0.00, r);
		g = std::max(r, g * body->GetLife());
		b *= (1.0 - body->GetLife());
		m_darkplantColor[i] = vector3d(r, g, b);
	}

	// Pick some sand colours, mainly yellow
	// TODO let some planetary value scale this colour
	for (int i = 0; i < int(COUNTOF(m_sandColor)); i++) {
		double r, g, b;
		r = m_rand.Double(0.6, 1.0);
		g = m_rand.Double(0.6, r);
		//b = m_rand.Double(0.0, g/2.0);
		b = 0;
		m_sandColor[i] = vector3d(r, g, b);
	}

	// Pick some darker sand colours mainly yellow
	// TODO let some planetary value scale this colour
	for (int i = 0; i < int(COUNTOF(m_darksandColor)); i++) {
		double r, g, b;
		r = m_rand.Double(0.05, 0.6);
		g = m_rand.Double(0.00, r);
		//b = m_rand.Double(0.00, g/2.0);
		b = 0;
		m_darksandColor[i] = vector3d(r, g, b);
	}

	// Pick some dirt colours, mainly red/brown
	// TODO let some planetary value scale this colour
	for (int i = 0; i < int(COUNTOF(m_dirtColor)); i++) {
		double r, g, b;
		r = m_rand.Double(0.3, 0.7);
		g = m_rand.Double(r - 0.1, 0.75);
		b = m_rand.Double(0.0, r / 2.0);
		m_dirtColor[i] = vector3d(r, g, b);
	}

	// Pick some darker dirt colours mainly red/brown
	// TODO let some planetary value scale this colour
	for (int i = 0; i < int(COUNTOF(m_darkdirtColor)); i++) {
		double r, g, b;
		r = m_rand.Double(0.05, 0.3);
		g = m_rand.Double(r - 0.05, 0.35);
		b = m_rand.Double(0.0, r / 2.0);
		m_darkdirtColor[i] = vector3d(r, g, b);
	}

	// These are used for gas giant colours, they are more m_random and *should* really use volatileGasses - TODO
	for (int i = 0; i < int(COUNTOF(m_gglightColor)); i++) {
		double r, g, b;
		r = m_rand.Double(0.0, 0.5);
		g = m_rand.Double(0.0, 0.5);
		b = m_rand.Double(0.0, 0.5);
		m_gglightColor[i] = vector3d(r, g, b);
	}
	//darker gas giant colours, more reds and greens
	for (int i = 0; i < int(COUNTOF(m_ggdarkColor)); i++) {
		double r, g, b;
		r = m_rand.Double(0.0, 0.3);
		g = m_rand.Double(0.0, r);
		b = m_rand.Double(0.0, std::min(r, g));
		m_ggdarkColor[i] = vector3d(r, g, b);
	}

	// Rocky bodies take their palette from their physical properties (star
	// type, composition, life) instead of the random colours above. The
	// random draws above stay, so the height fractals that draw from m_rand
	// after this constructor get the same numbers as before.
	if (body->GetSuperType() == SystemBody::SUPERTYPE_ROCKY_PLANET) {
		SurfaceProfile::Palette palette;
		m_surfaceProfile.MakePalette(m_seed, palette);
		std::copy(std::begin(palette.rock), std::end(palette.rock), m_rockColor);
		std::copy(std::begin(palette.darkrock), std::end(palette.darkrock), m_darkrockColor);
		std::copy(std::begin(palette.plant), std::end(palette.plant), m_plantColor);
		std::copy(std::begin(palette.darkplant), std::end(palette.darkplant), m_darkplantColor);
		std::copy(std::begin(palette.sand), std::end(palette.sand), m_sandColor);
		std::copy(std::begin(palette.darksand), std::end(palette.darksand), m_darksandColor);
		std::copy(std::begin(palette.dirt), std::end(palette.dirt), m_dirtColor);
		std::copy(std::begin(palette.darkdirt), std::end(palette.darkdirt), m_darkdirtColor);
	}

	m_erosion = TerrainErosion::Create(body, rad);
}

Terrain::~Terrain()
{
}

void Terrain::GetHeights(const vector3d *vP, double *heightsOut, const size_t count) const
{
	GetBaseHeights(vP, heightsOut, count);
	if (m_erosion)
		m_erosion->Apply(*this, vP, heightsOut, count);
	AddLandforms(vP, heightsOut, count);
	LevelSites(vP, heightsOut, count);
}

namespace {
	// metres: the landforms as shape, of every size up to this; 0: off (Terrain::SetLandformShape)
	std::atomic<double> s_landformShape(0.0);
	// metres: ...from this size up (Terrain::SetLandformShapeFrom)
	std::atomic<double> s_landformShapeFrom(500.0);
	// how much they shade the ground's colour (Terrain::SetLandformShadeStrength)
	std::atomic<double> s_landformShade(Terrain::LANDFORM_SHADE);
	// how they fade out from afar (Terrain::SetLandformHeightFade, SetLandformShadeFade)
	std::atomic<double> s_heightFadeFrom(Terrain::LANDFORM_HEIGHT_FADE.from), s_heightFadeFull(Terrain::LANDFORM_HEIGHT_FADE.full);
	std::atomic<double> s_shadeFadeFrom(Terrain::LANDFORM_SHADE_FADE.from), s_shadeFadeFull(Terrain::LANDFORM_SHADE_FADE.full);
	// as TerrainTiles: the band's octaves, the most octaves, and the classic bodies' largest craters by their radius
	constexpr int LANDFORM_BAND_OCTAVES = 3;
	constexpr int LANDFORM_MAX_OCTAVES = 16;
	constexpr double LANDFORM_CRATER_TOP_PER_RADIUS = 0.25;
	// The ladder of the landforms' sizes starts from the deepest vertex spacing as GeoSphere reckoned it
	// (GetLegacyDeepestSpacing), but always with the patches of the planet detail setting the look was judged on
	// (Very High: 33 vertices along an edge, plus 2 for the skirt), so the ground doesn't follow the setting
	constexpr int LANDFORM_EDGE_LEN = 33 + 2;
	// metres above the sea over which the landforms fade in, so they don't move the coasts
	constexpr double LANDFORM_SHORE_M = 30.0;

	inline double LandformSmoothStep(const double e0, const double e1, const double x)
	{
		const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}
} // namespace

void Terrain::SetLandformShape(const double topMetres)
{
	s_landformShape.store(std::max(topMetres, 0.0), std::memory_order_relaxed);
}

double Terrain::GetLandformShape()
{
	return s_landformShape.load(std::memory_order_relaxed);
}

void Terrain::SetLandformShapeFrom(const double metres)
{
	s_landformShapeFrom.store(std::max(metres, 0.0), std::memory_order_relaxed);
}

double Terrain::GetLandformShapeFrom()
{
	return s_landformShapeFrom.load(std::memory_order_relaxed);
}

void Terrain::SetLandformShadeStrength(const double strength)
{
	s_landformShade.store(std::clamp(strength, 0.0, 1.0), std::memory_order_relaxed);
}

double Terrain::GetLandformShadeStrength()
{
	return s_landformShade.load(std::memory_order_relaxed);
}

void Terrain::SetLandformHeightFade(const TerrainLandforms::Fade fade)
{
	s_heightFadeFrom.store(std::max(fade.from, 0.0), std::memory_order_relaxed);
	s_heightFadeFull.store(std::max(fade.full, fade.from), std::memory_order_relaxed);
}

TerrainLandforms::Fade Terrain::GetLandformHeightFade()
{
	return { s_heightFadeFrom.load(std::memory_order_relaxed), s_heightFadeFull.load(std::memory_order_relaxed) };
}

void Terrain::SetLandformShadeFade(const TerrainLandforms::Fade fade)
{
	s_shadeFadeFrom.store(std::max(fade.from, 0.0), std::memory_order_relaxed);
	s_shadeFadeFull.store(std::max(fade.full, fade.from), std::memory_order_relaxed);
}

TerrainLandforms::Fade Terrain::GetLandformShadeFade()
{
	return { s_shadeFadeFrom.load(std::memory_order_relaxed), s_shadeFadeFull.load(std::memory_order_relaxed) };
}

void Terrain::SetLargeLandforms(const bool on)
{
	SetLandformShape(on ? LARGE_LANDFORMS_TO_M : 0.0);
	if (on)
		SetLandformShapeFrom(LARGE_LANDFORMS_FROM_M);
}

void Terrain::GetLandformStyle(float style[4]) const
{
	if (GetTileStyle(style))
		return;
	const std::string name = GetHeightFractalName();
	auto has = [&](const char *s) { return name.find(s) != std::string::npos; };
	float rolling = 1.0f, ridged = 0.0f, dunes = 0.0f, craters = 0.0f;
	if (has("Dunes")) {
		rolling = 0.4f;
		dunes = 0.8f;
	} else if (has("Mountains") || has("Rugged") || has("Ridged") || has("Barren") || has("Mapped")) {
		rolling = 0.5f;
		ridged = 0.6f;
	}
	if (has("Crater") || has("Asteroid") || m_airless) {
		craters = 1.0f;
		rolling *= 0.4f;
		ridged *= 0.4f;
		dunes = 0.0f;
	}
	style[0] = rolling;
	style[1] = ridged;
	style[2] = dunes;
	style[3] = craters;
}

const TerrainLandforms::Params &Terrain::GetLandformParams() const
{
	std::call_once(m_landformOnce, [this]() {
		TerrainLandforms::Params &p = m_landformParams;
		p.radius = m_planetRadius;
		// the deepest level and its spacing as GeoSphere::CalculateMaxPatchDepth and GetLegacyDeepestSpacing work
		// them out, with LANDFORM_EDGE_LEN
		const double edge0 = 2.0 * M_PI * m_planetRadius / double(LANDFORM_EDGE_LEN * 8);
		double edge = edge0;
		int meant = 0;
		while (edge > 100.0 && meant < 15) {
			edge *= 0.5;
			meant++;
		}
		const int level = std::min(2 * meant, 15);
		const double ladderBase = 0.5 * edge0 / std::pow(2.0, double(level));
		// the finest octave as the tiles of the deepest patches have it
		p.finest = 1;
		p.finestWave = 2.0 * ladderBase;
		GetLandformStyle(p.style);
		p.band = LANDFORM_BAND_OCTAVES;
		p.count = LANDFORM_BAND_OCTAVES;
		p.craterTop = GetTileCraterTop();
		p.landformTop = GetTileLandformTop();
		p.blocks = GetTileBlocks();
		if (p.style[3] > 0.0f || p.landformTop > 0.0) {
			const double largest = std::max(p.style[3] > 0.0f ? (p.craterTop > 0.0 ? p.craterTop : m_planetRadius * LANDFORM_CRATER_TOP_PER_RADIUS) : 0.0, p.landformTop);
			const int top = int(std::floor(std::log2(std::max(largest / ladderBase, 1.0))));
			p.count = std::min(std::max(top - p.finest + 1, LANDFORM_BAND_OCTAVES), LANDFORM_MAX_OCTAVES);
		}
	});
	return m_landformParams;
}

void Terrain::AddLandforms(const vector3d *vP, double *heightsOut, const size_t count) const
{
	const double top = GetLandformShape();
	if (top <= 0.0)
		return;
	const TerrainLandforms::Params &params = GetLandformParams();
	// none finer than the band limit the heights are made with (TerrainNoise::ScopedBandLimit)
	const double maxFrequency = TerrainNoise::s_maxFrequency;
	const double finest = maxFrequency > 0.0 ? m_planetRadius / maxFrequency : 0.0;
	// only the large sizes: the smaller ones stay painted by the GPU detail tiles (TerrainTiles skips these)
	const double smallest = std::max(finest, GetLandformShapeFrom());
	if (params.count <= 0 || smallest > top)
		return;
	// No cracked slabs as shape: this large, they made plateaus with jagged cliffs (Io, 2026-11-10). They stay
	// painted at the smaller sizes.
	TerrainLandforms::Params shape = params;
	shape.blocks = 0.0;
	// (finest is two samples: the band limit's)
	const TerrainLandforms::Fade heightFade = GetLandformHeightFade();
	const bool water = (m_surfaceEffects & EFFECT_WATER) != 0;
	for (size_t i = 0; i < count; i++) {
		const double base = heightsOut[i];
		// none in the sea, fading in above it, so the coasts stay where they are
		const double weight = water ? LandformSmoothStep(0.0, LANDFORM_SHORE_M, base * m_planetRadius) : 1.0;
		if (weight <= 0.0)
			continue;
		const TerrainLandforms::Sample s = TerrainLandforms::Evaluate(shape, vP[i].Normalized(), smallest, top, 0.5 * finest, heightFade);
		const double h = base + weight * s.height * m_invPlanetRadius;
		// not below the zero level (the sea, or the lowest ground) where the ground was above it
		heightsOut[i] = base >= 0.0 ? std::max(h, 0.0) : h;
	}
}

double Terrain::LandformShade(const vector3d &p) const
{
	// the same octaves as AddLandforms puts into the heights at this band limit
	const double top = GetLandformShape();
	if (top <= 0.0 || GetLandformShadeStrength() <= 0.0)
		return 0.0;
	const TerrainLandforms::Params &params = GetLandformParams();
	const double maxFrequency = TerrainNoise::s_maxFrequency;
	const double finest = maxFrequency > 0.0 ? m_planetRadius / maxFrequency : 0.0;
	const double smallest = std::max(finest, GetLandformShapeFrom());
	if (params.count <= 0 || smallest > top)
		return 0.0;
	TerrainLandforms::Params shape = params;
	shape.blocks = 0.0;
	return TerrainLandforms::Evaluate(shape, p.Normalized(), smallest, top, 0.5 * finest, GetLandformHeightFade(), GetLandformShadeFade()).shade;
}

void Terrain::ProtectSite(const SystemBody *starport, const vector3d &p)
{
	if (m_erosion)
		m_erosion->AddCity(p);
	SetFlatSite(starport, p);
}

void Terrain::AddStarportSites(const SystemBody *body)
{
	for (const SystemBody *kid : body->GetChildren())
		if (kid->GetType() == SystemBody::TYPE_STARPORT_SURFACE)
			SetFlatSite(kid, kid->GetOrbit().GetPlane() * vector3d(0.0, 1.0, 0.0));
}

void Terrain::SetFlatSite(const SystemBody *starport, const vector3d &p)
{
	// the type SpaceStation::InitStation gives the starport, from the same seed
	Random rand(starport->GetSeed());
	const SpaceStationType *type = SpaceStationType::ForSystemBody(starport, rand);
	if (!type || type->FlattenRadius() <= 0.0f)
		return;

	FlatSite site;
	site.starport = starport;
	site.pos = p.Normalized();
	{
		// the ground at full detail, like collision
		TerrainNoise::ScopedBandLimit bandLimit(0.0);
		GetBaseHeights(&site.pos, &site.height, 1);
	}
	const double flat = type->FlattenRadius() + SITE_MARGIN_M;
	site.flatChord = flat / m_planetRadius;
	site.blendChord = (flat + std::max(SITE_BLEND_MIN_M, flat)) / m_planetRadius;

	std::lock_guard<std::mutex> lock(m_flatSitesLock);
	auto sites = m_flatSites ? std::make_shared<std::vector<FlatSite>>(*m_flatSites) : std::make_shared<std::vector<FlatSite>>();
	auto same = std::find_if(sites->begin(), sites->end(), [starport](const FlatSite &s) { return s.starport == starport; });
	if (same != sites->end())
		*same = site;
	else
		sites->push_back(site);
	m_flatSites = std::move(sites);
}

std::shared_ptr<const std::vector<Terrain::FlatSite>> Terrain::FlatSites() const
{
	std::lock_guard<std::mutex> lock(m_flatSitesLock);
	return m_flatSites;
}

void Terrain::LevelSites(const vector3d *vP, double *heightsOut, const size_t count) const
{
	const std::shared_ptr<const std::vector<FlatSite>> sites = FlatSites();
	if (!sites)
		return;

	for (const FlatSite &site : *sites) {
		const double blendSqr = site.blendChord * site.blendChord;
		for (size_t i = 0; i < count; i++) {
			const double distanceSqr = (vP[i] - site.pos).LengthSqr();
			if (distanceSqr >= blendSqr)
				continue;
			// smooth from the level disc back to the natural ground
			const double t = Clamp((sqrt(distanceSqr) - site.flatChord) / (site.blendChord - site.flatChord), 0.0, 1.0);
			const double natural = t * t * (3.0 - 2.0 * t);
			heightsOut[i] = site.height + (heightsOut[i] - site.height) * natural;
		}
	}
}

/**
 * Feature width means roughly one perlin noise blob or grain.
 * This will end up being one hill, mountain or continent, roughly.
 */
void Terrain::SetFracDef(const unsigned int index, const double featureHeightMeters, const double featureWidthMeters, const double smallestOctaveMeters)
{
	assert(index < MAX_FRACDEFS);
	// feature
	m_fracdef[index].amplitude = featureHeightMeters / (m_maxHeight * m_planetRadius);
	m_fracdef[index].frequency = m_planetRadius / featureWidthMeters;
	m_fracdef[index].octaves = std::max(1, int(ceil(log(featureWidthMeters / smallestOctaveMeters) / log(2.0))));
	m_fracdef[index].lacunarity = 2.0;
	//Output("%d octaves\n", m_fracdef[index].octaves); //print
}

void Terrain::SetColorFracDef(const unsigned int index, const double featureWidthMeters, const double smallestOctaveMeters)
{
	assert(index < MAX_COLOR_FRACDEFS);
	// features wider than the body itself would only give a flat tint on small bodies
	const double width = std::min(featureWidthMeters, m_planetRadius);
	const double smallest = std::min(smallestOctaveMeters, width);
	m_colorFracdef[index].amplitude = 1.0;
	m_colorFracdef[index].frequency = m_planetRadius / width;
	m_colorFracdef[index].octaves = std::max(1, int(ceil(log(width / smallest) / log(2.0))));
	m_colorFracdef[index].lacunarity = 2.0;
}

void Terrain::AddPaletteMaterials()
{
	using M = TerrainMaterialTable;
	m_materials.Add(M::ROCK, m_rockColor, 8);
	m_materials.Add(M::GRAVEL, m_darkrockColor, 8);
	m_materials.Add(M::SAND, m_sandColor, 8);
	m_materials.Add(M::SOIL, m_darksandColor, 8);
	m_materials.Add(M::SOIL, m_dirtColor, 8);
	m_materials.Add(M::SOIL, m_darkdirtColor, 8);
	m_materials.Add(M::GRASS, m_plantColor, 8);
	m_materials.Add(M::FOREST, m_darkplantColor, 8);
	m_materials.Add(M::SNOW, { vector3d(1.0, 1.0, 1.0), vector3d(0.9, 0.9, 0.9) });
}

void Terrain::AddBarrenMaterials(const SystemBody *body)
{
	using M = TerrainMaterialTable;
	m_materials.Add(M::REGOLITH, m_rockColor, 8);
	m_materials.Add(body->GetVolatileGas() < 0.05 ? M::BASALT : M::GRAVEL, m_darkrockColor, 8);
	m_materials.Add(M::ROCK); // steep slopes
}

double Terrain::GetSurfaceAltitude(const vector3d &p, const double height) const
{
	if (height > 0.0 || !(m_surfaceEffects & EFFECT_WATER))
		return height * m_planetRadius;

	// Under water. The height fractals flatten the sea floor to sea level, so
	// only bodies with a height map (Earth) know the real depth; elsewhere use
	// a typical shelf depth, which the water shader shows as open sea.
	const double depth = m_heightMap ? -BiCubicInterpolation(p) : 60.0;
	return -Clamp(depth, 2.0, 8000.0);
}

double Terrain::BiCubicInterpolation(const vector3d &p) const
{
	double latitude = -asin(p.y);
	if (p.y < -1.0) latitude = -0.5 * M_PI;
	if (p.y > 1.0) latitude = 0.5 * M_PI;
	//	if (!isfinite(latitude)) {
	//		// p.y is just n of asin domain [-1,1]
	//		latitude = (p.y < 0 ? -0.5*M_PI : M_PI*0.5);
	//	}
	const double longitude = atan2(p.x, p.z);
	const double px = (((m_heightMapSizeX - 1) * (longitude + M_PI)) / (2 * M_PI));
	const double py = ((m_heightMapSizeY - 1) * (latitude + 0.5 * M_PI)) / M_PI;
	int ix = int(floor(px));
	int iy = int(floor(py));
	ix = Clamp(ix, 0, m_heightMapSizeX - 1);
	iy = Clamp(iy, 0, m_heightMapSizeY - 1);
	const double dx = px - ix;
	const double dy = py - iy;

	// p0,3 p1,3 p2,3 p3,3
	// p0,2 p1,2 p2,2 p3,2
	// p0,1 p1,1 p2,1 p3,1
	// p0,0 p1,0 p2,0 p3,0
	double map[4][4];
	const double *pHMap = m_heightMap.get();
	for (int x = -1; x < 3; x++) {
		for (int y = -1; y < 3; y++) {
			map[x + 1][y + 1] = pHMap[Clamp(iy + y, 0, m_heightMapSizeY - 1) * m_heightMapSizeX + Clamp(ix + x, 0, m_heightMapSizeX - 1)];
		}
	}

	// Catmull-Rom spline through the 4 samples, evaluated between p1 and p2.
	// Unlike Lagrange interpolation its slope is continuous across sample
	// boundaries, so the heightmap grid does not show up as creases.
	auto catmullRom = [](const double p0, const double p1, const double p2, const double p3, const double t) {
		const double a1 = 0.5 * (p2 - p0);
		const double a2 = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
		const double a3 = 0.5 * (p3 - p0) + 1.5 * (p1 - p2);
		return p1 + t * (a1 + t * (a2 + t * a3));
	};

	double c[4];
	for (int j = 0; j < 4; j++) {
		c[j] = catmullRom(map[0][j], map[1][j], map[2][j], map[3][j], dx);
	}

	return (0.1 + catmullRom(c[0], c[1], c[2], c[3], dy));
}

Terrain::MinBodyData::MinBodyData(const SystemBody *body)
{
	m_radius = body->GetRadius();
	m_radiusEarthRatio = body->GetRadiusAsFixed().ToDouble();
	m_aspectRatio = body->GetAspectRatio();
	m_path = body->GetPath();
	m_name = body->GetName();
}
