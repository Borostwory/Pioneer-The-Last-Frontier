// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// The fog inside a gas giant's clouds (GasGiant::Render): drawn on the inside of the
// atmosphere's sphere, around the camera, over everything the planet's draw left behind.

#include "attributes.glsl"
#include "lib.glsl"

void main(void)
{
	gl_Position = matrixTransform();
}
