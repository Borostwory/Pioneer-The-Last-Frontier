// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "ScreenEffects.h"

#include "GameConfig.h"
#include "MathUtil.h"
#include "Pi.h"
#include "core/Log.h"
#include "core/StringHash.h"
#include "graphics/Material.h"
#include "graphics/RenderState.h"
#include "graphics/RenderTarget.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "graphics/Types.h"
#include "graphics/VertexArray.h"
#include "graphics/VertexBuffer.h"

// SMAA's two lookup textures, as their authors made them (MIT licence, contrib/smaa/LICENSE.txt)
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"

#include <algorithm>
#include <initializer_list>
#include <memory>

namespace {
	const size_t s_sceneDepthName = "sceneDepth"_hash;
	const size_t s_sceneDistName = "sceneDist"_hash;
	const size_t s_creaseTexName = "creaseTex"_hash;
	const size_t s_creaseProjName = "CreaseProj"_hash;
	const size_t s_creaseScreenName = "CreaseScreen"_hash;
	const size_t s_creaseParamsName = "CreaseParams"_hash;
	const size_t s_colourTexName = "colourTex"_hash;
	const size_t s_edgesTexName = "edgesTex"_hash;
	const size_t s_areaTexName = "areaTex"_hash;
	const size_t s_searchTexName = "searchTex"_hash;
	const size_t s_blendTexName = "blendTex"_hash;
	const size_t s_edgeScreenName = "EdgeScreen"_hash;

	// the most the crease shade's reach may span on screen, as a share of the screen's height (so a wall right in front
	// of the camera doesn't send the passes reading all over the screen)
	constexpr double CREASE_MOST_SPAN = 0.08;

	// each pass's materials: made once, and left off for good if they didn't compile
	struct Pass {
		bool tried = false;
		bool ready = false;
	};

	struct Resources {
		std::unique_ptr<Graphics::VertexArray> triangle;

		Pass creases;
		RefCountedPtr<Graphics::Material> creaseDepth, creaseAo, creaseBlur, creaseApply;
		std::unique_ptr<Graphics::RenderTarget> distTarget; // full size, R32F distance in front of the camera
		std::unique_ptr<Graphics::RenderTarget> aoTarget, blurTarget; // half size, the shade in red
		int creaseW = 0, creaseH = 0;

		Pass fxaa, smaa;
		RefCountedPtr<Graphics::Material> fxaaMaterial, smaaEdges, smaaWeights, smaaBlend;
		RefCountedPtr<Graphics::Texture> areaTex, searchTex;
		std::unique_ptr<Graphics::RenderTarget> copyTarget;					 // full size, the picture as it was
		std::unique_ptr<Graphics::RenderTarget> edgesTarget, weightsTarget; // full size, SMAA's steps 1 and 2
		int copyW = 0, copyH = 0, smaaW = 0, smaaH = 0;
	};
	std::unique_ptr<Resources> s_res;

	Resources &GetResources()
	{
		if (!s_res) {
			s_res.reset(new Resources());
			// one triangle covering the screen (clip-space positions; 0..1 across the screen)
			s_res->triangle.reset(new Graphics::VertexArray(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0, 3));
			s_res->triangle->Add(vector3f(-1.0f, -1.0f, 0.0f), vector2f(0.0f, 0.0f));
			s_res->triangle->Add(vector3f(3.0f, -1.0f, 0.0f), vector2f(2.0f, 0.0f));
			s_res->triangle->Add(vector3f(-1.0f, 3.0f, 0.0f), vector2f(0.0f, 2.0f));
		}
		return *s_res;
	}

	Graphics::Material *MakeMaterial(Graphics::Renderer *r, const char *shader, Graphics::BlendMode blend)
	{
		Graphics::RenderStateDesc rsd;
		rsd.blendMode = blend;
		rsd.cullMode = Graphics::CULL_NONE;
		rsd.depthTest = false;
		rsd.depthWrite = false;
		const Graphics::VertexFormatDesc vfmt = Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0);
		return r->CreateMaterial(shader, Graphics::MaterialDescriptor(), rsd, vfmt);
	}

	// the picture's render target, if the passes can read it: not multisampled, and with the parts they need
	Graphics::RenderTarget *ReadableTarget(Graphics::Renderer *r, bool needDepth)
	{
		Graphics::RenderTarget *target = r->GetRenderTarget();
		if (!target || target->GetDesc().numSamples > 0 || !target->GetColorTexture())
			return nullptr;
		if (needDepth && !target->GetDepthTexture())
			return nullptr;
		return target;
	}

	std::unique_ptr<Graphics::RenderTarget> MakeTarget(Graphics::Renderer *r, int w, int h, Graphics::TextureFormat format)
	{
		const Graphics::RenderTargetDesc desc(Uint16(w), Uint16(h), format, Graphics::TEXTURE_NONE, false, 0);
		return std::unique_ptr<Graphics::RenderTarget>(r->CreateRenderTarget(desc));
	}

	bool EnsureCreases(Graphics::Renderer *r, int w, int h)
	{
		Resources &res = GetResources();
		if (!res.creases.tried) {
			res.creases.tried = true;
			res.creaseDepth.Reset(MakeMaterial(r, "crease_depth", Graphics::BLEND_SOLID));
			res.creaseAo.Reset(MakeMaterial(r, "crease_ao", Graphics::BLEND_SOLID));
			res.creaseBlur.Reset(MakeMaterial(r, "crease_blur", Graphics::BLEND_SOLID));
			// (black over the picture by 1 - the shade: the picture times the shade)
			res.creaseApply.Reset(MakeMaterial(r, "crease_apply", Graphics::BLEND_ALPHA));
			res.creases.ready = r->MaterialCompiled(res.creaseDepth.Get()) && r->MaterialCompiled(res.creaseAo.Get()) &&
				r->MaterialCompiled(res.creaseBlur.Get()) && r->MaterialCompiled(res.creaseApply.Get());
			if (!res.creases.ready)
				Log::Warning("The crease shading's shaders didn't compile; it stays off.\n");
		}
		if (!res.creases.ready)
			return false;

		if (res.creaseW != w || res.creaseH != h || !res.distTarget) {
			const int hw = (w + 1) / 2, hh = (h + 1) / 2;
			res.distTarget = MakeTarget(r, w, h, Graphics::TEXTURE_R32F);
			res.aoTarget = MakeTarget(r, hw, hh, Graphics::TEXTURE_RGBA_8888);
			res.blurTarget = MakeTarget(r, hw, hh, Graphics::TEXTURE_RGBA_8888);
			res.creaseW = w;
			res.creaseH = h;
		}
		return res.distTarget && res.aoTarget && res.blurTarget;
	}

	bool EnsureCopy(Graphics::Renderer *r, int w, int h)
	{
		Resources &res = GetResources();
		if (res.copyW != w || res.copyH != h || !res.copyTarget) {
			res.copyTarget = MakeTarget(r, w, h, Graphics::TEXTURE_RGBA_8888);
			res.copyW = w;
			res.copyH = h;
		}
		return bool(res.copyTarget);
	}

	bool EnsureFxaa(Graphics::Renderer *r)
	{
		Resources &res = GetResources();
		if (!res.fxaa.tried) {
			res.fxaa.tried = true;
			res.fxaaMaterial.Reset(MakeMaterial(r, "fxaa", Graphics::BLEND_SOLID));
			res.fxaa.ready = r->MaterialCompiled(res.fxaaMaterial.Get());
			if (!res.fxaa.ready)
				Log::Warning("The edge smoothing's FXAA shader didn't compile; it stays off.\n");
		}
		return res.fxaa.ready;
	}

	bool EnsureSmaa(Graphics::Renderer *r, int w, int h)
	{
		Resources &res = GetResources();
		if (!res.smaa.tried) {
			res.smaa.tried = true;
			res.smaaEdges.Reset(MakeMaterial(r, "smaa_edges", Graphics::BLEND_SOLID));
			res.smaaWeights.Reset(MakeMaterial(r, "smaa_weights", Graphics::BLEND_SOLID));
			res.smaaBlend.Reset(MakeMaterial(r, "smaa_blend", Graphics::BLEND_SOLID));
			res.smaa.ready = r->MaterialCompiled(res.smaaEdges.Get()) && r->MaterialCompiled(res.smaaWeights.Get()) &&
				r->MaterialCompiled(res.smaaBlend.Get());
			if (res.smaa.ready) {
				// the lookup textures, their bytes as they come, first row first (smaa.glsl): the area read smoothly,
				// the search texel by texel
				const Graphics::TextureDescriptor areaDesc(Graphics::TEXTURE_RG_88, vector3f(float(AREATEX_WIDTH), float(AREATEX_HEIGHT), 0.0f),
					Graphics::LINEAR_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
				res.areaTex.Reset(r->CreateTexture(areaDesc));
				const Graphics::TextureDescriptor searchDesc(Graphics::TEXTURE_R8, vector3f(float(SEARCHTEX_WIDTH), float(SEARCHTEX_HEIGHT), 0.0f),
					Graphics::NEAREST_CLAMP, false, false, false, 0, Graphics::TEXTURE_2D);
				res.searchTex.Reset(r->CreateTexture(searchDesc));
				res.smaa.ready = res.areaTex.Valid() && res.searchTex.Valid();
				if (res.smaa.ready) {
					res.areaTex->Update(areaTexBytes, areaDesc.dataSize, Graphics::TEXTURE_RG_88);
					res.searchTex->Update(searchTexBytes, searchDesc.dataSize, Graphics::TEXTURE_R8);
					res.smaaWeights->SetTexture(s_areaTexName, res.areaTex.Get());
					res.smaaWeights->SetTexture(s_searchTexName, res.searchTex.Get());
				}
			}
			if (!res.smaa.ready)
				Log::Warning("The edge smoothing's SMAA shaders didn't compile; it stays off.\n");
		}
		if (!res.smaa.ready)
			return false;

		if (res.smaaW != w || res.smaaH != h || !res.edgesTarget) {
			res.edgesTarget = MakeTarget(r, w, h, Graphics::TEXTURE_RGBA_8888);
			res.weightsTarget = MakeTarget(r, w, h, Graphics::TEXTURE_RGBA_8888);
			res.smaaW = w;
			res.smaaH = h;
		}
		return res.edgesTarget && res.weightsTarget;
	}
} // namespace

bool ScreenEffects::CreasesEnabled()
{
	return Pi::config->Int("CreaseShade", 0) != 0;
}

double ScreenEffects::CreaseStrength()
{
	return Clamp(double(Pi::config->Float("CreaseShadeStrength", 4.0f)), 0.0, 4.0);
}

double ScreenEffects::CreaseReach()
{
	return Clamp(double(Pi::config->Float("CreaseShadeReach", 4.0f)), 0.1, 4.0);
}

double ScreenEffects::CreaseVacuumShare()
{
	return Clamp(double(Pi::config->Float("CreaseShadeVacuum", 0.5f)), 0.0, 1.0);
}

int ScreenEffects::EdgeSmoothingMode()
{
	return Clamp(Pi::config->Int("EdgeSmoothing", 2), 0, 2);
}

bool ScreenEffects::CreasesFailed()
{
	return s_res && s_res->creases.tried && !s_res->creases.ready;
}

bool ScreenEffects::EdgeSmoothingFailed(int mode)
{
	if (!s_res)
		return false;
	const Pass &pass = mode == 1 ? s_res->fxaa : s_res->smaa;
	return mode != 0 && pass.tried && !pass.ready;
}

void ScreenEffects::DrawCreases(Graphics::Renderer *r, double air)
{
	if (!CreasesEnabled())
		return;
	Graphics::RenderTarget *target = ReadableTarget(r, true);
	if (!target)
		return;
	const int w = target->GetDesc().width, h = target->GetDesc().height;
	// the camera's projection: infinite reverse-Z, depth = near plane / distance (cloud_depth.frag)
	const matrix4x4f proj = r->GetProjection();
	if (!(proj[14] > 0.0f) || !(proj[0] > 0.0f) || !(proj[5] > 0.0f))
		return;
	if (!EnsureCreases(r, w, h))
		return;
	Resources &res = GetResources();

	// the strength: less without air, where the sky lights nothing and what's in sunlight stays bright in a crease
	const double vacuum = CreaseVacuumShare();
	const float strength = float(CreaseStrength() * (vacuum + (1.0 - vacuum) * Clamp(air, 0.0, 1.0)));
	if (strength <= 0.0f)
		return;
	const int hw = (w + 1) / 2, hh = (h + 1) / 2;
	const vector3f projData{ proj[0], proj[5], proj[14] };
	const vector3f screen{ float(w), float(h), float(hw) };
	const vector3f params{ float(CreaseReach()), strength, float(CREASE_MOST_SPAN * h) };
	for (Graphics::Material *m : { res.creaseDepth.Get(), res.creaseAo.Get(), res.creaseBlur.Get(), res.creaseApply.Get() }) {
		m->SetPushConstant(s_creaseProjName, projData, 0.0f);
		m->SetPushConstant(s_creaseScreenName, screen, float(hh));
		m->SetPushConstant(s_creaseParamsName, params, 0.0f);
	}

	// restores the render target, viewport and matrices
	Graphics::Renderer::StateTicket ticket(r);
	r->BeginGpuTimer("creases");

	// 1. the scene's distance, so the steps after don't read the depth buffer of the picture they draw into
	r->SetRenderTarget(res.distTarget.get());
	r->SetViewport({ 0, 0, w, h });
	res.creaseDepth->SetTexture(s_sceneDepthName, target->GetDepthTexture());
	r->DrawBuffer(res.triangle.get(), res.creaseDepth.Get());

	// 2. the shade, at half size
	r->SetRenderTarget(res.aoTarget.get());
	r->SetViewport({ 0, 0, hw, hh });
	res.creaseAo->SetTexture(s_sceneDistName, res.distTarget->GetColorTexture());
	r->DrawBuffer(res.triangle.get(), res.creaseAo.Get());

	// 3. evened out
	r->SetRenderTarget(res.blurTarget.get());
	r->SetViewport({ 0, 0, hw, hh });
	res.creaseBlur->SetTexture(s_sceneDistName, res.distTarget->GetColorTexture());
	res.creaseBlur->SetTexture(s_creaseTexName, res.aoTarget->GetColorTexture());
	r->DrawBuffer(res.triangle.get(), res.creaseBlur.Get());

	// 4. over the picture
	r->SetRenderTarget(target);
	r->SetViewport({ 0, 0, w, h });
	res.creaseApply->SetTexture(s_sceneDistName, res.distTarget->GetColorTexture());
	res.creaseApply->SetTexture(s_creaseTexName, res.blurTarget->GetColorTexture());
	r->DrawBuffer(res.triangle.get(), res.creaseApply.Get());

	r->EndGpuTimer();
}

void ScreenEffects::DrawEdgeSmoothing(Graphics::Renderer *r)
{
	const int mode = EdgeSmoothingMode();
	if (mode == 0)
		return;
	Graphics::RenderTarget *target = ReadableTarget(r, false);
	if (!target)
		return;
	const int w = target->GetDesc().width, h = target->GetDesc().height;
	if (mode == 1 ? !EnsureFxaa(r) : !EnsureSmaa(r, w, h))
		return;
	if (!EnsureCopy(r, w, h))
		return;
	Resources &res = GetResources();
	const vector3f screen{ 1.0f / float(w), 1.0f / float(h), float(w) };

	// restores the render target, viewport and matrices
	Graphics::Renderer::StateTicket ticket(r);
	r->BeginGpuTimer("edge smoothing");

	// the picture as it is, to read from, as the last step draws into it
	r->CopyRenderTarget(target, res.copyTarget.get(), { 0, 0, w, h }, { 0, 0, w, h }, false);
	Graphics::Texture *picture = res.copyTarget->GetColorTexture();

	if (mode == 1) {
		res.fxaaMaterial->SetPushConstant(s_edgeScreenName, screen, float(h));
		res.fxaaMaterial->SetTexture(s_colourTexName, picture);
		r->SetRenderTarget(target);
		r->SetViewport({ 0, 0, w, h });
		r->DrawBuffer(res.triangle.get(), res.fxaaMaterial.Get());
	} else {
		for (Graphics::Material *m : { res.smaaEdges.Get(), res.smaaWeights.Get(), res.smaaBlend.Get() })
			m->SetPushConstant(s_edgeScreenName, screen, float(h));

		// 1. the edges (pixels without one are left out, so cleared first)
		r->SetRenderTarget(res.edgesTarget.get());
		r->SetViewport({ 0, 0, w, h });
		r->ClearScreen(Color(0, 0, 0, 0), false);
		res.smaaEdges->SetTexture(s_colourTexName, picture);
		r->DrawBuffer(res.triangle.get(), res.smaaEdges.Get());

		// 2. how much each pixel on an edge blends
		r->SetRenderTarget(res.weightsTarget.get());
		r->SetViewport({ 0, 0, w, h });
		r->ClearScreen(Color(0, 0, 0, 0), false);
		res.smaaWeights->SetTexture(s_edgesTexName, res.edgesTarget->GetColorTexture());
		r->DrawBuffer(res.triangle.get(), res.smaaWeights.Get());

		// 3. the blending, into the picture
		r->SetRenderTarget(target);
		r->SetViewport({ 0, 0, w, h });
		res.smaaBlend->SetTexture(s_colourTexName, picture);
		res.smaaBlend->SetTexture(s_blendTexName, res.weightsTarget->GetColorTexture());
		r->DrawBuffer(res.triangle.get(), res.smaaBlend.Get());
	}

	r->EndGpuTimer();
}

void ScreenEffects::Uninit()
{
	s_res.reset();
}
