"""Default textures for the MoonToon effect functions (MaterialFunctions/FX, tasks T09-T11).

Run:  uv run --with numpy --with pillow python make_fx_textures.py [out_dir]

All are linear data (import with sRGB OFF, address Wrap, mips on unless noted):
  T_ToonFX_GlitterDots   256x256  R = sparse soft dots (intensity), G = per-dot hue random   (no mips, filter Nearest or Bilinear)
  T_ToonFX_Caustic       256x256  R = tileable caustic-like ridges (flow light), A = 1
  T_ToonFX_Noise         256x256  R/G = two octaves of tileable value noise (dissolve, reveal edges, flicker)
  T_ToonFX_RainStreaks   128x512  R = vertical streaks (wet), G = droplet heads
"""
import os
import sys

import numpy as np
from PIL import Image

RNG = np.random.default_rng(20261101)


def save(arr, path):
    arr = np.clip(arr, 0.0, 1.0)
    if arr.ndim == 2:
        arr = np.stack([arr, arr, arr, np.ones_like(arr)], axis=-1)
    elif arr.shape[-1] == 3:
        arr = np.concatenate([arr, np.ones(arr.shape[:2] + (1,))], axis=-1)
    Image.fromarray(np.round(arr * 255.0).astype(np.uint8), "RGBA").save(path)
    print("wrote", path, arr.shape[1], "x", arr.shape[0])


def glitter_dots(size=256, count=2600):
    r = np.zeros((size, size))
    g = np.zeros((size, size))
    yy, xx = np.mgrid[0:size, 0:size]
    for _ in range(count):
        cx, cy = RNG.uniform(0, size, 2)
        rad = RNG.uniform(0.8, 2.2)
        inten = RNG.uniform(0.8, 1.0)
        hue = RNG.uniform(0.0, 1.0)
        # wrapped distance for tiling
        dx = np.minimum(np.abs(xx - cx), size - np.abs(xx - cx))
        dy = np.minimum(np.abs(yy - cy), size - np.abs(yy - cy))
        d = np.sqrt(dx * dx + dy * dy)
        w = np.clip(1.0 - d / rad, 0.0, 1.0) ** 0.5 * inten  # flat-topped: the two-layer product must reach the threshold
        upd = w > r
        r = np.where(upd, w, r)
        g = np.where(upd, hue, g)
    return np.stack([r, g, np.zeros_like(r), np.ones_like(r)], axis=-1)


def value_noise(size, cells, seed):
    rng = np.random.default_rng(seed)
    lat = rng.uniform(0, 1, (cells, cells))
    t = np.arange(size) / size * cells
    i0 = np.floor(t).astype(int) % cells
    i1 = (i0 + 1) % cells
    f = t - np.floor(t)
    f = f * f * (3 - 2 * f)
    a = lat[np.ix_(i0, i0)]
    b = lat[np.ix_(i0, i1)]
    c = lat[np.ix_(i1, i0)]
    d = lat[np.ix_(i1, i1)]
    fx = f[None, :]
    fy = f[:, None]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fbm(size, base_cells, octaves, seed):
    out = np.zeros((size, size))
    amp, total = 1.0, 0.0
    for o in range(octaves):
        out += value_noise(size, base_cells * (2 ** o), seed + o) * amp
        total += amp
        amp *= 0.5
    return out / total


def caustic(size=256):
    n1 = fbm(size, 4, 4, 11)
    n2 = fbm(size, 4, 4, 23)
    ridges = 1.0 - np.abs(np.sin((n1 * 3.0 + n2 * 2.0) * np.pi * 2.0))
    ridges = ridges ** 6
    return np.stack([ridges, ridges, ridges, np.ones_like(ridges)], axis=-1)


def noise_tex(size=256):
    a = fbm(size, 8, 4, 41)
    b = fbm(size, 16, 3, 57)
    a = (a - a.min()) / (a.max() - a.min())
    b = (b - b.min()) / (b.max() - b.min())
    return np.stack([a, b, np.zeros_like(a), np.ones_like(a)], axis=-1)


def rain_streaks(w=128, h=512, count=70):
    r = np.zeros((h, w))
    g = np.zeros((h, w))
    yy, xx = np.mgrid[0:h, 0:w]
    for _ in range(count):
        cx = RNG.uniform(0, w)
        y0 = RNG.uniform(0, h)
        length = RNG.uniform(60, 220)
        width = RNG.uniform(0.6, 1.6)
        dx = np.minimum(np.abs(xx - cx), w - np.abs(xx - cx))
        dy = (yy - y0) % h
        along = np.clip(1.0 - dy / length, 0.0, 1.0)
        streak = np.clip(1.0 - dx / width, 0.0, 1.0) * along * (dy < length)
        r = np.maximum(r, streak * RNG.uniform(0.5, 1.0))
        head = np.clip(1.0 - np.sqrt(dx * dx + ((dy - 2.0) ** 2) * 0.5) / (width * 2.2), 0.0, 1.0)
        g = np.maximum(g, head)
    return np.stack([r, g, np.zeros_like(r), np.ones_like(r)], axis=-1)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    os.makedirs(out, exist_ok=True)
    save(glitter_dots(), os.path.join(out, "T_ToonFX_GlitterDots.png"))
    save(caustic(), os.path.join(out, "T_ToonFX_Caustic.png"))
    save(noise_tex(), os.path.join(out, "T_ToonFX_Noise.png"))
    save(rain_streaks(), os.path.join(out, "T_ToonFX_RainStreaks.png"))
