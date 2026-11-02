"""Accuracy and consistency tests for the 8SSEDT implementation and the atlas mapping.

Run:
    uv run --with numpy --with pillow --with scipy python test_edt.py
    uv run --with numpy --with pillow --with scipy --with numba python test_edt.py   (also checks numba == numpy)

8SSEDT is compared against scipy.ndimage.distance_transform_edt (exact Euclidean distance between pixel
centres). 8SSEDT can only over-estimate (it always returns the distance to a real seed), so the error is
reported as 8SSEDT - exact >= 0, for the distance to the nearest opposite-class pixel of every pixel.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time

import numpy as np

import sdf_edt
import sdf360_common as C


def make_shapes(n: int, rng: np.random.Generator):
    from scipy import ndimage
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float64)
    cx = cy = (n - 1) / 2.0
    shapes = {}
    shapes["disk"] = (xx - cx) ** 2 + (yy - cy) ** 2 < (0.3 * n) ** 2
    shapes["square"] = (np.abs(xx - cx) < 0.25 * n) & (np.abs(yy - cy) < 0.25 * n)
    a = math.radians(30)
    rx = (xx - cx) * math.cos(a) + (yy - cy) * math.sin(a)
    ry = -(xx - cx) * math.sin(a) + (yy - cy) * math.cos(a)
    shapes["rot_rect"] = (np.abs(rx) < 0.35 * n) & (np.abs(ry) < 0.12 * n)
    r = np.hypot(xx - cx, yy - cy)
    shapes["ring"] = (r > 0.2 * n) & (r < 0.3 * n)
    shapes["half_plane_17deg"] = (xx - cx) * math.cos(math.radians(17)) + (yy - cy) * math.sin(math.radians(17)) > 0
    shapes["thin_diag_line"] = np.abs(xx - yy) < 0.5
    sp = np.zeros((n, n), bool)
    sp[n // 3, n // 2] = True
    shapes["single_pixel"] = sp
    shapes["checker_8"] = ((xx // 8 + yy // 8) % 2) == 0
    shapes["two_disks"] = (((xx - 0.3 * n) ** 2 + (yy - 0.4 * n) ** 2) < (0.12 * n) ** 2) | \
                          (((xx - 0.7 * n) ** 2 + (yy - 0.6 * n) ** 2) < (0.18 * n) ** 2)
    shapes["face_like"] = ((((xx - cx) / (0.42 * n)) ** 2 + ((yy - cy) / (0.48 * n)) ** 2) < 1) & \
                          ~((((xx - 0.38 * n) / (0.06 * n)) ** 2 + ((yy - 0.45 * n) / (0.03 * n)) ** 2) < 1)
    for k in range(4):
        noise = rng.standard_normal((n, n))
        sigma = [2.0, 4.0, 8.0, 16.0][k]
        sm = ndimage.gaussian_filter(noise, sigma, mode="reflect")
        shapes[f"random_blobs_s{int(sigma)}"] = sm > 0
    shapes["random_salt_5pct"] = rng.random((n, n)) < 0.05
    return shapes


def exact_opposite_distance(m: np.ndarray) -> np.ndarray:
    from scipy import ndimage
    d_lit = ndimage.distance_transform_edt(m)      # lit pixels: distance to nearest dark pixel
    d_dark = ndimage.distance_transform_edt(~m)    # dark pixels: distance to nearest lit pixel
    return np.where(m, d_lit, d_dark)


def ours_opposite_distance(m: np.ndarray, schedule: str, backend: str) -> np.ndarray:
    d = sdf_edt.signed_distance(m, schedule, backend)
    return np.abs(d) + 0.5


def test_edt(n: int, schedules, backend: str, seed: int = 1234) -> dict:
    rng = np.random.default_rng(seed)
    shapes = make_shapes(n, rng)
    report = {}
    for sched in schedules:
        rows = {}
        all_err = []
        t0 = time.perf_counter()
        for name, m in shapes.items():
            ex = exact_opposite_distance(m)
            ou = ours_opposite_distance(m, sched, backend)
            err = ou - ex
            if err.min() < -1e-9:
                raise AssertionError(f"{sched}/{name}: 8SSEDT under-estimates ({err.min()})")
            rows[name] = {"max": float(err.max()), "mean": float(err.mean()),
                          "frac_wrong": float((err > 1e-9).mean())}
            all_err.append(err.ravel())
        dt = time.perf_counter() - t0
        e = np.concatenate(all_err)
        report[sched] = {"per_shape": rows, "max": float(e.max()), "mean": float(e.mean()),
                         "p99_9": float(np.quantile(e, 0.999)), "frac_wrong": float((e > 1e-9).mean()),
                         "seconds": dt}
    return report


def test_backends_identical(n: int) -> dict:
    if not sdf_edt.HAVE_NUMBA:
        return {"skipped": "numba not installed"}
    rng = np.random.default_rng(7)
    shapes = make_shapes(n, rng)
    stack = np.stack(list(shapes.values()))
    out = {}
    for sched in ("sweep", "raster"):
        a = sdf_edt.signed_distance(stack, sched, "numpy")
        b = sdf_edt.signed_distance(stack, sched, "numba")
        out[sched] = bool(np.array_equal(a, b))
        if not out[sched]:
            raise AssertionError(f"numba and numpy differ for schedule {sched}")
    return out


def test_uniform_and_conventions() -> dict:
    allon = np.ones((16, 16), bool)
    d = sdf_edt.signed_distance(allon)
    assert np.all(np.isposinf(d)), "all-lit mask must give +inf"
    d = sdf_edt.signed_distance(~allon)
    assert np.all(np.isneginf(d)), "all-dark mask must give -inf"
    m = np.zeros((8, 8), bool)
    m[:, :4] = True
    d = sdf_edt.signed_distance(m)
    # half-texel convention: texels touching the boundary store +-0.5
    assert np.allclose(d[:, 3], 0.5) and np.allclose(d[:, 4], -0.5), d[0]
    assert np.allclose(d[:, 0], 3.5) and np.allclose(d[:, 7], -3.5), d[0]
    return {"ok": True}


def test_atlas_mapping(seed: int = 3) -> dict:
    """Sampling the padded atlas through the documented formula equals sampling each slice with clamp."""
    rng = np.random.default_rng(seed)
    grid = C.Grid(5, 3, -60, 60, True)
    W, H, P = 13, 9, 2
    cells = rng.random((grid.phi_count, grid.theta_count, H, W))
    atlas = C.pack_atlas(cells, P)
    aw, ah = C.atlas_size(grid, W, H, P)
    assert atlas.shape == (ah, aw)
    u = np.concatenate([rng.random(4000), [0.0, 1.0, 0.0, 1.0, 0.5 / W, 1 - 0.5 / W]])
    v = np.concatenate([rng.random(4000), [0.0, 0.0, 1.0, 1.0, 0.5 / H, 1 - 0.5 / H]])
    worst = 0.0
    for j in range(grid.phi_count):
        for i in range(grid.theta_count):
            ax, ay = C.atlas_uv(u, v, i, j, W, H, P, aw, ah)
            a = C.sample_bilinear(atlas, ax, ay)
            b = C.sample_bilinear(cells[j, i], u, v)
            worst = max(worst, float(np.abs(a - b).max()))
    # texel centre (k+0.5)/W returns exactly texel k
    k = 4
    ax, ay = C.atlas_uv((k + 0.5) / W, (2 + 0.5) / H, 1, 1, W, H, P, aw, ah)
    centre = float(C.sample_bilinear(atlas, ax, ay))
    assert abs(centre - cells[1, 1, 2, k]) < 1e-12
    assert worst < 1e-12, worst
    return {"max_abs_diff_atlas_vs_slice": worst, "texel_centre_exact": True}


def test_lookup() -> dict:
    g = C.Grid(9, 9, -80, 80, False)
    # wrap: theta = 180 and -180 are the same cell
    a = g.lookup(180.0, 0.0)
    b = g.lookup(-180.0, 0.0)
    assert (a[0], a[4]) == (b[0], b[4]) == (0, 0.0), (a, b)
    # theta just below 180 interpolates between the last cell and cell 0
    c = g.lookup(179.0, 0.0)
    assert c[0] == 8 and c[1] == 0, c
    gm = C.Grid(5, 9, -90, 90, True)
    d = gm.lookup(-45.0, 0.0)
    assert d[6] and d[0] == 1 and abs(d[4]) < 1e-12, d
    e = gm.lookup(180.0, 90.0)
    assert e[0] == 3 and e[1] == 4 and abs(e[4] - 1) < 1e-12 and e[2] == 7 and abs(e[5] - 1) < 1e-12, e
    # angles from vectors (UE: X forward, Y right, Z up)
    t, p = C.light_angles([0, 1, 0], [1, 0, 0], [0, 0, 1])
    assert abs(t - 90) < 1e-9 and abs(p) < 1e-9
    t, p = C.light_angles([1, 0, 1], [1, 0, 0], [0, 0, 1])
    assert abs(t) < 1e-9 and abs(p - 45) < 1e-9
    return {"ok": True}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--size", type=int, default=256, help="test image size for the sweep schedule")
    ap.add_argument("--raster-size", type=int, default=None,
                    help="test image size for the raster schedule (default: --size with numba, 96 without)")
    ap.add_argument("--json", default=None, help="write the report to this JSON file")
    args = ap.parse_args()
    backend = sdf_edt.resolve_backend("auto")
    print(f"backend: {backend}  (numba available: {sdf_edt.HAVE_NUMBA})")
    rep = {"backend": backend}
    rep["conventions"] = test_uniform_and_conventions()
    rep["lookup"] = test_lookup()
    rep["atlas_mapping"] = test_atlas_mapping()
    print("conventions / lookup / atlas mapping: OK  (atlas vs slice max diff "
          f"{rep['atlas_mapping']['max_abs_diff_atlas_vs_slice']:.2e})")
    rep["sweep"] = test_edt(args.size, ["sweep"], backend)["sweep"]
    rs = args.raster_size or (args.size if sdf_edt.HAVE_NUMBA else 96)
    rep["raster"] = test_edt(rs, ["raster"], backend)["raster"]
    rep["raster"]["size"] = rs
    rep["sweep"]["size"] = args.size
    rep["numba_vs_numpy_identical"] = test_backends_identical(64)
    for sched in ("sweep", "raster"):
        r = rep[sched]
        print(f"\n8SSEDT schedule '{sched}' ({r['size']}x{r['size']}, {r['seconds']:.2f}s) vs scipy exact EDT, "
              f"error in texels (8SSEDT - exact):")
        print(f"  ALL: max {r['max']:.4f}  mean {r['mean']:.2e}  p99.9 {r['p99_9']:.4f}  "
              f"pixels with any error {100 * r['frac_wrong']:.3f}%")
        for name, row in r["per_shape"].items():
            print(f"  {name:22s} max {row['max']:.4f}  mean {row['mean']:.2e}  wrong {100 * row['frac_wrong']:.3f}%")
    print(f"\nnumba vs numpy identical: {rep['numba_vs_numpy_identical']}")
    if args.json:
        C.write_json(args.json, rep)
    return 0


if __name__ == "__main__":
    sys.exit(main())
