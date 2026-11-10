// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Edge smoothing with SMAA (ScreenEffects.cpp): its settings, shared by its three steps (smaa_edges, smaa_weights,
// smaa_blend), then SMAA itself (smaa.glsl). Each stage sets SMAA_INCLUDE_VS or SMAA_INCLUDE_PS to 0 for the half
// it doesn't use.

// x: 1 / width, y: 1 / height, z: width, w: height of the screen (px)
uniform vec4 EdgeScreen;

#define SMAA_RT_METRICS EdgeScreen
#define SMAA_GLSL_4
#define SMAA_PRESET_HIGH

#include "smaa.glsl"
