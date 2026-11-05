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
#include "terrain/Terrain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>

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
		const double height = position.Length() - place.planet->GetTerrainHeight(place.up);
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
			s.underShip = terrain ? OneLine(terrain->DescribePoint(place.up)) : std::string();
		} else if (!place.planet) {
			s.underShip.clear();
		}

		const int frames = std::max(s.frames, 1);
		WriteTime();
		std::fprintf(s.file, "%s | fps %d, frame %.1f ms, longest %.1f, %d spikes | physics %.2f, terrain update %.2f (patches in %.2f, choosing %.2f, far %.2f), view %.2f, draw %.2f, flush %.2f, gui %.2f, height queries %.2f ms (%lld a frame)",
			place.text.c_str(), s.frames, s.frameSum / frames, s.frameMax, s.spikes,
			s_secondParts[FrontierLog::PART_STEP] / frames, s_secondParts[FrontierLog::PART_SPHERES] / frames,
			s_secondParts[FrontierLog::PART_RESULTS] / frames, s_secondParts[FrontierLog::PART_LOD] / frames, s_secondParts[FrontierLog::PART_FAR] / frames,
			s_secondParts[FrontierLog::PART_VIEW] / frames, s_secondParts[FrontierLog::PART_DRAW] / frames, s_secondParts[FrontierLog::PART_FLUSH] / frames,
			s.guiSum / frames, s.querySum / frames, s.queryCallsSum / frames);

		const GeoPatchGenStats patches = GetGeoPatchGenStats();
		const uint64_t patchCount = patches.patches >= s.patches.patches ? patches.patches - s.patches.patches : patches.patches;
		const uint64_t patchMicros = patches.microseconds >= s.patches.microseconds ? patches.microseconds - s.patches.microseconds : patches.microseconds;
		s.patches = patches;
		std::fprintf(s.file, " | patches +%llu", (unsigned long long)patchCount);
		if (patchCount > 0)
			std::fprintf(s.file, " at %.1f ms", double(patchMicros) / double(patchCount) / 1000.0);
		if (const TerrainSurfaceTiles *tiles = TerrainSurfaceTiles::Instance())
			std::fprintf(s.file, ", tiles %d (%d baking)", tiles->GetTileCount(), tiles->GetBakingCount());

		// (the debug window's reset button can take the totals below the last ones)
		const Terrain::SpeedTotals speed = Terrain::GetSpeedTotals();
		const auto since = [](const uint64_t now, const uint64_t before) { return now >= before ? now - before : now; };
		const uint64_t points = since(speed.pointCalls, s.speed.pointCalls);
		if (points > 0) {
			const uint64_t valleyCalls = since(speed.valleyCalls, s.speed.valleyCalls);
			const uint64_t paintCalls = since(speed.paintCalls, s.speed.paintCalls);
			std::fprintf(s.file, " | point %.1f us, valleys %.1f, materials %.1f, chunks +%llu in %.0f ms",
				double(since(speed.pointNanos, s.speed.pointNanos)) / double(points) / 1000.0,
				valleyCalls ? double(since(speed.valleyNanos, s.speed.valleyNanos)) / double(valleyCalls) / 1000.0 : 0.0,
				paintCalls ? double(since(speed.paintNanos, s.speed.paintNanos)) / double(paintCalls) / 1000.0 : 0.0,
				(unsigned long long)since(speed.chunks, s.speed.chunks), double(since(speed.chunkMicros, s.speed.chunkMicros)) / 1000.0);
		}
		s.speed = speed;

		for (const Graphics::Renderer::GpuTiming &t : Pi::renderer->GetGpuTimings()) {
			if (t.name == "terrain")
				std::fprintf(s.file, " | gpu terrain %.2f ms", t.milliseconds);
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
		std::fill(std::begin(s_secondParts), std::end(s_secondParts), 0.0);
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

void FrontierLog::EndFrame(const double frameMs, const double physicsMs, const double guiMs)
{
	const double queryMs = double(s_queryNanos.exchange(0, std::memory_order_relaxed)) / 1e6;
	const int queryCalls = s_queryCalls.exchange(0, std::memory_order_relaxed);
	State &s = s_state;
	if (Ready()) {
		const bool spike = s.usual > 0.0 && frameMs > s.usual * SPIKE_RATIO && frameMs > s.usual + SPIKE_ABOVE_MS;
		if (spike) {
			s.spikes++;
			if (s.spikesWritten < SPIKES_PER_SECOND) {
				s.spikesWritten++;
				// what's left over is mostly waiting for the screen or the graphics driver
				const double known = physicsMs + guiMs + s_parts[PART_VIEW] + s_parts[PART_DRAW] + s_parts[PART_FLUSH];
				WriteTime();
				std::fprintf(s.file, "SPIKE %.1f ms (usual %.1f): physics %.1f (step %.1f, terrain update %.1f: patches in %.1f, choosing %.1f, far %.1f), view %.1f, draw %.1f, flush %.1f, gui %.1f, height queries %.1f (%d), left over %.1f\n",
					frameMs, s.usual, physicsMs, s_parts[PART_STEP], s_parts[PART_SPHERES], s_parts[PART_RESULTS], s_parts[PART_LOD], s_parts[PART_FAR],
					s_parts[PART_VIEW], s_parts[PART_DRAW], s_parts[PART_FLUSH], guiMs, queryMs, queryCalls, std::max(frameMs - known, 0.0));
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
		const long long now = NowNanos();
		if (now - s.secondStart >= 1000000000LL) {
			s.secondStart = now;
			WriteSecond();
		}
	}
	std::fill(std::begin(s_parts), std::end(s_parts), 0.0);
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
