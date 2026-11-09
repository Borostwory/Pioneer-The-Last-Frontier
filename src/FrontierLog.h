// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _FRONTIERLOG_H
#define _FRONTIERLOG_H

#include "graphics/UploadStats.h"

#include <string>

// A running record of the game for working on The Last Frontier, in frontier_log.txt in the
// user's Pioneer folder (the last run's is frontier_log_previous.txt). Once a second: where
// the ship is, what is under it, frame times and terrain speed. Any frame much longer than
// usual gets its own line, with where its time went. Switched by the FrontierLog config key
// and in the debug window. Everything here but TerrainQuery is for the main thread only.
namespace FrontierLog {
	enum Part {
		PART_STEP, // Game::TimeStep
		PART_SPHERES, // updating the terrain, of which:
		PART_RESULTS, // taking finished patches
		PART_LOD, // choosing which patches to split or merge
		PART_VIEW, // View::Update
		PART_DRAW, // View::Draw3D
		PART_FLUSH, // sending the frame to the renderer
		PART_JOBS, // the finished jobs' work on the main thread
		PART_SWAP, // showing the frame, and waiting for the screen and the graphics driver
		PART_LOG, // writing this log
		PART_COUNT
	};

	// adds the time it lives to a part of this frame
	class Timed {
	public:
		explicit Timed(Part part);
		~Timed();
		Timed(const Timed &) = delete;
		Timed &operator=(const Timed &) = delete;

	private:
		Part m_part;
		long long m_start;
	};

	// adds the time it lives to this frame's height queries, such as collision
	class TerrainQuery {
	public:
		TerrainQuery();
		~TerrainQuery();
		TerrainQuery(const TerrainQuery &) = delete;
		TerrainQuery &operator=(const TerrainQuery &) = delete;

	private:
		long long m_start;
	};

	// adds the time it lives to this frame's patch meshes: their vertices packed and handed to the graphics driver
	// (GeoPatch::UpdateVBOs). Main thread only.
	class PatchMesh {
	public:
		PatchMesh();
		~PatchMesh();
		PatchMesh(const PatchMesh &) = delete;
		PatchMesh &operator=(const PatchMesh &) = delete;

	private:
		long long m_start;
	};

	// What went to the graphics driver over the last whole second (Graphics::UploadStats), for the debug window:
	// sums over its frames. Counted while the log is written, or for a moment after WantUploadCounts.
	struct UploadSecond {
		int frames = 0;
		double longestMs = 0.0; // the frame with the most
		Graphics::UploadCount kinds[Graphics::UPLOAD_KINDS];
		int patchMeshes = 0;
		double patchMeshMs = 0.0; // packing their vertices and their own buffer calls
	};
	const UploadSecond &GetUploadSecond();
	// keeps the counting on for the next few frames without the log (the debug window, while it shows them)
	void WantUploadCounts();

	// at the end of each frame's update
	void EndFrame(double physicsMs, double guiMs);
	// around what comes between one frame's update and the next one's
	void BeforeSwap();
	void AfterSwap();
	// a line in the log
	void Note(const std::string &text);
	// a line with everything about here, and a comment
	void Mark(const std::string &comment);
	// walks out from the ship in all directions, and writes the sharpest step in the ground along each, and which
	// layer of the height it's in (Terrain::GetHeightLayers)
	void HeightProfile();
	// What the GPU detail tiles' landforms would cost in the CPU heights (TerrainTilesCPU, option A of Q2 in
	// docs/implementation_plans/opengl46-tessellation-compute_plan.md): time per point around the ship, against the
	// heights' own, at full detail and at the detail of meshes and tiles of several spacings. And how fine the heights'
	// detail goes today (Q3): their second differences at scales from 25 cm up. Takes a moment.
	void LandformCost();

	bool Enabled();
	void SetEnabled(bool enabled);
} // namespace FrontierLog

#endif // _FRONTIERLOG_H
