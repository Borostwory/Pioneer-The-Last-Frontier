// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _SCREENEFFECTS_H
#define _SCREENEFFECTS_H

namespace Graphics {
	class Renderer;
}

/*
 * Passes over the whole picture, once it's drawn: the crease shading's switch and sliders in the debug window ("Screen
 * effects"), the edge smoothing's choice in the settings window (Video tab, "Anti-aliasing"):
 *
 * - Crease shading: soft shade in cracks and hollows and where things meet the ground (the rover's wheels, a
 *   building's foot, landing legs on a pad), from the scene's depth alone (crease_*.frag). It darkens the picture as
 *   drawn, sunlit or not, so it is weaker without air, where the sky lights nothing.
 * - Edge smoothing: the jagged edges of the finished picture blended, by FXAA or SMAA (fxaa.frag, smaa_*.frag).
 *   Reads only the picture, so it goes with the clouds and the crease shading.
 *
 * Neither works with the old multisampling (AntiAliasingMode): its picture and depth can't be read this way, and
 * both stand aside while it's on, as the clouds do.
 */
namespace ScreenEffects {
	// Crease shading, over what is drawn in the bound render target so far: call once the solid things are drawn,
	// before anything see-through. air: how much air round the camera, 0 none, 1 Earth's or more.
	void DrawCreases(Graphics::Renderer *r, double air);

	// Edge smoothing, over what is drawn in the bound render target so far: call once the world is drawn, before
	// the screen's menus and text.
	void DrawEdgeSmoothing(Graphics::Renderer *r);

	// frees what the passes hold on the graphics card
	void Uninit();

	// the settings (in the config file)
	bool CreasesEnabled();			// CreaseShade
	double CreaseStrength();		// CreaseShadeStrength
	double CreaseReach();			// CreaseShadeReach, metres
	double CreaseVacuumShare();		// CreaseShadeVacuum: how strong without air, as a share of in Earth's air
	int EdgeSmoothingMode();		// EdgeSmoothing: 0 off, 1 FXAA, 2 SMAA
	// whether a pass's shaders didn't compile (it then stays off), for the debug window
	bool CreasesFailed();
	bool EdgeSmoothingFailed(int mode);
} // namespace ScreenEffects

#endif
