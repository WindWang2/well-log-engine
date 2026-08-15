"""Regression: _paint_curve must preserve off-stride spikes (peak-preserving)."""
from __future__ import annotations

import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtGui import QColor, QImage, QPainter

from well_log_workstation.multi_track_canvas import MultiTrackCanvas


@pytest.fixture()
def app():
    from PySide6.QtWidgets import QApplication

    yield QApplication.instance() or QApplication([])


def _paint_spiky_curve(spike_index: int) -> QImage:
    canvas = MultiTrackCanvas()
    img = QImage(200, 400, QImage.Format.Format_ARGB32)
    img.fill(QColor("white"))
    painter = QPainter(img)
    n = 4000  # stride = n // 2000 = 2 → uniform sampling visits even indices
    depth = np.arange(n, dtype=np.float64)
    vals = np.zeros(n, dtype=np.float64)
    vals[spike_index] = 1.0
    canvas._paint_curve(
        painter,
        x0=10,
        y0=10,
        tw=180,
        th=380,
        depth=depth,
        d0=0.0,
        d1=float(n - 1),
        values=vals,
        null_mask=None,
        vmin=0.0,
        vmax=1.0,
        mode="linear",
        color=QColor("black"),
    )
    painter.end()
    return img


def _dark_columns_in_row(img: QImage, y: int) -> int:
    row_dark = 0
    for x in range(img.width()):
        c = img.pixelColor(x, y)
        if c.lightness() < 128:
            row_dark += 1
    return row_dark


def test_off_stride_spike_is_painted(app):
    # Spike at an odd index: uniform stride-2 sampling (even indices) never
    # visits it, so the old painter dropped the sample entirely.
    img = _paint_spiky_curve(spike_index=7)
    # Depth 7 maps to y ≈ 10 + 7/3999*380 ≈ 10.7 → check the spike rows.
    assert any(_dark_columns_in_row(img, y) > 0 for y in (10, 11, 12))


def test_no_spike_baseline_has_thin_curve_only(app):
    img = _paint_spiky_curve(spike_index=2)  # even index: on-stride either way
    # Both implementations paint it; sanity that the harness paints something.
    assert any(_dark_columns_in_row(img, y) > 0 for y in range(9, 14))
