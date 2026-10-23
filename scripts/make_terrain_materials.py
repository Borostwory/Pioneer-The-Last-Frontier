#!/usr/bin/env python3
# Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
# Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

"""Generates the terrain material textures (data/textures/terrain/*.dds) used
by data/shaders/opengl/terrain-materials.glsl.

The sources are CC0 photo materials from ambientCG (https://ambientcg.com),
1K-JPG downloads, unpacked one folder per asset:
    <source_dir>/Rock030/Rock030_1K-JPG_Color.jpg
    <source_dir>/Rock030/Rock030_1K-JPG_Displacement.jpg
    ...

Each becomes a greyscale detail texture, so the planet's own colours still
show through: the colour photo's brightness with its large-scale shading
removed, at a set contrast, stored with a mean of 0.5 in RGB (the shader
doubles it); the displacement map in alpha, used to blend materials along
their bumps. DXT5 with a full mipmap chain, as texture arrays need DDS.

usage: python make_terrain_materials.py <source_dir> <output_dir>
needs numpy and Pillow (11 or later, for DXT5 encoding)
"""

import io
import os
import struct
import sys

import numpy as np
from PIL import Image

# layer order must match the MATERIAL_* indices in terrain-materials.glsl
# (output name, ambientCG asset, contrast: standard deviation of log brightness)
MATERIALS = [
    ("rock", "Rock030", 0.30),
    ("gravel", "Ground110", 0.32),
    ("sand", "Ground097", 0.14),
    ("soil", "Ground109", 0.24),
    ("grass", "Grass004", 0.26),
    ("forest", "Moss002", 0.34),
    ("snow", "Snow008A", 0.08),
    ("ice", "Snow014", 0.10),
]

SIZE = 1024


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def periodic_blur(img, sigma):
    """Gaussian blur that wraps around the edges (the sources tile)."""
    n = img.shape[0]
    f = np.fft.fftfreq(n)
    fy, fx = np.meshgrid(f, f, indexing="ij")
    kernel = np.exp(-2.0 * (np.pi * sigma) ** 2 * (fx**2 + fy**2))
    return np.real(np.fft.ifft2(np.fft.fft2(img) * kernel))


def load(path):
    im = Image.open(path)
    if im.size != (SIZE, SIZE):
        im = im.resize((SIZE, SIZE), Image.LANCZOS)
    return np.asarray(im, dtype=np.float64) / 255.0


def detail_and_height(src_dir, asset, contrast):
    colour = load(os.path.join(src_dir, asset, f"{asset}_1K-JPG_Color.jpg"))
    lum = srgb_to_linear(colour[..., :3]) @ np.array([0.2126, 0.7152, 0.0722])
    # log brightness, minus its broad shading, at the set contrast
    d = np.log(np.maximum(lum, 1e-3))
    d -= periodic_blur(d, SIZE / 16.0)
    d *= contrast / max(d.std(), 1e-6)
    detail = np.clip(0.5 * np.exp(d - np.log(np.mean(np.exp(d)))), 0.0, 1.0)

    disp = load(os.path.join(src_dir, asset, f"{asset}_1K-JPG_Displacement.jpg"))
    if disp.ndim == 3:
        disp = disp[..., 0]
    lo, hi = np.percentile(disp, [1.0, 99.0])
    height = np.clip((disp - lo) / max(hi - lo, 1e-6), 0.0, 1.0)
    return detail, height


def encode_dxt5(rgba):
    """DXT5 blocks of one mip level (via Pillow), padded to whole 4x4 blocks."""
    h, w = rgba.shape[:2]
    pw, ph = max(w, 4), max(h, 4)
    if (pw, ph) != (w, h):
        rgba = np.pad(rgba, ((0, ph - h), (0, pw - w), (0, 0)), mode="edge")
    buf = io.BytesIO()
    Image.fromarray(rgba, "RGBA").save(buf, "DDS", pixel_format="DXT5")
    data = buf.getvalue()[128:]
    blocks = ((w + 3) // 4) * ((h + 3) // 4)
    if (pw, ph) != (w, h):
        data = data[:16]  # a padded 1x1 or 2x2 level is a single block
    assert len(data) == blocks * 16, (w, h, len(data))
    return data


def write_dds(path, levels):
    h, w = levels[0].shape[:2]
    data = b"".join(encode_dxt5(level) for level in levels)
    DDSD = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000  # caps, height, width, pixelformat, mipmapcount, linearsize
    header = struct.pack("<4s7I44x", b"DDS ", 124, DDSD, h, w, (w // 4) * (h // 4) * 16, 0, len(levels))
    header += struct.pack("<2I4s5I", 32, 0x4, b"DXT5", 0, 0, 0, 0, 0)  # pixel format: FOURCC
    header += struct.pack("<4I4x", 0x1000 | 0x8 | 0x400000, 0, 0, 0)  # caps: texture, complex, mipmap
    assert len(header) == 128
    with open(path, "wb") as f:
        f.write(header + data)


def mip_chain(rgba):
    levels = [rgba]
    level = rgba.astype(np.float64)
    while level.shape[0] > 1:
        level = 0.25 * (level[0::2, 0::2] + level[1::2, 0::2] + level[0::2, 1::2] + level[1::2, 1::2])
        levels.append(np.round(level).astype(np.uint8))
    return levels


def main():
    src_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    for name, asset, contrast in MATERIALS:
        detail, height = detail_and_height(src_dir, asset, contrast)
        v = np.round(detail * 255.0)
        rgba = np.stack([v, v, v, np.round(height * 255.0)], axis=-1).astype(np.uint8)
        path = os.path.join(out_dir, f"{name}.dds")
        write_dds(path, mip_chain(rgba))
        print(f"{path}: {asset}, detail mean {detail.mean() * 2.0:.3f} (x2), "
              f"5-95% {np.percentile(detail, 5) * 2:.2f}-{np.percentile(detail, 95) * 2:.2f}")


if __name__ == "__main__":
    main()
