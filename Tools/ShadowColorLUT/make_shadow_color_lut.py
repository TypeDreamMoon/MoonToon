"""Default shadow-colour LUT for MF_ToonShadowColorLUT (task T08, gap G22).

Run:  uv run --with numpy --with pillow python make_shadow_color_lut.py [out_dir]

A 32^3 colour cube unwrapped into a 1024x32 strip, the layout Arknights: Endfield's skin shadow LUT
uses (and the same as Unreal's 256x16 colour grading strip, at 32 instead of 16 steps):
    texel (x, y): r = x % 32, b = x // 32, g = y        (each axis value = index / 31)
It maps an ALBEDO to the colour that albedo takes in the main light's shadow. Import with sRGB OFF,
compression VectorDisplacementmap (uncompressed RGBA8), filter bilinear, clamp, no mips.

T_ShadowColorLUT_Skin  -- warm, slightly saturated skin shadow (Endfield measured about
                          x(0.67, 0.53, 0.50) on its skin, warmer in the mid tones).
T_ShadowColorLUT_Half  -- 0.5 x albedo, the "single colour multiply" reference for A/B.
"""
import os
import sys

import numpy as np
from PIL import Image

N = 32


def cube_axes():
    x = np.arange(N * N)
    y = np.arange(N)
    X, Y = np.meshgrid(x, y)
    r = (X % N) / (N - 1.0)
    b = (X // N) / (N - 1.0)
    g = Y / (N - 1.0)
    return np.stack([r, g, b], axis=-1)  # (32, 1024, 3)


def saturate(c, s):
    luma = (c * np.array([0.2126, 0.7152, 0.0722])).sum(-1, keepdims=True)
    return np.clip(luma + (c - luma) * s, 0.0, 1.0)


def skin_shadow(albedo):
    # Warm multiply, a little extra saturation, and a lift of the red channel in the mid tones so
    # the terminator side reads as blood under skin rather than grey.
    base = np.power(albedo, 1.08) * np.array([0.70, 0.53, 0.49])
    mid = np.clip(1.0 - np.abs(albedo.mean(-1, keepdims=True) - 0.55) * 2.0, 0.0, 1.0)
    base = base + mid * np.array([0.035, 0.0, -0.01])
    return saturate(np.clip(base, 0.0, 1.0), 1.12)


def save(arr, path):
    rgba = np.concatenate([arr, np.ones(arr.shape[:2] + (1,))], axis=-1)
    Image.fromarray(np.clip(np.round(rgba * 255.0), 0, 255).astype(np.uint8), "RGBA").save(path)
    print("wrote", path)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    os.makedirs(out, exist_ok=True)
    albedo = cube_axes()
    save(skin_shadow(albedo), os.path.join(out, "T_ShadowColorLUT_Skin.png"))
    save(albedo * 0.5, os.path.join(out, "T_ShadowColorLUT_Half.png"))
