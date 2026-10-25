// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "GeoPatchJobs.h"

#include "GeoSphere.h"
#include "MathUtil.h"
#include "perlin.h"
#include "profiler/Profiler.h"
#include "terrain/TerrainNoise.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace {
	std::atomic<uint64_t> s_genJobs(0);
	std::atomic<uint64_t> s_genPatches(0);
	std::atomic<uint64_t> s_genMicroseconds(0);
	std::atomic<int> s_quadJobsInFlight(0);

	// adds the lifetime of a patch job to the generation statistics
	class ScopedGenTimer {
	public:
		explicit ScopedGenTimer(const uint64_t numPatches) :
			m_numPatches(numPatches),
			m_start(std::chrono::steady_clock::now()) {}
		~ScopedGenTimer()
		{
			const auto elapsed = std::chrono::steady_clock::now() - m_start;
			s_genMicroseconds += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
			s_genPatches += m_numPatches;
			++s_genJobs;
		}

	private:
		const uint64_t m_numPatches;
		const std::chrono::steady_clock::time_point m_start;
	};

	// debug view: replaces the terrain colour with a colour per patch depth
	inline bool DebugPatchDepthColours()
	{
		return (GeoSphere::GetDebugFlags() & GeoSphere::DebugFlags::DEBUG_PATCHDEPTH) != 0;
	}

	// Highest noise frequency a vertex grid with the given spacing (unit-sphere
	// units) can show, or 0 (no limit) when band limiting is switched off for
	// A/B comparison in the debug window.
	inline double NyquistFrequency(const double spacing)
	{
		if (GeoSphere::GetDebugFlags() & GeoSphere::DebugFlags::DEBUG_NOBANDLIMIT)
			return 0.0;
		return 0.5 / spacing;
	}

	// Heights stay full detail on the two deepest levels, where ships fly low
	// and land, so the rendered ground matches GetTerrainHeight() (collision).
	inline bool BandLimitHeights(const uint32_t depth, const uint32_t maxDepth)
	{
		return depth + 1 < maxDepth;
	}

	// debug view colour per patch depth; darker where the heights are band limited
	inline vector3d PatchDepthColour(const uint32_t depth, const uint32_t maxDepth)
	{
		static const vector3d palette[8] = {
			{ 0.9, 0.1, 0.1 }, { 0.9, 0.5, 0.1 }, { 0.9, 0.9, 0.1 }, { 0.2, 0.8, 0.2 },
			{ 0.1, 0.8, 0.8 }, { 0.2, 0.3, 0.9 }, { 0.6, 0.2, 0.9 }, { 0.9, 0.9, 0.9 }
		};
		return palette[depth & 7] * (BandLimitHeights(depth, maxDepth) ? 0.6 : 1.0);
	}
} // namespace

GeoPatchGenStats GetGeoPatchGenStats()
{
	GeoPatchGenStats stats;
	stats.jobs = s_genJobs;
	stats.patches = s_genPatches;
	stats.microseconds = s_genMicroseconds;
	return stats;
}

int GetQuadPatchJobsInFlight()
{
	return s_quadJobsInFlight;
}

void CountQuadPatchJob(const int delta)
{
	s_quadJobsInFlight += delta;
}

void ResetGeoPatchGenStats()
{
	s_genJobs = 0;
	s_genPatches = 0;
	s_genMicroseconds = 0;
}

inline void setColour(GeoVertexSurface &r, const vector3d &v, const double altitude)
{
	r.col.r = static_cast<unsigned char>(Clamp(v.x * 255.0, 0.0, 255.0));
	r.col.g = static_cast<unsigned char>(Clamp(v.y * 255.0, 0.0, 255.0));
	r.col.b = static_cast<unsigned char>(Clamp(v.z * 255.0, 0.0, 255.0));
	r.col.a = EncodeAltitude(altitude);
}

// colour, altitude and material weights of one vertex
inline void setSurface(GeoVertexSurface &r, const Terrain *terrain, const vector3d &p, const double height, const vector3d &normal, const bool debugDepth, const int depth, const int maxDepth)
{
	const TerrainMaterialTable &materials = terrain->GetMaterialTable();
	if (debugDepth) {
		setColour(r, PatchDepthColour(depth, maxDepth), terrain->GetSurfaceAltitude(p, height));
		materials.Single(-1, r.materials);
		return;
	}
	const vector3d colour = terrain->GetColor(p, height, normal);
	const double altitude = SurfaceAltitude(terrain, p, height, colour);
	setColour(r, colour, altitude);
	// under water: the beach material, for the shore
	if (altitude < 0.0)
		materials.Single(materials.BeachSlot(), r.materials);
	else
		materials.Classify(colour, r.materials);
}

bool ColourSlopeNormals(const Terrain *terrain, const vector3d *points, const int gridWidth, const int x0, const int x1, const int y0, const int y1, std::vector<vector3d> &out)
{
	PROFILE_SCOPED()
	const double scale = COLOUR_SLOPE_SCALE;
	auto dir = [&](const int x, const int y) { return points[size_t(y) * gridWidth + x].Normalized(); };
	const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
	const vector3d c = dir(cx, cy);
	const double spacing = std::max((dir(cx + 1, cy) - c).Length(), (dir(cx, cy + 1) - c).Length());
	if (spacing >= 0.7 * scale)
		return false;

	// lattice nodes about 1.5 scales apart (the slope at that scale changes
	// little in between), on the region's first and last points
	const int step = std::max(1, int(1.5 * scale / spacing));
	auto nodes = [step](const int a, const int b) {
		std::vector<int> n;
		for (int i = a; i < b - 1; i += step)
			n.push_back(i);
		n.push_back(b - 1);
		return n;
	};
	const std::vector<int> nx = nodes(x0, x1), ny = nodes(y0, y1);
	const int w = int(nx.size());

	// at each node, the heights a scale away along the grid's two directions,
	// band limited to that scale
	std::vector<vector3d> samples(nx.size() * ny.size() * 4);
	std::vector<double> heights(samples.size());
	size_t k = 0;
	for (const int y : ny) {
		for (const int x : nx) {
			const vector3d p = dir(x, y);
			vector3d t1 = dir(x + 1, y) - dir(x - 1, y);
			t1 = (t1 - p * p.Dot(t1)).Normalized();
			const vector3d t2 = p.Cross(t1);
			samples[k++] = (p + t1 * scale).Normalized();
			samples[k++] = (p - t1 * scale).Normalized();
			samples[k++] = (p + t2 * scale).Normalized();
			samples[k++] = (p - t2 * scale).Normalized();
		}
	}
	{
		TerrainNoise::ScopedBandLimit bandLimit(0.5 / scale);
		terrain->GetHeights(samples.data(), heights.data(), samples.size());
	}
	// t1 x t2 = p, so these point outward
	auto position = [&](const size_t j) { return samples[j] * (1.0 + heights[j]); };
	std::vector<vector3d> nodeNormals(nx.size() * ny.size());
	for (size_t i = 0; i < nodeNormals.size(); i++)
		nodeNormals[i] = (position(4 * i) - position(4 * i + 1)).Cross(position(4 * i + 2) - position(4 * i + 3)).Normalized();

	// bilinear between the nodes
	auto locate = [](const std::vector<int> &n, const int v, int &i, double &f) {
		i = 0;
		while (i + 2 < int(n.size()) && v >= n[i + 1])
			++i;
		f = n.size() > 1 ? Clamp(double(v - n[i]) / double(n[i + 1] - n[i]), 0.0, 1.0) : 0.0;
	};
	out.resize(size_t(x1 - x0) * (y1 - y0));
	for (int y = y0; y < y1; y++) {
		int j;
		double fy;
		locate(ny, y, j, fy);
		const int j1 = std::min(j + 1, int(ny.size()) - 1);
		for (int x = x0; x < x1; x++) {
			int i;
			double fx;
			locate(nx, x, i, fx);
			const int i1 = std::min(i + 1, w - 1);
			const vector3d a = nodeNormals[j * w + i] * (1.0 - fx) + nodeNormals[j * w + i1] * fx;
			const vector3d b = nodeNormals[j1 * w + i] * (1.0 - fx) + nodeNormals[j1 * w + i1] * fx;
			out[size_t(y - y0) * (x1 - x0) + (x - x0)] = (a * (1.0 - fy) + b * fy).Normalized();
		}
	}
	return true;
}

// in patch surface coords, [0,1]
inline vector3d GetSpherePoint(const vector3d &v0, const vector3d &v1, const vector3d &v2, const vector3d &v3, const double x, const double y)
{
	return (v0 + x * (1.0 - y) * (v1 - v0) + x * y * (v2 - v0) + (1.0 - x) * y * (v3 - v0)).Normalized();
}

// ********************************************************************************
// Overloaded PureJob class to handle generating the mesh for each patch
// ********************************************************************************

// Generates full-detail vertices, and also non-edge normals and colors
void SSingleSplitRequest::GenerateMesh() const
{
	PROFILE_SCOPED()
	const int borderedEdgeLen = edgeLen + (BORDER_SIZE * 2);
	const int numBorderedVerts = borderedEdgeLen * borderedEdgeLen;

	// generate heights plus a 1 unit border
	vector3d *fill_verts = borderVertexs.get();
	for (int y = -BORDER_SIZE; y < borderedEdgeLen - BORDER_SIZE; y++) {
		const double yfrac = double(y) * fracStep;
		for (int x = -BORDER_SIZE; x < borderedEdgeLen - BORDER_SIZE; x++) {
			const double xfrac = double(x) * fracStep;
			const vector3d p = GetSpherePoint(v0, v1, v2, v3, xfrac, yfrac);
			*(fill_verts++) = p;
		}
	}

	// skip noise octaves finer than the vertex grid can show
	const double maxFrequency = NyquistFrequency((v1 - v0).Length() * fracStep);

	vector3d *vrts = borderVertexs.get();
	double *bhts = borderHeights.get();
	{
		TerrainNoise::ScopedBandLimit bandLimit(BandLimitHeights(depth, maxDepth) ? maxFrequency : 0.0);
		pTerrain->GetHeights(vrts, bhts, numBorderedVerts);
	}

	for (int iHeights = 0; iHeights < numBorderedVerts; iHeights++)
	{
		vrts[iHeights] *= (bhts[iHeights] + 1.0);
	}

	// the slope the colours see (COLOUR_SLOPE_SCALE), if not the vertices' own
	std::vector<vector3d> colourNormals;
	const bool fixedSlope = ColourSlopeNormals(pTerrain.Get(), vrts, borderedEdgeLen,
		BORDER_SIZE, borderedEdgeLen - BORDER_SIZE, BORDER_SIZE, borderedEdgeLen - BORDER_SIZE, colourNormals);

	// Generate normals & colors for non-edge vertices since they never change
	TerrainNoise::ScopedBandLimit bandLimit(maxFrequency);
	const bool debugDepth = DebugPatchDepthColours();
	GeoVertexSurface *col = colors;
	vector3f *nrm = normals;
	double *hts = heights;
	vrts = borderVertexs.get();
	const vector3d *colourNormal = colourNormals.data();
	for (int y = BORDER_SIZE; y < borderedEdgeLen - BORDER_SIZE; y++) {
		for (int x = BORDER_SIZE; x < borderedEdgeLen - BORDER_SIZE; x++) {
			// height
			const double height = borderHeights[x + y * borderedEdgeLen];
			assert(hts != &heights[edgeLen * edgeLen]);
			*(hts++) = height;

			// normal
			const vector3d &x1 = vrts[(x - 1) + y * borderedEdgeLen];
			const vector3d &x2 = vrts[(x + 1) + y * borderedEdgeLen];
			const vector3d &y1 = vrts[x + (y - 1) * borderedEdgeLen];
			const vector3d &y2 = vrts[x + (y + 1) * borderedEdgeLen];
			const vector3d n = ((x2 - x1).Cross(y2 - y1)).Normalized();
			assert(nrm != &normals[edgeLen * edgeLen]);
			*(nrm++) = vector3f(n);

			// color
			const vector3d p = GetSpherePoint(v0, v1, v2, v3, (x - BORDER_SIZE) * fracStep, (y - BORDER_SIZE) * fracStep);
			setSurface(*col, pTerrain.Get(), p, height, fixedSlope ? *(colourNormal++) : n, debugDepth, depth, maxDepth);
			assert(col != &colors[edgeLen * edgeLen]);
			++col;
		}
	}
	assert(hts == &heights[edgeLen * edgeLen]);
	assert(nrm == &normals[edgeLen * edgeLen]);
	assert(col == &colors[edgeLen * edgeLen]);
}

// ********************************************************************************
// Overloaded PureJob class to handle generating the mesh for each patch
// ********************************************************************************
void SinglePatchJob::OnFinish() // runs in primary thread of the context
{
	GeoSphere::OnAddSingleSplitResult(mData->sysPath, mpResults);
	mpResults = nullptr;
	BasePatchJob::OnFinish();
}

void SinglePatchJob::OnRun() // RUNS IN ANOTHER THREAD!! MUST BE THREAD SAFE!
{
	PROFILE_SCOPED()
	BasePatchJob::OnRun();
	ScopedGenTimer timer(1);

	const SSingleSplitRequest &srd = *mData;

	// fill out the data
	mData->GenerateMesh();

	// add this patches data
	SSingleSplitResult *sr = new SSingleSplitResult(srd.patchID.GetPatchFaceIdx(), srd.depth);
	sr->addResult(srd.heights, srd.normals, srd.colors,
		srd.v0, srd.v1, srd.v2, srd.v3,
		srd.patchID.NextPatchID(srd.depth + 1, 0));
	// store the result
	mpResults = sr;
}

SinglePatchJob::~SinglePatchJob()
{
	if (mpResults) {
		mpResults->OnCancel();
		delete mpResults;
		mpResults = nullptr;
	}
}

// ********************************************************************************
// Overloaded PureJob class to handle generating the mesh for each patch
// ********************************************************************************
void QuadPatchJob::OnFinish() // runs in primary thread of the context
{
	GeoSphere::OnAddQuadSplitResult(mData->sysPath, mpResults);
	mpResults = nullptr;
	BasePatchJob::OnFinish();
}

void QuadPatchJob::OnRun() // RUNS IN ANOTHER THREAD!! MUST BE THREAD SAFE!
{
	PROFILE_SCOPED()
	BasePatchJob::OnRun();
	ScopedGenTimer timer(4);

	const SQuadSplitRequest &srd = *mData;

	mData->GenerateBorderedData();

	const vector3d v01 = (srd.v0 + srd.v1).Normalized();
	const vector3d v12 = (srd.v1 + srd.v2).Normalized();
	const vector3d v23 = (srd.v2 + srd.v3).Normalized();
	const vector3d v30 = (srd.v3 + srd.v0).Normalized();
	const vector3d cn = (srd.centroid).Normalized();
	const vector3d vecs[4][4] = {
		{ srd.v0, v01, cn, v30 },
		{ v01, srd.v1, v12, cn },
		{ cn, v12, srd.v2, v23 },
		{ v30, cn, v23, srd.v3 }
	};

	const int borderedEdgeLen = (srd.edgeLen * 2) + (BORDER_SIZE * 2) - 1;
	const int offxy[4][2] = {
		{ 0, 0 },
		{ srd.edgeLen - 1, 0 },
		{ srd.edgeLen - 1, srd.edgeLen - 1 },
		{ 0, srd.edgeLen - 1 }
	};

	SQuadSplitResult *sr = new SQuadSplitResult(srd.patchID.GetPatchFaceIdx(), srd.depth);
	for (int i = 0; i < 4; i++) {
		// fill out the data
		mData->GenerateSubPatchData(i,
			vecs[i][0], vecs[i][1], vecs[i][2], vecs[i][3],
			srd.edgeLen, offxy[i][0], offxy[i][1],
			borderedEdgeLen);

		// add this patches data
		sr->addResult(i, srd.heights[i], srd.normals[i], srd.colors[i],
			vecs[i][0], vecs[i][1], vecs[i][2], vecs[i][3],
			srd.patchID.NextPatchID(srd.depth + 1, i));
	}
	mpResults = sr;
}

QuadPatchJob::~QuadPatchJob()
{
	CountQuadPatchJob(-1);
	if (mpResults) {
		mpResults->OnCancel();
		delete mpResults;
		mpResults = NULL;
	}
}

// Generates full-detail vertices, and also non-edge normals and colors
void SQuadSplitRequest::GenerateBorderedData() const
{
	PROFILE_SCOPED()
	const int borderedEdgeLen = (edgeLen * 2) + (BORDER_SIZE * 2) - 1;
	const int numBorderedVerts = borderedEdgeLen * borderedEdgeLen;

	// generate heights plus a N=BORDER_SIZE unit border
	vector3d *fill_verts = borderVertexs.get();
	for (int y = -BORDER_SIZE; y < (borderedEdgeLen - BORDER_SIZE); y++) {
		const double yfrac = double(y) * (fracStep * 0.5);
		for (int x = -BORDER_SIZE; x < (borderedEdgeLen - BORDER_SIZE); x++) {
			const double xfrac = double(x) * (fracStep * 0.5);
			const vector3d p = GetSpherePoint(v0, v1, v2, v3, xfrac, yfrac);
			*(fill_verts++) = p;
		}
	}

	// the bordered grid has the vertex spacing of the child patches (depth + 1)
	const double maxFrequency = NyquistFrequency((v1 - v0).Length() * fracStep * 0.5);

	vector3d *vrts = borderVertexs.get();
	double *bhts = borderHeights.get();
	{
		TerrainNoise::ScopedBandLimit bandLimit(BandLimitHeights(depth + 1, maxDepth) ? maxFrequency : 0.0);
		pTerrain->GetHeights(vrts, bhts, numBorderedVerts);
	}

	for (int iHeights = 0; iHeights < numBorderedVerts; iHeights++) {
		vrts[iHeights] *= (bhts[iHeights] + 1.0);
	}
}

void SQuadSplitRequest::GenerateSubPatchData(
	const int quadrantIndex,
	const vector3d &v0,
	const vector3d &v1,
	const vector3d &v2,
	const vector3d &v3,
	const int edgeLen,
	const int xoff,
	const int yoff,
	const int borderedEdgeLen) const
{
	PROFILE_SCOPED()
	vector3d *vrts = borderVertexs.get();
	// the slope the colours see (COLOUR_SLOPE_SCALE), if not the vertices' own
	std::vector<vector3d> colourNormals;
	const bool fixedSlope = ColourSlopeNormals(pTerrain.Get(), vrts, borderedEdgeLen,
		BORDER_SIZE + xoff, BORDER_SIZE + xoff + edgeLen, BORDER_SIZE + yoff, BORDER_SIZE + yoff + edgeLen, colourNormals);
	const vector3d *colourNormal = colourNormals.data();

	// Generate normals & colors for vertices
	TerrainNoise::ScopedBandLimit bandLimit(NyquistFrequency((v1 - v0).Length() * fracStep));
	const bool debugDepth = DebugPatchDepthColours();
	GeoVertexSurface *col = colors[quadrantIndex];
	vector3f *nrm = normals[quadrantIndex];
	double *hts = heights[quadrantIndex];

	// step over the small square
	for (int y = 0; y < edgeLen; y++) {
		const int by = (y + BORDER_SIZE) + yoff;
		for (int x = 0; x < edgeLen; x++) {
			const int bx = (x + BORDER_SIZE) + xoff;

			// height
			const double height = borderHeights[bx + (by * borderedEdgeLen)];
			assert(hts != &heights[quadrantIndex][edgeLen * edgeLen]);
			*(hts++) = height;

			// normal
			const vector3d &x1 = vrts[(bx - 1) + (by * borderedEdgeLen)];
			const vector3d &x2 = vrts[(bx + 1) + (by * borderedEdgeLen)];
			const vector3d &y1 = vrts[bx + ((by - 1) * borderedEdgeLen)];
			const vector3d &y2 = vrts[bx + ((by + 1) * borderedEdgeLen)];
			const vector3d n = ((x2 - x1).Cross(y2 - y1)).Normalized();
			assert(nrm != &normals[quadrantIndex][edgeLen * edgeLen]);
			*(nrm++) = vector3f(n);

			// color
			const vector3d p = GetSpherePoint(v0, v1, v2, v3, x * fracStep, y * fracStep);
			setSurface(*col, pTerrain.Get(), p, height, fixedSlope ? *(colourNormal++) : n, debugDepth, depth + 1, maxDepth);
			assert(col != &colors[quadrantIndex][edgeLen * edgeLen]);
			++col;
		}
	}
	assert(hts == &heights[quadrantIndex][edgeLen * edgeLen]);
	assert(nrm == &normals[quadrantIndex][edgeLen * edgeLen]);
	assert(col == &colors[quadrantIndex][edgeLen * edgeLen]);
}
