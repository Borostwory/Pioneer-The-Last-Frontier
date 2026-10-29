// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "SceneShadows.h"

#include "Camera.h"
#include "Frame.h"
#include "Game.h"
#include "GameConfig.h"
#include "MathUtil.h"
#include "ModelBody.h"
#include "Pi.h"
#include "Planet.h"
#include "Player.h"
#include "Ship.h"
#include "Space.h"
#include "TerrainBody.h"
#include "galaxy/SystemBody.h"
#include "gameconsts.h"
#include "graphics/Light.h"
#include "graphics/RenderTarget.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"

#include "profiler/Profiler.h"

#include <algorithm>
#include <cmath>

namespace {
	// map resolution per ShadowQuality setting (0 = off)
	const int MAP_SIZES[] = { 0, 1024, 2048, 4096 };

	// how far each map reaches toward the sun past the covered area, so that
	// tall casters and hills outside it still shadow it (m)
	const double DEPTH_TOWARD_SUN[] = { 2000.0, 3000.0, 4000.0 };

	// the cached maps are redrawn once the sun has moved this far across the sky,
	const double CACHE_REDRAW_SUN_COS = std::cos(DEG2RAD(0.1));
	// their centre has moved this share of the map's radius,
	constexpr double CACHE_REDRAW_MOVE = 0.2;
	// or this much game time has passed (station doors and the like)
	constexpr double CACHE_REDRAW_SECONDS = 5.0;

	// parked ships and stations stay put long enough for the cached maps
	bool IsStaticCaster(const Body *b)
	{
		if (b->IsType(ObjectType::SPACESTATION))
			return true;
		if (b->IsType(ObjectType::SHIP)) {
			const Ship::FlightState state = static_cast<const Ship *>(b)->GetFlightState();
			return state == Ship::DOCKED || state == Ship::LANDED;
		}
		return false;
	}

	bool SphereInBox(const vector3d &p, double r, const Aabb &box)
	{
		return p.x + r >= box.min.x && p.x - r <= box.max.x &&
			p.y + r >= box.min.y && p.y - r <= box.max.y &&
			p.z + r >= box.min.z && p.z - r <= box.max.z;
	}

	// Orthographic projection of a light-space box: x and y to -1..1 (or to
	// 0..1 map coordinates), z to 1 toward the sun .. 0, as the reverse-Z depth
	// buffer expects
	matrix4x4d BoxProjection(const Aabb &box, bool toMapCoords)
	{
		const vector3d size = box.max - box.min;
		matrix4x4d m(0.0);
		if (toMapCoords) {
			m[0] = 1.0 / size.x;
			m[5] = 1.0 / size.y;
			m[12] = -box.min.x / size.x;
			m[13] = -box.min.y / size.y;
		} else {
			m[0] = 2.0 / size.x;
			m[5] = 2.0 / size.y;
			m[12] = -(box.max.x + box.min.x) / size.x;
			m[13] = -(box.max.y + box.min.y) / size.y;
		}
		m[10] = 1.0 / size.z;
		m[14] = -box.min.z / size.z;
		m[15] = 1.0;
		return m;
	}

	// share of the sunlight a shadow removes: all of it in vacuum, less where
	// the sky lights the shadows, down to ShadowStrength at Earth's surface density
	double ShadowStrength(FrameId frameId, const vector3d &camPos)
	{
		const double strength = Clamp(Pi::config->Int("ShadowStrength", 75), 0, 100) * 0.01;
		const Body *body = Frame::GetFrame(frameId)->GetBody();
		if (!body || !body->IsType(ObjectType::PLANET))
			return 1.0;

		double pressure, density;
		static_cast<const Planet *>(body)->GetAtmosphericState(camPos.Length(), &pressure, &density);
		const double sky = Clamp(density / EARTH_ATMOSPHERE_SURFACE_DENSITY, 0.0, 1.0);
		return 1.0 - (1.0 - strength) * sky;
	}
} // namespace

SceneShadows::SceneShadows(Graphics::Renderer *r) :
	m_renderer(r)
{
}

SceneShadows::~SceneShadows()
{
}

void SceneShadows::EnsureTarget(Cascade &c, int size)
{
	if (c.target && c.size == size)
		return;

	const Graphics::RenderTargetDesc desc(Uint16(size), Uint16(size), Graphics::TEXTURE_NONE, Graphics::TEXTURE_DEPTH, true, 0);
	c.target.reset(m_renderer->CreateRenderTarget(desc));
	c.size = c.target ? size : 0;
	c.valid = false;
	if (c.target)
		c.target->GetDepthTexture()->SetDepthCompare(true);
}

size_t SceneShadows::CountCasters(const matrix4x4d &viewToLight, const Aabb &box, bool staticOnly) const
{
	size_t n = 0;
	for (const Caster &caster : m_casters)
		if ((!staticOnly || caster.isStatic) && SphereInBox(viewToLight * caster.viewPos, caster.radius, box))
			n++;
	return n;
}

void SceneShadows::DrawCascade(Cascade &c, const matrix4x4d &viewToLight, bool staticOnly)
{
	m_renderer->SetRenderTarget(c.target.get());
	m_renderer->SetViewport({ 0, 0, c.size, c.size });
	m_renderer->ClearDepthBuffer();
	m_renderer->SetProjection(matrix4x4f(BoxProjection(c.box, false)));

	for (const TerrainCaster &terrain : m_terrain)
		terrain.body->RenderShadowCaster(m_renderer, viewToLight * terrain.frameToView, c.box);

	c.numCasters = 0;
	for (const Caster &caster : m_casters) {
		if ((staticOnly && !caster.isStatic) || !SphereInBox(viewToLight * caster.viewPos, caster.radius, c.box))
			continue;
		caster.body->RenderShadowCaster(m_renderer, viewToLight * caster.frameToView, c.box);
		c.numCasters++;
	}
}

void SceneShadows::Begin(const CameraContext *camera, const Graphics::Light &sun)
{
	PROFILE_SCOPED()
	m_active = false;

	const int quality = Clamp(Pi::config->Int("ShadowQuality", 3), 0, 3);
	if (quality == 0 || !Pi::game) {
		// free the video memory
		for (Cascade &c : m_cascades) {
			c.target.reset();
			c.size = 0;
			c.valid = false;
		}
		return;
	}
	const int size = MAP_SIZES[quality];

	// radii growing by about the same factor from map to map
	double radii[NUM_CASCADES];
	radii[0] = Clamp(Pi::config->Int("ShadowNearRadius", 120), 20, 1000);
	radii[1] = std::max(2.0 * radii[0], std::sqrt(radii[0] * Clamp(Pi::config->Int("ShadowDistance", 10000), 200, 20000)));
	radii[2] = std::max(2.0 * radii[1], double(Clamp(Pi::config->Int("ShadowDistance", 10000), 200, 20000)));
	const double reach = radii[2] + DEPTH_TOWARD_SUN[2];

	const FrameId frameId = camera->GetCameraFrame();
	Frame *camFrame = Frame::GetFrame(camera->GetTempFrame());
	const vector3d sunView(sun.GetPosition());
	if (!frameId.valid() || !camFrame || sunView.LengthSqr() < 1e-12)
		return;

	// the camera in the frame its surroundings are positioned in
	const matrix3x3d camOrient = camFrame->GetInterpOrientRelTo(frameId); // view -> frame
	const vector3d camPos = camFrame->GetInterpPositionRelTo(frameId);
	const vector3d sunDir = (camOrient * sunView).Normalized();

	// light space: +z toward the sun, +y as close to the frame's +y (a planet's axis) as it gets
	const vector3d up = std::abs(sunDir.y) < 0.99 ? vector3d(0.0, 1.0, 0.0) : vector3d(1.0, 0.0, 0.0);
	const vector3d lightX = up.Cross(sunDir).Normalized();
	const vector3d lightY = sunDir.Cross(lightX);
	const matrix3x3d frameToLight = matrix3x3d::FromVectors(lightX, lightY, sunDir).Transpose();

	// view space -> a cascade's light space
	auto viewToLight = [&](const Cascade &c) {
		return matrix4x4d(c.frameToLight * camOrient, c.frameToLight * (camPos - c.anchor));
	};

	// everything that can cast into the maps
	m_casters.clear();
	m_terrain.clear();
	const FrameId camFrameId = camera->GetTempFrame();
	const Body *player = Pi::player;
	vector3d nearCentreView(0.0, 0.0, -0.5 * radii[0]); // ahead of the camera, where most of the view is
	bool nearPlayer = false;
	for (Body *b : Pi::game->GetSpace()->GetBodies()) {
		const bool isTerrain = b->IsType(ObjectType::TERRAINBODY);
		if (!(isTerrain || b->IsType(ObjectType::MODELBODY)) || (b->GetFlags() & Body::FLAG_DRAW_EXCLUDE))
			continue;
		Frame *f = Frame::GetFrame(b->GetFrame());
		if (!f)
			continue;
		const matrix4x4d frameToView(f->GetInterpOrientRelTo(camFrameId), f->GetInterpPositionRelTo(camFrameId));
		const vector3d viewPos = frameToView * b->GetInterpPosition();

		if (isTerrain) {
			// the surface of a planet or moon close below
			const SystemBody *sbody = b->GetSystemBody();
			TerrainBody *terrain = static_cast<TerrainBody *>(b);
			if (sbody->GetSuperType() != SystemBody::SUPERTYPE_STAR &&
				viewPos.Length() - sbody->GetRadius() - terrain->GetMaxFeatureRadius() < reach)
				m_terrain.push_back({ terrain, frameToView });
			continue;
		}

		Caster caster;
		caster.body = static_cast<ModelBody *>(b);
		caster.frameToView = frameToView;
		caster.viewPos = viewPos;
		caster.radius = b->GetClipRadius();
		if (caster.viewPos.Length() - caster.radius > reach)
			continue;
		caster.isStatic = IsStaticCaster(b);
		m_casters.push_back(caster);

		// the player's ship is the subject of most views (and the camera in the cockpit)
		if (b == player && caster.viewPos.Length() < radii[2]) {
			nearCentreView = caster.viewPos;
			nearPlayer = true;
			radii[0] = std::max(radii[0], 1.5 * caster.radius);
		}
	}

	// the maps are centred on the player's ship; without it the near map is
	// ahead of the camera, the cached ones on the camera (steady as it turns)
	const vector3d focus = camOrient * nearCentreView + camPos;
	const vector3d cacheFocus = nearPlayer ? focus : camPos;

	// near map: snapped to whole texels in a light space fixed to the frame,
	// so shadow edges don't crawl while the camera moves
	Cascade &nearMap = m_cascades[0];
	nearMap.frame = frameId;
	nearMap.frameToLight = frameToLight;
	nearMap.anchor = camPos;
	nearMap.focus = focus;
	nearMap.sunDir = sunDir;
	nearMap.radius = radii[0];
	{
		const double r = radii[0];
		vector3d centre = frameToLight * (focus - camPos);
		const vector3d camLight = frameToLight * camPos;
		const double texel = 2.0 * r / size;
		centre.x = std::floor((centre.x + camLight.x) / texel) * texel - camLight.x;
		centre.y = std::floor((centre.y + camLight.y) / texel) * texel - camLight.y;
		nearMap.box = Aabb(centre - vector3d(r), centre + vector3d(r, r, r + DEPTH_TOWARD_SUN[0]), r);
	}
	size_t numCasters = m_terrain.size() + CountCasters(viewToLight(nearMap), nearMap.box, false);

	// mid and far maps: reused while they still match
	bool redraw[NUM_CASCADES] = { true, false, false };
	for (int i = 1; i < NUM_CASCADES; i++) {
		Cascade &c = m_cascades[i];
		const double r = radii[i];
		redraw[i] = !c.valid || c.frame != frameId || c.size != size || c.radius != r;
		if (!redraw[i]) {
			const vector3d moved = c.frameToLight * (cacheFocus - c.focus);
			redraw[i] = sunDir.Dot(c.sunDir) < CACHE_REDRAW_SUN_COS ||
				moved.Length() > CACHE_REDRAW_MOVE * r ||
				std::abs(Pi::game->GetTime() - c.drawnTime) > CACHE_REDRAW_SECONDS ||
				// a ship landed or took off
				CountCasters(viewToLight(c), c.box, true) != c.numCasters;
		}
		// redraw one cached map per frame at most; the far one keeps its old
		// picture for another frame if it has one
		if (redraw[i] && i > 1 && redraw[1] && c.valid)
			redraw[i] = false;
		if (redraw[i]) {
			c.frame = frameId;
			c.frameToLight = frameToLight;
			c.anchor = camPos;
			c.focus = cacheFocus;
			c.sunDir = sunDir;
			c.radius = r;
			c.drawnTime = Pi::game->GetTime();
			const vector3d centre = frameToLight * (cacheFocus - camPos);
			c.box = Aabb(centre - vector3d(r), centre + vector3d(r, r, r + DEPTH_TOWARD_SUN[i]), r);
			c.valid = false;
		}
		numCasters += c.valid ? c.numCasters : CountCasters(viewToLight(c), c.box, true);
	}

	// nothing anywhere near: the shaders skip the shadow lookups
	if (numCasters == 0)
		return;

	for (Cascade &c : m_cascades) {
		EnsureTarget(c, size);
		if (!c.target)
			return;
	}

	{
		// restores the render target, viewport and matrices
		Graphics::Renderer::StateTicket ticket(m_renderer);

		static const char *const timerNames[NUM_CASCADES] = { "shadows: near", "shadows: mid", "shadows: far" };
		for (int i = 0; i < NUM_CASCADES; i++) {
			Cascade &c = m_cascades[i];
			if (i > 0 && c.valid)
				continue;
			m_renderer->BeginGpuTimer(timerNames[i]);
			DrawCascade(c, viewToLight(c), i > 0);
			m_renderer->EndGpuTimer();
			c.valid = true;
		}
	}

	Graphics::ShadowDataBlock data;
	Graphics::Texture *maps[NUM_CASCADES];
	for (int i = 0; i < NUM_CASCADES; i++) {
		const Cascade &c = m_cascades[i];
		data.cascadeMatrix[i] = matrix4x4f(BoxProjection(c.box, true) * viewToLight(c));
		data.cascadeParams[i][0] = float((c.box.max.x - c.box.min.x) / c.size); // texel size
		data.cascadeParams[i][1] = float(1.0 / (c.box.max.z - c.box.min.z));	  // depth per metre
		data.cascadeParams[i][2] = 0.f;
		data.cascadeParams[i][3] = 0.f;
		maps[i] = c.target->GetDepthTexture();
	}
	data.state[0] = 1.f;
	data.state[1] = float(ShadowStrength(frameId, camPos));
	data.state[2] = Pi::config->Int("ShadowDebug", 0) ? 1.f : 0.f;
	data.state[3] = 0.f;

	m_renderer->SetShadowData(&data, maps);
	m_active = true;
}

void SceneShadows::End()
{
	if (m_active)
		m_renderer->SetShadowData(nullptr, nullptr);
	m_active = false;
}
