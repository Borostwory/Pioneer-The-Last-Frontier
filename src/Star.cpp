// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Star.h"

#include "Background.h"
#include "Camera.h"
#include "Game.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Random.h"
#include "Space.h"
#include "core/Log.h"
#include "core/StringHash.h"
#include "galaxy/StarSystem.h"
#include "galaxy/SystemBody.h"
#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/RenderTarget.h"
#include "graphics/Renderer.h"
#include "graphics/RenderState.h"
#include "graphics/Texture.h"
#include "graphics/Types.h"
#include "graphics/VertexArray.h"
#include "graphics/VertexBuffer.h"

namespace {
	// length of each pulsar jet: 0.08 AU, so arriving from hyperspace (about 0.13 AU out) they fill much of the view
	const double PULSAR_JET_LENGTH = 0.08 * AU;
	// half the width of the strip the jets are drawn on, as a fraction of their length; it leaves room for the twist
	constexpr double PULSAR_JET_HALF_WIDTH = 0.2;
	// radius of the glow quad, as a fraction of the jet length; the flare line runs out to its edge
	constexpr double PULSAR_GLOW_SIZE = 0.5;
	// spin period used when a neutron star has none set, in seconds (the Vela pulsar's)
	constexpr double PULSAR_DEFAULT_PERIOD = 0.0893;
	// The beams come out of the magnetic poles, which are tilted from the spin axis, so each beam sweeps round a
	// cone once a turn, like a lighthouse. Each pulsar's tilt is picked from this range, in degrees (Vela's is
	// thought to be about 40 to 70)
	constexpr double PULSAR_MAGNETIC_TILT_MIN = 15.0;
	constexpr double PULSAR_MAGNETIC_TILT_MAX = 60.0;
	// how long the eye holds a picture, in seconds: a beam that sweeps round in less than this is seen as a
	// glowing hollow cone, a slower one as a beam with a fading trail behind it
	constexpr double PULSAR_EXPOSURE = 0.1;
	// half the width of a beam, as an angle seen from the star, in radians (about how far the jet strands spread)
	constexpr double PULSAR_BEAM_HALF_ANGLE = 0.06;
	// the trail behind a beam fades by e^-3 from the beam to its end
	constexpr double PULSAR_TRAIL_FADE = 3.0;
	// the gas streaming out along the jets wraps round after this many seconds
	constexpr double PULSAR_FLOW_WRAP = 3600.0;

	bool IsBlackHole(SystemBody::BodyType type)
	{
		return type == SystemBody::TYPE_STAR_S_BH || type == SystemBody::TYPE_STAR_IM_BH || type == SystemBody::TYPE_STAR_SM_BH;
	}

	// a black hole's Schwarzschild radius, 2GM/c^2, in metres
	double SchwarzschildRadius(const SystemBody *sbody)
	{
		return 2.0 * G * sbody->GetMass() / (LIGHT_SPEED * LIGHT_SPEED);
	}

	// How far round a black hole its light bending is drawn, in Schwarzschild radii. Its clip radius: the hole is drawn
	// (not culled, nor drawn as a dot) while this much of the sky around it is in view. The bending there is tiny: the
	// shader leaves out what moves less than a fraction of a pixel.
	constexpr double LENS_REACH = 1.0e4;
	// size of each face of the sky snapshot the light bending reads, in pixels
	constexpr Uint16 SKY_SIZE = 2048;

	// cubemap faces in OpenGL order (+X, -X, +Y, -Y, +Z, -Z): view direction and up vector,
	// as for the hyperspace snapshot (WorldView.cpp)
	const vector3d SKY_FACES[6][2] = {
		{ { 1, 0, 0 }, { 0, -1, 0 } },
		{ { -1, 0, 0 }, { 0, -1, 0 } },
		{ { 0, 1, 0 }, { 0, 0, 1 } },
		{ { 0, -1, 0 }, { 0, 0, -1 } },
		{ { 0, 0, 1 }, { 0, -1, 0 } },
		{ { 0, 0, -1 }, { 0, -1, 0 } },
	};

	// what the light bending needs, made once and kept for all black holes
	struct LensResources {
		bool tried = false;
		bool ready = false;
		std::unique_ptr<Graphics::Material> material;
		std::unique_ptr<Graphics::VertexArray> triangle;

		// the sky all round (stars, Milky Way, background), in the root frame's axes, and whose sky it is
		std::unique_ptr<Graphics::RenderTarget> skyTarget;
		RefCountedPtr<Graphics::Texture> skyCube;
		const Background::Container *skyOf = nullptr;
		SystemPath skyPath;
		bool haveSky = false;

		// the picture drawn so far (everything further away than the hole), to bend
		std::unique_ptr<Graphics::RenderTarget> sceneTarget;
		int sceneW = 0, sceneH = 0;
	};
	std::unique_ptr<LensResources> s_lens;

	bool EnsureLens(Graphics::Renderer *r)
	{
		if (!s_lens)
			s_lens.reset(new LensResources());
		LensResources &res = *s_lens;
		if (!res.tried) {
			res.tried = true;
			// one triangle covering the screen, as for the screen passes (ScreenEffects.cpp)
			res.triangle.reset(new Graphics::VertexArray(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0, 3));
			res.triangle->Add(vector3f(-1.0f, -1.0f, 0.0f), vector2f(0.0f, 0.0f));
			res.triangle->Add(vector3f(3.0f, -1.0f, 0.0f), vector2f(2.0f, 0.0f));
			res.triangle->Add(vector3f(-1.0f, 3.0f, 0.0f), vector2f(0.0f, 2.0f));

			Graphics::RenderStateDesc rsd;
			rsd.blendMode = Graphics::BLEND_SOLID;
			rsd.cullMode = Graphics::CULL_NONE;
			rsd.depthTest = false;
			rsd.depthWrite = false;
			const Graphics::VertexFormatDesc vfmt = Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0);
			res.material.reset(r->CreateMaterial("black_hole_lens", Graphics::MaterialDescriptor(), rsd, vfmt));

			res.skyTarget.reset(r->CreateRenderTarget({ SKY_SIZE, SKY_SIZE, Graphics::TEXTURE_NONE, Graphics::TEXTURE_NONE, false }));
			res.skyCube.Reset(r->CreateTexture(Graphics::TextureDescriptor(Graphics::TEXTURE_RGBA_8888,
				vector3f(SKY_SIZE, SKY_SIZE, 0.0f), vector2f(1.0f, 1.0f), Graphics::LINEAR_CLAMP,
				false, false, false, 0, Graphics::TEXTURE_CUBE_MAP)));

			// (waits for the shader if it's being compiled in the background)
			res.ready = res.material && r->WaitForMaterial(res.material.get()) && res.skyTarget && res.skyCube.Valid();
			if (!res.ready)
				Log::Warning("The black hole light bending couldn't be set up; black holes are drawn as plain black spheres.\n");
		}
		return res.ready;
	}

	// Draws the sky (and only the sky) all round into the snapshot: a 90 degree camera turned to face each side of the
	// cube in turn, in the root frame's axes
	void TakeSkySnapshot(Graphics::Renderer *r, Background::Container *background, float zNear)
	{
		LensResources &res = *s_lens;
		const float fov = Graphics::GetFov();

		RefCountedPtr<CameraContext> context(new CameraContext(SKY_SIZE, SKY_SIZE, 90.0f, zNear, 1.0e9f));

		// restores the render target, viewport and matrices
		Graphics::Renderer::StateTicket ticket(r);
		const Graphics::ViewportExtents extents = { 0, 0, SKY_SIZE, SKY_SIZE };
		for (Uint32 face = 0; face < 6; face++) {
			const vector3d &forward = SKY_FACES[face][0];
			const vector3d &up = SKY_FACES[face][1];

			res.skyTarget->SetCubeFaceTexture(face, res.skyCube.Get());
			r->SetRenderTarget(res.skyTarget.get());
			r->SetViewport(extents);
			r->ClearScreen(Color::BLACK, false);
			context->ApplyDrawTransforms(r);
			// root frame to view: the rows are the camera's axes (right, up, back)
			background->Draw(matrix4x4d::MakeRotMatrix(forward.Cross(up), up, -forward));
			// render this face before the next one is attached
			r->FlushCommandBuffers();
		}
		Graphics::SetFov(fov);
	}
} // namespace

void Star::UninitLens()
{
	s_lens.reset();
}

Star::Star(SystemBody *sbody) :
	TerrainBody(sbody)
{
	InitStar();
}

Star::Star(const Json &jsonObj, Space *space) :
	TerrainBody(jsonObj, space)
{
	InitStar();
}

Star::~Star()
{
}

void Star::InitStar()
{
	// this one should be atmosphere radius when stars have atmosphere
	SetPhysRadius(GetMaxFeatureRadius());

	// this one is much larger because stars have halo effect
	// if star is wolf-rayet it gets a very large halo effect
	const SystemBody *sbody = GetSystemBody();
	const float wf = (sbody->GetType() < SystemBody::TYPE_STAR_S_BH && sbody->GetType() > SystemBody::TYPE_STAR_O_HYPER_GIANT) ? 100.0f : 1.0f;
	SetClipRadius(sbody->GetRadius() * 8 * wf);

	m_visualProfile = GetVisualProfile(sbody);

	Graphics::MaterialDescriptor desc;

	Graphics::RenderStateDesc rsd;
	rsd.blendMode = Graphics::BLEND_ADDITIVE;
	rsd.depthWrite = false;
	rsd.depthTest = false; // drawn over the star's own disc (see Render)
	rsd.cullMode = Graphics::CULL_NONE; // the quad is turned to face the camera, whichever way it winds
	rsd.primitiveType = Graphics::TRIANGLE_STRIP;

	auto vtxFormat = Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0);
	m_haloMat.reset(Pi::renderer->CreateMaterial("star_halo", desc, rsd, vtxFormat));

	if (sbody->GetType() == SystemBody::TYPE_STAR_NEUTRON) {
		// the jets reach far beyond the tiny star, so it must not be culled or drawn as a dot while they're in view
		m_jetLength = PULSAR_JET_LENGTH;
		SetClipRadius(m_jetLength);
		m_pulsarJetMat.reset(Pi::renderer->CreateMaterial("pulsar_jet", desc, rsd, vtxFormat));
		m_pulsarGlowMat.reset(Pi::renderer->CreateMaterial("pulsar_glow", desc, rsd, vtxFormat));

		// the cone swept by the beams is a triangle mesh with normals
		Graphics::RenderStateDesc coneRsd = rsd;
		coneRsd.primitiveType = Graphics::TRIANGLES;
		auto coneFormat = Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_NORMAL | Graphics::ATTRIB_UV0);
		m_pulsarConeMat.reset(Pi::renderer->CreateMaterial("pulsar_cone", desc, coneRsd, coneFormat));

		// the angle between the magnetic poles and the spin axis, fixed for each pulsar
		Random rng({ sbody->GetSeed(), 0x5075u });
		m_magneticTilt = DEG2RAD(rng.Double(PULSAR_MAGNETIC_TILT_MIN, PULSAR_MAGNETIC_TILT_MAX));
	}

	if (IsBlackHole(sbody->GetType())) {
		// its light bending reaches far beyond its shadow
		SetClipRadius(std::max(sbody->GetRadius(), SchwarzschildRadius(sbody)) * LENS_REACH);
	}
}

Star::VisualProfile Star::GetVisualProfile(const SystemBody *sbody)
{
	const SystemBody::BodyType type = sbody->GetType();
	switch (type) {
	case SystemBody::TYPE_BROWN_DWARF:
		return { 20.f, 0.15f, 0.f, 0.6f, 0.2f, 0.2f, 0.f, 0.f };
	case SystemBody::TYPE_WHITE_DWARF:
		return { 90.f, 0.15f, 0.f, 0.2f, 1.5f, 0.2f, 0.f, 0.f };
	case SystemBody::TYPE_STAR_M_WF:
	case SystemBody::TYPE_STAR_B_WF:
	case SystemBody::TYPE_STAR_O_WF:
		return { 50.f, 0.3f, 0.f, 0.f, 1.0f, 0.5f, 0.f, 1.f };
	case SystemBody::TYPE_STAR_S_BH:
	case SystemBody::TYPE_STAR_IM_BH:
	case SystemBody::TYPE_STAR_SM_BH:
		return { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
	case SystemBody::TYPE_STAR_NEUTRON:
		// a smooth, searingly hot surface; its halo is drawn by RenderPulsar instead
		return { 120.f, 0.05f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
	default:
		break;
	}

	if (type < SystemBody::TYPE_STAR_M || type > SystemBody::TYPE_STAR_O_HYPER_GIANT)
		return { 40.f, 0.35f, 0.f, 0.25f, 0.5f, 0.6f, 0.f, 0.f };

	// the main sequence and giant types run M..O in blocks of seven
	const int spectral = (type - SystemBody::TYPE_STAR_M) % 7;
	const int size = (type - SystemBody::TYPE_STAR_M) / 7; // main sequence, giant, super giant, hyper giant

	static const VisualProfile mainSequence[7] = {
		// cells, contrast, spots, baked, corona, streaks, prominences, envelope
		{ 22.f, 0.45f, 0.70f, 0.30f, 0.5f, 0.6f, 0.8f, 0.f }, // M
		{ 30.f, 0.40f, 0.50f, 0.30f, 0.5f, 0.6f, 1.0f, 0.f }, // K
		{ 40.f, 0.35f, 0.35f, 0.25f, 0.5f, 0.6f, 1.0f, 0.f }, // G
		{ 50.f, 0.30f, 0.10f, 0.00f, 0.6f, 0.5f, 0.6f, 0.f }, // F
		{ 60.f, 0.22f, 0.00f, 0.00f, 0.6f, 0.5f, 0.5f, 0.f }, // A
		{ 70.f, 0.18f, 0.00f, 0.00f, 0.8f, 0.4f, 0.4f, 0.f }, // B
		{ 80.f, 0.15f, 0.00f, 0.00f, 0.8f, 0.4f, 0.4f, 0.f }, // O
	};
	// bigger stars have fewer, larger and more contrasted convection cells and a wider corona
	static const float sizeCells[4] = { 1.f, 0.5f, 0.35f, 0.25f };
	static const float sizeContrast[4] = { 1.f, 1.2f, 1.25f, 1.3f };
	static const float sizeCorona[4] = { 1.f, 1.4f, 1.8f, 2.2f };
	static const float sizeProminences[4] = { 1.f, 0.7f, 0.5f, 0.4f };

	VisualProfile p = mainSequence[spectral];
	p.granuleFrequency *= sizeCells[size];
	p.granuleContrast *= sizeContrast[size];
	p.coronaSize *= sizeCorona[size];
	p.prominences *= sizeProminences[size];
	return p;
}

vector3f Star::GetAnimationAngles()
{
	// real time, so time acceleration doesn't make the stars boil
	const double t = Pi::GetApp() ? Pi::GetApp()->GetTime() : 0.0;
	// periods in seconds: granule churn, large-scale flow, sunspot drift
	const double periods[3] = { 600.0, 1700.0, 20000.0 };
	float angles[3];
	for (int i = 0; i < 3; i++)
		angles[i] = float(std::fmod(t * 2.0 * M_PI / periods[i], 2.0 * M_PI));
	return vector3f(angles[0], angles[1], angles[2]);
}

void Star::BuildHaloBuffer(Graphics::Renderer *renderer)
{
	// a unit quad facing the camera, the shader draws the corona on it
	Graphics::VertexArray va(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0);
	va.Add(vector3f(-1.f, -1.f, 0.f), vector2f(-1.f, -1.f));
	va.Add(vector3f(1.f, -1.f, 0.f), vector2f(1.f, -1.f));
	va.Add(vector3f(-1.f, 1.f, 0.f), vector2f(-1.f, 1.f));
	va.Add(vector3f(1.f, 1.f, 0.f), vector2f(1.f, 1.f));

	m_haloMesh.reset(renderer->CreateMeshObjectFromArray(&va));
}

void Star::Render(Graphics::Renderer *renderer, const Camera *camera, const vector3d &viewCoords, const matrix4x4d &viewTransform)
{
	const bool pulsar = GetSystemBody()->GetType() == SystemBody::TYPE_STAR_NEUTRON;

	// a pulsar's clip radius covers its jets, so its glow gets its own size
	double rad = pulsar ? m_jetLength * PULSAR_GLOW_SIZE : GetClipRadius();
	double jetLength = m_jetLength;
	vector3d fpos = viewCoords;
	double len = fpos.Length();

	// where the star's limb falls on the halo quad (which sits in the plane through the star's centre),
	// as a fraction of the quad's radius
	const double starRadius = GetSystemBody()->GetRadius();
	const double sinLimb = std::min(starRadius / std::max(len, 1e-6), 0.999);
	const float limbFraction = float(std::min(starRadius / std::sqrt(1.0 - sinLimb * sinLimb) / rad, 1.0));

	while (len > 1000.0f) {
		rad *= 0.25;
		jetLength *= 0.25;
		fpos = 0.25 * fpos;
		len *= 0.25;
	}

	matrix4x4d trans = matrix4x4d::Identity;
	trans.Translate(float(fpos.x), float(fpos.y), float(fpos.z));

	// face the camera dammit
	vector3d zaxis = viewCoords.NormalizedSafe();
	vector3d xaxis = vector3d(0, 1, 0).Cross(zaxis).Normalized();
	vector3d yaxis = zaxis.Cross(xaxis);
	matrix4x4d rot = matrix4x4d::MakeRotMatrix(xaxis, yaxis, zaxis).Inverse();

	// Generate the halo if we don't have one
	if (!m_haloMesh) {
		BuildHaloBuffer(renderer);
	}

	const SystemBody::BodyType type = GetSystemBody()->GetType();

	// a black hole: the picture behind it bent round it, with its shadow in the middle. Its own black sphere is
	// smaller than the shadow, and drawn before, it would be bent into the picture as a black ring, so it's only
	// drawn if the bending can't be.
	if (IsBlackHole(type)) {
		if (!RenderBlackHoleLens(renderer, camera, viewCoords, viewTransform))
			TerrainBody::Render(renderer, camera, viewCoords, viewTransform);
		renderer->GetStats().AddToStatCount(Graphics::Stats::STAT_STARS, 1);
		return;
	}

	TerrainBody::Render(renderer, camera, viewCoords, viewTransform);

	// the halo goes over the star, so its glow can spill across the limb like bloom;
	// bodies in front of the star are drawn after it and still cover the glow
	if (pulsar) {
		RenderPulsar(renderer, fpos, trans * rot, rad, jetLength, viewTransform);
	} else if (type < SystemBody::TYPE_STAR_S_BH || type > SystemBody::TYPE_STAR_SM_BH) {
		const VisualProfile &vp = m_visualProfile;
		m_haloMat->SetPushConstant("StarColor"_hash, GetSystemBody()->GetStarColor());
		m_haloMat->SetPushConstant("StarHalo"_hash, vector3f(vp.coronaSize, vp.streaks, vp.prominences), vp.envelope);
		m_haloMat->SetPushConstant("StarHaloGeom"_hash, vector3f(limbFraction, 0.f, 0.f), 0.f);
		m_haloMat->SetPushConstant("StarTime"_hash, GetAnimationAngles(), 0.f);

		// scale the halo by the new radius from it's unit size
		renderer->SetTransform(matrix4x4f(trans * matrix4x4d::ScaleMatrix(rad) * rot));
		//render star halo
		renderer->DrawMesh(m_haloMesh.get(), m_haloMat.get());
	}

	renderer->GetStats().AddToStatCount(Graphics::Stats::STAT_STARS, 1);
}

void Star::RenderPulsar(Graphics::Renderer *renderer, const vector3d &fpos, const matrix4x4d &faceCamera,
	double glowRadius, double jetLength, const matrix4x4d &viewTransform)
{
	const SystemBody *sbody = GetSystemBody();

	// real time, like the rest of the star animation, so the spin keeps its true rate under time acceleration
	const double t = Pi::GetApp() ? Pi::GetApp()->GetTime() : 0.0;
	double period = sbody->GetRotationPeriod();
	if (!(period > 0.0))
		period = PULSAR_DEFAULT_PERIOD;
	// where the beams are round the spin axis: one turn per spin
	const double spinAngle = 2.0 * M_PI * std::fmod(t, period) / period;
	const float spin = float(spinAngle);
	const float flow = float(std::fmod(t, PULSAR_FLOW_WRAP));
	const Color starColor = sbody->GetStarColor();

	// How far the beams sweep while the eye holds a picture, and so how they are seen: a slow pulsar's beams with
	// a short trail, a fast one's as a glowing hollow cone (the same light spread round the whole turn).
	const double sweep = std::min(2.0 * M_PI * std::max(PULSAR_EXPOSURE, double(Pi::GetFrameTime())) / period, 4.0 * M_PI);
	const double arc = std::min(sweep, 2.0 * M_PI);
	// the trail fades behind the beam, until the beam laps it so often that the cone is lit evenly
	const double lapped = std::min(std::max((sweep - 2.0 * M_PI) / (2.0 * M_PI), 0.0), 1.0);
	const double fade = PULSAR_TRAIL_FADE * (1.0 - lapped * lapped * (3.0 - 2.0 * lapped));
	// how wide a beam is, as an angle round the cone
	const double tilt = m_magneticTilt;
	const double beamArc = 2.0 * PULSAR_BEAM_HALF_ANGLE / std::max(std::sin(tilt), 0.05);
	// the beam itself: as bright as the newest part of its trail would be, at most full brightness
	const double trailScale = fade > 1e-3 ? fade / (1.0 - std::exp(-fade)) : 1.0;
	const float beamWeight = float(std::min(1.0, trailScale * beamArc / arc));

	// The spin axis is tilted like a planet's (see Space.cpp, RotateX by the axial tilt). The beams come out of the
	// magnetic poles, tilted from the spin axis, and turn round it.
	const matrix3x3d spinFrame = matrix3x3d::RotateX(sbody->GetAxialTilt());
	const vector3d axisX = viewTransform.ApplyRotationOnly(spinFrame * vector3d(1.0, 0.0, 0.0)).NormalizedSafe();
	const vector3d axisY = viewTransform.ApplyRotationOnly(spinFrame * vector3d(0.0, 1.0, 0.0)).NormalizedSafe();
	const vector3d axisZ = viewTransform.ApplyRotationOnly(spinFrame * vector3d(0.0, 0.0, 1.0)).NormalizedSafe();
	const vector3d beam = (std::sin(tilt) * std::cos(spinAngle) * axisX + std::cos(tilt) * axisY +
		std::sin(tilt) * std::sin(spinAngle) * axisZ).NormalizedSafe();

	// the cone the beams sweep round, with their trail or their smear
	if (!m_pulsarConeMesh)
		BuildPulsarCone(renderer);
	const matrix4x4d coneTrans = matrix4x4d::Translation(fpos) *
		matrix4x4d::MakeRotMatrix(axisX, axisY, axisZ).Inverse() *
		matrix4x4d::ScaleMatrix(jetLength);
	m_pulsarConeMat->SetPushConstant("StarColor"_hash, starColor);
	m_pulsarConeMat->SetPushConstant("PulsarCone"_hash, vector3f(spin, float(arc), float(fade)), float(beamArc));
	m_pulsarConeMat->SetPushConstant("PulsarBeam"_hash, vector3f(beamWeight, float(sweep >= 2.0 * M_PI), 0.f), 0.f);
	renderer->SetTransform(matrix4x4f(coneTrans));
	renderer->DrawMesh(m_pulsarConeMesh.get(), m_pulsarConeMat.get());

	// the beams themselves: one strip through the star along the magnetic axis, turned about it to face the camera
	const vector3d toCamera = (-fpos).NormalizedSafe();
	vector3d side = beam.Cross(toCamera);
	if (side.LengthSqr() < 1e-12) // looking straight down the beam
		side = beam.Cross(std::abs(beam.x) < 0.9 ? vector3d(1.0, 0.0, 0.0) : vector3d(0.0, 1.0, 0.0));
	side = side.Normalized();
	const vector3d normal = side.Cross(beam);

	const matrix4x4d jetTrans = matrix4x4d::Translation(fpos) *
		matrix4x4d::MakeRotMatrix(side, beam, normal).Inverse() *
		matrix4x4d::ScaleMatrix(jetLength * PULSAR_JET_HALF_WIDTH, jetLength, 1.0);
	m_pulsarJetMat->SetPushConstant("StarColor"_hash, starColor);
	m_pulsarJetMat->SetPushConstant("PulsarTime"_hash, vector3f(spin, beamWeight, flow), float(PULSAR_JET_HALF_WIDTH));
	renderer->SetTransform(matrix4x4f(jetTrans));
	renderer->DrawMesh(m_haloMesh.get(), m_pulsarJetMat.get());

	const vector3f pulsarTime(spin, 0.f, flow);

	// the glow, drawn last so it sits over the roots of the jets
	m_pulsarGlowMat->SetPushConstant("StarColor"_hash, starColor);
	m_pulsarGlowMat->SetPushConstant("PulsarTime"_hash, pulsarTime, 0.f);
	renderer->SetTransform(matrix4x4f(faceCamera * matrix4x4d::ScaleMatrix(glowRadius)));
	renderer->DrawMesh(m_haloMesh.get(), m_pulsarGlowMat.get());
}

void Star::BuildPulsarCone(Graphics::Renderer *renderer)
{
	// Two cones with their tips at the star, opening round the spin axis (y) at the magnetic tilt: the north
	// beam's and, mirrored through the star, the south beam's. uv: x = distance from the star in jet lengths,
	// y = angle round the axis (radians), the same for both, as the south beam is always opposite the north one.
	const int rings = 16;
	const int segments = 128;
	const float sinT = float(std::sin(m_magneticTilt));
	const float cosT = float(std::cos(m_magneticTilt));

	Graphics::VertexArray va(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_NORMAL | Graphics::ATTRIB_UV0, 2 * rings * segments * 6);
	auto vertex = [&](float sign, int ring, int seg) {
		const float s = float(ring) / float(rings);
		const float a = float(2.0 * M_PI) * float(seg) / float(segments);
		const vector3f dir(sinT * std::cos(a), cosT, sinT * std::sin(a));
		const vector3f n(cosT * std::cos(a), -sinT, cosT * std::sin(a));
		va.Add(sign * s * dir, sign * n, vector2f(s, a));
	};
	for (float sign : { 1.f, -1.f }) {
		for (int r = 0; r < rings; r++) {
			for (int g = 0; g < segments; g++) {
				vertex(sign, r, g);
				vertex(sign, r + 1, g);
				vertex(sign, r + 1, g + 1);
				vertex(sign, r, g);
				vertex(sign, r + 1, g + 1);
				vertex(sign, r, g + 1);
			}
		}
	}

	m_pulsarConeMesh.reset(renderer->CreateMeshObjectFromArray(&va));
}

bool Star::RenderBlackHoleLens(Graphics::Renderer *renderer, const Camera *camera, const vector3d &viewCoords,
	const matrix4x4d &viewTransform)
{
	if (!camera || !Pi::game || !Pi::game->GetSpace())
		return false;
	if (!EnsureLens(renderer))
		return false;
	LensResources &res = *s_lens;

	const double rs = SchwarzschildRadius(GetSystemBody());
	const double distance = viewCoords.Length();
	if (!(rs > 0.0) || !(distance > 0.0))
		return false;

	// the camera's projection: only its scale is used (no off-centre views)
	const matrix4x4f proj = renderer->GetProjection();
	if (!(proj[0] > 0.0f) || !(proj[5] > 0.0f))
		return false;

	// the sky snapshot, taken again in each new system
	Space *space = Pi::game->GetSpace();
	Background::Container *background = space->GetBackground();
	if (!background)
		return false;
	if (!space->GetStarSystem())
		return false;
	const SystemPath path = space->GetStarSystem()->GetPath();
	const bool sameSystem = res.haveSky && res.skyPath.HasValidSystem() && path.HasValidSystem() && res.skyPath.IsSameSystem(path);
	if (!sameSystem || res.skyOf != background) {
		TakeSkySnapshot(renderer, background, camera->GetContext()->GetZNear());
		res.skyOf = background;
		res.skyPath = path;
		res.haveSky = true;
	}

	// the picture drawn so far: the sky and everything further away than the hole, to bend.
	// What's nearer is drawn after, unbent, in front.
	Graphics::RenderTarget *target = renderer->GetRenderTarget();
	const int w = target ? target->GetDesc().width : renderer->GetWindowWidth();
	const int h = target ? target->GetDesc().height : renderer->GetWindowHeight();
	if (w <= 0 || h <= 0)
		return false;
	if (!res.sceneTarget || res.sceneW != w || res.sceneH != h) {
		res.sceneTarget.reset(renderer->CreateRenderTarget({ Uint16(w), Uint16(h), Graphics::TEXTURE_RGBA_8888, Graphics::TEXTURE_NONE, false }));
		res.sceneW = w;
		res.sceneH = h;
	}
	if (!res.sceneTarget)
		return false;

	// restores the render target, viewport and matrices
	Graphics::Renderer::StateTicket ticket(renderer);
	renderer->BeginGpuTimer("black hole");

	bool haveScene = false;
	if (target) {
		if (target->GetDesc().numSamples > 0)
			renderer->ResolveRenderTarget(target, res.sceneTarget.get(), { 0, 0, w, h });
		else
			renderer->CopyRenderTarget(target, res.sceneTarget.get(), { 0, 0, w, h }, { 0, 0, w, h }, false);
		haveScene = true;
	}

	Graphics::Material *m = res.material.get();
	// the hole's direction from the camera, and its distance in Schwarzschild radii
	m->SetPushConstant("LensHole"_hash, vector3f(viewCoords / distance), float(distance / rs));
	// x, y: the projection's scale; z: the angle one pixel spans (radians); w: whether the picture can be read
	const float pixelAngle = float(2.0 / (double(proj[5]) * double(h)));
	m->SetPushConstant("LensProj"_hash, vector3f(proj[0], proj[5], pixelAngle), haveScene ? 1.0f : 0.0f);
	// view to root frame (the snapshot's axes): the rows are the frame-to-view rotation's columns.
	// (Frames that don't rotate all share the root frame's axes.)
	m->SetPushConstant("SkyX"_hash, vector3f(vector3d(viewTransform[0], viewTransform[1], viewTransform[2]).Normalized()), 0.0f);
	m->SetPushConstant("SkyY"_hash, vector3f(vector3d(viewTransform[4], viewTransform[5], viewTransform[6]).Normalized()), 0.0f);
	m->SetPushConstant("SkyZ"_hash, vector3f(vector3d(viewTransform[8], viewTransform[9], viewTransform[10]).Normalized()), 0.0f);
	m->SetTexture("sceneTex"_hash, res.sceneTarget->GetColorTexture());
	m->SetTexture("skyTex"_hash, res.skyCube.Get());

	if (target) {
		renderer->SetRenderTarget(target);
		renderer->SetViewport({ 0, 0, w, h });
	}
	renderer->DrawBuffer(res.triangle.get(), m);

	renderer->EndGpuTimer();
	return true;
}
