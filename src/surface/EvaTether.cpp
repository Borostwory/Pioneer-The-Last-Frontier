// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "EvaTether.h"

#include "Color.h"
#include "MathUtil.h"
#include "Pi.h"
#include "Walker.h"
#include "graphics/Material.h"
#include "graphics/RenderState.h"
#include "graphics/Renderer.h"
#include "graphics/VertexArray.h"
#include "graphics/VertexBuffer.h"

#include <algorithm>

namespace {
	constexpr int NUM_POINTS = 32;
	constexpr double SUBSTEP = 1.0 / 120.0; // s
	constexpr int MAX_SUBSTEPS = 8;			// per frame, after a long one
	constexpr int ITERATIONS = 16;			// constraint passes per substep
	// velocity kept per substep: space has no air, but a real line is not
	// perfectly elastic either
	constexpr double DAMPING = 0.995;
	// paid out: the distance times SLACK plus EXTRA metres, up to the walker's
	// tether limit, where it is taut
	constexpr double SLACK = 1.1;
	constexpr double EXTRA = 1.5;
	// how fast slack is taken up, per second of the excess
	constexpr double REEL_IN_RATE = 0.5;
	// with less slack than this (metres) it pulls straight: each substep it
	// moves this much of the way to the straight line, scaled by how taut it is
	constexpr double TAUT_SLACK = 4.0;
	constexpr double TAUT_PULL = 0.3;

	// the cord: radius (m), and white shading from its middle to its edges
	constexpr double RADIUS = 0.012;
	const Color MIDDLE_COLOUR(255, 255, 255, 255);
	const Color EDGE_COLOUR(120, 120, 125, 255);
} // namespace

EvaTether::EvaTether()
{
	Graphics::RenderStateDesc rsd;
	rsd.cullMode = Graphics::CULL_NONE; // a strip seen from either side
	rsd.primitiveType = Graphics::TRIANGLES;
	const auto format = Graphics::VertexFormatDesc::FromAttribSet(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_DIFFUSE);
	m_material.reset(Pi::renderer->CreateMaterial("vtxColor", Graphics::MaterialDescriptor(), rsd, format));
}

EvaTether::~EvaTether() {}

void EvaTether::Clear()
{
	m_points.clear();
	m_previous.clear();
	m_length = 0.0;
	m_accumulator = 0.0;
}

void EvaTether::Update(const vector3d &anchor, const vector3d &end, double dt, bool taut)
{
	const double distance = (end - anchor).Length();
	if (m_points.empty()) {
		for (int i = 0; i < NUM_POINTS; i++)
			m_points.push_back(anchor + (end - anchor) * (double(i) / (NUM_POINTS - 1)));
		m_previous = m_points;
		m_length = distance;
	}

	// never shorter than the ends are apart: at the limit the walker is held
	// by it, a little farther than EVA_TETHER from the hatch at most
	const double wanted = std::min(distance * SLACK + EXTRA, Walker::EVA_TETHER);
	if (wanted > m_length)
		m_length = wanted;
	else
		m_length += (wanted - m_length) * std::min(REEL_IN_RATE * dt, 1.0);
	m_length = std::max(m_length, distance);
	// holding the walker, it has no slack at all
	if (taut)
		m_length = distance;

	m_accumulator = std::min(m_accumulator + dt, SUBSTEP * MAX_SUBSTEPS);
	while (m_accumulator >= SUBSTEP) {
		Step(anchor, end, SUBSTEP);
		m_accumulator -= SUBSTEP;
	}
	m_points.front() = anchor;
	m_points.back() = end;
}

void EvaTether::Step(const vector3d &anchor, const vector3d &end, double dt)
{
	// coast (Verlet: the velocity is the last move)
	for (int i = 1; i < NUM_POINTS - 1; i++) {
		const vector3d moved = (m_points[i] - m_previous[i]) * DAMPING;
		m_previous[i] = m_points[i];
		m_points[i] += moved;
	}
	m_points.front() = m_previous.front() = anchor;
	m_points.back() = m_previous.back() = end;

	// keep each segment its length; the pinned ends don't move
	const double segment = m_length / (NUM_POINTS - 1);
	for (int k = 0; k < ITERATIONS; k++) {
		for (int i = 0; i < NUM_POINTS - 1; i++) {
			const vector3d d = m_points[i + 1] - m_points[i];
			const double len = d.Length();
			if (len < 1e-9)
				continue;
			const vector3d fix = d * ((len - segment) / len);
			const bool firstPinned = i == 0;
			const bool secondPinned = i + 1 == NUM_POINTS - 1;
			if (firstPinned && secondPinned)
				continue;
			if (firstPinned)
				m_points[i + 1] -= fix;
			else if (secondPinned)
				m_points[i] += fix;
			else {
				m_points[i] += fix * 0.5;
				m_points[i + 1] -= fix * 0.5;
			}
		}
	}

	// pulled taut it is straight: the constraints alone converge slowly over
	// 32 points, so draw it to the line between the ends, and calm it
	const double slack = m_length - (end - anchor).Length();
	if (slack < TAUT_SLACK) {
		const double pull = TAUT_PULL * (1.0 - slack / TAUT_SLACK);
		for (int i = 1; i < NUM_POINTS - 1; i++) {
			const vector3d straight = anchor + (end - anchor) * (double(i) / (NUM_POINTS - 1));
			const vector3d shift = (straight - m_points[i]) * pull;
			m_points[i] += shift;
			m_previous[i] += shift;
		}
	}
}

void EvaTether::Draw(Graphics::Renderer *r, const matrix3x3d &rootToView, const vector3d &shipInView)
{
	const size_t count = m_points.size();
	if (count < 2)
		return;

	std::vector<vector3d> view(count);
	for (size_t i = 0; i < count; i++)
		view[i] = rootToView * m_points[i] + shipInView;

	// across the cord at each point: square to it and to the line of sight
	// (the camera is at the origin), so the strip faces the camera
	std::vector<vector3f> across(count);
	for (size_t i = 0; i < count; i++) {
		const vector3d along = view[std::min(i + 1, count - 1)] - view[i > 0 ? i - 1 : 0];
		vector3d side = along.Cross(view[i]);
		side = side.LengthSqr() > 1e-18 ? side.Normalized() : vector3d(1.0, 0.0, 0.0);
		across[i] = vector3f(side * RADIUS);
	}

	// two quads per segment, edge to middle and middle to edge
	Graphics::VertexArray va(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_DIFFUSE, int((count - 1) * 12));
	for (size_t i = 0; i + 1 < count; i++) {
		const vector3f a(view[i]), b(view[i + 1]);
		for (const float side : { -1.0f, 1.0f }) {
			const vector3f aEdge = a + across[i] * side, bEdge = b + across[i + 1] * side;
			va.Add(a, MIDDLE_COLOUR);
			va.Add(aEdge, EDGE_COLOUR);
			va.Add(bEdge, EDGE_COLOUR);
			va.Add(a, MIDDLE_COLOUR);
			va.Add(bEdge, EDGE_COLOUR);
			va.Add(b, MIDDLE_COLOUR);
		}
	}

	r->SetTransform(matrix4x4f::Identity);
	r->DrawBuffer(&va, m_material.get());
}
