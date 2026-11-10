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

	// Frees the black hole light bending's textures and material (at shutdown)
	static void UninitLens();

protected:
	void InitStar();
	void BuildHaloBuffer(Graphics::Renderer *renderer);
	// A pulsar's two beams, sweeping round the spin axis like a lighthouse, the cone they smear into when they
	// turn faster than the eye can follow, and its glow (pulsar_jet.frag, pulsar_cone.frag, pulsar_glow.frag).
	// fpos is the star's scaled view position; glowRadius and jetLength are scaled the same way.
	void RenderPulsar(Graphics::Renderer *renderer, const vector3d &fpos, const matrix4x4d &faceCamera,
		double glowRadius, double jetLength, const matrix4x4d &viewTransform);
	// the two cones the beams sweep round, one jet length long, around the y axis
	void BuildPulsarCone(Graphics::Renderer *renderer);
	// A black hole: the picture behind it bent by its gravity, and its black shadow (black_hole_lens.frag).
	// Returns false if it couldn't be drawn, so the plain black sphere is drawn instead.
	bool RenderBlackHoleLens(Graphics::Renderer *renderer, const Camera *camera, const vector3d &viewCoords,
		const matrix4x4d &viewTransform);

	std::unique_ptr<Graphics::Material> m_haloMat;
	std::unique_ptr<Graphics::MeshObject> m_haloMesh;
	VisualProfile m_visualProfile;

	// pulsars only
	std::unique_ptr<Graphics::Material> m_pulsarJetMat;
	std::unique_ptr<Graphics::Material> m_pulsarGlowMat;
	std::unique_ptr<Graphics::Material> m_pulsarConeMat;
	std::unique_ptr<Graphics::MeshObject> m_pulsarConeMesh;
	double m_jetLength = 0.0;	  // length of each jet, in metres
	double m_magneticTilt = 0.0; // angle between the magnetic poles (where the beams come out) and the spin axis, radians
};

#endif /* _STAR_H */
