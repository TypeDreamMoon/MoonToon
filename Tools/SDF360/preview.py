"""Reconstruct the lit mask from a baked SDF360 exactly like the shader and check light sweeps for jumps.

Example:
    uv run --with numpy --with pillow python preview.py --bake baked/ --step 5

Reconstruction (same logic as sdf360_sample.hlsl): L -> (theta, phi) in the head frame -> 4-cell lookup with
wrap (full circle) / mirror (uv.x -> 1-uv.x for theta < 0) / phi clamp -> bilinear atlas samples at
atlasUV(saturate(faceUV)) -> s = (v - 0.5) * 2 -> s = lerp(lerp(s00, s10, tTheta), lerp(s01, s11, tTheta), tPhi).
Lit state for the jump metric: s > 0. Images show smoothstep(-w, w, s), w = --soft texels / R.

Sweeps (step --step degrees):
    h_phi0     theta -180 .. 180 at phi 0
    v_theta0   phi phi_min .. phi_max at theta 0
    v_theta90  phi phi_min .. phi_max at theta 90
Metric per consecutive step: flip = fraction of face texels (coverage.png, else all texels) whose lit state changes.
Flags: flip > --jump-threshold, or a spike (flip > --spike-ratio x mean of the neighbouring steps and > 0.5%).
Monotone check (lit area must only shrink while the light moves from the front toward the back / poles):
    h_phi0 from theta 0 toward +180 and toward -180, v_theta0 from phi 0 toward phi_max and toward phi_min.
    'regain' = fraction of texels turning dark -> lit in such a step; flagged above --monotone-tol.
Outputs in --out (default <bake>/preview): sheet_<sweep>.png, <sweep>.gif (and .png APNG with --apng),
preview_report.json.
"""

from __future__ import annotations

import argparse
import math
import os
import sys

import numpy as np

import sdf360_common as C


# ----------------------------------------------------------------------------------------------
# Reconstruction
# ----------------------------------------------------------------------------------------------

class Reconstructor:
    """Shader-equivalent evaluation of a bake (atlas or slices)."""

    def __init__(self, bake_dir: str, source: str = "atlas"):
        self.meta = C.read_json(os.path.join(bake_dir, C.SIDECAR_NAME))
        self.grid = C.Grid.from_json(self.meta["grid"])
        self.W = int(self.meta["cell"]["width"])
        self.H = int(self.meta["cell"]["height"])
        self.P = int(self.meta["padding"])
        self.R = float(self.meta["encoding"]["spread_texels"])
        self.source = source
        if source == "atlas":
            self.atlas = C.load_gray(os.path.join(bake_dir, self.meta["atlas"]["file"]))
            self.AW, self.AH = self.atlas.shape[1], self.atlas.shape[0]
            exp = C.atlas_size(self.grid, self.W, self.H, self.P)
            if (self.AW, self.AH) != exp:
                raise SystemExit(f"atlas is {self.AW}x{self.AH}, sidecar says {exp[0]}x{exp[1]}")
        else:
            n = self.grid.theta_count * self.grid.phi_count
            self.slices = [C.load_gray(os.path.join(bake_dir, C.SLICE_PATTERN.format(k=k))) for k in range(n)]
        # frame used to turn sweep angles into light vectors and back (UE: X forward, Z up)
        self.F = np.array([1.0, 0.0, 0.0])
        self.U = np.array([0.0, 0.0, 1.0])

    def sample_cell(self, i: int, j: int, u, v):
        if self.source == "atlas":
            ax, ay = C.atlas_uv(u, v, i, j, self.W, self.H, self.P, self.AW, self.AH)
            return C.sample_bilinear(self.atlas, ax, ay)
        return C.sample_bilinear(self.slices[self.grid.slice_index(i, j)], u, v)

    def evaluate_angles(self, theta: float, phi: float, face_u, face_v):
        i0, i1, j0, j1, tt, tp, flip = self.grid.lookup(theta, phi)
        u = np.clip(face_u, 0.0, 1.0)
        v = np.clip(face_v, 0.0, 1.0)
        if flip:
            u = 1.0 - u
        s00 = (self.sample_cell(i0, j0, u, v) - 0.5) * 2.0
        s10 = (self.sample_cell(i1, j0, u, v) - 0.5) * 2.0
        s01 = (self.sample_cell(i0, j1, u, v) - 0.5) * 2.0
        s11 = (self.sample_cell(i1, j1, u, v) - 0.5) * 2.0
        a = s00 + (s10 - s00) * tt          # HLSL lerp(x, y, s) = x + s * (y - x)
        b = s01 + (s11 - s01) * tt
        return a + (b - a) * tp

    def evaluate_light(self, L, face_u, face_v):
        theta, phi = C.light_angles(L, self.F, self.U)
        return self.evaluate_angles(theta, phi, face_u, face_v)

    def evaluate_sweep_angle(self, theta: float, phi: float, face_u, face_v):
        """Sweep angles -> UE light vector -> angles again (exercises the shader's vector path)."""
        d = C.direction_from_angles(theta, phi)                  # (F, R, U) components
        L = d[0] * self.F + d[1] * np.cross(self.U, self.F) + d[2] * self.U
        return self.evaluate_light(L, face_u, face_v)


def face_uv_grid(W: int, H: int, scale: int = 1):
    """Texel-centre UVs of a (H*scale, W*scale) evaluation image."""
    w, h = W * scale, H * scale
    u = (np.arange(w) + 0.5) / w
    v = (np.arange(h) + 0.5) / h
    return np.meshgrid(u, v)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# ----------------------------------------------------------------------------------------------
# Sweeps and metrics
# ----------------------------------------------------------------------------------------------

def _seq(a: float, b: float, step: float, anchor: float = 0.0):
    """Values from a to b (inclusive) on the anchor + k*step lattice, plus both end points."""
    vals = {round(a, 6), round(b, 6)}
    k0 = math.ceil((a - anchor) / step - 1e-9)
    k1 = math.floor((b - anchor) / step + 1e-9)
    for k in range(k0, k1 + 1):
        vals.add(round(anchor + k * step, 6))
    return sorted(vals)


def build_sweeps(grid: C.Grid, step: float):
    sweeps = []
    th = _seq(-180.0, 180.0, step)
    sweeps.append({"name": "h_phi0", "axis": "theta", "angles": [(t, 0.0) for t in th],
                   "monotone_from": th.index(0.0)})
    ph = _seq(grid.phi_min, grid.phi_max, step)
    mono = ph.index(0.0) if 0.0 in ph else None
    sweeps.append({"name": "v_theta0", "axis": "phi", "angles": [(0.0, p) for p in ph], "monotone_from": mono})
    sweeps.append({"name": "v_theta90", "axis": "phi", "angles": [(90.0, p) for p in ph], "monotone_from": None})
    return sweeps


def sweep_metrics(lit: np.ndarray, cov: np.ndarray, angles, axis: str, monotone_from, jump_threshold: float,
                  spike_ratio: float, monotone_tol: float):
    """lit: (N, H, W) bool along the sweep."""
    n_cov = max(int(cov.sum()), 1)
    lit_c = lit[:, cov]
    lit_frac = lit_c.mean(axis=1)
    flips = (lit_c[1:] != lit_c[:-1]).sum(axis=1) / n_cov
    gained = (lit_c[1:] & ~lit_c[:-1]).sum(axis=1) / n_cov
    lost = (~lit_c[1:] & lit_c[:-1]).sum(axis=1) / n_cov
    steps = []
    flags = []
    key = 0 if axis == "theta" else 1
    for k in range(len(flips)):
        nb = []
        if k > 0:
            nb.append(flips[k - 1])
        if k + 1 < len(flips):
            nb.append(flips[k + 1])
        ref = float(np.mean(nb)) if nb else 0.0
        spike = flips[k] / ref if ref > 1e-12 else (math.inf if flips[k] > 0 else 1.0)
        st = {"from": angles[k][key], "to": angles[k + 1][key], "flip": float(flips[k]),
              "gained": float(gained[k]), "lost": float(lost[k]), "spike_ratio": float(min(spike, 1e9))}
        steps.append(st)
        if flips[k] > jump_threshold:
            flags.append(f"jump {st['from']:g}->{st['to']:g}: {100 * flips[k]:.2f}% of face texels flip "
                         f"(> {100 * jump_threshold:.1f}%)")
        elif spike > spike_ratio and flips[k] > 0.005:
            flags.append(f"spike {st['from']:g}->{st['to']:g}: {100 * flips[k]:.2f}% flips, {spike:.1f}x the "
                         "neighbouring steps")
    mono = None
    if monotone_from is not None:
        regain = []
        multi = np.zeros(lit_c.shape[1], dtype=bool)
        for direction in (+1, -1):
            idx = list(range(monotone_from, len(angles))) if direction > 0 else list(range(monotone_from, -1, -1))
            seg = lit_c[idx]
            if len(idx) < 2:
                continue
            g = (seg[1:] & ~seg[:-1]).sum(axis=1) / n_cov
            for m, val in enumerate(g):
                a0, a1 = angles[idx[m]][key], angles[idx[m + 1]][key]
                regain.append({"from": a0, "to": a1, "regain": float(val)})
                if val > monotone_tol:
                    flags.append(f"non-monotone {a0:g}->{a1:g}: {100 * val:.3f}% of face texels turn lit while "
                                 "the light moves away from the front")
            changes = (seg[1:] != seg[:-1]).sum(axis=0)
            multi |= changes > 1
        mono = {"max_regain": max((r["regain"] for r in regain), default=0.0),
                "multi_flip_texels": float(multi.mean()), "steps": regain}
    return {
        "angles": [list(a) for a in angles],
        "lit_fraction": [float(x) for x in lit_frac],
        "max_flip": float(flips.max()) if len(flips) else 0.0,
        "mean_flip": float(flips.mean()) if len(flips) else 0.0,
        "max_flip_at": (steps[int(np.argmax(flips))]["from"], steps[int(np.argmax(flips))]["to"]) if len(flips) else None,
        "max_spike_ratio": float(max((s["spike_ratio"] for s in steps if s["flip"] > 0.005), default=0.0)),
        "monotone": mono,
        "steps": steps,
        "flags": flags,
    }


def run_sweeps(rec: Reconstructor, cov: np.ndarray, step: float = 5.0, scale: int = 1,
               jump_threshold: float = 0.10, spike_ratio: float = 3.0, monotone_tol: float = 0.002):
    """Evaluate all sweeps. Returns (results by name, s fields by name)."""
    uu, vv = face_uv_grid(rec.W, rec.H, scale)
    if cov.shape != uu.shape:
        cov = np.repeat(np.repeat(cov, scale, axis=0), scale, axis=1)
    results = {}
    fields = {}
    for sw in build_sweeps(rec.grid, step):
        s = np.stack([rec.evaluate_sweep_angle(t, p, uu, vv) for (t, p) in sw["angles"]])
        fields[sw["name"]] = s
        results[sw["name"]] = sweep_metrics(s > 0.0, cov, sw["angles"], sw["axis"], sw["monotone_from"],
                                            jump_threshold, spike_ratio, monotone_tol)
    return results, fields, cov


# ----------------------------------------------------------------------------------------------
# Images
# ----------------------------------------------------------------------------------------------

def frame_image(s: np.ndarray, cov: np.ndarray, w_s: float) -> np.ndarray:
    lit = smoothstep(-w_s, w_s, s)
    img = 45.0 + 200.0 * lit
    img = np.where(cov, img, img * 0.35)
    return np.round(img).astype(np.uint8)


def write_images(name: str, s: np.ndarray, cov: np.ndarray, angles, axis: str, w_s: float, out_dir: str,
                 thumb: int = 128, gif_ms: int = 60, apng: bool = False):
    from PIL import Image, ImageDraw
    key = 0 if axis == "theta" else 1
    other = 1 - key
    frames = []
    for k in range(len(s)):
        im = Image.fromarray(frame_image(s[k], cov, w_s)).convert("RGB")
        d = ImageDraw.Draw(im)
        label = f"theta {angles[k][0]:g}  phi {angles[k][1]:g}"
        d.rectangle([0, 0, 8 + 7 * len(label), 14], fill=(0, 0, 0))
        d.text((3, 1), label, fill=(255, 220, 90))
        frames.append(im)
    frames[0].save(os.path.join(out_dir, f"{name}.gif"), save_all=True, append_images=frames[1:],
                   duration=gif_ms, loop=0)
    if apng:
        frames[0].save(os.path.join(out_dir, f"{name}_apng.png"), save_all=True, append_images=frames[1:],
                       duration=gif_ms, loop=0)
    cols = min(len(frames), 12)
    rows = int(math.ceil(len(frames) / cols))
    h0, w0 = s.shape[1], s.shape[2]
    th = int(round(thumb * h0 / w0))
    sheet = Image.new("RGB", (cols * thumb, rows * (th + 14)), (20, 20, 20))
    d = ImageDraw.Draw(sheet)
    for k, f in enumerate(frames):
        im = Image.fromarray(frame_image(s[k], cov, w_s)).resize((thumb, th), Image.Resampling.BILINEAR)
        x = (k % cols) * thumb
        y = (k // cols) * (th + 14)
        sheet.paste(im, (x, y + 14))
        d.text((x + 3, y + 1), f"{'t' if key == 0 else 'p'}{angles[k][key]:g} ({'p' if key == 0 else 't'}{angles[k][other]:g})",
               fill=(230, 230, 230))
    sheet.save(os.path.join(out_dir, f"sheet_{name}.png"))


def load_coverage(path_or_none, bake_dir: str, W: int, H: int) -> np.ndarray:
    p = path_or_none or os.path.join(bake_dir, C.COVERAGE_NAME)
    if p and os.path.isfile(p):
        cov = C.load_gray(p) >= 0.5
        if cov.shape != (H, W):
            from PIL import Image
            cov = np.asarray(Image.fromarray(cov.astype(np.uint8) * 255).resize((W, H), Image.Resampling.NEAREST)) >= 128
        return cov
    return np.ones((H, W), dtype=bool)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bake", required=True, help="bake folder (sdf360.json + atlas.png / slices)")
    ap.add_argument("--out", default=None, help="output folder (default <bake>/preview)")
    ap.add_argument("--coverage", default=None, help="face texel mask (default <bake>/coverage.png, else all texels)")
    ap.add_argument("--source", choices=("atlas", "slices"), default="atlas")
    ap.add_argument("--step", type=float, default=5.0, help="sweep step in degrees (default 5)")
    ap.add_argument("--scale", type=int, default=1, help="evaluate at scale x cell resolution (sub-texel check)")
    ap.add_argument("--soft", type=float, default=1.0, help="image smoothstep half width in cell texels")
    ap.add_argument("--jump-threshold", type=float, default=0.10, help="max fraction of face texels flipping per step")
    ap.add_argument("--spike-ratio", type=float, default=3.0)
    ap.add_argument("--monotone-tol", type=float, default=0.002)
    ap.add_argument("--thumb", type=int, default=128)
    ap.add_argument("--gif-ms", type=int, default=60)
    ap.add_argument("--apng", action="store_true", help="also write animated PNGs")
    ap.add_argument("--no-images", action="store_true")
    args = ap.parse_args(argv)

    rec = Reconstructor(args.bake, args.source)
    out = args.out or os.path.join(args.bake, "preview")
    os.makedirs(out, exist_ok=True)
    cov = load_coverage(args.coverage, args.bake, rec.W, rec.H)
    results, fields, cov_s = run_sweeps(rec, cov, args.step, args.scale, args.jump_threshold, args.spike_ratio,
                                        args.monotone_tol)
    w_s = args.soft / rec.R
    if not args.no_images:
        for sw in build_sweeps(rec.grid, args.step):
            write_images(sw["name"], fields[sw["name"]], cov_s, sw["angles"], sw["axis"], w_s, out,
                         args.thumb, args.gif_ms, args.apng)
    report = {"bake": os.path.abspath(args.bake).replace("\\", "/"), "source": args.source, "step": args.step,
              "scale": args.scale, "face_texels": int(cov_s.sum()), "thresholds": {
                  "jump": args.jump_threshold, "spike_ratio": args.spike_ratio, "monotone_tol": args.monotone_tol},
              "sweeps": results}
    C.write_json(os.path.join(out, "preview_report.json"), report)
    total_flags = 0
    print(f"preview of {args.bake} ({args.source}, step {args.step:g} deg, {int(cov_s.sum())} face texels)")
    for name, r in results.items():
        mono = r["monotone"]
        mono_s = (f", max regain {100 * mono['max_regain']:.3f}%, multi-flip texels {100 * mono['multi_flip_texels']:.3f}%"
                  if mono else "")
        print(f"  {name:10s} max flip {100 * r['max_flip']:.2f}% at {r['max_flip_at']}, mean {100 * r['mean_flip']:.2f}%, "
              f"max spike {r['max_spike_ratio']:.2f}x{mono_s}, {len(r['flags'])} flag(s)")
        for f in r["flags"]:
            print(f"      ! {f}")
        total_flags += len(r["flags"])
    print(f"images + preview_report.json -> {out}")
    return 1 if total_flags else 0


if __name__ == "__main__":
    sys.exit(main())
