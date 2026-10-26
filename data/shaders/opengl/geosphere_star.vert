// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "attributes.glsl"
#include "lib.glsl"

out vec4 vertexColor;
out vec3 varyingEyepos;
out vec3 varyingNormal;
out vec3 varyingSpherePos;

void main(void)
{
	gl_Position = matrixTransform();
	// the colour's alpha holds the altitude above sea level
	vertexColor = vec4(a_color.rgb, 1.0);

	// eye-space position and normal for limb darkening
	varyingEyepos = vec3(uViewMatrix * a_vertex);
	varyingNormal = normalize(normalMatrix() * a_normal);

	// the star's surface is a smooth ellipsoid, so its body-space normal is a
	// position on the unit sphere that turns with the star: the noise lives there
	varyingSpherePos = a_normal;
}
