"""Curve wrap-around (FRS §2.x 超量程折叠).

Covers the ``ScaleSpec.wrap`` flag (parse + override roundtrip), the
paint-time behaviour (wrap folds back via modulo instead of clipping),
paint smoke for all four render sites, and the shell track-props checkbox.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import math

import numpy as np
import pytest
from PySide6.QtCore import QRectF
from PySide6.QtGui import QImage, QPainter

from well_log_workstation.template_model import (
    BoundCurveLayer,
    BoundTrack,
    HostPresentation,
    ScaleSpec,
    _parse_scale,
    apply_track_overrides,
    track_overrides_snapshot,
)


# -- ScaleSpec.wrap persistence -------------------------------------


def test_parse_scale_reads_wrap() -> None:
    s = _parse_scale({"mode": "linear", "min": 0, "max": 150, "wrap": True})
    assert s is not None and s.wrap is True


def test_parse_scale_legacy_defaults_wrap_false() -> None:
    s = _parse_scale({"mode": "linear", "min": 0, "max": 150})
    assert s is not None and s.wrap is False


def test_override_snapshot_apply_roundtrips_wrap() -> None:
    depth = np.array([0.0, 1.0])
    track = BoundTrack(
        id="gr",
        role="curve",
        title="GR",
        width_fraction=1.0,
        scale=ScaleSpec(min=0.0, max=150.0, wrap=True),
    )
    pres = HostPresentation(
        template_id="t",
        template_name="t",
        well_document_id="w",
        well_name="W",
        depth=depth,
        depth_unit="m",
        tracks=[track],
    )
    snap = track_overrides_snapshot(pres)
    assert snap["gr"]["scale_wrap"] is True

    # Apply onto a fresh track without wrap → wrap restored.
    track2 = BoundTrack(
        id="gr",
        role="curve",
        title="GR",
        width_fraction=1.0,
        scale=ScaleSpec(min=0.0, max=150.0),
    )
    pres2 = HostPresentation(
        template_id="t",
        template_name="t",
        well_document_id="w",
        well_name="W",
        depth=depth,
        depth_unit="m",
        tracks=[track2],
    )
    apply_track_overrides(pres2, snap)
    assert pres2.tracks[0].scale.wrap is True


def test_override_creates_scale_with_wrap_when_absent() -> None:
    depth = np.array([0.0, 1.0])
    track = BoundTrack(
        id="gr", role="curve", title="GR", width_fraction=1.0, scale=None
    )
    pres = HostPresentation(
        template_id="t",
        template_name="t",
        well_document_id="w",
        well_name="W",
        depth=depth,
        depth_unit="m",
        tracks=[track],
    )
    apply_track_overrides(pres, {"gr": {"scale_wrap": True}})
    assert pres.tracks[0].scale is not None
    assert pres.tracks[0].scale.wrap is True


# -- wrap fold-back geometry (unit test of the math) ----------------


def test_wrap_sawtooth_vs_clip() -> None:
    """The wrap branch must fold out-of-range t back into [0,1)."""

    def normalize(v: float, vmin: float, vmax: float) -> float:
        return (v - vmin) / (vmax - vmin)

    vmin, vmax = 0.0, 150.0
    # v=250 → t=1.667 → clip=1.0 (right edge), wrap=0.667 (folded back).
    t = normalize(250.0, vmin, vmax)
    clipped = max(0.0, min(1.0, t))
    wrapped = t - math.floor(t)
    assert clipped == 1.0
    assert abs(wrapped - (5.0 / 3.0 - 1.0)) < 1e-9
    # v=350 → t=2.333 → wrap=0.333 (second fold).
    t2 = normalize(350.0, vmin, vmax)
    assert abs((t2 - math.floor(t2)) - (7.0 / 3.0 - 2.0)) < 1e-9


# -- paint smoke: all four render sites -----------------------------


def _presentation(*, wrap: bool) -> HostPresentation:
    depth = np.array([1000.0, 1010.0, 1020.0])
    # A spike well beyond scale max to exercise wrap.
    vals = np.array([50.0, 250.0, 350.0])
    track = BoundTrack(
        id="gr",
        role="curve",
        title="GR",
        width_fraction=1.0,
        scale=ScaleSpec(min=0.0, max=150.0, wrap=wrap),
        layers=[
            BoundCurveLayer(
                mnemonic="GR",
                color="#1a6fb5",
                unit="gapi",
                values=vals,
                null_mask=np.zeros(3, bool),
            )
        ],
    )
    return HostPresentation(
        template_id="t",
        template_name="t",
        well_document_id="w",
        well_name="W",
        depth=depth,
        depth_unit="m",
        tracks=[track],
    )


def test_single_well_canvas_wrap_paint_smoke(qtbot) -> None:
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_presentation(_presentation(wrap=True))
    img = canvas.grab()
    assert img.width() == 600


def test_correlation_canvas_wrap_paint_smoke(qtbot) -> None:
    from well_log_workstation.correlation_canvas import CorrelationCanvas

    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_columns([_presentation(wrap=True), _presentation(wrap=True)])
    img = canvas.grab()
    assert img.width() == 600


def test_section_canvas_wrap_paint_smoke(qtbot) -> None:
    from well_log_workstation.section_canvas import SectionCanvas

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_section([_presentation(wrap=True), _presentation(wrap=True)])
    img = QImage(600, 400, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    canvas.render_to(painter, QRectF(0, 0, 600, 400))
    painter.end()
    assert img.width() == 600


def test_export_wrap_paint_smoke(qtbot) -> None:
    from well_log_workstation.export_plot import _paint_presentation

    canvas = QImage(600, 400, QImage.Format.Format_ARGB32)
    canvas.fill(0xFFFFFFFF)
    painter = QPainter(canvas)
    _paint_presentation(
        painter,
        _presentation(wrap=True),
        QRectF(0, 0, 600, 400),
    )
    painter.end()
    assert canvas.width() == 600


# -- production paint_curve: wrap folds over-range inboard (#614) ---


def test_curve_stroke_vertices_wrap_folds_overrange_inboard() -> None:
    """Production wrap mapping (not a test-local formula) folds v>vmax."""
    from well_log_workstation.curve_paint import curve_stroke_vertices

    depth = np.array([0.0, 1.0, 2.0])
    vals = np.array([250.0, 250.0, 250.0])  # t=1.667 → wrap 0.667, clip 1.0
    wrapped, _ = curve_stroke_vertices(
        depth, vals, None, 0.0, 2.0,
        vmin=0.0, vmax=150.0, wrap=True,
        x0=10.0, y0=0.0, tw=180.0, th=100.0,
    )
    clipped, _ = curve_stroke_vertices(
        depth, vals, None, 0.0, 2.0,
        vmin=0.0, vmax=150.0, wrap=False,
        x0=10.0, y0=0.0, tw=180.0, th=100.0,
    )
    assert wrapped and clipped
    expected_wrap = 10.0 + ((250.0 - 0.0) / 150.0 - 1.0) * 180.0
    assert all(abs(x - expected_wrap) < 1e-6 for x, _y, _i, _s in wrapped)
    assert all(abs(x - 190.0) < 1e-6 for x, _y, _i, _s in clipped)


def test_paint_curve_wrap_folds_overrange_off_the_right_edge() -> None:
    """#614: paint_curve wrap must not pin an over-range value to x0+tw."""
    from PySide6.QtGui import QColor, QPainter
    from PySide6.QtWidgets import QApplication

    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    QApplication.instance() or QApplication([])
    canvas = MultiTrackCanvas()
    img = QImage(200, 200, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    depth = np.array([0.0, 50.0, 100.0])
    vals = np.array([250.0, 250.0, 250.0])
    canvas._paint_curve(
        painter,
        x0=10,
        y0=10,
        tw=180,
        th=180,
        depth=depth,
        d0=0.0,
        d1=100.0,
        values=vals,
        null_mask=np.zeros(3, bool),
        vmin=0.0,
        vmax=150.0,
        mode="linear",
        color=QColor("#000000"),
        wrap=True,
    )
    painter.end()

    def _dark(x_lo: int, x_hi: int) -> bool:
        for y in range(img.height()):
            for x in range(x_lo, x_hi):
                if img.pixelColor(x, y).lightness() < 200:
                    return True
        return False

    # wrap t=2/3 → x = 10 + 120 = 130. Clip would paint x=190.
    assert _dark(122, 138), "wrapped over-range value must paint inboard"
    assert not _dark(175, 195), "wrap must not clip over-range values to the right edge"


# -- shell track-props checkbox -------------------------------------


def test_shell_track_wrap_checkbox_wires_scale(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace

    def _las(path: Path, well: str) -> Path:
        path.write_text(
            f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1003.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 20
1001 50
1002 90
1003 200
""",
            encoding="utf-8",
        )
        return path

    ws = create_workspace(tmp_path / "ws")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    wid = win.import_las_path(_las(tmp_path / "a.las", "A"))

    from well_log_workstation.plot_document import create_single_well_plot

    plot = create_single_well_plot(
        ws, well_id=wid, well_name="A", template_id="std-gr-rt-den"
    )
    win.open_plot_document(plot.id)

    # Select the GR track (first curve track) and toggle wrap on.
    pres = win._presentation
    assert pres is not None
    gr = next(t for t in pres.tracks if t.role == "curve")
    assert gr.scale is not None and gr.scale.wrap is False
    # Find the track in the list and select it, then toggle the checkbox.
    from PySide6.QtCore import Qt

    for i in range(win.track_list.count()):
        if win.track_list.item(i).data(Qt.ItemDataRole.UserRole) == gr.id:
            win.track_list.setCurrentRow(i)
            break
    win.track_scale_wrap.setChecked(True)
    assert gr.scale.wrap is True
