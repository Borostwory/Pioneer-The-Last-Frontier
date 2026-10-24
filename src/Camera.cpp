// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Camera.h"

#include "Body.h"
#include "CityOnPlanet.h"
#include "Frame.h"
#include "Game.h"
#include "GameConfig.h"
#include "Pi.h"
#include "Planet.h"
#include "PlanetClouds.h"
#include "Player.h"
#include "SceneShadows.h"
#include "Sfx.h"
#include "Space.h"
#include "SpaceStation.h"

#include "galaxy/StarSystem.h"
#include "graphics/TextureBuilder.h"
#include "graphics/Types.h"
#include "graphics/RenderState.h"
#include "graphics/VertexBuffer.h"

#include "profiler/Profiler.h"

using namespace Graphics;

// if a body would render smaller than this many pixels, just ignore it
static const float OBJECT_HIDDEN_PIXEL_THRESHOLD = 2.0f;

// if a terrain object would render smaller than this many pixels, draw a billboard instead
static const float BILLBOARD_PIXEL_THRESHOLD = 8.0f;

// city lights are fully on below this daylight on level ground and off above
// that (1: the Sun overhead on an airless body at 1 AU; CITY_LIGHTS_* in city-glow.glsl)
static const double CITY_LIGHTS_DARK = 0.03;
static const double CITY_LIGHTS_BRIGHT = 0.12;
// share of the starlight an Earth-like sky takes out per air mass overhead (about 20%)
static const double CITY_AIR_EXTINCTION = 0.22;
// cities seen from afar as dots (DrawCityDots): the smallest dot in pixels at
// 1080 lines; the city's width in pixels below which it is all dot, and above
// which all glow on the terrain; the dot's brightness; how far toward the
// camera it is drawn (share of its distance); the smallest planet in pixels
// across that gets dots
static const float CITY_DOT_SIZE = 2.5f;
static const double CITY_DOT_FULL = 3.0;
static const double CITY_DOT_GONE = 10.0;
static const double CITY_DOT_BRIGHTNESS = 0.6;
static const double CITY_DOT_LIFT = 0.01;
static const double CITY_DOT_MIN_PLANET = 32.0;

CameraContext::CameraContext(float width, float height, float fovAng, float zNear, float zFar) :
	m_width(width),
	m_height(height),
	m_fovAng(fovAng),
	m_zNear(zNear),
	m_zFar(zFar),
	m_frustum(m_width, m_height, m_fovAng, m_zNear, m_zFar),
	m_projMatrix(matrix4x4f::InfinitePerspectiveMatrix(DEG2RAD(m_fovAng), m_width / m_height, m_zNear)),
	m_frame(FrameId::Invalid),
	m_pos(0.0),
	m_orient(matrix3x3d::Identity),
	m_camFrame(FrameId::Invalid)
{
}

CameraContext::~CameraContext()
{
	if (m_camFrame)
		EndFrame();
}

void CameraContext::SetFovAng(float newAng)
{
	m_fovAng = newAng;
	const float fovAngRadians = DEG2RAD(m_fovAng);
	m_frustum = Frustum(m_width, m_height, fovAngRadians, m_zNear, m_zFar);
	m_projMatrix = matrix4x4f::InfinitePerspectiveMatrix(fovAngRadians, m_width / m_height, m_zNear);
}

void CameraContext::BeginFrame()
{
	assert(m_frame.valid());
	assert(!m_camFrame.valid());

	// make temporary camera frame
	m_camFrame = Frame::CreateCameraFrame(m_frame);

	Frame *camFrame = Frame::GetFrame(m_camFrame);
	// move and orient it to the camera position
	camFrame->SetOrient(m_orient, Pi::game ? Pi::game->GetTime() : 0.0);
	camFrame->SetPosition(m_pos);

	// make sure old orient and interpolated orient (rendering orient) are not rubbish
	camFrame->ClearMovement();
	camFrame->UpdateInterpTransform(1.0); // update root-relative pos/orient
}

void CameraContext::EndFrame()
{
	assert(m_frame.valid());
	assert(m_camFrame.valid());

	Frame::DeleteCameraFrame(m_camFrame);

	m_camFrame = FrameId::Invalid;
}

void CameraContext::ApplyDrawTransforms(Graphics::Renderer *r)
{
	Graphics::SetFov(m_fovAng);
	r->SetProjection(GetProjectionMatrix());
	r->SetTransform(matrix4x4f::Identity);
}

bool Camera::BodyAttrs::sort_BodyAttrs(const BodyAttrs &a, const BodyAttrs &b)
{
	// both drawing last; distance order
	if (a.bodyFlags & Body::FLAG_DRAW_LAST && b.bodyFlags & Body::FLAG_DRAW_LAST)
		return a.camDist > b.camDist;

	// a drawing last; draw b first
	if (a.bodyFlags & Body::FLAG_DRAW_LAST)
		return false;

	// b drawing last; draw a first
	if (b.bodyFlags & Body::FLAG_DRAW_LAST)
		return true;

	// both in normal draw; distance order
	return a.camDist > b.camDist;
}

Camera::Camera(RefCountedPtr<CameraContext> context, Graphics::Renderer *renderer) :
	m_context(context),
	m_renderer(renderer),
	m_shadows(new SceneShadows(renderer))
{
	Graphics::MaterialDescriptor desc;
	desc.textures = 1;

	Graphics::RenderStateDesc rsd;
	rsd.blendMode = Graphics::BLEND_ALPHA_ONE;
	rsd.depthWrite = false;
	rsd.primitiveType = Graphics::POINTS;

	Graphics::VertexFormatDesc vfmt = Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_NORMAL);

	m_billboardMaterial.reset(m_renderer->CreateMaterial("billboards", desc, rsd, vfmt));
	m_billboardMaterial->SetTexture("texture0"_hash,
		Graphics::TextureBuilder::Billboard("textures/planet_billboard.dds").GetOrCreateTexture(m_renderer, "billboard"));

	// lights of distant cities, added to the scene behind them
	Graphics::RenderStateDesc dotRsd;
	dotRsd.blendMode = Graphics::BLEND_ADDITIVE;
	dotRsd.depthWrite = false;
	dotRsd.primitiveType = Graphics::POINTS;
	m_cityDotMaterial.reset(m_renderer->CreateMaterial("city_dots", Graphics::MaterialDescriptor(), dotRsd,
		Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_DIFFUSE | Graphics::ATTRIB_NORMAL)));
}

Camera::~Camera()
{
}

static void position_system_lights(Frame *camFrame, Frame *frame, std::vector<Camera::LightSource> &lights)
{
	PROFILE_SCOPED()
	if (lights.size() > 3) return;

	SystemBody *body = frame->GetSystemBody();
	// IsRotFrame check prevents double counting
	if (body && !frame->IsRotFrame() && (body->GetSuperType() == SystemBody::SUPERTYPE_STAR)) {
		vector3d lpos = frame->GetPositionRelTo(camFrame->GetId());

		const Color &col = body->GetStarColor();

		const Color lightCol(col[0], col[1], col[2], 0);
		vector3f lightpos(lpos.x, lpos.y, lpos.z);
		Graphics::Light light(Graphics::Light::LIGHT_DIRECTIONAL, lightpos, lightCol, lightCol);
		lights.push_back(Camera::LightSource(frame->GetBody(), light));
	}

	for (FrameId kid : frame->GetChildren()) {
		Frame *kid_f = Frame::GetFrame(kid);
		position_system_lights(camFrame, kid_f, lights);
	}
}

void Camera::Update()
{
	FrameId camFrame = m_context->GetTempFrame();

	// evaluate each body and determine if/where/how to draw it
	m_sortedBodies.clear();
	m_spaceStations.clear();
	for (Body *b : Pi::game->GetSpace()->GetBodies()) {
		BodyAttrs attrs;
		attrs.body = b;
		attrs.billboard = false; // false by default
		attrs.calcAtmosphereLighting = false; // false by default
		attrs.calcInteriorLighting = false;

		// If the body wishes to be excluded from the draw, skip it.
		if (b->GetFlags() & Body::FLAG_DRAW_EXCLUDE)
			continue;

		// determine position and transform for draw
		//		Frame::GetFrameTransform(b->GetFrame(), camFrame, attrs.viewTransform);		// doesn't use interp coords, so breaks in some cases
		Frame *f = Frame::GetFrame(b->GetFrame());
		attrs.viewTransform = f->GetInterpOrientRelTo(camFrame);
		attrs.viewTransform.SetTranslate(f->GetInterpPositionRelTo(camFrame));
		attrs.viewCoords = attrs.viewTransform * b->GetInterpPosition();

		// cull off-screen objects
		double rad = b->GetClipRadius();
		if (!m_context->GetFrustum().TestSphereInfinite(attrs.viewCoords, rad))
			continue;

		attrs.camDist = attrs.viewCoords.Length();
		attrs.bodyFlags = b->GetFlags();

		// approximate pixel width (disc diameter) of body on screen
		// FIXME: this should reference a property set on the camera instead of querying the window size
		const float pixSize = m_renderer->GetWindowHeight() * 2.0 * rad / (attrs.camDist * Graphics::GetFovFactor());

		// terrain objects are visible from distance but might not have any discernable features
		if (b->IsType(ObjectType::TERRAINBODY)) {
			if (pixSize < BILLBOARD_PIXEL_THRESHOLD) {
				attrs.billboard = true;

				// scale the position of the terrain body until it fits within the far plane for its billboard to be rendered
				// Note that with an infinite projection matrix there is no far plane and this is somewhat unnecessary
				double zFar = m_context->GetZFar();
				double scaleFactor = zFar / attrs.viewCoords.Length() - 0.000001; // tiny nudge closer from the far plane

				attrs.billboardPos = vector3f(attrs.viewCoords * std::min(scaleFactor, 1.0));

				// limit the minimum billboard size for planets so they're always a little visible
				attrs.billboardSize = std::max(1.0f, pixSize);
				if (b->IsType(ObjectType::STAR)) {
					attrs.billboardColor = b->GetSystemBody()->GetStarColor();
				} else if (b->IsType(ObjectType::PLANET)) {
					// XXX this should incorporate some lighting effect
					// (ie, colour of the illuminating star(s))
					attrs.billboardColor = b->GetSystemBody()->GetAlbedo();
				} else {
					attrs.billboardColor = Color::WHITE;
				}

				// this should always be the main star in the system - except for the star itself!
				if (!m_lightSources.empty() && !b->IsType(ObjectType::STAR)) {
					const Graphics::Light &light = m_lightSources[0].GetLight();
					attrs.billboardColor *= light.GetDiffuse(); // colour the billboard a little with the Starlight
				}

				attrs.billboardColor.a = 255; // no alpha, these things are hard enough to see as it is
			}
		} else if (pixSize < OBJECT_HIDDEN_PIXEL_THRESHOLD) {
			continue;
		}

		Body *parentBody = f->GetBody();
		if (parentBody && parentBody->GetType() == ObjectType::PLANET) {
			auto *planet = static_cast<Planet *>(parentBody);

			double atmo_rad_sqr = planet->GetAtmosphereRadius() * planet->GetAtmosphereRadius();
			if (b->IsType(ObjectType::MODELBODY) && b->GetPosition().LengthSqr() <= atmo_rad_sqr)
				attrs.calcAtmosphereLighting = true;
		}

		if(b->IsType(ObjectType::SHIP)) {
			attrs.calcInteriorLighting = true;
		}

		m_sortedBodies.push_back(attrs);

		if(b->IsType(ObjectType::SPACESTATION)) {
			m_spaceStations.push_back(b);
		}
	}

	// depth sort
	m_sortedBodies.sort();
}

void Camera::Draw(const Body *excludeBody)
{
	PROFILE_SCOPED()

	FrameId camFrameId = m_context->GetTempFrame();
	FrameId rootFrameId = Pi::game->GetSpace()->GetRootFrame();

	Frame *camFrame = Frame::GetFrame(camFrameId);
	Frame *rootFrame = Frame::GetFrame(rootFrameId);

	m_renderer->ClearScreen();

	// planets queue their clouds while being drawn, see PlanetClouds.h
	PlanetClouds::BeginFrame();

	matrix4x4d trans2bg;
	Frame::GetFrameTransform(rootFrameId, camFrameId, trans2bg);
	trans2bg.ClearToRotOnly();

	// Pick up to four suitable system light sources (stars)
	m_lightSources.clear();
	m_lightSources.reserve(4);
	position_system_lights(camFrame, rootFrame, m_lightSources);

	if (m_lightSources.empty()) {
		// no lights means we're somewhere weird (eg hyperspace). fake one
		Graphics::Light light(Graphics::Light::LIGHT_DIRECTIONAL, vector3f(0.f), Color::WHITE, Color::WHITE);
		m_lightSources.push_back(LightSource(0, light));
	}

	//fade space background based on atmosphere thickness and light angle
	float bgIntensity = 1.f;
	Frame *camParent = Frame::GetFrame(camFrame->GetParent());
	if (camParent && camParent->IsRotFrame()) {
		//check if camera is near a planet
		Body *camParentBody = camParent->GetBody();
		if (camParentBody && camParentBody->IsType(ObjectType::PLANET)) {
			Planet *planet = static_cast<Planet *>(camParentBody);
			const vector3f relpos(planet->GetInterpPositionRelTo(camFrameId));
			double altitude(relpos.Length());
			double pressure, density;
			planet->GetAtmosphericState(altitude, &pressure, &density);
			if (pressure >= 0.001) {
				//go through all lights to calculate something resembling light intensity
				float intensity = 0.f;
				const Body *pBody = Pi::game->GetPlayer();
				for (Uint32 i = 0; i < m_lightSources.size(); i++) {
					// Set up data for eclipses. All bodies are assumed to be spheres.
					const LightSource &it = m_lightSources[i];
					const vector3f lightDir(it.GetLight().GetPosition().Normalized());
					intensity += ShadowedIntensity(i, pBody) * std::max(0.f, lightDir.Dot(-relpos.Normalized())) * (it.GetLight().GetDiffuse().GetLuminance() / 255.0f);
				}
				intensity = Clamp(intensity, 0.0f, 1.0f);

				//calculate background intensity with some hand-tweaked fuzz applied
				bgIntensity = Clamp(1.f - std::min(1.f, powf(density, 0.25f)) * (0.3f + powf(intensity, 0.25f)), 0.f, 1.f);
			}
		}
	}

	Pi::game->GetSpace()->GetBackground()->SetIntensity(bgIntensity);
	m_renderer->BeginGpuTimer("background");
	Pi::game->GetSpace()->GetBackground()->Draw(trans2bg);
	m_renderer->EndGpuTimer();

	{
		std::vector<Graphics::Light> rendererLights;
		rendererLights.reserve(m_lightSources.size());
		for (size_t i = 0; i < m_lightSources.size(); i++)
			rendererLights.push_back(m_lightSources[i].GetLight());
		m_renderer->SetLights(rendererLights.size(), &rendererLights[0]);
	}

	// Save lights for later restoring
	oldLightIntensities.clear();
	std::vector<float> lightIntensities;
	for (size_t i = 0; i < m_lightSources.size(); i++) {
		oldLightIntensities.push_back(m_renderer->GetLight(i).GetIntensity());
		lightIntensities.push_back(1.0f);
	}

	Graphics::VertexArray billboards(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_NORMAL);

	// shadows of ships, stations and buildings from the main star (not the
	// stand-in light used where there are no stars)
	if (m_lightSources[0].GetBody()) {
		m_renderer->BeginGpuTimer("shadows");
		m_shadows->Begin(m_context.Get(), m_lightSources[0].GetLight());
		m_renderer->EndGpuTimer();
	}

	// night lights of the nearest city, for the lit shaders (city-lights.glsl)
	SetCityLights();

	m_renderer->BeginGpuTimer("bodies");
	for (std::list<BodyAttrs>::iterator i = m_sortedBodies.begin(); i != m_sortedBodies.end(); ++i) {
		BodyAttrs *attrs = &(*i);

		// explicitly exclude a single body if specified (eg player)
		if (attrs->body == excludeBody)
			continue;

		// draw something!
		if (attrs->billboard) {
			billboards.Add(attrs->billboardPos, vector3f(0.f, 0.f, attrs->billboardSize));
			continue;
		}

		PrepareLighting(attrs->body, attrs->calcAtmosphereLighting, attrs->calcInteriorLighting);
		attrs->body->Render(m_renderer, this, attrs->viewCoords, attrs->viewTransform);
	}

	m_renderer->EndGpuTimer();
	RestoreLighting();
	m_shadows->End();

	// the lights of cities too far away to see on the terrain
	DrawCityDots(camFrameId);

	// clouds over everything drawn so far, using the scene depth; their
	// shadows leave the city lights alone, so those stay set until then
	m_renderer->BeginGpuTimer("clouds");
	PlanetClouds::RenderCloudPasses(m_renderer);
	m_renderer->EndGpuTimer();
	m_renderer->SetCityLightData(nullptr, nullptr, nullptr);

	if (!billboards.IsEmpty()) {
		Graphics::Renderer::MatrixTicket mt(m_renderer, matrix4x4f::Identity);
		m_renderer->DrawBuffer(&billboards, m_billboardMaterial.get());
	}

	float illuminationFactor = 1.f;
	if (Pi::game && Pi::player && !Pi::player->IsDead()) {
		double amb = 0.0, direct = 1.0;
		CalcLighting(Pi::player, amb, direct);
		illuminationFactor = Clamp(float(amb + direct), 0.08f, 1.0f);
		illuminationFactor = std::pow(illuminationFactor, 1.5);
	}
	SfxManager::RenderAll(m_renderer, rootFrameId, camFrameId, illuminationFactor);
}

// How far a city's lights are on (0..1) as the daylight over it fades: the
// starlight on level ground, dimmed by the distance to the star (as the
// terrain shaders do), by the air above and by eclipses (city-glow.glsl does
// the same per pixel). up: the view-space direction up at the city.
double Camera::CityDarkness(const SpaceStation *station, const vector3d &up) const
{
	double airMass = 0.0; // the air overhead relative to Earth's
	const Body *planet = Frame::GetFrame(station->GetFrame())->GetBody();
	if (planet && planet->IsType(ObjectType::PLANET)) {
		double pressure, density;
		static_cast<const Planet *>(planet)->GetAtmosphericState(station->GetPosition().Length(), &pressure, &density);
		airMass = density / EARTH_ATMOSPHERE_SURFACE_DENSITY;
	}
	double daylight = 0.0;
	for (size_t i = 0; i < m_lightSources.size(); i++) {
		const LightSource &source = m_lightSources[i];
		if (!source.GetBody()) {
			// the stand-in light where there are no stars
			daylight = 1.0;
			break;
		}
		const vector3d toStar(source.GetLight().GetPosition());
		const double sunUp = up.Dot(toStar.Normalized());
		if (sunUp <= 0.0)
			continue;
		const double starLight = source.GetLight().GetDiffuse().GetLuminance() / 255.0 * AU / toStar.Length();
		daylight += starLight * sunUp * std::exp(-CITY_AIR_EXTINCTION * airMass / std::max(sunUp, 0.05)) * ShadowedIntensity(int(i), station);
	}
	const double t = Clamp((CITY_LIGHTS_BRIGHT - daylight) / (CITY_LIGHTS_BRIGHT - CITY_LIGHTS_DARK), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

// From far away, where a city is only a few pixels across, its lights are
// drawn as a dot of at least CITY_DOT_SIZE pixels, so they stay visible as a
// faint point; closer, the city's glow on the terrain (city-glow.glsl) takes
// over. Drawn before the clouds, which cover them.
void Camera::DrawCityDots(FrameId camFrameId) const
{
	const int brightness = Pi::config->Int("CityGlow");
	if (brightness <= 0 || !Pi::game)
		return;

	const double pixelsPerRadian = m_renderer->GetWindowHeight() / Graphics::GetFovFactor();
	const float minSize = CITY_DOT_SIZE * m_renderer->GetWindowHeight() / 1080.0f;

	Graphics::VertexArray dots(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_DIFFUSE | Graphics::ATTRIB_NORMAL);
	for (const Body *b : Pi::game->GetSpace()->GetBodies()) {
		if (!b->IsType(ObjectType::SPACESTATION) || !static_cast<const SpaceStation *>(b)->IsGroundStation())
			continue;
		const SpaceStation *station = static_cast<const SpaceStation *>(b);
		Frame *frame = Frame::GetFrame(station->GetFrame());
		const Body *planet = frame->GetBody();
		if (!planet || !planet->IsType(ObjectType::PLANET))
			continue;

		matrix4x4d frameToView = frame->GetInterpOrientRelTo(camFrameId);
		frameToView.SetTranslate(frame->GetInterpPositionRelTo(camFrameId));
		const vector3d planetPos = frameToView * vector3d(0.0);
		const vector3d pos = frameToView * station->GetInterpPosition();
		const double dist = pos.Length();

		// only on planets drawn as more than a dot themselves
		const double planetRadius = planet->GetSystemBody()->GetRadius();
		if (2.0 * planetRadius * pixelsPerRadian < CITY_DOT_MIN_PLANET * planetPos.Length())
			continue;

		// on the side of the planet facing the camera, fading at the horizon
		const vector3d up = (pos - planetPos).Normalized();
		const double facing = -up.Dot(pos) / dist;
		if (facing <= 0.0)
			continue;

		double cityRadius;
		if (!CityOnPlanet::GetCitySummary(planet->GetSystemBody(), station, cityRadius))
			continue;
		// all dot below CITY_DOT_FULL pixels across, all glow on the terrain from CITY_DOT_GONE
		const double across = 2.0 * cityRadius * pixelsPerRadian / dist;
		const double t = Clamp((CITY_DOT_GONE - across) / (CITY_DOT_GONE - CITY_DOT_FULL), 0.0, 1.0);
		const double farShare = t * t * (3.0 - 2.0 * t);
		if (farShare <= 0.0)
			continue;

		const double light = farShare * CityDarkness(station, up) * std::min(1.0, facing * 10.0) * (brightness / 100.0) *
			CITY_DOT_BRIGHTNESS * Clamp(std::sqrt(cityRadius / 10000.0), 0.5, 1.5);
		if (light <= 0.0)
			continue;

		// pulled toward the camera along the line of sight, so the terrain
		// mesh, coarse from afar, does not hide it; it stays in place on screen
		const vector3d lifted = pos * (1.0 - CITY_DOT_LIFT);
		const float size = std::max(minSize, float(across));
		dots.Add(vector3f(lifted), Color(255, 179, 107), vector3f(float(light), 0.0f, size));
	}

	if (!dots.IsEmpty()) {
		Graphics::Renderer::MatrixTicket mt(m_renderer, matrix4x4f::Identity);
		m_renderer->DrawBuffer(&dots, m_cityDotMaterial.get());
	}
}

// Hands the renderer the night lights of the city nearest the camera, if any
// is close enough and it is dark there (see CityOnPlanet.h).
void Camera::SetCityLights() const
{
	const int brightness = Pi::config->Int("CityLights");
	if (brightness <= 0)
		return;

	const BodyAttrs *nearest = nullptr;
	for (const BodyAttrs &attrs : m_sortedBodies) {
		if (!attrs.body->IsType(ObjectType::SPACESTATION))
			continue;
		const CityOnPlanet *city = static_cast<const SpaceStation *>(attrs.body)->GetAdjacentCity();
		if (city && city->HasLights() && (!nearest || attrs.camDist < nearest->camDist))
			nearest = &attrs;
	}
	if (!nearest)
		return;

	// city space: the station's frame around its position
	const SpaceStation *station = static_cast<const SpaceStation *>(nearest->body);
	const matrix4x4d cityToView = nearest->viewTransform * matrix4x4d(station->GetInterpOrient(), station->GetInterpPosition());

	const vector3d up = cityToView.ApplyRotationOnly(vector3d(0.0, 1.0, 0.0));
	const double strength = CityDarkness(station, up) * CityOnPlanet::LightsFade(nearest->camDist) * (brightness / 100.0);
	if (strength <= 0.0)
		return;

	const CityOnPlanet *city = station->GetAdjacentCity();
	Graphics::CityLightDataBlock data;
	city->GetLightData(data);
	data.viewToCity = matrix4x4f(cityToView.Inverse());
	data.state[0] = float(strength);
	m_renderer->SetCityLightData(&data, city->GetLightMap(), city->GetLightGrid());
}

// Calculates the ambiently and directly lit portions of the lighting model taking into account the atmosphere and sun positions at a given location
// 1. Calculates the amount of direct illumination available taking into account
//    * multiple suns
//    * sun positions relative to up direction i.e. light is dimmed as suns set
//    * Thickness of the atmosphere overhead i.e. as atmospheres get thicker light starts dimming earlier as sun sets, without atmosphere the light switches off at point of sunset
// 2. Calculates the split between ambient and directly lit portions taking into account
//    * Atmosphere density (optical thickness) of the sky dome overhead
//        as optical thickness increases the fraction of ambient light increases
//        this takes altitude into account automatically
//    * As suns set the split is biased towards ambient
void Camera::CalcLighting(const Body *b, double &ambient, double &direct) const
{
	const double minAmbient = 0.05;
	ambient = minAmbient;
	direct = 1.0;

	Body *astro = Frame::GetFrame(b->GetFrame())->GetBody();
	if (!astro)
		return;

	Planet *planet = static_cast<Planet *>(astro);
	FrameId rotFrame = planet->GetFrame();

	// position relative to the rotating frame of the planet
	vector3d upDir = b->GetInterpPositionRelTo(rotFrame);
	const double planetRadius = planet->GetSystemBody()->GetRadius();
	const double dist = std::max(planetRadius, upDir.Length());
	upDir.Normalize();

	double pressure, density;
	planet->GetAtmosphericState(dist, &pressure, &density);
	double surfaceDensity;
	Color cl;
	planet->GetSystemBody()->GetAtmosphereFlavor(&cl, &surfaceDensity);

	// approximate optical thickness fraction as fraction of density remaining relative to earths
	double opticalThicknessFraction = density / EARTH_ATMOSPHERE_SURFACE_DENSITY;

	// tweak optical thickness curve - lower exponent ==> higher altitude before ambient level drops
	// Commenting this out, since it leads to a sharp transition at
	// atmosphereRadius, where density is suddenly 0
	//opticalThicknessFraction = pow(std::max(0.00001,opticalThicknessFraction),0.15); //max needed to avoid 0^power

	if (opticalThicknessFraction < 0.0001)
		return;

	//step through all the lights and calculate contributions taking into account sun position
	double light = 0.0;
	double light_clamped = 0.0;

	const std::vector<Camera::LightSource> &lightSources = m_lightSources;
	for (const LightSource &source : m_lightSources) {
		double sunAngle;
		// calculate the extent the sun is towards zenith
		const Body *lightBody = source.GetBody();
		if (lightBody) {
			// relative to the rotating frame of the planet
			const vector3d lightDir = (lightBody->GetInterpPositionRelTo(rotFrame).Normalized());
			sunAngle = lightDir.Dot(upDir);
		} else {
			// light is the default light for systems without lights
			sunAngle = 1.0;
		}

		const double critAngle = -sqrt(dist * dist - planetRadius * planetRadius) / dist;

		//0 to 1 as sunangle goes from critAngle to 1.0
		double sunAngle2 = (Clamp(sunAngle, critAngle, 1.0) - critAngle) / (1.0 - critAngle);

		// angle at which light begins to fade on Earth
		const double surfaceStartAngle = 0.3;
		// angle at which sun set completes, which should be after sun has dipped below the horizon on Earth
		const double surfaceEndAngle = -0.18;

		const double start = std::min((surfaceStartAngle * opticalThicknessFraction), 1.0);
		const double end = std::max((surfaceEndAngle * opticalThicknessFraction), -0.2);

		sunAngle = (Clamp(sunAngle - critAngle, end, start) - end) / (start - end);

		light += sunAngle;
		light_clamped += sunAngle2;
	}

	light_clamped /= lightSources.size();
	light /= lightSources.size();

	// brightness depends on optical depth and intensity of light from all the stars
	direct = 1.0 - Clamp((1.0 - light), 0.0, 1.0) * Clamp(opticalThicknessFraction, 0.0, 1.0);

	// ambient light fraction
	// alter ratio between directly and ambiently lit portions towards ambiently lit as sun sets
	const double fraction = (0.2 + 0.8 * (1.0 - light_clamped)) * Clamp(opticalThicknessFraction, 0.0, 1.0);

	// fraction of light left over to be lit directly
	direct = (1.0 - fraction) * direct;

	// scale ambient by amount of light
	ambient = fraction * (Clamp((light), 0.0, 1.0)) * 0.25;

	ambient = std::max(minAmbient, ambient);
}

void Camera::CalcShadows(const int lightNum, const Body *b, std::vector<Shadow> &shadowsOut) const
{
	// Set up data for eclipses. All bodies are assumed to be spheres.
	const Body *lightBody = m_lightSources[lightNum].GetBody();
	if (!lightBody)
		return;

	const double lightRadius = lightBody->GetPhysRadius();
	const vector3d bLightPos = lightBody->GetPositionRelTo(b);
	const vector3d lightDir = bLightPos.Normalized();

	double bRadius;
	if (b->IsType(ObjectType::TERRAINBODY))
		bRadius = b->GetSystemBody()->GetRadius();
	else
		bRadius = b->GetPhysRadius();

	// Look for eclipsing third bodies:
	for (const Body *b2 : Pi::game->GetSpace()->GetBodies()) {
		if (b2 == b || b2 == lightBody || !(b2->IsType(ObjectType::PLANET) || b2->IsType(ObjectType::STAR)))
			continue;

		double b2Radius = b2->GetSystemBody()->GetRadius();
		vector3d b2pos = b2->GetPositionRelTo(b);
		const double perpDist = lightDir.Dot(b2pos);

		if (perpDist <= 0 || perpDist > bLightPos.Length())
			// b2 isn't between b and lightBody; no eclipse
			continue;

		// Project to the plane perpendicular to lightDir, taking the line between the shadowed sphere
		// (b) and the light source as zero. Our calculations assume that the light source is at
		// infinity. All lengths are normalised such that b has radius 1. srad is then the radius of the
		// occulting sphere (b2), and lrad is the apparent radius of the light disc when considered to
		// be at the distance of b2, and projectedCentre is the normalised projected position of the
		// centre of b2 relative to the centre of b. The upshot is that from a point on b, with
		// normalised projected position p, the picture is of a disc of radius lrad being occulted by a
		// disc of radius srad centred at projectedCentre-p. To determine the light intensity at p, we
		// then just need to estimate the proportion of the light disc being occulted.
		const double srad = b2Radius / bRadius;
		const double lrad = (lightRadius / bLightPos.Length()) * perpDist / bRadius;
		if (srad / lrad < 0.01) {
			// any eclipse would have negligible effect - ignore
			continue;
		}
		const vector3d projectedCentre = (b2pos - perpDist * lightDir) / bRadius;
		if (projectedCentre.Length() < 1 + srad + lrad) {
			// some part of b is (partially) eclipsed
			Camera::Shadow shadow = { projectedCentre, srad, lrad };
			shadowsOut.push_back(shadow);
		}
	}
}

float discCovered(const double dist, const double rad)
{
	// proportion of unit disc covered by a second disc of radius rad placed
	// dist from centre of first disc.
	//
	// WLOG, the second disc is displaced horizontally to the right.
	// xl = rightwards distance to intersection of the two circles.
	// xs = normalised leftwards distance from centre of second disc to intersection.
	// d = vertical distance to an intersection point
	// The clampings handle the cases where one disc contains the other.
	const double radsq = rad * rad;
	const double xl = Clamp((dist * dist + 1.0 - radsq) / (2.0 * std::max(0.001, dist)), -1.0, 1.0);
	const double xs = Clamp((dist - xl) / std::max(0.001, rad), -1.0, 1.0);
	const double d = sqrt(std::max(0.0, 1.0 - xl * xl));

	const double th = Clamp(acos(xl), 0.0, M_PI);
	const double th2 = Clamp(acos(xs), 0.0, M_PI);

	assert(!is_nan(d) && !is_nan(th) && !is_nan(th2));

	// covered area can be calculated as the sum of segments from the two
	// discs plus/minus some triangles, and it works out as follows:
	return Clamp((th + radsq * th2 - dist * d) / M_PI, 0.0, 1.0);
}

static std::vector<Camera::Shadow> shadows;

float Camera::ShadowedIntensity(const int lightNum, const Body *b) const
{
	shadows.clear();
	shadows.reserve(16);
	CalcShadows(lightNum, b, shadows);
	double product = 1.0;
	for (std::vector<Camera::Shadow>::const_iterator it = shadows.begin(), itEnd = shadows.end(); it != itEnd; ++it)
		product *= 1.0 - discCovered(it->centre.Length() / it->lrad, it->srad / it->lrad);
	return float(product);
}

// PrincipalShadows(b,n): returns the n biggest shadows on b in order of size
void Camera::PrincipalShadows(const Body *b, const int n, std::vector<Shadow> &shadowsOut) const
{
	shadows.clear();
	shadows.reserve(16);
	for (size_t i = 0; i < 4 && i < m_lightSources.size(); i++) {
		CalcShadows(i, b, shadows);
	}
	shadowsOut.reserve(shadows.size());
	std::sort(shadows.begin(), shadows.end());
	std::vector<Shadow>::reverse_iterator it = shadows.rbegin(), itREnd = shadows.rend();
	for (int i = 0; i < n; i++) {
		if (it == itREnd) break;
		shadowsOut.push_back(*(it++));
	}
}
void Camera::CalcInteriorLighting(const Body* b, Color4ub &sLight, double &sFac) const
{
	for(const auto& ss : m_spaceStations) {
		const SpaceStation* as_ss = static_cast<SpaceStation *>(ss);
		const double distance2 = as_ss->GetPositionRelTo(b).LengthSqr();
		const double maxClip = b->GetClipRadius() + ss->GetClipRadius();

		// This short-circuits for efficient checking. Only one station may actually set
		// lighting values in CalcInteriorLighting, which is why we return as soon as that happens
		if (distance2 < maxClip * maxClip && as_ss->CalcInteriorLighting(b, sLight, sFac))
			return;
	}
}

void Camera::PrepareLighting(const Body *b, bool doAtmosphere, bool doInteriors) const
{
	std::vector<float> lightIntensities;

	double ambient = 0.05, direct = 1.0;
	if (doAtmosphere)
		CalcLighting(b, ambient, direct);

	Color4ub ambientLightColor = Color::WHITE;
	Color4ub stationLightColor = Color::WHITE;
	double stationFactor = 0.0;

	if (doInteriors)
		CalcInteriorLighting(b, stationLightColor, stationFactor);

	direct = direct * (1.0 - stationFactor);
	ambient = ambient * (1.0 - stationFactor) + stationFactor;

	for (size_t i = 0; i < m_lightSources.size(); i++)
		lightIntensities.push_back(direct * ShadowedIntensity(i, b));

	// Setup dynamic lighting parameters
	Color4ub ambientMix = (ambientLightColor.Shade((float)stationFactor)
						   + stationLightColor.Shade(1.0f - stationFactor)).Shade(1.0 - ambient);
	ambientMix.a = 255;

	m_renderer->SetAmbientColor(ambientMix);
	m_renderer->SetLightIntensity(m_lightSources.size(), lightIntensities.data());

}

void Camera::RestoreLighting() const
{
	m_renderer->SetAmbientColor(Color::WHITE);
	m_renderer->SetLightIntensity(m_lightSources.size(), oldLightIntensities.data());
}
