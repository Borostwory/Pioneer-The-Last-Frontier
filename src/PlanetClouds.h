// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _PLANETCLOUDS_H
#define _PLANETCLOUDS_H

#include "Color.h"
#include "JobQueue.h"
#include "RefCounted.h"
#include "matrix4x4.h"
#include "vector3.h"

#include <atomic>
#include <memory>
#include <vector>

class SystemBody;
class Terrain;

namespace Graphics {
	class Material;
	class Renderer;
	class Texture;
} // namespace Graphics

// Volumetric clouds for terrestrial planets.
//
// All cloud science (presence, species, layers, coverage, wind, lighting) runs
// in GLSL (data/shaders/opengl/cloud-*.glsl). This code:
//  - copies raw SystemBody values, the cloud clock and the planet orientation
//    into the CloudData uniform block,
//  - bakes a coarse terrain-height texture (clouds stay 750 m above ground),
//  - renders the clouds in a screen-space pass after all bodies are drawn, at
//    reduced resolution, using the scene depth so that terrain, cities and
//    ships are correctly hidden by (or in front of) clouds.
namespace PlanetClouds {

	// Must match the CloudData block in cloud-uniforms.glsl (std140, vec4s only)
	struct CloudDataBlock {
		float axisX[4]; // xyz: planet +X axis in view space; w: height map state (0 none, 0.5 baking, 1 ready)
		float axisY[4]; // xyz: planet +Y (spin) axis in view space
		float axisZ[4]; // xyz: planet +Z axis in view space
		float atmos[4]; // average temp [K], surface pressure [atm], surface density [kg/m^3], gravity [m/s^2]
		float geom[4];	// radius [m], tropopause [m], atmosphere top [m], max terrain height [m]
		float comp[4];	// volatileLiquid, volatileIces, atmosOxidizing, volcanicity
		float spin[4];	// rotation period [days], orbit period around a star [days] (0 for moons), time accel rate, enabled (0/1)
		float time[4];	// fmod(t, 2^20) [s], floor(t / 2^20), seed & 0xFFFF, seed >> 16 (t = cloud clock)
		float tuneDensity[4]; // user amount multiplier: low, medium, high layer; w: terrain shadow mode
		float tuneSize[4];	  // user cloud size multiplier: low, medium, high layer
		float tuneTransparency[4]; // user transparency multiplier: low, medium, high layer
		float tuneSystem[4];	   // user cloud system (weather pattern) size multiplier: low, medium, high
		float tuneCluster[4];	   // user clustering 0 (scattered) .. 1 (solid decks): low, medium, high
	};
	static_assert(sizeof(CloudDataBlock) == 208, "CloudDataBlock must match the std140 CloudData block");

	// Coarse equirectangular terrain-height map (metres above sea level),
	// baked once on the async job queue.
	class CloudHeightMap {
	public:
		CloudHeightMap(RefCountedPtr<Terrain> terrain, double radius);
		~CloudHeightMap();

		// uploads the baked data on the first call after the job has finished
		bool IsReady();
		// the baked texture, or a 1x1 zero texture until it is ready
		Graphics::Texture *GetTexture();

		struct BakeResult {
			std::vector<float> heights;
			std::atomic<bool> done { false };
		};

	private:
		std::shared_ptr<BakeResult> m_result;
		RefCountedPtr<Graphics::Texture> m_texture;
		Job::Handle m_job;
		bool m_ready = false;
	};

	// true if a height map should be baked for this body at this camera
	// position (campos in planet radii, relative to the planet centre)
	bool WantsHeightMap(const SystemBody *sbody, const vector3d &campos);

	// trans: the planet model-view matrix used by GeoSphere::Render
	void FillCloudData(CloudDataBlock &out, const SystemBody *sbody, const matrix4x4d &trans, double maxFeatureHeight, CloudHeightMap *heightMap);

	// binds the per-frame cloud data and height texture to a material
	// (the terrain shaders use it for cloud shadows)
	void SetMaterialCloudData(Graphics::Material *mat, CloudDataBlock &data, CloudHeightMap *heightMap);

	// Called by Camera::Draw before drawing bodies: clears the pass queue and
	// advances the cloud clock.
	void BeginFrame();

	// Called by GeoSphere::Render: draw clouds for this planet in this frame's
	// cloud pass. center: planet centre in view space [planet radii].
	void QueueCloudPass(const SystemBody *sbody, const CloudDataBlock &data, const vector3f &center, const Color4f &atmosColor, CloudHeightMap *heightMap);

	// Called by Camera::Draw after all bodies are drawn: renders the queued
	// clouds over the current render target.
	void RenderCloudPasses(Graphics::Renderer *renderer);

	// releases the shared textures, render targets and materials
	void Uninit();

} // namespace PlanetClouds

#endif /* _PLANETCLOUDS_H */
