// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _SPHERECELLS_H
#define _SPHERECELLS_H

#include "../vector3.h"
#include "TerrainNoise.h"

#include <cmath>
#include <cstdint>

// The hash and noise of the physical surface generator. Depends only on the
// position and the seed, so a feature is the same in every patch at every
// detail level. The same rules are in the planet lab (fields.py: _hash, noise,
// fbm) and will be in the shaders; change them together.
namespace SphereCells {

	// -1 to 1, for a cell of a lattice in space
	inline double Hash(const int64_t ix, const int64_t iy, const int64_t iz, const uint32_t seed)
	{
		uint32_t h = (uint32_t(ix) * 0x8da6b343u) ^ (uint32_t(iy) * 0xd8163841u) ^ (uint32_t(iz) * 0xcb1ab31fu) ^ seed;
		h ^= h >> 15;
		h *= 0x2c1b3c6du;
		h ^= h >> 12;
		h *= 0x297a2d39u;
		h ^= h >> 15;
		return double(h >> 8) / 8388607.5 - 1.0;
	}

	// value noise, -1 to 1
	inline double Noise(const vector3d &p, const uint32_t seed)
	{
		const double fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
		const int64_t ix = int64_t(fx), iy = int64_t(fy), iz = int64_t(fz);
		const double x = p.x - fx, y = p.y - fy, z = p.z - fz;
		const double ux = x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
		const double uy = y * y * y * (y * (y * 6.0 - 15.0) + 10.0);
		const double uz = z * z * z * (z * (z * 6.0 - 15.0) + 10.0);
		auto c = [&](const int dx, const int dy, const int dz) { return Hash(ix + dx, iy + dy, iz + dz, seed); };
		const double x00 = c(0, 0, 0) * (1.0 - ux) + c(1, 0, 0) * ux;
		const double x10 = c(0, 1, 0) * (1.0 - ux) + c(1, 1, 0) * ux;
		const double x01 = c(0, 0, 1) * (1.0 - ux) + c(1, 0, 1) * ux;
		const double x11 = c(0, 1, 1) * (1.0 - ux) + c(1, 1, 1) * ux;
		const double y0 = x00 * (1.0 - uy) + x10 * uy;
		const double y1 = x01 * (1.0 - uy) + x11 * uy;
		return y0 * (1.0 - uz) + y1 * uz;
	}

	// Fractal noise at a point on the unit sphere. Each octave doubles the
	// frequency. Octaves finer than the patch's band limit are skipped, with
	// their weight still counted, so the height keeps its average.
	inline double Fbm(const vector3d &p, const double frequency, const int octaves, const uint32_t seed, const double gain = 0.5)
	{
		const double maxFrequency = TerrainNoise::s_maxFrequency;
		double sum = 0.0, amplitude = 1.0, total = 0.0, f = frequency;
		for (int o = 0; o < octaves; o++) {
			total += amplitude;
			if (maxFrequency <= 0.0 || f <= maxFrequency) {
				const double shift = 17.3 * o;
				sum += amplitude * Noise(vector3d(p.x * f + shift, p.y * f + shift, p.z * f + shift), seed + uint32_t(o) * 101u);
			}
			amplitude *= gain;
			f *= 2.0;
		}
		return sum / total;
	}

} // namespace SphereCells

#endif // _SPHERECELLS_H
