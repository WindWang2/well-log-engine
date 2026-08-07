"""Crossover fill geometry (FRS §2.x 双曲线交叉充填).

Fills the enclosed region between two curve layers of the same track where
the upper layer's mapped track-x lies to the right of the lower layer's —
the SDK ``CrossoverFillLayerSpec`` / ``upper_minus_lower`` rule, host-side.
Curves may use different per-layer scales (dual-track / 对道).

Pure geometry over QPolygonF (no QPainter in the builder), shared by all
four render sites (multi-track / correlation / section / export).
"""

from __future__ import annotations

from typing import Callable

import numpy as np
from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QColor, QPainter, QPolygonF

from well_log_workstation.template_model import BoundTrack, ScaleSpec


def crossover_fill_polygons(
    x_upper_of: Callable[[float], float],
    x_lower_of: Callable[[float], float],
    y_of: Callable[[float], float],
    depth: np.ndarray,
    d0: float,
    d1: float,
    upper_vals: np.ndarray,
    upper_mask: np.ndarray,
    lower_vals: np.ndarray,
    lower_mask: np.ndarray,
    *,
    step: int,
) -> list[QPolygonF]:
    """Polygons for the enclosed region where upper x > lower x.

    Walks samples with the same ``step`` decimation as the curve paint loop;
    a run of samples where the upper layer's mapped x is strictly to the
    right of the lower's becomes one polygon (upper points forward, lower
    points reversed). Null / out-of-window / non-finite samples on either
    side break runs. Returns an empty list when nothing qualifies.
    """
    u_vals = np.asarray(upper_vals, dtype=np.float64)
    u_mask = np.asarray(upper_mask, dtype=bool)
    l_vals = np.asarray(lower_vals, dtype=np.float64)
    l_mask = np.asarray(lower_mask, dtype=bool)
    dep = np.asarray(depth, dtype=np.float64)
    n = min(dep.size, u_vals.size, l_vals.size)
    if n < 2:
        return []
    step = max(1, int(step))

    out: list[QPolygonF] = []
    run: list[QPointF] = []  # upper curve points
    run_lower: list[QPointF] = []  # parallel lower points
    for i in range(0, n, step):
        if bool(u_mask[i]) or bool(l_mask[i]):
            _flush(out, run, run_lower)
            continue
        d = float(dep[i])
        if d < d0 or d > d1:
            _flush(out, run, run_lower)
            continue
        u = float(u_vals[i])
        l = float(l_vals[i])
        if not (np.isfinite(u) and np.isfinite(l)):
            _flush(out, run, run_lower)
            continue
        xu = x_upper_of(u)
        xl = x_lower_of(l)
        yy = y_of(d)
        if not (np.isfinite(xu) and np.isfinite(xl) and np.isfinite(yy)):
            _flush(out, run, run_lower)
            continue
        if xu <= xl:
            _flush(out, run, run_lower)
            continue
        run.append(QPointF(xu, yy))
        run_lower.append(QPointF(xl, yy))
    _flush(out, run, run_lower)
    return out


def _flush(
    out: list[QPolygonF], run: list[QPointF], run_lower: list[QPointF]
) -> None:
    """Close the current run: upper points forward, lower points reversed."""
    if len(run) < 2 or len(run_lower) < 2:
        run.clear()
        run_lower.clear()
        return
    poly = QPolygonF()
    for pt in run:
        poly.append(pt)
    for pt in reversed(run_lower):
        poly.append(pt)
    out.append(poly)
    run.clear()
    run_lower.clear()


def _layer_x_map(
    x0: float,
    tw: float,
    scale: ScaleSpec | None,
    fallback: ScaleSpec | None,
):
    """Value→pixel x closure for a layer (per-layer scale or track fallback)."""
    import math

    eff = scale or fallback
    vmin = eff.min if eff else 0.0
    vmax = eff.max if eff else 100.0
    mode = eff.mode if eff else "linear"
    if mode == "log":
        vmin = max(vmin, 1e-6)
        vmax = max(vmax, vmin * 10)
        log_min, log_max = math.log10(vmin), math.log10(vmax)
        wrap = bool(getattr(eff, "wrap", False))
        reverse = bool(getattr(eff, "reverse", False))

        def x_map(v: float) -> float:
            if v <= 0 or not math.isfinite(v):
                return float("nan")
            t = (math.log10(v) - log_min) / (log_max - log_min)
            if wrap:
                t = t - math.floor(t)
            else:
                t = max(0.0, min(1.0, t))
            if reverse:
                t = 1.0 - t  # FRS §2.x 反向刻度: scale runs right->left
            return x0 + t * tw

        return x_map
    wrap = bool(getattr(eff, "wrap", False))
    reverse = bool(getattr(eff, "reverse", False))

    def x_map(v: float) -> float:
        if not math.isfinite(v):
            return float("nan")
        t = (v - vmin) / (vmax - vmin) if vmax > vmin else 0.5
        if wrap:
            t = t - math.floor(t)
        else:
            t = max(0.0, min(1.0, t))
        if reverse:
            t = 1.0 - t  # FRS §2.x 反向刻度: scale runs right->left
        return x0 + t * tw

    return x_map


def paint_crossover_fill(
    painter: QPainter,
    track: BoundTrack,
    x: int,
    tw: int,
    top: int,
    bottom: int,
    d0: float,
    d1: float,
    depth: np.ndarray,
) -> None:
    """Paint the crossover fill for a dual-curve track (layers ≥ 2 required).

    Fills where layers[0]'s mapped x is to the right of layers[1]'s, using
    each layer's own scale (per-layer or track fallback). No-op unless the
    track scale has ``crossover_fill`` set and the track has ≥2 layers.
    """
    if len(track.layers) < 2 or track.scale is None:
        return
    if not bool(getattr(track.scale, "crossover_fill", False)):
        return
    upper = track.layers[0]
    lower = track.layers[1]
    if upper.values is None or lower.values is None:
        return
    n = min(
        depth.size,
        np.asarray(upper.values).size,
        np.asarray(lower.values).size,
    )
    if n < 2 or tw < 4 or (bottom - top) < 4:
        return
    span = d1 - d0
    if span <= 0:
        return

    def y_of(d: float) -> float:
        return top + ((d - d0) / span) * (bottom - top)

    x0 = float(x)
    twf = float(tw)
    x_upper = _layer_x_map(x0, twf, upper.scale, track.scale)
    x_lower = _layer_x_map(x0, twf, lower.scale, track.scale)
    step = max(1, n // 2000)
    polys = crossover_fill_polygons(
        x_upper,
        x_lower,
        y_of,
        depth,
        d0,
        d1,
        np.asarray(upper.values, dtype=np.float64),
        np.asarray(upper.null_mask, dtype=bool),
        np.asarray(lower.values, dtype=np.float64),
        np.asarray(lower.null_mask, dtype=bool),
        step=step,
    )
    if not polys:
        return
    color_hex = str(getattr(track.scale, "crossover_color", "") or "")
    fill = QColor(color_hex) if color_hex else QColor(lower.color)
    fill.setAlpha(96)
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(fill)
    for poly in polys:
        painter.drawPolygon(poly)
    painter.setBrush(Qt.BrushStyle.NoBrush)
