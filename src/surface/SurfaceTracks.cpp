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
	// a boot's print: a full one this deep (m), its sole this long and wide
	// at the ball of the foot and the heel (m), the heel's middle this far
	// back from the print's, the ball's this far forward
	constexpr double FOOT_DEPTH = 0.03;
	constexpr double SOLE_LENGTH = 0.30;
	constexpr double SOLE_BALL_WIDTH = 0.115;
	constexpr double SOLE_HEEL_WIDTH = 0.085;

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
} // namespace

void SurfaceTracks::Layer::Clear()
{
	// (the map taken once there are tracks to keep)
	if (texels.empty())
		texels.assign(size_t(size) * size * 4, 0);
	else
		std::fill(texels.begin(), texels.end(), uint8_t(0));
	dirty.clear();
	allDirty = true;
	used = false;
	centre[0] = centre[1] = 0;
}

void SurfaceTracks::Layer::MarkDirty(int i0, int i1, int j0, int j1)
{
	if (allDirty)
		return;
	// split where it wraps round the texture's edges
	const int w = i1 - i0 + 1, h = j1 - j0 + 1;
	const int x = Wrap(i0), y = Wrap(j0);
	const int w0 = std::min(w, size - x), h0 = std::min(h, size - y);
	dirty.push_back({ x, y, w0, h0 });
	if (w > w0)
		dirty.push_back({ 0, y, w - w0, h0 });
	if (h > h0)
		dirty.push_back({ x, 0, w0, h - h0 });
	if (w > w0 && h > h0)
		dirty.push_back({ 0, 0, w - w0, h - h0 });
}

void SurfaceTracks::Layer::MoveWindow(double x, double z)
{
	// moved on once the point is a quarter of the way from the centre to the edge
	const int target[2] = { int(std::floor(x / texel)), int(std::floor(z / texel)) };
	if (std::abs(target[0] - centre[0]) < size / 4 && std::abs(target[1] - centre[1]) < size / 4)
		return;

	const int half = size / 2;
	for (int axis = 0; axis < 2; axis++) {
		const int from = centre[axis], to = target[axis];
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
			const int first = std::min(n, size - x0);
			for (int row = 0; row < size; row++) {
				uint8_t *r = &texels[size_t(row) * size * 4];
				std::memset(r + size_t(x0) * 4, 0, size_t(first) * 4);
				if (n > first)
					std::memset(r, 0, size_t(n - first) * 4);
			}
			MarkDirty(lo, hi - 1, 0, size - 1);
		} else {
			// rows: whole ones
			for (int k = lo; k < hi; k++)
				std::memset(&texels[size_t(Wrap(k)) * size * 4], 0, size_t(size) * 4);
			MarkDirty(0, size - 1, lo, hi - 1);
		}
	}
	centre[0] = target[0];
	centre[1] = target[1];
}

bool SurfaceTracks::Layer::ClipToWindow(int &i0, int &i1, int &j0, int &j1) const
{
	const int half = size / 2;
	i0 = std::max(i0, centre[0] - half);
	i1 = std::min(i1, centre[0] + half - 1);
	j0 = std::max(j0, centre[1] - half);
	j1 = std::min(j1, centre[1] + half - 1);
	return i0 <= i1 && j0 <= j1;
}

void SurfaceTracks::Layer::Write(int i, int j, double depth, double across, double ux, double uz)
{
	uint8_t *t = TexelAt(i, j);
	const uint8_t d = ToByte(depth);
	if (d == 0 || d < t[0])
		return;
	t[0] = d;
	t[1] = ToByte(across);
	t[2] = ToByte(ux * 0.5 + 0.5);
	t[3] = ToByte(uz * 0.5 + 0.5);
	used = true;
}

void SurfaceTracks::Layer::Upload(Graphics::Renderer *renderer)
{
	if (!texture) {
		const vector3f dim(float(size), float(size), 0.0f);
		const Graphics::TextureDescriptor desc(Graphics::TEXTURE_RGBA_8888, dim, Graphics::LINEAR_REPEAT, false, false, false, 0, Graphics::TEXTURE_2D);
		texture.Reset(renderer->CreateTexture(desc));
		allDirty = true;
	}
	// what changed: whole rows straight from the map, else packed row by row
	if (allDirty) {
		texture->Update(texels.data(), vector2f(0.0f, 0.0f), vector3f(float(size), float(size), 0.0f), Graphics::TEXTURE_RGBA_8888);
		allDirty = false;
		dirty.clear();
	}
	for (const Rect &r : dirty) {
		const uint8_t *data;
		if (r.x == 0 && r.w == size) {
			data = &texels[size_t(r.y) * size * 4];
		} else {
			upload.resize(size_t(r.w) * r.h * 4);
			for (int row = 0; row < r.h; row++)
				std::memcpy(&upload[size_t(row) * r.w * 4], &texels[(size_t(r.y + row) * size + r.x) * 4], size_t(r.w) * 4);
			data = upload.data();
		}
		texture->Update(data, vector2f(float(r.x), float(r.y)), vector3f(float(r.w), float(r.h), 0.0f), Graphics::TEXTURE_RGBA_8888);
	}
	dirty.clear();
}

SurfaceTracks::SurfaceTracks() :
	m_anchor(0.0),
	m_axes(matrix3x3d::Identity),
	m_wheels(WHEEL_SIZE, WHEEL_TEXEL),
	m_feet(FOOT_SIZE, FOOT_TEXEL)
{
}

SurfaceTracks::~SurfaceTracks() = default;

void SurfaceTracks::Clear()
{
	m_frame = FrameId();
	m_softness = 0.0;
	if (!m_wheels.texels.empty())
		m_wheels.Clear();
	if (!m_feet.texels.empty())
		m_feet.Clear();
}

void SurfaceTracks::Anchor(FrameId frame, const vector3d &pos)
{
	m_wheels.Clear();
	m_feet.Clear();
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
}

bool SurfaceTracks::Reach(FrameId frame, const vector3d &pos)
{
	if (frame != m_frame || (pos - m_anchor).LengthSqr() > m_reanchorDistance * m_reanchorDistance)
		Anchor(frame, pos);
	return m_softness > 0.0;
}

vector3d SurfaceTracks::ToPlane(const vector3d &pos) const
{
	return m_axes.Transpose() * (pos - m_anchor);
}

void SurfaceTracks::Press(FrameId frame, const vector3d &a, const vector3d &b, double halfWidth)
{
	if (!Reach(frame, b))
		return;
	Layer &l = m_wheels;
	const vector3d pa = ToPlane(a);
	const vector3d pb = ToPlane(b);
	l.MoveWindow(pb.x, pb.z);

	// in texels: the tyre's footprint swept from a to b, a capsule
	const double ax = pa.x / l.texel, az = pa.z / l.texel;
	const double dx = (pb.x - pa.x) / l.texel, dz = (pb.z - pa.z) / l.texel;
	const double len = std::sqrt(dx * dx + dz * dz);
	if (len < 0.1)
		return;
	const double ux = dx / len, uz = dz / len;
	const double hw = halfWidth / l.texel;

	int i0 = int(std::floor(std::min(ax, ax + dx) - hw - 1.0)), i1 = int(std::ceil(std::max(ax, ax + dx) + hw + 1.0));
	int j0 = int(std::floor(std::min(az, az + dz) - hw - 1.0)), j1 = int(std::ceil(std::max(az, az + dz) + hw + 1.0));
	if (!l.ClipToWindow(i0, i1, j0, j1))
		return;
	for (int j = j0; j <= j1; j++) {
		for (int i = i0; i <= i1; i++) {
			// the texel's centre from a: along the way and across it
			const double cx = i + 0.5 - ax, cz = j + 0.5 - az;
			const double along = Clamp(cx * ux + cz * uz, 0.0, len);
			const double across = -cx * uz + cz * ux;
			const double ex = cx - ux * along, ez = cz - uz * along;
			// full under the tyre, its edge a texel soft
			const double cover = Clamp(hw - std::sqrt(ex * ex + ez * ez) + 0.5, 0.0, 1.0);
			if (cover > 0.0)
				l.Write(i, j, cover, across / (2.0 * hw) + 0.5, ux, uz);
		}
	}
	l.MarkDirty(i0, i1, j0, j1);
}

void SurfaceTracks::PressFoot(FrameId frame, const vector3d &pos, const vector3d &forward)
{
	if (!Reach(frame, pos))
		return;
	Layer &l = m_feet;
	const vector3d p = ToPlane(pos);
	const vector3d f = m_axes.Transpose() * forward;
	const double flen = std::sqrt(f.x * f.x + f.z * f.z);
	if (flen < 1e-6)
		return;
	l.MoveWindow(p.x, p.z);

	// in texels: the sole, two ovals (the ball of the foot and the heel)
	// blended along its length, its edge a texel soft
	const double ux = f.x / flen, uz = f.z / flen;
	const double cx = p.x / l.texel, cz = p.z / l.texel;
	const double halfLength = 0.5 * SOLE_LENGTH / l.texel;
	const double ball = 0.5 * SOLE_BALL_WIDTH / l.texel, heel = 0.5 * SOLE_HEEL_WIDTH / l.texel;
	const double r = halfLength + 1.0;
	int i0 = int(std::floor(cx - r)), i1 = int(std::ceil(cx + r));
	int j0 = int(std::floor(cz - r)), j1 = int(std::ceil(cz + r));
	if (!l.ClipToWindow(i0, i1, j0, j1))
		return;
	for (int j = j0; j <= j1; j++) {
		for (int i = i0; i <= i1; i++) {
			const double ex = i + 0.5 - cx, ez = j + 0.5 - cz;
			const double along = ex * ux + ez * uz; // toe ahead
			const double across = -ex * uz + ez * ux;
			// how wide the sole is there: narrow at the heel, widest at the
			// ball, rounded off at both ends; and how far in from its edge
			const double s = Clamp(along / halfLength, -1.0, 1.0);
			const double width = (heel + (ball - heel) * Clamp(0.5 + 0.8 * s, 0.0, 1.0)) * std::sqrt(std::max(0.0, 1.0 - std::pow(std::abs(s), 4.0)));
			const double inside = std::min(width - std::abs(across), halfLength - std::abs(along));
			const double cover = Clamp(inside + 0.5, 0.0, 1.0);
			if (cover > 0.0)
				l.Write(i, j, cover * (FOOT_DEPTH / RUT_DEPTH), across / (2.0 * ball) + 0.5, ux, uz);
		}
	}
	l.MarkDirty(i0, i1, j0, j1);
}

bool SurfaceTracks::GetRenderData(Graphics::Renderer *renderer, FrameId frame, const matrix4x4d &frameToView,
	Graphics::SurfaceTrackDataBlock &out, Graphics::Texture *&wheels, Graphics::Texture *&feet)
{
	if (!m_frame.valid() || frame != m_frame || m_softness <= 0.0 || (!m_wheels.used && !m_feet.used))
		return false;
	m_wheels.Upload(renderer);
	m_feet.Upload(renderer);

	// view space -> the plane
	const matrix3x3d toPlane = m_axes.Transpose();
	const matrix4x4d frameToPlane(toPlane, -(toPlane * m_anchor));
	out.viewToTrack = matrix4x4f(frameToPlane * frameToView.Inverse());
	out.wheels[0] = float(m_wheels.centre[0]);
	out.wheels[1] = float(m_wheels.centre[1]);
	out.wheels[2] = float(m_wheels.texel);
	out.wheels[3] = m_wheels.used ? float(m_wheels.size) : 0.0f;
	out.feet[0] = float(m_feet.centre[0]);
	out.feet[1] = float(m_feet.centre[1]);
	out.feet[2] = float(m_feet.texel);
	out.feet[3] = m_feet.used ? float(m_feet.size) : 0.0f;
	out.params[0] = float(m_softness);
	out.params[1] = float(RUT_DEPTH);
	out.params[2] = out.params[3] = 0.0f;
	wheels = m_wheels.texture.Get();
	feet = m_feet.texture.Get();
	return true;
}
