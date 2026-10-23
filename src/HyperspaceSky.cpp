// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "HyperspaceSky.h"

#include "Game.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Random.h"
#include "Relativity.h"
#include "core/StringHash.h"
#include "gameconsts.h"
#include "galaxy/Galaxy.h"
#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/RenderState.h"
#include "graphics/Renderer.h"
#include "graphics/VertexArray.h"
#include "profiler/Profiler.h"

#include <algorithm>
#include <cmath>

namespace {
	// Total starlight of the sky at rest, as 550 nm flux in W m^-2 nm^-1: magnitude -3.1
	// against the V band zero point of 3.63e-11. Ties the brightness of the CMB
	// (absolute, from Planck's law) to the brightness of the starfield (arbitrary units).
	constexpr double REST_STARLIGHT_FLUX = 3.63e-11 * 17.38;
	// used when the starfield is turned off
	constexpr double DEFAULT_REST_STARLIGHT = 1000.0;

	// eye adaptation: time constants (real seconds) for adapting to a brighter / darker view
	constexpr double ADAPT_TO_BRIGHTER = 0.1;
	constexpr double ADAPT_TO_DARKER = 0.5;

	// stars within this angle of the direction of travel feed the glow
	constexpr double KNOT_RADIUS = 3.0 * M_PI / 180.0;
	// light below this (pixel units), on top of what the stars ahead give at rest, does not bloom
	constexpr double GLOW_THRESHOLD = 20.0;
	// halo: fractions of the light scattered into the inner and outer halo, and their radii in pixels
	constexpr float GLOW_INNER_FRACTION = 0.10f;
	constexpr float GLOW_INNER_RADIUS = 2.5f;
	constexpr float GLOW_OUTER_FRACTION = 0.04f;
	constexpr float GLOW_OUTER_RADIUS = 25.0f;
	constexpr float GLOW_QUAD_RADIUS = 8.0f * GLOW_OUTER_RADIUS;

	// a star sprite grows by at most this factor to show light it cannot show as brightness
	constexpr float MAX_SPRITE_GROWTH = 8.0f;

	const vector3d LUMA(0.2126, 0.7152, 0.0722);

	vector3d Multiply(const vector3d &a, const vector3d &b) { return vector3d(a.x * b.x, a.y * b.y, a.z * b.z); }

	// Matches GlowData in data/shaders/opengl/hyperspace_glow.frag
	struct GlowData {
		vector3f axis;
		float pixelAngle;
		vector3f flux;
		float quadRadius;
		float innerRadius;
		float innerFraction;
		float outerRadius;
		float outerFraction;
	};
	static_assert(sizeof(GlowData) == 48, "GlowData must match the std140 layout of the shader");
	static_assert(sizeof(Background::RelativisticView) == 64, "RelativisticView must match the std140 layout of the shader");
} // namespace

HyperspaceSky::HyperspaceSky(Graphics::Renderer *renderer, RefCountedPtr<Galaxy> galaxy, const SystemPath &source, const SystemPath &dest) :
	m_renderer(renderer),
	m_logExposure(0.0),
	m_exposureValid(false)
{
	PROFILE_SCOPED()
	m_departure = CreateSystemBackground(renderer, galaxy, source.SystemOnly());
	m_arrival = CreateSystemBackground(renderer, galaxy, dest.SystemOnly());

	Graphics::RenderStateDesc rsd;
	rsd.blendMode = Graphics::BLEND_ADDITIVE;
	rsd.cullMode = Graphics::CULL_NONE;
	rsd.depthTest = false;
	rsd.depthWrite = false;
	const auto vtxFormat = Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION);
	m_glowMaterial.reset(renderer->CreateMaterial("hyperspace_glow", Graphics::MaterialDescriptor(), rsd, vtxFormat));
}

HyperspaceSky::~HyperspaceSky()
{
}

std::unique_ptr<Background::Container> HyperspaceSky::CreateSystemBackground(Graphics::Renderer *renderer, RefCountedPtr<Galaxy> galaxy, const SystemPath &path)
{
	Uint32 _init[5] = { path.systemIndex, Uint32(path.sectorX), Uint32(path.sectorY), Uint32(path.sectorZ), UNIVERSE_SEED };
	Random rand(_init, 5);
	std::unique_ptr<Background::Container> background(new Background::Container(renderer, rand));
	background->GetStarfield()->Fill(rand, &path, galaxy);
	return background;
}

Background::Container *HyperspaceSky::GetBackground()
{
	// the ship flips halfway through the voyage: from then on it is coming to rest at its destination
	return Pi::game->GetHyperspaceTransitProgress() < Relativity::FLIP_PROGRESS ? m_departure.get() : m_arrival.get();
}

HyperspaceSky::Starlight HyperspaceSky::EstimateStarlight(const Background::Starfield &starfield, const vector3d &travelDir, double gamma, double oneMinusBeta) const
{
	Starlight light = { vector3d(0.0), vector3d(0.0), 0.0, 0.0 };
	const double knotCos = std::cos(KNOT_RADIUS);

	for (const Background::StarSample &star : starfield.GetSamples()) {
		const vector3d dir(star.dir);
		const double cosTheta = dir.Dot(travelDir);
		const double onePlusCos = 0.5 * (dir + travelDir).LengthSqr();

		// Doppler factor and observed angle, as in relativity.glsl
		const double doppler = std::max(gamma * (onePlusCos - oneMinusBeta * cosTheta), 1e-12);
		const double cosSeen = (onePlusCos - oneMinusBeta) / (onePlusCos - oneMinusBeta * cosTheta);

		vector3d flux;
		for (int c = 0; c < 3; c++)
			flux[c] = star.linearColor[c] * Relativity::PlanckRatio(Relativity::CHANNEL_WAVELENGTH[c], star.temperature * doppler, star.temperature);
		flux *= star.area / (doppler * doppler);

		light.total += flux;
		if (cosSeen > knotCos)
			light.knot += flux;

		const double restLuma = vector3d(star.linearColor).Dot(LUMA) * star.area;
		light.atRest += restLuma;
		if (cosTheta > knotCos)
			light.knotAtRest += restLuma;
	}

	const double weight = starfield.GetSampleWeight();
	light.total *= weight;
	light.knot *= weight;
	light.atRest *= weight;
	light.knotAtRest *= weight;
	return light;
}

void HyperspaceSky::Draw(const matrix4x4d &transform)
{
	PROFILE_SCOPED()
	const double progress = Pi::game->GetHyperspaceTransitProgress();
	const Relativity::Kinematics k = Relativity::FromTransitProgress(progress);
	const vector3d travelDir = Pi::game->GetHyperspaceTravelDir();
	Background::Container *sky = GetBackground();

	// angular size of a pixel at the centre of the view
	const matrix4x4f proj = m_renderer->GetProjection();
	const double viewportHeight = std::max(1, int(m_renderer->GetViewport().h));
	const double pixelAngle = 2.0 / (std::abs(proj[5]) * viewportHeight);
	const double pixelSolidAngle = pixelAngle * pixelAngle;
	// a disc of one pixel's area
	const double coreRadius = pixelAngle / std::sqrt(M_PI);

	// how bright everything is, in the units of the starfield
	const Starlight stars = EstimateStarlight(*sky->GetStarfield(), travelDir, k.gamma, k.oneMinusBeta);
	const double restStarlight = stars.atRest > 0.0 ? stars.atRest : DEFAULT_REST_STARLIGHT;
	const vector3d toDisplay = Relativity::WhiteBalance() * (restStarlight / REST_STARLIGHT_FLUX);
	const vector3d cmbTotal = Multiply(Relativity::CmbFluxInCone(k, M_PI), toDisplay);
	const vector3d cmbCore = Multiply(Relativity::CmbFluxInCone(k, coreRadius), toDisplay);

	// eye adaptation: keep the total light at what the sky gives at rest, never brighter
	const double total = (stars.total + cmbTotal).Dot(LUMA);
	const double target = std::log(Clamp(restStarlight / std::max(total, 1e-30), 1e-30, 1.0));
	if (!m_exposureValid) {
		m_logExposure = target;
		m_exposureValid = true;
	} else {
		const double timeConstant = target < m_logExposure ? ADAPT_TO_BRIGHTER : ADAPT_TO_DARKER;
		m_logExposure += (target - m_logExposure) * (1.0 - std::exp(-Pi::GetFrameTime() / timeConstant));
	}
	const double exposure = std::exp(m_logExposure);

	Background::RelativisticView view = {};
	view.axis = vector3f(transform.ApplyRotationOnly(travelDir).Normalized());
	view.beta = float(k.beta);
	view.gamma = float(k.gamma);
	view.oneMinusBeta = float(k.oneMinusBeta);
	view.exposure = float(exposure);
	view.cmbScale = float(pixelSolidAngle * toDisplay.y * exposure);
	view.cmbTemperature = float(Relativity::CMB_TEMPERATURE);
	view.coreRadius = float(coreRadius);
	view.maxSpriteGrowth = MAX_SPRITE_GROWTH;
	view.cmbCore = vector3f(cmbCore * exposure);
	sky->DrawRelativistic(transform, view);

	// only light far too bright for the screen blooms, so that the sky at rest is left untouched
	const vector3d glow = (stars.knot + cmbTotal) * exposure;
	const double glowLuma = glow.Dot(LUMA);
	const double glowThreshold = GLOW_THRESHOLD + 2.0 * stars.knotAtRest;
	if (glowLuma > glowThreshold)
		DrawGlow(view.axis, float(pixelAngle), vector3f(glow * (1.0 - glowThreshold / glowLuma)));
}

void HyperspaceSky::DrawGlow(const vector3f &axis, float pixelAngle, const vector3f &flux)
{
	// the camera looks down -z: skip the glow when it is behind the camera
	if (axis.z > 0.0f)
		return;

	// a quad facing the camera, centred on the direction of travel, given in view space
	constexpr float distance = 100.0f;
	const float halfSize = distance * std::tan(std::min(GLOW_QUAD_RADIUS * pixelAngle, 1.2f));
	vector3f right = axis.Cross(vector3f(0.0f, 1.0f, 0.0f));
	right = right.LengthSqr() > 1e-6f ? right.Normalized() : vector3f(1.0f, 0.0f, 0.0f);
	const vector3f up = right.Cross(axis);

	const vector3f centre = axis * distance;
	const vector3f a = centre + (-right - up) * halfSize;
	const vector3f b = centre + (right - up) * halfSize;
	const vector3f c = centre + (right + up) * halfSize;
	const vector3f d = centre + (-right + up) * halfSize;

	Graphics::VertexArray quad(Graphics::ATTRIB_POSITION, 6);
	quad.Add(a);
	quad.Add(b);
	quad.Add(c);
	quad.Add(a);
	quad.Add(c);
	quad.Add(d);

	GlowData data;
	data.axis = axis;
	data.pixelAngle = pixelAngle;
	data.flux = flux;
	data.quadRadius = GLOW_QUAD_RADIUS;
	data.innerRadius = GLOW_INNER_RADIUS;
	data.innerFraction = GLOW_INNER_FRACTION;
	data.outerRadius = GLOW_OUTER_RADIUS;
	data.outerFraction = GLOW_OUTER_FRACTION;
	m_glowMaterial->SetBufferDynamic("GlowData"_hash, &data);

	m_renderer->SetTransform(matrix4x4f::Identity);
	m_renderer->DrawBuffer(&quad, m_glowMaterial.get());
}
