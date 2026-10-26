// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Star.h"

#include "Pi.h"
#include "core/StringHash.h"
#include "galaxy/StarSystem.h"
#include "galaxy/SystemBody.h"
#include "graphics/Material.h"
#include "graphics/Renderer.h"
#include "graphics/RenderState.h"
#include "graphics/Types.h"
#include "graphics/VertexArray.h"
#include "graphics/VertexBuffer.h"

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
	double rad = GetClipRadius();
	vector3d fpos = viewCoords;
	double len = fpos.Length();

	// where the star's limb falls on the halo quad (which sits in the plane through the star's centre),
	// as a fraction of the quad's radius
	const double starRadius = GetSystemBody()->GetRadius();
	const double sinLimb = std::min(starRadius / std::max(len, 1e-6), 0.999);
	const float limbFraction = float(std::min(starRadius / std::sqrt(1.0 - sinLimb * sinLimb) / rad, 1.0));

	while (len > 1000.0f) {
		rad *= 0.25;
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

	TerrainBody::Render(renderer, camera, viewCoords, viewTransform);

	// the halo goes over the star, so its glow can spill across the limb like bloom;
	// bodies in front of the star are drawn after it and still cover the glow
	// black holes have no glow
	const SystemBody::BodyType type = GetSystemBody()->GetType();
	if (type < SystemBody::TYPE_STAR_S_BH || type > SystemBody::TYPE_STAR_SM_BH) {
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
