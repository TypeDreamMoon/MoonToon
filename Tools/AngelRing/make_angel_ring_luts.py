"""Default lookup textures for the Angel Ring material function (MF_ToonAngelRing, task T06).

Run:  uv run --with numpy --with pillow python make_angel_ring_luts.py [out_dir]

Writes two linear (non-sRGB) PNGs; import them with sRGB off, compression "Masks" or
"UserInterface2D", filter bilinear, address clamp, no mips:

  T_AngelRing_LUT_A.png  256x16  mode A (Wuthering Waves style), sampled at u = band coordinate:
      row 0  bright ring profile     (centre 0.5, hard-ish edges)
      row 1  dark band profile       (wider band around the ring; multiplies the diffuse down)
      row 2  thin colour line        (a narrow line just above the ring)
      row 3  unused (0)
    Each row is 4 texels tall (a 256x4 texture failed to build in the editor: 'contains no miplevels');
    rows are still sampled at v = (row + 0.5) / 4, which lands between two identical texel rows.

  T_AngelRing_LUT_B.png  64x64   mode B (Arknights: Endfield style):
      x = Kajiya lobe * mask, y = facing^2 * (T.H > 0)
      value = a hard threshold near x = 0.5 whose softness grows as y drops, so the band is crisp on
      the lit-facing side and feathers on the other ("one side hard, one side soft").
"""
import os
import sys

import numpy as np
from PIL import Image


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def lut_a(width=256):
    u = (np.arange(width) + 0.5) / width
    ring = smoothstep(0.40, 0.46, u) * (1.0 - smoothstep(0.54, 0.60, u))
    dark = smoothstep(0.22, 0.36, u) * (1.0 - smoothstep(0.64, 0.78, u))
    dark = np.clip(dark - ring, 0.0, 1.0)  # the dark band hugs the ring, it does not cover it
    line = smoothstep(0.64, 0.66, u) * (1.0 - smoothstep(0.68, 0.70, u))
    rows = np.repeat(np.stack([ring, dark, line, np.zeros_like(u)], axis=0), 4, axis=0)  # (16, W)
    rgb = np.repeat(rows[:, :, None], 3, axis=2)
    alpha = np.ones((16, width, 1))
    return np.concatenate([rgb, alpha], axis=2)


def lut_b(size=64):
    x = (np.arange(size) + 0.5) / size
    y = (np.arange(size) + 0.5) / size
    X, Y = np.meshgrid(x, y)
    soft = 0.02 + 0.23 * (1.0 - Y)
    val = smoothstep(0.5 - soft, 0.5 + 0.02, X)
    rgb = np.repeat(val[:, :, None], 3, axis=2)
    alpha = np.ones((size, size, 1))
    return np.concatenate([rgb, alpha], axis=2)


def save(arr, path):
    img = Image.fromarray(np.clip(np.round(arr * 255.0), 0, 255).astype(np.uint8), "RGBA")
    img.save(path)
    print("wrote", path, img.size)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    os.makedirs(out, exist_ok=True)
    save(lut_a(), os.path.join(out, "T_AngelRing_LUT_A.png"))
    save(lut_b(), os.path.join(out, "T_AngelRing_LUT_B.png"))
