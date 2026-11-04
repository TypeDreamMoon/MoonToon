"""Bake a classic face SDF (threshold map) from light masks, straight to the precise formats (MoonToon T24).

  uv run --with numpy --with pillow python bake_face_sdf.py --masks masks/ --out face_sdf
         [--phi 0] [--size 1024] [--rg] [--bits 16]

Masks: the SDF360 horizontal row in mirror mode -- mask_t{theta}_p{phi}.png for theta in [0, 180] (0 = light
straight in front, 90 = at the character's side, 180 = behind), white = lit, in the face UV space. Draw them
with Tools/SDF360/gen_masks.py (--phi-count 1 --mirror) and paint over, or by hand. Two masks at least;
0 and 180 may be pure white / pure black.

Per texel the light angle where it turns dark is found between the two masks that bracket the change, by
interpolating their signed distances (the same half-texel distance SDF360 bakes), so the terminator slides
smoothly between hand-drawn masks instead of stepping: v = angle / 180, lit while v > the light angle / 180.
A texel lit in every mask gets the last angle, one dark in every mask the first. Lit regions are assumed to
shrink as the angle grows; a texel that turns lit again later keeps its first crossing.

Outputs: <out>.png (16-bit, or 8-bit with --bits 8), <out>_rg.png with --rg (R+G split), <out>.json.
If the shadows come out on the wrong half in Unreal, tick "Face SDF Flip Left Right" on the material."""
import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import face_sdf_common as F  # noqa: E402


def load_masks(mask_dir, phi):
    found = []
    for name in sorted(os.listdir(mask_dir)):
        m = F.C.MASK_RE.match(name)
        if m and abs(float(m.group(2)) - phi) <= 0.5:
            found.append((float(m.group(1)), os.path.join(mask_dir, name)))
    found = sorted((t, p) for (t, p) in found if -1e-6 <= t <= 180.0 + 1e-6)
    if len(found) < 2:
        sys.exit("need at least two mask_t{theta}_p%s.png with theta in [0, 180] in %s" % (F.C.fmt_angle(phi), mask_dir))
    return found


def box_down(img, size):
    h, w = img.shape
    if size is None or (w == size and h == size):
        return img
    if w % size or h % size or w // size != h // size:
        sys.exit("--size must divide the mask size evenly (%dx%d)" % (w, h))
    f = w // size
    return img.reshape(size, f, size, f).mean(axis=(1, 3))


def synthesize_uniform(d, lit, region, margin=1.0):
    """All-lit / all-dark masks have no boundary (+-inf distance): every texel would cross at the
    neighbouring mask's angle. Like SDF360 (bake_sdf360.synthesize_uniform), such a mask takes its
    nearest real neighbour's field -- the next one for an all-lit mask, the previous for an all-dark one --
    shifted so it keeps its sign over the face region with `margin` texels to spare. The boundary then
    keeps moving at the neighbour's speed until it leaves the face at the uniform mask's angle."""
    K = len(d)
    real = [k for k in range(K) if lit[k].any() and (~lit[k]).any()]
    if not real:
        sys.exit("every mask is uniform (all lit or all dark); nothing to bake")
    out = d.copy()
    for k in range(K):
        if k in real:
            continue
        all_lit = bool(lit[k].all())
        after = [n for n in real if n > k]
        before = [n for n in real if n < k]
        n = (after[0] if after else before[-1]) if all_lit else (before[-1] if before else after[0])
        dn = d[n]
        if all_lit:
            out[k] = dn + (margin - dn[region].min())
        else:
            out[k] = dn - (margin + dn[region].max())
    return out


def bake(masks, edt_schedule="sweep", coverage=None):
    thetas = np.array([t for (t, _) in masks])
    lit = np.stack([F.C.load_gray(p) >= 0.5 for (_, p) in masks])
    shape = lit.shape[1:]
    if any(m.shape != shape for m in lit):
        sys.exit("all masks must have the same size")
    d = F.sdf_edt.signed_distance(lit, edt_schedule)
    region = np.ones(shape, dtype=bool) if coverage is None else coverage
    d = synthesize_uniform(d, lit, region)
    angle = np.full(shape, np.nan)
    angle[~lit[0]] = thetas[0]          # dark already in the first mask
    for k in range(len(thetas) - 1):
        a, b = d[k], d[k + 1]
        cross = np.isnan(angle) & (a > 0.0) & (b <= 0.0)
        f = np.where(cross, a / np.maximum(a - b, 1e-9), 0.0)
        angle = np.where(cross, thetas[k] + f * (thetas[k + 1] - thetas[k]), angle)
    angle = np.where(np.isnan(angle), thetas[-1], angle)   # lit in every mask
    return np.clip(angle / 180.0, 0.0, 1.0), thetas


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--masks", required=True)
    ap.add_argument("--out", required=True, help="output path without extension")
    ap.add_argument("--phi", type=float, default=0.0)
    ap.add_argument("--size", type=int, default=None, help="output size (divides the mask size; box filter)")
    ap.add_argument("--bits", type=int, default=16, choices=[8, 16])
    ap.add_argument("--rg", action="store_true", help="also write <out>_rg.png (R+G split, ~9 bits)")
    ap.add_argument("--edt", default="sweep", choices=list(F.sdf_edt.SCHEDULES))
    ap.add_argument("--coverage", help="white = face texels (SDF360 gen_masks.py coverage.png); limits the uniform-mask shift to the face")
    args = ap.parse_args()

    masks = load_masks(args.masks, args.phi)
    coverage = (F.C.load_gray(args.coverage) >= 0.5) if args.coverage else None
    v, thetas = bake(masks, args.edt, coverage)
    v = box_down(v, args.size)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    if args.bits == 16:
        F.save16(args.out + ".png", v)
    else:
        F.C.save_gray(args.out + ".png", np.round(v * 255.0).astype(np.uint8))
    print(F.levels_report(v, args.out + ".png"))
    if args.rg:
        F.save_rgba(args.out + "_rg.png", F.encode_rg(v))
    F.C.write_json(args.out + ".json", {
        "tool": "Tools/FaceSDF/bake_face_sdf.py", "convention": "v = angle where the texel turns dark / 180; lit while v > light angle / 180",
        "thetas": [float(t) for t in thetas], "phi": args.phi, "size": list(v.shape[::-1]), "bits": args.bits, "rg": args.rg,
        "import": "sRGB off, no mips, Half Float (R16F) / Grayscale for the 16-bit PNG, VectorDisplacementmap (BGRA8) for _rg"})


if __name__ == "__main__":
    main()
