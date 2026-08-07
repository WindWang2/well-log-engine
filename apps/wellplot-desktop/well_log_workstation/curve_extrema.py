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

    # First difference sign; treat null/non-finite as breaks.
    sign = np.zeros(n, dtype=np.int8)
    prev_valid: float | None = None
    for i in range(n):
        if mask[i] or not np.isfinite(vals[i]):
            prev_valid = None
            sign[i] = 0
            continue
        if prev_valid is not None:
            diff = float(vals[i]) - prev_valid
            sign[i] = 1 if diff > 0 else (-1 if diff < 0 else 0)
        prev_valid = float(vals[i])

    out: list[float] = []
    last_idx = -min_span - 1
    for i in range(1, n - 1):
        if mask[i] or not np.isfinite(vals[i]):
            continue
        # Sign change across sample i: s(i-1) != 0, s(i+1) != 0, differ.
        s_prev = int(sign[i])
        s_next = int(sign[i + 1]) if i + 1 < n else 0
        if s_prev == 0 or s_next == 0 or s_prev == s_next:
            continue
        if i - last_idx < min_span:
            continue
        out.append(float(dep[i]))
        last_idx = i
    return np.asarray(out, dtype=np.float64)


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
    if ext.size == 0:
        return float(target_depth)
    dd = np.abs(ext - float(target_depth))
    best = float(np.min(dd))
    if best <= float(tol):
        return float(ext[int(np.argmin(dd))])
    return float(target_depth)
