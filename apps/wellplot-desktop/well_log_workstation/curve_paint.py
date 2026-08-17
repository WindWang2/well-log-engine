"""Peak-preserving curve stroke shared by canvas and Qt export.

Block min/max decimation keeps thin-bed spikes that uniform stride sampling
drops (#494 / #595). The visible depth window is sliced with searchsorted
and block extrema are computed with numpy so a pan/zoom paint is O(window)
rather than a Python walk of the full array (#596).
"""

from __future__ import annotations

import math

import numpy as np
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen


TARGET_STROKE_POINTS = 2000


def visible_index_window(
    depth: np.ndarray,
    d0: float,
    d1: float,
) -> tuple[int, int]:
    """Return ``[i0, i1)`` covering samples that can affect ``[d0, d1]``.

    Assumes a non-decreasing depth axis (standard LAS). One sample of pad
    is kept on each side so a stroke that crosses the window edge stays
    connected. Unsorted / descending axes fall back to the full range.
    """
    n = int(depth.size)
    if n == 0:
        return 0, 0
    if n == 1 or not (math.isfinite(d0) and math.isfinite(d1)):
        return 0, n
    if float(depth[0]) > float(depth[-1]):
        return 0, n
    i0 = int(np.searchsorted(depth, d0, side="left"))
    i1 = int(np.searchsorted(depth, d1, side="right"))
    return max(0, i0 - 1), min(n, i1 + 1)


def _map_xy(
    depth: np.ndarray,
    values: np.ndarray,
    *,
    d0: float,
    d1: float,
    vmin: float,
    vmax: float,
    mode: str,
    wrap: bool,
    reverse: bool,
    x0: float,
    y0: float,
    tw: float,
    th: float,
) -> tuple[np.ndarray, np.ndarray]:
    if mode == "log":
        vmin = max(vmin, 1e-6)
        vmax = max(vmax, vmin * 10)
        log_min, log_max = math.log10(vmin), math.log10(vmax)
        with np.errstate(divide="ignore", invalid="ignore"):
            t = (np.log10(values) - log_min) / (log_max - log_min)
    else:
        span = vmax - vmin
        if span > 0:
            t = (values - vmin) / span
        else:
            t = np.full(values.shape, 0.5, dtype=np.float64)
    if wrap:
        t = t - np.floor(t)
    else:
        t = np.clip(t, 0.0, 1.0)
    if reverse:
        t = 1.0 - t
    xx = x0 + t * tw
    denom = d1 - d0
    if denom == 0:
        yy = np.full(depth.shape, y0, dtype=np.float64)
    else:
        yy = y0 + ((depth - d0) / denom) * th
    return xx, yy


def curve_stroke_vertices(
    depth: np.ndarray,
    values: np.ndarray,
    null_mask: np.ndarray | None,
    d0: float,
    d1: float,
    *,
    vmin: float,
    vmax: float,
    mode: str = "linear",
    wrap: bool = False,
    reverse: bool = False,
    x0: float = 0.0,
    y0: float = 0.0,
    tw: float = 1.0,
    th: float = 1.0,
    target_points: int = TARGET_STROKE_POINTS,
) -> tuple[list[tuple[float, float, int, bool]], int]:
    """Decimate ``depth``/``values`` to peak-preserving stroke vertices.

    Returns ``(vertices, window_size)`` where each vertex is
    ``(x, y, global_index, start_new_subpath)`` and ``window_size`` is the
    number of samples examined after viewport slicing.
    """
    depth = np.asarray(depth, dtype=np.float64)
    values = np.asarray(values, dtype=np.float64)
    n = min(depth.size, values.size)
    if null_mask is not None:
        null_mask = np.asarray(null_mask, dtype=bool)
        n = min(n, null_mask.size)
    if n < 2:
        return [], n
    depth = depth[:n]
    values = values[:n]
    mask = None if null_mask is None else null_mask[:n]

    i0, i1 = visible_index_window(depth, d0, d1)
    if i1 - i0 < 2:
        i0, i1 = 0, n
    d = depth[i0:i1]
    v = values[i0:i1]
    win_n = int(d.size)
    if win_n < 2:
        return [], win_n

    valid = np.isfinite(d) & np.isfinite(v)
    if mask is not None:
        valid &= ~mask[i0:i1]
    if mode == "log":
        valid &= v > 0

    xx, yy = _map_xy(
        d,
        v,
        d0=d0,
        d1=d1,
        vmin=vmin,
        vmax=vmax,
        mode=mode,
        wrap=wrap,
        reverse=reverse,
        x0=x0,
        y0=y0,
        tw=tw,
        th=th,
    )
    valid &= np.isfinite(xx) & np.isfinite(yy)

    invalid_global = np.flatnonzero(~valid) + i0
    step = max(1, win_n // max(1, int(target_points)))
    n_blocks = (win_n + step - 1) // step
    pad = n_blocks * step - win_n
    work = np.empty(n_blocks * step, dtype=np.float64)
    work[:win_n] = np.where(valid, v, np.nan)
    if pad:
        work[win_n:] = np.nan
    blocks = work.reshape(n_blocks, step)
    all_nan = np.isnan(blocks).all(axis=1)
    filled_min = np.where(np.isnan(blocks), np.inf, blocks)
    filled_max = np.where(np.isnan(blocks), -np.inf, blocks)
    argmin = np.argmin(filled_min, axis=1)
    argmax = np.argmax(filled_max, axis=1)

    out: list[tuple[float, float, int, bool]] = []
    prev_idx: int | None = None
    for b in range(n_blocks):
        if bool(all_nan[b]):
            prev_idx = None
            continue
        pair = {int(argmin[b]), int(argmax[b])}
        locals_sorted = sorted(pair)
        for local in locals_sorted:
            gi = i0 + b * step + local
            if gi >= i1:
                continue
            li = gi - i0
            start = prev_idx is None
            if prev_idx is not None and gi > prev_idx + 1 and invalid_global.size:
                lo = int(np.searchsorted(invalid_global, prev_idx, side="right"))
                hi = int(np.searchsorted(invalid_global, gi, side="left"))
                if hi > lo:
                    start = True
            out.append((float(xx[li]), float(yy[li]), gi, start))
            prev_idx = gi
    return out, win_n


def paint_curve(
    painter: QPainter,
    x0: float,
    y0: float,
    tw: float,
    th: float,
    depth: np.ndarray,
    d0: float,
    d1: float,
    values: np.ndarray,
    null_mask: np.ndarray | None,
    vmin: float,
    vmax: float,
    mode: str,
    color: QColor,
    wrap: bool = False,
    reverse: bool = False,
    *,
    pen_width: float = 1.5,
    target_points: int = TARGET_STROKE_POINTS,
) -> None:
    """Stroke one curve with peak-preserving, viewport-windowed decimation."""
    n = min(
        np.asarray(depth).size,
        np.asarray(values).size,
        np.asarray(null_mask).size if null_mask is not None else np.asarray(values).size,
    )
    if n < 2 or tw < 4 or th < 4:
        return
    verts, _win = curve_stroke_vertices(
        depth,
        values,
        null_mask,
        d0,
        d1,
        vmin=vmin,
        vmax=vmax,
        mode=mode,
        wrap=wrap,
        reverse=reverse,
        x0=x0,
        y0=y0,
        tw=tw,
        th=th,
        target_points=target_points,
    )
    if not verts:
        return
    path = QPainterPath()
    for xx, yy, _i, start in verts:
        if start:
            path.moveTo(xx, yy)
        else:
            path.lineTo(xx, yy)
    painter.setPen(QPen(color, pen_width))
    painter.drawPath(path)
