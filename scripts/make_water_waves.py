#!/usr/bin/env python3
# Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
# Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

"""Generates data/textures/water_waves.png, the wave slope texture of the
terrain water shader (data/shaders/opengl/terrain-water.glsl).

A tileable patch of wind waves from a random spectrum: wavelengths from 1/2 to
1/32 of the tile (four octaves), the slope variance about the same in each
octave, longer crests across the wind (along v). Stored for LEAN mapping, so
mipmaps keep the slope variance of the waves they average away:
  R, G: slope along u, v, divided by 3 and mapped from [-1, 1] to [0, 1]
  B, A: squared slope along u, v, divided by 9
Slopes are in units of their RMS (mean of su^2 + sv^2 is 1); the shader scales
them to the real slope variance.

usage: python make_water_waves.py <output.png>
needs numpy and Pillow
"""

import sys

import numpy as np
from PIL import Image

N = 256
rng = np.random.default_rng(20260424)

f = np.fft.fftfreq(N) * N  # cycles per tile
ku, kv = np.meshgrid(f, f)  # [v, u]
k = np.hypot(ku, kv)
kk = np.maximum(k, 1e-6)

# band 2..32 cycles per tile with soft edges (in octaves)
octave = np.log2(kk)
band = np.clip((octave - 0.7) / 0.6, 0.0, 1.0) * np.clip((5.3 - octave) / 0.6, 0.0, 1.0)
# height amplitude ~ k^-2: equal slope variance per octave
amp = band / kk**2
# wind along u: wave vectors mostly along u, so crests run along v
cos = ku / kk
amp *= np.sqrt(0.25 + 0.75 * cos**2)
amp[0, 0] = 0.0

spec = amp * (rng.standard_normal((N, N)) + 1j * rng.standard_normal((N, N)))
su = np.real(np.fft.ifft2(2j * np.pi * ku * spec))
sv = np.real(np.fft.ifft2(2j * np.pi * kv * spec))
norm = np.sqrt(np.mean(su**2 + sv**2))
su /= norm
sv /= norm

rgba = np.stack([
    np.clip(su / 3.0, -1.0, 1.0) * 0.5 + 0.5,
    np.clip(sv / 3.0, -1.0, 1.0) * 0.5 + 0.5,
    np.clip(su**2 / 9.0, 0.0, 1.0),
    np.clip(sv**2 / 9.0, 0.0, 1.0),
], axis=-1)
Image.fromarray(np.round(rgba * 255.0).astype(np.uint8), "RGBA").save(sys.argv[1], optimize=True)
print(f"saved {sys.argv[1]}: slope RMS u {np.sqrt(np.mean(su**2)):.2f} v {np.sqrt(np.mean(sv**2)):.2f}, "
      f"clipped {np.mean((np.abs(su) > 3) | (np.abs(sv) > 3)) * 100:.2f}%")
