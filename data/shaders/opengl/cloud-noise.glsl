// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Texture-free noise for the cloud shaders.
// noise.glsl is not used: it needs permTexture/gradTexture, which are only
// bound for gas giant generation.

// "Hash without Sine", Dave Hoskins, MIT licence (https://www.shadertoy.com/view/4djSRW)
float cloudHash13(vec3 p3)
{
	p3 = fract(p3 * 0.1031);
	p3 += dot(p3, p3.zyx + 31.32);
	return fract((p3.x + p3.y) * p3.z);
}

vec3 cloudHash33(vec3 p3)
{
	p3 = fract(p3 * vec3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yxz + 33.33);
	return fract((p3.xxy + p3.yxx) * p3.zyx);
}

float cloudRemap(float v, float l0, float h0, float l1, float h1)
{
	return l1 + (v - l0) * (h1 - l1) / (h0 - l0);
}

// 3D gradient noise, Inigo Quilez, MIT licence. Returns roughly [-1, 1].
float cloudGradNoise(vec3 x)
{
	vec3 i = floor(x);
	vec3 f = fract(x);
	vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

	vec3 g000 = cloudHash33(i + vec3(0.0, 0.0, 0.0)) * 2.0 - 1.0;
	vec3 g100 = cloudHash33(i + vec3(1.0, 0.0, 0.0)) * 2.0 - 1.0;
	vec3 g010 = cloudHash33(i + vec3(0.0, 1.0, 0.0)) * 2.0 - 1.0;
	vec3 g110 = cloudHash33(i + vec3(1.0, 1.0, 0.0)) * 2.0 - 1.0;
	vec3 g001 = cloudHash33(i + vec3(0.0, 0.0, 1.0)) * 2.0 - 1.0;
	vec3 g101 = cloudHash33(i + vec3(1.0, 0.0, 1.0)) * 2.0 - 1.0;
	vec3 g011 = cloudHash33(i + vec3(0.0, 1.0, 1.0)) * 2.0 - 1.0;
	vec3 g111 = cloudHash33(i + vec3(1.0, 1.0, 1.0)) * 2.0 - 1.0;

	float v000 = dot(g000, f - vec3(0.0, 0.0, 0.0));
	float v100 = dot(g100, f - vec3(1.0, 0.0, 0.0));
	float v010 = dot(g010, f - vec3(0.0, 1.0, 0.0));
	float v110 = dot(g110, f - vec3(1.0, 1.0, 0.0));
	float v001 = dot(g001, f - vec3(0.0, 0.0, 1.0));
	float v101 = dot(g101, f - vec3(1.0, 0.0, 1.0));
	float v011 = dot(g011, f - vec3(0.0, 1.0, 1.0));
	float v111 = dot(g111, f - vec3(1.0, 1.0, 1.0));

	return 1.6 * mix(mix(mix(v000, v100, u.x), mix(v010, v110, u.x), u.y),
					 mix(mix(v001, v101, u.x), mix(v011, v111, u.x), u.y), u.z);
}

// 3D cellular noise (F1 distance) with a 2x2x2 search, after Stefan Gustavson
// (MIT licence). Feature points are jittered within 0.1..0.9 of their cell so
// the reduced search stays artefact-free. Returns [0, 1].
float cloudWorley(vec3 x)
{
	vec3 i = floor(x);
	vec3 f = fract(x);
	vec3 s = step(0.5, f) - 1.0;
	float d = 8.0;
	for (int k = 0; k < 8; k++) {
		vec3 o = s + vec3(float(k & 1), float((k >> 1) & 1), float((k >> 2) & 1));
		vec3 fp = o + 0.1 + 0.8 * cloudHash33(i + o);
		vec3 r = fp - f;
		d = min(d, dot(r, r));
	}
	return clamp(sqrt(d), 0.0, 1.0);
}

// fBm of gradient noise, returns roughly [0, 1]
float cloudFbm(vec3 p, int octaves, float weightLast)
{
	float sum = 0.0;
	float amp = 0.5;
	float norm = 0.0;
	for (int i = 0; i < 9; i++) {
		if (i >= octaves) break;
		float w = (i == octaves - 1) ? weightLast : 1.0;
		sum += w * amp * cloudGradNoise(p);
		norm += w * amp;
		amp *= 0.5;
		p = p * 2.03 + vec3(17.1, 31.7, 7.3);
	}
	return 0.5 + 0.5 * sum / max(norm, 1e-4);
}

// inverted Worley fBm (1 at cell centres), returns [0, 1]
float cloudWorleyFbm(vec3 p, int octaves, float weightLast)
{
	float sum = 0.0;
	float amp = 0.625;
	float norm = 0.0;
	for (int i = 0; i < 4; i++) {
		if (i >= octaves) break;
		float w = (i == octaves - 1) ? weightLast : 1.0;
		sum += w * amp * (1.0 - cloudWorley(p));
		norm += w * amp;
		amp *= 0.5;
		p = p * 2.0 + vec3(5.3, 11.9, 23.1);
	}
	return sum / max(norm, 1e-4);
}

// Perlin-Worley (Schneider 2015): billowy cells with connected gaps, [0, 1]
float cloudPerlinWorley(vec3 p, int octaves, float weightLast)
{
	float perlin = cloudFbm(p, octaves, weightLast);
	float worley = 1.0 - cloudWorley(p * 0.5);
	// dense at cell centres, perlin detail towards the cell edges
	return clamp(cloudRemap(perlin, 0.0, 1.0, worley * 0.85, 1.0), 0.0, 1.0);
}
