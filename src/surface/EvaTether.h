// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _EVATETHER_H
#define _EVATETHER_H

#include "matrix3x3.h"
#include "vector3.h"

#include <memory>
#include <vector>

namespace Graphics {
	class Material;
	class Renderer;
} // namespace Graphics

// The line between the player on EVA and the ship. A rope of point masses,
// Verlet integrated with distance constraints, no gravity; the reel pays it
// out at once and takes up slack slowly, up to Walker::EVA_TETHER, where it
// pulls taut and straight as it holds the walker. Its points are relative
// to the ship in root frame axes, as the walker's EVA state, so the ship's
// orbit doesn't drag them. Drawn as a white cord: a strip turned to face
// the camera, shaded darker at its edges.
class EvaTether {
public:
	EvaTether();
	~EvaTether();

	bool IsActive() const { return !m_points.empty(); }
	void Clear();
	// pins the ends (relative to the ship, root axes) and steps the rope by
	// dt seconds; starts straight between them. taut: the line is holding the
	// walker back (Walker::IsEvaTetherTaut), so it is as long as the ends are apart
	void Update(const vector3d &anchor, const vector3d &end, double dt, bool taut);
	// rootToView: root axes to camera space; shipInView: the ship in camera space
	void Draw(Graphics::Renderer *r, const matrix3x3d &rootToView, const vector3d &shipInView);

private:
	void Step(const vector3d &anchor, const vector3d &end, double dt);

	std::vector<vector3d> m_points;
	std::vector<vector3d> m_previous;
	double m_length = 0.0; // paid out
	double m_accumulator = 0.0;

	std::unique_ptr<Graphics::Material> m_material;
};

#endif /* _EVATETHER_H */
