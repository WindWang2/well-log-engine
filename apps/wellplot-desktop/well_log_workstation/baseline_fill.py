"""Baseline fill geometry for curve tracks (FRS §2.x 基线充填, e.g. GR>80).

Builds the filled polygons between a curve and the track's right edge where
the value is above (or below) a threshold — classic wireline-log rendering.
Pure geometry over QPolygonF (no QPainter), so it is headless-testable and
shared by all four render sites (multi-track / correlation / section /
export).
"""

from __future__ import annotations

from typing import Callable

import numpy as np
from PySide6.QtCore import QPointF
from PySide6.QtGui import QPolygonF


def baseline_fill_polygons(
    x_of: Callable[[float], float],
    y_of: Callable[[float], float],
    right_x: float,
    depth: np.ndarray,
    d0: float,
    d1: float,
    values: np.ndarray,
    null_mask: np.ndarray,
    *,
    step: int,
    threshold: float,
    direction: str = "above",
) -> list[QPolygonF]:
    """Polygons filling the area where the curve passes the threshold.

    Walks the samples with the same ``step`` decimation as the curve paint
    loop and groups contiguous qualifying samples into one polygon per run:
    the curve points forward, then the right-edge points (same y) reversed
    to close the strip. Null samples / out-of-window depths break runs.
    Returns an empty list when nothing qualifies.
    """
    vals = np.asarray(values, dtype=np.float64)
    mask = np.asarray(null_mask, dtype=bool)
    dep = np.asarray(depth, dtype=np.float64)
    n = min(dep.size, vals.size, mask.size)
    if n < 2 or threshold is None:
        return []
    th = float(threshold)
    above = direction != "below"
    step = max(1, int(step))

    out: list[QPolygonF] = []
    run: list[QPointF] = []  # curve points of the current run
    run_ys: list[float] = []  # parallel y values (for the right edge)
    for i in range(0, n, step):
        if bool(mask[i]):
            run, run_ys = _flush(out, run, run_ys, x_of, right_x)
            continue
        d = float(dep[i])
        if d < d0 or d > d1:
            run, run_ys = _flush(out, run, run_ys, x_of, right_x)
            continue
        v = float(vals[i])
        if not np.isfinite(v):
            run, run_ys = _flush(out, run, run_ys, x_of, right_x)
            continue
        qualifies = v > th if above else v < th
        yy = y_of(d)
        if not np.isfinite(yy):
            run, run_ys = _flush(out, run, run_ys, x_of, right_x)
            continue
        if not qualifies:
            run, run_ys = _flush(out, run, run_ys, x_of, right_x)
            continue
        xx = x_of(v)
        if not np.isfinite(xx):
            run, run_ys = _flush(out, run, run_ys, x_of, right_x)
            continue
        run.append(QPointF(xx, yy))
        run_ys.append(float(yy))
    _flush(out, run, run_ys, x_of, right_x)
    return out


def _flush(
    out: list[QPolygonF],
    run: list[QPointF],
    run_ys: list[float],
    x_of: Callable[[float], float],
    right_x: float,
) -> tuple[list[QPointF], list[float]]:
    """Close the current run into a polygon (curve pts + right-edge pts)."""
    if len(run) < 2:
        return [], []
    poly = QPolygonF()
    for pt in run:
        poly.append(pt)
    for yy in reversed(run_ys):
        poly.append(QPointF(right_x, yy))
    out.append(poly)
    return [], []
