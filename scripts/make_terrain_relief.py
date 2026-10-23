#!/usr/bin/env python3
# Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
# Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

"""Generates data/textures/terrain/relief.png, the slope texture of the
terrain relief shading (terrainReliefNormal() in
data/shaders/opengl/terrain-materials.glsl).

A tileable random landscape with wavelengths of 4 to 32 texels (three
octaves, the same slope in each). The shader maps the longest waves to the
mesh's smallest (twice the vertex spacing), so the texture holds the terrain
detail below the mesh.
  R, G: slope along u, v, divided by 3 and mapped from [-1, 1] to [0, 1]
Slopes are in units of their RMS (mean of su^2 + sv^2 is 1).

usage: python make_terrain_relief.py <output.png>
needs numpy and Pillow
"""

import sys

import numpy as np
from PIL import Image

N = 256
rng = np.random.default_rng(20261024)

f = np.fft.fftfreq(N) * N  # cycles per tile
ku, kv = np.meshgrid(f, f)
k = np.maximum(np.hypot(ku, kv), 1e-6)

# band 8..64 cycles per tile with soft edges (in octaves)
octave = np.log2(k)
band = np.clip((octave - 2.7) / 0.6, 0.0, 1.0) * np.clip((6.3 - octave) / 0.6, 0.0, 1.0)
spec = band / k**2 * (rng.standard_normal((N, N)) + 1j * rng.standard_normal((N, N)))
spec[0, 0] = 0.0

su = np.real(np.fft.ifft2(2j * np.pi * ku * spec))
sv = np.real(np.fft.ifft2(2j * np.pi * kv * spec))
norm = np.sqrt(np.mean(su**2 + sv**2))
su /= norm
sv /= norm

rgba = np.stack([
    np.clip(su / 3.0, -1.0, 1.0) * 0.5 + 0.5,
    np.clip(sv / 3.0, -1.0, 1.0) * 0.5 + 0.5,
    np.full_like(su, 0.5),
    np.ones_like(su),
], axis=-1)
Image.fromarray(np.round(rgba * 255.0).astype(np.uint8), "RGBA").save(sys.argv[1], optimize=True)
print(f"saved {sys.argv[1]}")
