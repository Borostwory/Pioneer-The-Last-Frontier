// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"

// x: distance from the star in jet lengths, y: angle round the spin axis (radians)
out vec2 conePos;
// position and surface direction, seen from the camera
out vec3 eyePos;
out vec3 eyeNormal;

void main(void)
{
	gl_Position = matrixTransform();
	conePos = a_uv0.xy;
	eyePos = vec3(uViewMatrix * a_vertex);
	eyeNormal = normalize(normalMatrix() * a_normal);
}
