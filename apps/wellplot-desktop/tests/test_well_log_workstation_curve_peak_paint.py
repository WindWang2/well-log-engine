"""Regression: _paint_curve must preserve off-stride spikes (peak-preserving)
and break the stroke across null/out-of-range samples, never bridging gaps."""
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


def _dark_in_region(img: QImage, x_lo: int, x_hi: int, y_lo: int, y_hi: int) -> bool:
    for y in range(y_lo, y_hi):
        for x in range(x_lo, x_hi):
            if img.pixelColor(x, y).lightness() < 128:
                return True
    return False


def test_off_stride_spike_is_painted(app):
    # Spike at an odd index: uniform stride-2 sampling (even indices) never
    # visits it, so the old painter dropped the sample entirely.
    img = _paint_spiky_curve(spike_index=7)
    # v=1.0 with vmin=0/vmax=1 maps to x = x0 + 1.0*tw = 190 (right edge),
    # rasterized to columns ≈189-190 by the 1.5px pen; depth 7 maps to
    # y ≈ 10.7 → rows 9-13. The old painter only drew the v=0 baseline at
    # x≈10, so asserting the right-edge columns fails on it.
    assert any(
        img.pixelColor(x, y).lightness() < 128
        for x in range(186, 191)
        for y in range(9, 14)
    )


def test_no_spike_baseline_has_thin_curve_only(app):
    img = _paint_spiky_curve(spike_index=2)  # even index: on-stride either way
    # Both implementations paint it; sanity that the harness paints something.
    assert any(_dark_columns_in_row(img, y) > 0 for y in range(9, 14))


def _paint_curve_with_null_gap() -> QImage:
    canvas = MultiTrackCanvas()
    img = QImage(200, 400, QImage.Format.Format_ARGB32)
    img.fill(QColor("white"))
    painter = QPainter(img)
    n = 4000  # stride = n // 2000 = 2 → blocks cover two samples each
    depth = np.arange(n, dtype=np.float64)
    vals = np.zeros(n, dtype=np.float64)
    vals[103] = 1.0  # peak on the far side of the null gap
    null = np.zeros(n, dtype=bool)
    null[101:103] = True  # null gap between emitted samples 100 and 103
    canvas._paint_curve(
        painter,
        x0=10,
        y0=10,
        tw=180,
        th=380,
        depth=depth,
        d0=100.0,
        d1=104.0,
        values=vals,
        null_mask=null,
        vmin=0.0,
        vmax=1.0,
        mode="linear",
        color=QColor("black"),
    )
    painter.end()
    return img


def test_null_gap_breaks_stroke(app):
    # Depth 100..104 maps to y = 10 + (d-100)/4*380; emitted points are
    # (10,10)@100, (190,295)@103, (10,390)@104. Null samples 101-102 sit
    # strictly between the first two emitted points, so the peak-preserving
    # painter must break the stroke — bridging the gap would draw the
    # (10,10)→(190,295) diagonal through rows ≈60-200. With the depth axis
    # panned to the gap (d0=100, d1=104) those rows must stay clean.
    img = _paint_curve_with_null_gap()
    assert not _dark_in_region(img, 0, img.width(), 60, 200)
    # Sanity: the return stroke (190,295)→(10,390) still renders below the gap.
    assert _dark_in_region(img, 0, img.width(), 294, 300)
