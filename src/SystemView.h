// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#pragma once

#ifndef _SYSTEMVIEW_H
#define _SYSTEMVIEW_H

#include "Color.h"
#include "DeleteEmitter.h"
#include "Frame.h"
#include "Input.h"
#include "Orbit.h"
#include "TransferPlanner.h"
#include "enum_table.h"
#include "graphics/Drawables.h"
#include "graphics/Graphics.h"
#include "matrix4x4.h"
#include "vector3.h"
#include "View.h"

#include <sigc++/signal.h>

#include <utility>

class GuiApplication;
class StarSystem;
class SystemBody;
class Orbit;
class Ship;
class HyperspaceCloud;
class Game;
class Body;

namespace Background {
	class Container;
}

namespace Graphics {
	class RenderTarget;
}

enum ShipDrawing {
	BOXES,
	ORBITS,
	OFF
};

enum CloudDrawing {
	CLOUD_ON,
	CLOUD_ARRIVAL,
	CLOUD_DEPARTURE,
	CLOUD_OFF
};

enum class GridDrawing {
	GRID,
	GRID_AND_LEGS,
	OFF
};

enum ShowLagrange {
	LAG_ICON,
	LAG_ICONTEXT,
	LAG_OFF
};

struct ProjectedOrbit {
	Orbit orbit;
	Color color;
	double planetRadius;
};

/**
 * Projectable is used as an opaque data container to represent an
 * "object track" that the system map operates on. External systems push
 * a projectable to represent a sensor contact or object in worldspace that
 * should be visible to the player, and the map is responsible for transforming
 * and culling them.
 *
 * OBJECT-type projectables refer to a concrete body or sensor contact, and can
 * potentially be centered, focused, and selected by the user.
 *
 * All other projectable types represent visual-only elements which represent
 * the intent to draw icons or orbit lines to the system map.
 *
 * User code is permitted to extend the Projectable::types enum by passing
 * integer values beyond types::_MAX to implement custom projectable types if
 * needed. This should only be done with a wholly-owned SystemMapViewport
 * however (i.e. don't use it with SystemView).
 */
struct Projectable {
	enum types {	// <enum name=ProjectableTypes scope='Projectable' public>
		NONE = 0,	// empty projectable, don't try to get members
		OBJECT = 1, // clickable space object, may be without phys.body (other starsystem)
		L4 = 2,
		L5 = 3,
		APOAPSIS = 4,
		PERIAPSIS = 5,
		ASCENDING_NODE = 6,	 // where an orbit crosses the planner's reference plane northwards
		DESCENDING_NODE = 7, // ... and southwards
		IMPACT = 8,			 // where the player's or the planner's trajectory meets a body's surface
		ORBIT = 9, // <enum skip>
		_MAX = 10 // <enum skip>
	} type;
	enum bases {		// <enum name=ProjectableBases scope='Projectable' public>
		SYSTEMBODY = 0, // ref class SystemBody, may not have a physical body
		BODY = 1,		// generic body
		SHIP = 2,
		PLAYER = 3, // player's ship
		PLANNER = 4, // player's ship planned by transfer planner, refers to player's object
		PLANNER_BURN = 5 // planned burn point, refers to the player's object
	} base;
	union {
		const Body *body;
		const SystemBody *sbody;
	} ref;
	vector3f screenpos;		// x,y - screen coordinate, z - in NDC
	float screensize = 0.f; // approximate size in screen pixels
	vector3d worldpos;
	int orbitIdx = -1;
	// apsis and node markers of the player's and the planner's orbits: the
	// marker's height above the surface of the orbited body
	double altitude = 0.0;
	// impact markers: the game time the trajectory meets the surface
	double time = 0.0;

	Projectable(const types t, const bases b, const Body *obj, const vector3d &pos = vector3d()) :
		type(t), base(b), worldpos(pos)
	{
		ref.body = obj;
	}
	Projectable(const types t, const bases b, const SystemBody *obj, const vector3d &pos = vector3d()) :
		type(t), base(b), worldpos(pos)
	{
		ref.sbody = obj;
	}
	Projectable() :
		type(NONE), worldpos() {}

	void *getRef() const { return base == SYSTEMBODY ? (void *)ref.sbody : (void *)ref.body; }

	bool operator==(const Projectable &rhs) const
	{
		return (type == rhs.type && base == rhs.base && getRef() == rhs.getRef());
	}

	struct GroupInfo {
		GroupInfo(int trackIdx, vector3f screenpos, types type = OBJECT) :
			type(type),
			screenpos(screenpos),
			specials(0),
			tracks({ trackIdx })
		{
		}

		types type;
		vector3f screenpos;
		uint32_t specials;
		std::vector<int> tracks;

		void setSpecial(int index) { specials |= (1 << (index & 0x1F)); }
		bool hasSpecial(int index) const { return specials & (1 << (index & 0x1F)); }
	};
};

struct AtlasBodyLayout {
	SystemBody *body;
	float radius;
	bool isVertical;
	bool isBinary;
	vector2f offset;
	vector2f size;

	std::vector<AtlasBodyLayout> children;
};

class SystemMapViewport;

/**
 * SystemView glues a SystemMapViewport to the View framework and handles
 * most user interaction in the context of a running game.
 *
 * It is responsible for pushing ship contacts and managing the orbit planner
 * interface.
 */
class SystemView : public View, public DeleteEmitter {
public:
	enum class Mode { // <enum name=SystemViewMode scope='SystemView::Mode' public>
		Orrery = 0,
		Atlas = 1
	};

	enum class SystemSelectionMode { // <enum name=SystemSelectionMode scope='SystemView::SystemSelectionMode' public>
		CURRENT_SYSTEM = 0,
		SELECTED_SYSTEM = 1,
	};

	enum class HudMapFocus {
		PLAYER = 0,
		FRAME_BODY = 1,
	};

	SystemView(Game *game);
	~SystemView() override;
	void Update() override;
	void Draw3D() override;
	void OnSwitchFrom() override;

	Mode GetDisplayMode() { return m_displayMode; }
	void SetDisplayMode(Mode displayMode) { m_displayMode = displayMode; }

	SystemSelectionMode GetSystemSelectionMode() { return m_systemSelectionMode; }
	void SetSystemSelectionMode(SystemSelectionMode systemMode);

	TransferPlanner *GetTransferPlanner() const { return m_planner; }
	double GetOrbitPlannerStartTime() const { return m_planner->GetStartTime(); }

	SystemMapViewport *GetMap() { return m_map.get(); }
	SystemMapViewport *GetHudMap() { return m_hudMap.get(); }

	void SetHudMapSize(vector2d size);
	void RenderHudMap();
	void DrawHudMapPiGui();
	HudMapFocus GetHudMapFocus() const { return m_hudMapFocus; }
	void SetHudMapFocus(HudMapFocus focus);
	// centre the main map on the player's ship, zoomed to frame its orbit
	void ViewPlayer();
	// keep the main map centred on where the plan puts the ship at map time
	// (the planner's ghost marker), and whether it is
	void ViewPlannedShip();
	bool IsViewingPlannedShip();
	// the map shows the system the player is in, whichever selection mode
	// got it there, so the player's ship is on it
	bool IsViewingCurrentSystem() const { return m_viewingCurrentSystem; }

private:
	void RefreshShips(void);
	void AddShipTracks(double atTime);

	void RefreshClouds(void);
	void AddCloudTracks(double atTime);

	void PopulatePlayerAndPlannerTracks(SystemMapViewport *map, double atTime);
	void UpdateHudMap(float deltaTime);
	void CreateHudRenderTarget();

	void CalculateShipPositionAtTime(const Ship *s, const Orbit &o, double t, vector3d &pos);
	void CalculateFramePositionAtTime(FrameId frameId, double t, vector3d &pos);

	double CalculateStarportHeight(const SystemBody *body);

private:
	Game *m_game;

	std::unique_ptr<SystemMapViewport> m_map;
	std::unique_ptr<SystemMapViewport> m_hudMap;
	std::unique_ptr<Graphics::RenderTarget> m_hudRenderTarget;
	std::unique_ptr<Graphics::RenderTarget> m_hudResolveTarget;
	vector2f m_hudMapSize = vector2f(0.f, 0.f);
	bool m_hudMapNeedsResize = false;
	HudMapFocus m_hudMapFocus = HudMapFocus::FRAME_BODY;
	bool m_hudMapRotating = false;
	bool m_hudMapInitialZoomSet = false;

	TransferPlanner *m_planner;
	std::list<std::pair<Ship *, Orbit>> m_contacts;
	std::list<HyperspaceCloud *> m_clouds;

	Mode m_displayMode;
	SystemSelectionMode m_systemSelectionMode;
	bool m_viewingCurrentSystem;
	bool m_unexplored;
};

/**
 * The SystemMapViewport is a context-agnostic map interface concerned with
 * rendering the layout of the selected system and optionally positioning and
 * processing Projectables pushed to it by external code.
 *
 * It is intentionally "blind" to all Game-related functionality so it can be
 * used in both a gameplay and tools-related context regardless of whether the
 * Pi framework is currently running.
 *
 * The typical flow for the SystemMap is:
 *
 * - Call Update(deltaTime) to handle all per-frame setup and generate
 *   projectables for all SystemBodies in the current system.
 *
 * - Call AddObjectTrack() / AddOrbitTrack() to push any external contacts
 *   that should be displayed in the map at this time.
 *
 * - Call Draw3D() to process all tracks and project bodies to the viewport.
 *
 * - Call GroupProjectables() to receive a list of screen-space Projectables
 *   which the owning application can use to render icons etc. This is an
 *   expensive operation and should not be called spuriously.
 *
 */
class SystemMapViewport {
public:
	SystemMapViewport(GuiApplication *app);
	~SystemMapViewport();

	void Update(float deltaTime);
	void HandleInput(float deltaTime);
	void Draw3D();

	Projectable *GetSelectedObject() { return &m_selectedObject; }
	void SetSelectedObject(const Projectable &p) { m_selectedObject = p; }
	void ClearSelectedObject();

	void ViewSelectedObject() { SetViewedObject(m_selectedObject); }
	void SetViewedObject(const Projectable &p);
	void ResetViewpoint();

	Projectable *GetViewedObject() { return &m_viewedObject; }

	ShowLagrange GetShowLagrange() { return m_showL4L5; }
	ShipDrawing GetShipDrawing() { return m_shipDrawing; }
	CloudDrawing GetCloudDrawing() { return m_cloudDrawing; }
	GridDrawing GetGridDrawing() { return m_gridDrawing; }

	// Generate a sorted list of non-overlapping projectable icon groups
	// Up to 32 objects can be treated as "special" and will be noted in the
	// GroupInfo bitvector of any group they appear in
	std::vector<Projectable::GroupInfo> GroupProjectables(vector2f groupThreshold, const std::vector<Projectable> &specialObjects);

	// Push a tracked object / sensor contact for display on the map.
	// Object tracks are cleared every frame.
	void AddObjectTrack(const Projectable &p);

	// Push an orbital conic for display on the map
	// Expects the orbit's center to be provide as p.worldpos
	// Orbit tracks are cleared every frame.
	void AddOrbitTrack(Projectable p, const Orbit *orbit, Color color, double planetRadius);

	// Submit a leg of the planned trajectory as a dynamic vertex list (world
	// space), drawn as a line strip in the next orrery pass. The legs are
	// cleared every frame.
	void AddPlannerLeg(std::vector<vector3d> &&verts, Color color)
	{
		m_plannerLegs.push_back({ std::move(verts), color });
	}

	RefCountedPtr<StarSystem> GetCurrentSystem();
	void SetCurrentSystem(RefCountedPtr<StarSystem> system);

	void AccelerateTime(float step);
	bool IsRealTime() const { return m_realtime; }
	void SetRealTime();
	// jump the map to time t and hold it there
	void SetTime(double t);
	void SetReferenceTime(double time) { m_refTime = time; }
	double GetTime() { return m_time; }
	const std::vector<Projectable> &GetProjected() const { return m_projected; }
	void SetVisibility(const std::string &param);
	void SetZoomMode(bool enable);
	void SetRotateMode(bool enable);
	void SetDisplayMode(SystemView::Mode displayMode) { m_displayMode = displayMode; }
	void SetBackground(Background::Container *bg) { m_background = bg; }
	void SetShowGravpoints(bool enabled) { m_showGravpoints = enabled; }

	double ProjectedSize(double size, vector3d pos);
	float AtlasViewPlanetGap(float planetRadius) { return std::max(planetRadius * 0.6, 1.33); }
	float AtlasViewPixelPerUnit();

	float GetZoom() const;
	void RotateCameraByMouse(float dx, float dy);
	void ZoomCameraByWheel(float wheel, float speedMod = 1.0f);
	void SetZoomDirect(float zoom);
	// zoom to the given level with the usual zoom animation
	void SetZoomTarget(float zoom);
	void SnapTransitionToViewedObject();

	// all used colors. defined in system-view-ui.lua
	enum ColorIndex { // <enum name=SystemViewColorIndex scope='SystemMapViewport' public>
		GRID = 0,
		GRID_LEG = 1,
		SYSTEMBODY = 2,
		SYSTEMBODY_ORBIT = 3,
		PLAYER_ORBIT = 4,
		PLANNER_ORBIT = 5,
		SELECTED_SHIP_ORBIT = 6,
		SHIP_ORBIT = 7,
		PLANNER_BURN = 8
	};

	Color svColor[9];
	void SetColor(ColorIndex color_index, Color *color_value) { svColor[color_index] = *color_value; }

	sigc::slot<double(const SystemBody *)> GetStarportHeightAboveTerrain;

private:
	struct InputBindings : public Input::InputFrame {
		using InputFrame::InputFrame;
		void RegisterBindings() override;

		Axis *mapViewPitch;
		Axis *mapViewYaw;
		Axis *mapViewZoom;
	} m_input;

	void DrawOrreryView();
	void DrawAtlasView();

	// Push the system body tree as object tracks
	void AddBodyTrack(const SystemBody *b, const vector3d &offset = vector3d());

	void LayoutSystemBody(SystemBody *body, AtlasBodyLayout &layout);
	void RenderAtlasBody(const AtlasBodyLayout &layout, vector3f pos, const matrix4x4f &cameraTrans);

	// Project a track to screenspace with the current renderer state and add it to the list of projected objects
	void AddProjected(Projectable p, Projectable::types type, const vector3d &transformedPos, float screensize = 0.f);
	void RenderBody(const SystemBody *b, const vector3d &pos, const matrix4x4f &trans);
	// queue a planet's or moon's latitude and longitude lines, in camera space
	void AddLatLongLines(const SystemBody *b, const vector3f &centre, float radius, const matrix4x4f &trans);
	void RenderOrbit(const Projectable &p, const ProjectedOrbit *orbitData, const vector3d &transformedPos);

	// draw a grid with `radius` * 2 gridlines on an evenly spaced 1-AU grid
	void DrawGrid(uint32_t radius);

private:
	GuiApplication *m_app;
	Graphics::Renderer *m_renderer;
	Background::Container *m_background;

	RefCountedPtr<StarSystem> m_system;
	Projectable m_selectedObject;
	Projectable m_viewedObject;

	std::vector<Projectable> m_objectTracks;
	std::vector<ProjectedOrbit> m_orbitTracks;

	struct PlannerLeg {
		std::vector<vector3d> verts;
		Color color;
	};
	std::vector<PlannerLeg> m_plannerLegs;

	std::vector<Projectable> m_projected;

	SystemView::Mode m_displayMode; // FIXME: separate Atlas from SystemMapViewport
	Graphics::ViewportExtents m_viewportSize;

	AtlasBodyLayout m_atlasLayout = {};
	float m_atlasZoom, m_atlasZoomTo, m_atlasZoomDefault;
	vector2f m_atlasPos, m_atlasPosTo, m_atlasPosDefault;
	float m_atlasViewW, m_atlasViewH;

	bool m_showGravpoints;
	ShowLagrange m_showL4L5;
	ShipDrawing m_shipDrawing;
	CloudDrawing m_cloudDrawing;
	GridDrawing m_gridDrawing;
	// latitude and longitude lines on planets and moons, every this many
	// degrees; 0 when off
	int m_latLongStep = 0;

	bool m_rotateWithMouseButton = false;
	bool m_rotateView = false;
	bool m_zoomView = false;
	const float CAMERA_FOV = 50.f;
	const float CAMERA_FOV_RADIANS = CAMERA_FOV / 57.295779f;
	matrix4x4f m_cameraSpace;

	float m_rot_x, m_rot_y;
	float m_rot_x_to, m_rot_y_to;
	float m_zoom, m_zoomTo;
	int m_animateTransition;
	vector3d m_trans;
	vector3d m_transTo;
	double m_time;
	double m_refTime;
	bool m_realtime;
	double m_timeStep;

	std::unique_ptr<Graphics::Drawables::Disk> m_bodyIcon;
	std::unique_ptr<Graphics::Material> m_bodyMat;
	std::unique_ptr<Graphics::Material> m_atlasMat;
	std::unique_ptr<Graphics::Material> m_lineMat;
	std::unique_ptr<Graphics::Material> m_gridMat;
	Graphics::Drawables::Lines m_orbits;

	std::unique_ptr<vector3f[]> m_orbitVts;
	std::unique_ptr<Color[]> m_orbitColors;

	std::unique_ptr<Graphics::VertexArray> m_lineVerts;
	Graphics::Drawables::Lines m_lines;

	// this frame's latitude and longitude lines, in camera space
	std::vector<vector3f> m_latLongVerts;
	std::vector<Color> m_latLongColors;
	Graphics::Drawables::Lines m_latLongLines;
};

#endif /* _SYSTEMVIEW_H */
