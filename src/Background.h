// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _BACKGROUND_H
#define _BACKGROUND_H

#include "galaxy/SystemPath.h"
#include "graphics/Drawables.h"

class Random;
class Galaxy;
class Space;

namespace Graphics {
	class Renderer;
	class Material;
	class RenderState;
	class Texture;

} // namespace Graphics

/*
 * Classes to draw background stars and the milky way
 */

namespace Background {

	// The sky as seen from a ship moving at relativistic speed.
	// Matches the uniform block RelativityData in data/shaders/opengl/relativity.glsl
	struct RelativisticView {
		vector3f axis; // direction of travel in view space
		float beta;
		float gamma;
		float oneMinusBeta; // kept separately because beta rounds to 1 near the peak
		float exposure;
		float unused0;
		float cmbScale;		  // CMB spectral radiance (W m^-2 sr^-1 nm^-1) -> pixel value
		float cmbTemperature; // K, at rest
		float coreRadius;	  // angular radius (rad) of the disc holding the unresolved CMB core
		float maxSpriteGrowth;
		vector3f cmbCore; // pixel value inside that disc
		float unused1;
	};

	// A star of the starfield, kept to estimate how bright the sky looks when moving
	struct StarSample {
		vector3f dir; // in background (root frame) coordinates
		vector3f linearColor;
		float area;		   // sprite area in pixels
		float temperature; // estimated blackbody temperature, K
	};

	class BackgroundElement {
	public:
		void SetIntensity(float intensity);

	protected:
		Graphics::Renderer *m_renderer;
		RefCountedPtr<Graphics::Material> m_material;
		RefCountedPtr<Graphics::Material> m_materialRelativistic;

		float m_rMin;
		float m_rMax;
		float m_gMin;
		float m_gMax;
		float m_bMin;
		float m_bMax;
	};

	class UniverseBox : public BackgroundElement {
	public:
		UniverseBox(Graphics::Renderer *r);
		~UniverseBox();

		void Draw();
		void DrawRelativistic(RelativisticView view);
		void LoadCubeMap(Random &rand);

	private:
		void Init();

		std::unique_ptr<Graphics::MeshObject> m_universeBox;
		RefCountedPtr<Graphics::Texture> m_cubemap;

		Uint32 m_numCubemaps;
	};

	class Starfield : public BackgroundElement {
	public:
		//does not Fill the starfield
		Starfield(Graphics::Renderer *r);
		void Draw();
		void DrawRelativistic(RelativisticView view);
		//create or recreate the starfield
		void Fill(Random &rand, const SystemPath *const systemPath, RefCountedPtr<Galaxy> galaxy);

		// a subset of the stars; each one stands for GetSampleWeight() stars
		const std::vector<StarSample> &GetSamples() const { return m_samples; }
		float GetSampleWeight() const { return m_sampleWeight; }

	private:
		void Init();

		std::unique_ptr<Graphics::Drawables::PointSprites> m_pointSprites;

		std::vector<StarSample> m_samples;
		float m_sampleWeight = 0.f;
	};

	class MilkyWay : public BackgroundElement {
	public:
		MilkyWay(Graphics::Renderer *);
		void Draw();

	private:
		std::unique_ptr<Graphics::MeshObject> m_meshObject;
	};

	// contains starfield, milkyway, possibly other Background elements
	class Container {
	public:
		enum BackgroundDrawFlags {
			DRAW_STARS = 1 << 0,
			DRAW_MILKY = 1 << 1,
			DRAW_SKYBOX = 1 << 2
		};

		Container(Graphics::Renderer *, Random &rand);
		void Draw(const matrix4x4d &transform);
		// draw as seen from a ship moving at relativistic speed
		void DrawRelativistic(const matrix4x4d &transform, const RelativisticView &view);

		void SetIntensity(float intensity);
		void SetDrawFlags(const Uint32 flags);
		Uint32 GetDrawFlags() const { return m_drawFlags; }

		Starfield *GetStarfield() { return &m_starField; }

	private:
		Graphics::Renderer *m_renderer;
		MilkyWay m_milkyWay;
		Starfield m_starField;
		UniverseBox m_universeBox;
		Uint32 m_drawFlags;
	};

} //namespace Background

#endif
