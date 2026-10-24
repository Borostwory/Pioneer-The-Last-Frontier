#!/usr/bin/env python3
# Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
# Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

"""Generates data/heightmaps/erosion_valleys.bin, the river valleys carved
into the mountains of planets with liquid water (TerrainErosion in
src/terrain/TerrainErosion.cpp).

1. A tileable eroded mountain landscape: stream-power river incision with
   uplift, talus collapse and soil creep on a wrap-around grid, solved with
   the implicit scheme of Braun & Willett (2013) coarse to fine (512 up to
   the output size). Closed basins are filled (priority flood) so every slope
   drains to a network of river corridors. Takes a few minutes for 4096.
2. Its ridges and valleys: the landscape minus its Gaussian blur over 1/128
   of the tile, mapped from valley floor (-0.15, full cut) to crest (+0.20,
   no cut) and shaped as valley = (1 - crest)^1.3.

File: "EVAL", uint32 size, float32 tile size in metres on the planet,
float32 mean valley value, then size x size uint16 valley values (0..65535),
rows of increasing v.

usage: python make_erosion_valleys.py <output.bin> [--size 4096] [--stamp landscape.npy]
  --stamp: convert an existing landscape (float32 .npy, square, tileable)
           instead of simulating one
needs numpy, scipy and numba
"""
import argparse
import heapq
import struct
import time

import numba
import numpy as np
from scipy import ndimage

# the simulation runs at this physical tile size; the game stretches the
# result over TILE_M (wider valleys, ~2 km apart)
TILE_KM = 409.6
TILE_M = 819200.0
HIGH_PASS_DIV = 128
CREST_RANGE = (-0.15, 0.20)
VALLEY_POW = 1.3

START = 512
SEED = 1234
CORRIDOR_KM = 150.0   # wavelength of the noise whose zero lines are the corridors
CORRIDOR_AREA = 0.03  # share of the tile pinned as corridor floor
K_EROSION = 4e-3      # slope 0.25 where 1 km^2 drains, at U = 1
M_EXP = 0.5
TALUS = 0.8           # ~39 degrees
# soil creep per iteration, as a share of the gap to the neighbour mean. The
# same at every level, so each level keeps valleys a few cells apart and adds
# gullies at its own scale (a physical constant would smooth away everything
# the fine levels add)
CREEP = 0.15
FILL_EPS = 0.01       # m per cell across filled basins
# per level: iterations, time step, iterations between depression fills
SCHEDULE = {512: (600, 50.0, 5), 1024: (400, 25.0, 5), 2048: (250, 15.0, 10), 4096: (120, 10.0, 20),
            8192: (60, 6.0, 30), 16384: (40, 4.0, 40)}

NBR = np.array([(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)], np.int64)


def periodic_noise(n, wavelength_px, slope=-2.0, seed=0):
    """Wrap-around fBm-like noise: white noise shaped to a power spectrum
    ~ f^slope above 1/wavelength_px, normalised to unit std."""
    r = np.random.default_rng(seed)
    white = r.standard_normal((n, n)).astype(np.float32)
    f = np.fft.rfft2(white)
    fy = np.fft.fftfreq(n)[:, None]
    fx = np.fft.rfftfreq(n)[None, :]
    fr = np.sqrt(fx * fx + fy * fy)
    f0 = 1.0 / wavelength_px
    shape = np.where(fr < f0, (fr / f0) ** 2, (np.maximum(fr, 1e-9) / f0) ** (slope * 0.5))
    shape[0, 0] = 0.0
    out = np.fft.irfft2(f * shape, s=(n, n)).astype(np.float32)
    return out / out.std()


def at_level(field, n):
    if n == field.shape[0]:
        return field
    return ndimage.zoom(field, n / field.shape[0], order=3, mode="grid-wrap", grid_mode=True).astype(np.float32)


# large-scale fields, defined at START and upsampled per level
_corr = periodic_noise(START, START * CORRIDOR_KM / TILE_KM, slope=-2.6, seed=SEED + 7)
_corr_floor = 150.0 + 120.0 * periodic_noise(START, START / 3, slope=-3.0, seed=SEED + 11)
_uplift = np.clip(1.0 + 0.45 * periodic_noise(START, START / 4, slope=-3.0, seed=SEED + 3), 0.35, 2.0)


def outlet_heights(n):
    c = np.abs(at_level(_corr, n))
    width = np.quantile(c, CORRIDOR_AREA)
    return np.where(c <= width, at_level(_corr_floor, n), -1.0).astype(np.float32)


@numba.njit(cache=True)
def priority_flood(h, pinned, eps):
    """Raise closed depressions to their spill height plus a tiny gradient."""
    n = h.shape[0]
    flat = h.ravel()
    pin = pinned.ravel()
    closed = np.zeros(n * n, np.bool_)
    heap = [(0.0, 0)]
    heap.pop()
    for i in range(n * n):
        if pin[i] >= 0.0:
            closed[i] = True
            heapq.heappush(heap, (np.float64(flat[i]), i))
    while len(heap) > 0:
        z, i = heapq.heappop(heap)
        y, x = i // n, i % n
        for k in range(8):
            j = ((y + NBR[k, 1]) % n) * n + (x + NBR[k, 0]) % n
            if closed[j]:
                continue
            closed[j] = True
            if flat[j] <= z + eps:
                flat[j] = z + eps
            heapq.heappush(heap, (np.float64(flat[j]), j))


@numba.njit(parallel=True, cache=True)
def receivers(h, pinned, cell, salt):
    """Steepest descent neighbour of every cell (itself for outlets and pits).
    Slopes are ranked with +-25% random jitter, new every iteration, so rivers
    don't line up with the 8 grid directions."""
    n = h.shape[0]
    rcv = np.empty(n * n, np.int64)
    dist = np.empty(n * n, np.float32)
    for y in numba.prange(n):
        for x in range(n):
            i = y * n + x
            rcv[i] = i
            dist[i] = cell
            if pinned[y, x] >= 0.0:
                continue
            best = 0.0
            for k in range(8):
                xx, yy = (x + NBR[k, 0]) % n, (y + NBR[k, 1]) % n
                d = cell * (1.41421356 if (k & 1) == 1 else 1.0)
                s = (h[y, x] - h[yy, xx]) / d
                if s <= 0.0:
                    continue
                q = np.uint32(i * 8 + k) * np.uint32(2654435761) ^ np.uint32(salt) * np.uint32(2246822519)
                q = (q ^ (q >> np.uint32(15))) * np.uint32(2654435761)
                s *= 0.75 + 0.5 * (float(q >> np.uint32(8)) / 16777216.0)
                if s > best:
                    best = s
                    rcv[i] = yy * n + xx
                    dist[i] = d
    return rcv, dist


@numba.njit(cache=True)
def drainage_order(rcv):
    """Cells ordered so each comes after its receiver (outlets first)."""
    count = rcv.shape[0]
    ndon = np.zeros(count, np.int64)
    for i in range(count):
        if rcv[i] != i:
            ndon[rcv[i]] += 1
    start = np.zeros(count + 1, np.int64)
    for i in range(count):
        start[i + 1] = start[i] + ndon[i]
    fill = start[:-1].copy()
    donors = np.empty(start[count], np.int64)
    for i in range(count):
        r = rcv[i]
        if r != i:
            donors[fill[r]] = i
            fill[r] += 1
    order = np.empty(count, np.int64)
    todo = np.empty(count, np.int64)
    k = 0
    for root in range(count):
        if rcv[root] != root:
            continue
        top = 0
        todo[top] = root
        top += 1
        while top > 0:
            top -= 1
            c = todo[top]
            order[k] = c
            k += 1
            for d in range(start[c], start[c + 1]):
                todo[top] = donors[d]
                top += 1
    return order


@numba.njit(cache=True)
def erode(h, rcv, dist, order, uplift, pinned, cell, dt, K, m):
    flat = h.ravel()
    up = uplift.ravel()
    pin = pinned.ravel()
    count = flat.shape[0]
    area = np.full(count, cell * cell, np.float32)
    for o in range(count - 1, -1, -1):
        i = order[o]
        if rcv[i] != i:
            area[rcv[i]] += area[i]
    for o in range(count):
        i = order[o]
        if pin[i] >= 0.0:
            flat[i] = pin[i]
            continue
        hi = flat[i] + dt * up[i]
        r = rcv[i]
        if r == i:
            flat[i] = hi
            continue
        f = dt * K * area[i] ** m / dist[i]
        flat[i] = (hi + f * flat[r]) / (1.0 + f)
    return area.reshape(h.shape)


@numba.njit(parallel=True, cache=True)
def thermal(h, out, pinned, area, cell, talus, rate, creep):
    """Talus collapse and soil creep. Rivers carry off what falls in: the
    more water a cell carries (4 to 30 cells' worth), the less material it
    gains, so channels keep draining between the depression fills."""
    n = h.shape[0]
    for y in numba.prange(n):
        for x in range(n):
            hp = h[y, x]
            gain = 0.0
            loss = 0.0
            mean = 0.0
            for k in range(8):
                hq = h[(y + NBR[k, 1]) % n, (x + NBR[k, 0]) % n]
                limit = talus * cell * (1.41421356 if (k & 1) == 1 else 1.0)
                gain += max(hq - hp - limit, 0.0)
                loss += max(hp - hq - limit, 0.0)
                mean += hq
            mean *= 0.125
            t = min(max((area[y, x] / (cell * cell) - 4.0) / 26.0, 0.0), 1.0)
            keep = 1.0 - t * t * (3.0 - 2.0 * t)
            c = creep * (mean - hp)
            if c > 0.0:
                c *= keep
            out[y, x] = pinned[y, x] if pinned[y, x] >= 0.0 else hp + rate * (gain * keep - loss) + c


@numba.njit(parallel=True, cache=True)
def fill_small_pits(h, out, pinned, eps):
    """Raise single-cell pits (creep leaves them in valley floors) to just
    above their lowest neighbour, so rivers keep draining between the full
    depression fills."""
    n = h.shape[0]
    for y in numba.prange(n):
        for x in range(n):
            hp = h[y, x]
            lowest = 1e30
            for k in range(8):
                lowest = min(lowest, h[(y + NBR[k, 1]) % n, (x + NBR[k, 0]) % n])
            out[y, x] = hp if pinned[y, x] >= 0.0 or hp > lowest else lowest + eps


def run_level(h, n):
    iters, dt, fill_every = SCHEDULE[n]
    cell = TILE_KM * 1000.0 / n
    pinned = outlet_heights(n)
    uplift = at_level(_uplift, n)
    h = h.astype(np.float32)
    tmp = np.empty_like(h)
    t0 = time.time()
    area = None
    for it in range(iters):
        if it % fill_every == 0:
            priority_flood(h, pinned, FILL_EPS)
        rcv, dist = receivers(h, pinned, cell, it * 7919 + n)
        order = drainage_order(rcv)
        area = erode(h, rcv, dist, order, uplift, pinned, cell, dt, K_EROSION, M_EXP)
        thermal(h, tmp, pinned, area, cell, TALUS, 0.1, CREEP)  # 8 x 0.1 + 0.15 < 1: stable
        h, tmp = tmp, h
        for _ in range(3):
            fill_small_pits(h, tmp, pinned, FILL_EPS)
            h, tmp = tmp, h
        if n == START and (it + 1) % 100 == 0:
            print(f"  {it + 1}: relief {h.min():.0f}..{h.max():.0f} m", flush=True)
    print(f"level {n}: {iters} iterations in {time.time() - t0:.1f} s, cell {cell:.0f} m, "
          f"relief {h.min():.0f}..{h.max():.0f} m, max area {area.max() / 1e6:.0f} km^2", flush=True)
    return h, area


def simulate(final):
    n = START
    h = periodic_noise(n, n / 8, seed=SEED) * 150.0 + 300.0
    while True:
        h, _ = run_level(h, n)
        if n >= final:
            break
        # 2x: smooth upsample plus new roughness at the new finest scales
        n *= 2
        cell = TILE_KM * 1000.0 / n
        h = ndimage.zoom(h, 2, order=3, mode="grid-wrap", grid_mode=True).astype(np.float32)
        h += periodic_noise(n, 16, slope=-2.5, seed=SEED + n) * (0.1 * cell)
    # a little creep at the end softens the single-cell zigzags that steepest
    # descent leaves along channels
    tmp = np.empty_like(h)
    nopin = np.full_like(h, -1.0)
    for _ in range(4):
        thermal(h, tmp, nopin, np.zeros_like(h), TILE_KM * 1000.0 / n, 10.0, 0.0, 0.25)
        h, tmp = tmp, h
    return ((h - h.min()) / (h.max() - h.min())).astype(np.float32)


def valleys(land):
    size = land.shape[0]
    sigma = size / (2.0 * HIGH_PASS_DIV)
    f = np.fft.rfftfreq(size)[None, :] ** 2 + np.fft.fftfreq(size)[:, None] ** 2
    blur = np.fft.irfft2(np.fft.rfft2(land) * np.exp(-2.0 * np.pi ** 2 * sigma ** 2 * f), s=land.shape)
    crest = np.clip((land - blur - CREST_RANGE[0]) / (CREST_RANGE[1] - CREST_RANGE[0]), 0.0, 1.0)
    return (1.0 - crest) ** VALLEY_POW


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--size", type=int, default=4096)
    ap.add_argument("--stamp")
    args = ap.parse_args()
    if args.stamp:
        land = np.load(args.stamp).astype(np.float32)
        land = (land - land.min()) / (land.max() - land.min())
        if land.shape[0] != args.size:
            k = land.shape[0] // args.size
            land = land.reshape(args.size, k, args.size, k).mean(axis=(1, 3))
    else:
        t0 = time.time()
        land = simulate(args.size)
        print(f"simulated in {time.time() - t0:.0f} s")
    v = valleys(land)
    with open(args.output, "wb") as out:
        out.write(b"EVAL")
        out.write(struct.pack("<Iff", v.shape[0], TILE_M, float(v.mean())))
        out.write(np.round(v * 65535.0).astype("<u2").tobytes())
    print(f"{args.output}: {v.shape[0]}^2, mean valley {v.mean():.3f}")


if __name__ == "__main__":
    main()
