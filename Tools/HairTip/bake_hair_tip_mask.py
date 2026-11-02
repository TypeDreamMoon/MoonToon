"""Bake a hair-tip mask (MoonToon "Use Hair Tip Translucency", task T12 / G27) from a section's geometry.

Run:  uv run --with numpy --with pillow python bake_hair_tip_mask.py <triangles.json> <out.png> [options]

<triangles.json> comes from export_section_triangles.py (run inside the editor). The mask follows the
convention in Docs/HairTipTranslucency.md:

    0                    not a tip
    0.5 .. 1             tip; 0.5 where the tip starts (the cut), rising to 1 at the lowest point

so Hair Tip Threshold stays at 0.5, and Hair Tip End Opacity < 1 fades the tips toward their ends.

A triangle is a tip when its centroid lies below the cut height (and, with --front, in front of the
given plane). The whole triangle is rasterised with its vertices' values, and the result is dilated by
a few texels, so EVERY vertex of a tip triangle reads >= 0.5. That matters: the translucent redraw
pushes vertices below the threshold back, and a tip triangle with one pushed vertex would tear.

Options:
  --size N            output size in texels (default 2048; match the albedo)
  --cut-fraction F    cut height as a fraction of the section's height above its lowest point (default 0.3)
  --cut-height Z      absolute cut height in the mesh's space, cm (overrides --cut-fraction)
  --up AXIS           up axis: x, y, z, -x, -y, -z (default z)
  --front AXIS:VALUE  only triangles whose centroid has AXIS >= VALUE (e.g. "-y:0" = in front of y=0)
  --dilate N          dilation in texels (default 4)
  --preview IMG       also write <out>_preview.png: IMG (the albedo) with the tips tinted
"""
import argparse
import json

import numpy as np
from PIL import Image

AXES = {"x": (0, 1.0), "y": (1, 1.0), "z": (2, 1.0), "-x": (0, -1.0), "-y": (1, -1.0), "-z": (2, -1.0)}


def rasterize(tri_uv, tri_val, size, out):
    """Max-blend each triangle's interpolated values into out (size x size), texel centres."""
    for uv, val in zip(tri_uv, tri_val):
        px = uv * size - 0.5
        x0 = int(np.floor(px[:, 0].min()))
        x1 = int(np.ceil(px[:, 0].max()))
        y0 = int(np.floor(px[:, 1].min()))
        y1 = int(np.ceil(px[:, 1].max()))
        if x1 < 0 or y1 < 0 or x0 >= size or y0 >= size:
            continue
        x0, y0 = max(x0, 0), max(y0, 0)
        x1, y1 = min(x1, size - 1), min(y1, size - 1)
        ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]
        a, b, c = px
        den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(den) < 1e-12:
            continue
        l0 = ((b[1] - c[1]) * (xs - c[0]) + (c[0] - b[0]) * (ys - c[1])) / den
        l1 = ((c[1] - a[1]) * (xs - c[0]) + (a[0] - c[0]) * (ys - c[1])) / den
        l2 = 1.0 - l0 - l1
        eps = -1e-4
        inside = (l0 >= eps) & (l1 >= eps) & (l2 >= eps)
        if not inside.any():
            # Sub-texel triangle: stamp its centroid texel so it is not lost.
            cx, cy = np.clip(np.round(px.mean(0)).astype(int), 0, size - 1)
            out[cy, cx] = max(out[cy, cx], val.max())
            continue
        v = l0 * val[0] + l1 * val[1] + l2 * val[2]
        region = out[y0:y1 + 1, x0:x1 + 1]
        region[inside] = np.maximum(region[inside], v[inside])


def dilate(mask, steps):
    """Grow non-zero texels outward by `steps` texels, each new texel taking its neighbours' max."""
    m = mask.copy()
    for _ in range(steps):
        pad = np.pad(m, 1, mode="edge")
        nb = np.max(np.stack([pad[1:-1, :-2], pad[1:-1, 2:], pad[:-2, 1:-1], pad[2:, 1:-1],
                              pad[:-2, :-2], pad[:-2, 2:], pad[2:, :-2], pad[2:, 2:]]), axis=0)
        m = np.where(m > 0, m, nb)
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("triangles")
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=2048)
    ap.add_argument("--cut-fraction", type=float, default=0.3)
    ap.add_argument("--cut-height", type=float, default=None)
    ap.add_argument("--up", default="z")
    ap.add_argument("--front", default=None)
    ap.add_argument("--dilate", type=int, default=4)
    ap.add_argument("--preview", default=None)
    args = ap.parse_args()

    data = json.load(open(args.triangles, encoding="utf-8"))
    tris = np.array(data["triangles"], dtype=np.float64)  # (n, 3, 5): x y z u v
    pos, uv = tris[..., :3], tris[..., 3:5]
    uv = uv - np.floor(uv.reshape(-1, 2).min(0))  # keep tiles in [0, 1) when the set is offset as a whole

    up_i, up_s = AXES[args.up]
    h = pos[..., up_i] * up_s
    lo, hi = h.min(), h.max()
    cut = args.cut_height * up_s if args.cut_height is not None else lo + args.cut_fraction * (hi - lo)
    centroid_h = h.mean(1)
    is_tip = centroid_h < cut
    if args.front:
        ax, val = args.front.split(":")
        f_i, f_s = AXES[ax]
        is_tip &= (pos[..., f_i] * f_s).mean(1) >= float(val)

    # Vertex value: 0.5 at the cut -> 1 at the lowest point; vertices of a tip triangle above the cut
    # still get 0.5 (the triangle is a tip as a whole).
    t = np.clip((cut - h) / max(cut - lo, 1e-6), 0.0, 1.0)
    vals = 0.5 + 0.5 * t

    mask = np.zeros((args.size, args.size))
    rasterize(uv[is_tip], vals[is_tip], args.size, mask)
    mask = dilate(mask, args.dilate)

    img = np.round(np.clip(mask, 0, 1) * 255).astype(np.uint8)
    Image.fromarray(np.stack([img, img, img], -1), "RGB").save(args.out)
    print("tips: %d of %d triangles, cut at %.2f (section %.2f .. %.2f); %.1f%% of the texture -> %s"
          % (is_tip.sum(), len(is_tip), cut * up_s, lo * up_s, hi * up_s, 100.0 * (mask > 0).mean(), args.out))

    if args.preview:
        alb = Image.open(args.preview).convert("RGB").resize((args.size, args.size))
        a = np.asarray(alb).astype(np.float64)
        tint = np.array([255.0, 40.0, 160.0])
        k = (mask > 0)[..., None] * (0.35 + 0.5 * np.clip((mask[..., None] - 0.5) * 2.0, 0, 1))
        pv = a * (1 - k) + tint * k
        Image.fromarray(pv.astype(np.uint8)).save(args.out.rsplit(".", 1)[0] + "_preview.png")


if __name__ == "__main__":
    main()
