// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "GeoPatch.h"

#include "Aabb.h"

#include "FrontierLog.h"
#include "GeoPatchContext.h"
#include "GeoPatchJobs.h"
#include "GeoSphere.h"
#include "MathUtil.h"
#include "Pi.h"
#include "RefCounted.h"
#include "galaxy/SystemBody.h"
#include "graphics/Frustum.h"
#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/Renderer.h"
#include "graphics/Types.h"
#include "graphics/VertexBuffer.h"
#include "perlin.h"
#include "TerrainSurfaceTiles.h"
#include "TerrainTiles.h"
#include "profiler/Profiler.h"
#include "vcacheopt/vcacheopt.h"

#include <algorithm>
#include <deque>
#include <sstream>

#define DEBUG_CENTROIDS 0

#if DEBUG_PATCHES
#ifdef DEBUG_BOUNDING_SPHERES
#include "graphics/RenderState.h"
#endif

#include "graphics/Drawables.h"
#endif

namespace PatchMaths{
	// in patch surface coords, [0,1]
	inline vector3d GetSpherePoint(const vector3d &v0, const vector3d &v1, const vector3d &v2, const vector3d &v3, const double x, const double y)
	{
		return (v0 + x * (1.0 - y) * (v1 - v0) + x * y * (v2 - v0) + (1.0 - x) * y * (v3 - v0)).Normalized();
	}

	// The patch surface coords of the point along dir (unit): the inverse of
	// GetSpherePoint, by Newton's method on the point's offset from dir seen
	// from the centre (projected onto the plane square to dir). False if it
	// fails (dir far off the patch).
	inline bool GetPatchCoords(const vector3d &v0, const vector3d &v1, const vector3d &v2, const vector3d &v3, const vector3d &dir, double &x, double &y)
	{
		const vector3d a = (std::abs(dir.x) < 0.9 ? vector3d(1.0, 0.0, 0.0) : vector3d(0.0, 1.0, 0.0)).Cross(dir).Normalized();
		const vector3d b = dir.Cross(a);
		x = y = 0.5;
		for (int i = 0; i < 16; i++) {
			const vector3d q = v0 * ((1.0 - x) * (1.0 - y)) + v1 * (x * (1.0 - y)) + v2 * (x * y) + v3 * ((1.0 - x) * y);
			const vector3d qx = (v1 - v0) * (1.0 - y) + (v2 - v3) * y;
			const vector3d qy = (v3 - v0) * (1.0 - x) + (v2 - v1) * x;
			const double d = q.Dot(dir);
			if (d <= 0.0)
				return false;
			const double ra = q.Dot(a) / d;
			const double rb = q.Dot(b) / d;
			const double j00 = (qx.Dot(a) - ra * qx.Dot(dir)) / d;
			const double j01 = (qy.Dot(a) - ra * qy.Dot(dir)) / d;
			const double j10 = (qx.Dot(b) - rb * qx.Dot(dir)) / d;
			const double j11 = (qy.Dot(b) - rb * qy.Dot(dir)) / d;
			const double det = j00 * j11 - j01 * j10;
			if (det == 0.0)
				return false;
			const double sx = (ra * j11 - rb * j01) / det;
			const double sy = (rb * j00 - ra * j10) / det;
			x -= sx;
			y -= sy;
			if (std::abs(sx) + std::abs(sy) < 1e-12)
				break;
		}
		return true;
	}
}

// dir is on a patch whose surface coords are within this of [0,1]
static constexpr double DRAWN_COORDS_MARGIN = 1e-6;

// tri edge lengths
static constexpr double GEOPATCH_SUBDIVIDE_AT_CAMDIST = 5.0;
// how many of its own widths from the camera a patch below the usual deepest level splits
static constexpr double NEAR_SPLIT_WIDTHS = 1.5;
// how deep a patch's skirt is at least on the physical generator's ground, in gaps between its vertices
static constexpr double SKIRT_VERTEX_GAPS = 2.0;
// added to a skirt vertex's uv (the terrain vertex shaders' TERRAIN_SKIRT_UV_MARK)
static constexpr float SKIRT_UV_MARK = 2.0f;
// children are merged back only beyond this multiple of the split distance
static constexpr double LOD_MERGE_HYSTERESIS = 1.25;
// until a patch's detail tile is baked, it is drawn with an ancestor's up to
// this many levels up (GeoPatch::FindTile)
static constexpr int TILE_ANCESTOR_LEVELS = 2;
// how far beyond an edge (a share of the patch's width) the point is that finds the neighbour across a cube face's edge
// (UpdateCoarseEdges): well inside a neighbour as large, and clear of DRAWN_COORDS_MARGIN on this face at any depth
static constexpr double NEIGHBOUR_PROBE = 0.25;
// a new patch fades in from its parent's look over this long, so none pops in, even if the camera moved in fast
static constexpr double MORPH_FADE_IN_SECONDS = 0.6;

#if USE_SUB_CENTROID_CLIPPING
#if NUM_HORIZON_POINTS == 9
static const vector2d sample[NUM_HORIZON_POINTS] = {
	{ 0.2, 0.2 }, { 0.5, 0.2 }, { 0.8, 0.2 },
	{ 0.2, 0.5 }, { 0.5, 0.5 }, { 0.8, 0.8 },
	{ 0.2, 0.8 }, { 0.5, 0.8 }, { 0.8, 0.8 }
};
#elif NUM_HORIZON_POINTS == 5
static const vector2d sample[NUM_HORIZON_POINTS] = {
	// Naive ordering
	// { 0.2, 0.2 }, { 0.8, 0.2 }, // top
	//{ 0.5, 0.5 }, // middle
	//{ 0.2, 0.8 }, { 0.8, 0.8 } // bottom

	// possibly more optimal elimination for early out
	{ 0.5, 0.5 }, // centre
	{ 0.2, 0.2 }, // top-left corner
	{ 0.8, 0.8 }, // opposite corner
	{ 0.8, 0.2 }, // other diagonal
	{ 0.2, 0.8 }  // ^^^
};
#endif
#endif // #if USE_SUB_CENTROID_CLIPPING

GeoPatch::GeoPatch(const RefCountedPtr<GeoPatchContext> &ctx_, GeoSphere *gs,
	const vector3d &v0_, const vector3d &v1_, const vector3d &v2_, const vector3d &v3_,
	const int depth, const GeoPatchID &ID_) :
	m_ctx(ctx_),
	m_corners(new Corners(v0_, v1_, v2_, v3_)),
	m_geosphere(gs),
	m_PatchID(ID_),
	m_depth(depth),
	m_needUpdateVBOs(false),
	m_hasJobRequest(false)
{
	m_clipCentroid = ((v0_ + v1_ + v2_ + v3_) * 0.25).Normalized();
	m_clipRadius = 0.0;
	m_clipRadius = std::max(m_clipRadius, (v0_ - m_clipCentroid).Length());
	m_clipRadius = std::max(m_clipRadius, (v1_ - m_clipCentroid).Length());
	m_clipRadius = std::max(m_clipRadius, (v2_ - m_clipCentroid).Length());
	m_clipRadius = std::max(m_clipRadius, (v3_ - m_clipCentroid).Length());

	double distMult;
	if (m_geosphere->GetSystemBody()->GetType() < SystemBody::TYPE_PLANET_ASTEROID) {
		distMult = 10.0 / Clamp(m_depth, 1, 10);
	} else {
		distMult = 5.0 / Clamp(m_depth, 1, 5);
	}
	m_splitLength = GEOPATCH_SUBDIVIDE_AT_CAMDIST / pow(2.0, m_depth) * distMult;
	m_edgeLength = (v1_ - v0_).Length();

#if USE_SUB_CENTROID_CLIPPING
	for (size_t i = 0; i < NUM_HORIZON_POINTS; i++) {
		m_clipHorizon[i] = SSphere(PatchMaths::GetSpherePoint(v0_, v1_, v2_, v3_, sample[i].x, sample[i].y), m_clipRadius * CLIP_RADIUS_MULTIPLIER);
	}
#endif // #if USE_SUB_CENTROID_CLIPPING
}

GeoPatch::~GeoPatch()
{
	TerrainTiles::ReleaseSlot(m_tileSlot);
	TerrainSurfaceTiles::ReleaseSlot(m_surfaceSlot);
	m_hasJobRequest = false;
	for (int i = 0; i < NUM_KIDS; i++) {
		m_kids[i].reset();
	}
	m_corners.reset();
	m_patchVBOData.reset();
}

void GeoPatch::UpdateVBOs(Graphics::Renderer *renderer)
{
	PROFILE_SCOPED()
	if (m_needUpdateVBOs) {
		assert(renderer);
		m_needUpdateVBOs = false;
		FrontierLog::PatchMesh logMesh;

		//create buffer and upload data
		auto vbd = GeoPatchContext::GetVertexFormat();
		Graphics::VertexBuffer *vtxBuffer = renderer->CreateVertexBuffer(Graphics::BUFFER_USAGE_STATIC, m_ctx->NUMVERTICES(), vbd.bindings[0].stride);

		GeoPatchContext::VBOVertex *VBOVtxPtr = vtxBuffer->Map<GeoPatchContext::VBOVertex>(Graphics::BUFFER_MAP_WRITE);
		assert(vtxBuffer->GetStride() == sizeof(GeoPatchContext::VBOVertex));

		const Sint32 edgeLen = m_ctx->GetEdgeLen();
		const double frac = m_ctx->GetFrac();
		const double *pHts = m_patchVBOData->m_heights.get();
		const vector3f *pNorm = m_patchVBOData->m_normals.get();
		const GeoVertexSurface *pColr = m_patchVBOData->m_colors.get();

		double minh = DBL_MAX;
		// (500 m leaves room for islands between the vertices)
		static const unsigned char DEEP_UNDER_SEA = EncodeAltitude(-500.0);
		bool deepUnderSea = m_geosphere->GetTerrain()->HasFixedDetail();

		const vector3d v0 = m_corners->m_v0;
		const vector3d v1 = m_corners->m_v1;
		const vector3d v2 = m_corners->m_v2;
		const vector3d v3 = m_corners->m_v3;

		// The parent's surface at each vertex, to blend towards (terrain-morph.glsl), as the parent's own triangles
		// draw it: a vertex where the parent has one takes the parent's; one halfway along a parent's edge, the average
		// of its two ends; one in the middle of a parent's square, the average of the ends of the diagonal its two
		// triangles share (GeoPatchContext's indices). Blended all the way, the patch lies on its parent's triangles.
		// Not below the six faces' patches, which are always split, so never drawn.
		const GeoPatch *parent = m_parent;
		const PatchVBOData *parentData = parent ? parent->m_patchVBOData.get() : nullptr;
		m_hasMorph = parent && parent->m_depth >= 1 && parentData && parentData->m_heights && parentData->m_normals && parentData->m_colors;
		// where this patch's vertex grid starts on the parent's at twice its density (QuadPatchJob's offxy)
		const Sint32 inner = edgeLen - 2;
		Sint32 offX = 0, offY = 0;
		if (m_hasMorph) {
			const int idx = parent->GetChildIdx(this);
			offX = (idx == 1 || idx == 2) ? inner - 1 : 0;
			offY = idx >= 2 ? inner - 1 : 0;
		}
		auto parentPoint = [&](const Sint32 px, const Sint32 py) {
			const Corners &c = *parent->m_corners;
			return PatchMaths::GetSpherePoint(c.m_v0, c.m_v1, c.m_v2, c.m_v3, double(px) * frac, double(py) * frac) *
				(1.0 + parentData->m_heights[py * inner + px]);
		};

		// ----------------------------------------------------
		// inner loops
		for (Sint32 y = 1; y < edgeLen - 1; y++) {
			for (Sint32 x = 1; x < edgeLen - 1; x++) {
				const double height = *pHts;
				minh = std::min(height, minh);
				const double xFrac = double(x - 1) * frac;
				const double yFrac = double(y - 1) * frac;
				const vector3d p((PatchMaths::GetSpherePoint(v0, v1, v2, v3, xFrac, yFrac) * (height + 1.0)) - m_clipCentroid);
				m_clipRadius = std::max(m_clipRadius, p.Length());

				GeoPatchContext::VBOVertex *vtxPtr = &VBOVtxPtr[x + (y * edgeLen)];
				vtxPtr->pos = vector3f(p);
				++pHts; // next height

				const vector3f norma(pNorm->Normalized());
				vtxPtr->norm = norma;
				++pNorm; // next normal

				vtxPtr->col = pColr->col; // alpha: altitude above sea level, see EncodeAltitude()
				deepUnderSea = deepUnderSea && pColr->col.a <= DEEP_UNDER_SEA;
				std::copy(pColr->materials, pColr->materials + TerrainMaterialTable::SLOT_COUNT, vtxPtr->materials);
				++pColr; // next colour

				if (m_hasMorph) {
					// the two parent vertices it lies between (the same one twice where it's on one)
					const Sint32 fx = (x - 1) + offX, fy = (y - 1) + offY;
					const Sint32 px = fx / 2, py = fy / 2;
					Sint32 ax = px, ay = py, bx = px, by = py;
					if ((fx & 1) && (fy & 1)) {
						ax = px + 1;
						by = py + 1;
					} else if (fx & 1) {
						bx = px + 1;
					} else if (fy & 1) {
						by = py + 1;
					}
					const vector3d target = (parentPoint(ax, ay) + parentPoint(bx, by)) * 0.5;
					vtxPtr->morphPos = vector3f(target - m_clipCentroid - p);
					// (it can lie lower or further out than the patch's own vertices: the skirt and the bounds take it in)
					minh = std::min(minh, target.Length() - 1.0);
					m_clipRadius = std::max(m_clipRadius, (target - m_clipCentroid).Length());
					const Sint32 ia = ay * inner + ax, ib = by * inner + bx;
					const vector3f na = parentData->m_normals[ia].Normalized(), nb = parentData->m_normals[ib].Normalized();
					vtxPtr->morphNorm = (na + nb).Length() > 0.0f ? (na + nb).Normalized() : norma;
					const Color4ub &ca = parentData->m_colors[ia].col, &cb = parentData->m_colors[ib].col;
					vtxPtr->morphCol = Color4ub(Uint8((int(ca.r) + cb.r + 1) / 2), Uint8((int(ca.g) + cb.g + 1) / 2),
						Uint8((int(ca.b) + cb.b + 1) / 2), Uint8((int(ca.a) + cb.a + 1) / 2));
				} else {
					vtxPtr->morphPos = vector3f(0.0f, 0.0f, 0.0f);
					vtxPtr->morphNorm = norma;
					vtxPtr->morphCol = vtxPtr->col;
				}

				// uv coords
				vtxPtr->uv.x = 1.0f - xFrac;
				vtxPtr->uv.y = yFrac;

				++vtxPtr; // next vertex
			}
		}
		m_deepUnderSea = deepUnderSea;
		double minhScale = (minh + 1.0) * 0.999995;
		// (A share of the radius is next to nothing on a small moon, whose steep ground opens wide gaps beside a coarser
		// patch, with black space showing through. So on the physical generator's ground, also as deep as those gaps get.)
		if (m_geosphere->GetTerrain()->HasFixedDetail())
			minhScale = std::min(minhScale, minh + 1.0 - SKIRT_VERTEX_GAPS * GetVertexSpacing());
		// ----------------------------------------------------
		// (the skirts' vertices carry their inner neighbour's uv plus SKIRT_UV_MARK, so the terrain's vertex shaders can
		// tell them from the ground's: the stones raised on a skirt taper to nothing at its bottom, rather than standing
		// it up through the ground beside, a row of dark teeth along the patch's edge; 2026-11-09)
		const vector2f skirtMark(SKIRT_UV_MARK, SKIRT_UV_MARK);
		const Sint32 innerLeft = 1;
		const Sint32 innerRight = edgeLen - 2;
		const Sint32 outerLeft = 0;
		const Sint32 outerRight = edgeLen - 1;
		// vertical edges
		// left-edge
		for (Sint32 y = 1; y < edgeLen - 1; y++) {
			const Sint32 x = innerLeft - 1;
			const double xFrac = double(x - 1) * frac;
			const double yFrac = double(y - 1) * frac;
			const vector3d p((PatchMaths::GetSpherePoint(v0, v1, v2, v3, xFrac, yFrac) * minhScale) - m_clipCentroid);

			GeoPatchContext::VBOVertex *vtxPtr = &VBOVtxPtr[outerLeft + (y * edgeLen)];
			GeoPatchContext::VBOVertex *vtxInr = &VBOVtxPtr[innerLeft + (y * edgeLen)];
			vtxPtr->pos = vector3f(p);
			vtxPtr->norm = vtxInr->norm;
			vtxPtr->col = vtxInr->col;
			vtxPtr->uv = vtxInr->uv + skirtMark;
			std::copy(vtxInr->materials, vtxInr->materials + TerrainMaterialTable::SLOT_COUNT, vtxPtr->materials);
			vtxPtr->morphPos = vector3f(0.0f, 0.0f, 0.0f);
			vtxPtr->morphNorm = vtxInr->morphNorm;
			vtxPtr->morphCol = vtxInr->morphCol;
		}
		// right-edge
		for (Sint32 y = 1; y < edgeLen - 1; y++) {
			const Sint32 x = innerRight + 1;
			const double xFrac = double(x - 1) * frac;
			const double yFrac = double(y - 1) * frac;
			const vector3d p((PatchMaths::GetSpherePoint(v0, v1, v2, v3, xFrac, yFrac) * minhScale) - m_clipCentroid);

			GeoPatchContext::VBOVertex *vtxPtr = &VBOVtxPtr[outerRight + (y * edgeLen)];
			GeoPatchContext::VBOVertex *vtxInr = &VBOVtxPtr[innerRight + (y * edgeLen)];
			vtxPtr->pos = vector3f(p);
			vtxPtr->norm = vtxInr->norm;
			vtxPtr->col = vtxInr->col;
			vtxPtr->uv = vtxInr->uv + skirtMark;
			std::copy(vtxInr->materials, vtxInr->materials + TerrainMaterialTable::SLOT_COUNT, vtxPtr->materials);
			vtxPtr->morphPos = vector3f(0.0f, 0.0f, 0.0f);
			vtxPtr->morphNorm = vtxInr->morphNorm;
			vtxPtr->morphCol = vtxInr->morphCol;
		}
		// ----------------------------------------------------
		const Sint32 innerTop = 1;
		const Sint32 innerBottom = edgeLen - 2;
		const Sint32 outerTop = 0;
		const Sint32 outerBottom = edgeLen - 1;
		// horizontal edges
		// top-edge
		for (Sint32 x = 1; x < edgeLen - 1; x++) {
			const Sint32 y = innerTop - 1;
			const double xFrac = double(x - 1) * frac;
			const double yFrac = double(y - 1) * frac;
			const vector3d p((PatchMaths::GetSpherePoint(v0, v1, v2, v3, xFrac, yFrac) * minhScale) - m_clipCentroid);

			GeoPatchContext::VBOVertex *vtxPtr = &VBOVtxPtr[x + (outerTop * edgeLen)];
			GeoPatchContext::VBOVertex *vtxInr = &VBOVtxPtr[x + (innerTop * edgeLen)];
			vtxPtr->pos = vector3f(p);
			vtxPtr->norm = vtxInr->norm;
			vtxPtr->col = vtxInr->col;
			vtxPtr->uv = vtxInr->uv + skirtMark;
			std::copy(vtxInr->materials, vtxInr->materials + TerrainMaterialTable::SLOT_COUNT, vtxPtr->materials);
			vtxPtr->morphPos = vector3f(0.0f, 0.0f, 0.0f);
			vtxPtr->morphNorm = vtxInr->morphNorm;
			vtxPtr->morphCol = vtxInr->morphCol;
		}
		// bottom-edge
		for (Sint32 x = 1; x < edgeLen - 1; x++) {
			const Sint32 y = innerBottom + 1;
			const double xFrac = double(x - 1) * frac;
			const double yFrac = double(y - 1) * frac;
			const vector3d p((PatchMaths::GetSpherePoint(v0, v1, v2, v3, xFrac, yFrac) * minhScale) - m_clipCentroid);

			GeoPatchContext::VBOVertex *vtxPtr = &VBOVtxPtr[x + (outerBottom * edgeLen)];
			GeoPatchContext::VBOVertex *vtxInr = &VBOVtxPtr[x + (innerBottom * edgeLen)];
			vtxPtr->pos = vector3f(p);
			vtxPtr->norm = vtxInr->norm;
			vtxPtr->col = vtxInr->col;
			vtxPtr->uv = vtxInr->uv + skirtMark;
			std::copy(vtxInr->materials, vtxInr->materials + TerrainMaterialTable::SLOT_COUNT, vtxPtr->materials);
			vtxPtr->morphPos = vector3f(0.0f, 0.0f, 0.0f);
			vtxPtr->morphNorm = vtxInr->morphNorm;
			vtxPtr->morphCol = vtxInr->morphCol;
		}
		// ----------------------------------------------------
		// corners
		{
			// top left
			GeoPatchContext::VBOVertex *tarPtr = &VBOVtxPtr[0];
			GeoPatchContext::VBOVertex *srcPtr = &VBOVtxPtr[1];
			(*tarPtr) = (*srcPtr);
		}
		{
			// top right
			GeoPatchContext::VBOVertex *tarPtr = &VBOVtxPtr[(edgeLen - 1)];
			GeoPatchContext::VBOVertex *srcPtr = &VBOVtxPtr[(edgeLen - 2)];
			(*tarPtr) = (*srcPtr);
		}
		{
			// bottom left
			GeoPatchContext::VBOVertex *tarPtr = &VBOVtxPtr[(edgeLen - 1) * edgeLen];
			GeoPatchContext::VBOVertex *srcPtr = &VBOVtxPtr[(edgeLen - 2) * edgeLen];
			(*tarPtr) = (*srcPtr);
		}
		{
			// bottom right
			GeoPatchContext::VBOVertex *tarPtr = &VBOVtxPtr[(edgeLen - 1) + ((edgeLen - 1) * edgeLen)];
			GeoPatchContext::VBOVertex *srcPtr = &VBOVtxPtr[(edgeLen - 1) + ((edgeLen - 2) * edgeLen)];
			(*tarPtr) = (*srcPtr);
		}

		// ----------------------------------------------------
		// end of mapping
		vtxBuffer->Unmap();

		// use the new vertex buffer and the shared index buffer
		m_patchVBOData->m_patchMesh.reset(renderer->CreateMeshObject(vbd, vtxBuffer, m_ctx->GetIndexBuffer()));

		// The heights, normals and colours are kept: the children blend towards them (m_hasMorph above, made again
		// whenever their vertex buffers are), and the patches fine enough to stand on work out the stones under feet and
		// wheels from them (GetDrawnRadius, P4d of the OpenGL 4.6 plan). Until 2026-11-10 all but those dropped their
		// normals and colours here: keeping them costs about 32 bytes a vertex, some 40 MB of memory near the ground.

		// it fades in from its parent's look from now (SetMorphConstants)
		if (!m_shown) {
			m_shown = true;
			m_shownAt = std::chrono::steady_clock::now();
		}

#if DEBUG_PATCHES
#ifdef DEBUG_BOUNDING_SPHERES
		RefCountedPtr<Graphics::Material> mat(Pi::renderer->CreateMaterial("unlit", Graphics::MaterialDescriptor(), Graphics::RenderStateDesc()));
		switch (m_depth) {
		case 0: mat->diffuse = Color::WHITE; break;
		case 1: mat->diffuse = Color::RED; break;
		case 2: mat->diffuse = Color::GREEN; break;
		case 3: mat->diffuse = Color::BLUE; break;
		default: mat->diffuse = Color::BLACK; break;
		}
		m_boundsphere.reset(new Graphics::Drawables::Sphere3D(Pi::renderer, mat, 1, m_clipRadius));
#endif
		if (m_label3D == nullptr) {
			std::stringstream stuff;
			stuff << m_PatchID.GetPatchFaceIdx();
			m_label3D.reset(new Graphics::Drawables::Label3D(Pi::renderer, stuff.str()));
		}
#endif // DEBUG_PATCHES
	}
}

// the default sphere we do the horizon culling against
static const SSphere s_sph;
void GeoPatch::Render(Graphics::Renderer *renderer, const vector3d &campos, const matrix4x4d &modelView, const Graphics::Frustum &frustum)
{
	PROFILE_SCOPED()
	// must update the VBOs to calculate the clipRadius...
	UpdateVBOs(renderer);
	// ...before doing the visibility testing that relies on it.
	if (!IsPatchVisible(frustum, campos))
		return;

#if DEBUG_PATCHES
	RenderLabelDebug(campos, modelView);
#endif // DEBUG_PATCHES

	if (m_kids[0]) {
		for (int i = 0; i < NUM_KIDS; i++)
			m_kids[i]->Render(renderer, campos, modelView, frustum);
	} else if (HasHeightData()) {
		RenderImmediate(renderer, campos, modelView);
	}
}

void GeoPatch::RenderImmediate(Graphics::Renderer *renderer, const vector3d &campos, const matrix4x4d &modelView) const
{
	PROFILE_SCOPED()

#if DEBUG_PATCHES
	RenderLabelDebug(campos, modelView);
#endif // DEBUG_PATCHES

	if (m_patchVBOData->m_heights) {
		const vector3d relpos = m_clipCentroid - campos;
		renderer->SetTransform(matrix4x4f(modelView * matrix4x4d::Translation(relpos)));

		Pi::statSceneTris += (m_ctx->GetNumTris());
		++Pi::statNumPatches;

		// per-patch detail texture scaling value
		const float fDetailFrequency = pow(2.0f, float(m_geosphere->GetMaxDepth()) - float(m_depth));

		m_geosphere->GetSurfaceMaterial()->SetPushConstant("PatchDetailFrequency"_hash, fDetailFrequency);

		// this patch's detail tile, or its part of an ancestor's: atlas
		// texture coordinates (origin, size), and whether it has one
		vector3f part;
		const int tileSlot = FindTile(part);
		const TerrainTiles *tiles = tileSlot >= 0 ? TerrainTiles::Get(renderer) : nullptr;
		vector3f tile(0.0f, 0.0f, 0.0f);
		if (tiles) {
			const vector3f slot = tiles->SlotUV(tileSlot);
			tile = vector3f(slot.x + part.x * slot.z, slot.y + part.y * slot.z, part.z * slot.z);
		}
		m_geosphere->GetSurfaceMaterial()->SetPushConstant("PatchTile"_hash, tile, tiles ? 1.0f : 0.0f);

		// this patch's surface tile: atlas texture coordinates (origin, size)
		// and its weight, 0 without one (or in the patch depth view): how far
		// it has faded in
		const TerrainSurfaceTiles *surface = m_surfaceSlot >= 0 ? TerrainSurfaceTiles::Instance() : nullptr;
		const bool depthView = GeoSphere::GetDebugFlags() & GeoSphere::DebugFlags::DEBUG_PATCHDEPTH;
		float surfaceWeight = 0.0f;
		if (surface && !depthView)
			surfaceWeight = surface->SlotFade(m_surfaceSlot);
		m_geosphere->GetSurfaceMaterial()->SetPushConstant("PatchSurface"_hash,
			surface ? surface->SlotUV(m_surfaceSlot) : vector3f(0.0f), surfaceWeight);

		// the blend towards the parent's surface (terrain-morph.glsl), and the parent's surface tile with it, so the
		// patch takes on the parent's tile as it takes on its shape (terrain-far.glsl): the part of it this patch covers
		// (as FindTile), and how far it has faded in, 0 without one
		SetMorphConstants(m_geosphere->GetSurfaceMaterial().Get(), false);
		vector3f parentTile(0.0f);
		float parentWeight = 0.0f;
		const TerrainSurfaceTiles *atlas = TerrainSurfaceTiles::Instance();
		if (m_hasMorph && atlas && !depthView && m_parent->m_surfaceSlot >= 0) {
			const int idx = m_parent->GetChildIdx(this);
			const float u = (idx == 0 || idx == 3) ? 0.5f : 0.0f;
			const float v = idx >= 2 ? 0.5f : 0.0f;
			const vector3f slot = atlas->SlotUV(m_parent->m_surfaceSlot);
			parentTile = vector3f(slot.x + u * slot.z, slot.y + v * slot.z, 0.5f * slot.z);
			parentWeight = atlas->SlotFade(m_parent->m_surfaceSlot);
		}
		m_geosphere->GetSurfaceMaterial()->SetPushConstant("PatchSurfaceParent"_hash, parentTile, parentWeight);

		// P4a: tessellated where its triangles are large enough on screen to be cut (at its nearest point), else as
		// before. The control shader cuts each edge by its own length and distance, so neighbours agree, and an edge
		// smaller than the aim on screen isn't cut: a patch that isn't tessellated meets one that is cleanly.
		// P4c: with the stones raised, every patch they reach, so none meets the ground they lift flat, and only those:
		// further out nothing changes shape, so cutting it would cost for nothing (nor where even the tallest stone
		// would stand too small on screen and they have faded out). (nearest is in planet radii; until 2026-11-09 it was
		// held against the reach in metres, so every patch in view was tessellated.)
		bool tessellate = false;
		const double nearest = std::max((m_clipCentroid - campos).Length() - m_clipRadius, 1e-9);
		if (GeoSphere::StonesActive())
			tessellate = InStonesReach(campos, 0.0);
		else if (GeoSphere::TessellationEnabled())
			tessellate = GetVertexSpacing() / (nearest * m_geosphere->GetPixelAngle()) > GeoSphere::TessellationPixels();
		if (m_geosphere->GetSurfaceMaterial()->SetTessellated(tessellate) && tessellate)
			m_geosphere->CountTessellated();
		renderer->DrawMesh(m_patchVBOData->m_patchMesh.get(), m_geosphere->GetSurfaceMaterial().Get());
		m_geosphere->GetSurfaceMaterial()->SetTessellated(false);

#if DEBUG_CENTROIDS
		renderer->SetTransform(matrix4x4f(modelView * matrix4x4d::Translation(m_clipCentroid - campos) * matrix4x4d::ScaleMatrix(0.2 / pow(2.0f, m_depth))));
		Graphics::Drawables::GetAxes3DDrawable(renderer)->Draw(renderer);
#endif

#ifdef DEBUG_BOUNDING_SPHERES
		if (m_boundsphere.get()) {
			renderer->SetWireFrameMode(true);
			m_boundsphere->Draw(renderer);
			renderer->SetWireFrameMode(false);
		}
#endif
		renderer->GetStats().AddToStatCount(Graphics::Stats::STAT_PATCHES, 1);
	}
}

void GeoPatch::RenderShadowCaster(Graphics::Renderer *renderer, const matrix4x4d &unitToLight, double radius, const Aabb &lightBox, Graphics::Material *material) const
{
	const vector3d centre = unitToLight * m_clipCentroid;
	const double r = m_clipRadius * radius;
	if (centre.x + r < lightBox.min.x || centre.x - r > lightBox.max.x ||
		centre.y + r < lightBox.min.y || centre.y - r > lightBox.max.y ||
		centre.z + r < lightBox.min.z || centre.z - r > lightBox.max.z)
		return;

	// the same patches as GatherRenderablePatches picks, without the camera's culling
	if (m_kids[0]) {
		for (int i = 0; i < NUM_KIDS; i++)
			m_kids[i]->RenderShadowCaster(renderer, unitToLight, radius, lightBox, material);
	} else if (HasHeightData() && m_patchVBOData->m_patchMesh) {
		renderer->SetTransform(matrix4x4f(unitToLight * matrix4x4d::Translation(m_clipCentroid)));
		SetMorphConstants(material, true);
		renderer->DrawMesh(m_patchVBOData->m_patchMesh.get(), material);
	}
}

double GeoPatch::SplitLength(const vector3d &campos) const
{
	// the six faces' patches always split
	if (m_depth == 0)
		return DBL_MAX;
	// The base split length shrinks to about 3 patch widths on deep
	// levels, so distant terrain stayed coarse (quads of ~25 px at 4K).
	// Also split within a set number of patch widths (the terrain detail
	// distance setting), read every update so changes apply live.
	double splitLength = std::max(m_splitLength, m_geosphere->GetLodDistance() * m_edgeLength);
	// Seen edge-on (towards the limb from orbit, or the horizon from low
	// down) a patch is squashed on screen along the view direction, and
	// its triangles shrink to a pixel or two, where the GPU shades every
	// one of them in 2x2 pixel blocks: a lot of terrain shading for little
	// visible detail. Such patches split later, by the square root of the
	// foreshortening (constant triangle area on screen), at most at half
	// the distance. The detail across the view direction is coarser there
	// by the same factor. GetLodForeshortening() blends this in (0 = off).
	const double centroidDist = (campos - m_clipCentroid).Length();
	const double foreshortening = GeoSphere::GetLodForeshortening();
	if (foreshortening > 0.0 && centroidDist > 0.0) {
		const double facing = m_clipCentroid.Dot(campos - m_clipCentroid) / centroidDist;
		const double shrink = std::sqrt(Clamp(facing, 0.25, 1.0));
		splitLength *= 1.0 + (shrink - 1.0) * foreshortening;
	}
	// the extra levels near the player on foot or in the rover: a small radius, whatever the terrain detail distance
	if (m_depth >= m_geosphere->GetMaxDepth())
		splitLength = NEAR_SPLIT_WIDTHS * m_edgeLength;
	return splitLength;
}

void GeoPatch::SetMorphConstants(Graphics::Material *material, const bool shadow) const
{
	// Along each edge that meets a coarser patch, a strip of the patch takes on its parent's surface: all of it at the
	// edge, where it then matches the coarser neighbour, none of it the strip's width in (GeoSphere::MorphWidth). The
	// rest keeps its own detail at any distance. And a patch just made fades in from its parent's look.
	if (!m_hasMorph || !GeoSphere::MorphEnabled()) {
		material->SetPushConstant("PatchMorphRange"_hash, vector3f(0.25f, 0.0f, 0.0f), 0.0f);
		return;
	}
	UpdateCoarseEdges();
	float fadeIn = 0.0f;
	if (m_shown && !shadow) {
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_shownAt).count();
		fadeIn = float(1.0 - Clamp(seconds / MORPH_FADE_IN_SECONDS, 0.0, 1.0));
	}
	material->SetPushConstant("PatchMorph"_hash, vector3f(m_coarseEdges[0], m_coarseEdges[1], m_coarseEdges[2]), m_coarseEdges[3]);
	material->SetPushConstant("PatchMorphRange"_hash, vector3f(float(GeoSphere::MorphWidth()), fadeIn, 1.0f), GeoSphere::MorphTint() ? 1.0f : 0.0f);
}

void GeoPatch::UpdateCoarseEdges() const
{
	const Uint32 version = m_geosphere->GetMeshVersion();
	if (m_coarseEdgesVersion == version)
		return;
	m_coarseEdgesVersion = version;
	// The neighbour across each edge, from the patches of this cube face: the patch drawn at the cell next to this
	// one, found down the quadtree by the cell's bits (no geometry). Across the face's own edge the neighbour is on
	// another face, turned: found by a point just beyond the edge instead (GeoSphere::DrawnDepthAt).
	const GeoPatch *root = this;
	while (root->m_parent)
		root = root->m_parent;
	const int cells = 1 << m_depth;
	auto faceDepthAt = [&](const int gx, const int gy) {
		const GeoPatch *p = root;
		for (int k = 1; k <= m_depth; k++) {
			if (!p->m_kids[0])
				return p->m_depth;
			const int xb = (gx >> (m_depth - k)) & 1, yb = (gy >> (m_depth - k)) & 1;
			p = p->m_kids[yb ? (xb ? 2 : 3) : (xb ? 1 : 0)].get();
		}
		return m_depth; // as fine, or finer
	};
	const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
	const double px[4] = { -NEIGHBOUR_PROBE, 1.0 + NEIGHBOUR_PROBE, 0.5, 0.5 };
	const double py[4] = { 0.5, 0.5, -NEIGHBOUR_PROBE, 1.0 + NEIGHBOUR_PROBE };
	for (int e = 0; e < 4; e++) {
		const int gx = m_gridX + dx[e], gy = m_gridY + dy[e];
		int depth;
		if (gx >= 0 && gy >= 0 && gx < cells && gy < cells) {
			depth = faceDepthAt(gx, gy);
		} else {
			const vector3d probe = PatchMaths::GetSpherePoint(m_corners->m_v0, m_corners->m_v1, m_corners->m_v2, m_corners->m_v3, px[e], py[e]);
			depth = m_geosphere->DrawnDepthAt(probe);
		}
		m_coarseEdges[e] = depth >= 0 && depth < m_depth ? 1.0f : 0.0f;
	}
}

int GeoPatch::DrawnDepthAt(const vector3d &dir) const
{
	double x, y;
	if (!PatchMaths::GetPatchCoords(m_corners->m_v0, m_corners->m_v1, m_corners->m_v2, m_corners->m_v3, dir, x, y))
		return -1;
	const double lo = -DRAWN_COORDS_MARGIN, hi = 1.0 + DRAWN_COORDS_MARGIN;
	if (x < lo || x > hi || y < lo || y > hi)
		return -1;
	// (the children as GetDrawnRadius picks them)
	if (m_kids[0]) {
		const int kid = x < 0.5 ? (y < 0.5 ? 0 : 3) : (y < 0.5 ? 1 : 2);
		const int depth = m_kids[kid]->DrawnDepthAt(dir);
		if (depth >= 0)
			return depth;
		for (int i = 0; i < NUM_KIDS; i++) {
			const int other = i != kid ? m_kids[i]->DrawnDepthAt(dir) : -1;
			if (other >= 0)
				return other;
		}
		return m_depth + 1;
	}
	return m_depth;
}

bool GeoPatch::FineEnoughToStandOn() const
{
	// The two deepest levels there can be here. (Until 2026-11-09 by the vertex spacing against twice
	// GetLegacyDeepestSpacing, an old reckoning that comes out finer than the deepest patches really are: on Mercury
	// 6 m against 12, so no patch kept its surface, and feet and wheels didn't stand on the stones there.)
	const int deepest = std::min(int(GEOPATCH_MAX_DEPTH), int(m_geosphere->GetMaxDepth() + m_geosphere->GetExtraDepth()));
	return m_depth + 1 >= deepest;
}

double GeoPatch::GetVertexSpacing() const
{
	return m_edgeLength * m_ctx->GetFrac();
}

bool GeoPatch::GetDrawnRadius(const vector3d &dir, double &radius, DrawnSurface *surface) const
{
	const vector3d &v0 = m_corners->m_v0;
	const vector3d &v1 = m_corners->m_v1;
	const vector3d &v2 = m_corners->m_v2;
	const vector3d &v3 = m_corners->m_v3;
	double x, y;
	if (!PatchMaths::GetPatchCoords(v0, v1, v2, v3, dir, x, y))
		return false;
	const double lo = -DRAWN_COORDS_MARGIN;
	const double hi = 1.0 + DRAWN_COORDS_MARGIN;
	if (x < lo || x > hi || y < lo || y > hi)
		return false;

	// split, the children are drawn: the one with dir (corners v0, v1, v2
	// and v3 at (0, 0), (1, 0), (1, 1), (0, 1)), else whichever has it
	if (m_kids[0]) {
		const int kid = x < 0.5 ? (y < 0.5 ? 0 : 3) : (y < 0.5 ? 1 : 2);
		if (m_kids[kid]->GetDrawnRadius(dir, radius, surface))
			return true;
		for (int i = 0; i < NUM_KIDS; i++) {
			if (i != kid && m_kids[i]->GetDrawnRadius(dir, radius, surface))
				return true;
		}
	}
	if (!HasHeightData())
		return false;
	// Too coarse to stand on: the camera is far off, and where a ship rests mustn't depend on where it's seen from. The
	// exact height stands in.
	if (!FineEnoughToStandOn())
		return false;

	// the mesh as UpdateVBOs builds it: the inner vertices at frac steps of
	// the surface coords, raised by their heights
	const int n = m_ctx->GetEdgeLen() - 2;
	const double frac = m_ctx->GetFrac();
	const double gx = Clamp(x, 0.0, 1.0) / frac;
	const double gy = Clamp(y, 0.0, 1.0) / frac;
	const int i = Clamp(int(gx), 0, n - 2);
	const int j = Clamp(int(gy), 0, n - 2);
	const double fx = gx - i;
	const double fy = gy - j;
	const double *heights = m_patchVBOData->m_heights.get();
	auto vertex = [&](int vi, int vj) {
		return PatchMaths::GetSpherePoint(v0, v1, v2, v3, vi * frac, vj * frac) * (1.0 + heights[vj * n + vi]);
	};

	// a cell's two triangles share its diagonal from (i + 1, j) to
	// (i, j + 1) (GeoPatchContext's indices, offset by the skirt)
	vector3d a, b, c;
	int ia, ib, ic; // their vertices' indices
	if (fx + fy <= 1.0) {
		a = vertex(i, j);
		b = vertex(i + 1, j);
		c = vertex(i, j + 1);
		ia = j * n + i;
		ib = j * n + i + 1;
		ic = (j + 1) * n + i;
	} else {
		a = vertex(i + 1, j);
		b = vertex(i + 1, j + 1);
		c = vertex(i, j + 1);
		ia = j * n + i + 1;
		ib = (j + 1) * n + i + 1;
		ic = (j + 1) * n + i;
	}
	const vector3d normal = (b - a).Cross(c - a);
	const double along = normal.Dot(dir);
	if (along == 0.0)
		return false;
	radius = normal.Dot(a) / along;

	if (surface) {
		// the point's share of each corner (barycentric, on the triangle itself), and the corners' normals (normalised,
		// as the vertex buffer has them), weights and altitude blended by it, as the evaluation shaders blend them
		const vector3d point = dir * radius;
		const double area = normal.Dot(normal);
		const double wa = area > 0.0 ? (b - point).Cross(c - point).Dot(normal) / area : 1.0 / 3.0;
		const double wb = area > 0.0 ? (c - point).Cross(a - point).Dot(normal) / area : 1.0 / 3.0;
		const double wc = 1.0 - wa - wb;
		const vector3f *normals = m_patchVBOData->m_normals.get();
		const GeoVertexSurface *colours = m_patchVBOData->m_colors.get();
		if (normals) {
			const vector3d blended = vector3d(normals[ia].Normalized()) * wa + vector3d(normals[ib].Normalized()) * wb +
				vector3d(normals[ic].Normalized()) * wc;
			surface->normal = blended.Length() > 0.0 ? blended.Normalized() : point.Normalized();
		} else {
			surface->normal = point.Normalized();
		}
		surface->found = normals && colours;
		if (colours) {
			for (int s = 0; s < 16 && s < TerrainMaterialTable::SLOT_COUNT; s++)
				surface->slots[s] = float(wa * colours[ia].materials[s] + wb * colours[ib].materials[s] + wc * colours[ic].materials[s]) / 255.0f;
			const double alpha = (wa * colours[ia].col.a + wb * colours[ib].col.a + wc * colours[ic].col.a) / 255.0;
			surface->seaHeight = float(alpha * 2.0 - 1.0);
		}
	}
	return true;
}

int GeoPatch::FindTile(vector3f &part) const
{
	// a child covers a quarter of its parent (QuadPatchJob): patch
	// coordinates x, y offset by 0 or 0.5, and texture u = 1 - x, v = y
	part = vector3f(0.0f, 0.0f, 1.0f);
	const GeoPatch *p = this;
	for (int up = 0; up <= TILE_ANCESTOR_LEVELS; up++) {
		if (p->m_tileSlot >= 0)
			return p->m_tileSlot;
		if (!p->m_parent)
			break;
		const int idx = p->m_parent->GetChildIdx(p);
		const float u = (idx == 0 || idx == 3) ? 0.5f : 0.0f;
		const float v = (idx >= 2) ? 0.5f : 0.0f;
		part = vector3f(u + 0.5f * part.x, v + 0.5f * part.y, 0.5f * part.z);
		p = p->m_parent;
	}
	return -1;
}

void GeoPatch::GatherRenderablePatches(std::vector<std::pair<double, GeoPatch *>> &visiblePatches, Graphics::Renderer *renderer, const vector3d &campos, const Graphics::Frustum &frustum)
{
	PROFILE_SCOPED()
	// must update the VBOs to calculate the clipRadius...
	UpdateVBOs(renderer);
	// ...before doing the visibility testing that relies on it.
	if (!IsPatchVisible(frustum, campos))
		return;

	if (m_kids[0]) {
		for (int i = 0; i < NUM_KIDS; i++)
			m_kids[i]->GatherRenderablePatches(visiblePatches, renderer, campos, frustum);
	} else if (m_patchVBOData->m_heights) {
		visiblePatches.emplace_back((m_clipCentroid - campos).LengthSqr(), this);
	}
}


void GeoPatch::LODUpdate(const vector3d &campos, const Graphics::Frustum &frustum)
{
	PROFILE_SCOPED()
	// there should be no LOD update when we have active split requests
	if (m_hasJobRequest)
		return;

	bool canSplit = true;
	bool canMerge = bool(m_kids[0]);

	// always split at first level
	double centroidDist = DBL_MAX;
	if (m_depth > 0) {
		centroidDist = (campos - m_clipCentroid).Length();		// distance from camera to centre of the patch
		const double splitLength = SplitLength(campos);
		// (From 2026-11-09 the patches within the stones' reach were kept at the finest level, for walls along the
		// ground that turned out to be the textures' turns: taken out on 2026-11-10, it cost about half again of the
		// ground's GPU time, and the choosing of the patches twenty times over.)
		const bool tooFar = (centroidDist >= splitLength);	// check if the distance is greater than the rough length, which is how far it should be before it can split
		const bool atMaxDepth = m_depth >= std::min(GEOPATCH_MAX_DEPTH, m_geosphere->GetMaxDepth() + m_geosphere->GetExtraDepth());
		if (atMaxDepth || tooFar) {
			canSplit = false; // we're too deep in the quadtree or too far away so cannot split
		}
		// Merge only a little further out than the split distance, so a camera
		// hovering near the threshold does not split and merge the same patch
		// over and over (each split regenerates four patches).
		if (!atMaxDepth && centroidDist < splitLength * LOD_MERGE_HYSTERESIS) {
			canMerge = false;
		}
	}

	if (canSplit) {
		if (!m_kids[0]) {
			// Test if this patch is visible
			if (!IsPatchVisible(frustum, campos))
				return;

			// the GeoSphere submits the nearest candidates at the end of its update
			m_geosphere->AddQuadSplitCandidate(centroidDist, this);
		} else {
			for (int i = 0; i < NUM_KIDS; i++) {
				m_kids[i]->LODUpdate(campos, frustum);
			}
		}
	} else if (m_kids[0] && !canMerge) {
		// between the split and merge distances: keep the children up to date
		for (int i = 0; i < NUM_KIDS; i++) {
			m_kids[i]->LODUpdate(campos, frustum);
		}
	} else if (canMerge) {
		for (int i = 0; i < NUM_KIDS; i++) {
			canMerge &= m_kids[i]->canBeMerged();
		}
		if (canMerge) {
			for (int i = 0; i < NUM_KIDS; i++) {
				m_kids[i].reset();
			}
			m_geosphere->MarkMeshChanged();
		}
	}
}

void GeoPatch::RequestQuadSplit()
{
	assert(!m_hasJobRequest);
	assert(!m_kids[0]);
	m_hasJobRequest = true;

	SQuadSplitRequest *ssrd = new SQuadSplitRequest(m_corners->m_v0, m_corners->m_v1, m_corners->m_v2, m_corners->m_v3, m_clipCentroid.Normalized(), m_depth, m_geosphere->GetMaxDepth(),
		m_geosphere->GetSystemBody()->GetPath(), m_PatchID, m_ctx->GetEdgeLen() - 2,
		m_ctx->GetFrac(), m_geosphere->GetTerrain());
	ReceiveJobHandle(Pi::GetAsyncJobQueue()->Queue(new QuadPatchJob(ssrd)));
}

void GeoPatch::RequestSinglePatch()
{
	if (!HasHeightData()) {
		assert(!m_hasJobRequest);
		m_hasJobRequest = true;
		SSingleSplitRequest *ssrd = new SSingleSplitRequest(m_corners->m_v0, m_corners->m_v1, m_corners->m_v2, m_corners->m_v3, m_clipCentroid.Normalized(), m_depth, m_geosphere->GetMaxDepth(),
			m_geosphere->GetSystemBody()->GetPath(), m_PatchID, m_ctx->GetEdgeLen() - 2, m_ctx->GetFrac(), m_geosphere->GetTerrain());
		m_job = Pi::GetAsyncJobQueue()->Queue(new SinglePatchJob(ssrd));
	}
}

void GeoPatch::ReceiveHeightmaps(SQuadSplitResult *psr)
{
	PROFILE_SCOPED()
	assert(NULL != psr);
	if (m_depth < psr->depth()) {
		// this should work because each depth should have a common history
		const Uint32 kidIdx = psr->data(0).patchID.GetPatchIdx(m_depth + 1);
		if (m_kids[kidIdx]) {
			m_kids[kidIdx]->ReceiveHeightmaps(psr);
		} else {
			psr->OnCancel();
		}
	} else {
		assert(m_hasJobRequest);
		const int newDepth = m_depth + 1;
		for (int i = 0; i < NUM_KIDS; i++) {
			assert(!m_kids[i]);
			const SSplitResultData &data = psr->data(i);
			assert(i == data.patchID.GetPatchIdx(newDepth));
			assert(0 == data.patchID.GetPatchIdx(newDepth + 1));
			m_kids[i].reset(new GeoPatch(m_ctx, m_geosphere,
				data.v0, data.v1, data.v2, data.v3,
				newDepth, data.patchID));
			m_kids[i]->m_parent = this;
			// (the children's corners as QuadPatchJob lays them out: 1 and 2 on the far side along x, 2 and 3 along y)
			m_kids[i]->m_gridX = 2 * m_gridX + ((i == 1 || i == 2) ? 1 : 0);
			m_kids[i]->m_gridY = 2 * m_gridY + (i >= 2 ? 1 : 0);
		}

		for (int i = 0; i < NUM_KIDS; i++) {
			m_kids[i]->ReceiveHeightResult(psr->data(i));
		}
		m_hasJobRequest = false;
		m_geosphere->MarkMeshChanged();
	}
}

void GeoPatch::ReceiveHeightmap(const SSingleSplitResult *psr)
{
	PROFILE_SCOPED()
	assert(nullptr != psr);
	assert(m_hasJobRequest);

	ReceiveHeightResult(psr->data());
	m_geosphere->MarkMeshChanged();
	m_hasJobRequest = false;
}

void GeoPatch::ReceiveHeightResult(const SSplitResultData &data)
{
	m_patchVBOData.reset(new PatchVBOData(data.heights, data.normals, data.colors));

	// skirt vertices are not present in the heights array
	const int edgeLen = m_ctx->GetEdgeLen() - 2;

	const double h0 = data.heights[0];
	const double h1 = data.heights[edgeLen - 1];
	const double h2 = data.heights[edgeLen * (edgeLen - 1)];
	const double h3 = data.heights[edgeLen * edgeLen - 1];

	const double height = (h0 + h1 + h2 + h3) * 0.25;
	m_clipCentroid *= (1.0 + height);

	SetNeedToUpdateVBOs();
}

void GeoPatch::ReceiveJobHandle(Job::Handle job)
{
	assert(!m_job.HasJob());
	m_job = static_cast<Job::Handle &&>(job);
}

void GeoPatch::CollectDrawn(std::vector<const GeoPatch *> &out) const
{
	// (as GatherRenderablePatches, without the camera's culling)
	if (m_kids[0]) {
		for (int i = 0; i < NUM_KIDS; i++)
			m_kids[i]->CollectDrawn(out);
	} else if (m_patchVBOData && m_patchVBOData->m_heights) {
		out.push_back(this);
	}
}

void GeoPatch::EdgePoints(std::vector<EdgePoint> &out) const
{
	if (!m_patchVBOData || !m_patchVBOData->m_heights)
		return;
	// (as UpdateVBOs: the heights are the inner (edgeLen - 2) squared, the skirt around them)
	const int edgeLen = m_ctx->GetEdgeLen();
	const int inner = edgeLen - 2;
	const double frac = m_ctx->GetFrac();
	const double *heights = m_patchVBOData->m_heights.get();
	for (int y = 0; y < inner; y++) {
		for (int x = 0; x < inner; x++) {
			if (x != 0 && y != 0 && x != inner - 1 && y != inner - 1)
				continue;
			EdgePoint e;
			e.dir = PatchMaths::GetSpherePoint(m_corners->m_v0, m_corners->m_v1, m_corners->m_v2, m_corners->m_v3, double(x) * frac, double(y) * frac);
			e.radius = heights[y * inner + x] + 1.0;
			if (m_patchVBOData->m_normals && m_patchVBOData->m_colors) {
				e.hasSurface = true;
				e.normal = m_patchVBOData->m_normals[y * inner + x].Normalized();
				std::copy(m_patchVBOData->m_colors[y * inner + x].materials, m_patchVBOData->m_colors[y * inner + x].materials + TerrainMaterialTable::SLOT_COUNT, e.materials);
			}
			out.push_back(e);
		}
	}
}

bool GeoPatch::InStonesReach(const vector3d &campos, double margin) const
{
	// (the nearest the patch could come, its bounding sphere's; and where even the tallest stone would stand too small
	// on screen they've faded out)
	const double nearest = std::max((m_clipCentroid - campos).Length() - m_clipRadius, 1e-9) * m_geosphere->GetSystemBody()->GetRadius();
	const double within = std::max(nearest - margin, 1e-3);
	return within < GeoSphere::StonesReach() && m_geosphere->StonePixelsAt(within) >= 0.5 * GeoSphere::StonesFadePixels();
}

bool GeoPatch::IsPatchVisible(const Graphics::Frustum &frustum, const vector3d &camPos) const
{
	PROFILE_SCOPED()
	// Test if this patch is visible (with the stones raised, as far out as the tallest of them could raise the ground:
	// ground just below the bottom of the screen comes into view raised)
	if (!frustum.TestSphere(m_clipCentroid, m_clipRadius + m_geosphere->GetStoneCullMargin()))
		return false; // nothing below this patch is visible

	// We only want to horizon cull patches that can actually be over the horizon!
	// depth == zero patches cannot be tested against the horizon
	if (m_depth != 0 && IsOverHorizon(camPos)) {
		return false;
	}

	return true;
}

bool GeoPatch::IsOverHorizon(const vector3d &camPos) const
{
	PROFILE_SCOPED()
	const vector3d camDir(camPos - m_clipCentroid);
	const vector3d camDirNorm(camDir.Normalized());
	const vector3d cenDir(m_clipCentroid.Normalized());
	const double dotProd = camDirNorm.Dot(cenDir);

	if (dotProd < 0.25 && (camDir.LengthSqr() > (m_clipRadius * m_clipRadius))) {
		// return the result of the Horizon Culling test, inverted to match naming semantic
		// eg: HC returns true==visible, but this method returns true==hidden
#if USE_SUB_CENTROID_CLIPPING
		bool gather = false;
		for (size_t i = 0; i < NUM_HORIZON_POINTS; i++) {
			gather |= s_sph.HorizonCulling(camPos, m_clipHorizon[i]);
			if (gather)
				return false; // early out if any test is visible
		}
		return !gather; // if true then it's visible, return semantic is true == over the horizon and thus invisible so invert
#else
		return !s_sph.HorizonCulling(camPos, SSphere(m_clipCentroid, m_clipRadius));
#endif // #if USE_SUB_CENTROID_CLIPPING
	}

	// not over the horizon
	return false;
}

#if DEBUG_PATCHES
void GeoPatch::RenderLabelDebug(const vector3d &campos, const matrix4x4d &modelView) const
{
	if (m_label3D != nullptr) {
		const vector3d relpos = m_clipCentroid - campos;
		m_label3D->Draw(Pi::renderer, matrix4x4f(modelView * matrix4x4d::Translation(relpos)));
	}
}
#endif // DEBUG_PATCHES
