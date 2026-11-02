"""Generate FIRST-DRAFT lit masks for every SDF360 grid direction from a face mesh.

The masks are mask = dot(N_smooth, L) > --bias rasterised in UV space. They are drafts: artists are expected to
repaint them (stylised shapes, nose/eye-socket shadows, consistent topology between neighbouring cells).

Example:
    uv run --with numpy --with pillow python gen_masks.py --mesh face.obj --out masks/ --resolution 1024 ^
        --forward 0,-1,0 --up 0,0,1 --handedness right --normals mesh --smooth-iters 4 ^
        --theta-count 9 --phi-count 9 --phi-min -80 --phi-max 80

Mesh formats: OBJ (built-in parser), glTF/GLB (built-in reader, any TEXCOORD_n), anything else via trimesh if it
is installed (uv run --with trimesh ...), UV channel 0 only. FBX: export OBJ or glTF from the DCC.

Head frame (mesh space): --forward = direction the face looks, --up = head up. The character's right is
cross(Up, Forward) for a left-handed space (Unreal) and cross(Forward, Up) for a right-handed space (Blender, Maya,
OBJ, glTF). The default --handedness is right because OBJ/glTF are right-handed; --right overrides both. Check the
printed right axis and the theta=+90 mask (light on the character's right side).

Normal modes:
    mesh       geometric vertex normals (positions welded across UV seams), then --smooth-iters rounds of
               neighbour averaging on the mesh
    file       the normals stored in the file (custom/transferred face normals)
    ellipsoid  normal of an axis-aligned ellipsoid (in the head frame) fitted to the vertices; the classic clean
               anime-face normal. --ellipsoid-scale / --ellipsoid-center / --ellipsoid-axes override the fit.

Outputs: mask_t{theta}_p{phi}.png per cell, coverage.png (UV islands before dilation), normals_preview.png
(R = N.Right, G = N.Up, B = N.Forward), masks.json (grid + settings; bake_sdf360.py reads the grid from it).
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import os
import struct
import sys
from dataclasses import dataclass
from typing import List, Optional

import numpy as np

import sdf360_common as C
import sdf_edt


# ----------------------------------------------------------------------------------------------
# Mesh container and loaders
# ----------------------------------------------------------------------------------------------

@dataclass
class Mesh:
    pos: np.ndarray                    # (Np, 3)
    tri_pos: np.ndarray                # (T, 3) int
    uv: Optional[np.ndarray] = None    # (Nu, 2)
    tri_uv: Optional[np.ndarray] = None
    nrm: Optional[np.ndarray] = None   # (Nn, 3)
    tri_nrm: Optional[np.ndarray] = None
    notes: Optional[List[str]] = None


def load_obj(path: str, select: Optional[str] = None) -> Mesh:
    vs, vts, vns = [], [], []
    tp, tt, tn = [], [], []
    names = {"o": "", "g": "", "usemtl": ""}
    sel = select.lower() if select else None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            parts = line.split()
            if not parts:
                continue
            tag = parts[0]
            if tag == "v":
                vs.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif tag == "vt":
                vts.append((float(parts[1]), float(parts[2]) if len(parts) > 2 else 0.0))
            elif tag == "vn":
                vns.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif tag in ("o", "g", "usemtl"):
                names[tag] = " ".join(parts[1:])
            elif tag == "f":
                if sel and not any(sel in n.lower() for n in names.values()):
                    continue
                corners = []
                for tok in parts[1:]:
                    p = tok.split("/")
                    vi = int(p[0])
                    ti = int(p[1]) if len(p) > 1 and p[1] else 0
                    ni = int(p[2]) if len(p) > 2 and p[2] else 0
                    vi = vi - 1 if vi > 0 else len(vs) + vi
                    ti = (ti - 1 if ti > 0 else len(vts) + ti) if ti != 0 else -1
                    ni = (ni - 1 if ni > 0 else len(vns) + ni) if ni != 0 else -1
                    corners.append((vi, ti, ni))
                for k in range(1, len(corners) - 1):  # fan triangulation
                    a, b, c = corners[0], corners[k], corners[k + 1]
                    tp.append((a[0], b[0], c[0]))
                    tt.append((a[1], b[1], c[1]))
                    tn.append((a[2], b[2], c[2]))
    if not tp:
        raise SystemExit(f"no faces read from {path}" + (f" matching --select {select!r}" if select else ""))
    tri_uv = np.array(tt, dtype=np.int64)
    tri_n = np.array(tn, dtype=np.int64)
    return Mesh(np.array(vs, dtype=np.float64), np.array(tp, dtype=np.int64),
                np.array(vts, dtype=np.float64) if vts else None, tri_uv if (tri_uv >= 0).all() else None,
                np.array(vns, dtype=np.float64) if vns else None, tri_n if (tri_n >= 0).all() else None, [])


_GL_COMP = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
_GL_NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def _gl_accessor(gl, buffers, idx) -> np.ndarray:
    acc = gl["accessors"][idx]
    n = acc["count"]
    nc = _GL_NC[acc["type"]]
    dt = np.dtype(_GL_COMP[acc["componentType"]]).newbyteorder("<")
    if "bufferView" not in acc:
        return np.zeros((n, nc))
    bv = gl["bufferViews"][acc["bufferView"]]
    buf = buffers[bv["buffer"]]
    off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", 0) or dt.itemsize * nc
    arr = np.ndarray(shape=(n, nc), dtype=dt, buffer=buf, offset=off, strides=(stride, dt.itemsize)).copy()
    if acc.get("normalized"):
        mx = {np.uint8: 255.0, np.uint16: 65535.0, np.int8: 127.0, np.int16: 32767.0}[_GL_COMP[acc["componentType"]]]
        return np.maximum(arr.astype(np.float64) / mx, -1.0)
    return arr.astype(np.float64) if arr.dtype.kind == "f" else arr


def _quat_mat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def _node_matrix(node) -> np.ndarray:
    if "matrix" in node:
        return np.array(node["matrix"], dtype=np.float64).reshape(4, 4).T  # column-major
    m = np.eye(4)
    s = np.array(node.get("scale", [1, 1, 1]), dtype=np.float64)
    r = _quat_mat(node.get("rotation", [0, 0, 0, 1]))
    m[:3, :3] = r * s[None, :]
    m[:3, 3] = node.get("translation", [0, 0, 0])
    return m


def load_gltf(path: str, uv_channel: int = 0, select: Optional[str] = None) -> Mesh:
    with open(path, "rb") as f:
        data = f.read()
    bin_chunk = None
    if data[:4] == b"glTF":
        _, _, total = struct.unpack_from("<III", data, 0)
        off = 12
        gl = None
        while off < total:
            clen, ctype = struct.unpack_from("<II", data, off)
            chunk = data[off + 8: off + 8 + clen]
            if ctype == 0x4E4F534A:
                gl = json.loads(chunk.decode("utf-8"))
            elif ctype == 0x004E4942:
                bin_chunk = chunk
            off += 8 + clen
    else:
        gl = json.loads(data.decode("utf-8"))
    buffers = []
    for b in gl.get("buffers", []):
        uri = b.get("uri")
        if uri is None:
            buffers.append(bin_chunk)
        elif uri.startswith("data:"):
            buffers.append(base64.b64decode(uri.split(",", 1)[1]))
        else:
            with open(os.path.join(os.path.dirname(path), uri), "rb") as f:
                buffers.append(f.read())
    sel = select.lower() if select else None
    P, U, N, T = [], [], [], []
    notes = []
    base = 0
    have_n = True
    have_uv = True

    def emit(mesh_idx, mat):
        nonlocal base, have_n, have_uv
        mesh = gl["meshes"][mesh_idx]
        for prim in mesh.get("primitives", []):
            if prim.get("mode", 4) != 4:
                notes.append(f"skipped non-triangle primitive in mesh {mesh.get('name', mesh_idx)}")
                continue
            mname = ""
            if "material" in prim and "materials" in gl:
                mname = gl["materials"][prim["material"]].get("name", "")
            if sel and sel not in (mesh.get("name", "") + " " + mname).lower():
                continue
            at = prim["attributes"]
            pos = _gl_accessor(gl, buffers, at["POSITION"])[:, :3]
            pos = pos @ mat[:3, :3].T + mat[:3, 3]
            P.append(pos)
            key = f"TEXCOORD_{uv_channel}"
            if key in at:
                U.append(_gl_accessor(gl, buffers, at[key])[:, :2])
            else:
                have_uv = False
                U.append(np.zeros((len(pos), 2)))
            if "NORMAL" in at:
                nm = _gl_accessor(gl, buffers, at["NORMAL"])[:, :3]
                nmat = np.linalg.inv(mat[:3, :3]).T
                nm = nm @ nmat.T
                N.append(nm / np.maximum(np.linalg.norm(nm, axis=1, keepdims=True), 1e-20))
            else:
                have_n = False
                N.append(np.zeros((len(pos), 3)))
            if "indices" in prim:
                idx = _gl_accessor(gl, buffers, prim["indices"]).reshape(-1).astype(np.int64)
            else:
                idx = np.arange(len(pos), dtype=np.int64)
            T.append(idx.reshape(-1, 3) + base)
            base += len(pos)

    def walk(ni, parent):
        node = gl["nodes"][ni]
        m = parent @ _node_matrix(node)
        if "mesh" in node:
            emit(node["mesh"], m)
        for c in node.get("children", []):
            walk(c, m)

    if gl.get("scenes"):
        for root in gl["scenes"][gl.get("scene", 0)].get("nodes", []):
            walk(root, np.eye(4))
    else:
        for mi in range(len(gl.get("meshes", []))):
            emit(mi, np.eye(4))
    if not T:
        raise SystemExit(f"no triangles read from {path}")
    tri = np.concatenate(T)
    pos = np.concatenate(P)
    # glTF stores UV with the origin at the top-left (v down); convert to the OBJ convention (v up)
    uv = np.concatenate(U)
    uv = np.stack([uv[:, 0], 1.0 - uv[:, 1]], axis=1)
    notes.append("glTF texcoords converted to v-up (OBJ) convention")
    return Mesh(pos, tri, uv if have_uv else None, tri if have_uv else None,
                np.concatenate(N) if have_n else None, tri if have_n else None, notes)


def load_trimesh(path: str) -> Mesh:
    try:
        import trimesh  # type: ignore
    except Exception:
        raise SystemExit(f"{path}: format needs trimesh (uv run --with trimesh ...) or export OBJ/glTF")
    m = trimesh.load(path, force="mesh", process=False)
    uv = getattr(getattr(m, "visual", None), "uv", None)
    tri = np.asarray(m.faces, dtype=np.int64)
    return Mesh(np.asarray(m.vertices, dtype=np.float64), tri,
                None if uv is None else np.asarray(uv, dtype=np.float64), None if uv is None else tri,
                np.asarray(m.vertex_normals, dtype=np.float64), tri, ["loaded via trimesh (UV channel 0, v-up)"])


def load_mesh(path: str, uv_channel: int = 0, select: Optional[str] = None) -> Mesh:
    ext = os.path.splitext(path)[1].lower()
    if ext == ".obj":
        if uv_channel != 0:
            raise SystemExit("OBJ has a single UV channel (use --uv-channel 0)")
        return load_obj(path, select)
    if ext in (".gltf", ".glb"):
        return load_gltf(path, uv_channel, select)
    if uv_channel != 0:
        raise SystemExit("only UV channel 0 is available through trimesh")
    return load_trimesh(path)


# ----------------------------------------------------------------------------------------------
# Normals
# ----------------------------------------------------------------------------------------------

def weld(pos: np.ndarray, tol_rel: float):
    diag = float(np.linalg.norm(pos.max(0) - pos.min(0))) or 1.0
    tol = max(tol_rel * diag, 1e-12)
    key = np.round(pos / tol).astype(np.int64)
    _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
    return inv.reshape(-1), pos[first]


def geometric_vertex_normals(mesh: Mesh, wid_tri: np.ndarray, n_weld: int, smooth_iters: int, flip: bool):
    p = mesh.pos[mesh.tri_pos]
    fn = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])          # area weighted
    vn = np.zeros((n_weld, 3))
    for k in range(3):
        np.add.at(vn, wid_tri[:, k], fn)
    # orientation: outward = pointing away from the mesh centroid on average
    centre = mesh.pos.mean(0)
    outward = float(np.sum(fn * (p.mean(1) - centre)))
    flipped = (outward < 0) != flip
    if flipped:
        vn = -vn
    vn /= np.maximum(np.linalg.norm(vn, axis=1, keepdims=True), 1e-20)
    if smooth_iters > 0:
        e = np.concatenate([wid_tri[:, [0, 1]], wid_tri[:, [1, 2]], wid_tri[:, [2, 0]]])
        e = np.unique(np.sort(e, axis=1), axis=0)
        e = e[e[:, 0] != e[:, 1]]
        for _ in range(smooth_iters):
            acc = vn.copy()
            np.add.at(acc, e[:, 0], vn[e[:, 1]])
            np.add.at(acc, e[:, 1], vn[e[:, 0]])
            vn = acc / np.maximum(np.linalg.norm(acc, axis=1, keepdims=True), 1e-20)
    return vn, flipped


def fit_ellipsoid(q: np.ndarray):
    """Axis-aligned ellipsoid A x^2 + B y^2 + C z^2 + D x + E y + G z = 1 (head-frame coords q)."""
    x, y, z = q[:, 0], q[:, 1], q[:, 2]
    M = np.stack([x * x, y * y, z * z, x, y, z], axis=1)
    try:
        coef, *_ = np.linalg.lstsq(M, np.ones(len(q)), rcond=None)
        A, B, Cc, D, E, G = coef
        if min(A, B, Cc) > 0:
            c = np.array([-D / (2 * A), -E / (2 * B), -G / (2 * Cc)])
            k = 1 + D * D / (4 * A) + E * E / (4 * B) + G * G / (4 * Cc)
            if k > 0:
                axes = np.sqrt(k / np.array([A, B, Cc]))
                ext = q.max(0) - q.min(0)
                if np.all(axes < 10 * ext.max()) and np.all(axes > 0.05 * ext.max()):
                    return c, axes, "least-squares quadric"
    except np.linalg.LinAlgError:
        pass
    lo, hi = q.min(0), q.max(0)
    return (lo + hi) / 2, np.maximum((hi - lo) / 2, 1e-9), "bounding box (quadric fit failed)"


# ----------------------------------------------------------------------------------------------
# UV-space rasteriser
# ----------------------------------------------------------------------------------------------

def rasterize(tri_xy: np.ndarray, tri_attr: np.ndarray, W: int, H: int):
    """Rasterise triangles (T,3,2) in pixel coordinates (pixel k centred at k+0.5) with per-corner attributes
    (T,3,C) using barycentric interpolation. Returns (image (H,W,C), covered (H,W), overlap_fraction)."""
    T = tri_xy.shape[0]
    Cn = tri_attr.shape[2]
    out = np.zeros((H, W, Cn))
    cov = np.zeros((H, W), dtype=bool)
    hits = np.zeros((H, W), dtype=np.int32)
    x = tri_xy[..., 0]
    y = tri_xy[..., 1]
    a, b, c = tri_xy[:, 0], tri_xy[:, 1], tri_xy[:, 2]
    area = (b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])
    x0 = np.clip(np.ceil(x.min(1) - 0.5), 0, W - 1).astype(np.int64)
    x1 = np.clip(np.floor(x.max(1) - 0.5), -1, W - 1).astype(np.int64)
    y0 = np.clip(np.ceil(y.min(1) - 0.5), 0, H - 1).astype(np.int64)
    y1 = np.clip(np.floor(y.max(1) - 0.5), -1, H - 1).astype(np.int64)
    valid = (np.abs(area) > 1e-12) & (x1 >= x0) & (y1 >= y0) & (x.max(1) > 0) & (y.max(1) > 0) & \
            (x.min(1) < W) & (y.min(1) < H)
    S = np.maximum(x1 - x0 + 1, y1 - y0 + 1)
    eps_in, eps_strict = -1e-9, 1e-6

    def run(idx, s):
        oy, ox = np.mgrid[0:s, 0:s]
        oy = oy.reshape(-1)
        ox = ox.reshape(-1)
        step = max(1, int(2_000_000 // (s * s)))
        for c0 in range(0, len(idx), step):
            t = idx[c0:c0 + step]
            px = x0[t, None] + ox[None, :]
            py = y0[t, None] + oy[None, :]
            inb = (px <= x1[t, None]) & (py <= y1[t, None])
            cx = px + 0.5
            cy = py + 0.5
            ar = area[t, None]
            A_, B_, C_ = a[t], b[t], c[t]
            w0 = ((B_[:, None, 0] - cx) * (C_[:, None, 1] - cy) - (B_[:, None, 1] - cy) * (C_[:, None, 0] - cx)) / ar
            w1 = ((C_[:, None, 0] - cx) * (A_[:, None, 1] - cy) - (C_[:, None, 1] - cy) * (A_[:, None, 0] - cx)) / ar
            w2 = 1.0 - w0 - w1
            inside = inb & (w0 >= eps_in) & (w1 >= eps_in) & (w2 >= eps_in)
            if not inside.any():
                continue
            ti, pi = np.nonzero(inside)
            tt = t[ti]
            attr = (w0[ti, pi, None] * tri_attr[tt, 0] + w1[ti, pi, None] * tri_attr[tt, 1] +
                    w2[ti, pi, None] * tri_attr[tt, 2])
            yy = py[ti, pi]
            xx = px[ti, pi]
            out[yy, xx] = attr
            cov[yy, xx] = True
            strict = (w0[ti, pi] > eps_strict) & (w1[ti, pi] > eps_strict) & (w2[ti, pi] > eps_strict)
            np.add.at(hits, (yy[strict], xx[strict]), 1)

    prev = 0
    for s in (1, 2, 4, 8, 16, 32, 64):
        idx = np.nonzero(valid & (S > prev) & (S <= s))[0]
        if len(idx):
            run(idx, s)
        prev = s
    for t in np.nonzero(valid & (S > prev))[0]:
        run(np.array([t]), int(S[t]))
    n_cov = int(cov.sum())
    overlap = float((hits > 1).sum()) / max(n_cov, 1)
    return out, cov, overlap


def fill_from_islands(img: np.ndarray, covered: np.ndarray, dilate: int, background: str):
    """Give uncovered texels the value of the nearest covered texel (8SSEDT offsets).

    Texels within `dilate` texels always get filled. Farther texels are filled too for background 'extend'
    (natural continuation, no artificial lit/shadow edge at the island border) or marked background ('dark')."""
    if covered.all():
        return img, np.zeros_like(covered)
    oy, ox, dist = sdf_edt.nearest_seed(covered)
    H, W = covered.shape
    yy, xx = np.mgrid[0:H, 0:W]
    sy = np.clip(yy + oy, 0, H - 1)
    sx = np.clip(xx + ox, 0, W - 1)
    fill = ~covered & np.isfinite(dist)
    if background == "dark":
        bg = ~covered & (dist > dilate)
        fill &= ~bg
    else:
        bg = ~np.isfinite(dist)
    out = img.copy()
    out[fill] = img[sy[fill], sx[fill]]
    return out, bg


# ----------------------------------------------------------------------------------------------
# Main pipeline (also used by demo_synthetic.py)
# ----------------------------------------------------------------------------------------------

def normal_field(mesh: Mesh, frame, W: int, H: int, mode: str = "mesh", smooth_iters: int = 2,
                 v_origin: str = "bottom", dilate: int = 8, background: str = "extend", weld_tol: float = 1e-5,
                 flip_normals: bool = False, ell_scale=None, ell_center=None, ell_axes=None, log=print):
    """Rasterise the smoothed normal field into UV space. Returns dict with 'n' (H,W,3) in head-frame
    components (F, R, U), 'covered', 'background', and info."""
    F, R, Um = frame
    basis = np.stack([F, R, Um])                           # rows: head axes in mesh space
    if mesh.uv is None or mesh.tri_uv is None:
        raise SystemExit("mesh has no UVs on the requested channel")
    uv = mesh.uv[mesh.tri_uv]                               # (T,3,2)
    outside = float(((uv < -1e-3) | (uv > 1 + 1e-3)).any(axis=2).mean())
    if outside > 0.01:
        log(f"WARNING: {100 * outside:.1f}% of UV corners lie outside [0,1]; only the 0-1 tile is rasterised")
    px = uv[..., 0] * W
    py = (1.0 - uv[..., 1]) * H if v_origin == "bottom" else uv[..., 1] * H
    tri_xy = np.stack([px, py], axis=2)
    info = {"mode": mode}
    if mode == "mesh":
        wid, wpos = weld(mesh.pos, weld_tol)
        wid_tri = wid[mesh.tri_pos]
        vn, flipped = geometric_vertex_normals(mesh, wid_tri, len(wpos), smooth_iters, flip_normals)
        attr = vn[wid_tri] @ basis.T
        info.update({"welded_vertices": int(len(wpos)), "smooth_iters": smooth_iters, "winding_flipped": bool(flipped)})
    elif mode == "file":
        if mesh.nrm is None or mesh.tri_nrm is None:
            raise SystemExit("--normals file: the mesh has no normals")
        attr = mesh.nrm[mesh.tri_nrm] @ basis.T
        if flip_normals:
            attr = -attr
    elif mode == "ellipsoid":
        centroid = mesh.pos.mean(0)
        q_all = (mesh.pos - centroid) @ basis.T
        c, axes, how = fit_ellipsoid(q_all)
        if ell_center is not None:
            c = (np.asarray(ell_center) - centroid) @ basis.T
            how += " + center override"
        if ell_axes is not None:
            axes = np.asarray(ell_axes, dtype=np.float64)
            how += " + axes override"
        if ell_scale is not None:
            axes = axes * np.asarray(ell_scale, dtype=np.float64)
        attr = (mesh.pos[mesh.tri_pos] - centroid) @ basis.T - c   # head-frame position rel. to centre
        centre_mesh = centroid + c @ basis
        info.update({"ellipsoid_fit": how, "ellipsoid_center_mesh": centre_mesh.round(6).tolist(),
                     "ellipsoid_axes_FRU": axes.round(6).tolist()})
        log(f"ellipsoid ({how}): centre (mesh space) {np.round(centre_mesh, 4).tolist()}, "
            f"semi-axes F/R/U {np.round(axes, 4).tolist()}")
    else:
        raise SystemExit(f"unknown --normals {mode}")
    img, cov, overlap = rasterize(tri_xy, attr, W, H)
    if overlap > 0.01:
        log(f"WARNING: {100 * overlap:.1f}% of covered texels are covered by more than one triangle "
            "(overlapping/mirrored UVs are not supported: one texel must be one surface point)")
    img, bg = fill_from_islands(img, cov, dilate, background)
    if mode == "ellipsoid":
        img = img / (axes[None, None, :] ** 2)
    n = img / np.maximum(np.linalg.norm(img, axis=2, keepdims=True), 1e-20)
    info.update({"coverage_fraction": float(cov.mean()), "uv_overlap_fraction": overlap})
    return {"n": n, "covered": cov, "background": bg, "info": info}


def masks_for_grid(n: np.ndarray, background: np.ndarray, grid: C.Grid, bias: float):
    for (i, j, t, p) in grid.cells():
        L = C.direction_from_angles(t, p)
        lit = (n @ L) > bias
        lit &= ~background
        yield i, j, t, p, lit


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mesh", required=True, help="OBJ / glTF / GLB (others via trimesh)")
    ap.add_argument("--out", required=True, help="output folder for the masks")
    ap.add_argument("--resolution", default="1024", help="mask size W or WxH (default 1024)")
    ap.add_argument("--uv-channel", type=int, default=0)
    ap.add_argument("--v-origin", choices=("bottom", "top"), default="bottom",
                    help="UV v origin of the loaded mesh: bottom = OBJ/DCC convention (image row = 1-v), "
                         "top = image row = v. glTF is converted to 'bottom' on load. Default bottom")
    ap.add_argument("--forward", required=True, help="face forward direction in mesh space, x,y,z")
    ap.add_argument("--up", required=True, help="head up direction in mesh space, x,y,z")
    ap.add_argument("--right", default=None, help="explicit character-right direction in mesh space (overrides --handedness)")
    ap.add_argument("--handedness", choices=("right", "left"), default="right",
                    help="mesh coordinate system: right (OBJ/glTF/Blender/Maya, default) or left (Unreal)")
    ap.add_argument("--normals", choices=("mesh", "file", "ellipsoid"), default="mesh")
    ap.add_argument("--smooth-iters", type=int, default=2, help="neighbour-averaging rounds for --normals mesh")
    ap.add_argument("--flip-normals", action="store_true", help="invert the normal orientation")
    ap.add_argument("--weld-tol", type=float, default=1e-5, help="position weld tolerance relative to the bbox diagonal")
    ap.add_argument("--ellipsoid-scale", default=None, help="multiply fitted semi-axes, f,r,u")
    ap.add_argument("--ellipsoid-axes", default=None, help="explicit semi-axes f,r,u (mesh units)")
    ap.add_argument("--ellipsoid-center", default=None, help="explicit centre x,y,z in mesh space")
    ap.add_argument("--bias", type=float, default=0.0, help="lit if dot(N, L) > bias (default 0)")
    ap.add_argument("--dilate", type=int, default=8, help="texels filled beyond UV island borders (default 8)")
    ap.add_argument("--background", choices=("extend", "dark"), default="extend",
                    help="texels farther than --dilate: extend the nearest island (default) or force dark")
    ap.add_argument("--select", default=None, help="only faces whose OBJ o/g/usemtl or glTF mesh/material name contains this")
    C.add_grid_args(ap, required=True)
    args = ap.parse_args(argv)

    grid = C.grid_from_args(args)
    W, H = C.parse_size(args.resolution)
    mesh = load_mesh(args.mesh, args.uv_channel, args.select)
    for note in mesh.notes or []:
        print(f"note: {note}")
    right = C.parse_vec3(args.right) if args.right else None
    frame = C.head_frame(C.parse_vec3(args.forward), C.parse_vec3(args.up), right, args.handedness)
    F, R, U = frame
    print(f"mesh: {len(mesh.pos)} vertices, {len(mesh.tri_pos)} triangles")
    print(f"head frame (mesh space): Forward {np.round(F, 4).tolist()}  Right {np.round(R, 4).tolist()}  "
          f"Up {np.round(U, 4).tolist()}  (theta=+90 lights the character's Right side)")
    nf = normal_field(mesh, frame, W, H, args.normals, args.smooth_iters, args.v_origin, args.dilate,
                      args.background, args.weld_tol, args.flip_normals,
                      C.parse_vec3(args.ellipsoid_scale) if args.ellipsoid_scale else None,
                      C.parse_vec3(args.ellipsoid_center) if args.ellipsoid_center else None,
                      C.parse_vec3(args.ellipsoid_axes) if args.ellipsoid_axes else None)
    os.makedirs(args.out, exist_ok=True)
    files = []
    for i, j, t, p, lit in masks_for_grid(nf["n"], nf["background"], grid, args.bias):
        name = C.mask_filename(t, p)
        C.save_mask(os.path.join(args.out, name), lit)
        files.append(name)
    C.save_mask(os.path.join(args.out, C.COVERAGE_NAME), nf["covered"])
    from PIL import Image
    n = nf["n"]
    rgb = np.stack([n[..., 1], n[..., 2], n[..., 0]], axis=2) * 0.5 + 0.5
    Image.fromarray(np.round(np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(os.path.join(args.out, "normals_preview.png"))
    meta = {
        "generator": {
            "tool": "gen_masks.py", "version": C.TOOL_VERSION, "mesh": os.path.abspath(args.mesh).replace("\\", "/"),
            "resolution": [W, H], "uv_channel": args.uv_channel, "v_origin": args.v_origin,
            "frame_mesh_space": {"forward": F.tolist(), "right": R.tolist(), "up": U.tolist(),
                                 "handedness": args.handedness if right is None else "explicit --right"},
            "bias": args.bias, "dilate": args.dilate, "background": args.background, "select": args.select,
            **nf["info"],
        },
        "grid": grid.to_json(),
        "naming": "mask_t{theta}_p{phi}.png, angles in degrees as integers (or up to 3 decimals), white = lit",
        "files": files,
    }
    C.write_json(os.path.join(args.out, C.MASKS_SIDECAR_NAME), meta)
    print(f"wrote {len(files)} masks ({W}x{H}) + coverage.png + normals_preview.png to {args.out}; "
          f"UV coverage {100 * nf['info']['coverage_fraction']:.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
