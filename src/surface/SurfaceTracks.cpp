// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "SurfaceTracks.h"

#include "Body.h"
#include "Frame.h"
#include "MathUtil.h"
#include "galaxy/SystemBody.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
	// pressing this far from the plane's anchor starts a new one (m), or this
	// share of the planet's radius on small ones: the plane leaves the ground
	// as the planet curves away under it
	constexpr double REANCHOR_DISTANCE = 5000.0;
	constexpr double REANCHOR_RADIUS_SHARE = 0.05;
	// a full rut on the softest ground (m)
	constexpr double RUT_DEPTH = 0.05;

	// how deep the wheels sink: deep in the dust and sand of airless and dry
	// worlds, less in the soil and plants of living ones and on ice
	double SoftnessOf(const SystemBody *sb)
	{
		if (!sb || sb->GetSuperType() != SystemBody::SUPERTYPE_ROCKY_PLANET)
			return 0.0;
		return (1.0 - 0.4 * Clamp(sb->GetLife(), 0.0, 1.0)) * (1.0 - 0.3 * Clamp(sb->GetVolatileIces(), 0.0, 1.0));
	}

	uint8_t ToByte(double v)
	{
		return uint8_t(Clamp(v, 0.0, 1.0) * 255.0 + 0.5);
	}

	int Wrap(int i)
	{
		return ((i % SurfaceTracks::SIZE) + SurfaceTracks::SIZE) % SurfaceTracks::SIZE;
	}
} // namespace

SurfaceTracks::SurfaceTracks() :
	m_anchor(0.0),
	m_axes(matrix3x3d::Identity)
{
}

SurfaceTracks::~SurfaceTracks() = default;

void SurfaceTracks::Clear()
{
	m_frame = FrameId();
	m_softness = 0.0;
	std::fill(m_texels.begin(), m_texels.end(), uint8_t(0));
	m_dirty.clear();
	m_allDirty = true;
}

void SurfaceTracks::Anchor(FrameId frame, const vector3d &pos)
{
	// (the map taken once there are tracks to keep)
	if (m_texels.empty())
		m_texels.assign(size_t(SIZE) * SIZE * 4, 0);
	Clear();
	m_frame = frame;
	const Frame *f = Frame::GetFrame(frame);
	const SystemBody *sb = f && f->GetBody() ? f->GetBody()->GetSystemBody() : nullptr;
	m_softness = SoftnessOf(sb);
	m_reanchorDistance = sb ? std::min(REANCHOR_DISTANCE, REANCHOR_RADIUS_SHARE * sb->GetRadius()) : REANCHOR_DISTANCE;

	// x and z along the ground, y up
	const vector3d up = pos.Normalized();
	const vector3d axis = std::abs(up.y) < 0.9 ? vector3d(0.0, 1.0, 0.0) : vector3d(1.0, 0.0, 0.0);
	const vector3d x = axis.Cross(up).Normalized();
	const vector3d z = x.Cross(up);
	m_anchor = pos;
	m_axes = matrix3x3d::FromVectors(x, up, z);
	m_centre[0] = m_centre[1] = 0;
}

vector3d SurfaceTracks::ToPlane(const vector3d &pos) const
{
	return m_axes.Transpose() * (pos - m_anchor);
}

uint8_t *SurfaceTracks::TexelAt(int i, int j)
{
	return &m_texels[(size_t(Wrap(j)) * SIZE + Wrap(i)) * 4];
}

void SurfaceTracks::MarkDirty(int i0, int i1, int j0, int j1)
{
	if (m_allDirty)
		return;
	// split where it wraps round the texture's edges
	const int w = i1 - i0 + 1, h = j1 - j0 + 1;
	const int x = Wrap(i0), y = Wrap(j0);
	const int w0 = std::min(w, SIZE - x), h0 = std::min(h, SIZE - y);
	m_dirty.push_back({ x, y, w0, h0 });
	if (w > w0)
		m_dirty.push_back({ 0, y, w - w0, h0 });
	if (h > h0)
		m_dirty.push_back({ x, 0, w0, h - h0 });
	if (w > w0 && h > h0)
		m_dirty.push_back({ 0, 0, w - w0, h - h0 });
}

void SurfaceTracks::MoveWindow(double x, double z)
{
	// moved on once the point is a quarter of the way from the centre to the edge
	const int target[2] = { int(std::floor(x / TEXEL)), int(std::floor(z / TEXEL)) };
	if (std::abs(target[0] - m_centre[0]) < SIZE / 4 && std::abs(target[1] - m_centre[1]) < SIZE / 4)
		return;

	const int half = SIZE / 2;
	for (int axis = 0; axis < 2; axis++) {
		const int from = m_centre[axis], to = target[axis];
		if (from == to)
			continue;
		// the texels of the old window outside the new one
		int lo, hi;
		if (to > from) {
			lo = from - half;
			hi = std::min(to - half, from + half);
		} else {
			lo = std::max(to + half, from - half);
			hi = from + half;
		}
		if (hi <= lo)
			continue;
		if (axis == 0) {
			// columns: on each row a span, or two where it wraps
			const int x0 = Wrap(lo), n = hi - lo;
			const int first = std::min(n, SIZE - x0);
			for (int row = 0; row < SIZE; row++) {
				uint8_t *r = &m_texels[size_t(row) * SIZE * 4];
				std::memset(r + size_t(x0) * 4, 0, size_t(first) * 4);
				if (n > first)
					std::memset(r, 0, size_t(n - first) * 4);
			}
			MarkDirty(lo, hi - 1, 0, SIZE - 1);
		} else {
			// rows: whole ones
			for (int k = lo; k < hi; k++)
				std::memset(&m_texels[size_t(Wrap(k)) * SIZE * 4], 0, size_t(SIZE) * 4);
			MarkDirty(0, SIZE - 1, lo, hi - 1);
		}
	}
	m_centre[0] = target[0];
	m_centre[1] = target[1];
}

void SurfaceTracks::Press(FrameId frame, const vector3d &a, const vector3d &b, double halfWidth)
{
	if (frame != m_frame || (b - m_anchor).LengthSqr() > m_reanchorDistance * m_reanchorDistance)
		Anchor(frame, b);
	if (m_softness <= 0.0)
		return;

	const vector3d pa = ToPlane(a);
	const vector3d pb = ToPlane(b);
	MoveWindow(pb.x, pb.z);

	// in texels: the tyre's footprint swept from a to b, a capsule
	const double ax = pa.x / TEXEL, az = pa.z / TEXEL;
	const double dx = (pb.x - pa.x) / TEXEL, dz = (pb.z - pa.z) / TEXEL;
	const double len = std::sqrt(dx * dx + dz * dz);
	if (len < 0.1)
		return;
	const double ux = dx / len, uz = dz / len;
	const double hw = halfWidth / TEXEL;

	const int half = SIZE / 2;
	const int i0 = std::max(int(std::floor(std::min(ax, ax + dx) - hw - 1.0)), m_centre[0] - half);
	const int i1 = std::min(int(std::ceil(std::max(ax, ax + dx) + hw + 1.0)), m_centre[0] + half - 1);
	const int j0 = std::max(int(std::floor(std::min(az, az + dz) - hw - 1.0)), m_centre[1] - half);
	const int j1 = std::min(int(std::ceil(std::max(az, az + dz) + hw + 1.0)), m_centre[1] + half - 1);
	if (i1 < i0 || j1 < j0)
		return;
	const uint8_t dirX = ToByte(ux * 0.5 + 0.5), dirZ = ToByte(uz * 0.5 + 0.5);

	for (int j = j0; j <= j1; j++) {
		for (int i = i0; i <= i1; i++) {
			// the texel's centre from a: along the way and across it
			const double cx = i + 0.5 - ax, cz = j + 0.5 - az;
			const double along = Clamp(cx * ux + cz * uz, 0.0, len);
			const double across = -cx * uz + cz * ux;
			const double ex = cx - ux * along, ez = cz - uz * along;
			// full under the tyre, its edge a texel soft
			const double cover = Clamp(hw - std::sqrt(ex * ex + ez * ez) + 0.5, 0.0, 1.0);
			if (cover <= 0.0)
				continue;
			uint8_t *t = TexelAt(i, j);
			const uint8_t depth = ToByte(cover);
			if (depth < t[0])
				continue;
			t[0] = depth;
			t[1] = ToByte(across / (2.0 * hw) + 0.5);
			t[2] = dirX;
			t[3] = dirZ;
		}
	}
	MarkDirty(i0, i1, j0, j1);
}

bool SurfaceTracks::GetRenderData(Graphics::Renderer *renderer, FrameId frame, const matrix4x4d &frameToView,
	Graphics::SurfaceTrackDataBlock &out, Graphics::Texture *&texture)
{
	if (!m_frame.valid() || frame != m_frame || m_softness <= 0.0)
		return false;

	if (!m_texture) {
		const vector3f size(float(SIZE), float(SIZE), 0.0f);
		const Graphics::TextureDescriptor desc(Graphics::TEXTURE_RGBA_8888, size, Graphics::LINEAR_REPEAT, false, false, false, 0, Graphics::TEXTURE_2D);
		m_texture.Reset(renderer->CreateTexture(desc));
		m_allDirty = true;
	}
	// what changed: whole rows straight from the map, else packed row by row
	if (m_allDirty) {
		m_texture->Update(m_texels.data(), vector2f(0.0f, 0.0f), vector3f(float(SIZE), float(SIZE), 0.0f), Graphics::TEXTURE_RGBA_8888);
		m_allDirty = false;
		m_dirty.clear();
	}
	for (const Rect &r : m_dirty) {
		const uint8_t *data;
		if (r.x == 0 && r.w == SIZE) {
			data = &m_texels[size_t(r.y) * SIZE * 4];
		} else {
			m_upload.resize(size_t(r.w) * r.h * 4);
			for (int row = 0; row < r.h; row++)
				std::memcpy(&m_upload[size_t(row) * r.w * 4], &m_texels[(size_t(r.y + row) * SIZE + r.x) * 4], size_t(r.w) * 4);
			data = m_upload.data();
		}
		m_texture->Update(data, vector2f(float(r.x), float(r.y)), vector3f(float(r.w), float(r.h), 0.0f), Graphics::TEXTURE_RGBA_8888);
	}
	m_dirty.clear();

	// view space -> the plane
	const matrix3x3d toPlane = m_axes.Transpose();
	const matrix4x4d frameToPlane(toPlane, -(toPlane * m_anchor));
	out.viewToTrack = matrix4x4f(frameToPlane * frameToView.Inverse());
	out.window[0] = float(m_centre[0]);
	out.window[1] = float(m_centre[1]);
	out.window[2] = float(TEXEL);
	out.window[3] = float(SIZE);
	out.params[0] = float(m_softness);
	out.params[1] = float(RUT_DEPTH);
	out.params[2] = out.params[3] = 0.0f;
	texture = m_texture.Get();
	return true;
}
