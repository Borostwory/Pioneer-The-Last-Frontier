// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Tracks of the rover's wheels on soft ground (SurfaceTracks.h), drawn per
// pixel on the terrain: ruts pressed into it, the ground in them packed
// darker and smoothed, their sides shaded as they slope, and near the camera
// the blocks of the tread. Not on paving, nor on steep ground (rock).
// Shaders using this declare in their .shaderdef:
//   Texture sampler2D surfaceTracks
//   Buffer SurfaceTrackData binding=9

layout(std140) uniform SurfaceTrackData {
	mat4 trackViewToMap; // view space -> the map's plane [m]: x, z along it, y up
	vec4 trackWindow;	 // x, z of the window's centre [texels], texel size [m], texels per side (0: no tracks)
	vec4 trackParams;	 // x: strength (how soft the ground is), y: depth of a full rut [m]
};

// R: the rut's depth (0..1), G: where across the tyre (0..1), BA: the way
// it went (x, z, 0..1 for -1..1); repeating, the map is a ring
uniform sampler2D surfaceTracks;

// how much darker the packed ground is, at the most
#define TRACK_SHADE 0.7
// how much of the ground's own detail the tyres smooth out
#define TRACK_SMOOTHING 0.6
// the tread: blocks this far apart along the track [m], slanted back from
// the tyre's middle by this much of the pitch at its edges, this much
// shallower than the rut; drawn while a pixel is smaller than TRACK_TREAD_PX [m]
#define TRACK_TREAD_PITCH 0.16
#define TRACK_TREAD_SLANT 1.6
#define TRACK_TREAD_DEPTH 0.35
#define TRACK_TREAD_PX 0.05
// the tracks fade out this many texels in from the window's edge
#define TRACK_EDGE 128.0
// slopes (cosine from the vertical) from rock to soft ground
#define TRACK_STEEP 0.8
#define TRACK_FLAT 0.92

// The tracks at view-space position viewPos, on ground with surfaceUp up and
// meshNormal its normal without its detail, paved this much (city-ground.glsl):
// bends N into the ruts and returns the multiplier of the ground's colour
// there (1: no tracks)
float surfaceTracksAt(vec3 viewPos, vec3 surfaceUp, vec3 meshNormal, float paved, inout vec3 N)
{
	if (trackWindow.w <= 0.0)
		return 1.0;
	vec3 p = (trackViewToMap * vec4(viewPos, 1.0)).xyz;
	// pixel size, before any branch on the position
	float px = length(fwidth(p.xz));

	float texel = trackWindow.z;
	vec2 t = p.xz / texel;
	vec2 off = abs(t - trackWindow.xy);
	float inside = clamp((0.5 * trackWindow.w - 1.0 - max(off.x, off.y)) / TRACK_EDGE, 0.0, 1.0);
	float soft = (1.0 - paved) * smoothstep(TRACK_STEEP, TRACK_FLAT, dot(meshNormal, surfaceUp));
	float strength = trackParams.x * soft * inside;
	if (strength <= 0.0 || abs(p.y) > 100.0)
		return 1.0;

	// (texel centres at the half texels, as SurfaceTracks presses them)
	vec2 uv = t / trackWindow.w;
	vec4 s = textureLod(surfaceTracks, uv, 0.0);
	if (s.r <= 0.0)
		return 1.0;

	// the tread's blocks, slanted back from the middle of the tyre
	vec2 dir = s.ba * 2.0 - 1.0;
	dir = dot(dir, dir) > 1e-4 ? normalize(dir) : vec2(1.0, 0.0);
	float phase = dot(p.xz, dir) / TRACK_TREAD_PITCH + abs(s.g - 0.5) * TRACK_TREAD_SLANT;
	float tread = step(0.5, fract(phase)) * clamp(2.0 - px / TRACK_TREAD_PX, 0.0, 1.0);

	// the rut's sides from the depths round it
	float d = 1.0 / trackWindow.w;
	float dx = textureLod(surfaceTracks, uv + vec2(d, 0.0), 0.0).r - textureLod(surfaceTracks, uv - vec2(d, 0.0), 0.0).r;
	float dz = textureLod(surfaceTracks, uv + vec2(0.0, d), 0.0).r - textureLod(surfaceTracks, uv - vec2(0.0, d), 0.0).r;
	float depth = trackParams.y * strength;
	// the ground's height falls with the depth: its slope along the plane's x and z
	vec2 slope = -depth * vec2(dx, dz) / (2.0 * texel);
	mat3 mapToView = transpose(mat3(trackViewToMap));

	float cover = s.r * strength;
	N = normalize(mix(N, meshNormal, TRACK_SMOOTHING * cover));
	N = normalize(N - slope.x * mapToView[0] - slope.y * mapToView[2]);
	return mix(1.0, TRACK_SHADE, cover * (1.0 - TRACK_TREAD_DEPTH * tread));
}
