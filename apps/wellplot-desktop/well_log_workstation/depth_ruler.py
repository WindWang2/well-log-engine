"""Shared depth ruler for the section / correlation canvases (FRS §3.x).

The canvases share one depth window (pan/zoom); without a scale the numbers
are only guessable from the footer note. This module provides:

* :func:`nice_depth_ticks` — tick selection on a depth-friendly nice ladder
  ({1, 2, 2.5, 5} × 10^k: 25 m, 250 m, …), headless-testable;
* :func:`format_depth_label` — tick labels trimmed to the step's precision;
* :func:`paint_depth_ruler` — the QPainter strip both canvases call, so the
  ruler follows the depth window and the vertical exaggeration exactly.

The ruler is painted into a reserved left-margin strip
(:data:`RULER_WIDTH` px) that both canvases subtract from the column layout.
"""

from __future__ import annotations

import math

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPen

# Reserved left-margin width for the depth ruler (px).
RULER_WIDTH = 52


def nice_depth_ticks(
    d0: float, d1: float, max_ticks: int = 9
) -> tuple[list[float], float]:
    """Choose tick values for a depth window ``[d0, d1]``.

    Picks the smallest step from the nice ladder {1, 2, 2.5, 5} × 10^k that
    keeps the tick count ≤ ``max_ticks`` (e.g. 0–200 m → 25 m steps, 8 ticks).

    Returns ``(ticks, step)``; ``([], 0.0)`` for a degenerate/non-finite
    window. The first tick is the first multiple of ``step`` ≥ ``d0``.
    """
    if (
        not math.isfinite(float(d0))
        or not math.isfinite(float(d1))
        or d1 <= d0
        or max_ticks < 1
    ):
        return [], 0.0
    span = d1 - d0
    raw = span / max_ticks
    mag = 10.0 ** math.floor(math.log10(raw))
    step = 10.0 * mag
    for factor in (1.0, 2.0, 2.5, 5.0):
        cand = factor * mag
        if cand >= raw:
            step = cand
            break
    first = math.ceil(d0 / step) * step
    ticks: list[float] = []
    v = first
    while v <= d1 + step * 1e-9:
        ticks.append(v)
        v += step
    return ticks, step


def format_depth_label(value: float, step: float) -> str:
    """Trim a tick label to the step's precision.

    1050 / step 25 → "1050"; 1050.5 / step 0.5 → "1050.5"; 1050.25 / step
    0.25 → "1050.25". Float drift is rounded away.
    """
    decimals = 0
    while decimals < 8 and abs(round(step, decimals) - step) > 1e-9:
        decimals += 1
    if decimals == 0:
        return f"{value:.0f}"
    text = f"{value:.{decimals}f}".rstrip("0").rstrip(".")
    return text


def paint_depth_ruler(
    p: QPainter,
    rect: QRectF,
    d0: float,
    d1: float,
    caption: str = "深度 (m)",
    *,
    vertical_exaggeration: float = 1.0,
) -> None:
    """Paint the shared-depth ruler strip into ``rect``.

    White background, tick marks + trimmed labels on the left, unit caption
    along the bottom. Tick y follows the same mapping as the curve layers —
    including the vertical exaggeration (correlation VE stretches the axis);
    ticks outside the visible band are skipped.
    """
    ticks, step = nice_depth_ticks(d0, d1)
    p.save()
    p.fillRect(rect, QColor("#ffffff"))
    if ticks and rect.height() > 0:
        ve = vertical_exaggeration if vertical_exaggeration > 0 else 1.0
        span = d1 - d0
        p.setPen(QPen(QColor("#333"), 1))
        left = int(rect.left())
        for v in ticks:
            t = (v - d0) / span
            y = rect.top() + t * rect.height() * ve
            if y < rect.top() or y > rect.bottom():
                continue
            p.drawLine(left, int(y), left + 8, int(y))
            p.drawText(
                QRectF(rect.left() + 12, y - 6, rect.width() - 14, 12),
                Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                format_depth_label(v, step),
            )
    p.setPen(QColor("#555"))
    p.drawText(
        QRectF(rect.left(), rect.bottom() - 15, rect.width(), 13),
        Qt.AlignmentFlag.AlignCenter,
        caption,
    )
    p.restore()
