"""Bake a full-angle face SDF (SDF360) atlas + Texture2DArray slices from a grid of lit masks.

Example:
    uv run --with numpy --with pillow python bake_sdf360.py --masks masks/ --out baked/ ^
        --theta-count 9 --phi-count 9 --phi-min -80 --phi-max 80 --cell-size 256 --padding 4

Inputs: one mask per grid cell named mask_t{theta}_p{phi}.png (see README / sdf360_common.mask_filename),
white = lit, binarised at --threshold (0.5). Masks may be larger than --cell-size (supersampling): the SDF is
computed at mask resolution and the distance is box-filtered down and rescaled to cell texels.
If the grid arguments are omitted they are read from masks.json written by gen_masks.py.

Outputs (in --out):
    atlas.png            Ntheta x Nphi cells, each WxH plus P texels of edge-replicated padding per side
    slice_000.png ...    one WxH slice per cell, k = j*Ntheta + i (Texture2DArray order)
    sdf360.json          grid, layout, encoding and frame conventions (the material contract)
    coverage.png         copied/resampled from the mask folder when present (used by preview.py)
"""

from __future__ import annotations

import argparse
import math
import os
import sys
import time

import numpy as np

import sdf360_common as C
import sdf_edt


def resample_distance(d: np.ndarray, out_hw, warn) -> np.ndarray:
    """Signed distance in mask texels (H_m, W_m) -> signed distance in cell texels (H, W)."""
    hm, wm = d.shape
    h, w = out_hw
    if (hm, wm) == (h, w):
        return d.astype(np.float64)
    fx = wm / w
    fy = hm / h
    if abs(fx - fy) > 1e-6:
        warn(f"mask aspect differs from cell aspect (x factor {fx:.4f}, y factor {fy:.4f}); "
             "distances are rescaled with the geometric mean")
    scale = 1.0 / math.sqrt(fx * fy)
    if abs(fx - round(fx)) < 1e-9 and abs(fy - round(fy)) < 1e-9 and fx >= 1 and fy >= 1:
        f_x, f_y = int(round(fx)), int(round(fy))
        out = d.reshape(h, f_y, w, f_x).mean(axis=(1, 3))
    else:
        from PIL import Image
        img = Image.fromarray(d.astype(np.float32))  # mode F
        flt = Image.Resampling.BOX if (fx >= 1 and fy >= 1) else Image.Resampling.BILINEAR
        out = np.asarray(img.resize((w, h), flt), dtype=np.float64)
    return out * scale


def quad_pairs(grid: C.Grid):
    """Every pair of cells that can meet in one bilinear 4-cell lookup (including diagonals)."""
    nt, nph = grid.theta_count, grid.phi_count
    if grid.mirror:
        tpairs = [(i, i + 1) for i in range(nt - 1)]
    else:
        tpairs = [(i, (i + 1) % nt) for i in range(nt)]
    pairs = set()
    for (i0, i1) in tpairs:
        if nph == 1:
            pairs.add(((i0, 0), (i1, 0)))
            continue
        for j0 in range(nph - 1):
            j1 = j0 + 1
            a, b, c, d = (i0, j0), (i1, j0), (i0, j1), (i1, j1)
            for p in ((a, b), (c, d), (a, c), (b, d), (a, d), (b, c)):
                pairs.add(tuple(sorted(p)))
    return sorted(pairs)


def required_spread(D: np.ndarray, grid: C.Grid, region: np.ndarray):
    """Smallest R for which no texel saturates in two interpolated cells of opposite sign.

    If a texel is deeper than R inside the lit region of one cell and deeper than R inside the shadow of
    another cell of the same bilinear quad, both values clamp to +-1 and the texel (together with every
    other such texel) flips at the same interpolation weight: a visible jump. D is (Nphi, Ntheta, H, W),
    +-inf for uniform cells. Only texels in `region` (face coverage + margin) are considered.
    Returns (R_min, driving pair).
    """
    req = 0.0
    pair = None
    for (i0, j0), (i1, j1) in quad_pairs(grid):
        a = D[j0, i0][region]
        b = D[j1, i1][region]
        opp = (a > 0) != (b > 0)
        if not opp.any():
            continue
        m = np.minimum(np.abs(a[opp]), np.abs(b[opp]))
        m = m[np.isfinite(m)]
        if m.size and float(m.max()) > req:
            req = float(m.max())
            pair = [[i0, j0], [i1, j1]]
    return req, pair


def synthesize_uniform(D: np.ndarray, grid: C.Grid, uniform, region: np.ndarray, margin: float = 1.0):
    """Give every uniform (all-lit / all-dark) cell a signed field instead of a constant.

    A constant +-R cell makes the boundary of a neighbouring cell stand still and then rush in right next to
    the uniform cell's angle (all texels cross at almost the same interpolation weight). Instead every non-uniform
    edge neighbour's field (diagonal ones when there is none) is shifted so that it keeps the uniform sign with at
    least `margin` texels to spare, and the uniform cell takes, per texel, the shifted value closest to zero
    (min for an all-lit cell, max for an all-dark one). Interpolating towards a neighbour then moves its boundary
    at (nearly) constant speed until it leaves the face exactly at the uniform cell's angle.
    The shift is measured inside `region` (face coverage + margin), so the cell is guaranteed uniform there;
    outside the face region the synthesised field may change sign (those texels are never sampled).
    Mirror mode: the theta=0 / theta=180 cells also use the mirrored image of their only theta neighbour.
    """
    nt, nph = grid.theta_count, grid.phi_count
    is_uni = {(i, j) for (i, j, _) in uniform}
    out = {}
    for (i, j, state) in uniform:
        edge, diag = [], []
        for di, dj, lst in ((-1, 0, edge), (1, 0, edge), (0, -1, edge), (0, 1, edge),
                            (-1, -1, diag), (1, -1, diag), (-1, 1, diag), (1, 1, diag)):
            jj = j + dj
            if not (0 <= jj < nph):
                continue
            ii = i + di
            flip = False
            if grid.mirror:
                if ii < 0 or ii >= nt:          # across the 0 / 180 seam: mirrored image of the inner neighbour
                    ii = i - di
                    flip = True
                    if not (0 <= ii < nt) or ii == i:
                        continue
            else:
                ii %= nt
            if (ii, jj) in is_uni or (ii, jj) == (i, j):
                continue
            f = D[jj, ii]
            lst.append(f[:, ::-1] if flip else f)
        src = edge or diag
        if not src:
            continue
        acc = []
        for f in src:
            fr = f[region]
            if state == "lit":
                dark = fr[fr < 0]
                shift = (float(-dark.min()) if dark.size else 0.0) + margin
                acc.append(f + shift)
            else:
                lit = fr[fr > 0]
                shift = (float(lit.max()) if lit.size else 0.0) + margin
                acc.append(f - shift)
        # closest-to-zero combination (min for lit, max for dark): towards every neighbour the boundary then
        # moves (nearly) linearly instead of stalling behind the other neighbours' deeper values
        stack = np.stack(acc)
        out[(i, j)] = (stack.min(axis=0) if state == "lit" else stack.max(axis=0), len(src))
    for (i, j), (field, _) in out.items():
        D[j, i] = field
    return {k: v[1] for k, v in out.items()}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--masks", required=True, help="folder with mask_t{theta}_p{phi}.png files")
    ap.add_argument("--out", required=True, help="output folder")
    C.add_grid_args(ap, required=False)
    ap.add_argument("--cell-size", default=None, help="cell size W or WxH in texels (default: mask size)")
    ap.add_argument("--padding", type=int, default=4, help="edge-replicated padding texels around every atlas cell")
    ap.add_argument("--spread", default="auto",
                    help="R in cell texels for v = saturate(0.5 + d/(2R)); 'auto' = smallest jump-free R (default)")
    ap.add_argument("--bits", type=int, choices=(8, 16), default=16, help="PNG bit depth (default 16)")
    ap.add_argument("--threshold", type=float, default=0.5, help="mask binarisation level (default 0.5)")
    ap.add_argument("--edt", choices=sdf_edt.SCHEDULES, default="sweep", help="8SSEDT scan schedule (default sweep)")
    ap.add_argument("--backend", choices=("auto", "numpy", "numba"), default="auto")
    ap.add_argument("--no-symmetrize-seams", action="store_true",
                    help="mirror mode: do not force the theta=0 / theta=180 cells to be left-right symmetric")
    ap.add_argument("--uniform-fill", choices=("shift", "constant"), default="shift",
                    help="all-lit / all-dark cells: 'shift' = shifted neighbour fields (boundary keeps moving at "
                         "constant speed, default) or 'constant' = +-R everywhere")
    ap.add_argument("--coverage", default=None,
                    help="face texel mask for the jump-free spread and the warnings (default <masks>/coverage.png; "
                         "without one the whole cell is used)")
    ap.add_argument("--no-slices", action="store_true", help="skip writing the Texture2DArray slices")
    ap.add_argument("--no-atlas", action="store_true", help="skip writing the atlas")
    args = ap.parse_args(argv)

    warnings = []

    def warn(msg):
        warnings.append(msg)
        print(f"WARNING: {msg}")

    t_start = time.perf_counter()
    grid = C.grid_from_args(args, os.path.join(args.masks, C.MASKS_SIDECAR_NAME))
    mapping, missing, unused = C.find_mask_files(args.masks, grid)
    if missing:
        names = ", ".join(C.mask_filename(t, p) for (_, _, t, p) in missing[:8])
        raise SystemExit(f"{len(missing)} grid cells have no mask (expected e.g. {names}"
                         f"{' ...' if len(missing) > 8 else ''}) in {args.masks}")
    if unused:
        warn(f"{len(unused)} mask files do not belong to the grid and are ignored (e.g. {unused[:3]})")

    cells = grid.cells()
    first = C.load_gray(mapping[(0, 0)])
    hm, wm = first.shape
    if args.cell_size:
        W, H = C.parse_size(args.cell_size)
    else:
        W, H = wm, hm
    P = int(args.padding)
    if P < 1:
        warn("padding < 1: bilinear sampling at the cell border will bleed into the neighbouring cell")
    backend = sdf_edt.resolve_backend(args.backend)
    print(f"grid: {grid.theta_count} x {grid.phi_count} ({'mirror [0,180]' if grid.mirror else 'full circle [-180,180)'}), "
          f"phi [{grid.phi_min:g}, {grid.phi_max:g}], masks {wm}x{hm} -> cells {W}x{H}, padding {P}, "
          f"8SSEDT {args.edt}/{backend}")
    if args.edt == "raster" and backend == "numpy" and wm * hm > 256 * 256:
        warn("raster schedule without numba loops per pixel and is very slow at this size; use --edt sweep or numba")

    # --- face coverage: the region where jumps matter (dilated by the padding for the bilinear footprint) --
    cov_src = args.coverage or os.path.join(args.masks, C.COVERAGE_NAME)
    has_cov = os.path.isfile(cov_src)
    if has_cov:
        cov = C.load_gray(cov_src)
        if cov.shape != (H, W):
            from PIL import Image
            cov = np.asarray(Image.fromarray((cov * 255).astype(np.uint8)).resize((W, H), Image.Resampling.BOX),
                             dtype=np.float64) / 255.0
        cov = cov >= 0.5
        if not cov.any():
            warn(f"{cov_src} is empty; using the whole cell")
            cov = np.ones((H, W), dtype=bool)
        region = sdf_edt.edt(cov) <= max(2, P)
    else:
        cov = np.ones((H, W), dtype=bool)
        region = cov

    # --- SDF per mask (chunked to bound memory: 2 seed images x 16 bytes per texel per mask) -------------
    D = np.zeros((grid.phi_count, grid.theta_count, H, W), dtype=np.float64)
    lit_frac = np.zeros((grid.phi_count, grid.theta_count))
    uniform = []
    chunk = max(1, int(512 * 1024 * 1024 // (2 * 16 * wm * hm)))
    order = [(i, j) for (i, j, _, _) in cells]
    for c0 in range(0, len(order), chunk):
        part = order[c0:c0 + chunk]
        lit = np.empty((len(part), hm, wm), dtype=bool)
        for n, (i, j) in enumerate(part):
            g = first if (i, j) == (0, 0) else C.load_gray(mapping[(i, j)])
            if g.shape != (hm, wm):
                raise SystemExit(f"{mapping[(i, j)]} is {g.shape[1]}x{g.shape[0]}, expected {wm}x{hm}")
            lit[n] = g >= args.threshold
        d = sdf_edt.signed_distance(lit, args.edt, backend)
        for n, (i, j) in enumerate(part):
            lit_frac[j, i] = lit[n].mean()
            dn = d[n]
            if np.isinf(dn).all():
                uniform.append((i, j, "lit" if dn.flat[0] > 0 else "dark"))
                D[j, i] = dn.flat[0]
            else:
                D[j, i] = resample_distance(dn, (H, W), warn)
        print(f"  sdf {min(c0 + chunk, len(order))}/{len(order)}")

    cap = 4.0 * max(W, H)

    def capped(a):
        return np.where(np.isinf(a), np.sign(a) * cap, a)

    # --- uniform cells -----------------------------------------------------------------------------------
    synthesized = {}
    if uniform and args.uniform_fill == "shift":
        synthesized = synthesize_uniform(D, grid, uniform, region)
        if synthesized:
            print(f"uniform cells synthesised from shifted neighbour fields: "
                  f"{', '.join(f'({i},{j})' for (i, j) in synthesized)}")

    # --- mirror seams: theta=0 and theta=180 must be symmetric under uv.x -> 1-uv.x ----------------------
    if grid.mirror:
        for i in (0, grid.theta_count - 1):
            for j in range(grid.phi_count):
                a = D[j, i]
                rs = region & region[:, ::-1]
                asym = float(((a > 0) != (a[:, ::-1] > 0))[rs].mean()) if rs.any() else 0.0
                if asym > 0.002:
                    warn(f"mirror seam cell theta={grid.theta_values()[i]:g} phi={grid.phi_values()[j]:g} is not "
                         f"left-right symmetric ({100 * asym:.2f}% texels differ)"
                         + ("" if args.no_symmetrize_seams else "; symmetrised"))
                if not args.no_symmetrize_seams and np.isfinite(a).all():
                    D[j, i] = 0.5 * (a + a[:, ::-1])

    # --- poles: every theta of a phi = +-90 row is the same direction --------------------------------------
    for j in grid.pole_rows():
        row = [capped(D[j, i]) for i in range(grid.theta_count)]
        if grid.mirror:
            row = row + [r[:, ::-1] for r in row]
        signs = np.stack([r > 0 for r in row])
        rp = region & region[:, ::-1] if grid.mirror else region
        differ = float((signs != signs[0]).any(axis=0)[rp].mean())
        if differ > 0.002:
            warn(f"pole row phi={grid.phi_values()[j]:g}: masks differ between theta samples "
                 f"({100 * differ:.2f}% texels); they were averaged into one field")
        avg = np.mean(np.stack(row), axis=0)
        for i in range(grid.theta_count):
            D[j, i] = avg
        uniform = [u for u in uniform if u[1] != j]

    # --- spread -------------------------------------------------------------------------------------------
    req, req_pair = required_spread(D, grid, region)
    if str(args.spread).lower() == "auto":
        R = float(max(2, math.ceil(req) + 1))
    else:
        R = float(args.spread)
        if R < req:
            warn(f"--spread {R:g} is below the jump-free minimum {req:.1f} texels: texels deeper than R on "
                 "opposite sides in neighbouring cells clamp together and flip at the same light angle")
    q_step = 2.0 * R / (65535.0 if args.bits == 16 else 255.0)
    if q_step > 0.25:
        warn(f"quantisation step is {q_step:.2f} texels (bits {args.bits}, R {R:g}); use --bits 16 or a smaller R")

    # --- encode / write -------------------------------------------------------------------------------------
    os.makedirs(args.out, exist_ok=True)
    V = C.encode(D, R)
    Q = C.quantize(V, args.bits)
    aw, ah = C.atlas_size(grid, W, H, P)
    if not args.no_atlas:
        atlas = C.pack_atlas(Q, P)
        C.save_gray(os.path.join(args.out, C.ATLAS_NAME), atlas)
    if not args.no_slices:
        for (i, j, _, _) in cells:
            C.save_gray(os.path.join(args.out, C.SLICE_PATTERN.format(k=grid.slice_index(i, j))), Q[j, i])
    if has_cov:
        C.save_mask(os.path.join(args.out, C.COVERAGE_NAME), cov)

    frame = {
        "space": "Unreal, left-handed, Z up",
        "forward": "face looks along Forward",
        "up": "head up",
        "right": "Right = cross(Up, Forward) (UE: +Y when Forward=+X, Up=+Z) = the character's right",
        "light": "L points from the surface toward the light (normalised)",
        "theta": "theta = degrees(atan2(dot(L,Right), dot(L,Forward))); 0 = front, +90 = character's right",
        "phi": "phi = degrees(asin(dot(L,Up))); +90 = from above",
        "mirror": "mirror mode: theta < 0 uses |theta| and face uv.x -> 1 - uv.x",
    }
    masks_meta = {}
    mj = os.path.join(args.masks, C.MASKS_SIDECAR_NAME)
    if os.path.isfile(mj):
        try:
            masks_meta = C.read_json(mj)
        except Exception:
            masks_meta = {}
    meta = {
        "tool": C.TOOL_NAME,
        "version": C.TOOL_VERSION,
        "grid": grid.to_json(),
        "cell": {"width": W, "height": H},
        "padding": P,
        "atlas": {
            "file": None if args.no_atlas else C.ATLAS_NAME,
            "width": aw, "height": ah,
            "columns": grid.theta_count, "rows": grid.phi_count,
            "column": "i = theta index", "row": "j = phi index, row 0 = phi_min at the top of the image",
            "uv_formula": C.ATLAS_UV_FORMULA,
            "texel_convention": C.TEXEL_CONVENTION,
        },
        "slices": {
            "pattern": None if args.no_slices else "slice_{k:03d}.png",
            "count": grid.theta_count * grid.phi_count,
            "order": "k = j * theta_count + i (theta fastest)",
            "addressing": "clamp, sample at face uv directly (no padding)",
        },
        "encoding": {
            "spread_texels": R,
            "required_spread_texels": req,
            "required_spread_pair": req_pair,
            "required_spread_region": "coverage dilated by max(2, padding) texels" if has_cov else "whole cell",
            "bits": args.bits,
            "formula": C.ENCODING_FORMULA,
            "decode": C.DECODING_FORMULA,
            "sign": "positive = lit, v > 0.5 = lit",
            "distance_convention": C.DISTANCE_CONVENTION,
            "srgb": False,
        },
        "frame": frame,
        "edt": {"algorithm": "8SSEDT", "schedule": args.edt, "backend": backend},
        "source": {
            "mask_dir": os.path.abspath(args.masks).replace("\\", "/"),
            "mask_size": [wm, hm],
            "supersample": [wm / W, hm / H],
            "threshold": args.threshold,
            "generator": masks_meta.get("generator"),
        },
        "stats": {
            "lit_fraction": lit_frac.round(5).tolist(),
            "uniform_cells": [{"i": i, "j": j, "state": s, "fill": ("shifted neighbours" if (i, j) in synthesized
                                                                     else "constant")} for (i, j, s) in uniform],
            "mirror_seams_symmetrised": bool(grid.mirror and not args.no_symmetrize_seams),
            "pole_rows_unified": grid.pole_rows(),
            "coverage": C.COVERAGE_NAME if has_cov else None,
            "warnings": warnings,
            "seconds": round(time.perf_counter() - t_start, 3),
        },
    }
    C.write_json(os.path.join(args.out, C.SIDECAR_NAME), meta)
    print(f"spread R = {R:g} texels (jump-free minimum {req:.2f}, set by cells {req_pair}), {args.bits}-bit, "
          f"quantisation {q_step:.4f} texels")
    if uniform:
        print(f"uniform cells: {', '.join(f'({i},{j})={s}' for (i, j, s) in uniform)}")
    print(f"atlas {aw}x{ah} -> {'(skipped)' if args.no_atlas else os.path.join(args.out, C.ATLAS_NAME)}; "
          f"{0 if args.no_slices else len(cells)} slices; "
          f"{len(warnings)} warning(s); {time.perf_counter() - t_start:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
