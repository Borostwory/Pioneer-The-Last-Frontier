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

// The tracks the rover's wheels and the walker's boots leave on soft ground,
// drawn per pixel by the terrain shaders (surface-tracks.glsl) so they lie
// on the ground as drawn, lit as it is. They are pressed into maps on a
// plane tangent to the planet at an anchor: one for the wheels (WHEEL_SIZE
// texels of WHEEL_TEXEL metres a side: 410 m), a finer one for the boots
// (FOOT_SIZE of FOOT_TEXEL: 41 m). Each follows what presses it: it is a
// ring (texels addressed modulo its size, the texture repeats), so moving
// it clears the texels it leaves behind instead of moving the rest, and only
// what changed is uploaded; the tracks stay at least a quarter of its width
// behind. Texels: R the depth (0..1), G where across the tyre or the sole it
// is (0..1, for the tread), BA the way it went along the plane (x, z, 0..1
// for -1..1).
//
// Only on the terrain of a rocky planet or moon (not on pads, roads or
// buildings, nor steep rock, which the shader leaves out), as deep as its
// ground is soft (SoftnessOf). Kept while the player is on the planet; gone
// on leaving it, moving far off, or loading a game.
class SurfaceTracks {
public:
	static constexpr int WHEEL_SIZE = 4096;	   // 64 MB, here and on the GPU
	static constexpr double WHEEL_TEXEL = 0.1; // m
	static constexpr int FOOT_SIZE = 2048;	   // 16 MB
	static constexpr double FOOT_TEXEL = 0.02;

	SurfaceTracks();
	~SurfaceTracks();

	// a wheel pressed into the ground from a to b (in frame, the planet's
	// rotating frame, on the ground), its tyre halfWidth (m) wide each side
	void Press(FrameId frame, const vector3d &a, const vector3d &b, double halfWidth);
	// a boot's print at pos (in frame, on the ground), its toe along forward
	void PressFoot(FrameId frame, const vector3d &pos, const vector3d &forward);
	void Clear();
	// the planet frame they are in, invalid while there are none
	FrameId GetFrame() const { return m_frame; }

	// for the camera's frame, with frameToView the frame in view space:
	// uploads what changed and fills the shaders' data; false if there are
	// no tracks to draw
	bool GetRenderData(Graphics::Renderer *renderer, FrameId frame, const matrix4x4d &frameToView,
		Graphics::SurfaceTrackDataBlock &out, Graphics::Texture *&wheels, Graphics::Texture *&feet);

private:
	// one map: a ring of size x size texels, texel metres each
	struct Layer {
		Layer(int size, double texel) :
			size(size), texel(texel) {}
		const int size;
		const double texel;
		int centre[2] = { 0, 0 };	 // the window's centre texel (unwrapped)
		std::vector<uint8_t> texels; // RGBA, row by row
		struct Rect {
			int x, y, w, h; // in the texture
		};
		std::vector<Rect> dirty;
		bool allDirty = false;
		bool used = false;			 // pressed since it was last cleared
		std::vector<uint8_t> upload; // a rect's rows, packed
		RefCountedPtr<Graphics::Texture> texture;

		int Wrap(int i) const { return ((i % size) + size) % size; }
		uint8_t *TexelAt(int i, int j) { return &texels[(size_t(Wrap(j)) * size + Wrap(i)) * 4]; }
		void Clear();
		// texels i0..i1, j0..j1 (unwrapped, inclusive, at most size each way) to upload
		void MarkDirty(int i0, int i1, int j0, int j1);
		// the window round the plane position (x, z) (m), clearing the texels it leaves
		void MoveWindow(double x, double z);
		// texels i0..i1, j0..j1 cut to the window; false if none are left
		bool ClipToWindow(int &i0, int &i1, int &j0, int &j1) const;
		// a texel pressed this deep (0..1) unless it is deeper, where across
		// the tyre or sole (0..1), going along (ux, uz)
		void Write(int i, int j, double depth, double across, double ux, double uz);
		// the texture made, and what changed uploaded
		void Upload(Graphics::Renderer *renderer);
	};

	// the plane for a press at pos: a new one on another planet or too far
	// from its anchor; false where nothing leaves tracks
	bool Reach(FrameId frame, const vector3d &pos);
	// the plane at the ground under pos, and the softness of its planet
	void Anchor(FrameId frame, const vector3d &pos);
	vector3d ToPlane(const vector3d &pos) const;

	FrameId m_frame;
	vector3d m_anchor;
	matrix3x3d m_axes; // columns: the plane's x, up and z, in the frame
	double m_softness = 0.0;
	double m_reanchorDistance = 0.0; // m
	Layer m_wheels;
	Layer m_feet;
};

#endif /* _SURFACETRACKS_H */
