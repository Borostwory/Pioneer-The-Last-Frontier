// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "GameConfig.h"
#include "FileSystem.h"
#include "core/OS.h"

GameConfig::GameConfig(const map_string &override_)
{
	// set defaults
	std::map<std::string, std::string> &map = m_map[""];
	map["Lang"] = OS::GetUserLangCode();
	map["AMD_MESA_HACKS"] = "0";
	map["DisableSound"] = "0";
	map["StartFullscreen"] = "0";
	map["ScrWidth"] = "1280";
	map["ScrHeight"] = "720";
	map["UIScaleFactor"] = "1";
	map["DetailCities"] = "1";
	map["DetailPlanets"] = "1";
	map["SfxVolume"] = "0.8";
	map["EnableJoystick"] = "1";
	map["InvertMouseY"] = "0";
	map["FOVVertical"] = "65";
	map["DisplayNavTunnel"] = "0";
	map["CompactRadar"] = "1";
	map["ConfirmQuit"] = "1";
	map["AudioBackend"] = "";
	map["MasterVolume"] = "0.8";
	map["MusicVolume"] = "0.8";
	map["MasterMuted"] = "0";
	map["SfxMuted"] = "0";
	map["MusicMuted"] = "0";
	map["BinauralRendering"] = "0";
	map["SectorViewXRotation"] = "-10.0";
	map["SectorViewZRotation"] = "0";
	map["SectorViewZoom"] = "2.0";
	map["MaxPhysicsCyclesPerRender"] = "4";
	map["AntiAliasingMode"] = "2";
	map["JoystickDeadzone"] = "0.2"; // 20% deadzone is common
	map["PlanetLodDistance"] = "8"; // terrain patches split within this many patch widths of the camera
	map["PlanetLodForeshortening"] = "100"; // percent: how much later terrain patches seen edge-on split
	map["TerrainDetailTiles"] = "1"; // bake GPU detail tiles for nearby terrain patches
	map["TerrainErosion"] = "1"; // cut river valleys into the mountains of planets with liquid water (TerrainErosion.cpp)
	map["ShadowQuality"] = "2";   // sun shadows of ships, stations, buildings and terrain: 0 off, 1 1024, 2 2048, 3 4096 maps (SceneShadows.cpp)
	map["ShadowNearRadius"] = "120"; // metres around the player's ship with sharp shadows, redrawn every frame
	map["ShadowDistance"] = "2000";  // metres around the camera with shadows of terrain, stations, cities and parked ships
	map["ShadowStrength"] = "75";    // percent of the sunlight a shadow removes under an Earth-like sky (always 100 in vacuum)
	map["ShadowDebug"] = "0";        // 1: tint by shadow map, near red, mid green, far blue
	map["CityLights"] = "100";       // percent: brightness of the cities' street lamps and building lights at night, 0 off (CityOnPlanet.h)
	map["CityGlow"] = "100";         // percent: brightness of the starport cities' lights seen from afar at night, 0 off (city-glow.glsl)
	map["CloudsEnabled"] = "1";
	map["CloudResolution"] = "1"; // 0 low .. 5 full, see PlanetClouds.cpp
	map["CloudTemporal"] = "2";   // temporal reconstruction: 0 off, 1 4x (2x2 blocks), 2 9x (3x3), 3 16x (4x4)
	map["CloudSoftness"] = "1";   // 0 sharp .. 2 soft
	map["CloudShadows"] = "2";    // terrain shadows: 0 off, 1 soft, 2 detailed
	map["CloudResolutionSpace"] = "5"; // resolution when far above the clouds (5 = full)
	map["CloudQuality"] = "2";    // raymarch steps: 0 16, 1 24, 2 32, 3 48
	map["CloudWeatherMaps"] = "1"; // bake the large-scale weather pattern instead of computing it per pixel
	map["CloudSpaceDetail"] = "50"; // percent: how soon clouds sharpen toward the space resolution with distance
	map["CloudSpeed"] = "10";     // cloud motion relative to the real wind speed
	map["CloudDensityLow"] = "100";    // percent, per cloud layer
	map["CloudDensityMedium"] = "100";
	map["CloudDensityHigh"] = "100";
	map["CloudSizeLow"] = "100";       // percent, per cloud layer
	map["CloudSizeMedium"] = "100";
	map["CloudSizeHigh"] = "100";
	map["CloudTransparencyLow"] = "100"; // percent, per cloud layer
	map["CloudTransparencyMedium"] = "100";
	map["CloudTransparencyHigh"] = "100";
	map["CloudSystemSizeLow"] = "100"; // percent, per cloud layer
	map["CloudSystemSizeMedium"] = "100";
	map["CloudSystemSizeHigh"] = "100";
	map["CloudClusteringLow"] = "75";  // percent: 0 scattered .. 100 solid decks
	map["CloudClusteringMedium"] = "50";
	map["CloudClusteringHigh"] = "50";
	map["DefaultLowThrustPower"] = "0.25";
	map["VSync"] = "1";
	map["UseTextureCompression"] = "1";
	map["WorkerThreads"] = "0";
	map["SpeedLines"] = "0";
	map["EnableCockpit"] = "0";
	map["HudTrails"] = "0";
	map["EnableServerAgent"] = "0";
	map["AmountOfBackgroundStars"] = "0.25";
	map["StarFieldStarSizeFactor"] = "0.7";
	map["UseAnisotropicFiltering"] = "0";
	map["RendererName"] = "Opengl 3.x"; // default to our best renderer
	map["EnableGLDebug"] = "0";
	map["EnableGPUJobs"] = "1";
	map["GL3ForwardCompatible"] = "1";
	map["LogVerbose"] = "1";
	map["ProfileSlowFrames"] = "0";
	map["ProfilerZoneOutput"] = "0";
	map["CameraSmoothing"] = "0";
	map["AimingSensitivity"] = "1.0";

	Read(FileSystem::userFiles, "config.ini");

	for (auto i = override_.begin(); i != override_.end(); ++i) {
		const std::string &key = (*i).first;
		const std::string &val = (*i).second;
		map[key] = val;
	}
}
