"""Curve-shape similarity for correlation links (FRS §3.x 曲线形态自动对比).

Refines the ``right_depth`` of name-matched horizon links by sliding
windowed normalized cross-correlation (Pearson r) between adjacent wells'
primary curves. This is the "shape" counterpart to
``correlation_links.match_tops_by_name`` (which matches by name only):
after name-matching, each link's right-depth is nudged to the lag that
best aligns the two curves' morphology around the picked tops.

Pure numpy, headless (no Qt). This is the math layer; the host collects
per-well primary curves from ``HostPresentation`` tracks and feeds them
to :func:`refine_link_depths`, persisting the result through the
existing ``_set_correlation_links`` path (no schema change - only
``right_depth`` values change).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Sequence

import numpy as np

from well_log_workstation.correlation_links import HorizonLink


@dataclass(frozen=True)
class CurveSamples:
    """A well's primary curve arrays aligned to ``depth`` (MD, float64)."""

    depth: np.ndarray
    values: np.ndarray
    null_mask: np.ndarray
    mnemonic: str = ""


def _resample_window(
    depth: np.ndarray,
    values: np.ndarray,
    mask: np.ndarray,
    center: float,
    half_window: float,
    grid_step: float,
) -> np.ndarray:
    """Linearly resample a curve onto a regular depth grid within the window.

    Samples outside the available depth range or flagged null/NaN are
    excluded from the returned grid (returned as NaN so the caller can
    mask them pairwise). Returns an array of resampled values on the
    grid ``[center-half_window .. center+half_window]`` at ``grid_step``.
    """
    dep = np.asarray(depth, dtype=np.float64)
    vals = np.asarray(values, dtype=np.float64)
    msk = np.asarray(mask, dtype=bool)
    lo = center - half_window
    hi = center + half_window
    grid = np.arange(lo, hi + 0.5 * grid_step, grid_step, dtype=np.float64)
    if dep.size < 2 or grid.size < 2:
        return np.full(grid.size, np.nan, dtype=np.float64)
    # Interpolate; samples outside [dep.min, dep.max] -> NaN via left_nan/right_nan.
    interp = np.interp(grid, dep, vals, left=np.nan, right=np.nan)
    # Mark grid points whose nearest source sample is null/NaN as NaN.
    if msk.size == dep.size:
        nearest = np.clip(np.searchsorted(dep, grid, side="right") - 1, 0, dep.size - 1)
        src_null = msk[nearest] | ~np.isfinite(vals[nearest])
        interp = np.where(src_null, np.nan, interp)
    interp = np.where(np.isfinite(interp), interp, np.nan)
    return interp


def best_lag(
    depth_l: np.ndarray,
    values_l: np.ndarray,
    mask_l: np.ndarray,
    depth_r: np.ndarray,
    values_r: np.ndarray,
    mask_r: np.ndarray,
    center_l: float,
    center_r: float,
    *,
    window: float = 20.0,
    max_lag: float = 10.0,
    grid_step: float | None = None,
) -> tuple[float, float] | None:
    """Find the depth lag that best aligns two curves around picked tops.

    Resamples both curves onto a common regular grid centered at
    ``center_l`` / ``center_r`` (± ``window`` m), then slides the right
    curve over ``[-max_lag, +max_lag]`` m and scores each offset by the
    normalized cross-correlation (Pearson r) over the overlapping,
    both-valid samples.

    Returns ``(lag_m, score)`` where ``lag_m`` is the right-curve depth
    offset (added to ``center_r`` to align with ``center_l``) and
    ``score`` is the signed Pearson r at that lag (|r| near 1 = strong
    match). Returns ``None`` when there is insufficient overlapping data
    (fewer than 5 both-valid samples, all-null window, or no curve).
    """
    dep_l = np.asarray(depth_l, dtype=np.float64)
    val_l = np.asarray(values_l, dtype=np.float64)
    msk_l = np.asarray(mask_l, dtype=bool)
    dep_r = np.asarray(depth_r, dtype=np.float64)
    val_r = np.asarray(values_r, dtype=np.float64)
    msk_r = np.asarray(mask_r, dtype=bool)
    if dep_l.size < 2 or dep_r.size < 2:
        return None
    if grid_step is None:
        # Default grid step: ~10 samples per window half-width, clamped to
        # the median of the two wells' median sample intervals.
        def _med_step(d: np.ndarray) -> float:
            diffs = np.diff(np.sort(d))
            diffs = diffs[diffs > 0]
            return float(np.median(diffs)) if diffs.size else 1.0

        grid_step = max(0.1, min(_med_step(dep_l), _med_step(dep_r)))
        grid_step = min(grid_step, window / 10.0)

    left = _resample_window(dep_l, val_l, msk_l, center_l, window, grid_step)
    # The right curve is resampled on its own grid centered at center_r;
    # a lag shifts the effective alignment by offsetting which right-grid
    # index lines up with each left-grid index.
    right = _resample_window(dep_r, val_r, msk_r, center_r, window, grid_step)
    n = left.size
    if n < 5 or right.size < 5:
        return None

    # Lag in metres -> sample shift on the (uniform) grid.
    lag_samples = int(round(max_lag / grid_step))
    if lag_samples < 1:
        return None

    best_lag_m = 0.0
    best_score = 0.0  # require positive r > 0 to beat "no change"
    found = False
    for shift in range(-lag_samples, lag_samples + 1):
        # A positive shift means the right curve is sampled deeper by
        # ``shift*step`` metres relative to the left window. The lag that
        # aligns the right marker with the left is ``right_depth = left_depth
        # + lag`` where ``lag = -shift*step`` (a deeper right sample means the
        # true marker sits shallower than the picked depth).
        if shift >= 0:
            li = slice(0, n - shift)
            ri = slice(shift, n)
        else:
            li = slice(-shift, n)
            ri = slice(0, n + shift)
        a = left[li]
        b = right[ri]
        valid = np.isfinite(a) & np.isfinite(b)
        k = int(np.count_nonzero(valid))
        if k < 5:
            continue
        aa = a[valid]
        bb = b[valid]
        sa = aa.std()
        sb = bb.std()
        if sa < 1e-12 or sb < 1e-12:
            continue  # constant segment -> undefined correlation
        r = float(np.mean((aa - aa.mean()) * (bb - bb.mean())) / (sa * sb))
        # Maximize positive correlation (shape similarity, not inversion).
        if r > best_score:
            best_score = r
            best_lag_m = -shift * grid_step
            found = True
    if not found:
        return None
    return (best_lag_m, best_score)


def refine_link_depths(
    links: Sequence[HorizonLink],
    well_curves: dict[str, CurveSamples],
    *,
    window: float = 20.0,
    max_lag: float = 10.0,
    min_score: float = 0.3,
) -> list[HorizonLink]:
    """Refine ``right_depth`` on name-matched links via curve similarity.

    For each link whose left/right wells both have a primary curve in
    ``well_curves``, runs :func:`best_lag` around the picked tops. When
    the best lag has ``abs(score) >= min_score`` and is non-zero, the
    link is rebuilt with ``right_depth = left_depth + lag`` (a positive
    lag means the right-well marker sits deeper than the left by that
    amount). Links without curves, with insufficient data, or below the
    score threshold are returned unchanged.

    Non-destructive: the input ``links`` sequence is not mutated; a new
    list is returned. ``HorizonLink`` is frozen, so refinement rebuilds
    the dataclass (preserving id/name/marker ids/color).
    """
    out: list[HorizonLink] = []
    for lk in links:
        lc = well_curves.get(lk.left_well_id)
        rc = well_curves.get(lk.right_well_id)
        if lc is None or rc is None:
            out.append(lk)
            continue
        result = best_lag(
            lc.depth, lc.values, lc.null_mask,
            rc.depth, rc.values, rc.null_mask,
            lk.left_depth, lk.right_depth,
            window=window, max_lag=max_lag,
        )
        if result is None:
            out.append(lk)
            continue
        lag_m, score = result
        if abs(score) < min_score or abs(lag_m) < 1e-6:
            out.append(lk)
            continue
        new_right = float(lk.left_depth + lag_m)
        out.append(
            HorizonLink(
                id=lk.id,
                left_well_id=lk.left_well_id,
                right_well_id=lk.right_well_id,
                name=lk.name,
                left_depth=lk.left_depth,
                right_depth=new_right,
                left_marker_id=lk.left_marker_id,
                right_marker_id=lk.right_marker_id,
                color=lk.color,
            )
        )
    return out
