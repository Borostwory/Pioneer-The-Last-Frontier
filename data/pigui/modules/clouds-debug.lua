-- Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- The Last Frontier: the volumetric cloud settings (src/PlanetClouds.cpp), in the debug window's The Last Frontier
-- tab. The settings window keeps only the on/off switch. Everything here applies at once.

local Engine = require 'Engine'
local Lang = require 'Lang'
local ui = require 'pigui'
local debugView = require 'pigui.views.debug'

local lui = Lang.GetResource("ui-core")

local colors = ui.theme.colors

local function combo(label, selected, items, tooltip)
	local color = colors.buttonBlue
	local changed, ret = 0, nil
	ui.withStyleColors({Button=color,ButtonHovered=color:tint(0.1),ButtonActive=color:tint(0.2)},function()
		changed, ret = ui.combo(label, selected, items)
	end)
	if ui.isItemHovered() and tooltip then
		Engine.pigui.SetTooltip(tooltip)
	end
	return changed, ret
end

local function checkbox(label, checked, tooltip)
	local changed, ret = ui.checkbox(label, checked)
	if ui.isItemHovered() and tooltip then
		Engine.pigui.SetTooltip(tooltip)
	end
	return changed, ret
end

local function slider(lbl, value, min, max, tooltip)
	local ret,c = ui.sliderInt(lbl, value, min, max)
	if ui.isItemHovered() and tooltip then
		Engine.pigui.SetTooltip(tooltip)
	end
	return c, ret
end

local function drawCloudSettings()
	local c

	if Engine.SettingsGetInt("CloudsEnabled") == 0 then
		ui.text("Volumetric clouds are off. Turn them on in Settings, The Last Frontier.")
		return
	end

	local cloudResolutionLabels = { lui.CLOUD_RESOLUTION_LOW, lui.CLOUD_RESOLUTION_MEDIUM, lui.CLOUD_RESOLUTION_HIGH, lui.CLOUD_RESOLUTION_VERY_HIGH, lui.CLOUD_RESOLUTION_ULTRA, lui.CLOUD_RESOLUTION_FULL }
	local cloudResolution = Engine.SettingsGetInt("CloudResolution")
	c,cloudResolution = combo(lui.CLOUD_RESOLUTION, cloudResolution, cloudResolutionLabels,
		lui.CLOUD_RESOLUTION_DESC)
	if c then
		Engine.SettingsSetInt("CloudResolution", cloudResolution)
	end

	local cloudResolutionSpace = Engine.SettingsGetInt("CloudResolutionSpace")
	c,cloudResolutionSpace = combo(lui.CLOUD_RESOLUTION_SPACE, cloudResolutionSpace, cloudResolutionLabels,
		lui.CLOUD_RESOLUTION_SPACE_DESC)
	if c then
		Engine.SettingsSetInt("CloudResolutionSpace", cloudResolutionSpace)
	end

	local cloudSpaceDetail = Engine.SettingsGetInt("CloudSpaceDetail")
	c,cloudSpaceDetail = slider(lui.CLOUD_SPACE_DETAIL, cloudSpaceDetail, 0, 200,
		lui.CLOUD_SPACE_DETAIL_DESC)
	if c then
		Engine.SettingsSetInt("CloudSpaceDetail", cloudSpaceDetail)
	end

	local cloudQualityLabels = { lui.CLOUD_QUALITY_LOW, lui.CLOUD_QUALITY_MEDIUM, lui.CLOUD_QUALITY_HIGH, lui.CLOUD_QUALITY_VERY_HIGH }
	local cloudQuality = Engine.SettingsGetInt("CloudQuality")
	c,cloudQuality = combo(lui.CLOUD_QUALITY, cloudQuality, cloudQualityLabels,
		lui.CLOUD_QUALITY_DESC)
	if c then
		Engine.SettingsSetInt("CloudQuality", cloudQuality)
	end

	local cloudTemporalLabels = { lui.OFF, lui.CLOUD_TEMPORAL_4X, lui.CLOUD_TEMPORAL_9X, lui.CLOUD_TEMPORAL_16X }
	local cloudTemporal = Engine.SettingsGetInt("CloudTemporal")
	c,cloudTemporal = combo(lui.CLOUD_TEMPORAL, cloudTemporal, cloudTemporalLabels,
		lui.CLOUD_TEMPORAL_DESC)
	if c then
		Engine.SettingsSetInt("CloudTemporal", cloudTemporal)
	end

	local cloudSoftnessLabels = { lui.CLOUD_SOFTENING_SHARP, lui.CLOUD_SOFTENING_SOFT, lui.CLOUD_SOFTENING_VERY_SOFT }
	local cloudSoftness = Engine.SettingsGetInt("CloudSoftness")
	c,cloudSoftness = combo(lui.CLOUD_SOFTENING, cloudSoftness, cloudSoftnessLabels,
		lui.CLOUD_SOFTENING_DESC)
	if c then
		Engine.SettingsSetInt("CloudSoftness", cloudSoftness)
	end

	local cloudWeatherMaps = Engine.SettingsGetInt("CloudWeatherMaps") ~= 0
	c,cloudWeatherMaps = checkbox(lui.CLOUD_WEATHER_MAPS, cloudWeatherMaps,
		lui.CLOUD_WEATHER_MAPS_DESC)
	if c then
		Engine.SettingsSetInt("CloudWeatherMaps", cloudWeatherMaps and 1 or 0)
	end

	local cloudShadowLabels = { lui.OFF, lui.CLOUD_SHADOWS_SOFT, lui.CLOUD_SHADOWS_DETAILED }
	local cloudShadows = Engine.SettingsGetInt("CloudShadows")
	c,cloudShadows = combo(lui.CLOUD_SHADOWS, cloudShadows, cloudShadowLabels,
		lui.CLOUD_SHADOWS_DESC)
	if c then
		Engine.SettingsSetInt("CloudShadows", cloudShadows)
	end

	local cloudSpeed = Engine.SettingsGetInt("CloudSpeed")
	c,cloudSpeed = slider(lui.CLOUD_SPEED, cloudSpeed, 1, 100,
		lui.CLOUD_SPEED_DESC)
	if c then
		Engine.SettingsSetInt("CloudSpeed", cloudSpeed)
	end

	-- per-layer tuning, in percent of the physically derived value
	local cloudLayers = {
		{ key = "High", label = lui.CLOUD_LAYER_HIGH },
		{ key = "Medium", label = lui.CLOUD_LAYER_MEDIUM },
		{ key = "Low", label = lui.CLOUD_LAYER_LOW },
	}
	local function layerSliders(header, prefix, layerLabel, min, max, tooltip)
		ui.text(header)
		for _, layer in ipairs(cloudLayers) do
			local key = prefix .. layer.key
			local value = Engine.SettingsGetInt(key)
			c,value = slider(string.interp(layerLabel, { layer = layer.label }), value, min, max, tooltip)
			if c then
				Engine.SettingsSetInt(key, value)
			end
		end
	end
	layerSliders(lui.CLOUD_COVER_HEADER, "CloudDensity", lui.CLOUD_COVER_LAYER, 0, 300, lui.CLOUD_COVER_DESC)
	layerSliders(lui.CLOUD_SIZE_HEADER, "CloudSize", lui.CLOUD_SIZE_LAYER, 25, 1600, lui.CLOUD_SIZE_DESC)
	layerSliders(lui.CLOUD_SYSTEM_SIZE_HEADER, "CloudSystemSize", lui.CLOUD_SYSTEM_SIZE_LAYER, 25, 400, lui.CLOUD_SYSTEM_SIZE_DESC)
	layerSliders(lui.CLOUD_CLUSTERING_HEADER, "CloudClustering", lui.CLOUD_CLUSTERING_LAYER, 0, 100, lui.CLOUD_CLUSTERING_DESC)
	layerSliders(lui.CLOUD_TRANSPARENCY_HEADER, "CloudTransparency", lui.CLOUD_TRANSPARENCY_LAYER, 25, 1000, lui.CLOUD_TRANSPARENCY_DESC)
end

debugView.registerTab('frontier-clouds', {
	icon = ui.theme.icons.gas_giant,
	label = "Volumetric Clouds",
	mod = true,
	draw = drawCloudSettings,
})
