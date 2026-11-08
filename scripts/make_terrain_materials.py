#!/usr/bin/env python3
# Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
# Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

"""Generates the terrain material textures (data/textures/terrain/*.dds) used
by data/shaders/opengl/terrain-materials.glsl.

The sources are CC0 photo materials from ambientCG (https://ambientcg.com) and
Poly Haven (https://polyhaven.com), 1K, 2K or 4K downloads, one folder per
asset, with the files named the same way for both (here 1K):
    <source_dir>/Rock030/Rock030_1K-JPG_Color.jpg
    <source_dir>/Rock030/Rock030_1K-JPG_Displacement.jpg
    <source_dir>/Rock030/Rock030_1K-JPG_Roughness.jpg
    ...
(tools/material-preview/fetch_candidates.py downloads them into
tools/material-preview/sources/<resolution>)

Each becomes a greyscale detail texture, so the planet's own colours still
show through: the colour photo's brightness with its large-scale shading
removed, at a set contrast, stored with a mean of 0.5 in green (the shader
doubles it); the roughness map in blue, as it is (0 a mirror, 1 fully matte),
for the shine; the displacement map in alpha, used to blend materials along
their bumps; the emission map, where there is one (lava), in red.
DXT5 with a full mipmap chain, as texture arrays need DDS. The encoder is this
script's own (encode_dxt5): Pillow's lost much of the detail once blue held
something other than a copy of green, while this one loses less than Pillow's
did before.

usage: python make_terrain_materials.py <source_dir> <output_dir> [--res 1K|2K|4K] [layer ...]
(--res: the downloads used and the size made, 4K by default, as the game uses;
MATERIAL_TEXTURE_SIZE in terrain-materials.glsl must match it. Only the named
layers, when given)
needs numpy and Pillow; prints each material's mean roughness, for
materialMeanRoughness() in terrain-materials.glsl
"""

import os
import struct
import sys

import numpy as np
from PIL import Image

# layer order must match the MATERIAL_* indices in terrain-materials.glsl
# (output name, asset, contrast: standard deviation of log brightness)
MATERIALS = [
    ("rock", "Rock030", 0.30),
    ("gravel", "Ground110", 0.32),
    ("sand", "Ground097", 0.14),
    ("soil", "Ground109", 0.24),
    ("grass", "Grass004", 0.26),
    ("forest", "forest_floor", 0.34),
    ("snow", "Snow008A", 0.08),
    ("ice", "Ice003", 0.12),
    ("basalt", "Rock035", 0.30),
    ("lava", "Lava001", 0.35),
    ("ash", "moon_dusted_03", 0.18),
    ("regolith", "Ground099", 0.16),
    ("salt", "Ground101", 0.10),
    ("tundra", "Moss002", 0.34),
    ("dirty_ice", "snow_03", 0.22),
    ("lava_flow", "Lava003", 0.30),
    ("cracked_mud", "mud_cracked_dry_03", 0.30),
    ("dust", "moon_dusted_03", 0.10),
    ("metal", "Metal059C", 0.20),
    ("forest_far", "Moss003", 0.34),
]

RESOLUTIONS = {"1K": 1024, "2K": 2048, "4K": 4096}
RES = "4K"
SIZE = RESOLUTIONS[RES]
# blocks encoded at a time (memory)
ENCODE_CHUNK = 32768


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
    colour = load(os.path.join(src_dir, asset, f"{asset}_{RES}-JPG_Color.jpg"))
    lum = srgb_to_linear(colour[..., :3]) @ np.array([0.2126, 0.7152, 0.0722])
    # log brightness, minus its broad shading, at the set contrast
    d = np.log(np.maximum(lum, 1e-3))
    d -= periodic_blur(d, SIZE / 16.0)
    d *= contrast / max(d.std(), 1e-6)
    detail = np.clip(0.5 * np.exp(d - np.log(np.mean(np.exp(d)))), 0.0, 1.0)

    disp_path = os.path.join(src_dir, asset, f"{asset}_{RES}-JPG_Displacement.jpg")
    if os.path.exists(disp_path):
        disp = load(disp_path)
        if disp.ndim == 3:
            disp = disp[..., 0]
        lo, hi = np.percentile(disp, [1.0, 99.0])
        height = np.clip((disp - lo) / max(hi - lo, 1e-6), 0.0, 1.0)
    else:
        height = np.full(lum.shape, 0.5)

    emission = np.zeros_like(height)
    emission_path = os.path.join(src_dir, asset, f"{asset}_{RES}-JPG_Emission.jpg")
    if os.path.exists(emission_path):
        e = load(emission_path)
        if e.ndim == 3:
            e = srgb_to_linear(e[..., :3]) @ np.array([0.2126, 0.7152, 0.0722])
        emission = np.clip(e / max(np.percentile(e, 99.5), 1e-6), 0.0, 1.0)

    rough_path = os.path.join(src_dir, asset, f"{asset}_{RES}-JPG_Roughness.jpg")
    if os.path.exists(rough_path):
        roughness = load(rough_path)
        if roughness.ndim == 3:
            roughness = roughness[..., 0]
    else:
        roughness = np.full(lum.shape, 0.7)
    return detail, height, emission, roughness


def _blocks(img):
    """(h, w, c) to (blocks, 16 pixels, c), row by row within a block"""
    h, w, c = img.shape
    return img.reshape(h // 4, 4, w // 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(-1, 16, c)


def _quantise565(e):
    return np.stack([np.clip(np.round(e[:, 0] * 31 / 255), 0, 31), np.clip(np.round(e[:, 1] * 63 / 255), 0, 63),
                     np.clip(np.round(e[:, 2] * 31 / 255), 0, 31)], -1).astype(np.int64)


def _expand565(q):
    """as the GPU expands 5:6:5 to 8 bits"""
    r, g, b = q[:, 0], q[:, 1], q[:, 2]
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float64)


# share of the second endpoint for each code
COLOUR_SHARE = np.array([0.0, 1.0, 1 / 3, 2 / 3])
ALPHA_SHARE = np.array([0.0, 1.0, 1 / 7, 2 / 7, 3 / 7, 4 / 7, 5 / 7, 6 / 7])
# how much each colour channel's error counts: emission, detail, roughness
COLOUR_WEIGHTS = np.array([0.5, 1.0, 0.6])
FIT_ROUNDS = 6


def _endpoints_lsq(share, x, e0, e1):
    """least-squares endpoints for the pixels x, given each one's share of the second endpoint"""
    a, b = share, 1.0 - share
    aa, bb, ab = (a * a).sum(1), (b * b).sum(1), (a * b).sum(1)
    if x.ndim == 3:
        a, b = a[..., None], b[..., None]
        aa, bb, ab = aa[:, None], bb[:, None], ab[:, None]
    ax, bx = (a * x).sum(1), (b * x).sum(1)
    det = aa * bb - ab * ab
    ok = det > 1e-6
    safe = np.where(ok, det, 1.0)
    return np.where(ok, (aa * bx - ab * ax) / safe, e0), np.where(ok, (bb * ax - ab * bx) / safe, e1)


def _keep_better(best, best_err, err, values):
    if best is None:
        return list(values), err
    better = err < best_err
    for kept, new in zip(best, values):
        kept[better] = new[better]
    return best, np.minimum(err, best_err)


def _colour_fit(x):
    """endpoints at the ends of the block's main axis (weighted), then a few rounds of least squares"""
    wt = COLOUR_WEIGHTS
    xs = x * wt
    mean = xs.mean(1, keepdims=True)
    d = xs - mean
    cov = np.einsum("nki,nkj->nij", d, d)
    v = np.full((x.shape[0], 3), 1.0 / np.sqrt(3.0))
    for _ in range(8):
        v = np.einsum("nij,nj->ni", cov, v)
        v /= np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-9)
    t = np.einsum("nki,ni->nk", d, v)
    e0 = (mean[:, 0] + t.min(1)[:, None] * v) / wt
    e1 = (mean[:, 0] + t.max(1)[:, None] * v) / wt
    # (also the two steps either side of the block's mean, so that flat blocks, the smallest mip levels among them,
    # keep their mean between the 5:6:5 steps instead of rounding it to one)
    steps = np.array([31.0, 63.0, 31.0])
    low = np.clip(np.floor(x.mean(1) * steps / 255), 0, steps - 1).astype(np.int64)
    bracket = (low, low + 1)
    best, best_err = None, None
    for rnd in range(-1, FIT_ROUNDS + 1):
        if rnd < 0:
            q0, q1 = bracket
        else:
            q0, q1 = _quantise565(np.clip(e0, 0, 255)), _quantise565(np.clip(e1, 0, 255))
        p0, p1 = _expand565(q0), _expand565(q1)
        palette = p0[:, None, :] + COLOUR_SHARE[None, :, None] * (p1 - p0)[:, None, :]
        dist = (((x[:, :, None, :] - palette[:, None, :, :]) * wt) ** 2).sum(-1)
        idx = dist.argmin(-1)
        err = np.take_along_axis(dist, idx[..., None], -1)[..., 0].sum(1)
        best, best_err = _keep_better(best, best_err, err, (q0.copy(), q1.copy(), idx))
        if 0 <= rnd < FIT_ROUNDS:
            e0, e1 = _endpoints_lsq(COLOUR_SHARE[idx], x, e0, e1)
    q0, q1, idx = best
    c0 = (q0[:, 0] << 11) | (q0[:, 1] << 5) | q0[:, 2]
    c1 = (q1[:, 0] << 11) | (q1[:, 1] << 5) | q1[:, 2]
    # four-colour blocks need c0 > c1: swap them (codes 0 and 1, 2 and 3 trade places); equal ones are one colour
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    idx = np.where(swap[:, None], idx ^ 1, idx)
    idx = np.where((c0 == c1)[:, None], 0, idx)
    return c0, c1, idx


def _alpha_fit(x):
    """eight-value alpha blocks: the block's range, then a few rounds of least squares"""
    e0, e1 = x.max(1), x.min(1)
    best, best_err = None, None
    for rnd in range(FIT_ROUNDS + 1):
        a0, a1 = np.clip(np.round(e0), 0, 255), np.clip(np.round(e1), 0, 255)
        a0, a1 = np.maximum(a0, a1), np.minimum(a0, a1)
        # (eight values need a0 > a1)
        same, top = a0 == a1, a0 >= 255
        a0 = np.where(same & ~top, a0 + 1, a0)
        a1 = np.where(same & top, a1 - 1, a1)
        palette = a0[:, None] + ALPHA_SHARE[None, :] * (a1 - a0)[:, None]
        dist = (x[:, :, None] - palette[:, None, :]) ** 2
        idx = dist.argmin(-1)
        err = np.take_along_axis(dist, idx[..., None], -1)[..., 0].sum(1)
        best, best_err = _keep_better(best, best_err, err, (a0, a1, idx))
        if rnd < FIT_ROUNDS:
            e0, e1 = _endpoints_lsq(ALPHA_SHARE[idx], x, e0, e1)
    return best


def encode_dxt5(rgba):
    """DXT5 blocks of one mip level, padded to whole 4x4 blocks."""
    h, w = rgba.shape[:2]
    pw, ph = (w + 3) // 4 * 4, (h + 3) // 4 * 4
    if (pw, ph) != (w, h):
        rgba = np.pad(rgba, ((0, ph - h), (0, pw - w), (0, 0)), mode="edge")
    blocks = _blocks(rgba.astype(np.float64))
    parts = [(_colour_fit(chunk[..., :3]), _alpha_fit(chunk[..., 3]))
             for chunk in (blocks[i:i + ENCODE_CHUNK] for i in range(0, blocks.shape[0], ENCODE_CHUNK))]
    c0, c1, cidx = (np.concatenate([p[0][k] for p in parts]) for k in range(3))
    a0, a1, aidx = (np.concatenate([p[1][k] for p in parts]) for k in range(3))
    out = np.zeros((blocks.shape[0], 16), np.uint8)
    out[:, 0], out[:, 1] = a0, a1
    abits = (aidx.astype(np.uint64) << (np.uint64(3) * np.arange(16, dtype=np.uint64))).sum(1)
    for k in range(6):
        out[:, 2 + k] = (abits >> np.uint64(8 * k)) & np.uint64(255)
    out[:, 8], out[:, 9], out[:, 10], out[:, 11] = c0 & 255, c0 >> 8, c1 & 255, c1 >> 8
    cbits = (cidx.astype(np.uint64) << (np.uint64(2) * np.arange(16, dtype=np.uint64))).sum(1)
    for k in range(4):
        out[:, 12 + k] = (cbits >> np.uint64(8 * k)) & np.uint64(255)
    return out.tobytes()


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
    global RES, SIZE
    src_dir, out_dir = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]
    if len(rest) >= 2 and rest[0] == "--res":
        RES, rest = rest[1], rest[2:]
        SIZE = RESOLUTIONS[RES]
    only = set(rest)
    os.makedirs(out_dir, exist_ok=True)
    for name, asset, contrast in MATERIALS:
        if only and name not in only:
            continue
        detail, height, emission, roughness = detail_and_height(src_dir, asset, contrast)
        rgba = np.stack([np.round(emission * 255.0), np.round(detail * 255.0), np.round(roughness * 255.0),
                         np.round(height * 255.0)], axis=-1).astype(np.uint8)
        path = os.path.join(out_dir, f"{name}.dds")
        write_dds(path, mip_chain(rgba))
        print(f"{path}: {asset}, detail mean {detail.mean() * 2.0:.3f} (x2), "
              f"5-95% {np.percentile(detail, 5) * 2:.2f}-{np.percentile(detail, 95) * 2:.2f}, "
              f"roughness mean {roughness.mean():.3f}")


if __name__ == "__main__":
    main()
