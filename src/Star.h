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
	// A pulsar's two jets along its spin axis and its glow (pulsar_jet.frag, pulsar_glow.frag).
	// fpos is the star's scaled view position; glowRadius and jetLength are scaled the same way.
	void RenderPulsar(Graphics::Renderer *renderer, const vector3d &fpos, const matrix4x4d &faceCamera,
		double glowRadius, double jetLength, const matrix4x4d &viewTransform);

	std::unique_ptr<Graphics::Material> m_haloMat;
	std::unique_ptr<Graphics::MeshObject> m_haloMesh;
	VisualProfile m_visualProfile;

	// pulsars only
	std::unique_ptr<Graphics::Material> m_pulsarJetMat;
	std::unique_ptr<Graphics::Material> m_pulsarGlowMat;
	double m_jetLength = 0.0; // length of each jet, in metres
};

#endif /* _STAR_H */
