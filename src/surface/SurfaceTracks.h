// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _SURFACETRACKS_H
#define _SURFACETRACKS_H

#include "FrameId.h"
#include "RefCounted.h"
#include "matrix3x3.h"
#include "matrix4x4.h"
#include "vector3.h"

#include <cstdint>
#include <vector>

namespace Graphics {
	class Renderer;
	class Texture;
	struct SurfaceTrackDataBlock;
} // namespace Graphics

// The tracks the rover's wheels leave on soft ground, drawn per pixel by the
// terrain shaders (surface-tracks.glsl) so they lie on the ground as drawn,
// lit as it is. A map of SIZE x SIZE texels, TEXEL metres each, on a plane
// tangent to the planet at an anchor, follows the rover: it is a ring
// (texels addressed modulo SIZE, the texture repeats), so moving it clears
// the texels it leaves behind instead of moving the rest, and only what
// changed is uploaded. Texels: R the rut's depth (0..1), G where across the
// tyre it is (0..1, for the tread), BA the way it went along the plane (x,
// z, 0..1 for -1..1). 4096 texels of 10 cm: 410 m across, the tracks kept
// at least a quarter of that behind the rover (64 MB here and on the GPU).
//
// Only on the terrain of a rocky planet or moon (not on pads, roads or
// buildings, nor steep rock, which the shader leaves out), as deep as its
// ground is soft (SoftnessOf). Kept while the player is on the planet; gone
// on leaving it, moving far off, or loading a game.
class SurfaceTracks {
public:
	static constexpr int SIZE = 4096;
	static constexpr double TEXEL = 0.1; // m

	SurfaceTracks();
	~SurfaceTracks();

	// a wheel pressed into the ground from a to b (in frame, the planet's
	// rotating frame, on the ground), its tyre halfWidth (m) wide each side
	void Press(FrameId frame, const vector3d &a, const vector3d &b, double halfWidth);
	void Clear();
	// the planet frame they are in, invalid while there are none
	FrameId GetFrame() const { return m_frame; }

	// for the camera's frame, with frameToView the frame in view space:
	// uploads what changed and fills the shaders' data; false if there are
	// no tracks to draw
	bool GetRenderData(Graphics::Renderer *renderer, FrameId frame, const matrix4x4d &frameToView,
		Graphics::SurfaceTrackDataBlock &out, Graphics::Texture *&texture);

private:
	// the plane at the ground under pos, and the softness of its planet
	void Anchor(FrameId frame, const vector3d &pos);
	// the window round the plane position (x, z) (m), clearing the texels it leaves
	void MoveWindow(double x, double z);
	vector3d ToPlane(const vector3d &pos) const;
	uint8_t *TexelAt(int i, int j);
	// texels i0..i1, j0..j1 (unwrapped, inclusive, at most SIZE each way)
	// to upload
	void MarkDirty(int i0, int i1, int j0, int j1);

	FrameId m_frame;
	vector3d m_anchor;
	matrix3x3d m_axes;	   // columns: the plane's x, up and z, in the frame
	double m_softness = 0.0;
	double m_reanchorDistance = 0.0; // m
	int m_centre[2] = { 0, 0 };		 // the window's centre texel (unwrapped)

	std::vector<uint8_t> m_texels; // RGBA, row by row
	struct Rect {
		int x, y, w, h; // in the texture
	};
	std::vector<Rect> m_dirty;
	bool m_allDirty = false;
	std::vector<uint8_t> m_upload; // a rect's rows, packed
	RefCountedPtr<Graphics::Texture> m_texture;
};

#endif /* _SURFACETRACKS_H */
