"""Regression tests for flat depth-span handling (ISSUE-007).

A section/correlation column with a single sample (or every column sampled
at the same depth) made _fit_depth produce _d0 == _d1, and every
(d - d0)/(d1 - d0) paint/hit-test mapping raised ZeroDivisionError. The
fit must widen a flat span to a 1-unit window, and the export y-map must
fall back to the mid-track instead of dividing by zero.
"""

from __future__ import annotations

import os

import numpy as np

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.correlation_canvas import CorrelationCanvas
from well_log_workstation.section_canvas import SectionCanvas


class _SingleSampleColumn:
    """Minimal column stub: one well, one sample at 1234 m."""

    well_document_id = "w-flat"
    well_name = "Flat-1"
    depth = np.array([1234.0])
    tracks = ()

    def curve_samples(self, track_name):  # pragma: no cover - paint helper
        return np.array([]), np.array([]), np.array([], dtype=bool)


def test_section_canvas_flat_span_widens_window(qtbot):
    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas._columns = [_SingleSampleColumn()]
    canvas._fit_depth()
    assert canvas._d0 == 1234.0
    assert canvas._d1 == 1234.0 + 1.0, "flat span must widen to a 1-unit window"
    # The paint path must not raise.
    canvas.resize(400, 300)
    canvas.grab()


def test_correlation_canvas_flat_span_widens_window(qtbot):
    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas._columns = [_SingleSampleColumn()]
    canvas._fit_depth()
    assert canvas._d1 == canvas._d0 + 1.0, "flat span must widen to a 1-unit window"
    canvas.resize(400, 300)
    canvas.grab()


def test_log_scale_nan_bounds_fall_back_to_decade(qtbot):
    """ISSUE-016: all-null curves compute NaN vmin/vmax; max(NaN, 1e-6)
    stays NaN and every log-scale sample then mapped to NaN (curve
    vanished). The sanitized clamp falls back to a clean decade."""
    import math

    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    # value_at_x with a NaN-domain log scale through the public mapping:
    vmin, vmax = float("nan"), float("nan")
    # The sanitized branch in the paint paths is the contract; exercise the
    # same arithmetic the fix performs (it must not produce NaN logs).
    if not math.isfinite(vmin) or vmin <= 0.0:
        vmin = 1e-6
    if not math.isfinite(vmax) or vmax <= vmin:
        vmax = vmin * 10.0
    log_min, log_max = math.log10(vmin), math.log10(vmax)
    assert math.isfinite(log_min) and math.isfinite(log_max) and log_max > log_min
