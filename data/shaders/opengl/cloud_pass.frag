// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Volumetric clouds of one planet at reduced resolution. Each pixel marches
// its view ray up to the scene distance (terrain, cities, ships), so solid
// objects in front of clouds stay visible and objects behind them are hidden.
// Output: premultiplied colour (tonemapped) and opacity.

#include "attributes.glsl"
#include "lib.glsl"
#include "cloud-pass-uniforms.glsl"

#define atmosColor cloudPassAtmos

#include "cloud-config.glsl"
#include "cloud-uniforms.glsl"
#include "cloud-noise.glsl"
#include "cloud-derive.glsl"
#include "cloud-march.glsl"

uniform sampler2D sceneDist;

out vec4 frag_color;

void main(void)
{
	// this march pixel traces one history pixel of its 2x2 block (the
	// checkerboard offset rotates every frame), or the block centre when the
	// planet has no history
	vec2 histPos = floor(gl_FragCoord.xy) * 2.0 + cloudPassFrame.xy + 0.5;
	vec2 fullPos = histPos * cloudPassMarch.z;
	ivec2 ip = ivec2(min(fullPos, cloudPassSize.zw - 1.0));
	float viewZ = texelFetch(sceneDist, ip, 0).r;

	// view ray; the camera looks down -Z
	vec2 ndc = fullPos / cloudPassSize.zw * 2.0 - 1.0;
	vec3 dirV = vec3(ndc.x / cloudPassProj.x, ndc.y / cloudPassProj.y, -1.0);
	vec3 eyenorm = normalize(dirV);

	float a = max(cloudGeom.x, 1.0);
	float tMax = min(viewZ * length(dirV) / a, 1e6); // planet radii
	vec3 camPos = -cloudPassCenter.xyz;
	float pixelAngle = 2.0 / cloudPassProj.y / cloudPassSize.w * cloudPassMarch.w;

	CloudPlanet cp = cloudDerive();
	CloudResult r = cloudMarchView(cp, camPos, eyenorm, tMax, pixelAngle, cloudJitter());
	vec4 col = vec4(cloudToneMap(r.inscatter), 1.0 - r.transmittance);

#if (CLOUD_DEBUG > 0)
	// debug view on the surface (or on a shell 5 km up where the ray misses it)
	vec2 hit = cloudSphereHit(camPos, eyenorm, 1.0 + 5000.0 / a);
	float tD = tMax < 1e5 ? tMax : (hit.x > 0.0 ? hit.x : hit.y);
	if (tD > 0.0 && (tMax < 1e5 || hit.y > 0.0)) {
		vec4 dbg = cloudDebugColor(cp, normalize(cloudToPf(camPos + eyenorm * tD)));
		col = vec4(dbg.rgb * dbg.a + col.rgb * (1.0 - dbg.a), dbg.a + col.a * (1.0 - dbg.a));
	}
#endif

	frag_color = col;
}
