"""Bake a sub-material ID map (MoonToon T22) from the UV0 triangles of the sections being merged.

Run:  uv run --with numpy --with pillow python bake_submaterial_id.py <out.png> --size 2048 tris0.json:0 tris1.json:1 ...

Each <tris.json>:<row> is Tools/HairTip/export_section_triangles.py's output for one section and the table row its pixels
get. The value written is row * 16 (8 bit), the encoding MF_ToonSubMaterialID reads (SubMaterial ID Split
off). Pixels no section covers are filled by dilation (--dilate texels) from their nearest covered
neighbour, so bilinear-free edge texels still read a sensible row; the rest stays 0 (row 0).

Sections whose UV islands overlap cannot be told apart by a texture: the overlap is reported (pixels claimed
by two different rows) and the later row wins. Import the result with sRGB off, Nearest filtering, no mips,
uncompressed (apply_submaterial.py does).
"""
import argparse
import json

import numpy as np
from PIL import Image


def rasterize(tris, size, value, ids, claimed):
    over = 0
    for tri in tris:
        uv = np.array([[p[3], p[4]] for p in tri], dtype=np.float64)
        px = uv * size - 0.5
        x0, y0 = np.floor(px.min(0)).astype(int)
        x1, y1 = np.ceil(px.max(0)).astype(int)
        x0, y0 = max(x0, 0), max(y0, 0)
        x1, y1 = min(x1, size - 1), min(y1, size - 1)
        if x1 < x0 or y1 < y0:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1))
        p = np.stack([xs, ys], -1).astype(np.float64)
        a, b, c = px
        d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(d) < 1e-12:
            continue
        l1 = ((b[1] - c[1]) * (p[..., 0] - c[0]) + (c[0] - b[0]) * (p[..., 1] - c[1])) / d
        l2 = ((c[1] - a[1]) * (p[..., 0] - c[0]) + (a[0] - c[0]) * (p[..., 1] - c[1])) / d
        inside = (l1 >= -1e-6) & (l2 >= -1e-6) & (1 - l1 - l2 >= -1e-6)
        yy, xx = ys[inside], xs[inside]
        other = claimed[yy, xx] & (ids[yy, xx] != value)
        over += int(other.sum())
        ids[yy, xx] = value
        claimed[yy, xx] = True
    return over


def dilate(ids, claimed, n):
    for _ in range(n):
        grow_ids, grow = ids.copy(), claimed.copy()
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            src_c = np.roll(claimed, (dy, dx), (0, 1))
            src_i = np.roll(ids, (dy, dx), (0, 1))
            take = src_c & ~grow
            grow_ids[take] = src_i[take]
            grow |= take
        ids, claimed = grow_ids, grow
    return ids, claimed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("sections", nargs="+", help="tris.json:row")
    ap.add_argument("--size", type=int, default=2048)
    ap.add_argument("--dilate", type=int, default=4)
    a = ap.parse_args()
    ids = np.zeros((a.size, a.size), np.uint8)
    claimed = np.zeros((a.size, a.size), bool)
    for spec in a.sections:
        path, row = spec.rsplit(":", 1)
        row = int(row)
        if not 0 <= row <= 15:
            raise SystemExit("row %d out of 0..15" % row)
        tris = json.load(open(path, encoding="utf-8"))["triangles"]
        over = rasterize(tris, a.size, row * 16, ids, claimed)
        print("row %2d  %6d triangles  %8d texels  %s" % (row, len(tris), int((ids == row * 16).sum()),
              ("OVERLAP %d texels with earlier rows" % over) if over else ""))
    ids, claimed = dilate(ids, claimed, a.dilate)
    Image.fromarray(ids, "L").save(a.out)
    print("wrote %s (%dx%d), covered %.1f%%" % (a.out, a.size, a.size, 100.0 * claimed.mean()))


if __name__ == "__main__":
    main()
