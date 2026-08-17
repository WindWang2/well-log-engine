"""Curve local-extrema detection + snap (FRS §2.x 分层线拖拽吸附 曲线极值磁吸).

Pure numpy — no Qt, headless-testable. Finds local peaks/valleys of a well
curve (first-difference sign changes) and snaps a target depth to the
nearest one within tolerance.
"""

from __future__ import annotations

import numpy as np


def local_extrema_depths(
    depth: np.ndarray,
    values: np.ndarray,
    null_mask: np.ndarray,
    *,
    min_span: int = 2,
) -> np.ndarray:
    """Depths of local peaks and valleys.

    A sample is an extremum when the first-difference sign changes across
    it (+→− peak, −→+ valley). Null / non-finite samples are skipped;
    ``min_span`` enforces a minimum sample distance between reported
    extrema to damp noise (consecutive sign flips within ``min_span``
    samples collapse to the first one). Returns an (M,) depth array.
    """
    dep = np.asarray(depth, dtype=np.float64)
    vals = np.asarray(values, dtype=np.float64)
    mask = np.asarray(null_mask, dtype=bool)
    n = min(dep.size, vals.size, mask.size)
    if n < 3:
        return np.empty(0, dtype=np.float64)

    valid = (~mask[:n]) & np.isfinite(vals[:n])
    # First-difference sign; a null/non-finite sample breaks the chain so
    # the next valid sample has sign 0 (no left neighbour).
    sign = np.zeros(n, dtype=np.int8)
    pair_ok = valid[1:] & valid[:-1]
    sign[1:] = np.where(pair_ok, np.sign(vals[1:n] - vals[: n - 1]), 0).astype(
        np.int8
    )

    cand = (
        valid[1:-1]
        & (sign[1:-1] != 0)
        & (sign[2:] != 0)
        & (sign[1:-1] != sign[2:])
    )
    idx = np.flatnonzero(cand) + 1
    span = max(1, int(min_span))
    if idx.size == 0:
        return np.empty(0, dtype=np.float64)
    # Greedy keep-first: collapse extrema closer than min_span samples.
    kept: list[int] = [int(idx[0])]
    last = int(idx[0])
    for raw in idx[1:]:
        i = int(raw)
        if i - last >= span:
            kept.append(i)
            last = i
    return dep[np.asarray(kept, dtype=np.intp)].astype(np.float64, copy=False)


def snap_to_extrema_depths(
    extrema: np.ndarray,
    target_depth: float,
    *,
    tol: float,
) -> float:
    """Snap ``target_depth`` to a precomputed extrema array within ``tol``."""
    ext = np.asarray(extrema, dtype=np.float64)
    if ext.size == 0:
        return float(target_depth)
    dd = np.abs(ext - float(target_depth))
    best_i = int(np.argmin(dd))
    if float(dd[best_i]) <= float(tol):
        return float(ext[best_i])
    return float(target_depth)


def snap_depth_to_extrema(
    depth: np.ndarray,
    values: np.ndarray,
    null_mask: np.ndarray,
    target_depth: float,
    *,
    tol: float,
) -> float:
    """Snap ``target_depth`` to the nearest local extremum within ``tol``.

    Returns the extremum depth when one lies within ``tol`` of the target,
    otherwise the target unchanged.
    """
    ext = local_extrema_depths(depth, values, null_mask)
    return snap_to_extrema_depths(ext, target_depth, tol=tol)
