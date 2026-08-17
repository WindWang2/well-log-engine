"""Section print preview: SectionCanvas.render_to depth windows + preview flow.

Covers the print-preview gap for sections (single-well + correlation were
already supported):
* SectionCanvas.render_to paints into an arbitrary rect and honors a
  depth window (pixel-different output per window, geometry clipped);
* open_print_preview gains a section branch (landscape, viewport depth);
* _paint_active_plot's section branch threads depth_range.
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QRectF
from PySide6.QtGui import QImage, QPainter

from well_log_workstation.section_canvas import SectionCanvas
from well_log_workstation.section_geometry import SectionFault2D, TieQuad2D
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.template_model import HostPresentation


def _quad(x: float, top: float, bottom: float) -> TieQuad2D:
    return TieQuad2D(
        corners=np.array(
            [[0.0, top], [x, top], [x, bottom], [0.0, bottom]]
        ),
        fill_color="#93c5fd",
        pattern_id=None,
    )


def _presentation(well_id: str, name: str) -> HostPresentation:
    depth = np.array([1000.0, 1100.0])

    class _Layer:
        color = "#1f77b4"
        values = np.array([10.0, 30.0])
        null_mask = np.array([False, False])

    class _Scale:
        mode = "linear"
        min = 0.0
        max = 100.0

    class _Track:
        role = "curve"
        layers = [_Layer()]
        scale = _Scale()

    return HostPresentation(
        template_id="t",
        template_name="T",
        well_document_id=well_id,
        well_name=name,
        depth=depth,
        depth_unit="m",
        tracks=[_Track()],  # type: ignore[arg-type]
    )


def _section_canvas(qtbot) -> SectionCanvas:
    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)
    quad = _quad(1.0, 1005.0, 1095.0)
    fault = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000,
        bottom_depth=1100, throw=20.0,
    )
    canvas.set_section(
        [_presentation("w1", "W1"), _presentation("w2", "W2")],
        [[], []],
        faults=[fault],
        tie_quads=[quad],
    )
    return canvas


def _render(canvas: SectionCanvas, rect: QRectF, depth_range=None) -> QImage:
    img = QImage(600, 480, QImage.Format.Format_ARGB32)
    img.fill(0)
    painter = QPainter(img)
    try:
        canvas.render_to(painter, rect, depth_range=depth_range)
    finally:
        painter.end()
    return img


def test_render_to_smoke(qtbot) -> None:
    canvas = _section_canvas(qtbot)
    img = _render(canvas, QRectF(0, 0, 600, 480))
    assert not img.isNull()


def test_render_to_honors_depth_window(qtbot, pixel_bytes) -> None:
    canvas = _section_canvas(qtbot)
    rect = QRectF(0, 0, 600, 480)
    # The tie quad spans 1005–1095: an upper window and a lower window must
    # produce visibly different pages (quad painted in different positions).
    upper = _render(canvas, rect, depth_range=(1000.0, 1050.0))
    lower = _render(canvas, rect, depth_range=(1050.0, 1100.0))
    assert upper.size() == lower.size()
    assert pixel_bytes(upper) != pixel_bytes(lower)
    # A window entirely below the quad (1090–1100 barely touches its edge)
    # renders differently from the full fit — geometry is windowed/clipped.
    deep = _render(canvas, rect, depth_range=(1095.0, 1100.0))
    assert deep.size() == upper.size()
    assert pixel_bytes(deep) != pixel_bytes(upper)


def test_render_to_restores_viewport(qtbot) -> None:
    canvas = _section_canvas(qtbot)
    canvas.set_depth_range(1000.0, 1100.0)
    _render(canvas, QRectF(0, 0, 600, 480), depth_range=(1000.0, 1020.0))
    # The interactive viewport must be untouched after the export.
    assert canvas.depth_range() == (1000.0, 1100.0)


def test_open_print_preview_section_branch(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws", name="Sec")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win._active_plot_type = "section"
    win.section_canvas.set_section(
        [_presentation("w1", "W1"), _presentation("w2", "W2")]
    )

    info = win.open_print_preview(show=False)
    assert info is not None
    assert info.plot_name == "油藏剖面"
    assert info.orientation == "landscape"
    assert info.page_count == 1  # whole depth on one page by default
    assert info.depth_bottom > info.depth_top
    assert info.depth_unit == "m"


def test_paint_active_plot_section_depth_range(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws2", name="Sec2")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win._active_plot_type = "section"
    win.section_canvas.set_section(
        [_presentation("w1", "W1"), _presentation("w2", "W2")]
    )

    img = QImage(600, 480, QImage.Format.Format_ARGB32)
    img.fill(0)
    painter = QPainter(img)
    try:
        win._paint_active_plot(
            painter, QRectF(0, 0, 600, 480), depth_range=(1000.0, 1050.0)
        )
    finally:
        painter.end()
    assert not img.isNull()
