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
	struct CityGlowDataBlock;
}

class SystemBody;
class SpaceStation;
class GeoPatch;
class GeoPatchContext;
class SQuadSplitRequest;
class SQuadSplitResult;
class SSingleSplitResult;

namespace PlanetClouds {
	class CloudHeightMap;
	struct CloudDataBlock;
}

#define NUM_PATCHES 6

class GeoSphere : public BaseSphere {
public:
	GeoSphere(const SystemBody *body);
	virtual ~GeoSphere();

	void Update() override;
	void Render(Graphics::Renderer *renderer, const matrix4x4d &modelView, vector3d campos, const float radius, const std::vector<Camera::Shadow> &shadows) override;
	void RenderShadowCaster(Graphics::Renderer *renderer, const matrix4x4d &unitToLight, double radius, const Aabb &lightBox) override;

	// the terrain mesh along p (GeoPatch::GetDrawnRadius)
	bool GetDrawnRadius(const vector3d &p, double &radius) const override;
	uint32_t GetMeshVersion() const override { return m_meshVersion; }
	void MarkMeshChanged() { m_meshVersion++; }

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
	// 0..1: how much later patches seen edge-on split (config
	// PlanetLodForeshortening in percent, re-read every frame)
	static double GetLodForeshortening();

	// in sbody radii
	double GetMaxFeatureHeight() const final { return m_terrain->GetMaxHeight(); }

	bool AddQuadSplitResult(SQuadSplitResult *res);
	bool AddSingleSplitResult(SSingleSplitResult *res);
	void ProcessSplitResults();

	void Reset() override;

	// the deepest level of the patches (CalculateMaxPatchDepth), and the level that triangles of about 100 m would need
	inline Sint32 GetMaxDepth() const { return m_maxDepth; }
	Sint32 GetMeantDepth() const;
	// how many levels deeper patches go near the player, now: on foot or in the rover, on physical terrain
	inline Sint32 GetExtraDepth() const { return m_extraDepth; }
	// metres between the vertices of the deepest patches, where they are farthest apart: in the middle of a cube face
	// (about 0.8 times that near its corners)
	double GetDeepestVertexSpacing() const;
	// The old reckoning of that spacing, about half of it (0.46 on most settings). The GPU detail tiles' octave
	// ladder, the relief shading's finest size and the rule for which patches are fine enough to stand on were tuned
	// to it, so they keep it until they are tuned again.
	double GetLegacyDeepestSpacing() const;
	// the patches it drew when last drawn, for the debug window
	struct DrawnPatches {
		int count = 0;
		int deepest = 0; // depth of the deepest
		double finestSpacing = 0.0; // metres between the vertices of the deepest
		int nearestDepth = 0; // depth of the one nearest the camera
		double nearestSpacing = 0.0; // metres between its vertices
		int tessellated = 0; // how many were drawn tessellated (P4a)
	};
	const DrawnPatches &GetDrawnPatches() const { return m_drawnPatches; }

	// P4a of the OpenGL 4.6 plan: the ground near the camera tessellated, its triangles cut finer on the graphics card
	// (no new shape yet). Switched in the debug window (config TerrainTessellation); the triangle edge on screen it aims
	// for, in pixels (TerrainTessellationPixels).
	static bool TessellationEnabled();
	static double TessellationPixels();
	// P4b: the ground textures at one size near the camera (config TerrainTextureFixed), tiles of 2^level metres
	// (TerrainTextureFixedLevel), out to this many metres (TerrainTextureFixedRange)
	static bool TextureFixedEnabled();
	static double TextureFixedLevel();
	static double TextureFixedRange();
	// P4c: the stones raised near the camera (config TerrainStones), within the textures' one size, so only with it;
	// how high, times each material's own height (TerrainStonesHeight)
	static bool StonesEnabled();
	static double StonesHeight();
	// P4e: whether the stones shade the ground from the sun (config TerrainStoneShadows)
	static bool StoneShadowsEnabled();
	// whether stones are raised at all now, and how far out they go (metres): every patch that near is tessellated, and
	// with them on, only those
	static bool StonesActive() { return StonesEnabled() && TextureFixedEnabled(); }
	static double StonesReach() { return 2.0 * TextureFixedRange(); }
	// P4d: whether feet and wheels stand on the stones (config TerrainStonesContact): GetDrawnRadius adds them, as drawn
	static bool StonesContactEnabled();
	// how tall on screen (pixels) the tallest stone must stand for the ground to be cut at the full aim; below, the cuts
	// thin out with it (config TerrainStonesFullPixels; TessParams.w, terrain-stone-aim.glsl)
	static double StonesFullPixels();
	// P8: whether the sky reads its light from a table worked out once a frame, with the camera in the air (config
	// SkyTable; only with RealisticScattering=2)
	static bool SkyTableEnabled();
	// the cheaper photo textures (terrain-materials.glsl, TextureLod), each switched in the debug window: simply far
	// off (TerrainTextureSimpleFar) from a distance (TerrainTextureSimpleFrom, m); one size at a time
	// (TerrainTextureOneScale), blending over a share of the way between sizes (TerrainTextureScaleBand); the turns'
	// hashes from a table (TerrainTurnTexture, off: never faster, kept as the ground's shader is at the card's most
	// instructions and, taken out, the driver laid it out over them)
	static bool TextureSimpleFar();
	static double TextureSimpleFrom();
	static bool TextureOneScale();
	static double TextureScaleBand();
	static bool TurnTable();
	// the debug window's ground cost breakdown: parts of the ground's drawing left out (GROUND_SKIP in
	// terrain-surface.glsl), not saved
	static int GetGroundSkip();
	static void SetGroundSkip(int mask);
	// P7: what the drawn ground along dir is made of, as shares adding up to 1, for what ships blow off it (Ship::
	// SpawnGroundDust); false where the mesh there holds no materials (coarse, far from the camera)
	struct GroundMix {
		float dust = 0.0f; // sand, regolith, dust, ash, soil, salt, cracked mud
		float rock = 0.0f; // rock, gravel, basalt, lava and its flows, metal
		float ice = 0.0f; // snow, ice, dirty ice
		float plants = 0.0f; // grass, forest, tundra
		float water = 0.0f; // the sea (all of it, where the ground is under it)
	};
	bool GetGroundMix(const vector3d &dir, GroundMix &mix) const;
	// how tall on screen (pixels) the tallest stone must stand for the stones to be raised at all; they fade out from it
	// to half of it, and nothing is cut there (config TerrainStonesFadePixels; MaterialNear.w, terrain-stone-aim.glsl)
	static double StonesFadePixels();
	// how many pixels tall the tallest stone of this body could stand on screen at a distance (metres), as
	// terrainStonePixels() works it out
	double StonePixelsAt(double metres) const;
	// the mesh's vertex spacing, metres: the finest (the deepest patches, with the extra near the player) and per metre
	// of distance, which the stones are smoothed to where it's cut as finely as it can be (terrainStoneSpacing())
	void StoneMeshSpacing(double &finest, double &perMetre) const;
	// how far below the screen ground may lie and still be raised into view by the stones, planet radii
	// (GeoPatch::IsPatchVisible)
	double GetStoneCullMargin() const { return m_stoneCullMargin; }
	// Debug: where two drawn patches share an edge point, whether they put it at the same height; the worst to the log
	// (FrontierLog), with both patches' levels
	void CheckSeams() const;
	// P4d's check: the stones' height under the camera, worked out by the main processor and by the graphics card
	struct StoneProbe {
		bool valid = false; // both are in
		double cpu = 0.0, gpu = 0.0; // metres
		int layer1 = -1, layer2 = -1; // the two materials there (texture layers)
		double distance = 0.0; // from the camera, metres
		bool meshData = false; // the patch there still had its normals and material weights
	};
	// while on, the body drawn nearest works the check out each frame
	static void SetStoneProbe(bool on);
	static const StoneProbe &GetStoneProbe();
	// radians per pixel of the view the patches are being drawn for (GeoPatch decides which to tessellate)
	double GetPixelAngle() const { return m_pixelAngle; }
	// counts a patch drawn tessellated, for GetDrawnPatches
	void CountTessellated() { m_tessellatedThisFrame++; }
	// weights of the detail tile styles: rolling, ridged, dunes, craters (TerrainTiles)
	const float *GetTileStyle() const { return m_tileStyle; }
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
	// depth-only material for the patches in the sun shadow maps, created on first use
	RefCountedPtr<Graphics::Material> m_shadowMaterial;
	std::vector<std::pair<double, GeoPatch *>> m_visiblePatches;
	// GetDrawnPatches, from m_visiblePatches before it is cleared
	void NoteDrawnPatches();
	DrawnPatches m_drawnPatches;
	double m_pixelAngle = 0.0;
	int m_tessellatedThisFrame = 0;
	// GetStoneCullMargin, set in Render
	double m_stoneCullMargin = 0.0;
	// Render: the ground's version with the stones started building (SetUpMaterials starts the plain one)
	bool m_stonesVersionStarted = false;
	// P4d: the stones' height at a point of the drawn mesh (planet radii), from what the mesh holds there, in metres
	double StoneLiftAt(const vector3d &point, const vector3d &normal, const float slots[16], float seaHeight) const;
	// P4d's check, in Render: reads last frame's answer from the graphics card and asks again
	void RunStoneProbe(Graphics::Renderer *renderer, const matrix4x4d &trans, const vector3d &campos, PlanetClouds::CloudDataBlock &cloudData);
	// P4: the ground kept free of stones near the camera (planet-fixed, metres): the cities and the landed ships. Within
	// radius of the line from centre to end (the same for a city's circle), the stones back over margin beyond
	struct StoneFlat {
		vector3d centre;
		vector3d end;
		double radius;
		double margin;
	};
	std::vector<StoneFlat> m_stoneFlats;
	void UpdateStoneFlats(const vector3d &campos);
	// sets them on a material, in view space (trans: planet radii to view)
	void SetStoneFlats(Graphics::Material *material, const matrix4x4d &trans) const;
	// P8: the sky's table round the camera (sky-table.glsl), filled by m_skyTableMaterial
	RefCountedPtr<Graphics::Texture> m_skyTable;
	RefCountedPtr<Graphics::Material> m_probeMaterial;
	RefCountedPtr<Graphics::Texture> m_probeImage;
	bool m_probePending = false;

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
	RefCountedPtr<Graphics::Texture> m_texWater;
	RefCountedPtr<Graphics::Texture> m_texMaterials;
	RefCountedPtr<Graphics::Texture> m_texRelief;

	// terrain heights for the cloud shaders, baked on first close approach
	std::unique_ptr<PlanetClouds::CloudHeightMap> m_cloudHeightMap;

	void SetFarLook();
	// each material slot's colour, roughness, relief roughness and bump strength, for the ground to read
	// (MaterialSlotData in terrain-materials.glsl), worked out once for the body in SetUpMaterials
	struct MaterialSlotDataBlock {
		float colour[16][4];
		float traits[16][4];
	};

	MaterialSlotDataBlock m_materialSlotData = {};
	void FillMaterialSlotData();
	// the turns' cell hashes (materialCellHashTex in terrain-materials.glsl): the same for every body, made once
	static Graphics::Texture *CellHashTexture(Graphics::Renderer *renderer);
	// the patch surface tiles (TerrainSurfaceTiles) of the visible patches
	void SetSurfaceTiles(double pixelAngle);

	// the starport cities, for the night lights seen from afar (city-glow.glsl);
	// found on the first draw in a game
	struct GlowCity {
		const SpaceStation *station;
		vector3d pos;  // planet-fixed, m
		double radius; // m
	};
	std::vector<GlowCity> m_glowCities;
	bool m_glowCitiesFound = false;
	void SetCityGlowData(const vector3d &campos, Graphics::CityGlowDataBlock &data);

	enum EGSInitialisationStage {
		eBuildFirstPatches = 0,
		eRequestedFirstPatches,
		eReceivedFirstPatches,
		eDefaultUpdateState
	};
	EGSInitialisationStage m_initStage;

	Sint32 m_maxDepth;
	Sint32 m_extraDepth = 0;
	uint32_t m_meshVersion = 0;

	// effective terrain detail distance for this frame, see GetLodDistance()
	double m_lodDistance = 8.0;
	// see GetTileStyle()
	float m_tileStyle[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	void SetUpTileStyle();
};

#endif /* _GEOSPHERE_H */
