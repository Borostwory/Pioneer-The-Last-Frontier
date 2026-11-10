// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "PerfInfo.h"
#include "FrontierLog.h"
#include "Frame.h"
#include "Game.h"
#include "GameConfig.h"
#include "GeoPatch.h"
#include "GeoPatchContext.h"
#include "GeoPatchJobs.h"
#include "GeoSphere.h"
#include "Input.h"
#include "LuaPiGui.h"
#include "Planet.h"
#include "Pi.h"
#include "Player.h"
#include "ScreenEffects.h"
#include "SectorView.h"
#include "Space.h"
#include "TerrainSurfaceTiles.h"
#include "Random.h"
#include "galaxy/Galaxy.h"
#include "galaxy/Sector.h"
#include "galaxy/StarSystem.h"
#include "galaxy/SystemBody.h"
#include "terrain/Terrain.h"
#include "terrain/TerrainNoise.h"
#include "core/Log.h"
#include "graphics/Material.h"
#include "graphics/Renderer.h"
#include "graphics/VertexBuffer.h"
#include "graphics/Stats.h"
#include "graphics/Texture.h"
#include "lua/Lua.h"
#include "lua/LuaManager.h"
#include "scenegraph/Model.h"
#include "JsonUtils.h"
#include "FileSystem.h"

#include <fmt/core.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
// order of header includes matters, thanks Windows.h!
#include <psapi.h>
#endif

// using namespace PiGui;
using PerfInfo = PiGui::PerfInfo;

static constexpr double scale_MB = 1024.0 * 1024.0;

struct PerfInfo::ImGuiState {
	bool perfWindowOpen = true;
	bool updatePause = false;
	bool metricsWindowOpen = false;
	bool stackToolOpen = false;
	uint32_t playerModelDebugFlags = 0;

	bool textureCacheViewerOpen = false;

	std::map<std::string, std::vector<std::string>> indirectionMap;

	bool hasSelectedTexture = false;
	std::pair<std::string, std::string> selectedTexture;
};

PerfInfo::CounterInfo::CounterInfo(const char *n, const char *u, uint32_t recent) :
	name(n),
	unit(u),
	numRecentSamples(recent)
{
	history.fill(0.f);
}

PerfInfo::PerfInfo() :
	m_fpsCounter("Frame Time", "ms"),
	m_physCounter("Update Time", "ms"),
	m_piguiCounter("PiGui Time", "ms"),
	m_procMemCounter("Process memory usage", "MB", 1),
	m_luaMemCounter("Lua memory usage", "MB", 1),
	m_state(new ImGuiState({}))
{
}

PerfInfo::~PerfInfo()
{
	delete m_state;
}

// ============================================================================
//
// Hardware Performance Information
//
// ============================================================================

#define ignoreLine(f) f.ignore(std::numeric_limits<std::streamsize>::max(), '\n')
static PerfInfo::MemoryInfo GetMemoryInfo()
{
	PerfInfo::MemoryInfo ret{};
#if __linux__
	std::ifstream statusFile("/proc/self/status");
	while (statusFile.good() && !statusFile.eof()) {
		if (statusFile.peek() != 'V') {
			ignoreLine(statusFile);
			continue;
		}

		char statName[16] = { 0 };
		statusFile.getline(statName, 16, ':');

		if (strcmp(statName, "VmHWM") == 0) {
			statusFile >> ret.peakMemSize;
		} else if (strcmp(statName, "VmRSS") == 0) {
			statusFile >> ret.currentMemSize;
		}

		ignoreLine(statusFile);
	}
	statusFile.close();

#elif _WIN32
	// Get win32 memory count
	PROCESS_MEMORY_COUNTERS pmc;
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
		// convert from bytes to kilobytes
		ret.peakMemSize = pmc.PeakWorkingSetSize / 1024;
		ret.currentMemSize = pmc.WorkingSetSize / 1024;
	}
#elif __APPLE__
	// TODO: get OSX memory count
#endif

	return ret;
}
#undef ignoreLine

PerfInfo::CounterInfo &PerfInfo::GetCounter(CounterType ct)
{
	switch (ct) {
	case COUNTER_FPS: return m_fpsCounter;
	case COUNTER_PHYS: return m_physCounter;
	case COUNTER_PIGUI: return m_piguiCounter;
	case COUNTER_PROCMEM: return m_procMemCounter;
	case COUNTER_LUAMEM: return m_luaMemCounter;
	// default value is never reached, calm down -Werror=return-type
	default: return m_fpsCounter;
	}
}

void PerfInfo::ClearCounter(CounterType ct)
{
	CounterInfo &counter = GetCounter(ct);
	counter.history.fill(0.);
	counter.average = 0.;
	counter.max = 0.;
	counter.min = 0.;
}

void PerfInfo::UpdateCounter(CounterType ct, float sample)
{
	// Don't accumulate new frames when performance data is paused.
	if (m_state->updatePause)
		return;

	CounterInfo &counter = GetCounter(ct);

	// Index of the first "recent" sample in the history buffer
	size_t recentSamplesIdx = counter.history.size() - counter.numRecentSamples;
	float oldestSample = counter.history.front();

	// Recalculate average values for the history range
	counter.average -= oldestSample / float(NUM_FRAMES);
	counter.average += sample / float(NUM_FRAMES);
	counter.recent -= counter.history[recentSamplesIdx] / float(counter.numRecentSamples);
	counter.recent += sample / float(counter.numRecentSamples);

	// Drop the oldest frame, append the new frame.
	std::move(counter.history.begin() + 1, counter.history.end(), counter.history.begin());
	counter.history[NUM_FRAMES - 1] = sample;

	// The stored min/max value is invalidated if:
	// 1. The new sample lies outside the existing range
	// 2. The retired sample is exactly equal to the current minimum or maximum value
	// If 1) is true, we can just widen the range covered by min/max as we
	// already know all samples in the array lie within the existing range.
	// If 2) is true, we have to rescan the array to find the new min/max value.
	// There's no way around this.
	if (oldestSample == counter.min || oldestSample == counter.max) {
		counter.min = FLT_MAX;
		counter.max = FLT_MIN;
		std::for_each(counter.history.begin(), counter.history.end(), [&](float i) {
			counter.max = std::max(counter.max, i);
			counter.min = std::min(counter.min, i);
		});
	} else {
		counter.min = std::min(counter.min, sample);
		counter.max = std::max(counter.max, sample);
	}

	if (sample < counter.min)
		counter.min = sample;
	if (sample > counter.max)
		counter.max = sample;
}

void PerfInfo::Update(float deltaTime)
{
	UpdateCounter(COUNTER_FPS, deltaTime * 1e3);

	lastUpdateTime += deltaTime;
	constexpr double update_rate = 0.5;
	if (lastUpdateTime > update_rate) {
		lastUpdateTime = fmod(lastUpdateTime, update_rate);

		lua_mem = ::Lua::manager->GetMemoryUsage();
		process_mem = GetMemoryInfo();

		UpdateCounter(COUNTER_PROCMEM, process_mem.currentMemSize * 1.0e-3);
		UpdateCounter(COUNTER_LUAMEM, double(lua_mem) / scale_MB);
	}
}

// TODO: evaluate whether this method of tracking FPS is necessary.
void PerfInfo::UpdateFrameInfo(int fS, int pfS)
{
	framesThisSecond = fS;
	physFramesThisSecond = pfS;
}

void PerfInfo::SetUpdatePause(bool pause)
{
	m_state->updatePause = pause;
}

// ============================================================================
//
// Main Performance Window
//
// ============================================================================

namespace ImGui {
	IMGUI_API void Value(const char *prefix, const std::string &str)
	{
		ImGui::TextWrapped("%s: %s", prefix, str.c_str());
	}
} // namespace ImGui

void PerfInfo::Draw()
{
	if (m_state->perfWindowOpen)
		DrawPerfWindow();

	if (m_state->textureCacheViewerOpen)
		DrawTextureCache();

	if (m_state->hasSelectedTexture &&
		Pi::renderer->GetCachedTexture(m_state->selectedTexture.first, m_state->selectedTexture.second))
		DrawTextureInspector();

	if (m_state->metricsWindowOpen)
		ImGui::ShowMetricsWindow(&m_state->metricsWindowOpen);

	if (m_state->stackToolOpen)
		ImGui::ShowIDStackToolWindow(&m_state->stackToolOpen);
}

// Icons are found in the file /data/icons/icons.svg
// The value below can be calculated like this: \uF0A4
// \uF0 ignore this part
// A == 10 zero index based so the 11th row of icons.svg
// 4 == 4 zero index based so the 5th column of icons.svg
static const char *s_rendererIcon = u8"\uF082";
static const char *s_worldIcon = u8"\uF092";
static const char *s_perfIcon = u8"\uF0F0";
static const char *s_planetIcon = u8"\uF0A0";

bool BeginDebugTab(const char *icon, const char *label)
{
	bool ret = ImGui::BeginTabItem(icon);
	ImGui::SetItemTooltip("%s", label);

	if (ret) {
		ImGui::BeginChild(label, ImVec2(0, 0), false,
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_HorizontalScrollbar |
			ImGuiWindowFlags_NoBackground);
	}

	return ret;
}

void EndDebugTab()
{
	ImGui::EndChild();
	ImGui::EndTabItem();
}

void PerfInfo::DrawCounter(CounterInfo &counter, const char *label, float min, float max, float height, bool drawStats)
{
	if (drawStats) {
		std::string line1 = fmt::format("{}: {:.1f} {}", counter.name, counter.recent, counter.unit);
		std::string line2 = fmt::format("Min: {:.1f} {} | Avg: {:.1f} {} | Max: {:.1f} {}",
			counter.min, counter.unit, counter.average, counter.unit, counter.max, counter.unit);

		ImGui::TextUnformatted(line1.c_str());
		ImGui::TextUnformatted(line2.c_str());
	}

	if (min == 0.0 && max == 0.0) {
		int l2min = floor(log2(counter.min));
		int l2max = ceil(log2(counter.max));

		min = pow(2.0, l2min);
		max = pow(2.0, l2max);
	}

	ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
	ImGui::PlotLines(label, counter.history.data(), counter.history.size(), 0, nullptr, min, max, { 0.f, height });
	ImGui::PopItemWidth();
}

void PerfInfo::DrawPerfWindow()
{
	// Draw headline counter
	std::string perf_text = fmt::format("Debug Tools | {:.1f} fps ({:.1f} ms)###Performance",
		framesThisSecond, m_fpsCounter.recent);

	if (ImGui::Begin(perf_text.c_str(), nullptr, ImGuiWindowFlags_NoNav)) {

		ImVec4 color = ImVec4(1, 1, 1, 1);
		if (m_fpsCounter.recent > 16.8)
			color = ImVec4(1.0, 0.6, 0.4, 1);
		if (m_fpsCounter.recent > 33.4)
			color = ImVec4(1.0, 0.4, 0.4, 1);

		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::Text("frame: %.1f ms | physics: %.1f ms | gui: %.1f ms",
			m_fpsCounter.recent, m_physCounter.recent, m_piguiCounter.recent);
		ImGui::PopStyleColor();

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		DrawCounter(m_fpsCounter, "#ft1", 8.0, 33.3, 30);

		ImGui::Spacing();

		if (ImGui::BeginTabBar("PerfInfoTabs")) {

			if (BeginDebugTab(s_rendererIcon, "Renderer Stats")) {
				DrawRendererStats();
				ImGui::Spacing();
				DrawImGuiStats();

				EndDebugTab();
			}

			if (BeginDebugTab(s_perfIcon, "Performance")) {
				DrawPerformanceStats();
				EndDebugTab();
			}

			if (false && ImGui::BeginTabItem("Input")) {
				DrawInputDebug();
				ImGui::EndTabItem();
			}

			if (Pi::game && Pi::player->GetFlightState() != Ship::HYPERSPACE) {
				if (BeginDebugTab(s_worldIcon, "WorldView Stats")) {
					DrawWorldViewStats();
					EndDebugTab();
				}
			}

			PiGui::RunHandler(Pi::GetFrameTime(), "debug-tabs");

			if (BeginDebugTab(s_planetIcon, "The Last Frontier")) {
				DrawFrontierTab();
				EndDebugTab();
			}

			ImGui::EndTabBar();
		}
	}

	ImGui::End();
}

void PerfInfo::DrawPerformanceStats()
{
	ImGui::SeparatorText("Frame Stats");

	ImGui::Text("Average FPS: %.1f", framesThisSecond);
	DrawCounter(m_fpsCounter, "##framet", 2.0, 33.0, 45, true);
	ImGui::Spacing();

	ImGui::Text("Average UPS: %.1f", physFramesThisSecond);
	DrawCounter(m_physCounter, "##physt", 0.0, 10.0, 45, true);
	ImGui::Spacing();

	DrawCounter(m_piguiCounter, "##guit", 0.0, 5.0, 45, true);
	ImGui::Spacing();

	if (ImGui::Button(m_state->updatePause ? "Unpause" : "Pause")) {
		SetUpdatePause(!m_state->updatePause);
	}

	ImGui::Spacing();

	ImGui::SeparatorText("Memory Usage");

	if (process_mem.peakMemSize > 0.0)
		ImGui::Text("Peak memory allocation: %.1f MB", (process_mem.peakMemSize * 1e-3));
	DrawCounter(m_procMemCounter, "##procmem", 0, 0, 45, true);
	ImGui::Spacing();

	DrawCounter(m_luaMemCounter, "##luamem", 0, 0, 25, true);
}

void PerfInfo::DrawRendererStats()
{
	const Graphics::Stats::TFrameData &stats = Pi::renderer->GetStats().FrameStatsPrevious();
	const Uint32 numDrawCalls = stats.m_stats[Graphics::Stats::STAT_DRAWCALL];
	const Uint32 numDrawCallInstances = stats.m_stats[Graphics::Stats::STAT_DRAWCALLINSTANCES];
	const Uint32 numDrawCallsInstanced = stats.m_stats[Graphics::Stats::STAT_DRAWCALLSINSTANCED];
	const Uint32 numTris = stats.m_stats[Graphics::Stats::STAT_NUM_TRIS];
	const Uint32 numLines = stats.m_stats[Graphics::Stats::STAT_NUM_LINES];
	const Uint32 numPoints = stats.m_stats[Graphics::Stats::STAT_NUM_POINTS];
	const Uint32 numCmdListFlushes = stats.m_stats[Graphics::Stats::STAT_NUM_CMDLIST_FLUSHES];
	const Uint32 numBuffersCreated = stats.m_stats[Graphics::Stats::STAT_CREATE_BUFFER];
	const Uint32 numBuffersInUse = stats.m_stats[Graphics::Stats::STAT_BUFFER_INUSE];
	const Uint32 numDynamicBuffersCreated = stats.m_stats[Graphics::Stats::STAT_DYNAMIC_DRAW_BUFFER_CREATED];
	const Uint32 numDynamicBuffersInUse = stats.m_stats[Graphics::Stats::STAT_DYNAMIC_DRAW_BUFFER_INUSE];
	const Uint32 numDrawBuffers = stats.m_stats[Graphics::Stats::STAT_DRAW_UNIFORM_BUFFER_INUSE];
	const Uint32 numDrawBufferAllocs = stats.m_stats[Graphics::Stats::STAT_DRAW_UNIFORM_BUFFER_ALLOCS];
	const Uint32 numRenderStates = stats.m_stats[Graphics::Stats::STAT_NUM_RENDER_STATES];
	const Uint32 numShaderPrograms = stats.m_stats[Graphics::Stats::STAT_NUM_SHADER_PROGRAMS];

	const Uint32 numDrawBuildings = stats.m_stats[Graphics::Stats::STAT_BUILDINGS];
	const Uint32 numDrawCities = stats.m_stats[Graphics::Stats::STAT_CITIES];
	const Uint32 numDrawGroundStations = stats.m_stats[Graphics::Stats::STAT_GROUNDSTATIONS];
	const Uint32 numDrawSpaceStations = stats.m_stats[Graphics::Stats::STAT_SPACESTATIONS];
	const Uint32 numDrawAtmospheres = stats.m_stats[Graphics::Stats::STAT_ATMOSPHERES];
	const Uint32 numDrawPlanets = stats.m_stats[Graphics::Stats::STAT_PLANETS];
	const Uint32 numDrawGasGiants = stats.m_stats[Graphics::Stats::STAT_GASGIANTS];
	const Uint32 numDrawStars = stats.m_stats[Graphics::Stats::STAT_STARS];
	const Uint32 numDrawShips = stats.m_stats[Graphics::Stats::STAT_SHIPS];
	const Uint32 numDrawBillBoards = stats.m_stats[Graphics::Stats::STAT_BILLBOARD];

	const Uint32 numTex2ds = stats.m_stats[Graphics::Stats::STAT_NUM_TEXTURE2D];
	const Uint32 tex2dMemUsage = stats.m_stats[Graphics::Stats::STAT_MEM_TEXTURE2D];
	const Uint32 numTexCubemaps = stats.m_stats[Graphics::Stats::STAT_NUM_TEXTURECUBE];
	const Uint32 texCubeMemUsage = stats.m_stats[Graphics::Stats::STAT_MEM_TEXTURECUBE];
	const Uint32 numTexArray2ds = stats.m_stats[Graphics::Stats::STAT_NUM_TEXTUREARRAY2D];
	const Uint32 texArray2dMemUsage = stats.m_stats[Graphics::Stats::STAT_MEM_TEXTUREARRAY2D];
	const Uint32 numCachedTextures = numTex2ds + numTexCubemaps + numTexArray2ds;
	const Uint32 cachedTextureMemUsage = tex2dMemUsage + texCubeMemUsage + texArray2dMemUsage;

	ImGui::SeparatorText("Renderer");
	ImGui::Text("%u Draw calls, %u Instances (%u Draw calls), %u CommandList flushes",
		numDrawCalls, numDrawCallsInstanced, numDrawCallInstances, numCmdListFlushes);

	ImGui::Indent();
	ImGui::Text("%u points", numPoints);
	ImGui::Text("%u lines", numLines);
	ImGui::Text("%u tris (%.2fM tris/sec)", numTris, numTris * framesThisSecond * 1e-6);
	ImGui::Unindent();
	ImGui::Spacing();

	ImGui::Text("%u Buildings, %u Cities, %u Gd.Stations, %u Sp.Stations",
		numDrawBuildings, numDrawCities, numDrawGroundStations, numDrawSpaceStations);
	ImGui::Text("%u Atmospheres, %u Planets, %u Gas Giants, %u Stars, %u Ships",
		numDrawAtmospheres, numDrawPlanets, numDrawGasGiants, numDrawStars, numDrawShips);
	ImGui::Text("%u Billboards, %u GeoPatches (%d tris)",
		numDrawBillBoards, Pi::statNumPatches, Pi::statSceneTris);
	ImGui::Text("%u Buffers Created (%u in use)", numBuffersCreated, numBuffersInUse);
	ImGui::Text("%u Dynamic Draw Buffers Created (%u in use)", numDynamicBuffersCreated, numDynamicBuffersInUse);
	ImGui::Text("%u Draw Uniform Buffers (%u allocations)", numDrawBuffers, numDrawBufferAllocs);
	ImGui::Spacing();

	ImGui::Text("%u cached shader programs", numShaderPrograms);
	ImGui::Text("%u cached render states", numRenderStates);
	ImGui::Text("%u cached textures, using %.3f MB VRAM", numCachedTextures, double(cachedTextureMemUsage) / scale_MB);

	if (ImGui::Button("Open Texture Cache Viewer"))
		m_state->textureCacheViewerOpen = true;

	if (ImGui::Button("Reload Shaders"))
		Pi::renderer->ReloadShaders();

	ImGui::Text("%u Texture2D in cache (%.3f MB)", numTex2ds, double(tex2dMemUsage) / scale_MB);
	ImGui::Text("%u Cubemaps in cache (%.3f MB)", numTexCubemaps, double(texCubeMemUsage) / scale_MB);
	ImGui::Text("%u TextureArray2D in cache (%.3f MB)", numTexArray2ds, double(texArray2dMemUsage) / scale_MB);
}

void PerfInfo::DrawWorldViewStats()
{
	ImGui::SeparatorText("World View");

	vector3d pos = Pi::player->GetPosition();
	vector3d abs_pos = Pi::player->GetPositionRelTo(Pi::game->GetSpace()->GetRootFrame());

	const FrameId playerFrame = Pi::player->GetFrame();

	ImGui::TextUnformatted(fmt::format("Player Position: {:.5}, {:.5}, {:.5}", pos.x, pos.y, pos.z).c_str());
	ImGui::TextUnformatted(fmt::format("Absolute Position: {:.5}, {:.5}, {:.5}", abs_pos.x, abs_pos.y, abs_pos.z).c_str());

	const Frame *frame = Frame::GetFrame(playerFrame);
	const SystemPath &path(frame->GetSystemBody()->GetPath());

	std::string tempStr;
	tempStr = fmt::format("Relative to frame: {} [{}, {}, {}, {}, {}]",
		frame->GetLabel(), path.sectorX, path.sectorY, path.sectorZ, path.systemIndex, path.bodyIndex);

	ImGui::TextUnformatted(tempStr.c_str());

	tempStr = fmt::format("Distance from frame: {:.2f} km, rotating: {}, has rotation: {}",
		pos.Length() / 1000.0, frame->IsRotFrame(), frame->HasRotFrame());

	ImGui::TextUnformatted(tempStr.c_str());

	ImGui::Spacing();

	//Calculate lat/lon for ship position
	const vector3d dir = pos.NormalizedSafe();
	const float lat = RAD2DEG(asin(dir.y));
	const float lon = RAD2DEG(atan2(dir.x, dir.z));

	ImGui::TextUnformatted(fmt::format("Lat / Lon: {:.8} / {:.8}", lat, lon).c_str());

	char aibuf[256];
	Pi::player->AIGetStatusText(aibuf);

	ImGui::TextUnformatted(aibuf);

	ImGui::Spacing();
	ImGui::TextUnformatted("Player Model ShowFlags:");

	using Flags = SceneGraph::Model::DebugFlags;

	bool showColl = m_state->playerModelDebugFlags & Flags::DEBUG_COLLMESH;
	bool showBBox = m_state->playerModelDebugFlags & Flags::DEBUG_BBOX;
	bool showTags = m_state->playerModelDebugFlags & Flags::DEBUG_TAGS;

	bool changed = ImGui::Checkbox("Show Collision Mesh", &showColl);
	changed |= ImGui::Checkbox("Show Bounding Box", &showBBox);
	changed |= ImGui::Checkbox("Show Tag Locations", &showTags);

	/* clang-format off */
	if (changed) {
		m_state->playerModelDebugFlags = (showColl ? Flags::DEBUG_COLLMESH : 0)
			| (showBBox ? Flags::DEBUG_BBOX : 0)
			| (showTags ? Flags::DEBUG_TAGS : 0);
		Pi::player->GetModel()->SetDebugFlags(m_state->playerModelDebugFlags);
	}
	/* clang-format on */

	if (Pi::player->GetNavTarget() && Pi::player->GetNavTarget()->GetSystemBody()) {
		const auto *sbody = Pi::player->GetNavTarget()->GetSystemBody();
		ImGui::TextUnformatted(fmt::format("Name: {}, Population: {}", sbody->GetName(), sbody->GetPopulation() * 1e9).c_str());
	}

	if (Pi::GetView() == Pi::game->GetSectorView()) {
		if (ImGui::Button("Dump Selected System")) {
			SystemPath path = Pi::game->GetSectorView()->GetSelected();
			RefCountedPtr<StarSystem> system = Pi::game->GetGalaxy()->GetStarSystem(path);

			if (system)
				system->Dump(Log::GetLog()->GetLogFileHandle());
		}

		if (ImGui::Button("Dump System-Editor JSON")) {
			SystemPath path = Pi::game->GetSectorView()->GetSelected();
			RefCountedPtr<StarSystem> system = Pi::game->GetGalaxy()->GetStarSystem(path);

			if (system) {
				const std::string fname = FileSystem::JoinPathBelow(FileSystem::userFiles.GetRoot(), fmt::format("{}.json", system->GetName()));
				FILE *f = fopen(fname.c_str(), "w");

				if (f) {
					Json systemdef = Json::object();

					system->DumpToJson(systemdef);

					std::string jsonData = systemdef.dump(1, '\t');

					fwrite(jsonData.data(), 1, jsonData.size(), f);
					fclose(f);
				}
			}
		}
	}

	DrawTerrainDebug();

	if (ImGui::Button("ObjectViewer"))
	{
		if (Pi::game)
		{
			Pi::ToggleObjectViewer();
		}
	}
}

void PerfInfo::DrawInputDebug()
{
	std::ostringstream output;
	auto frameList = Pi::input->GetInputFrames();
	uint32_t index = 0;
	for (const auto *frame : frameList) {
		ImGui::Text("InputFrame %d: modal=%d", index, frame->modal);
		ImGui::Indent();
		uint32_t actionNum = 0;
		for (const auto *action : frame->actions) {
			ImGui::Text("Action %d: active=%d (%d, %d)", actionNum, action->m_active, action->binding.m_active, action->binding2.m_active);
			if (action->binding.Enabled()) {
				output << action->binding;
				ImGui::TextUnformatted(output.str().c_str());
				output.str("");
			}
			if (action->binding2.Enabled()) {
				output << action->binding2;
				ImGui::TextUnformatted(output.str().c_str());
				output.str("");
			}

			ImGui::Separator();
			actionNum++;
		}

		ImGui::Spacing();

		actionNum = 0;
		for (const auto *axis : frame->axes) {
			ImGui::Text("Axis %d: value=%.2f (%d, %d)", actionNum, axis->m_value, axis->positive.m_active, axis->negative.m_active);
			if (axis->positive.Enabled()) {
				output << axis->positive;
				ImGui::TextUnformatted(output.str().c_str());
				output.str("");
			}

			if (axis->negative.Enabled()) {
				output << axis->negative;
				ImGui::TextUnformatted(output.str().c_str());
				output.str("");
			}

			ImGui::Separator();
			actionNum++;
		}
		ImGui::Unindent();

		ImGui::Spacing();
		index++;
	}
}

void PiGui::PerfInfo::DrawTerrainDebug()
{
	ImGui::Spacing();
	ImGui::SeparatorText("Terrain Debug");

	using Flags = GeoSphere::DebugFlags;
	uint32_t debugFlags = GeoSphere::GetDebugFlags();
	bool showSort = debugFlags & Flags::DEBUG_SORTGEOPATCHES;
	bool showWire = debugFlags & Flags::DEBUG_WIREFRAME;
	//bool showFace = debugFlags & Flags::DEBUG_FACELABELS;

	bool changed = ImGui::Checkbox("Distance Sort GeoPatches", &showSort);
	changed |= ImGui::Checkbox("Show Wireframe", &showWire);
	//changed |= ImGui::Checkbox("Show Face IDs", &showFace);

	/* clang-format off */
	if (changed) {
		debugFlags = (debugFlags & ~uint32_t(Flags::DEBUG_SORTGEOPATCHES | Flags::DEBUG_WIREFRAME))
			| (showSort ? Flags::DEBUG_SORTGEOPATCHES : 0)
			| (showWire ? Flags::DEBUG_WIREFRAME : 0);
			//| (showFace ? Flags::DEBUG_FACELABELS : 0);
		GeoSphere::SetDebugFlags(debugFlags);
		Pi::config->SetInt("SortGeoPatches", showSort ? 1 : 0);

		Pi::config->Save();
	}
	/* clang-format on */
}

// The Last Frontier's own debug features: terrain and GPU here, and the tabs
// registered from Lua with mod = true (pigui/views/debug.lua)
void PiGui::PerfInfo::DrawFrontierTab()
{
	// the running record (FrontierLog), with a place for a comment about what is on screen
	bool writeLog = FrontierLog::Enabled();
	if (ImGui::Checkbox("Write frontier_log.txt", &writeLog))
		FrontierLog::SetEnabled(writeLog);
	ImGui::SameLine();
	static char s_comment[256] = "";
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
	ImGui::InputText("##frontierComment", s_comment, sizeof(s_comment));
	ImGui::SameLine();
	if (ImGui::Button("Mark in log")) {
		FrontierLog::Mark(s_comment);
		s_comment[0] = 0;
	}

	if (ImGui::BeginTabBar("FrontierTabs")) {
		if (BeginDebugTab(s_planetIcon, "Terrain and GPU")) {
			DrawFrontierTerrain();
			DrawGeology();
			EndDebugTab();
		}
		if (BeginDebugTab(s_rendererIcon, "OpenGL 4.6 Plan")) {
			DrawOpenGLPlan();
			EndDebugTab();
		}

		PiGui::RunHandler(Pi::GetFrameTime(), "frontier-debug-tabs");

		ImGui::EndTabBar();
	}
}

// The OpenGL 4.6 plan (docs/implementation_plans/opengl46-tessellation-compute_plan.md): what the driver has for it,
// the measurements of P0 live, and the mesh where the ship is. Later phases put their switches and readouts here.
void PiGui::PerfInfo::DrawOpenGLPlan()
{
	const ImVec4 yes(0.4f, 0.9f, 0.4f, 1.0f), no(1.0f, 0.45f, 0.35f, 1.0f), dim(0.7f, 0.7f, 0.7f, 1.0f);

	ImGui::SeparatorText("Context");
	const Graphics::Renderer::ContextInfo &info = Pi::renderer->GetContextInfo();
	ImGui::Text("The game asks for: %s", info.requested.c_str());
	ImGui::Text("The driver gave: OpenGL %d.%d, GLSL %s", info.major, info.minor, info.shading.c_str());
	ImGui::TextColored(dim, "%s | %s | %s", info.version.c_str(), info.vendor.c_str(), info.device.c_str());
	if (!info.features.empty() && ImGui::BeginTable("glFeatures", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
		ImGui::TableSetupColumn("Driver");
		ImGui::TableSetupColumn("Feature");
		ImGui::TableSetupColumn("Phase");
		ImGui::TableHeadersRow();
		for (const Graphics::Renderer::ContextInfo::Feature &f : info.features) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextColored(f.present ? yes : no, f.present ? "yes" : "no");
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(f.name.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(f.phase.c_str());
		}
		ImGui::EndTable();
	}

	// P0.1: counted while this shows, with or without the log
	ImGui::Spacing();
	ImGui::SeparatorText("P0: what goes to the graphics driver (per frame, over the last second)");
	FrontierLog::WantUploadCounts();
	const FrontierLog::UploadSecond &up = FrontierLog::GetUploadSecond();
	if (up.frames == 0) {
		ImGui::TextColored(dim, "counting...");
	} else {
		const double frames = double(up.frames);
		double totalMs = 0.0;
		if (ImGui::BeginTable("glUploads", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn("Kind");
			ImGui::TableSetupColumn("Calls");
			ImGui::TableSetupColumn("MB");
			ImGui::TableSetupColumn("ms");
			ImGui::TableHeadersRow();
			for (int i = 0; i < Graphics::UPLOAD_KINDS; i++) {
				const Graphics::UploadCount &k = up.kinds[i];
				totalMs += double(k.nanos) / 1e6 / frames;
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Graphics::UploadKindName(Graphics::UploadKind(i)));
				ImGui::TableNextColumn();
				ImGui::Text("%.1f", double(k.calls) / frames);
				ImGui::TableNextColumn();
				ImGui::Text("%.2f", double(k.bytes) / 1048576.0 / frames);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", double(k.nanos) / 1e6 / frames);
			}
			ImGui::EndTable();
		}
		ImGui::Text("In all %.2f ms a frame, the longest frame %.2f ms, over %d frames", totalMs, up.longestMs, up.frames);
		ImGui::Text("Patch meshes: %.1f a frame in %.2f ms (packing the vertices and their own buffer calls)", double(up.patchMeshes) / frames, up.patchMeshMs / frames);
		ImGui::TextColored(dim, "CPU time in the calls. A driver that defers its work moves it to swap instead.");
	}

	// P0.2 and P0.3, since the reset here
	ImGui::Spacing();
	ImGui::SeparatorText("P0: patch jobs and surface tile bakes (thread time)");
	static GeoPatchGenStats s_patchBase;
	static TerrainSurfaceTiles::BakeStats s_bakeBase;
	const GeoPatchGenStats patches = GetGeoPatchGenStats();
	const TerrainSurfaceTiles::BakeStats bakes = TerrainSurfaceTiles::GetBakeStats();
	// (the Terrain and GPU tab's reset takes the patch totals below the base)
	if (patches.patches < s_patchBase.patches)
		s_patchBase = GeoPatchGenStats();
	const uint64_t patchCount = patches.patches - s_patchBase.patches;
	if (patchCount > 0) {
		const double perPatch = 1e6 * double(patchCount);
		const double total = double(patches.microseconds - s_patchBase.microseconds) / 1000.0 / double(patchCount);
		const double heights = double(patches.heightsNanos - s_patchBase.heightsNanos) / perPatch;
		const double slopes = double(patches.slopesNanos - s_patchBase.slopesNanos) / perPatch;
		const double normals = double(patches.normalsNanos - s_patchBase.normalsNanos) / perPatch;
		const double surface = double(patches.surfaceNanos - s_patchBase.surfaceNanos) / perPatch;
		ImGui::Text("%llu patches at %.2f ms each: heights %.2f, colour slopes %.2f, normals %.2f, colours and materials %.2f, other %.2f",
			(unsigned long long)patchCount, total, heights, slopes, normals, surface, std::max(total - heights - slopes - normals - surface, 0.0));
	} else {
		ImGui::TextColored(dim, "no patches made since the reset");
	}
	const uint64_t bakeCount = bakes.bakes - s_bakeBase.bakes;
	if (bakeCount > 0) {
		const double perBake = 1e6 * double(bakeCount);
		ImGui::Text("%llu surface tile bakes at %.2f ms each: heights %.2f, colour slopes %.2f, texels %.2f", (unsigned long long)bakeCount,
			double(bakes.nanos - s_bakeBase.nanos) / perBake, double(bakes.heightsNanos - s_bakeBase.heightsNanos) / perBake,
			double(bakes.slopesNanos - s_bakeBase.slopesNanos) / perBake, double(bakes.texelsNanos - s_bakeBase.texelsNanos) / perBake);
	} else {
		ImGui::TextColored(dim, "no surface tiles baked since the reset");
	}
	if (ImGui::Button("Reset##openglPlan")) {
		s_patchBase = patches;
		s_bakeBase = bakes;
	}

	// what section 1 of the plan says about the mesh, against what is drawn
	ImGui::Spacing();
	ImGui::SeparatorText("The mesh where the ship is");
	const Frame *frame = Pi::player && Pi::game ? Frame::GetFrame(Pi::player->GetFrame()) : nullptr;
	const Body *body = frame ? frame->GetBody() : nullptr;
	const GeoSphere *sphere = body && body->IsType(ObjectType::PLANET) ? dynamic_cast<const GeoSphere *>(static_cast<const Planet *>(body)->GetBaseSphere()) : nullptr;
	if (!sphere) {
		ImGui::TextColored(dim, "not at a body with ground");
	} else {
		// the rule of GeoSphere::CalculateMaxPatchDepth (P1)
		ImGui::Text("%s: deepest level %d: twice level %d (where triangles would be about 100 m), at most %d",
			body->GetLabel().c_str(), sphere->GetMaxDepth(), sphere->GetMeantDepth(), GEOPATCH_MAX_DEPTH);
		ImGui::Text("Vertices there up to %.1f m apart (the middle of a cube face; about 0.8 times that near its corners); %d more levels near the player now",
			sphere->GetDeepestVertexSpacing(), sphere->GetExtraDepth());
		const GeoSphere::DrawnPatches &drawn = sphere->GetDrawnPatches();
		ImGui::Text("Drawn: %d patches; the deepest at level %d (%.1f m between vertices), the nearest at level %d (%.1f m)",
			drawn.count, drawn.deepest, drawn.finestSpacing, drawn.nearestDepth, drawn.nearestSpacing);
	}

	// P3: the landforms as real shape, being tried. The patches are made again after a change.
	ImGui::Spacing();
	ImGui::SeparatorText("P3: the large landforms as real shape (on/off in the settings' mod tab)");
	{
		static bool s_shape = Terrain::GetLandformShape() > 0.0;
		static float s_top = Terrain::GetLandformShape() > 0.0 ? float(Terrain::GetLandformShape()) : float(Terrain::LARGE_LANDFORMS_TO_M);
		static float s_from = float(Terrain::GetLandformShapeFrom());
		// (switched in the settings window's mod tab too: follow it)
		static double s_seenTop = Terrain::GetLandformShape(), s_seenFrom = Terrain::GetLandformShapeFrom();
		if (Terrain::GetLandformShape() != s_seenTop || Terrain::GetLandformShapeFrom() != s_seenFrom) {
			s_seenTop = Terrain::GetLandformShape();
			s_seenFrom = Terrain::GetLandformShapeFrom();
			s_shape = s_seenTop > 0.0;
			if (s_shape)
				s_top = float(s_seenTop);
			s_from = float(s_seenFrom);
		}
		ImGui::Checkbox("Landforms in the ground's shape##openglPlan", &s_shape);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::SliderFloat("smallest as shape##openglPlan", &s_from, 10.0f, 40000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::SliderFloat("largest as shape##openglPlan", &s_top, 10.0f, 40000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
		const double wanted = s_shape ? double(s_top) : 0.0;
		const bool changed = std::abs(wanted - Terrain::GetLandformShape()) > 0.5 || (wanted > 0.0 && std::abs(s_from - Terrain::GetLandformShapeFrom()) > 0.5);
		if (ImGui::Button(changed ? "Apply (makes the ground again)##openglPlan" : "Applied##openglPlan") && changed) {
			Terrain::SetLandformShape(wanted);
			Terrain::SetLandformShapeFrom(s_from);
			Pi::config->SetFloat("TerrainLandformShape", float(wanted));
			Pi::config->SetFloat("TerrainLandformShapeFrom", s_from);
			Pi::config->Save();
			FrontierLog::Note(wanted > 0.0 ? "landforms as real shape, from " + std::to_string(int(s_from)) + " m up to " + std::to_string(int(wanted)) + " m" : "landforms as real shape: off");
			Pi::OnChangeDetailLevel();
		}
		ImGui::TextColored(dim, "On: the landforms from the smallest to the largest size become real shape; the smaller ones stay painted");
		ImGui::TextColored(dim, "as before, and none are drawn twice. No cracked slabs as shape. Ground that moves can bury or lift a landed ship: take off first.");
		if (sphere) {
			const TerrainLandforms::Params &lp = sphere->GetTerrain()->GetLandformParams();
			ImGui::Text("Here: %d octaves, %.1f m to %.0f m; styles rolling %.2f, ridged %.2f, dunes %.2f, craters %.2f",
				lp.count, lp.finestWave, lp.finestWave * std::pow(2.0, double(lp.count - 1)), lp.style[0], lp.style[1], lp.style[2], lp.style[3]);
			// the sizes this world has that are shape now (the same rule as Terrain::AddLandforms)
			if (Terrain::GetLandformShape() > 0.0) {
				std::string sizes;
				for (int i = 0; i < lp.count; i++) {
					const double wave = lp.finestWave * std::pow(2.0, double(i));
					if (wave >= Terrain::GetLandformShapeFrom() * 0.99 && wave <= Terrain::GetLandformShape() * 1.01)
						sizes += (sizes.empty() ? "" : ", ") + std::to_string(int(wave + 0.5)) + " m";
				}
				ImGui::TextWrapped("As shape here: %s", sizes.empty() ? "none (this world's landforms are all smaller, or all larger)" : sizes.c_str());
			}
		}

		// their shade in the ground's colour (Terrain::LandformShade): switched at once, to compare before and after
		ImGui::Spacing();
		static bool s_shadeOn = Terrain::GetLandformShadeStrength() > 0.0;
		static float s_shade = Terrain::GetLandformShadeStrength() > 0.0 ? float(Terrain::GetLandformShadeStrength()) : float(Terrain::LANDFORM_SHADE);
		const auto applyShade = [&]() {
			const double strength = s_shadeOn ? double(s_shade) : 0.0;
			Terrain::SetLandformShadeStrength(strength);
			Pi::config->SetFloat("TerrainLandformShade", float(strength));
			Pi::config->Save();
			char note[48];
			std::snprintf(note, sizeof(note), "landform shade %.2f", strength);
			FrontierLog::Note(strength > 0.0 ? std::string(note) : std::string("landform shade: off"));
			Pi::OnChangeDetailLevel();
		};
		if (ImGui::Checkbox("Hills shaded in the ground's colour, as craters are##openglPlan", &s_shadeOn))
			applyShade();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::SliderFloat("shade strength##openglPlan", &s_shade, 0.05f, 1.0f, "%.2f");
		// (on letting go of the slider, so the ground isn't made again at every step)
		if (ImGui::IsItemDeactivatedAfterEdit() && s_shadeOn)
			applyShade();
		ImGui::TextColored(dim, "Hollows between the hills darker, crests paler, by up to this share on bare airless ground; less with air,");
		ImGui::TextColored(dim, "snow or ice. Only the hills made into shape (above), so it needs those on. Makes the ground again.");
		// what it does here: the shade over the ground within 3 km of the ship, at full detail, every half second
		if (sphere && frame && frame->IsRotFrame() && Pi::player) {
			static int s_shadeFrames = 0;
			static double s_shadeTypical = 0.0, s_shadeLow = 0.0, s_shadeHigh = 0.0;
			if (s_shadeFrames-- <= 0) {
				s_shadeFrames = 60;
				const Terrain *terrain = sphere->GetTerrain();
				const vector3d up = Pi::player->GetPosition().Normalized();
				const vector3d east = (std::abs(up.y) < 0.9 ? vector3d(0.0, 1.0, 0.0) : vector3d(1.0, 0.0, 0.0)).Cross(up).Normalized();
				const vector3d north = up.Cross(east);
				const double reach = 3000.0 / sphere->GetSystemBody()->GetRadius();
				TerrainNoise::ScopedBandLimit fullDetail(0.0);
				double sum = 0.0;
				int n = 0;
				s_shadeLow = 1.0;
				s_shadeHigh = -1.0;
				for (int a = -10; a <= 10; a++) {
					for (int b = -10; b <= 10; b++) {
						const double s = terrain->LandformShade((up + east * (reach * a / 10.0) + north * (reach * b / 10.0)).Normalized());
						sum += s * s;
						s_shadeLow = std::min(s_shadeLow, s);
						s_shadeHigh = std::max(s_shadeHigh, s);
						n++;
					}
				}
				s_shadeTypical = std::sqrt(sum / n);
			}
			const double strength = Terrain::GetLandformShadeStrength() * sphere->GetTerrain()->LandformShadeAir();
			ImGui::Text("Here, within 3 km: shade typically %.2f (from %.2f to %.2f); the colour %.0f %% darker or paler typically, at most %.0f %%",
				s_shadeTypical, s_shadeLow, s_shadeHigh, 100.0 * strength * s_shadeTypical, 100.0 * strength * std::max(-s_shadeLow, s_shadeHigh));
			ImGui::TextColored(dim, "(on bare ground; snow and ice halve it)");
		}

		// how the hills fade out from afar (Terrain::SetLandformHeightFade, SetLandformShadeFade)
		ImGui::Spacing();
		ImGui::Text("Fading out from afar, in samples across a hill (the ground's points, or the colour's from orbit):");
		static float s_heightFade[2] = { float(Terrain::GetLandformHeightFade().from), float(Terrain::GetLandformHeightFade().full) };
		static float s_shadeFade[2] = { float(Terrain::GetLandformShadeFade().from), float(Terrain::GetLandformShadeFade().full) };
		const auto applyFades = [&]() {
			s_heightFade[1] = std::max(s_heightFade[1], s_heightFade[0]);
			s_shadeFade[1] = std::max(s_shadeFade[1], s_shadeFade[0]);
			Terrain::SetLandformHeightFade({ s_heightFade[0], s_heightFade[1] });
			Terrain::SetLandformShadeFade({ s_shadeFade[0], s_shadeFade[1] });
			Pi::config->SetFloat("TerrainLandformFadeFrom", s_heightFade[0]);
			Pi::config->SetFloat("TerrainLandformFadeFull", s_heightFade[1]);
			Pi::config->SetFloat("TerrainLandformShadeFadeFrom", s_shadeFade[0]);
			Pi::config->SetFloat("TerrainLandformShadeFadeFull", s_shadeFade[1]);
			Pi::config->Save();
			char note[128];
			std::snprintf(note, sizeof(note), "landform fades: height none at %.1f, all from %.1f samples; shade none at %.1f, all from %.1f",
				s_heightFade[0], s_heightFade[1], s_shadeFade[0], s_shadeFade[1]);
			FrontierLog::Note(note);
			Pi::OnChangeDetailLevel();
		};
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::SliderFloat2("hills' height: none at / all from##openglPlan", s_heightFade, 1.0f, 64.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
		if (ImGui::IsItemDeactivatedAfterEdit())
			applyFades();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::SliderFloat2("their shade: none at / all from##openglPlan", s_shadeFade, 1.0f, 64.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
		if (ImGui::IsItemDeactivatedAfterEdit())
			applyFades();
		if (ImGui::Button("Defaults (2 / 8 and 2 / 64)##openglPlan")) {
			s_heightFade[0] = float(Terrain::LANDFORM_HEIGHT_FADE.from);
			s_heightFade[1] = float(Terrain::LANDFORM_HEIGHT_FADE.full);
			s_shadeFade[0] = float(Terrain::LANDFORM_SHADE_FADE.from);
			s_shadeFade[1] = float(Terrain::LANDFORM_SHADE_FADE.full);
			applyFades();
		}
		ImGui::TextColored(dim, "Higher numbers: the hills fade out sooner as you climb, so less crumpling and speckle from orbit, and");
		ImGui::TextColored(dim, "less of them from mid heights. The shade never outlasts the height. Makes the ground again on letting go.");

		// the blend between detail levels (GeoSphere::MorphEnabled and the rest, terrain-morph.glsl): live, no new ground
		ImGui::Spacing();
		ImGui::SeparatorText("Blend between the ground's detail levels");
		{
			bool morph = GeoSphere::MorphEnabled();
			if (ImGui::Checkbox("Blend each level into the coarser one at their borders##morph", &morph)) {
				Pi::config->SetInt("TerrainMorph", morph ? 1 : 0);
				Pi::config->Save();
				FrontierLog::Note(std::string("blend between detail levels: ") + (morph ? "on" : "off"));
			}
			bool tint = GeoSphere::MorphTint();
			if (ImGui::Checkbox("Show the blend in red (debug)##morph", &tint))
				Pi::config->SetInt("TerrainMorphTint", tint ? 1 : 0);
			static float s_morphWidth = float(GeoSphere::MorphWidth());
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
			if (ImGui::SliderFloat("strip width (share of a square)##morph", &s_morphWidth, 0.02f, 0.5f, "%.2f"))
				Pi::config->SetFloat("TerrainMorphWidth", s_morphWidth);
			if (ImGui::IsItemDeactivatedAfterEdit())
				Pi::config->Save();
			ImGui::TextColored(dim, "Where a square of ground meets a coarser one, a strip along that edge takes on the coarser look, all of");
			ImGui::TextColored(dim, "it at the edge (no line); the rest keeps its full detail at any distance. New squares fade in over 0.6 s");
			ImGui::TextColored(dim, "from the coarser look instead of popping. Red shows the strips and the fading.");

			// the distant shadow maps' tolerance for ground that changed since they were drawn (SceneShadows.cpp)
			// (in thousandths of the distance)
			static float s_tolerance = 1000.0f * Pi::config->Float("ShadowGroundShare", 0.004f);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
			if (ImGui::SliderFloat("distant shadows: ground tolerance (thousandths of its distance)##morph", &s_tolerance, 0.0f, 20.0f, "%.1f"))
				Pi::config->SetFloat("ShadowGroundShare", 0.001f * s_tolerance);
			if (ImGui::IsItemDeactivatedAfterEdit())
				Pi::config->Save();
			ImGui::TextColored(dim, "The middle and far sun shadow maps are kept and redrawn now and then; ground that came out lower since");
			ImGui::TextColored(dim, "went black in them. 4: ground 10 km away may lie 40 m lower, 1 km away 4 m. Higher: less black, but small");
			ImGui::TextColored(dim, "shadows in the distance are lost. 0: as before.");

			// finding where black shadows come from (SceneShadows.cpp): live
			bool shadowTint = Pi::config->Int("ShadowDebug", 0) != 0;
			if (ImGui::Checkbox("Tint by sun shadow map: red near, green middle, blue far##morph", &shadowTint))
				Pi::config->SetInt("ShadowDebug", shadowTint ? 1 : 0);
			bool distantMaps = Pi::config->Int("ShadowDistantMaps", 1) != 0;
			if (ImGui::Checkbox("Middle and far sun shadow maps (off: near one only)##morph", &distantMaps))
				Pi::config->SetInt("ShadowDistantMaps", distantMaps ? 1 : 0);
		}
	}

	// P4a: the ground near the camera tessellated
	ImGui::Spacing();
	ImGui::SeparatorText("P4a: the ground near the camera tessellated (no new shape yet)");
	{
		bool tessellation = GeoSphere::TessellationEnabled();
		if (ImGui::Checkbox("Tessellate the ground near the camera##openglPlan", &tessellation)) {
			Pi::config->SetInt("TerrainTessellation", tessellation ? 1 : 0);
			Pi::config->Save();
			FrontierLog::Note(std::string("terrain tessellation: ") + (tessellation ? "on" : "off"));
		}
		float pixels = float(GeoSphere::TessellationPixels());
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		if (ImGui::SliderFloat("triangle edge on screen##openglPlan", &pixels, 2.0f, 64.0f, "%.0f px"))
			Pi::config->SetFloat("TerrainTessellationPixels", pixels);
		if (ImGui::IsItemDeactivatedAfterEdit())
			Pi::config->Save();
		if (sphere)
			ImGui::Text("Patches drawn tessellated: %d of %d", sphere->GetDrawnPatches().tessellated, sphere->GetDrawnPatches().count);
		// where two patches meet at different heights, gaps in the ground (to the log)
		if (sphere && ImGui::Button("Check the patches' seams (to the log)##openglPlan"))
			sphere->CheckSeams();
		ImGui::TextColored(dim, "Should look the same. The wireframe view (Terrain and GPU) shows the finer triangles; GPU time: \"terrain\".");
	}

	// P4b: the ground textures at one size near the camera
	ImGui::Spacing();
	ImGui::SeparatorText("P4b: the ground textures at one size near the camera");
	{
		bool fixed = GeoSphere::TextureFixedEnabled();
		if (ImGui::Checkbox("One texture size near the camera##openglPlan", &fixed)) {
			Pi::config->SetInt("TerrainTextureFixed", fixed ? 1 : 0);
			Pi::config->Save();
			FrontierLog::Note(std::string("ground textures at one size near the camera: ") + (fixed ? "on" : "off"));
		}
		float level = float(GeoSphere::TextureFixedLevel());
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		if (ImGui::SliderFloat("size##openglPlanTexLevel", &level, 0.0f, 8.0f, "level %.1f"))
			Pi::config->SetFloat("TerrainTextureFixedLevel", level);
		if (ImGui::IsItemDeactivatedAfterEdit())
			Pi::config->Save();
		ImGui::SameLine();
		ImGui::Text("(each texture covers %.1f m)", std::pow(2.0, double(level)));
		float range = float(GeoSphere::TextureFixedRange());
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		if (ImGui::SliderFloat("out to##openglPlanTexRange", &range, 2.0f, 500.0f, "%.0f m", ImGuiSliderFlags_Logarithmic))
			Pi::config->SetFloat("TerrainTextureFixedRange", range);
		if (ImGui::IsItemDeactivatedAfterEdit())
			Pi::config->Save();
		ImGui::TextColored(dim, "Within that distance the textures keep one size as you move; beyond it they grow with the distance as before.");
	}

	// P4c: the stones raised near the camera
	ImGui::Spacing();
	ImGui::SeparatorText("P4c: the stones raised near the camera");
	{
		// (switched in the settings window's The Last Frontier tab since 2026-11-09)
		ImGui::Text("Stones raised: %s", GeoSphere::StonesEnabled() ? "on" : "off");
		ImGui::SameLine();
		ImGui::TextColored(dim, "(Settings, The Last Frontier tab: \"Ground tessellation and bump mapping\")");
		float height = float(GeoSphere::StonesHeight());
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		if (ImGui::SliderFloat("height##openglPlanStones", &height, 0.0f, 4.0f, "x %.2f"))
			Pi::config->SetFloat("TerrainStonesHeight", height);
		if (ImGui::IsItemDeactivatedAfterEdit())
			Pi::config->Save();
		// (as materialStoneDepth() in terrain-materials.glsl)
		const double tile = std::pow(2.0, GeoSphere::TextureFixedLevel()) * GeoSphere::StonesHeight() * 100.0;
		ImGui::Text("Highest: rock %.0f cm, gravel %.0f cm, regolith %.0f cm, sand %.1f cm", 0.10 * tile, 0.05 * tile, 0.03 * tile, 0.008 * tile);
		float fullPixels = float(GeoSphere::StonesFullPixels());
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		if (ImGui::SliderFloat("full detail down to##openglPlanStonesFull", &fullPixels, 1.0f, 32.0f, "%.0f px", ImGuiSliderFlags_Logarithmic))
			Pi::config->SetFloat("TerrainStonesFullPixels", fullPixels);
		if (ImGui::IsItemDeactivatedAfterEdit())
			Pi::config->Save();
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Where the tallest stone would stand shorter than this on screen, the ground is cut more coarsely,\nin step with the stones. Higher: thins out sooner, faster, distant stones blockier. Lower: the reverse.");
		float fadePixels = float(GeoSphere::StonesFadePixels());
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		if (ImGui::SliderFloat("fade out below##openglPlanStonesFade", &fadePixels, 0.5f, 16.0f, "%.1f px", ImGuiSliderFlags_Logarithmic))
			Pi::config->SetFloat("TerrainStonesFadePixels", fadePixels);
		if (ImGui::IsItemDeactivatedAfterEdit())
			Pi::config->Save();
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Where the tallest stone would stand shorter than this on screen, the stones fade out, gone at half of it;\nthat ground isn't tessellated at all. Higher: faster, the stones end nearer. Lower: the reverse.");
		bool shadows = GeoSphere::StoneShadowsEnabled();
		if (ImGui::Checkbox("The stones shade the ground from the sun (P4e)##openglPlan", &shadows)) {
			Pi::config->SetInt("TerrainStoneShadows", shadows ? 1 : 0);
			Pi::config->Save();
			FrontierLog::Note(std::string("stone shadows: ") + (shadows ? "on" : "off"));
		}
		if (!GeoSphere::TextureFixedEnabled())
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Needs \"One texture size near the camera\" (P4b) on: the stones are that size's texture.");
		ImGui::TextColored(dim, "Raised within P4b's \"out to\", gone by twice that. With the stones on, only that ground is tessellated,");
		ImGui::TextColored(dim, "whatever the P4a box says (its pixel slider still sets how fine). A whole-number size reads each texture");
		ImGui::TextColored(dim, "once, a size in between twice.");
	}

	// P4d: feet and wheels on the stones
	ImGui::Spacing();
	ImGui::SeparatorText("P4d: feet and wheels on the stones");
	{
		bool contact = GeoSphere::StonesContactEnabled();
		if (ImGui::Checkbox("Feet and wheels stand on the stones##openglPlan", &contact)) {
			Pi::config->SetInt("TerrainStonesContact", contact ? 1 : 0);
			Pi::config->Save();
			FrontierLog::Note(std::string("feet and wheels on the stones: ") + (contact ? "on" : "off"));
		}
		static bool s_probe = false;
		if (ImGui::Checkbox("Check: the height under the camera, worked out on both sides##openglPlan", &s_probe))
			GeoSphere::SetStoneProbe(s_probe);
		if (s_probe) {
			const GeoSphere::StoneProbe &probe = GeoSphere::GetStoneProbe();
			if (!GeoSphere::StonesActive())
				ImGui::TextColored(dim, "The stones are off.");
			else if (!probe.valid)
				ImGui::TextColored(dim, "Waiting for the graphics card's answer (or no drawn ground under the camera).");
			else {
				const double diff = std::abs(probe.cpu - probe.gpu) * 1000.0;
				ImGui::Text("Main processor %.2f cm, graphics card %.2f cm: they differ by %.3f mm", probe.cpu * 100.0, probe.gpu * 100.0, diff);
				ImGui::SameLine();
				ImGui::TextColored(diff < 1.0 ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.3f, 1.0f), diff < 1.0 ? "(agree)" : "(DISAGREE)");
				ImGui::Text("%.1f m from the camera; texture layers %d and %d", probe.distance, probe.layer1, probe.layer2);
				if (!probe.meshData)
					ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "The mesh there had no material weights: the main processor raises nothing (a bug).");
			}
			if (ImGui::Button("Note it in the log##openglPlanProbe") && probe.valid)
				FrontierLog::Note("stones under the camera: main processor " + std::to_string(probe.cpu * 100.0) + " cm, graphics card " +
					std::to_string(probe.gpu * 100.0) + " cm, " + std::to_string(probe.distance) + " m away, layers " +
					std::to_string(probe.layer1) + " and " + std::to_string(probe.layer2));
		}
		ImGui::TextColored(dim, "The walker's feet and the rover's wheels stand on the stones as drawn, so they fade with the camera's");
		ImGui::TextColored(dim, "distance as the stones do. Landed ships stay on the flat ground. The first switch-on loads the heights.");
	}

	// what the ground's drawing costs, part by part
	ImGui::Spacing();
	ImGui::SeparatorText("The ground's cost, part by part (leave parts out, read \"terrain\" in the log)");
	{
		static const char *const parts[20] = { "photo textures", "relief below the mesh", "surface tiles", "city streets and lights",
			"water", "sun shadows", "ships' lamps and tracks", "materials' shine", "air light per corner",
			"texture turns", "coarse texture size", "second material", "grazing-angle filtering",
			"slope and shore weights", "colour shift", "fine texture read",
			"city roads", "city lots", "city gardens", "city lights" };
		int mask = GeoSphere::GetGroundSkip();
		for (int i = 0; i < 20; i++) {
			bool skip = (mask & (1 << i)) != 0;
			if (i % 4)
				ImGui::SameLine();
			if (ImGui::Checkbox((std::string("leave out ") + parts[i] + "##groundSkip").c_str(), &skip)) {
				mask = skip ? (mask | (1 << i)) : (mask & ~(1 << i));
				GeoSphere::SetGroundSkip(mask);
				FrontierLog::Note(std::string("ground cost: ") + parts[i] + (skip ? " left out" : " back in"));
			}
		}
		ImGui::TextColored(dim, "For measuring only, not saved. Stand still, give each about 5 seconds; the log notes each switch.");
	}

	// the latest changes, each against how it was before them, for their cost
	ImGui::Spacing();
	ImGui::SeparatorText("The latest changes, as before (compare fps, \"terrain\" and \"physics\" in the log)");
	{
		static const struct {
			int bit;
			const char *label, *note;
		} tries[] = {
			{ 20, "Textures past the near size as before (the ring)", "texture size as before" },
			{ 21, "Off screen by the flat ground (holes can show)", "off screen by the flat ground" },
		};
		int mask = GeoSphere::GetGroundSkip();
		for (const auto &t : tries) {
			bool on = (mask & (1 << t.bit)) != 0;
			if (ImGui::Checkbox((std::string(t.label) + "##groundTry").c_str(), &on)) {
				mask = on ? (mask | (1 << t.bit)) : (mask & ~(1 << t.bit));
				GeoSphere::SetGroundSkip(mask);
				FrontierLog::Note(std::string("as before: ") + t.note + (on ? " on" : " off"));
			}
		}
		bool oldRover = Pi::config->Int("RoverSprings", 1) == 0;
		if (ImGui::Checkbox("Rover wheels as before (one point under each, no springs)##groundTry", &oldRover)) {
			Pi::config->SetInt("RoverSprings", oldRover ? 0 : 1);
			FrontierLog::Note(std::string("as before: rover wheels ") + (oldRover ? "on" : "off"));
		}
		ImGui::TextColored(dim, "Not saved. The rover's cost shows in \"physics\", the ground's in \"terrain\".");
	}

	// the cheaper photo textures
	ImGui::Spacing();
	ImGui::SeparatorText("Cheaper photo textures (compare the look and \"terrain\" in the log)");
	{
		bool simpleFar = GeoSphere::TextureSimpleFar();
		if (ImGui::Checkbox("Simple textures far off##openglPlan", &simpleFar)) {
			Pi::config->SetInt("TerrainTextureSimpleFar", simpleFar ? 1 : 0);
			Pi::config->Save();
			FrontierLog::Note(std::string("textures: simple far off ") + (simpleFar ? "on" : "off"));
		}
		ImGui::SameLine();
		float from = float(GeoSphere::TextureSimpleFrom());
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		if (ImGui::SliderFloat("from##openglPlanSimpleFrom", &from, 100.0f, 20000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic))
			Pi::config->SetFloat("TerrainTextureSimpleFrom", from);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			Pi::config->Save();
			FrontierLog::Note("textures: simple from " + std::to_string(int(from)) + " m");
		}
		ImGui::TextColored(dim, "Beyond that (fading in over half as far again): one read of the main material, no coarse copy, no second.");

		bool oneScale = GeoSphere::TextureOneScale();
		if (ImGui::Checkbox("One texture size at a time##openglPlan", &oneScale)) {
			Pi::config->SetInt("TerrainTextureOneScale", oneScale ? 1 : 0);
			Pi::config->Save();
			FrontierLog::Note(std::string("textures: one size at a time ") + (oneScale ? "on" : "off"));
		}
		ImGui::SameLine();
		float band = float(GeoSphere::TextureScaleBand());
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		if (ImGui::SliderFloat("blend##openglPlanScaleBand", &band, 0.02f, 1.0f, "%.2f"))
			Pi::config->SetFloat("TerrainTextureScaleBand", band);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			Pi::config->Save();
			FrontierLog::Note("textures: size blend " + std::to_string(band));
		}
		ImGui::TextColored(dim, "Between two texture sizes, both are read only in this share of the way (1: everywhere, as before).");

		bool turnTable = GeoSphere::TurnTable();
		if (ImGui::Checkbox("Texture turns from a table##openglPlan", &turnTable)) {
			Pi::config->SetInt("TerrainTurnTexture", turnTable ? 1 : 0);
			Pi::config->Save();
			FrontierLog::Note(std::string("textures: turns from a table ") + (turnTable ? "on" : "off"));
		}
		ImGui::TextColored(dim, "The same turns, looked up instead of worked out: never faster, off (on Titan 0.25 ms slower).");
	}

	// P7: the rover's dust on the graphics card
	ImGui::Spacing();
	ImGui::SeparatorText("P7: the rover's dust on the graphics card");
	{
		bool gpuDust = Pi::config->Int("GpuDust", 1) != 0;
		if (ImGui::Checkbox("Rover dust on the graphics card##openglPlan", &gpuDust)) {
			Pi::config->SetInt("GpuDust", gpuDust ? 1 : 0);
			Pi::config->Save();
			FrontierLog::Note(std::string("rover dust on the graphics card: ") + (gpuDust ? "on" : "off"));
		}
		float amount = Pi::config->Float("RoverDustAmount", 1.0f);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		if (ImGui::SliderFloat("how much dust##openglPlanDust", &amount, 0.0f, 8.0f, "x %.1f"))
			Pi::config->SetFloat("RoverDustAmount", amount);
		if (ImGui::IsItemDeactivatedAfterEdit())
			Pi::config->Save();
		ImGui::TextColored(dim, "The same puffs as before, moved and drawn on the graphics card (GPU time: \"dust\"), up to 16384 at once.");
		ImGui::TextColored(dim, "x1 throws up as many as before; more is cheap now. Switching it takes effect for new puffs.");
		ImGui::TextColored(dim, "Ships near the ground: dust off dusty ground, little off rock and plants, frost off ice, spray off water.");
	}

	// P8: the sky from a table
	ImGui::Spacing();
	ImGui::SeparatorText("P8: the sky's light from a table, worked out once a frame");
	{
		bool skyTable = GeoSphere::SkyTableEnabled();
		if (ImGui::Checkbox("Sky from a table (with the camera in the air)##openglPlan", &skyTable)) {
			Pi::config->SetInt("SkyTable", skyTable ? 1 : 0);
			Pi::config->Save();
			FrontierLog::Note(std::string("sky from a table: ") + (skyTable ? "on" : "off"));
		}
		ImGui::TextColored(dim, "Should look the same. GPU time: \"atmosphere\" (the sky) and \"sky table\" (filling the table) in the log.");
		ImGui::TextColored(dim, "Only with the realistic scattering setting at its highest; from outside the air the sky works as before.");
	}

	// passes over the whole picture (ScreenEffects.h)
	ImGui::Spacing();
	ImGui::SeparatorText("Screen effects: crease shading and anti-aliasing");
	{
		bool creases = ScreenEffects::CreasesEnabled();
		if (ImGui::Checkbox("Shade in creases##screenEffects", &creases)) {
			Pi::config->SetInt("CreaseShade", creases ? 1 : 0);
			Pi::config->Save();
			FrontierLog::Note(std::string("crease shading: ") + (creases ? "on" : "off"));
		}
		if (ScreenEffects::CreasesFailed())
			ImGui::TextColored(no, "Its shaders didn't compile (see the log); it stays off.");
		const auto slider = [](const char *label, const char *key, float def, float lo, float hi, const char *format, const char *note) {
			float value = std::clamp(Pi::config->Float(key, def), lo, hi);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
			if (ImGui::SliderFloat(label, &value, lo, hi, format))
				Pi::config->SetFloat(key, value);
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				Pi::config->Save();
				FrontierLog::Note(fmt::format("{} {}", note, value));
			}
		};
		slider("strength##creaseStrength", "CreaseShadeStrength", 4.0f, 0.0f, 4.0f, "x %.2f", "crease shading: strength");
		slider("reach##creaseReach", "CreaseShadeReach", 4.0f, 0.1f, 4.0f, "%.2f m", "crease shading: reach (m)");
		slider("without air##creaseVacuum", "CreaseShadeVacuum", 0.5f, 0.0f, 1.0f, "%.2f", "crease shading: share without air");
		ImGui::TextColored(dim, "GTAO: the sky hidden by what rises round each point, out to the reach. Fades out from 200 to 800 m away.");
		ImGui::TextColored(dim, "Strength: x1 as worked out, more darker. Without air: its strength there, as a share of in Earth's air.");
		ImGui::TextColored(dim, "It darkens the picture as drawn, sunlit or not. GPU time: \"creases\" in the log.");

		// (chosen in the settings window, Video tab: Anti-aliasing)
		static const char *const modeNames[3] = { "off", "FXAA", "SMAA" };
		const int mode = ScreenEffects::EdgeSmoothingMode();
		ImGui::Text("Anti-aliasing: %s (set in the settings window, Video tab)", modeNames[mode]);
		if (ScreenEffects::EdgeSmoothingFailed(mode))
			ImGui::TextColored(no, "Its shaders didn't compile (see the log); it stays off.");
		ImGui::TextColored(dim, "GPU time: \"edge smoothing\" in the log. Smooths each picture on its own: the shimmer of distant textures as you move stays.");
	}

	// P6: compute fills an image every frame while ticked, and the tessellation stages compile and link
	ImGui::Spacing();
	ImGui::SeparatorText("P6: compute and tessellation, tested");
	{
		static bool s_computeTest = false;
		static RefCountedPtr<Graphics::Texture> s_testImage;
		static RefCountedPtr<Graphics::Material> s_computeMaterial, s_tessMaterial;
		static bool s_tessLinked = false, s_computeLinked = false;
		if (ImGui::Checkbox("Run the compute test every frame##openglPlan", &s_computeTest) && s_computeTest && !s_testImage) {
			const Graphics::TextureDescriptor desc(Graphics::TEXTURE_RGBA16F, vector3f(256.0f, 256.0f, 0.0f), Graphics::LINEAR_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
			s_testImage.Reset(Pi::renderer->CreateTexture(desc));
			s_computeMaterial.Reset(Pi::renderer->CreateMaterial("compute_test", Graphics::MaterialDescriptor(), Graphics::RenderStateDesc(), Graphics::VertexFormatDesc()));
			s_computeMaterial->SetTexture("outImage"_hash, s_testImage.Get());
			s_computeLinked = Pi::renderer->MaterialCompiled(s_computeMaterial.Get());
			Graphics::RenderStateDesc patches;
			patches.primitiveType = Graphics::PATCHES;
			patches.patchVertices = 3;
			Graphics::MaterialDescriptor tessDesc;
			tessDesc.tessellated = true;
			s_tessMaterial.Reset(Pi::renderer->CreateMaterial("tess_test", tessDesc, patches,
				Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION)));
			s_tessLinked = Pi::renderer->MaterialCompiled(s_tessMaterial.Get());
			FrontierLog::Note(std::string("P6 tests: the compute program ") + (s_computeLinked ? "compiled and linked" : "FAILED") +
				", the tessellation program (four stages) " + (s_tessLinked ? "compiled and linked" : "FAILED"));
		}
		if (s_computeTest && s_computeMaterial) {
			s_computeMaterial->SetPushConstant("Time"_hash, float(ImGui::GetTime()));
			Pi::renderer->BeginGpuTimer("compute test");
			Pi::renderer->DispatchCompute(s_computeMaterial.Get(), 256 / 8, 256 / 8, 1);
			Pi::renderer->EndGpuTimer();
			Pi::renderer->AddComputeBarrier(Graphics::Renderer::BARRIER_TEXTURE_FETCH | Graphics::Renderer::BARRIER_IMAGE_ACCESS);
		}
		if (s_testImage) {
			ImGui::Text("Compute program: %s; tessellation program: %s", s_computeLinked ? "linked" : "FAILED", s_tessLinked ? "linked" : "FAILED");
			ImGui::Image(ImTextureID(s_testImage.Get()), { 128, 128 });
			ImGui::TextColored(dim, "Moving rings: compute ran. Its GPU time is \"compute test\" in the log's gpu ms.");
		}
	}

	// P0.4 and the height profile
	ImGui::Spacing();
	ImGui::SeparatorText("To the log");
	if (!FrontierLog::Enabled())
		ImGui::TextColored(no, "The log is off, so these write nothing: tick 'Write frontier_log.txt' above.");
	if (ImGui::Button("Landform Cost on the CPU##openglPlan"))
		FrontierLog::LandformCost();
	ImGui::SameLine();
	if (ImGui::Button("Height Profile Around the Ship##openglPlan"))
		FrontierLog::HeightProfile();
	ImGui::TextColored(dim, "Landform Cost: the GPU tiles' landforms timed on the CPU (P0.4), and how fine the heights' detail goes (Q3).");
}

void PiGui::PerfInfo::DrawFrontierTerrain()
{
	// the rover can be deployed without the surface rover equipment (Excursion)
	bool alwaysRover = Pi::config->Int("DebugAlwaysAllowRover", 0) != 0;
	if (ImGui::Checkbox("Always Allow Rover", &alwaysRover)) {
		Pi::config->SetInt("DebugAlwaysAllowRover", alwaysRover ? 1 : 0);
		Pi::config->Save();
	}

	const std::vector<Graphics::Renderer::GpuTiming> &gpuTimings = Pi::renderer->GetGpuTimings();
	if (!gpuTimings.empty()) {
		ImGui::Spacing();
		ImGui::SeparatorText("GPU time (ms, a few frames old)");
		for (const Graphics::Renderer::GpuTiming &t : gpuTimings)
			ImGui::Text("%*s%s: %.2f", t.depth * 2, "", t.name.c_str(), t.milliseconds);
	}

	ImGui::Spacing();
	ImGui::SeparatorText("Terrain");

	using Flags = GeoSphere::DebugFlags;
	uint32_t debugFlags = GeoSphere::GetDebugFlags();
	bool showDepth = debugFlags & Flags::DEBUG_PATCHDEPTH;
	bool noBandLimit = debugFlags & Flags::DEBUG_NOBANDLIMIT;
	bool regenerate = ImGui::Checkbox("Colour Patches by Depth", &showDepth);
	regenerate |= ImGui::Checkbox("Disable Noise Band Limit (A/B)", &noBandLimit);

	// these options are baked into patches when they are generated, so rebuild them all
	if (regenerate) {
		debugFlags = (debugFlags & ~uint32_t(Flags::DEBUG_PATCHDEPTH | Flags::DEBUG_NOBANDLIMIT)) |
			(showDepth ? Flags::DEBUG_PATCHDEPTH : 0) |
			(noBandLimit ? Flags::DEBUG_NOBANDLIMIT : 0);
		GeoSphere::SetDebugFlags(debugFlags);
		Pi::config->Save();
		Pi::OnChangeDetailLevel();
	}

	// how early terrain patches subdivide (also in the settings window); applies live
	float lodDistance = float(GeoSphere::GetLodDistanceScale());
	if (ImGui::SliderFloat("Terrain Detail Distance", &lodDistance, 1.0f, 30.0f, "%.1f patch widths")) {
		GeoSphere::SetLodDistanceScale(lodDistance);
		Pi::config->SetFloat("PlanetLodDistance", float(GeoSphere::GetLodDistanceScale()));
	}
	if (ImGui::IsItemDeactivatedAfterEdit())
		Pi::config->Save();
	ImGui::Text("(peak value; reduced near the ground and in high orbit)");

	// how much later patches seen edge-on split (GeoPatch::LODUpdate); applies live
	int foreshortening = std::max(0, std::min(100, Pi::config->Int("PlanetLodForeshortening", 100)));
	if (ImGui::SliderInt("Edge-on Terrain Reduction", &foreshortening, 0, 100, "%d%%"))
		Pi::config->SetInt("PlanetLodForeshortening", foreshortening);
	if (ImGui::IsItemDeactivatedAfterEdit())
		Pi::config->Save();

	// GPU detail tiles (TerrainTiles); off frees the atlas, for A/B comparison
	bool detailTiles = Pi::config->Int("TerrainDetailTiles", 1) != 0;
	if (ImGui::Checkbox("GPU Detail Tiles", &detailTiles)) {
		Pi::config->SetInt("TerrainDetailTiles", detailTiles ? 1 : 0);
		Pi::config->Save();
	}

	// how a planet is drawn from high orbit (GeoSphere.cpp); changes at once
	int orbitSurface = Pi::config->Int("PlanetOrbitSurface", 2) == 1 ? 0 : 1;
	if (ImGui::Combo("Orbit Surface", &orbitSurface, "Tiles\0" "Tiles, full detail\0")) {
		Pi::config->SetInt("PlanetOrbitSurface", orbitSurface + 1);
		Pi::config->Save();
		static const char *const names[] = { "tiles", "tiles, full detail" };
		FrontierLog::Note(std::string("orbit surface: ") + names[orbitSurface]);
	}

	// sharper tiles from the same texels (terrain-far.glsl); changes at once
	bool bicubic = Pi::config->Int("TerrainTileBicubic", 1) != 0;
	if (ImGui::Checkbox("Bicubic Tiles", &bicubic)) {
		Pi::config->SetInt("TerrainTileBicubic", bicubic ? 1 : 0);
		Pi::config->Save();
		FrontierLog::Note(std::string("bicubic tiles: ") + (bicubic ? "on" : "off"));
	}

	// the patches' surface tiles (TerrainSurfaceTiles); off frees the atlas,
	// for A/B comparison
	bool surfaceTiles = TerrainSurfaceTiles::Enabled();
	if (ImGui::Checkbox("Surface Texture Tiles", &surfaceTiles)) {
		Pi::config->SetInt("TerrainSurfaceTiles", surfaceTiles ? 1 : 0);
		Pi::config->Save();
	}
	if (const TerrainSurfaceTiles *tiles = TerrainSurfaceTiles::Instance())
		ImGui::Text("Surface tiles: %d in use, %d baking, %d waiting", tiles->GetTileCount(), tiles->GetBakingCount(), tiles->GetWaitingCount());

	// the order the patches are drawn in (GeoSphere::Render), for A/B comparison; changes at once
	bool nearestFirst = Pi::config->Int("TerrainNearestFirst", 1) != 0;
	if (ImGui::Checkbox("Draw Nearest Patches First", &nearestFirst)) {
		Pi::config->SetInt("TerrainNearestFirst", nearestFirst ? 1 : 0);
		Pi::config->Save();
		FrontierLog::Note(std::string("nearest patches first: ") + (nearestFirst ? "on" : "off"));
	}

	// the sharpest steps in the ground around the ship, in the log; takes a moment
	if (ImGui::Button("Height Profile Around the Ship (to the log)"))
		FrontierLog::HeightProfile();
	// what the GPU detail tiles' landforms would cost on the CPU, and how fine the heights go; takes a moment
	if (ImGui::Button("Landform Cost on the CPU (to the log)"))
		FrontierLog::LandformCost();

	const GeoPatchGenStats stats = GetGeoPatchGenStats();
	const double totalMs = double(stats.microseconds) / 1000.0;
	const double perPatchUs = stats.patches ? double(stats.microseconds) / double(stats.patches) : 0.0;
	ImGui::Text("Patch generation: %llu jobs, %llu patches", (unsigned long long)stats.jobs, (unsigned long long)stats.patches);
	ImGui::Text("Generation CPU time: %.1f ms total, %.1f us per patch", totalMs, perPatchUs);
	if (ImGui::Button("Reset Patch Stats")) {
		ResetGeoPatchGenStats();
		Terrain::ResetSpeedCounters();
	}
}

namespace {
	// The surface starports of up to 100 random bodies within 20 sectors of Sol, placed as the game places them (Space::
	// PlaceSurfaceStarport), and how the ground is where they ended up, in output.txt. It builds each body's terrain on the
	// spot, so it takes a while.
	void CheckStarports()
	{
		static constexpr int BODIES = 100;
		static constexpr int SECTORS = 20;
		static constexpr double LAVA_SHARE = 0.25;
		RefCountedPtr<Galaxy> galaxy = Pi::game->GetGalaxy();
		Random rng(20261107u);
		std::vector<SystemPath> seen;
		int bodies = 0, ports = 0, moved = 0, uneven = 0, wet = 0, lavaPorts = 0;
		Log::Info("STARPORT CHECK: the surface starports of up to {} random bodies within {} sectors of Sol", BODIES, SECTORS);
		for (int attempt = 0; attempt < 5000 && bodies < BODIES; attempt++) {
			const SystemPath sectorPath(int(rng.Int32(2 * SECTORS + 1)) - SECTORS, int(rng.Int32(2 * SECTORS + 1)) - SECTORS, int(rng.Int32(2 * SECTORS + 1)) - SECTORS);
			RefCountedPtr<const Sector> sector = galaxy->GetSector(sectorPath);
			if (!sector || sector->m_systems.empty())
				continue;
			const SystemPath path = sector->m_systems[rng.Int32(int(sector->m_systems.size()))].GetPath();
			if (std::find(seen.begin(), seen.end(), path) != seen.end())
				continue;
			seen.push_back(path);
			RefCountedPtr<StarSystem> system = galaxy->GetStarSystem(path);
			if (!system)
				continue;
			for (const RefCountedPtr<SystemBody> &body : system->GetBodies()) {
				if (bodies >= BODIES)
					break;
				if (body->GetSuperType() != SystemBody::SUPERTYPE_ROCKY_PLANET)
					continue;
				std::vector<const SystemBody *> starports;
				for (const SystemBody *kid : body->GetChildren())
					if (kid->GetType() == SystemBody::TYPE_STARPORT_SURFACE)
						starports.push_back(kid);
				if (starports.empty())
					continue;
				bodies++;
				RefCountedPtr<Terrain> terrain(Terrain::InstanceTerrain(body.Get()));
				const double radius = body->GetRadius();
				const double maxFeatureRadius = (terrain->GetMaxHeight() + 1.0) * radius;
				const TerrainMaterialTable &table = terrain->GetMaterialTable();
				// how much of it is sea and lava, from its geology
				std::string shares;
				for (const std::string &line : terrain->GetGeologyInfo()) {
					const size_t start = line.find_first_not_of(' ');
					if (start != std::string::npos && (line.compare(start, 7, "Became:") == 0 || line.compare(start, 4, "Sea ") == 0))
						shares += (shares.empty() ? "" : " | ") + line.substr(start);
				}
				Log::Info("STARPORT CHECK body {} in the {} system ({}, {}, {} #{}, {}): {}", body->GetName(), system->GetName(), path.sectorX, path.sectorY, path.sectorZ, path.systemIndex,
					system->GetUnexplored() ? "unexplored" : "explored", shares);
				std::vector<vector3d> placed;
				for (const SystemBody *starport : starports) {
					vector3d pos;
					matrix3x3d rot;
					Space::StarportPlacement report;
					Space::PlaceSurfaceStarport(starport, terrain.Get(), radius, maxFeatureRadius, placed, pos, rot, false, &report);
					placed.push_back(pos);
					// lava under its middle
					const vector3d up = pos.Normalized();
					vector3d colour;
					uint8_t weights[TerrainMaterialTable::SLOT_COUNT];
					terrain->GetSurface(up, 0.0, up, colour, weights);
					double lava = 0.0;
					for (int slot = 0; slot < table.SlotCount(); slot++)
						if (table.SlotLayer(slot) == TerrainMaterialTable::LAVA)
							lava += weights[slot] / 255.0;
					ports++;
					moved += report.moved ? 1 : 0;
					uneven += report.level ? 0 : 1;
					wet += report.underwater ? 1 : 0;
					lavaPorts += lava > LAVA_SHARE ? 1 : 0;
					const bool problem = !report.level || report.underwater || lava > LAVA_SHARE;
					const std::string lavaText = lava > LAVA_SHARE ? fmt::format(", on lava ({:.0f} %)", 100.0 * lava) : std::string();
					Log::Info("STARPORT CHECK {}{} on {} ({}, {}, {} #{}, {} terrain): {}, the ground varies {:.1f} m of {:.1f} allowed, {} tries{}{}{}; found by {}, {} tries dry, land search {} spots far and {} near other starports",
						problem ? "PROBLEM " : "", starport->GetName(), body->GetName(), path.sectorX, path.sectorY, path.sectorZ, path.systemIndex,
						terrain->GetHeightFractalName(), report.moved ? "moved" : "where the system put it", report.variation, report.maxVariation,
						report.tries, report.level ? "" : ", too uneven", report.underwater ? ", in water" : (report.byWater ? ", by water" : ""), lavaText,
						report.how, report.dryTries, report.landSpots, report.crowdedSpots);
				}
			}
		}
		Log::Info("STARPORT CHECK done: {} bodies, {} starports: {} moved, {} on ground too uneven, {} in water, {} on lava", bodies, ports, moved, uneven, wet, lavaPorts);
		FrontierLog::Note(fmt::format("starport check: {} bodies, {} starports, {} too uneven, {} in water, {} on lava (output.txt)", bodies, ports, uneven, wet, lavaPorts));
	}

	// How rough the solid ground of up to 100 random rocky bodies within 20 sectors of Sol is, at full detail, in
	// output.txt: at random points on land (not in water or on lava), the bumpiness at 30 m, 300 m and 3 km, as the
	// angle of the bend between the ground on either side (the second difference over the distance), which a steady
	// slope doesn't count. A body whose median at 30 m and 300 m are both under FLAT_DEGREES is marked FLAT.
	void CheckFlatness()
	{
		static constexpr int BODIES = 100;
		static constexpr int SECTORS = 20;
		static constexpr int POINTS = 48;
		static constexpr double FLAT_DEGREES = 0.3;
		static constexpr double LIQUID_SHARE = 0.5;
		static constexpr double PI = 3.14159265358979323846;
		static constexpr double SCALES[3] = { 30.0, 300.0, 3000.0 };
		RefCountedPtr<Galaxy> galaxy = Pi::game->GetGalaxy();
		Random rng(20261108u);
		std::vector<SystemPath> seen;
		int bodies = 0, flat = 0;
		Log::Info("FLATNESS CHECK: the solid ground of up to {} random rocky bodies within {} sectors of Sol, bumpiness in degrees at 30 m, 300 m and 3 km", BODIES, SECTORS);
		TerrainNoise::ScopedBandLimit fullDetail(0.0);
		for (int attempt = 0; attempt < 5000 && bodies < BODIES; attempt++) {
			const SystemPath sectorPath(int(rng.Int32(2 * SECTORS + 1)) - SECTORS, int(rng.Int32(2 * SECTORS + 1)) - SECTORS, int(rng.Int32(2 * SECTORS + 1)) - SECTORS);
			RefCountedPtr<const Sector> sector = galaxy->GetSector(sectorPath);
			if (!sector || sector->m_systems.empty())
				continue;
			const SystemPath path = sector->m_systems[rng.Int32(int(sector->m_systems.size()))].GetPath();
			if (std::find(seen.begin(), seen.end(), path) != seen.end())
				continue;
			seen.push_back(path);
			RefCountedPtr<StarSystem> system = galaxy->GetStarSystem(path);
			if (!system)
				continue;
			for (const RefCountedPtr<SystemBody> &body : system->GetBodies()) {
				if (bodies >= BODIES)
					break;
				if (body->GetSuperType() != SystemBody::SUPERTYPE_ROCKY_PLANET)
					continue;
				RefCountedPtr<Terrain> terrain(Terrain::InstanceTerrain(body.Get()));
				const double radius = body->GetRadius();
				const TerrainMaterialTable &table = terrain->GetMaterialTable();
				std::vector<double> bumps[3];
				int liquid = 0;
				for (int i = 0; i < POINTS; i++) {
					const double y = 2.0 * rng.Double() - 1.0;
					const double angle = 2.0 * PI * rng.Double();
					const double r = std::sqrt(std::max(0.0, 1.0 - y * y));
					const vector3d p(r * std::cos(angle), y, r * std::sin(angle));
					double h0 = 0.0;
					terrain->GetHeights(&p, &h0, 1);
					// water: below 0 altitude; lava: most of its material
					bool fluid = terrain->GetSurfaceAltitude(p, h0) < 0.0;
					if (!fluid) {
						vector3d colour;
						uint8_t weights[TerrainMaterialTable::SLOT_COUNT];
						terrain->GetSurface(p, h0, p, colour, weights);
						double lava = 0.0;
						for (int slot = 0; slot < table.SlotCount(); slot++)
							if (table.SlotLayer(slot) == TerrainMaterialTable::LAVA)
								lava += weights[slot] / 255.0;
						fluid = lava > LIQUID_SHARE;
					}
					if (fluid) {
						liquid++;
						continue;
					}
					const vector3d e1 = (std::abs(p.y) < 0.9 ? p.Cross(vector3d(0.0, 1.0, 0.0)) : p.Cross(vector3d(1.0, 0.0, 0.0))).Normalized();
					const vector3d e2 = p.Cross(e1);
					for (int s = 0; s < 3; s++) {
						const double step = SCALES[s] / radius;
						const vector3d around[4] = { (p + e1 * step).Normalized(), (p - e1 * step).Normalized(), (p + e2 * step).Normalized(), (p - e2 * step).Normalized() };
						double h[4];
						terrain->GetHeights(around, h, 4);
						const double bend = 0.5 * (std::abs(h[0] + h[1] - 2.0 * h0) + std::abs(h[2] + h[3] - 2.0 * h0)) * radius / SCALES[s];
						bumps[s].push_back(std::atan(bend) * 180.0 / PI);
					}
				}
				bodies++;
				std::string geology;
				for (const std::string &line : terrain->GetGeologyInfo()) {
					const size_t start = line.find_first_not_of(' ');
					if (start != std::string::npos && (line.compare(start, 9, "Interior:") == 0 || line.compare(start, 7, "Became:") == 0))
						geology += (geology.empty() ? "" : " | ") + line.substr(start);
				}
				if (bumps[0].empty()) {
					Log::Info("FLATNESS CHECK body {} ({}, {}, {} #{}, {} terrain): all {} points liquid | {}", body->GetName(), path.sectorX, path.sectorY, path.sectorZ,
						path.systemIndex, terrain->GetHeightFractalName(), POINTS, geology);
					continue;
				}
				double median[3], low[3];
				for (int s = 0; s < 3; s++) {
					std::vector<double> &b = bumps[s];
					std::sort(b.begin(), b.end());
					median[s] = b[b.size() / 2];
					low[s] = b[b.size() / 10];
				}
				const bool isFlat = median[0] < FLAT_DEGREES && median[1] < FLAT_DEGREES;
				flat += isFlat ? 1 : 0;
				Log::Info("FLATNESS CHECK {}body {} ({}, {}, {} #{}, {} terrain): {} land points, {} liquid; median {:.2f} / {:.2f} / {:.2f} deg, lowest tenth {:.2f} / {:.2f} / {:.2f} deg | {}",
					isFlat ? "FLAT " : "", body->GetName(), path.sectorX, path.sectorY, path.sectorZ, path.systemIndex, terrain->GetHeightFractalName(),
					int(bumps[0].size()), liquid, median[0], median[1], median[2], low[0], low[1], low[2], geology);
			}
		}
		Log::Info("FLATNESS CHECK done: {} bodies, {} flat (median under {} deg at 30 m and 300 m)", bodies, flat, FLAT_DEGREES);
		FrontierLog::Note(fmt::format("flatness check: {} bodies, {} flat (output.txt)", bodies, flat));
	}

	// The highest and lowest points of Sol's rocky bodies and of up to 200 random ones within 20 sectors, and how their
	// heights spread, in output.txt, to judge whether the relief is realistic: Sol's bodies against their real values, the
	// others against what rock and ice can hold up. Heights are of the solid ground (the floor under seas), in km from sea
	// level, or from the mean height where there is no sea. It only measures; nothing in the terrain changes.
	void CheckRelief()
	{
		static constexpr int BODIES = 200;
		static constexpr int SECTORS = 20;
		static constexpr int SAMPLES = 16000; // evenly spread, then a climb from the highest and lowest of them
		static constexpr double TAIL = 0.1; // the lowest and highest share of the samples, for what makes them
		static constexpr int HIGH_CANDIDATES = 16, LOW_CANDIDATES = 16, PEAK_CANDIDATES = 30, VOLCANO_CANDIDATES = 30;
		static constexpr double CANDIDATE_APART = 3.0; // sample spacings between the climbs' starts
		static constexpr double CLIMB_LEAST_M = 5.0; // the climb stops when its step is this small
		static constexpr int CLIMB_STEPS = 400;
		static constexpr double PI = 3.14159265358979323846;
		static constexpr double GOLDEN_ANGLE = 2.39996322972865332;
		static constexpr double G = 6.674e-11;
		// about the tallest rock holds up above its surroundings at Earth's gravity; ice holds about half
		static constexpr double ROCK_LIMIT_M = 9000.0, EARTH_G = 9.81, ICE_LIMIT = 0.5;
		static constexpr double LIMIT_SLACK = 1.5; // Earth's highest is about 1.25 times the limit above its mean
		// total relief over the strength limit, on round bodies: rock between about 0.2 (Moon 0.37, Mercury 0.42) and 3.5
		// (Earth 2.2, Mars 1.2); ice up to about 0.5 (Ganymede 0.08, Titan 0.07)
		static constexpr double ROCK_LEAST = 0.2, ROCK_MOST = 3.5, ICE_MOST = 0.5;
		// total relief over the radius on bodies too small to be round: Amalthea about 50 %, Hyperion 30 %, Proteus 10 %
		static constexpr double LUMPY_KM = 100.0, LUMPY_MOST = 0.5, ROUNDING_KM = 300.0, ROUNDING_MOST = 0.25;
		static constexpr int BINS = 48;
		// the height layers: shape, large craters, small craters, small relief, lines, maps, volcano
		static constexpr int LAYER_COUNT = 7;
		using LayerValues = std::array<double, LAYER_COUNT>;
		static constexpr double LEVEL_LEAST = 0.2; // a level holds at least this share of the fullest bin
		static constexpr double LEVEL_GAP = 0.6; // and the dip between two levels is under this share of the lower

		// real values in km, from sea level (Earth) or the mean radius; NaN where nobody knows
		struct Real {
			const char *name;
			double highest, lowest, total;
			const char *note;
		};
		const double NONE = std::nan("");
		const Real REAL[] = {
			{ "Mercury", 4.48, -5.38, 9.9, "MESSENGER global model" },
			{ "Venus", 10.7, NONE, 13.0, "Maxwell Montes; total from Pioneer Venus, 93 % mapped" },
			{ "Earth", 8.85, -10.94, 19.8, "Everest, Challenger Deep" },
			{ "Moon", 10.79, -9.13, 19.9, "LRO laser altimeter" },
			{ "Mars", 21.2, -8.2, 29.4, "Olympus Mons, Hellas" },
			{ "Io", NONE, NONE, NONE, "tallest mountain about 17.5 km above the plains" },
			{ "Europa", NONE, NONE, 1.0, "relief under about 1 km" },
			{ "Ganymede", NONE, NONE, 2.5, "largest known relief about 2.5 km" },
			{ "Callisto", NONE, NONE, NONE, "no global data; worn down" },
			{ "Titan", 0.52, -1.7, 2.2, "sparse radar data, 9 % covered" },
			{ "Pluto", NONE, NONE, NONE, "mountains up to 6 km above their base, Sputnik Planitia about 2 km below the mean" },
			{ "Vesta", 19.0, -22.0, 41.0, "Dawn, from an ellipsoid; Rheasilvia" },
			{ "Ceres", 7.5, -7.5, 15.0, "Dawn, from an ellipsoid" },
			{ "Tethys", NONE, NONE, 10.0, "Odysseus about 8 km deep; maps span about 10 km" },
			{ "Dione", NONE, NONE, 10.0, "maps span about 10 km" },
			{ "Rhea", NONE, NONE, 10.0, "maps span about 10 km" },
			{ "Iapetus", 15.0, -9.0, 24.0, "leading side; equatorial ridge up to about 20 km" },
			{ "Triton", NONE, NONE, 2.0, "features under about 1 km, no deep basins" },
		};
		auto km = [](double value) { return std::isnan(value) ? std::string("?") : fmt::format("{:+.1f}", value); };

		// runs fn(0..count-1) on all cores, each thread at the given band limit
		auto parallelFor = [](int count, double band, const std::function<void(int)> &fn) {
			const int threads = int(std::max(1u, std::thread::hardware_concurrency()));
			std::atomic<int> next(0);
			std::vector<std::thread> pool;
			for (int t = 0; t < threads; t++)
				pool.emplace_back([&]() {
					TerrainNoise::ScopedBandLimit limit(band);
					for (int i = next++; i < count; i = next++)
						fn(i);
				});
			for (std::thread &thread : pool)
				thread.join();
		};

		int bodies = 0;
		int flagged[6] = {};
		static const char *FLAG_NAMES[6] = { "TOO TALL", "TOO DEEP", "TOO MUCH RELIEF", "TOO LITTLE RELIEF", "ONE LEVEL", "" };

		auto check = [&](const SystemBody *body, const SystemPath &path) {
			RefCountedPtr<Terrain> terrain(Terrain::InstanceTerrain(body));
			const double radius = body->GetRadius();
			const double gravity = G * body->GetMass() / (radius * radius);
			std::string geology, fullGeology;
			bool hasSea = false, plates = false, ice = false;
			for (const std::string &line : terrain->GetGeologyInfo()) {
				const size_t start = line.find_first_not_of(' ');
				if (start == std::string::npos)
					continue;
				if (line.compare(start, 6, "Speed:") != 0)
					fullGeology += (fullGeology.empty() ? "" : " | ") + line.substr(start);
				hasSea = hasSea || line.compare(start, 11, "Levels: sea") == 0;
				plates = plates || line.compare(start, 9, "Plates 1,") == 0;
				ice = ice || (line.compare(start, 9, "Interior:") == 0 && line.find("ice crust") != std::string::npos);
				if (line.compare(start, 9, "Interior:") == 0 || line.compare(start, 7, "Became:") == 0)
					geology += (geology.empty() ? "" : " | ") + line.substr(start);
			}

			// metres of the solid ground: the sea's floor where the height stops at sea level
			auto solid = [&](const vector3d &p) {
				double h = 0.0;
				terrain->GetHeights(&p, &h, 1);
				const double metres = h * radius;
				if (metres < 1.0) {
					const double altitude = terrain->GetSurfaceAltitude(p, h);
					if (altitude < 0.0)
						return altitude;
				}
				return metres;
			};

			std::vector<vector3d> points(SAMPLES);
			for (int i = 0; i < SAMPLES; i++) {
				const double y = 1.0 - 2.0 * (i + 0.5) / SAMPLES;
				const double r = std::sqrt(std::max(0.0, 1.0 - y * y));
				points[i] = vector3d(r * std::cos(i * GOLDEN_ANGLE), y, r * std::sin(i * GOLDEN_ANGLE));
			}
			const double spacing = std::sqrt(4.0 * PI / SAMPLES); // radians
			std::vector<double> heights(SAMPLES);
			std::vector<Terrain::HeightLayers> layers(SAMPLES);
			std::atomic<bool> hasLayers(true);
			// a few times the finest the samples can tell apart; the climbs below go to full detail
			parallelFor(SAMPLES, 2.0 / spacing, [&](int i) {
				heights[i] = solid(points[i]);
				if (!terrain->GetHeightLayers(points[i], layers[i]))
					hasLayers = false;
			});

			double mean = 0.0;
			for (const double h : heights)
				mean += h / SAMPLES;
			const double reference = hasSea ? 0.0 : mean;

			// where the climbs start: the highest and lowest samples, apart from each other, and the map's peaks and volcanoes
			std::vector<int> order(SAMPLES);
			for (int i = 0; i < SAMPLES; i++)
				order[i] = i;
			std::sort(order.begin(), order.end(), [&](int a, int b) { return heights[a] > heights[b]; });
			const double apart = std::cos(CANDIDATE_APART * spacing);
			auto pick = [&](auto begin, auto end, int most, std::vector<vector3d> &out) {
				std::vector<vector3d> picked;
				for (auto it = begin; it != end && int(picked.size()) < most; ++it) {
					const vector3d &p = points[*it];
					if (std::none_of(picked.begin(), picked.end(), [&](const vector3d &q) { return p.Dot(q) > apart; }))
						picked.push_back(p);
				}
				out.insert(out.end(), picked.begin(), picked.end());
			};
			std::vector<vector3d> highStarts, lowStarts;
			pick(order.begin(), order.end(), HIGH_CANDIDATES, highStarts);
			pick(order.rbegin(), order.rend(), LOW_CANDIDATES, lowStarts);
			const std::pair<const char *, int> kinds[2] = { { "peak", PEAK_CANDIDATES }, { "volcano", VOLCANO_CANDIDATES } };
			for (const auto &[kind, most] : kinds) {
				std::vector<Terrain::Feature> features = terrain->FindFeatures(kind, vector3d(0.0));
				std::sort(features.begin(), features.end(), [](const Terrain::Feature &a, const Terrain::Feature &b) { return a.size > b.size; });
				for (int i = 0; i < int(features.size()) && i < most; i++)
					highStarts.push_back(features[i].position.Normalized());
			}

			// up (sign 1) or down (-1) in eight directions, halving the step when none is better
			auto climb = [&](vector3d p, double sign, vector3d &at, double &height) {
				double best = sign * solid(p);
				double step = 0.5 * spacing;
				for (int i = 0; i < CLIMB_STEPS && step * radius > CLIMB_LEAST_M; i++) {
					const vector3d e1 = (std::abs(p.y) < 0.9 ? p.Cross(vector3d(0.0, 1.0, 0.0)) : p.Cross(vector3d(1.0, 0.0, 0.0))).Normalized();
					const vector3d e2 = p.Cross(e1);
					vector3d next = p;
					for (int k = 0; k < 8; k++) {
						const double angle = k * PI / 4.0;
						const vector3d q = (p + (e1 * std::cos(angle) + e2 * std::sin(angle)) * step).Normalized();
						const double h = sign * solid(q);
						if (h > best) {
							best = h;
							next = q;
						}
					}
					if (next == p)
						step *= 0.5;
					p = next;
				}
				at = p;
				height = sign * best;
			};
			std::vector<vector3d> highAt(highStarts.size()), lowAt(lowStarts.size());
			std::vector<double> highHeight(highStarts.size()), lowHeight(lowStarts.size());
			const int highs = int(highStarts.size());
			parallelFor(highs + int(lowStarts.size()), 0.0, [&](int i) {
				if (i < highs)
					climb(highStarts[i], 1.0, highAt[i], highHeight[i]);
				else
					climb(lowStarts[i - highs], -1.0, lowAt[i - highs], lowHeight[i - highs]);
			});
			vector3d highest = points[order.front()], lowest = points[order.back()];
			double top = heights[order.front()], bottom = heights[order.back()];
			for (size_t i = 0; i < highAt.size(); i++)
				if (highHeight[i] > top) {
					top = highHeight[i];
					highest = highAt[i];
				}
			for (size_t i = 0; i < lowAt.size(); i++)
				if (lowHeight[i] < bottom) {
					bottom = lowHeight[i];
					lowest = lowAt[i];
				}

			// how the heights spread: the share at 1, 10, 50, 90 and 99 %, and the levels most ground sits at
			std::vector<double> sorted = heights;
			std::sort(sorted.begin(), sorted.end());
			auto at = [&](double share) { return sorted[std::clamp(int(share * (SAMPLES - 1)), 0, SAMPLES - 1)]; };
			const double low = at(0.01), high = at(0.99);
			std::vector<int> levels;
			if (high - low > 1.0) {
				double bins[BINS] = {}, smooth[BINS] = {};
				for (const double h : heights)
					bins[std::clamp(int((h - low) / (high - low) * BINS), 0, BINS - 1)] += 1.0;
				for (int i = 0; i < BINS; i++)
					smooth[i] = 0.25 * bins[std::max(i - 1, 0)] + 0.5 * bins[i] + 0.25 * bins[std::min(i + 1, BINS - 1)];
				const double fullest = *std::max_element(smooth, smooth + BINS);
				std::vector<int> tops;
				for (int i = 0; i < BINS; i++)
					if ((i == 0 || smooth[i] >= smooth[i - 1]) && (i == BINS - 1 || smooth[i] > smooth[i + 1]) && smooth[i] >= LEVEL_LEAST * fullest)
						tops.push_back(i);
				std::sort(tops.begin(), tops.end(), [&](int a, int b) { return smooth[a] > smooth[b]; });
				for (const int t : tops) {
					const bool apartFromAll = std::all_of(levels.begin(), levels.end(), [&](int l) {
						const double dip = *std::min_element(smooth + std::min(t, l), smooth + std::max(t, l) + 1);
						return dip < LEVEL_GAP * std::min(smooth[t], smooth[l]);
					});
					if (apartFromAll)
						levels.push_back(t);
				}
				std::sort(levels.begin(), levels.end());
			}
			std::string spread;
			for (const int l : levels)
				spread += fmt::format("{}{:+.1f}", spread.empty() ? "" : " and ", (low + (l + 0.5) * (high - low) / BINS - reference) / 1000.0);

			const double limit = ROCK_LIMIT_M * EARTH_G / std::max(gravity, 0.01) * (ice ? ICE_LIMIT : 1.0);
			const double total = top - bottom;
			const double share = total / radius;
			const double radiusKm = radius / 1000.0;
			const bool round = radiusKm >= ROUNDING_KM;
			const bool flags[5] = {
				top - mean > LIMIT_SLACK * limit,
				mean - bottom > LIMIT_SLACK * limit,
				round ? total > (ice ? ICE_MOST : ROCK_MOST) * limit : share > (radiusKm < LUMPY_KM ? LUMPY_MOST : ROUNDING_MOST),
				round && !ice && total < ROCK_LEAST * limit,
				plates && hasSea && levels.size() < 2,
			};

			// what makes the low and high ground: each layer's mean over the lowest and highest tenth of the samples, less
			// its mean over all, and its value at the lowest and highest point, all in km. Layers are raw, so they needn't
			// add up to the height.
			auto layerValues = [](const Terrain::HeightLayers &l) {
				return LayerValues{ l.shape, l.largeCraters, l.smallCraters, l.smallRelief, l.lines, l.maps, l.volcano };
			};
			std::string layerText;
			if (hasLayers) {
				LayerValues all = {}, lowTail = {}, highTail = {};
				const int tail = std::max(1, int(TAIL * SAMPLES));
				for (int i = 0; i < SAMPLES; i++) {
					const LayerValues v = layerValues(layers[i]);
					for (int k = 0; k < LAYER_COUNT; k++)
						all[k] += v[k] / SAMPLES;
				}
				for (int i = 0; i < tail; i++) {
					const LayerValues low = layerValues(layers[order[SAMPLES - 1 - i]]), high = layerValues(layers[order[i]]);
					for (int k = 0; k < LAYER_COUNT; k++) {
						lowTail[k] += low[k] / tail;
						highTail[k] += high[k] / tail;
					}
				}
				Terrain::HeightLayers atLowest, atHighest;
				terrain->GetHeightLayers(lowest, atLowest);
				terrain->GetHeightLayers(highest, atHighest);
				const LayerValues lowestV = layerValues(atLowest), highestV = layerValues(atHighest);
				// km from the mean over all samples
				auto fromMean = [](const LayerValues &v, const LayerValues &base) {
					std::string out;
					for (int k = 0; k < LAYER_COUNT; k++)
						out += fmt::format("{}{:+.2f}", k ? "," : "", (v[k] - base[k]) / 1000.0);
					return out;
				};
				layerText = fmt::format("lowTail={};highTail={};atLowest={};atHighest={}", fromMean(lowTail, all), fromMean(highTail, all), fromMean(lowestV, all), fromMean(highestV, all));
			}
			std::string flagText;
			for (int f = 0; f < 5; f++)
				if (flags[f]) {
					flagText += std::string(FLAG_NAMES[f]) + " ";
					flagged[f]++;
				}
			if (!flagText.empty())
				flagged[5]++;

			std::string real;
			for (const Real &r : REAL)
				if (path.IsSameSystem(SystemPath(0, 0, 0, 0)) && body->GetName() == r.name) {
					real = fmt::format(" | real: highest {} km, lowest {} km, total {} km", km(r.highest), km(r.lowest), std::isnan(r.total) ? std::string("?") : fmt::format("{:.1f}", r.total));
					if (!std::isnan(r.total))
						real += fmt::format(" (ours {:.2f} times)", total / 1000.0 / r.total);
					real += fmt::format(", {}", r.note);
				}
			auto place = [](const vector3d &p) {
				return fmt::format("{:.1f}, {:.1f}", std::asin(std::clamp(p.y, -1.0, 1.0)) * 180.0 / PI, std::atan2(p.x, p.z) * 180.0 / PI);
			};
			bodies++;
			Log::Info("RELIEF CHECK {}body {} ({}, {}, {} #{}, {} terrain): radius {:.0f} km, g {:.2f}; from {}: highest {:+.2f} km at {}, lowest {:+.2f} km at {}; "
					  "total {:.1f} km = {:.2f} % of the radius; above the mean {:.2f} and below it {:.2f} times the strength limit ({:.1f} km); "
					  "heights at 1/10/50/90/99 %: {:+.1f} / {:+.1f} / {:+.1f} / {:+.1f} / {:+.1f} km; {} {} km{} | {}",
				flagText, body->GetName(), path.sectorX, path.sectorY, path.sectorZ, path.systemIndex, terrain->GetHeightFractalName(), radius / 1000.0, gravity,
				hasSea ? "sea level" : "the mean", (top - reference) / 1000.0, place(highest), (bottom - reference) / 1000.0, place(lowest),
				total / 1000.0, 100.0 * share, (top - mean) / limit, (mean - bottom) / limit, limit / 1000.0,
				(at(0.01) - reference) / 1000.0, (at(0.1) - reference) / 1000.0, (at(0.5) - reference) / 1000.0, (at(0.9) - reference) / 1000.0, (at(0.99) - reference) / 1000.0,
				levels.size() >= 2 ? "levels at" : "one level at", spread.empty() ? "?" : spread, real, geology);
			// the same for the report, one field per value; layers in the order shape, large craters, small craters,
			// small relief, lines, maps, volcano
			Log::Info("RELIEF ROW;sol={};name={};path={},{},{},{};terrain={};radiusKm={:.1f};g={:.3f};ice={};sea={};plates={};reference={};"
					  "highestKm={:.3f};lowestKm={:.3f};meanKm={:.3f};limitKm={:.3f};p1={:.3f};p10={:.3f};p50={:.3f};p90={:.3f};p99={:.3f};levels={};flags={};{};geology={}",
				path.IsSameSystem(SystemPath(0, 0, 0, 0)) ? 1 : 0, body->GetName(), path.sectorX, path.sectorY, path.sectorZ, path.systemIndex, terrain->GetHeightFractalName(),
				radiusKm, gravity, ice ? 1 : 0, hasSea ? 1 : 0, plates ? 1 : 0, hasSea ? "sea" : "mean",
				(top - reference) / 1000.0, (bottom - reference) / 1000.0, (mean - reference) / 1000.0, limit / 1000.0,
				(at(0.01) - reference) / 1000.0, (at(0.1) - reference) / 1000.0, (at(0.5) - reference) / 1000.0, (at(0.9) - reference) / 1000.0, (at(0.99) - reference) / 1000.0,
				int(levels.size()), flagText, layerText, fullGeology);
		};

		RefCountedPtr<Galaxy> galaxy = Pi::game->GetGalaxy();
		Log::Info("RELIEF CHECK: highest and lowest points and height spread of Sol's rocky bodies and up to {} random ones within {} sectors", BODIES, SECTORS);
		const SystemPath sol(0, 0, 0, 0);
		if (RefCountedPtr<StarSystem> system = galaxy->GetStarSystem(sol))
			for (const RefCountedPtr<SystemBody> &body : system->GetBodies())
				if (body->GetSuperType() == SystemBody::SUPERTYPE_ROCKY_PLANET)
					check(body.Get(), sol);
		const int solBodies = bodies;
		Random rng(20261109u);
		std::vector<SystemPath> seen = { sol };
		for (int attempt = 0; attempt < 5000 && bodies - solBodies < BODIES; attempt++) {
			const SystemPath sectorPath(int(rng.Int32(2 * SECTORS + 1)) - SECTORS, int(rng.Int32(2 * SECTORS + 1)) - SECTORS, int(rng.Int32(2 * SECTORS + 1)) - SECTORS);
			RefCountedPtr<const Sector> sector = galaxy->GetSector(sectorPath);
			if (!sector || sector->m_systems.empty())
				continue;
			const SystemPath path = sector->m_systems[rng.Int32(int(sector->m_systems.size()))].GetPath();
			if (std::find(seen.begin(), seen.end(), path) != seen.end())
				continue;
			seen.push_back(path);
			RefCountedPtr<StarSystem> system = galaxy->GetStarSystem(path);
			if (!system)
				continue;
			for (const RefCountedPtr<SystemBody> &body : system->GetBodies())
				if (bodies - solBodies < BODIES && body->GetSuperType() == SystemBody::SUPERTYPE_ROCKY_PLANET)
					check(body.Get(), path);
		}
		Log::Info("RELIEF CHECK done: {} bodies ({} of Sol), {} flagged: {} too tall, {} too deep, {} too much relief, {} too little relief, {} plate worlds with one level",
			bodies, solBodies, flagged[5], flagged[0], flagged[1], flagged[2], flagged[3], flagged[4]);
		FrontierLog::Note(fmt::format("relief check: {} bodies, {} flagged (output.txt)", bodies, flagged[5]));
	}
} // namespace

void PiGui::PerfInfo::DrawGeology()
{
	ImGui::Spacing();
	ImGui::SeparatorText("Geology");

	// bodies already made keep their generator, so this applies from the next start
	bool physical = Pi::config->Int("TerrainGenerator", 0) == 1;
	if (ImGui::Checkbox("Physical surface generator (applies after a restart)", &physical)) {
		Pi::config->SetInt("TerrainGenerator", physical ? 1 : 0);
		Pi::config->Save();
	}

	// the starports of 100 random bodies, as the game places them; builds their terrains, so the game stops for a while
	if (Pi::game && ImGui::Button("Check Starports on 100 Random Bodies (to output.txt)"))
		CheckStarports();
	// how rough the solid ground of 100 random bodies is up close; builds their terrains, so the game stops for a while
	if (Pi::game && ImGui::Button("Check Flatness on 100 Random Bodies (to output.txt)"))
		CheckFlatness();
	// highest and lowest points and height spread of Sol's rocky bodies and 200 random ones; the game stops for a while
	if (Pi::game && ImGui::Button("Check Relief of Sol and 200 Random Bodies (to output.txt)"))
		CheckRelief();

	if (!Pi::player || !Pi::game || Pi::player->GetFlightState() == Ship::HYPERSPACE)
		return;
	const SystemBody *body = Frame::GetFrame(Pi::player->GetFrame())->GetSystemBody();
	if (!body || body->GetSuperType() != SystemBody::SUPERTYPE_ROCKY_PLANET)
		return;

	// made once per body: a look at its data, not the one being drawn
	static const SystemBody *s_body = nullptr;
	static RefCountedPtr<Terrain> s_terrain;
	if (body != s_body) {
		s_body = body;
		s_terrain.Reset(Terrain::InstanceTerrain(body));
	}
	ImGui::Text("%s: height %s, colour %s", body->GetName().c_str(), s_terrain->GetHeightFractalName(), s_terrain->GetColorFractalName());
	for (const std::string &line : s_terrain->GetGeologyInfo())
		ImGui::TextUnformatted(line.c_str());
}

void PerfInfo::DrawImGuiStats()
{
	ImGui::SeparatorText("ImGui Stats");

	auto &io = ImGui::GetIO();
	ImGui::Text("%d verts, %d tris", io.MetricsRenderVertices, io.MetricsRenderIndices / 3);
	ImGui::Text("%d active windows (%d visible)", io.MetricsActiveWindows, io.MetricsRenderWindows);

	if (ImGui::Button("Toggle Metrics Window")) {
		m_state->metricsWindowOpen = !m_state->metricsWindowOpen;
	}

	if (ImGui::Button("Toggle Stack Tool")) {
		m_state->stackToolOpen = !m_state->stackToolOpen;
	}
}

void PerfInfo::DrawStatList(const Perf::Stats::FrameInfo &fi)
{
	ImGui::BeginChild("FrameInfo");
	ImGui::Columns(2);
	for (auto &t : fi) {
		ImGui::Text("%s:", t.first.c_str());
		ImGui::NextColumn();
		ImGui::Text("%'d", t.second);
		ImGui::NextColumn();
	}
	ImGui::Columns();
	ImGui::EndChild();
}

// ============================================================================
//
// Texture Cache Visualizer
//
// ============================================================================

bool DrawTexture(PerfInfo::ImGuiState *m_state, const Graphics::Texture *tex)
{
	ImGui::BeginGroup();
	auto pos0 = ImGui::GetCursorPos();

	const vector2f texUVs = tex->GetDescriptor().texSize;
	ImGui::Image(ImTextureID(tex), { 128, 128 }, { 0, 0 }, { texUVs.x, texUVs.y });

	auto pos1 = ImGui::GetCursorPos();
	ImGui::SetCursorPos(pos0);

	const vector3f &dataSize = tex->GetDescriptor().dataSize;
	ImGui::Text("%ux%u", uint32_t(dataSize.x), uint32_t(dataSize.y));
	ImGui::Text("%.1f KB", double(tex->GetTextureMemSize()) / 1024.0);

	ImGui::SetCursorPos(pos1);
	ImGui::EndGroup();
	return ImGui::IsItemClicked();
}

void PerfInfo::DrawTextureCache()
{
	if (ImGui::Begin("Texture Cache Viewer", &m_state->textureCacheViewerOpen, ImGuiWindowFlags_NoNav)) {
		for (auto &v : m_state->indirectionMap)
			// purge any textures which may have changed / evicted, but don't resize the vector
			// in the average case, this will simply re-fill the vector each frame, a fairly trivial operation.
			v.second.clear();

		for (const auto &t : Pi::renderer->GetTextureCache()) {
			const auto &key = t.first;
			if (t.second->Get()->GetDescriptor().type != Graphics::TEXTURE_2D)
				continue;

			m_state->indirectionMap[key.first].push_back(key.second);
		}

		if (ImGui::BeginTabBar("Texture List")) {
			const int item_width = 128 + ImGui::GetStyle().ItemSpacing.x;
			for (const auto &t : m_state->indirectionMap) {
				if (!ImGui::BeginTabItem(t.first.c_str()))
					continue;

				if (ImGui::BeginChild("##Texture List Scroll")) {
					const int window_width = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
					const int num_columns = std::max(int(window_width) / item_width, 1);
					ImGui::Columns(num_columns);
					if (num_columns > 1) {
						for (int idx = 0; idx < num_columns; idx++)
							ImGui::SetColumnWidth(idx, item_width);
					}

					for (const std::string &name : t.second) {
						const auto *ptr = Pi::renderer->GetCachedTexture(t.first, name);
						if (DrawTexture(m_state, ptr)) {
							m_state->selectedTexture = { t.first, name };
							m_state->hasSelectedTexture = true;
						}

						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", name.c_str());

						if (num_columns > 1)
							ImGui::NextColumn();
					}
				}
				ImGui::EndChild();

				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}

void PerfInfo::DrawTextureInspector()
{
	const auto &selectedTex = m_state->selectedTexture;
	const auto *tex = Pi::renderer->GetCachedTexture(selectedTex.first, selectedTex.second);
	if (tex != nullptr && m_state->hasSelectedTexture) {
		ImGui::SetNextWindowSize({ 300, 400 }, ImGuiCond_Appearing);
		std::string window_name = selectedTex.second + "###Texture Inspector";
		if (ImGui::Begin(window_name.c_str(), &m_state->hasSelectedTexture)) {
			const Graphics::TextureDescriptor &descriptor = tex->GetDescriptor();

			// If we have POT-extended textures, only display the part of the texture corresponding to the actual texture data.
			const vector2f texUVs = tex->GetDescriptor().texSize;
			ImGui::Image(ImTextureID(tex), { 256, 256 }, { 0, 0 }, { texUVs.x, texUVs.y });
			ImGui::Text("Dimensions: %ux%u", uint32_t(descriptor.dataSize.x), uint32_t(descriptor.dataSize.y));
			ImGui::Value("Mipmap Count", tex->GetDescriptor().numberOfMipMaps);
			ImGui::Value("VRAM Size", tex->GetTextureMemSize());
			if (texUVs.x < 1.0 || texUVs.y < 1.0)
				ImGui::Text("Original Size: %ux%u", int(texUVs.x * descriptor.dataSize.x), int(texUVs.y * descriptor.dataSize.y));

			ImGui::Spacing();
			ImGui::Value("Cache Bucket", selectedTex.first);
			ImGui::Value("Cache ID", selectedTex.second);
			ImGui::Value("TextureID", tex->GetTextureID());
			ImGui::Value("RefCount", tex->GetRefCount());
		}
		ImGui::End();
	}
}
