// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "terrain-morph.glsl"

void main(void)
{
	// the ground as the terrain shaders draw it, blended towards the parent patch's surface
	gl_Position = uViewProjectionMatrix * terrainMorphPosition(terrainMorph());
}
