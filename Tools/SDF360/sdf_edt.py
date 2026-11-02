"""8SSEDT: 8-point sequential signed Euclidean distance transform (written from scratch).

Every pixel carries an integer offset vector (oy, ox) to the nearest "seed" pixel found so far.
Sequential scans propagate offsets from already-visited 8-neighbours: a neighbour q = p + (dy, dx)
proposes offset(q) + (dy, dx) for p, which is accepted when its squared length is strictly smaller.

Two scan schedules are provided; both are deterministic and give identical results with or without numba:

* ``raster`` - the classic two-pass 8SSEDT (Leymarie & Levine 1992 style):
      pass 1, rows top -> bottom:  left->right with neighbours (0,-1) (-1,0) (-1,-1) (-1,+1)
                                   then right->left with (0,+1)
      pass 2, rows bottom -> top:  right->left with (0,+1) (+1,0) (+1,-1) (+1,+1)
                                   then left->right with (0,-1)
  (neighbours written as (dy, dx)). Each row's horizontal scans are inherently sequential, so the
  numpy fallback of this schedule loops over pixels (vectorised only over a batch of images); it is
  meant for small images / reference use. With numba it is fast.

* ``sweep`` (default) - the same 8-neighbour propagation organised as four line sweeps
      down  : row y from row y-1   (neighbours (-1,-1) (-1,0) (-1,+1))
      up    : row y from row y+1   (neighbours (+1,-1) (+1,0) (+1,+1))
      right : column x from x-1    (neighbours (-1,-1) (0,-1) (+1,-1))
      left  : column x from x+1    (neighbours (-1,+1) (0,+1) (+1,+1))
  followed by a second down/up pair. A whole row (or column) only reads the previous line, so numpy
  vectorises each line: O(H + W) Python iterations, fast without numba.

Signed distance (half-texel convention, see sdf360_common.DISTANCE_CONVENTION):
    lit pixel  : d = +(distance to nearest dark pixel centre - 0.5)
    dark pixel : d = -(distance to nearest lit pixel centre - 0.5)
Pixels outside the image do not exist (they are never seeds). A mask with no opposite class gives +-inf.
"""

from __future__ import annotations

import numpy as np

INF_OFF = 1 << 20          # "no seed yet" offset; drift by +-(W+H) during propagation is harmless
_NO_SEED_D2 = (INF_OFF // 2) ** 2

try:  # optional acceleration
    import numba as _nb  # type: ignore
    HAVE_NUMBA = True
except Exception:  # pragma: no cover - depends on environment
    _nb = None
    HAVE_NUMBA = False


# ----------------------------------------------------------------------------------------------
# numpy implementations
# ----------------------------------------------------------------------------------------------

def _relax_rows_np(oy, ox, d2, y, ys):
    """Row y takes candidates from row ys (= y-1 or y+1), neighbours dx = -1, 0, +1 in that order."""
    dyn = ys - y
    W = oy.shape[2]
    for dx in (-1, 0, 1):
        if dx == -1:
            tgt = slice(1, W)
            src = slice(0, W - 1)
        elif dx == 0:
            tgt = slice(0, W)
            src = slice(0, W)
        else:
            tgt = slice(0, W - 1)
            src = slice(1, W)
        cy = oy[:, ys, src] + dyn
        cx = ox[:, ys, src] + dx
        cd = cy.astype(np.int64) ** 2 + cx.astype(np.int64) ** 2
        cur = d2[:, y, tgt]
        better = cd < cur
        if better.any():
            ty = oy[:, y, tgt]
            tx = ox[:, y, tgt]
            ty[better] = cy[better]
            tx[better] = cx[better]
            cur[better] = cd[better]


def _relax_cols_np(oy, ox, d2, x, xs):
    """Column x takes candidates from column xs (= x-1 or x+1), neighbours dy = -1, 0, +1 in that order."""
    dxn = xs - x
    H = oy.shape[1]
    for dy in (-1, 0, 1):
        if dy == -1:
            tgt = slice(1, H)
            src = slice(0, H - 1)
        elif dy == 0:
            tgt = slice(0, H)
            src = slice(0, H)
        else:
            tgt = slice(0, H - 1)
            src = slice(1, H)
        cy = oy[:, src, xs] + dy
        cx = ox[:, src, xs] + dxn
        cd = cy.astype(np.int64) ** 2 + cx.astype(np.int64) ** 2
        cur = d2[:, tgt, x]
        better = cd < cur
        if better.any():
            ty = oy[:, tgt, x]
            tx = ox[:, tgt, x]
            ty[better] = cy[better]
            tx[better] = cx[better]
            cur[better] = cd[better]


def _sweep_np(oy, ox, d2):
    # Basic slicing returns views, so the _relax_* helpers update oy/ox/d2 in place.
    K, H, W = oy.shape
    for y in range(1, H):
        _relax_rows_np(oy, ox, d2, y, y - 1)
    for y in range(H - 2, -1, -1):
        _relax_rows_np(oy, ox, d2, y, y + 1)
    for x in range(1, W):
        _relax_cols_np(oy, ox, d2, x, x - 1)
    for x in range(W - 2, -1, -1):
        _relax_cols_np(oy, ox, d2, x, x + 1)
    for y in range(1, H):
        _relax_rows_np(oy, ox, d2, y, y - 1)
    for y in range(H - 2, -1, -1):
        _relax_rows_np(oy, ox, d2, y, y + 1)


def _cmp_px_np(oy, ox, d2, y, x, dy, dx):
    """Per-pixel compare, vectorised over the batch only (raster fallback)."""
    cy = oy[:, y + dy, x + dx] + dy
    cx = ox[:, y + dy, x + dx] + dx
    cd = cy.astype(np.int64) ** 2 + cx.astype(np.int64) ** 2
    better = cd < d2[:, y, x]
    if better.any():
        oy[better, y, x] = cy[better]
        ox[better, y, x] = cx[better]
        d2[better, y, x] = cd[better]


def _raster_np(oy, ox, d2):
    K, H, W = oy.shape
    for y in range(H):
        for x in range(W):
            if x > 0:
                _cmp_px_np(oy, ox, d2, y, x, 0, -1)
            if y > 0:
                _cmp_px_np(oy, ox, d2, y, x, -1, 0)
                if x > 0:
                    _cmp_px_np(oy, ox, d2, y, x, -1, -1)
                if x < W - 1:
                    _cmp_px_np(oy, ox, d2, y, x, -1, 1)
        for x in range(W - 2, -1, -1):
            _cmp_px_np(oy, ox, d2, y, x, 0, 1)
    for y in range(H - 1, -1, -1):
        for x in range(W - 1, -1, -1):
            if x < W - 1:
                _cmp_px_np(oy, ox, d2, y, x, 0, 1)
            if y < H - 1:
                _cmp_px_np(oy, ox, d2, y, x, 1, 0)
                if x > 0:
                    _cmp_px_np(oy, ox, d2, y, x, 1, -1)
                if x < W - 1:
                    _cmp_px_np(oy, ox, d2, y, x, 1, 1)
        for x in range(1, W):
            _cmp_px_np(oy, ox, d2, y, x, 0, -1)


# ----------------------------------------------------------------------------------------------
# numba implementations (same comparison order as the numpy ones -> identical results)
# ----------------------------------------------------------------------------------------------

if HAVE_NUMBA:
    @_nb.njit(cache=True, inline="always")
    def _cmp_nb(oy, ox, d2, k, y, x, dy, dx):
        cy = oy[k, y + dy, x + dx] + dy
        cx = ox[k, y + dy, x + dx] + dx
        cd = np.int64(cy) * np.int64(cy) + np.int64(cx) * np.int64(cx)
        if cd < d2[k, y, x]:
            oy[k, y, x] = cy
            ox[k, y, x] = cx
            d2[k, y, x] = cd

    @_nb.njit(cache=True)
    def _raster_nb(oy, ox, d2):
        K, H, W = oy.shape
        for k in range(K):
            for y in range(H):
                for x in range(W):
                    if x > 0:
                        _cmp_nb(oy, ox, d2, k, y, x, 0, -1)
                    if y > 0:
                        _cmp_nb(oy, ox, d2, k, y, x, -1, 0)
                        if x > 0:
                            _cmp_nb(oy, ox, d2, k, y, x, -1, -1)
                        if x < W - 1:
                            _cmp_nb(oy, ox, d2, k, y, x, -1, 1)
                for x in range(W - 2, -1, -1):
                    _cmp_nb(oy, ox, d2, k, y, x, 0, 1)
            for y in range(H - 1, -1, -1):
                for x in range(W - 1, -1, -1):
                    if x < W - 1:
                        _cmp_nb(oy, ox, d2, k, y, x, 0, 1)
                    if y < H - 1:
                        _cmp_nb(oy, ox, d2, k, y, x, 1, 0)
                        if x > 0:
                            _cmp_nb(oy, ox, d2, k, y, x, 1, -1)
                        if x < W - 1:
                            _cmp_nb(oy, ox, d2, k, y, x, 1, 1)
                for x in range(1, W):
                    _cmp_nb(oy, ox, d2, k, y, x, 0, -1)

    @_nb.njit(cache=True)
    def _rows_nb(oy, ox, d2, k, y, ys):
        W = oy.shape[2]
        dyn = ys - y
        for x in range(W):
            # candidates from the source row, dx = -1, 0, +1 in that order (matches numpy)
            for dx in (-1, 0, 1):
                xs = x + dx
                if xs < 0 or xs >= W:
                    continue
                cy = oy[k, ys, xs] + dyn
                cx = ox[k, ys, xs] + dx
                cd = np.int64(cy) * np.int64(cy) + np.int64(cx) * np.int64(cx)
                if cd < d2[k, y, x]:
                    oy[k, y, x] = cy
                    ox[k, y, x] = cx
                    d2[k, y, x] = cd

    @_nb.njit(cache=True)
    def _cols_nb(oy, ox, d2, k, x, xs):
        H = oy.shape[1]
        dxn = xs - x
        for y in range(H):
            for dy in (-1, 0, 1):
                ysrc = y + dy
                if ysrc < 0 or ysrc >= H:
                    continue
                cy = oy[k, ysrc, xs] + dy
                cx = ox[k, ysrc, xs] + dxn
                cd = np.int64(cy) * np.int64(cy) + np.int64(cx) * np.int64(cx)
                if cd < d2[k, y, x]:
                    oy[k, y, x] = cy
                    ox[k, y, x] = cx
                    d2[k, y, x] = cd

    @_nb.njit(cache=True)
    def _sweep_nb(oy, ox, d2):
        K, H, W = oy.shape
        for k in range(K):
            for y in range(1, H):
                _rows_nb(oy, ox, d2, k, y, y - 1)
            for y in range(H - 2, -1, -1):
                _rows_nb(oy, ox, d2, k, y, y + 1)
            for x in range(1, W):
                _cols_nb(oy, ox, d2, k, x, x - 1)
            for x in range(W - 2, -1, -1):
                _cols_nb(oy, ox, d2, k, x, x + 1)
            for y in range(1, H):
                _rows_nb(oy, ox, d2, k, y, y - 1)
            for y in range(H - 2, -1, -1):
                _rows_nb(oy, ox, d2, k, y, y + 1)


# ----------------------------------------------------------------------------------------------
# public API
# ----------------------------------------------------------------------------------------------

SCHEDULES = ("sweep", "raster")


def resolve_backend(backend: str = "auto") -> str:
    if backend == "auto":
        return "numba" if HAVE_NUMBA else "numpy"
    if backend == "numba" and not HAVE_NUMBA:
        raise RuntimeError("numba requested but not importable (run with: uv run --with numba ...)")
    if backend not in ("numba", "numpy"):
        raise ValueError(backend)
    return backend


def nearest_seed(seeds: np.ndarray, schedule: str = "sweep", backend: str = "auto"):
    """Offsets to the nearest seed pixel (8SSEDT propagation).

    seeds: bool array (H, W) or (K, H, W).
    Returns (oy, ox, dist): int32 offsets (seed = pixel + offset) and float64 Euclidean distance
    between pixel centres (np.inf where the image has no seed at all).
    """
    s = np.asarray(seeds, dtype=bool)
    squeeze = s.ndim == 2
    if squeeze:
        s = s[None]
    if s.ndim != 3:
        raise ValueError("seeds must be 2D or 3D")
    oy = np.where(s, 0, INF_OFF).astype(np.int32)
    ox = oy.copy()
    d2 = oy.astype(np.int64) ** 2 + ox.astype(np.int64) ** 2
    be = resolve_backend(backend)
    if schedule == "sweep":
        (_sweep_nb if be == "numba" else _sweep_np)(oy, ox, d2)
    elif schedule == "raster":
        (_raster_nb if be == "numba" else _raster_np)(oy, ox, d2)
    else:
        raise ValueError(f"unknown schedule {schedule!r}")
    dist = np.sqrt(d2.astype(np.float64))
    dist[d2 >= _NO_SEED_D2] = np.inf
    if squeeze:
        return oy[0], ox[0], dist[0]
    return oy, ox, dist


def edt(seeds: np.ndarray, schedule: str = "sweep", backend: str = "auto") -> np.ndarray:
    """Distance from every pixel centre to the nearest seed pixel centre (0 on seeds)."""
    return nearest_seed(seeds, schedule, backend)[2]


def signed_distance(lit: np.ndarray, schedule: str = "sweep", backend: str = "auto") -> np.ndarray:
    """Signed distance to the lit/shadow boundary in pixels, half-texel convention, + inside lit.

    lit: bool (H, W) or (K, H, W). Uniform images give +inf (all lit) or -inf (all dark).
    """
    m = np.asarray(lit, dtype=bool)
    squeeze = m.ndim == 2
    if squeeze:
        m = m[None]
    K = m.shape[0]
    both = np.concatenate([~m, m], axis=0)        # seeds: dark pixels, then lit pixels
    dist = edt(both, schedule, backend)
    to_dark = dist[:K]
    to_lit = dist[K:]
    d = np.where(m, to_dark - 0.5, -(to_lit - 0.5))
    return d[0] if squeeze else d
