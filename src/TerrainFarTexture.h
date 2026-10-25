// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINFARTEXTURE_H
#define _TERRAINFARTEXTURE_H

#include "JobQueue.h"
#include "RefCounted.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

class Terrain;

namespace Graphics {
	class Texture;
}

// The planet seen from afar: its colour, altitude above sea level and surface
// normals baked into two cube maps from the same terrain functions as the
// patches (Terrain::GetHeights and GetColor), for the terrain shaders to use
// per pixel where the mesh is coarser than the texture
// (data/shaders/opengl/terrain-far.glsl). From high orbit the patch vertices
// are 10 to 20 pixels apart and their colours are blended across each
// triangle; at 1024 texels per face Earth keeps its detail down to 1-2 px.
//
// Baked on the async job queue in strips of rows, only a few at a time so
// the patch jobs keep most threads, and one planet at a time; then uploaded
// one cube face per frame and faded in. Until then the planet is drawn as
// before.
class TerrainFarTexture {
public:
	// a planet gets one once it is this many pixels across, and loses it
	// after RELEASE_SECONDS below RELEASE_PIXELS
	static constexpr double START_PIXELS = 256.0;
	static constexpr double RELEASE_PIXELS = 96.0;
	static constexpr double RELEASE_SECONDS = 20.0;

	// texels across a cube face (config PlanetFarTextureSize, a power of two
	// from 256 to 2048), 0 if switched off (config PlanetFarTexture)
	static int ConfiguredSize();
	// false while another planet's texture is being baked
	static bool CanStart();
	// the last finished bake, for the Terrain Debug window
	static const std::string &GetLastBakeInfo();

	TerrainFarTexture(RefCountedPtr<Terrain> terrain, const std::string &bodyName, double radius, int size);
	~TerrainFarTexture();

	// main thread, often: queues the next jobs, uploads a finished face, fades
	// in by frameTime (real seconds; 0 where it is not drawn)
	void Update(float frameTime);

	bool IsReady() const { return m_stage == Stage::Ready; }
	// 0 to 1 over the first moments after it is ready
	float GetFade() const { return m_fade; }
	int GetSize() const { return m_size; }
	// texel size in metres at the face centres, where texels are largest
	double GetTexelMetres() const { return 2.0 * m_radius / double(m_size); }

	// rgb: colour as in the vertex colour, a: altitude above sea level as in
	// the vertex alpha (EncodeAltitude in GeoPatchJobs.h)
	Graphics::Texture *GetColour() const { return m_colour.Get(); }
	// rgb: the surface normal minus the radial direction, planet-fixed,
	// stored as 0.5 + 0.5 * sign(v) * sqrt(|v| / 0.5) (FAR_NORMAL_RANGE)
	Graphics::Texture *GetNormal() const { return m_normal.Get(); }

	// filled by the jobs, see TerrainFarTexture.cpp
	struct BakeState;

private:
	enum class Stage {
		Baking,
		Uploading,
		Ready
	};

	std::shared_ptr<BakeState> m_state;
	std::vector<Job::Handle> m_jobs;
	RefCountedPtr<Graphics::Texture> m_colour;
	RefCountedPtr<Graphics::Texture> m_normal;
	std::string m_bodyName;
	double m_radius;
	int m_size;
	int m_queued = 0;	// strips handed to the job queue
	int m_uploaded = 0; // cube faces uploaded, colour and normal alternating
	Stage m_stage = Stage::Baking;
	float m_fade = 0.0f;
	std::chrono::steady_clock::time_point m_start;
};

#endif // _TERRAINFARTEXTURE_H
