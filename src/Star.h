// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _STAR_H
#define _STAR_H

#include "TerrainBody.h"

class Camera;
class Space;
class SystemBody;

namespace Graphics {
	class Material;
	class MeshObject;
	class Renderer;
} // namespace Graphics

class Star : public TerrainBody {
public:
	OBJDEF(Star, TerrainBody, STAR);
	Star() = delete;
	Star(SystemBody *sbody);
	Star(const Json &jsonObj, Space *space);
	virtual ~Star();

	void Render(Graphics::Renderer *r, const Camera *camera, const vector3d &viewCoords, const matrix4x4d &viewTransform) override;

	// Per-type look of the star surface (geosphere_star.frag) and halo (star_halo.frag)
	struct VisualProfile {
		// surface
		float granuleFrequency; // convection cells per unit-sphere distance
		float granuleContrast;
		float spotAmount;	// 0 = no sunspots
		float bakedWeight;	// how much of the baked TerrainColorStar* texture shows through
		// halo, sizes in star radii
		float coronaSize;
		float streaks;
		float prominences;
		float envelope; // Wolf-Rayet wind envelope
	};
	static VisualProfile GetVisualProfile(const SystemBody *sbody);

	// Slowly turning angles (radians, wrapped) that animate the star shaders seamlessly
	static vector3f GetAnimationAngles();

protected:
	void InitStar();
	void BuildHaloBuffer(Graphics::Renderer *renderer);

	std::unique_ptr<Graphics::Material> m_haloMat;
	std::unique_ptr<Graphics::MeshObject> m_haloMesh;
	VisualProfile m_visualProfile;
};

#endif /* _STAR_H */
