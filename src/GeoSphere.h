// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _GEOSPHERE_H
#define _GEOSPHERE_H

#include <SDL_stdinc.h>

#include "BaseSphere.h"
#include "Camera.h"
#include "core/Log.h"
#include "vector3.h"

#include <deque>

namespace Graphics {
	class Renderer;
	class Texture;
}

class SystemBody;
class GeoPatch;
class GeoPatchContext;
class SQuadSplitRequest;
class SQuadSplitResult;
class SSingleSplitResult;

namespace PlanetClouds {
	class CloudHeightMap;
}

#define NUM_PATCHES 6

class GeoSphere : public BaseSphere {
public:
	GeoSphere(const SystemBody *body);
	virtual ~GeoSphere();

	void Update() override;
	void Render(Graphics::Renderer *renderer, const matrix4x4d &modelView, vector3d campos, const float radius, const std::vector<Camera::Shadow> &shadows) override;

	double GetTerrainHeight(const vector3d &p) const final
	{
		double h;
		m_terrain->GetHeights(&p, &h, 1);
#ifndef NDEBUG
		// XXX don't remove this. Fix your fractals instead
		// Fractals absolutely MUST return heights >= 0.0 (one planet radius)
		// otherwise atmosphere and other things break.
		if (h < 0.0) {
			Output("GetTerrainHeight({ %f, %f, %f }) returned %f\n", p.x, p.y, p.z, h);
			m_terrain->DebugDump();
			assert(h >= 0.0);
		}
#endif /* NDEBUG */
		return h;
	}

	static void InitGeoSphere();
	static void UninitGeoSphere();
	static void UpdateAllGeoSpheres();
	static void OnChangeGeoSphereDetailLevel();
	static bool OnAddQuadSplitResult(const SystemPath &path, SQuadSplitResult *res);
	static bool OnAddSingleSplitResult(const SystemPath &path, SSingleSplitResult *res);

	enum DebugFlags : uint32_t { // <enum scope='GeoSphere' name=GeoSphereDebugFlags prefix=DEBUG_ public>
		DEBUG_NONE = 0x0,
		DEBUG_SORTGEOPATCHES = 0x1,
		DEBUG_WIREFRAME = 0x2,
		DEBUG_FACELABELS = 0x4,
		DEBUG_PATCHDEPTH = 0x8,
		DEBUG_NOBANDLIMIT = 0x10
	};
	static void SetDebugFlags(Uint32 flags);
	static Uint32 GetDebugFlags();

	// how early patches subdivide: a patch splits once the camera is closer
	// than this many patch widths (config PlanetLodDistance, re-read every
	// frame by UpdateAllGeoSpheres)
	static double GetLodDistanceScale();
	static void SetLodDistanceScale(double scale);
	// the setting adjusted for the camera's current altitude above this body
	double GetLodDistance() const { return m_lodDistance; }

	// in sbody radii
	double GetMaxFeatureHeight() const final { return m_terrain->GetMaxHeight(); }

	bool AddQuadSplitResult(SQuadSplitResult *res);
	bool AddSingleSplitResult(SSingleSplitResult *res);
	void ProcessSplitResults();

	void Reset() override;

	inline Sint32 GetMaxDepth() const { return m_maxDepth; }

	// a patch that wants to split; the nearest ones are submitted at the end of the update
	void AddQuadSplitCandidate(double distance, GeoPatch *patch);

private:
	void BuildFirstPatches();
	void CalculateMaxPatchDepth();
	inline vector3d GetColor(const vector3d &p, double height, const vector3d &norm) const
	{
		return m_terrain->GetColor(p, height, norm);
	}
	void ProcessQuadSplitRequests();

	std::unique_ptr<GeoPatch> m_patches[6];
	std::vector<std::pair<double, GeoPatch *>> m_visiblePatches;

	// patches that want to split this update, with their distance to the camera
	std::vector<std::pair<double, GeoPatch *>> mQuadSplitCandidates;

	std::deque<SQuadSplitResult *> mQuadSplitResults;
	std::deque<SSingleSplitResult *> mSingleSplitResults;

	bool m_hasTempCampos;
	vector3d m_tempCampos;
	Graphics::Frustum m_tempFrustum;

	static RefCountedPtr<GeoPatchContext> s_patchContext;

	void SetUpMaterials() override;
	void CreateAtmosphereMaterial();

	RefCountedPtr<Graphics::Texture> m_texHi;
	RefCountedPtr<Graphics::Texture> m_texLo;

	// terrain heights for the cloud shaders, baked on first close approach
	std::unique_ptr<PlanetClouds::CloudHeightMap> m_cloudHeightMap;

	enum EGSInitialisationStage {
		eBuildFirstPatches = 0,
		eRequestedFirstPatches,
		eReceivedFirstPatches,
		eDefaultUpdateState
	};
	EGSInitialisationStage m_initStage;

	Sint32 m_maxDepth;

	// effective terrain detail distance for this frame, see GetLodDistance()
	double m_lodDistance = 8.0;
};

#endif /* _GEOSPHERE_H */
