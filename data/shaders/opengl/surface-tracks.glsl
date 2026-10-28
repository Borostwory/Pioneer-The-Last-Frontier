// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Tracks of the rover's wheels and the walker's boots on soft ground
// (SurfaceTracks.h), drawn per pixel on the terrain: ruts and prints pressed
// into it, the ground in them packed darker and smoothed, their sides shaded
// as they slope, and near the camera the blocks of the tyres' tread and the
// lugs of the soles. Not on paving, nor on steep ground (rock).
// Shaders using this declare in their .shaderdef:
//   Texture sampler2D wheelTracks
//   Texture sampler2D footTracks
//   Buffer SurfaceTrackData binding=9

layout(std140) uniform SurfaceTrackData {
	mat4 trackViewToMap; // view space -> the maps' plane [m]: x, z along it, y up
	// each map: x, z of its window's centre [texels], texel size [m], texels per side (0: no tracks)
	vec4 trackWheels;
	vec4 trackFeet;
	vec4 trackParams; // x: strength (how soft the ground is), y: depth of a full track [m]
};

// R: the depth (0..1), G: where across the tyre or sole (0..1), BA: the way
// it went (x, z, 0..1 for -1..1); repeating, each map is a ring
uniform sampler2D wheelTracks;
uniform sampler2D footTracks;

// how much darker the packed ground is, at the most
#define TRACK_SHADE 0.7
// how much of the ground's own detail the tyres and soles smooth out
#define TRACK_SMOOTHING 0.6
// the tread: blocks this far apart along the track [m], slanted back from
// the middle by this much of the pitch at the edges, this much shallower
// than the track; drawn while a pixel is smaller than a fifth of the pitch
#define TRACK_TREAD_PITCH 0.16
#define TRACK_TREAD_SLANT 1.6
#define TRACK_TREAD_DEPTH 0.35
#define FOOT_LUG_PITCH 0.028
#define FOOT_LUG_SLANT 0.0
#define FOOT_LUG_DEPTH 0.45
// the tracks fade out this many texels in from a map's edge
#define TRACK_EDGE 128.0
// slopes (cosine from the vertical) from rock to soft ground
#define TRACK_STEEP 0.8
#define TRACK_FLAT 0.92

// one map's track at plane position p (pixel px wide): its cover (depth,
// 0..1), the ground's slope along the plane's x and z, and how much of it
// is the tread's raised blocks
float trackLayerAt(sampler2D map, vec4 window, vec2 p, float px, float pitch, float slant, out vec2 slope, out float tread)
{
	slope = vec2(0.0);
	tread = 0.0;
	if (window.w <= 0.0)
		return 0.0;
	float texel = window.z;
	vec2 t = p / texel;
	vec2 off = abs(t - window.xy);
	float inside = clamp((0.5 * window.w - 1.0 - max(off.x, off.y)) / TRACK_EDGE, 0.0, 1.0);
	if (inside <= 0.0)
		return 0.0;

	// (texel centres at the half texels, as SurfaceTracks presses them)
	vec2 uv = t / window.w;
	vec4 s = textureLod(map, uv, 0.0);
	if (s.r <= 0.0)
		return 0.0;

	// the blocks, slanted back from the middle
	vec2 dir = s.ba * 2.0 - 1.0;
	dir = dot(dir, dir) > 1e-4 ? normalize(dir) : vec2(1.0, 0.0);
	float phase = dot(p, dir) / pitch + abs(s.g - 0.5) * slant;
	tread = step(0.5, fract(phase)) * clamp(2.0 - px * 5.0 / pitch, 0.0, 1.0);

	// the sides from the depths round it
	float d = 1.0 / window.w;
	float dx = textureLod(map, uv + vec2(d, 0.0), 0.0).r - textureLod(map, uv - vec2(d, 0.0), 0.0).r;
	float dz = textureLod(map, uv + vec2(0.0, d), 0.0).r - textureLod(map, uv - vec2(0.0, d), 0.0).r;
	slope = -vec2(dx, dz) * inside / (2.0 * texel);
	return s.r * inside;
}

// The tracks at view-space position viewPos, on ground with surfaceUp up and
// meshNormal its normal without its detail, paved this much (city-ground.glsl):
// bends N into them and returns the multiplier of the ground's colour there
// (1: no tracks)
float surfaceTracksAt(vec3 viewPos, vec3 surfaceUp, vec3 meshNormal, float paved, inout vec3 N)
{
	if (trackWheels.w <= 0.0 && trackFeet.w <= 0.0)
		return 1.0;
	vec3 p = (trackViewToMap * vec4(viewPos, 1.0)).xyz;
	// pixel size, before any branch on the position
	float px = length(fwidth(p.xz));

	float soft = (1.0 - paved) * smoothstep(TRACK_STEEP, TRACK_FLAT, dot(meshNormal, surfaceUp));
	float strength = trackParams.x * soft;
	if (strength <= 0.0 || abs(p.y) > 100.0)
		return 1.0;

	vec2 wheelSlope, footSlope;
	float wheelTread, footTread;
	float wheel = trackLayerAt(wheelTracks, trackWheels, p.xz, px, TRACK_TREAD_PITCH, TRACK_TREAD_SLANT, wheelSlope, wheelTread);
	float foot = trackLayerAt(footTracks, trackFeet, p.xz, px, FOOT_LUG_PITCH, FOOT_LUG_SLANT, footSlope, footTread);
	if (wheel <= 0.0 && foot <= 0.0)
		return 1.0;

	// the deeper of the two, their sides together; the ground's height falls with the depth
	float cover = max(wheel, foot) * strength;
	float tread = wheel >= foot ? wheelTread * TRACK_TREAD_DEPTH : footTread * FOOT_LUG_DEPTH;
	vec2 slope = (wheelSlope + footSlope) * trackParams.y * strength;
	mat3 mapToView = transpose(mat3(trackViewToMap));

	N = normalize(mix(N, meshNormal, TRACK_SMOOTHING * cover));
	N = normalize(N - slope.x * mapToView[0] - slope.y * mapToView[2]);
	return mix(1.0, TRACK_SHADE, cover * (1.0 - tread));
}
