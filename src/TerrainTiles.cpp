// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainTiles.h"

#include "GeoPatch.h"
#include "GeoPatchContext.h"
#include "GeoSphere.h"
#include "core/Log.h"
#include "core/StringHash.h"
#include "galaxy/SystemBody.h"
#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/RenderState.h"
#include "graphics/RenderTarget.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "graphics/Types.h"
#include "graphics/VertexArray.h"
#include "graphics/VertexBuffer.h"
#include "profiler/Profiler.h"

#include <algorithm>
#include <cmath>

namespace {
	std::unique_ptr<TerrainTiles> s_tiles;
	bool s_unsupported = false;

	double PositiveMod(const double v, const double p)
	{
		const double m = std::fmod(v, p);
		return m < 0.0 ? m + p : m;
	}
} // namespace

TerrainTiles *TerrainTiles::Get(Graphics::Renderer *r)
{
	if (!s_tiles && !s_unsupported) {
		s_tiles.reset(new TerrainTiles(r));
		if (!s_tiles->m_target) {
			Log::Warning("Terrain detail tiles need a {}x{} render target; using the generic relief instead.\n", ATLAS_SIZE, ATLAS_SIZE);
			s_tiles.reset();
			s_unsupported = true;
		}
	}
	return s_tiles.get();
}

void TerrainTiles::Destroy()
{
	if (s_tiles) {
		for (GeoPatch *owner : s_tiles->m_owners) {
			if (owner)
				owner->SetTileSlot(-1);
		}
	}
	s_tiles.reset();
}

void TerrainTiles::ReleaseSlot(const int slot)
{
	if (!s_tiles || slot < 0 || slot >= int(s_tiles->m_owners.size()) || !s_tiles->m_owners[slot])
		return;
	s_tiles->m_owners[slot] = nullptr;
	s_tiles->m_free.push_back(slot);
}

TerrainTiles::TerrainTiles(Graphics::Renderer *r)
{
	m_target.reset(r->CreateRenderTarget(Graphics::RenderTargetDesc(ATLAS_SIZE, ATLAS_SIZE, Graphics::TEXTURE_RGBA_8888, Graphics::TEXTURE_NONE, false, 0)));
	if (!m_target || !m_target->GetColorTexture()) {
		m_target.reset();
		return;
	}

	Graphics::RenderStateDesc rsd;
	rsd.blendMode = Graphics::BLEND_SOLID;
	rsd.cullMode = Graphics::CULL_NONE;
	rsd.depthTest = false;
	rsd.depthWrite = false;
	const Graphics::VertexFormatDesc vfmt = Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0);
	m_material.Reset(r->CreateMaterial("terrain_tile", Graphics::MaterialDescriptor(), rsd, vfmt));

	// full-screen triangle (positions in clip space), clipped to the slot by the viewport
	m_triangle.reset(new Graphics::VertexArray(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0, 3));
	m_triangle->Add(vector3f(-1.0f, -1.0f, 0.0f), vector2f(0.0f, 0.0f));
	m_triangle->Add(vector3f(3.0f, -1.0f, 0.0f), vector2f(2.0f, 0.0f));
	m_triangle->Add(vector3f(-1.0f, 3.0f, 0.0f), vector2f(0.0f, 2.0f));

	const int slots = SLOTS_PER_ROW * SLOTS_PER_ROW;
	m_owners.assign(slots, nullptr);
	m_lastSeen.assign(slots, 0);
	m_bakedBand.assign(slots, 0);
	m_free.reserve(slots);
	for (int i = slots - 1; i >= 0; i--)
		m_free.push_back(i);
}

Graphics::Texture *TerrainTiles::GetAtlas() const
{
	return m_target ? m_target->GetColorTexture() : nullptr;
}

vector3f TerrainTiles::SlotUV(const int slot) const
{
	const float x = float((slot % SLOTS_PER_ROW) * SLOT_STRIDE + TILE_BORDER);
	const float y = float((slot / SLOTS_PER_ROW) * SLOT_STRIDE + TILE_BORDER);
	return vector3f(x, y, float(TILE_SIZE)) / float(ATLAS_SIZE);
}

int TerrainTiles::Allocate(GeoPatch *patch)
{
	int slot = -1;
	if (!m_free.empty()) {
		slot = m_free.back();
		m_free.pop_back();
	} else {
		// take the tile of the patch seen longest ago, if it is out of view
		uint32_t oldest = m_frame;
		for (int i = 0; i < int(m_owners.size()); i++) {
			if (m_owners[i] && m_lastSeen[i] + 2 < m_frame && m_lastSeen[i] < oldest) {
				oldest = m_lastSeen[i];
				slot = i;
			}
		}
		if (slot < 0)
			return -1;
		m_owners[slot]->SetTileSlot(-1);
	}
	m_owners[slot] = patch;
	m_lastSeen[slot] = m_frame;
	patch->SetTileSlot(slot);
	return slot;
}

void TerrainTiles::Update(Graphics::Renderer *r, GeoSphere *sphere, const std::vector<std::pair<double, GeoPatch *>> &visible, const double pixelAngle)
{
	PROFILE_SCOPED()
	++m_frame;

	const double radius = sphere->GetSystemBody()->GetRadius();
	// the craters' finest octave follows the distance: bake again as it changes
	const bool rebake = sphere->GetTileStyle()[3] > 0.0f;

	// patches large enough on screen to show the landforms, and those whose
	// band no longer fits their distance: (squared distance, patch, band)
	struct Request {
		double distSqr;
		GeoPatch *patch;
		int band;
	};
	std::vector<Request> wanted, stale;
	for (const auto &v : visible) {
		GeoPatch *patch = v.second;
		const double dist = std::max(std::sqrt(v.first), 1e-9);
		const double footprint = dist * radius * pixelAngle;
		const int slot = patch->GetTileSlot();
		if (slot >= 0) {
			m_lastSeen[slot] = m_frame;
			// again once the band could be an octave finer (its finest
			// octave LANDFORM_PIXELS / 4 on screen), or is two too fine
			if (rebake) {
				const int band = BandIndex(sphere, patch, footprint);
				if (band < m_bakedBand[slot] || band > m_bakedBand[slot] + 1)
					stale.push_back({ v.first, patch, band });
			}
			continue;
		}
		// keep the ancestor's tile it is drawn with meanwhile
		vector3f part;
		const int ancestor = patch->FindTile(part);
		if (ancestor >= 0)
			m_lastSeen[ancestor] = m_frame;
		const double patchPixels = patch->GetVertexSpacing() / GeoPatchContext::GetFrac() / (dist * pixelAngle);
		if (patchPixels >= MIN_PATCH_PIXELS)
			wanted.push_back({ v.first, patch, BandIndex(sphere, patch, footprint) });
	}
	if (wanted.empty() && stale.empty())
		return;
	const auto nearest = [](const Request &a, const Request &b) { return a.distSqr < b.distSqr; };
	std::sort(wanted.begin(), wanted.end(), nearest);
	std::sort(stale.begin(), stale.end(), nearest);

	Graphics::Renderer::StateTicket ticket(r);
	r->SetRenderTarget(m_target.get());
	int baked = 0;
	for (const Request &b : wanted) {
		if (baked >= BAKES_PER_FRAME)
			break;
		const int slot = Allocate(b.patch);
		if (slot < 0)
			break;
		Bake(r, sphere, b.patch, slot, b.band);
		++baked;
	}
	for (const Request &b : stale) {
		if (baked >= BAKES_PER_FRAME)
			break;
		Bake(r, sphere, b.patch, b.patch->GetTileSlot(), b.band);
		++baked;
	}
}

// octaves of the detail: wavelengths on a fixed ladder (so neighbouring
// tiles agree), the band's longest twice the vertex spacing and at least
// LANDFORM_PIXELS on screen (so the landforms do not depend on the terrain
// detail distance), down to a quarter of that: at least two tile texels, as
// a tile has four texels per vertex gap
int TerrainTiles::BandIndex(const GeoSphere *sphere, const GeoPatch *patch, const double footprint)
{
	const double spacing = patch->GetVertexSpacing() * sphere->GetSystemBody()->GetRadius();
	const double ladderBase = sphere->GetDeepestVertexSpacing() * 0.5;
	const double largest = std::max(2.0 * spacing, LANDFORM_PIXELS * footprint);
	return int(std::round(std::log2(std::max(largest / ladderBase, 1.0))));
}

void TerrainTiles::Bake(Graphics::Renderer *r, GeoSphere *sphere, GeoPatch *patch, const int slot, const int band)
{
	const double radius = sphere->GetSystemBody()->GetRadius();
	m_bakedBand[slot] = band;

	// the patch centre, as the patch jobs place it (GetSpherePoint at 0.5, 0.5)
	const vector3d &v0 = patch->GetCorner(0), &v1 = patch->GetCorner(1), &v2 = patch->GetCorner(2), &v3 = patch->GetCorner(3);
	const vector3d centreDir = (v0 + 0.25 * (v1 - v0) + 0.25 * (v2 - v0) + 0.25 * (v3 - v0)).Normalized();
	const vector3d centre = centreDir * radius;

	// the band's octaves (BandIndex()), and the craters' above it up to their
	// largest size, the same for every tile
	const double ladderBase = sphere->GetDeepestVertexSpacing() * 0.5;
	const int finest = band - (BAND_OCTAVES - 1);
	const double finestWave = ladderBase * std::pow(2.0, double(finest));
	const float *style = sphere->GetTileStyle();
	int count = BAND_OCTAVES;
	if (style[3] > 0.0f) {
		const int top = int(std::floor(std::log2(std::max(radius * CRATER_TOP_PER_RADIUS / ladderBase, 1.0))));
		count = std::min(std::max(top - finest + 1, BAND_OCTAVES), MAX_OCTAVES);
	}

	const int x0 = (slot % SLOTS_PER_ROW) * SLOT_STRIDE;
	const int y0 = (slot / SLOTS_PER_ROW) * SLOT_STRIDE;
	r->SetViewport({ x0, y0, SLOT_STRIDE, SLOT_STRIDE });

	Graphics::Material *m = m_material.Get();
	m->SetPushConstant("TileSlot"_hash, vector3f(float(x0), float(y0), float(TILE_SIZE)), float(TILE_BORDER));
	const size_t cornerNames[4] = { "TileCorner0"_hash, "TileCorner1"_hash, "TileCorner2"_hash, "TileCorner3"_hash };
	for (int i = 0; i < 4; i++)
		m->SetPushConstant(cornerNames[i], vector3f(patch->GetCorner(i) * radius - centre), 0.0f);
	m->SetPushConstant("TileCentre"_hash, vector3f(centreDir), float(radius));
	m->SetPushConstant("TileOctaves"_hash, vector3f(float(finestWave), float(finest), float(BAND_OCTAVES)), float(count));
	// the patch centre in cells of the coarsest octave of each group of four,
	// modulo 256: a float holds it for the finer octaves of its group only
	const size_t originNames[4] = { "TileOrigin0"_hash, "TileOrigin1"_hash, "TileOrigin2"_hash, "TileOrigin3"_hash };
	for (int g = 0; g < 4; g++) {
		const double wave = finestWave * std::pow(2.0, double(std::min(4 * g + 3, count - 1)));
		m->SetPushConstant(originNames[g],
			vector3f(float(PositiveMod(centre.x / wave, 256.0)), float(PositiveMod(centre.y / wave, 256.0)), float(PositiveMod(centre.z / wave, 256.0))), 0.0f);
	}
	m->SetPushConstant("TileStyle"_hash, vector3f(style[0], style[1], style[2]), style[3]);
	r->DrawBuffer(m_triangle.get(), m);
}
