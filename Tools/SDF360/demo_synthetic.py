"""Self-contained SDF360 demo / end-to-end test (no assets needed).

1. Builds a synthetic face mesh: the front cap of an ellipsoid with a nose bump and two eye-socket dents,
   planar left-right symmetric UVs (character's right on the image's left, like a front-view texture), written
   as OBJ in a left-handed X-forward / Y-right / Z-up frame.
2. gen_masks.py -> masks for a 9x9 full-circle grid (phi -80..80) and a 5x9 mirror grid (phi -90..90, poles).
3. bake_sdf360.py -> atlas + slices (512^2 masks supersampled into 256^2 cells).
4. preview.py -> sweeps, contact sheets, GIFs and jump metrics.
5. In-process checks: atlas vs slice reconstruction identical; reconstruction vs the exact N.L ground truth at
   every sweep direction (how much the 81/45 drawn masks + SDF interpolation deviate from the true terminator).

Run:
    uv run --with numpy --with pillow python demo_synthetic.py
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

import numpy as np

import sdf360_common as C
import gen_masks
import preview

HERE = os.path.dirname(os.path.abspath(__file__))


def make_face_obj(path: str, n: int = 161) -> None:
    span = 1.1
    lattice = (np.arange(n) - (n - 1) / 2) * (2 * span / (n - 1))   # exactly symmetric about 0
    ys = lattice                                  # character right (+Y)
    zs = -lattice                                 # up (+Z), top row first
    Y, Z = np.meshgrid(ys, zs)
    r2 = Y ** 2 + Z ** 2
    inside = r2 <= 1.0
    base = 0.8 * np.sqrt(np.clip(1.0 - r2, 0.0, None))
    nose = 0.22 * np.exp(-((Y / 0.08) ** 2 + ((Z + 0.05) / 0.16) ** 2))
    eyes = -0.07 * (np.exp(-(((Y - 0.38) / 0.12) ** 2 + ((Z - 0.18) / 0.12) ** 2)) +
                    np.exp(-(((Y + 0.38) / 0.12) ** 2 + ((Z - 0.18) / 0.12) ** 2)))
    X = base + (nose + eyes) * np.clip((1.0 - r2) * 3.0, 0.0, 1.0)  # forward (+X)
    idx = -np.ones((n, n), dtype=np.int64)
    idx[inside] = np.arange(int(inside.sum()))
    lines = ["# synthetic SDF360 demo face, left-handed X forward / Y right / Z up"]
    for (x, y, z) in zip(X[inside], Y[inside], Z[inside]):
        lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
    for (y, z) in zip(Y[inside], Z[inside]):
        u = 0.5 - y / (2 * 1.15)                   # character right -> image left
        v_img = 0.5 - z / (2 * 1.15)               # top of the face -> top of the image
        lines.append(f"vt {u:.6f} {1.0 - v_img:.6f}")
    for r in range(n - 1):
        for c in range(n - 1):
            a, b, cc, d = idx[r, c], idx[r, c + 1], idx[r + 1, c], idx[r + 1, c + 1]
            # mirrored quad diagonals on the two halves so the mesh (and its normals) is exactly symmetric
            tris = ((a, cc, d), (a, d, b)) if c < (n - 1) // 2 else ((a, cc, b), (b, cc, d))
            for tri in tris:
                if min(tri) >= 0:
                    lines.append("f " + " ".join(f"{t + 1}/{t + 1}" for t in tri))
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def run(cmd, allow_codes=(0,)):
    print("$ " + " ".join(cmd[1:]))
    t0 = time.perf_counter()
    p = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
    sys.stdout.write("".join("    " + l + "\n" for l in p.stdout.splitlines()))
    if p.stderr.strip():
        sys.stdout.write("".join("    [stderr] " + l + "\n" for l in p.stderr.splitlines()))
    if p.returncode not in allow_codes:
        raise SystemExit(f"command failed with exit code {p.returncode}")
    print(f"    ({time.perf_counter() - t0:.1f}s, exit {p.returncode})")
    return p.returncode


def ground_truth_check(rec: preview.Reconstructor, nfield: dict, bias: float, step: float):
    """Compare reconstructed lit (s > 0) with the exact N.L > bias mask at every sweep direction."""
    cov = nfield["covered"]
    n = nfield["n"]
    bg = nfield["background"]
    res, fields, _ = preview.run_sweeps(rec, cov, step)
    out = {}
    for sw in preview.build_sweeps(rec.grid, step):
        s = fields[sw["name"]]
        mism = []
        gt_lit = []
        for k, (t, p) in enumerate(sw["angles"]):
            p_eff = min(max(p, rec.grid.phi_min), rec.grid.phi_max)
            L = C.direction_from_angles(t, p_eff)
            gt = ((n @ L) > bias) & ~bg
            gt_lit.append(gt)
            mism.append(float(((s[k] > 0) != gt)[cov].mean()))
        gt_lit = np.stack(gt_lit)
        gt_flips = (gt_lit[1:] != gt_lit[:-1])[:, cov].mean(axis=1)
        out[sw["name"]] = {"max_mismatch": max(mism), "mean_mismatch": float(np.mean(mism)),
                           "worst_angle": sw["angles"][int(np.argmax(mism))],
                           "gt_max_flip": float(gt_flips.max()), "recon_max_flip": res[sw["name"]]["max_flip"]}
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(HERE, "_test_out", "demo"))
    ap.add_argument("--mask-res", type=int, default=512)
    ap.add_argument("--cell", type=int, default=256)
    ap.add_argument("--step", type=float, default=5.0)
    args = ap.parse_args(argv)
    out = os.path.abspath(args.out).replace("\\", "/")
    os.makedirs(out, exist_ok=True)
    py = sys.executable
    obj = f"{out}/face.obj"
    make_face_obj(obj)
    print(f"synthetic face mesh -> {obj}")

    frame_args = ["--forward", "1,0,0", "--up", "0,0,1", "--handedness", "left"]
    variants = {
        "full9x9": ["--theta-count", "9", "--phi-count", "9", "--phi-min", "-80", "--phi-max", "80"],
        "mirror5x9": ["--theta-count", "5", "--phi-count", "9", "--phi-min", "-90", "--phi-max", "90", "--mirror"],
    }
    summary = {}
    for name, grid_args in variants.items():
        mdir, bdir = f"{out}/masks_{name}", f"{out}/baked_{name}"
        run([py, "gen_masks.py", "--mesh", obj, "--out", mdir, "--resolution", str(args.mask_res),
             "--normals", "mesh", "--smooth-iters", "2", *frame_args, *grid_args])
        run([py, "bake_sdf360.py", "--masks", mdir, "--out", bdir, "--cell-size", str(args.cell), "--padding", "4"])
        code = run([py, "preview.py", "--bake", bdir, "--step", str(args.step)], allow_codes=(0, 1))
        rep = C.read_json(f"{bdir}/preview/preview_report.json")
        meta = C.read_json(f"{bdir}/sdf360.json")
        rec = preview.Reconstructor(bdir, "atlas")
        rec_sl = preview.Reconstructor(bdir, "slices")
        uu, vv = preview.face_uv_grid(rec.W, rec.H, 2)
        worst = 0.0
        for t in range(-180, 181, 15):
            for p in (-60.0, 0.0, 35.0):
                worst = max(worst, float(np.abs(rec.evaluate_sweep_angle(t, p, uu, vv) -
                                                rec_sl.evaluate_sweep_angle(t, p, uu, vv)).max()))
        mesh = gen_masks.load_mesh(obj)
        frame = C.head_frame([1, 0, 0], [0, 0, 1], None, "left")
        nf = gen_masks.normal_field(mesh, frame, rec.W, rec.H, "mesh", 2, log=lambda *_: None)
        gt = ground_truth_check(rec, nf, 0.0, args.step)
        summary[name] = {
            "preview_exit": code,
            "spread": meta["encoding"]["spread_texels"],
            "required_spread": meta["encoding"]["required_spread_texels"],
            "bake_warnings": meta["stats"]["warnings"],
            "atlas_vs_slices_max_abs_s": worst,
            "sweeps": {k: {"max_flip": v["max_flip"], "max_flip_at": v["max_flip_at"], "mean_flip": v["mean_flip"],
                           "max_spike_ratio": v["max_spike_ratio"],
                           "max_regain": (v["monotone"] or {}).get("max_regain"),
                           "multi_flip_texels": (v["monotone"] or {}).get("multi_flip_texels"),
                           "flags": v["flags"]} for k, v in rep["sweeps"].items()},
            "ground_truth": gt,
        }

    # ellipsoid normal mode smoke test
    run([py, "gen_masks.py", "--mesh", obj, "--out", f"{out}/masks_ellipsoid", "--resolution", "128",
         "--normals", "ellipsoid", *frame_args, "--theta-count", "4", "--phi-count", "3", "--phi-min", "-45",
         "--phi-max", "45"])

    C.write_json(f"{out}/demo_report.json", summary)
    print("\n================ SDF360 synthetic demo summary ================")
    for name, s in summary.items():
        print(f"[{name}] spread R={s['spread']:g} (jump-free min {s['required_spread']:.1f}), "
              f"atlas==slices max|ds| {s['atlas_vs_slices_max_abs_s']:.2e}, bake warnings {len(s['bake_warnings'])}")
        for k, v in s["sweeps"].items():
            g = s["ground_truth"][k]
            mono = "" if v["max_regain"] is None else (f", regain {100 * v['max_regain']:.3f}%, "
                                                        f"multi-flip {100 * v['multi_flip_texels']:.3f}%")
            print(f"   {k:10s} max flip/5deg {100 * v['max_flip']:.2f}% (true N.L {100 * g['gt_max_flip']:.2f}%), "
                  f"mean {100 * v['mean_flip']:.2f}%, spike {v['max_spike_ratio']:.2f}x{mono}, "
                  f"vs N.L mismatch max {100 * g['max_mismatch']:.2f}% @{g['worst_angle']} mean {100 * g['mean_mismatch']:.2f}%, "
                  f"flags {len(v['flags'])}")
            for f in v["flags"]:
                print(f"      ! {f}")
    print(f"report -> {out}/demo_report.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
