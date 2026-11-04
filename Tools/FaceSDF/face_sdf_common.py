"""Shared helpers for the classic face SDF tools (MoonToon T24 / T28).

A classic face SDF ("threshold map") stores, per face texel, the horizontal light angle at which that texel
turns dark, as v = angle / 180 deg: MoonToon lights a texel while v > the light's azimuth off the face
forward / 180 (Toon/ToonShadingFeature.ush, ApplyDistanceFieldFacialShadow). The map is read for one half
of the face and mirrored in U for the other (MF_ToonDFFacialShadow).

Delivery formats (Docs/FaceSDF.md):
  16-bit   one channel, 16-bit PNG -> import sRGB off, Half Float (R16F) or Grayscale (G16), no mips
  R+G      8-bit RGBA PNG with v spread over R and G, read back as (R + G) / 2: 511 levels (~9 bits) from an
           uncompressed BGRA8 texture -- Endfield's layout. Material switch "Distance Field Facial Shadow Map
           R+G Average".
Either way only "Face SDF Extended Payload" carries more than 8 bits to the lights.

Pure numpy + Pillow. Image IO and the signed distance come from ../SDF360 (same conventions)."""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SDF360 = os.path.join(os.path.dirname(HERE), "SDF360")
if SDF360 not in sys.path:
    sys.path.insert(0, SDF360)
import sdf360_common as C  # noqa: E402  (load_gray, save_gray, MASK_RE, fmt_angle)
import sdf_edt  # noqa: E402  (signed_distance)

CHANNELS = {"r": 0, "g": 1, "b": 2, "a": 3}


SINGLE = ("L", "I", "I;16", "I;16B", "I;16L", "F")


def load_channel(path, channel="r"):
    """One channel of an image as float64 in [0, 1]. channel: r, g, b, a, gray. A single-channel image
    (8-bit L, 16-bit I;16, float F) is returned as is whatever the channel; 16-bit sources must be
    single-channel (Pillow reads 16-bit RGB as 8-bit)."""
    from PIL import Image
    img = Image.open(path)
    img.load()
    if img.mode in SINGLE:
        return _load_single(img)
    if channel == "gray":
        return _load_single(img.convert("L"))
    arr = np.asarray(img if img.mode in ("RGBA", "RGB") else img.convert("RGBA"))
    idx = CHANNELS[channel]
    if idx >= arr.shape[-1]:
        return np.ones(arr.shape[:2])
    return arr[..., idx].astype(np.float64) / 255.0


def _load_single(img):
    arr = np.asarray(img)
    if img.mode in ("I;16", "I;16B", "I;16L"):
        return arr.astype(np.float64) / 65535.0
    if img.mode == "I":
        return arr.astype(np.float64) / (65535.0 if arr.max() > 255 else 255.0)
    if img.mode == "F":
        return arr.astype(np.float64)
    return arr.astype(np.float64) / 255.0


def srgb_to_linear(v):
    v = np.asarray(v, dtype=np.float64)
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def gaussian_blur(img, sigma):
    """Separable Gaussian, edge-clamped. sigma in texels; 0 = no-op."""
    if sigma <= 0:
        return img
    r = max(1, int(np.ceil(3.0 * sigma)))
    x = np.arange(-r, r + 1, dtype=np.float64)
    k = np.exp(-0.5 * (x / sigma) ** 2)
    k /= k.sum()
    out = img
    for axis in (0, 1):
        pad = [(0, 0), (0, 0)]
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode="edge")
        acc = np.zeros_like(out)
        n = out.shape[axis]
        for i, w in enumerate(k):
            acc += w * (p[i:i + n, :] if axis == 0 else p[:, i:i + n])
        out = acc
    return out


def encode_rg(v):
    """v in [0, 1] -> uint8 RGBA (R, G, 0, 255) with (R + G) / 510 == round(v * 510) / 510."""
    total = np.round(np.clip(v, 0.0, 1.0) * 510.0).astype(np.int32)
    r = (total + 1) // 2
    g = total // 2
    out = np.zeros(v.shape + (4,), dtype=np.uint8)
    out[..., 0] = r
    out[..., 1] = g
    out[..., 3] = 255
    return out


def decode_rg(rgba):
    return (rgba[..., 0].astype(np.float64) + rgba[..., 1].astype(np.float64)) / 510.0


def save_rgba(path, rgba):
    from PIL import Image
    Image.fromarray(rgba, "RGBA").save(path)


def save16(path, v):
    C.save_gray(path, np.round(np.clip(v, 0.0, 1.0) * 65535.0).astype(np.uint16))


def levels_report(v, name):
    """How many distinct 8-bit / 16-bit levels the map really has (a dequantised 8-bit source still has
    ~256 plateaus unless it was smoothed)."""
    q8 = np.unique(np.round(v * 255.0)).size
    q16 = np.unique(np.round(v * 65535.0)).size
    return "%s: %d x %d, min %.4f max %.4f, distinct 8-bit levels %d, 16-bit levels %d" % (
        name, v.shape[1], v.shape[0], v.min(), v.max(), q8, q16)
