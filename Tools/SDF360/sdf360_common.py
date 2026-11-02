"""Shared definitions for the MoonToon SDF360 (full-angle face SDF) tools.

Everything that the bake, the preview and the material must agree on lives here:

* the grid definition (theta / phi sample positions, mirror mode, wrap rules),
* the light-direction -> (theta, phi) mapping in the head frame,
* the 4-cell lookup (indices + weights + UV flip) used by the runtime shader,
* the encoding v = saturate(0.5 + d / (2R)),
* the atlas UV mapping,
* mask file naming and image I/O.

Head-frame convention (Unreal, left-handed, Z up):
    Up      : unit up vector of the head.
    Forward : unit vector the face looks along (made orthogonal to Up).
    Right   : cross(Up, Forward)   (UE: X forward, Z up -> Right = +Y, the character's right).
    L       : unit vector FROM the shaded surface TOWARD the light.
    theta   = degrees(atan2(dot(L, Right), dot(L, Forward)))   in [-180, 180]
              0 = light in front, +90 = light on the character's right side.
    phi     = degrees(asin(dot(L, Up)))                         in [-90, 90]
"""

from __future__ import annotations

import json
import math
import os
import re
from dataclasses import dataclass, field, asdict
from typing import Iterable, List, Optional, Sequence, Tuple

import numpy as np

TOOL_NAME = "MoonToon SDF360 baker"
TOOL_VERSION = "1.0.0"

SIDECAR_NAME = "sdf360.json"
ATLAS_NAME = "atlas.png"
SLICE_PATTERN = "slice_{k:03d}.png"
COVERAGE_NAME = "coverage.png"
MASKS_SIDECAR_NAME = "masks.json"

ENCODING_FORMULA = "v = saturate(0.5 + d / (2 * R)); d = signed distance to the lit/shadow boundary in cell texels (positive = lit), R = spread_texels"
DECODING_FORMULA = "s = (v - 0.5) * 2  (R-normalised signed distance, |s| <= 1); d_texels = s * R"
ATLAS_UV_FORMULA = ("atlasUV = ((i*(W+2P) + P + uv.x*W) / AtlasW, (j*(H+2P) + P + uv.y*H) / AtlasH), "
                    "uv = saturate(faceUV) (x mirrored to 1-x when the lookup says so), i = theta index (column), "
                    "j = phi index (row, row 0 = phi_min at the top of the image), AtlasW = Ntheta*(W+2P), AtlasH = Nphi*(H+2P)")
TEXEL_CONVENTION = ("Standard texture convention: cell texel k covers face UV [k/W, (k+1)/W) and its centre is at (k+0.5)/W. "
                    "uv = 0 / uv = 1 land on the outer EDGES of the first / last cell texel, not on texel centres; "
                    "bilinear filtering there blends with the replicated padding texel, which equals clamp addressing. "
                    "Mask pixel k, slice texel k and cell texel k all use this same convention, so atlas and array sampling agree.")
DISTANCE_CONVENTION = ("Half-texel convention: the boundary lies half way between a lit and a dark texel centre. "
                       "For a lit texel d = +(Euclidean distance to the nearest dark texel centre - 0.5); "
                       "for a dark texel d = -(distance to the nearest lit texel centre - 0.5). "
                       "Adjacent lit/dark texels therefore store +0.5 / -0.5 and bilinear filtering crosses 0 exactly on the boundary.")

EPS_DEG = 1e-6


# ----------------------------------------------------------------------------------------------
# Grid
# ----------------------------------------------------------------------------------------------

@dataclass
class Grid:
    theta_count: int
    phi_count: int
    phi_min: float
    phi_max: float
    mirror: bool

    def validate(self) -> None:
        if self.mirror and self.theta_count < 2:
            raise ValueError("mirror mode needs --theta-count >= 2 (theta 0 and 180 at least)")
        if not self.mirror and self.theta_count < 2:
            raise ValueError("full-circle mode needs --theta-count >= 2")
        if self.phi_count < 1:
            raise ValueError("--phi-count must be >= 1")
        if self.phi_min < -90 - EPS_DEG or self.phi_max > 90 + EPS_DEG:
            raise ValueError("phi range must lie inside [-90, 90]")
        if self.phi_count == 1 and abs(self.phi_max - self.phi_min) > EPS_DEG:
            raise ValueError("--phi-count 1 needs --phi-min == --phi-max")
        if self.phi_count > 1 and not (self.phi_max > self.phi_min):
            raise ValueError("--phi-max must be greater than --phi-min")

    # -- sample positions ---------------------------------------------------------------------
    @property
    def theta_step(self) -> float:
        return 180.0 / (self.theta_count - 1) if self.mirror else 360.0 / self.theta_count

    @property
    def theta_start(self) -> float:
        return 0.0 if self.mirror else -180.0

    @property
    def phi_step(self) -> float:
        return (self.phi_max - self.phi_min) / (self.phi_count - 1) if self.phi_count > 1 else 0.0

    def theta_values(self) -> List[float]:
        return [self.theta_start + i * self.theta_step for i in range(self.theta_count)]

    def phi_values(self) -> List[float]:
        if self.phi_count == 1:
            return [float(self.phi_min)]
        return [self.phi_min + j * self.phi_step for j in range(self.phi_count)]

    def cells(self) -> List[Tuple[int, int, float, float]]:
        """All cells as (i, j, theta, phi), in slice order k = j * Ntheta + i."""
        out = []
        for j, p in enumerate(self.phi_values()):
            for i, t in enumerate(self.theta_values()):
                out.append((i, j, t, p))
        return out

    def slice_index(self, i: int, j: int) -> int:
        return j * self.theta_count + i

    def pole_rows(self) -> List[int]:
        rows = []
        if abs(self.phi_min + 90.0) < EPS_DEG:
            rows.append(0)
        if abs(self.phi_max - 90.0) < EPS_DEG and self.phi_count > 1:
            rows.append(self.phi_count - 1)
        return rows

    # -- runtime lookup (must match sdf360_sample.hlsl) --------------------------------------
    def lookup(self, theta_deg: float, phi_deg: float):
        """Return (i0, i1, j0, j1, t_theta, t_phi, flip) exactly as the shader computes them.

        Result s = lerp(lerp(s(i0,j0), s(i1,j0), t_theta), lerp(s(i0,j1), s(i1,j1), t_theta), t_phi),
        every cell sampled at uv with uv.x -> 1 - uv.x when flip is True.
        """
        nt = float(self.theta_count)
        if self.mirror:
            flip = theta_deg < 0.0
            step = 180.0 / max(nt - 1.0, 1.0)
            u = min(max(abs(theta_deg) / step, 0.0), nt - 1.0)
            i0 = min(math.floor(u), max(nt - 2.0, 0.0))
            tt = u - i0
            i1 = min(i0 + 1.0, nt - 1.0)
        else:
            flip = False
            step = 360.0 / nt
            u = (theta_deg + 180.0) / step          # [0, Ntheta]
            f0 = math.floor(u)
            tt = u - f0
            i0 = f0 - nt * math.floor(f0 / nt)      # wrap, also maps u == Ntheta to 0
            i1 = i0 + 1.0
            if i1 >= nt:
                i1 -= nt
        np_ = float(self.phi_count)
        if self.phi_count > 1:
            pstep = (self.phi_max - self.phi_min) / (np_ - 1.0)
            v = min(max((phi_deg - self.phi_min) / pstep, 0.0), np_ - 1.0)
            j0 = min(math.floor(v), np_ - 2.0)
            tp = v - j0
            j1 = j0 + 1.0
        else:
            j0 = j1 = 0.0
            tp = 0.0
        return int(i0), int(i1), int(j0), int(j1), float(tt), float(tp), bool(flip)

    def to_json(self) -> dict:
        d = asdict(self)
        d.update({
            "theta_start": self.theta_start,
            "theta_step": self.theta_step,
            "phi_step": self.phi_step,
            "theta_values": self.theta_values(),
            "phi_values": self.phi_values(),
            "theta_wrap": not self.mirror,
            "theta_range": [0.0, 180.0] if self.mirror else [-180.0, 180.0],
            "theta_range_note": ("inclusive [0,180]; theta<0 uses |theta| with face uv.x -> 1-uv.x" if self.mirror
                                 else "periodic [-180,180); interpolation wraps index Ntheta-1 <-> 0"),
            "phi_note": "inclusive grid, values outside [phi_min, phi_max] are clamped",
            "pole_rows": self.pole_rows(),
        })
        return d

    @staticmethod
    def from_json(d: dict) -> "Grid":
        return Grid(int(d["theta_count"]), int(d["phi_count"]), float(d["phi_min"]), float(d["phi_max"]), bool(d["mirror"]))


def add_grid_args(ap, required: bool = True) -> None:
    g = ap.add_argument_group("grid")
    req = dict(required=True) if required else dict(default=None)
    g.add_argument("--theta-count", type=int, help="number of theta (azimuth) samples", **req)
    g.add_argument("--phi-count", type=int, help="number of phi (elevation) samples", **req)
    g.add_argument("--phi-min", type=float, help="lowest elevation in degrees (row 0)", **req)
    g.add_argument("--phi-max", type=float, help="highest elevation in degrees (last row)", **req)
    g.add_argument("--mirror", action="store_true", default=None if not required else False,
                   help="theta covers [0,180]; the other side reuses the cells with uv.x mirrored")


def grid_from_args(args, fallback_json: Optional[str] = None) -> Grid:
    base = None
    if fallback_json and os.path.isfile(fallback_json):
        with open(fallback_json, "r", encoding="utf-8") as f:
            base = Grid.from_json(json.load(f)["grid"])
    vals = [args.theta_count, args.phi_count, args.phi_min, args.phi_max]
    if any(v is None for v in vals) and base is None:
        raise SystemExit("grid arguments missing (--theta-count/--phi-count/--phi-min/--phi-max) "
                         f"and no {MASKS_SIDECAR_NAME} found to take them from")
    pick = lambda v, b: v if v is not None else b  # noqa: E731
    grid = Grid(int(pick(args.theta_count, base.theta_count if base else None)),
                int(pick(args.phi_count, base.phi_count if base else None)),
                float(pick(args.phi_min, base.phi_min if base else None)),
                float(pick(args.phi_max, base.phi_max if base else None)),
                bool(args.mirror) if args.mirror is not None else (base.mirror if base else False))
    grid.validate()
    return grid


# ----------------------------------------------------------------------------------------------
# Head frame and directions
# ----------------------------------------------------------------------------------------------

def parse_vec3(s: str) -> np.ndarray:
    parts = [float(x) for x in s.replace(" ", "").split(",")]
    if len(parts) != 3:
        raise ValueError(f"expected x,y,z but got {s!r}")
    return np.array(parts, dtype=np.float64)


def head_frame(forward, up, right=None, handedness: str = "left"):
    """Orthonormal head frame (F, R, U).

    Up is kept, Forward is made orthogonal to Up. Right defaults to cross(Up, Forward), which is the
    character's right in a left-handed space such as Unreal. In a right-handed space (most DCC exports,
    OBJ, glTF) the character's right is cross(Forward, Up); pass handedness='right' for that.
    """
    u = np.asarray(up, dtype=np.float64)
    u = u / np.linalg.norm(u)
    f = np.asarray(forward, dtype=np.float64)
    f = f - u * np.dot(f, u)
    nf = np.linalg.norm(f)
    if nf < 1e-8:
        raise ValueError("forward and up are parallel")
    f = f / nf
    if right is not None:
        r = np.asarray(right, dtype=np.float64)
        r = r - u * np.dot(r, u) - f * np.dot(r, f)
        r = r / np.linalg.norm(r)
    elif handedness == "left":
        r = np.cross(u, f)
    elif handedness == "right":
        r = np.cross(f, u)
    else:
        raise ValueError("handedness must be 'left' or 'right'")
    return f, r, u


def light_angles(L, forward, up, right=None) -> Tuple[float, float]:
    """(theta, phi) in degrees for light direction L (surface -> light) in the given head frame.

    With right=None the frame is (Forward, cross(Up, Forward), Up), i.e. the runtime/shader convention.
    """
    f, r, u = head_frame(forward, up, right, "left")
    ln = np.asarray(L, dtype=np.float64)
    ln = ln / np.linalg.norm(ln)
    x = float(np.dot(ln, f))
    y = float(np.dot(ln, r))
    z = float(np.clip(np.dot(ln, u), -1.0, 1.0))
    theta = 0.0 if (x * x + y * y) < 1e-12 else math.degrees(math.atan2(y, x))   # pole guard, same as HLSL
    phi = math.degrees(math.asin(z))
    return theta, phi


def direction_from_angles(theta_deg: float, phi_deg: float) -> np.ndarray:
    """Light direction in head-frame components (F, R, U) for the given angles."""
    t = math.radians(theta_deg)
    p = math.radians(phi_deg)
    return np.array([math.cos(p) * math.cos(t), math.cos(p) * math.sin(t), math.sin(p)], dtype=np.float64)


# ----------------------------------------------------------------------------------------------
# Encoding
# ----------------------------------------------------------------------------------------------

def encode(d_cell: np.ndarray, spread: float) -> np.ndarray:
    """Signed distance in cell texels -> normalised value v in [0, 1] (float64)."""
    return np.clip(0.5 + np.asarray(d_cell, dtype=np.float64) / (2.0 * spread), 0.0, 1.0)


def quantize(v: np.ndarray, bits: int) -> np.ndarray:
    if bits == 16:
        return np.round(v * 65535.0).astype(np.uint16)
    if bits == 8:
        return np.round(v * 255.0).astype(np.uint8)
    raise ValueError("bits must be 8 or 16")


def dequantize(q: np.ndarray) -> np.ndarray:
    if q.dtype == np.uint8:
        return q.astype(np.float64) / 255.0
    return q.astype(np.float64) / 65535.0


def decode_s(v: np.ndarray) -> np.ndarray:
    """Normalised texture value -> R-normalised signed value s (same expression as the shader)."""
    return (v - 0.5) * 2.0


# ----------------------------------------------------------------------------------------------
# Atlas mapping
# ----------------------------------------------------------------------------------------------

def atlas_size(grid: Grid, w: int, h: int, pad: int) -> Tuple[int, int]:
    return grid.theta_count * (w + 2 * pad), grid.phi_count * (h + 2 * pad)


def atlas_uv(face_uv_x, face_uv_y, i: int, j: int, w: int, h: int, pad: int, atlas_w: int, atlas_h: int):
    """Atlas UV for face uv (already saturated / mirrored). Same expression as MoonSdf360AtlasUV()."""
    ax = (i * (w + 2 * pad) + pad + np.asarray(face_uv_x) * w) / atlas_w
    ay = (j * (h + 2 * pad) + pad + np.asarray(face_uv_y) * h) / atlas_h
    return ax, ay


def pack_atlas(cells: np.ndarray, pad: int) -> np.ndarray:
    """cells: (Nphi, Ntheta, H, W) -> atlas (Nphi*(H+2P), Ntheta*(W+2P)); padding = edge replication."""
    nphi, ntheta, h, w = cells.shape
    sh, sw = h + 2 * pad, w + 2 * pad
    atlas = np.empty((nphi * sh, ntheta * sw), dtype=cells.dtype)
    for j in range(nphi):
        for i in range(ntheta):
            atlas[j * sh:(j + 1) * sh, i * sw:(i + 1) * sw] = np.pad(cells[j, i], pad, mode="edge")
    return atlas


def sample_bilinear(img: np.ndarray, u, v) -> np.ndarray:
    """GPU-style bilinear sample (clamp addressing) of a 2D float image at normalised UV.

    Texel k is centred at (k + 0.5) / size, exactly like a D3D/Unreal Texture2D with a bilinear sampler.
    """
    h, w = img.shape
    x = np.asarray(u, dtype=np.float64) * w - 0.5
    y = np.asarray(v, dtype=np.float64) * h - 0.5
    x0 = np.floor(x)
    y0 = np.floor(y)
    fx = x - x0
    fy = y - y0
    x0 = x0.astype(np.int64)
    y0 = y0.astype(np.int64)
    x1 = np.clip(x0 + 1, 0, w - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)
    x0 = np.clip(x0, 0, w - 1)
    y0 = np.clip(y0, 0, h - 1)
    a = img[y0, x0] * (1 - fx) + img[y0, x1] * fx
    b = img[y1, x0] * (1 - fx) + img[y1, x1] * fx
    return a * (1 - fy) + b * fy


# ----------------------------------------------------------------------------------------------
# Naming and image I/O
# ----------------------------------------------------------------------------------------------

def fmt_angle(x: float) -> str:
    r = round(x)
    if abs(x - r) < 1e-6:
        return str(int(r) + 0)  # "+ 0" turns -0 into 0
    s = f"{x:.3f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def mask_filename(theta: float, phi: float) -> str:
    return f"mask_t{fmt_angle(theta)}_p{fmt_angle(phi)}.png"


MASK_RE = re.compile(r"^mask_t([+-]?\d+(?:\.\d+)?)_p([+-]?\d+(?:\.\d+)?)\.png$", re.IGNORECASE)


def find_mask_files(mask_dir: str, grid: Grid, tol: float = 0.5 + 1e-6):
    """Map each grid cell to a mask file. Returns ({(i,j): path}, missing_cells, unused_files)."""
    files = []
    for name in sorted(os.listdir(mask_dir)):
        m = MASK_RE.match(name)
        if m:
            files.append((name, float(m.group(1)), float(m.group(2))))
    mapping = {}
    used = set()
    missing = []
    for (i, j, t, p) in grid.cells():
        hits = []
        for (name, ft, fp) in files:
            dt = abs(ft - t)
            if not grid.mirror:
                dt = min(dt, abs(dt - 360.0))  # 180 is the same direction as -180
            if dt <= tol and abs(fp - p) <= tol:
                hits.append(name)
        if not hits:
            missing.append((i, j, t, p))
            continue
        if len(hits) > 1:
            raise SystemExit(f"cell theta={t:g} phi={p:g} matches several files: {hits}")
        mapping[(i, j)] = os.path.join(mask_dir, hits[0])
        used.add(hits[0])
    unused = [n for (n, _, _) in files if n not in used]
    return mapping, missing, unused


def load_gray(path: str) -> np.ndarray:
    """Load an image as float64 grey values in [0, 1]. Alpha (if any) multiplies the grey value."""
    from PIL import Image
    img = Image.open(path)
    img.load()
    mode = img.mode
    if mode == "P":
        img = img.convert("RGBA")
        mode = "RGBA"
    if mode == "1":
        return np.asarray(img, dtype=np.float64)
    arr = np.asarray(img)
    if mode in ("I;16", "I;16B", "I;16L"):
        return arr.astype(np.float64) / 65535.0
    if mode == "I":
        # 16-bit PNGs open as 'I' in some Pillow versions
        mx = 65535.0 if arr.max() > 255 or path.lower().endswith(".png") else 255.0
        return arr.astype(np.float64) / mx
    if mode == "F":
        return arr.astype(np.float64)
    a = arr.astype(np.float64)
    maxv = 65535.0 if arr.dtype == np.uint16 else 255.0
    a = a / maxv
    if a.ndim == 2:
        return a
    if mode in ("LA", "La"):
        return a[..., 0] * a[..., 1]
    rgb = a[..., :3]
    grey = rgb @ np.array([0.2126, 0.7152, 0.0722])
    if a.shape[-1] == 4:
        grey = grey * a[..., 3]
    return grey


def save_gray(path: str, q: np.ndarray) -> None:
    """Save a uint8 or uint16 2D array as a greyscale PNG (16-bit PNG for uint16)."""
    from PIL import Image
    if q.dtype == np.uint16:
        img = Image.fromarray(q)  # mode I;16
        if img.mode not in ("I;16", "I;16L", "I;16B"):
            img = Image.frombytes("I;16", (q.shape[1], q.shape[0]), q.astype("<u2").tobytes())
    elif q.dtype == np.uint8:
        img = Image.fromarray(q)  # mode L
    else:
        raise TypeError(q.dtype)
    img.save(path)


def save_mask(path: str, lit: np.ndarray) -> None:
    save_gray(path, np.where(lit, 255, 0).astype(np.uint8))


def write_json(path: str, data: dict) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def read_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def parse_size(s: str) -> Tuple[int, int]:
    """'256' -> (256, 256); '256x128' -> (W=256, H=128)."""
    s = s.lower().replace(" ", "")
    if "x" in s:
        a, b = s.split("x")
        return int(a), int(b)
    return int(s), int(s)
