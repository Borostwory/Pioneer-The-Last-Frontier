// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _SCENESHADOWS_H
#define _SCENESHADOWS_H

#include "Aabb.h"
#include "FrameId.h"
#include "matrix3x3.h"
#include "matrix4x4.h"
#include "vector3.h"

#include <memory>
#include <vector>

class Body;
class CameraContext;
class ModelBody;
class TerrainBody;

namespace Graphics {
	class Light;
	class Renderer;
	class RenderTarget;
} // namespace Graphics

/*
 * Sun shadows of ships, stations, city buildings and terrain, received by
 * everything drawn with the lit model and terrain shaders (shadows.glsl).
 *
 * The main star's light is captured in three depth maps (cascades):
 * - near: around the player's ship (or ahead of the camera), ShadowNearRadius
 *   metres, with everything in it; redrawn every frame
 * - mid and far: around the same point, up to ShadowDistance metres, with the
 *   terrain, stations, their cities and parked ships; drawn once and reused
 *   until the sun or the camera has moved enough, so they cost almost
 *   nothing most frames. Moving ships only cast in the near map.
 *
 * A cascade's light space is the camera's frame rotated so that +z points at
 * the sun, with its origin where the camera was when the map was drawn.
 */
class SceneShadows {
public:
	SceneShadows(Graphics::Renderer *r);
	~SceneShadows();

	// Draws the shadow maps and hands them to the renderer for the scene's lit
	// shaders. Call after the scene's lights are set, before drawing the bodies.
	void Begin(const CameraContext *camera, const Graphics::Light &sun);
	// Stops shadowing; call before anything is drawn from another point of view.
	void End();

private:
	static constexpr int NUM_CASCADES = 3;

	struct Cascade {
		std::unique_ptr<Graphics::RenderTarget> target;
		int size = 0;
		FrameId frame;
		matrix3x3d frameToLight; // rotation from the frame into light space
		vector3d anchor;		 // light-space origin in the frame
		vector3d focus;			 // what the map is centred on, in the frame
		vector3d sunDir;		 // direction to the sun in the frame, when drawn
		Aabb box;				 // light-space volume covered by the map
		double radius = 0.0;
		double drawnTime = 0.0;
		size_t numCasters = 0;
		bool valid = false;
	};

	struct Caster {
		ModelBody *body;
		matrix4x4d frameToView; // body's frame -> view space
		vector3d viewPos;
		double radius;
		bool isStatic; // doesn't move relative to its frame for a while
	};

	struct TerrainCaster {
		TerrainBody *body;
		matrix4x4d frameToView;
	};

	void EnsureTarget(Cascade &c, int size);
	size_t CountCasters(const matrix4x4d &viewToLight, const Aabb &box, bool staticOnly) const;
	void DrawCascade(Cascade &c, const matrix4x4d &viewToLight, bool staticOnly);

	Graphics::Renderer *m_renderer;
	Cascade m_cascades[NUM_CASCADES]; // near, mid, far
	std::vector<Caster> m_casters;
	std::vector<TerrainCaster> m_terrain;
	bool m_active = false;
};

#endif /* _SCENESHADOWS_H */
