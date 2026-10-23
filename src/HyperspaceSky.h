// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _HYPERSPACESKY_H
#define _HYPERSPACESKY_H

#include "Background.h"
#include "RefCounted.h"
#include "galaxy/SystemPath.h"
#include "matrix4x4.h"
#include "vector3.h"

#include <memory>

class Galaxy;

namespace Graphics {
	class Material;
	class Renderer;
} // namespace Graphics

/*
 * The sky seen during a hyperspace transit: the departure system's sky until
 * the ship flips, the arrival system's sky afterwards, both drawn as seen at
 * the current relativistic speed (see Relativity.h), plus the glow of the
 * bright point that forms ahead of the ship.
 */
class HyperspaceSky {
public:
	HyperspaceSky(Graphics::Renderer *renderer, RefCountedPtr<Galaxy> galaxy, const SystemPath &source, const SystemPath &dest);
	~HyperspaceSky();

	// background of a star system as seen at rest
	static std::unique_ptr<Background::Container> CreateSystemBackground(Graphics::Renderer *renderer, RefCountedPtr<Galaxy> galaxy, const SystemPath &path);

	// the background currently in view, for drawing it at rest (e.g. by the system map)
	Background::Container *GetBackground();

	// transform: from the root frame to the camera, rotation only
	void Draw(const matrix4x4d &transform);

private:
	struct Starlight {
		vector3d total;	   // all the stars, display units
		vector3d knot;	   // the stars crowded around the direction of travel
		double atRest;	   // luminance of all the stars when not moving
		double knotAtRest; // luminance of the stars around the direction of travel when not moving
	};
	Starlight EstimateStarlight(const Background::Starfield &starfield, const vector3d &travelDir, double gamma, double oneMinusBeta) const;
	void DrawGlow(const vector3f &axis, float pixelAngle, const vector3f &flux);

	Graphics::Renderer *m_renderer;
	std::unique_ptr<Background::Container> m_departure;
	std::unique_ptr<Background::Container> m_arrival;

	// eye adaptation
	double m_logExposure;
	bool m_exposureValid;

	std::unique_ptr<Graphics::Material> m_glowMaterial;
};

#endif
