// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "FrontierLog.h"

#include "FileSystem.h"
#include "Frame.h"
#include "Game.h"
#include "GameConfig.h"
#include "GeoPatchJobs.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Planet.h"
#include "Player.h"
#include "TerrainFarTexture.h"
#include "TerrainSurfaceTiles.h"
#include "graphics/Renderer.h"
#include "galaxy/SystemBody.h"
#include "terrain/Terrain.h"
#include "terrain/TerrainNoise.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <vector>

namespace {
	constexpr int SPIKES_PER_SECOND = 30;
	constexpr double SPIKE_RATIO = 1.5; // a frame this many times the usual is a spike...
	constexpr double SPIKE_ABOVE_MS = 3.0; // ...if it's also this much longer

	long long NowNanos()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	double s_parts[FrontierLog::PART_COUNT] = {}; // ms, this frame
	double s_secondParts[FrontierLog::PART_COUNT] = {}; // ms, this second
	std::atomic<long long> s_queryNanos(0);
	std::atomic<int> s_queryCalls(0);
	long long s_lastFrameEnd = 0; // when the last frame's update was over, for how long this frame took
	long long s_jobsStart = 0, s_swapStart = 0;

	// a GPU timer's times this second
	struct GpuScope {
		std::string name;
		double sum = 0.0, longest = 0.0;
		int count = 0;
	};

	struct State {
		FILE *file = nullptr;
		bool triedOpening = false;
		long long startNanos = 0;
		long long secondStart = 0;
		double usual = 0.0; // a slow average of frame times without the spikes
		int frames = 0;
		int spikes = 0, spikesWritten = 0;
		double frameSum = 0.0, frameMax = 0.0, physicsSum = 0.0, guiSum = 0.0, querySum = 0.0;
		long long queryCallsSum = 0;
		GeoPatchGenStats patches;
		Terrain::SpeedTotals speed;
		const Body *body = nullptr;
		vector3d lastUp = vector3d(0.0, 0.0, 0.0);
		std::string underShip;
		std::string farBake;
		std::vector<GpuScope> gpuScopes;
	};
	State s_state;

	void OpenFile()
	{
		s_state.triedOpening = true;
		const std::string root = FileSystem::userFiles.GetRoot();
		const std::string current = FileSystem::JoinPathBelow(root, "frontier_log.txt");
		const std::string previous = FileSystem::JoinPathBelow(root, "frontier_log_previous.txt");
		std::remove(previous.c_str());
		std::rename(current.c_str(), previous.c_str());
		s_state.file = FileSystem::userFiles.OpenWriteStream("frontier_log.txt", FileSystem::FileSourceFS::WRITE_TEXT);
		s_state.startNanos = NowNanos();
		s_state.secondStart = s_state.startNanos;
	}

	bool Ready()
	{
		if (!FrontierLog::Enabled())
			return false;
		if (!s_state.triedOpening)
			OpenFile();
		return s_state.file != nullptr;
	}

	// the wall clock, to match screenshots, and seconds since the log began
	void WriteTime()
	{
		const std::time_t now = std::time(nullptr);
		char clock[16] = "";
		std::strftime(clock, sizeof(clock), "%H:%M:%S", std::localtime(&now));
		std::fprintf(s_state.file, "%s %9.3f ", clock, double(NowNanos() - s_state.startNanos) / 1e9);
	}

	struct Place {
		const Planet *planet = nullptr;
		vector3d up = vector3d(0.0, 0.0, 0.0);
		double height = 0.0; // metres above the ground
		std::string text;
	};

	// the ship's place on the body whose frame it is in, if it's a planet or moon
	Place WhereIsTheShip()
	{
		Place place;
		if (!Pi::player || !Pi::game || Pi::player->GetFlightState() == Ship::HYPERSPACE) {
			place.text = "not in a system";
			return place;
		}
		const Frame *frame = Frame::GetFrame(Pi::player->GetFrame());
		const Body *body = frame ? frame->GetBody() : nullptr;
		if (!body || !body->IsType(ObjectType::PLANET)) {
			place.text = "in space";
			return place;
		}
		place.planet = static_cast<const Planet *>(body);
		const FrameId rot = frame->GetRotFrame();
		const vector3d position = Pi::player->GetPositionRelTo(rot);
		place.up = position.Normalized();
		const double latitude = std::asin(std::clamp(place.up.y, -1.0, 1.0)) * 180.0 / M_PI;
		const double longitude = std::atan2(place.up.x, place.up.z) * 180.0 / M_PI;
		const double height = position.Length() - place.planet->GetTerrainHeightFrom(place.up, position.Length());
		place.height = height;
		const char *state = "flying";
		switch (Pi::player->GetFlightState()) {
		case Ship::DOCKED: state = "docked"; break;
		case Ship::LANDED: state = "landed"; break;
		case Ship::DOCKING: state = "docking"; break;
		case Ship::UNDOCKING: state = "undocking"; break;
		default: break;
		}
		char line[160];
		std::snprintf(line, sizeof(line), "%s lat %.4f lon %.4f, %s %.0f m above ground at %.0f m/s", body->GetLabel().c_str(), latitude, longitude, state, height, Pi::player->GetVelocityRelTo(rot).Length());
		place.text = line;
		return place;
	}

	std::string OneLine(std::string text)
	{
		std::replace(text.begin(), text.end(), '\n', ';');
		return text;
	}

	void WriteGeology(const Planet *planet)
	{
		const Terrain *terrain = planet->GetTerrain();
		if (!terrain)
			return;
		WriteTime();
		std::fprintf(s_state.file, "BODY %s: height %s, colour %s\n", planet->GetLabel().c_str(), terrain->GetHeightFractalName(), terrain->GetColorFractalName());
		for (const std::string &line : terrain->GetGeologyInfo())
			std::fprintf(s_state.file, "    %s\n", line.c_str());
	}

	void WriteSecond()
	{
		State &s = s_state;
		const Place place = WhereIsTheShip();
		if (place.planet != s.body) {
			s.body = place.planet;
			if (place.planet)
				WriteGeology(place.planet);
		}
		// only when the ship has moved, so a ship standing still isn't asking the terrain for anything
		if (place.planet && (place.up - s.lastUp).LengthSqr() > 1e-14) {
			s.lastUp = place.up;
			const Terrain *terrain = place.planet->GetTerrain();
			// at the detail that can be seen from here, so that this log doesn't hold up the game
			const double spacing = place.height / 20.0;
			TerrainNoise::ScopedBandLimit bandLimit(spacing > 1.0 ? 0.5 * place.planet->GetSystemBody()->GetRadius() / spacing : 0.0);
			s.underShip = terrain ? OneLine(terrain->DescribePoint(place.up)) : std::string();
		} else if (!place.planet) {
			s.underShip.clear();
		}

		const int frames = std::max(s.frames, 1);
		WriteTime();
		std::fprintf(s.file, "%s | accel %.0fx | fps %d, frame %.1f ms, longest %.1f, %d spikes | physics %.2f, terrain update %.2f (patches in %.2f, choosing %.2f, far %.2f), view %.2f, draw %.2f, flush %.2f, gui %.2f, jobs %.2f, swap %.2f, log %.2f, height queries %.2f ms (%lld a frame)",
			place.text.c_str(), Pi::game ? double(Pi::game->GetTimeAccelRate()) : 1.0, s.frames, s.frameSum / frames, s.frameMax, s.spikes,
			s_secondParts[FrontierLog::PART_STEP] / frames, s_secondParts[FrontierLog::PART_SPHERES] / frames,
			s_secondParts[FrontierLog::PART_RESULTS] / frames, s_secondParts[FrontierLog::PART_LOD] / frames, s_secondParts[FrontierLog::PART_FAR] / frames,
			s_secondParts[FrontierLog::PART_VIEW] / frames, s_secondParts[FrontierLog::PART_DRAW] / frames, s_secondParts[FrontierLog::PART_FLUSH] / frames,
			s.guiSum / frames, s_secondParts[FrontierLog::PART_JOBS] / frames, s_secondParts[FrontierLog::PART_SWAP] / frames, s_secondParts[FrontierLog::PART_LOG] / frames,
			s.querySum / frames, s.queryCallsSum / frames);

		const GeoPatchGenStats patches = GetGeoPatchGenStats();
		const uint64_t patchCount = patches.patches >= s.patches.patches ? patches.patches - s.patches.patches : patches.patches;
		const uint64_t patchMicros = patches.microseconds >= s.patches.microseconds ? patches.microseconds - s.patches.microseconds : patches.microseconds;
		s.patches = patches;
		std::fprintf(s.file, " | patches +%llu", (unsigned long long)patchCount);
		if (patchCount > 0)
			std::fprintf(s.file, " at %.1f ms", double(patchMicros) / double(patchCount) / 1000.0);
		if (const TerrainSurfaceTiles *tiles = TerrainSurfaceTiles::Instance())
			std::fprintf(s.file, ", tiles %d (%d baking, %d waiting)", tiles->GetTileCount(), tiles->GetBakingCount(), tiles->GetWaitingCount());

		// (the debug window's reset button can take the totals below the last ones)
		const Terrain::SpeedTotals speed = Terrain::GetSpeedTotals();
		const auto since = [](const uint64_t now, const uint64_t before) { return now >= before ? now - before : now; };
		const uint64_t points = since(speed.pointCalls, s.speed.pointCalls);
		if (points > 0) {
			const uint64_t valleyCalls = since(speed.valleyCalls, s.speed.valleyCalls);
			const uint64_t paintCalls = since(speed.paintCalls, s.speed.paintCalls);
			std::fprintf(s.file, " | point %.1f us, valleys %.1f, materials %.1f, chunks +%llu in %.0f ms over all threads, by level",
				double(since(speed.pointNanos, s.speed.pointNanos)) / double(points) / 1000.0,
				valleyCalls ? double(since(speed.valleyNanos, s.speed.valleyNanos)) / double(valleyCalls) / 1000.0 : 0.0,
				paintCalls ? double(since(speed.paintNanos, s.speed.paintNanos)) / double(paintCalls) / 1000.0 : 0.0,
				(unsigned long long)since(speed.chunks, s.speed.chunks), double(since(speed.chunkMicros, s.speed.chunkMicros)) / 1000.0);
			for (size_t level = 0; level < std::size(speed.chunksAtLevel); level++)
				std::fprintf(s.file, " %llu", (unsigned long long)since(speed.chunksAtLevel[level], s.speed.chunksAtLevel[level]));
		}
		s.speed = speed;

		for (const GpuScope &g : s.gpuScopes) {
			if (g.name == "terrain")
				std::fprintf(s.file, " | gpu terrain %.2f ms", g.sum / g.count);
		}
		if (!s.gpuScopes.empty()) {
			std::fputs(" | gpu ms, mean/longest:", s.file);
			for (const GpuScope &g : s.gpuScopes)
				std::fprintf(s.file, " %s %.2f/%.2f,", g.name.c_str(), g.sum / g.count, g.longest);
		}
		if (!s.underShip.empty())
			std::fprintf(s.file, " | under: %s", s.underShip.c_str());
		std::fputc('\n', s.file);

		const std::string &bake = TerrainFarTexture::GetLastBakeInfo();
		if (bake != s.farBake) {
			s.farBake = bake;
			WriteTime();
			std::fprintf(s.file, "FAR TEXTURE %s\n", bake.c_str());
		}
		std::fflush(s.file);

		s.frames = 0;
		s.spikes = 0;
		s.spikesWritten = 0;
		s.frameSum = s.frameMax = s.physicsSum = s.guiSum = s.querySum = 0.0;
		s.queryCallsSum = 0;
		s.gpuScopes.clear();
		std::fill(std::begin(s_secondParts), std::end(s_secondParts), 0.0);
	}

	void AddGpuTimes()
	{
		for (const Graphics::Renderer::GpuTiming &t : Pi::renderer->GetGpuTimings()) {
			auto it = std::find_if(s_state.gpuScopes.begin(), s_state.gpuScopes.end(), [&t](const GpuScope &g) { return g.name == t.name; });
			if (it == s_state.gpuScopes.end()) {
				s_state.gpuScopes.emplace_back();
				it = s_state.gpuScopes.end() - 1;
				it->name = t.name;
			}
			it->sum += t.milliseconds;
			it->longest = std::max(it->longest, t.milliseconds);
			it->count++;
		}
	}
} // namespace

FrontierLog::Timed::Timed(const Part part) :
	m_part(part),
	m_start(NowNanos())
{}

FrontierLog::Timed::~Timed()
{
	s_parts[m_part] += double(NowNanos() - m_start) / 1e6;
}

FrontierLog::TerrainQuery::TerrainQuery() :
	m_start(NowNanos())
{}

FrontierLog::TerrainQuery::~TerrainQuery()
{
	s_queryNanos.fetch_add(NowNanos() - m_start, std::memory_order_relaxed);
	s_queryCalls.fetch_add(1, std::memory_order_relaxed);
}

bool FrontierLog::Enabled()
{
	return Pi::config->Int("FrontierLog", 1) != 0;
}

void FrontierLog::SetEnabled(const bool enabled)
{
	Pi::config->SetInt("FrontierLog", enabled ? 1 : 0);
	Pi::config->Save();
}

void FrontierLog::BeforeSwap()
{
	const long long now = NowNanos();
	if (s_jobsStart != 0)
		s_parts[PART_JOBS] += double(now - s_jobsStart) / 1e6;
	s_jobsStart = 0; // only the game's frames set it
	s_swapStart = now;
}

void FrontierLog::AfterSwap()
{
	if (s_swapStart != 0)
		s_parts[PART_SWAP] += double(NowNanos() - s_swapStart) / 1e6;
	s_swapStart = 0;
}

void FrontierLog::EndFrame(const double physicsMs, const double guiMs)
{
	// from the end of the last frame's update, with what came after it, as the game's own
	// frame time at this point is the last frame's
	const long long frameEnd = NowNanos();
	const double frameMs = s_lastFrameEnd != 0 ? double(frameEnd - s_lastFrameEnd) / 1e6 : 0.0;
	s_lastFrameEnd = frameEnd;
	const double queryMs = double(s_queryNanos.exchange(0, std::memory_order_relaxed)) / 1e6;
	const int queryCalls = s_queryCalls.exchange(0, std::memory_order_relaxed);
	State &s = s_state;
	if (Ready()) {
		const bool spike = s.usual > 0.0 && frameMs > s.usual * SPIKE_RATIO && frameMs > s.usual + SPIKE_ABOVE_MS;
		if (spike) {
			s.spikes++;
			if (s.spikesWritten < SPIKES_PER_SECOND) {
				s.spikesWritten++;
				const double known = physicsMs + guiMs + s_parts[PART_VIEW] + s_parts[PART_DRAW] + s_parts[PART_FLUSH] + s_parts[PART_JOBS] + s_parts[PART_SWAP] + s_parts[PART_LOG];
				WriteTime();
				std::fprintf(s.file, "SPIKE %.1f ms (usual %.1f): physics %.1f (step %.1f, terrain update %.1f: patches in %.1f, choosing %.1f, far %.1f), view %.1f, draw %.1f, flush %.1f, gui %.1f, jobs %.1f, swap %.1f, log %.1f, height queries %.1f (%d), left over %.1f\n",
					frameMs, s.usual, physicsMs, s_parts[PART_STEP], s_parts[PART_SPHERES], s_parts[PART_RESULTS], s_parts[PART_LOD], s_parts[PART_FAR],
					s_parts[PART_VIEW], s_parts[PART_DRAW], s_parts[PART_FLUSH], guiMs, s_parts[PART_JOBS], s_parts[PART_SWAP], s_parts[PART_LOG], queryMs, queryCalls, std::max(frameMs - known, 0.0));
			}
		} else {
			s.usual = s.usual > 0.0 ? s.usual + 0.02 * (frameMs - s.usual) : frameMs;
		}
		s.frames++;
		s.frameSum += frameMs;
		s.frameMax = std::max(s.frameMax, frameMs);
		s.physicsSum += physicsMs;
		s.guiSum += guiMs;
		s.querySum += queryMs;
		s.queryCallsSum += queryCalls;
		for (int i = 0; i < PART_COUNT; i++)
			s_secondParts[i] += s_parts[i];
		AddGpuTimes();
		const long long now = NowNanos();
		if (now - s.secondStart >= 1000000000LL) {
			s.secondStart = now;
			WriteSecond();
		}
	}
	std::fill(std::begin(s_parts), std::end(s_parts), 0.0);
	// what this cost goes with the next frame, which it holds up
	s_jobsStart = NowNanos();
	s_parts[PART_LOG] = double(s_jobsStart - frameEnd) / 1e6;
}

void FrontierLog::Note(const std::string &text)
{
	if (!Ready())
		return;
	WriteTime();
	std::fprintf(s_state.file, "NOTE %s\n", text.c_str());
	std::fflush(s_state.file);
}

void FrontierLog::Mark(const std::string &comment)
{
	if (!Ready())
		return;
	const Place place = WhereIsTheShip();
	WriteTime();
	std::fprintf(s_state.file, "MARK %s | %s\n", comment.c_str(), place.text.c_str());
	if (place.planet) {
		if (const Terrain *terrain = place.planet->GetTerrain())
			std::fprintf(s_state.file, "    under: %s\n", OneLine(terrain->DescribePoint(place.up)).c_str());
		WriteGeology(place.planet);
	}
	std::fflush(s_state.file);
}

void FrontierLog::HeightProfile()
{
	if (!Ready())
		return;
	const Place place = WhereIsTheShip();
	const Terrain *terrain = place.planet ? place.planet->GetTerrain() : nullptr;
	Terrain::HeightLayers layers;
	if (!terrain || !terrain->GetHeightLayers(place.up, layers)) {
		Note("height profile: this ground doesn't say what its height is made of");
		return;
	}
	const long long start = NowNanos();
	const double radius = place.planet->GetSystemBody()->GetRadius();

	// (bearings from north, as the HUD has them)
	vector3d north = vector3d(0.0, 1.0, 0.0) - place.up * place.up.y;
	if (north.LengthSqr() < 1e-12)
		north = vector3d(0.0, 0.0, 1.0) - place.up * place.up.z;
	north = north.Normalized();
	const vector3d east = north.Cross(place.up);

	WriteTime();
	std::fprintf(s_state.file, "HEIGHT PROFILE %s: the sharpest step along each bearing, in metres, the steps either side of it, and each layer's share\n", place.text.c_str());

	struct Walk {
		double step; // metres
		int count;
		double spacing; // metres: the detail of a mesh of this spacing; 0: all of it
	};
	static const Walk WALKS[] = { { 1.0, 2000, 0.0 }, { 10.0, 5000, 0.0 }, { 16.0, 2000, 16.0 } };
	constexpr int BEARINGS = 16;
	std::vector<Terrain::HeightLayers> samples;
	std::vector<double> meshHeights; // metres: what the patches get, with what is added after the layers
	for (const Walk &walk : WALKS) {
		TerrainNoise::ScopedBandLimit bandLimit(walk.spacing > 0.0 ? 0.5 * radius / walk.spacing : 0.0);
		samples.resize(walk.count + 1);
		meshHeights.resize(walk.count + 1);
		for (int b = 0; b < BEARINGS; b++) {
			const double bearing = 2.0 * M_PI * b / BEARINGS;
			const vector3d direction = north * std::cos(bearing) + east * std::sin(bearing);
			const auto placeAt = [&](const int i) {
				const double angle = i * walk.step / radius;
				return place.up * std::cos(angle) + direction * std::sin(angle);
			};
			for (int i = 0; i <= walk.count; i++) {
				const vector3d p = placeAt(i);
				terrain->GetHeightLayers(p, samples[i]);
				double height = 0.0;
				terrain->GetHeights(&p, &height, 1);
				meshHeights[i] = height * radius;
			}

			int sharpest = 1;
			for (int i = 1; i <= walk.count; i++) {
				if (std::abs(meshHeights[i] - meshHeights[i - 1]) > std::abs(meshHeights[sharpest] - meshHeights[sharpest - 1]))
					sharpest = i;
			}
			const Terrain::HeightLayers &a = samples[sharpest - 1], &c = samples[sharpest];
			const double before = sharpest >= 2 ? meshHeights[sharpest - 1] - meshHeights[sharpest - 2] : 0.0;
			const double after = sharpest < walk.count ? meshHeights[sharpest + 1] - meshHeights[sharpest] : 0.0;
			const vector3d at = placeAt(sharpest);
			const double shape = c.shape - a.shape, largeCraters = c.largeCraters - a.largeCraters, smallCraters = c.smallCraters - a.smallCraters;
			const double relief = c.smallRelief - a.smallRelief, lines = c.lines - a.lines, maps = c.maps - a.maps, volcano = c.volcano - a.volcano;
			std::fprintf(s_state.file, "    bearing %5.1f, %g m steps, %s: %.0f m out, lat %.5f lon %.5f: %+.3f (%+.3f, %+.3f); before erosion and sites %+.3f; shape %+.3f, large craters %+.3f, small craters %+.3f, small relief %+.3f, lines %+.3f, maps %+.3f, volcano %+.3f\n",
				bearing * 180.0 / M_PI, walk.step, walk.spacing > 0.0 ? "mesh detail" : "all detail", sharpest * walk.step,
				std::asin(std::clamp(at.y, -1.0, 1.0)) * 180.0 / M_PI, std::atan2(at.x, at.z) * 180.0 / M_PI,
				meshHeights[sharpest] - meshHeights[sharpest - 1], before, after, c.total - a.total, shape, largeCraters, smallCraters, relief, lines, maps, volcano);
			// a real step, far sharper than the ground either side, in the craters: which ones
			const double stepHeight = meshHeights[sharpest] - meshHeights[sharpest - 1];
			const auto describe = [&](const int i) {
				const vector3d p0 = placeAt(i - 1).Normalized(), p1 = placeAt(i).Normalized();
				for (const std::string &line : terrain->DescribeCraterStep(p0, p1))
					std::fprintf(s_state.file, "        %s\n", line.c_str());
				for (const std::string &line : terrain->DescribeLineStep(p0, p1))
					std::fprintf(s_state.file, "        %s\n", line.c_str());
			};
			if (std::abs(stepHeight) >= 1.0 && std::abs(stepHeight) > 3.0 * std::max(std::abs(before), std::abs(after)))
				describe(sharpest);

			// The step that stands out most from the ground either side, however small: a thin line on the ground
			// can be a few centimetres, far less than the slope of a crater's wall
			if (walk.step == 1.0) {
				int isolated = 0;
				double best = 0.005;
				for (int i = 2; i < walk.count; i++) {
					const double around = std::max(std::abs(meshHeights[i - 1] - meshHeights[i - 2]), std::abs(meshHeights[i + 1] - meshHeights[i]));
					const double score = std::abs(meshHeights[i] - meshHeights[i - 1]) - 3.0 * around;
					if (score > best) {
						best = score;
						isolated = i;
					}
				}
				if (isolated > 0 && isolated != sharpest) {
					const vector3d there = placeAt(isolated);
					std::fprintf(s_state.file, "    bearing %5.1f, isolated step: %d m out, lat %.5f lon %.5f: %+.3f (%+.3f, %+.3f)\n",
						bearing * 180.0 / M_PI, isolated, std::asin(std::clamp(there.y, -1.0, 1.0)) * 180.0 / M_PI, std::atan2(there.x, there.z) * 180.0 / M_PI,
						meshHeights[isolated] - meshHeights[isolated - 1], meshHeights[isolated - 1] - meshHeights[isolated - 2], meshHeights[isolated + 1] - meshHeights[isolated]);
					describe(isolated);
				}
			}
		}
	}
	WriteTime();
	std::fprintf(s_state.file, "HEIGHT PROFILE done in %.0f ms\n", double(NowNanos() - start) / 1e6);
	std::fflush(s_state.file);
}
