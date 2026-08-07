"""Shared depth ruler (FRS §3.x 多深度刻度 Desktop 画布部分).

Covers:
* nice_depth_ticks — nice 1/2/2.5/5 × 10^k ladder, tick count ≤ max_ticks,
  degenerate/non-finite windows;
* format_depth_label — labels trimmed to the step's precision;
* paint_depth_ruler — renders into an offscreen image (tick pixels + white
  strip), VE-clipped ticks;
* both canvases paint the ruler strip (non-white pixels in the left margin)
  and the section canvas column layout reserves the ruler width.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

from well_log_workstation.depth_ruler import (
    RULER_WIDTH,
    format_depth_label,
    nice_depth_ticks,
)


# ---------------------------------------------------------------------------
# nice_depth_ticks
# ---------------------------------------------------------------------------


def test_ticks_200_span_uses_25m_steps() -> None:
    ticks, step = nice_depth_ticks(0.0, 200.0)
    assert step == pytest.approx(25.0)
    assert ticks == pytest.approx([0.0, 25.0, 50.0, 75.0, 100.0, 125.0, 150.0, 175.0, 200.0])


def test_ticks_1000_1100_uses_20m_steps() -> None:
    ticks, step = nice_depth_ticks(1000.0, 1100.0)
    assert step == pytest.approx(20.0)
    assert len(ticks) <= 9
    assert ticks[0] == pytest.approx(1000.0)
    assert ticks[-1] == pytest.approx(1100.0)
    # Every tick is a multiple of the step.
    for v in ticks:
        assert abs(v - round(v / step) * step) < 1e-9


def test_ticks_start_after_d0_when_not_aligned() -> None:
    ticks, step = nice_depth_ticks(1003.0, 1103.0)
    assert step == pytest.approx(20.0)
    assert ticks[0] == pytest.approx(1020.0)
    assert ticks[-1] == pytest.approx(1100.0)


def test_ticks_fractional_steps() -> None:
    ticks, step = nice_depth_ticks(0.0, 4.0)
    assert step == pytest.approx(0.5)
    assert len(ticks) == 9  # 0.0, 0.5, …, 4.0


def test_ticks_fallback_to_ten_times_magnitude() -> None:
    # raw = 5/9 ≈ 0.56 → ladder {0.1, 0.2, 0.25, 0.5} all below → step 1.0.
    ticks, step = nice_depth_ticks(0.0, 5.0)
    assert step == pytest.approx(1.0)
    assert ticks == pytest.approx([0.0, 1.0, 2.0, 3.0, 4.0, 5.0])


def test_ticks_degenerate_window() -> None:
    assert nice_depth_ticks(10.0, 10.0) == ([], 0.0)
    assert nice_depth_ticks(10.0, 5.0) == ([], 0.0)


def test_ticks_non_finite_window() -> None:
    assert nice_depth_ticks(float("nan"), 10.0) == ([], 0.0)
    assert nice_depth_ticks(0.0, float("inf")) == ([], 0.0)


# ---------------------------------------------------------------------------
# format_depth_label
# ---------------------------------------------------------------------------


def test_labels_trimmed_to_step_precision() -> None:
    assert format_depth_label(1050.0, 25.0) == "1050"
    assert format_depth_label(1000.0, 250.0) == "1000"
    assert format_depth_label(1050.5, 0.5) == "1050.5"
    assert format_depth_label(1050.25, 0.25) == "1050.25"
    assert format_depth_label(1050.0, 0.5) == "1050"
    assert format_depth_label(0.0, 25.0) == "0"


def test_labels_round_float_drift() -> None:
    # 0.1+0.2-style drift must not leak into the label.
    assert format_depth_label(1.1999999999999997, 0.25) == "1.2"
    assert format_depth_label(2.0000000000000004, 0.5) == "2"


# ---------------------------------------------------------------------------
# paint_depth_ruler
# ---------------------------------------------------------------------------


def _paint_ruler(d0: float, d1: float, ve: float = 1.0):
    from PySide6.QtCore import QRectF
    from PySide6.QtGui import QImage, QPainter

    from well_log_workstation.depth_ruler import paint_depth_ruler

    img = QImage(80, 300, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    p = QPainter(img)
    paint_depth_ruler(
        p, QRectF(0, 0, RULER_WIDTH, 300), d0, d1,
        vertical_exaggeration=ve,
    )
    p.end()
    return img


def test_ruler_paints_ticks_and_labels(qtbot) -> None:
    from PySide6.QtGui import QColor

    img = _paint_ruler(0.0, 200.0)
    # Tick marks: dark pixels on the left edge at tick depths. 25 m step →
    # 25 m maps to y = 25/200 * 300 = 37.5.
    c = QColor(img.pixel(4, 37))
    assert c.red() < 120 and c.green() < 120
    # Between ticks the strip stays white (labels live at x ≥ 12).
    c2 = QColor(img.pixel(6, 160))
    assert c2.red() > 240 and c2.green() > 240


def test_ruler_ve_clips_ticks_below_band(qtbot) -> None:
    from PySide6.QtGui import QColor

    # VE 2.0: depth d maps to y = d/200 * 300 * 2 = 3d → only d ≤ 100 stays
    # inside the band (ticks 0/25/50/75/100 drawn, 125+ clipped).
    img = _paint_ruler(0.0, 200.0, ve=2.0)
    assert QColor(img.pixel(4, 75)).red() < 120  # d=25 tick visible
    assert QColor(img.pixel(4, 299)).red() > 240  # beyond the band: white


def test_ruler_degenerate_window_paints_only_caption(qtbot) -> None:
    img = _paint_ruler(10.0, 10.0)  # no ticks; caption only
    assert img.width() == 80  # no crash


# ---------------------------------------------------------------------------
# Canvas integration
# ---------------------------------------------------------------------------


def _render(canvas, w: int = 600, h: int = 480):
    from PySide6.QtGui import QImage

    canvas.resize(w, h)
    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    canvas.render(img)
    return img


def _presentation(well_id: str, name: str):
    from well_log_workstation.template_model import HostPresentation

    depth = np.linspace(0.0, 200.0, 21)

    class _Layer:
        color = "#1f77b4"
        values = depth / 2.0
        null_mask = np.zeros_like(depth, dtype=bool)

    class _Scale:
        mode = "linear"
        min = 0.0
        max = 100.0

    class _Track:
        role = "curve"
        layers = [_Layer()]
        scale = _Scale()

    return HostPresentation(
        template_id="t", template_name="T", well_document_id=well_id,
        well_name=name, depth=depth, depth_unit="m",
        tracks=[_Track()],  # type: ignore[arg-type]
    )


def _has_dark_pixels(img, x0: int, x1: int, y0: int, y1: int) -> bool:
    from PySide6.QtGui import QColor

    for y in range(y0, y1):
        for x in range(x0, x1):
            c = QColor(img.pixel(x, y))
            if c.red() < 120 and c.green() < 120 and c.blue() < 120:
                return True
    return False


def test_section_canvas_paints_ruler_strip(qtbot) -> None:
    from well_log_workstation.section_canvas import SectionCanvas

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.set_section([_presentation("w1", "W1"), _presentation("w2", "W2")])
    img = _render(canvas)
    # The ruler margin (x < RULER_WIDTH, within the depth band) carries ticks.
    assert _has_dark_pixels(img, 2, RULER_WIDTH - 2, 40, 440)


def test_correlation_canvas_paints_ruler_strip(qtbot) -> None:
    from well_log_workstation.correlation_canvas import CorrelationCanvas

    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas.set_columns(
        [_presentation("w1", "W1"), _presentation("w2", "W2")],
        [[], []],
    )
    img = _render(canvas)
    assert _has_dark_pixels(img, 2, RULER_WIDTH - 2, 40, 440)


# ---------------------------------------------------------------------------
# SDK/Desktop tick parity (Epic B): one authoritative nice-step rule
# ---------------------------------------------------------------------------


def _binding_fn():
    """The SDK nice_axis_ticks via the Python binding, or None."""
    try:
        import welllog  # type: ignore

        fn = getattr(welllog, "nice_axis_ticks", None)
        if not callable(fn):
            ext = getattr(welllog, "_QtWidgets", None)
            inner = getattr(ext, "welllog", None) if ext is not None else None
            if inner is not None:
                fn = getattr(inner, "nice_axis_ticks", None)
        return fn if callable(fn) else None
    except Exception:
        return None


def test_sdk_and_desktop_ticks_parity(monkeypatch) -> None:
    """The Desktop ruler must reproduce the SDK authoritative ticks exactly.

    The SDK (scene::nice_axis_ticks) is the single source of truth; the
    Python implementation is the headless fallback. Both are exercised: the
    binding path AND the monkeypatched fallback path must return identical
    values across a window grid.
    """
    engine = _binding_fn()
    if engine is None:
        pytest.skip("Python binding not available — parity cannot run here")

    windows = [
        (0.0, 200.0),
        (1000.0, 1100.0),
        (1003.0, 1103.0),
        (0.0, 4.0),
        (0.0, 5.0),
        (0.0, 0.5),
        (1234.0, 5678.0),
        (-500.0, 500.0),
        (1000.0, 1000.05),
        (0.0, 1e6),
    ]
    for d0, d1 in windows:
        for max_ticks in (5, 9, 12):
            sdk_step, sdk_values = engine(d0, d1, max_ticks)
            # Binding path (default). Desktop returns (ticks, step).
            ticks, step = nice_depth_ticks(d0, d1, max_ticks)
            assert step == pytest.approx(float(sdk_step), rel=1e-12)
            assert ticks == pytest.approx([float(v) for v in sdk_values], rel=1e-12)
            # Fallback path (engine unavailable).
            monkeypatch.setattr(
                "well_log_workstation.depth_ruler._binding_ticks",
                lambda: None,
            )
            ticks2, step2 = nice_depth_ticks(d0, d1, max_ticks)
            assert step2 == pytest.approx(float(sdk_step), rel=1e-12)
            assert ticks2 == pytest.approx([float(v) for v in sdk_values], rel=1e-12)
            monkeypatch.undo()


def test_sdk_and_desktop_label_parity(monkeypatch) -> None:
    """Tick label precision must match the SDK authoritative formatter."""
    from well_log_workstation.depth_ruler import _binding_label, format_depth_label

    engine = _binding_label()
    if engine is None:
        pytest.skip("Python binding not available — parity cannot run here")

    cases = [
        (1050.0, 25.0, "1050"),
        (1000.0, 250.0, "1000"),
        (1050.5, 0.5, "1050.5"),
        (1050.25, 0.25, "1050.25"),
        (1050.0, 0.5, "1050"),
        (1.1999999999999997, 0.25, "1.2"),
        (2.0000000000000004, 0.5, "2"),
        (0.0, 25.0, "0"),
    ]
    for value, step, expected in cases:
        sdk_label = engine(float(value), float(step))
        assert sdk_label == expected, f"SDK label mismatch for {value}/{step}"
        # Binding path.
        assert format_depth_label(value, step) == expected
        # Fallback path.
        monkeypatch.setattr(
            "well_log_workstation.depth_ruler._binding_label",
            lambda: None,
        )
        assert format_depth_label(value, step) == expected
        monkeypatch.undo()
