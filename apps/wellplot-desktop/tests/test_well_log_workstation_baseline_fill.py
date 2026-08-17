"""Baseline fill (FRS §2.x 基线充填, e.g. GR>80).

Covers the pure-geometry ``baseline_fill_polygons``, the ``ScaleSpec``
fill fields (parse + override roundtrip), paint smoke for the four render
sites, and the shell track-props wiring.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QImage, QMouseEvent, QPainter

from well_log_workstation.baseline_fill import baseline_fill_polygons
from well_log_workstation.template_model import (
    BoundCurveLayer,
    BoundTrack,
    HostPresentation,
    ScaleSpec,
    _parse_scale,
    apply_track_overrides,
    track_overrides_snapshot,
)


# -- pure geometry ---------------------------------------------------


def _id_maps():
    return (lambda v: float(v), lambda d: float(d))


def test_baseline_fill_above_threshold_runs() -> None:
    depth = np.arange(1000.0, 1010.0)
    vals = np.array([10.0, 90.0, 95.0, 90.0, 10.0, 10.0, 85.0, 10.0, 10.0, 10.0])
    x_of, y_of = _id_maps()
    polys = baseline_fill_polygons(
        x_of, y_of, 200.0, depth, 1000.0, 1009.0, vals,
        np.zeros(10, bool), step=1, threshold=80.0, direction="above",
    )
    # Runs: indices 1-3 (3 pts) and 6 (1 pt, no area → dropped) → 1 polygon.
    assert len(polys) == 1
    poly = polys[0]
    assert poly.count() == 6  # 3 curve pts + 3 right-edge pts
    assert poly.at(0) == QPointF(90.0, 1001.0)
    assert poly.at(3).x() == 200.0  # right edge


def test_baseline_fill_below_threshold() -> None:
    depth = np.arange(1000.0, 1010.0)
    vals = np.array([10.0, 90.0, 95.0, 90.0, 10.0, 10.0, 85.0, 10.0, 10.0, 10.0])
    x_of, y_of = _id_maps()
    polys = baseline_fill_polygons(
        x_of, y_of, 200.0, depth, 1000.0, 1009.0, vals,
        np.zeros(10, bool), step=1, threshold=80.0, direction="below",
    )
    # Runs: 0 (1 pt dropped), 4-5, 7-9 → 2 polygons.
    assert len(polys) == 2
    assert polys[0].count() == 4  # 2 curve pts + 2 right-edge pts
    assert polys[1].count() == 6  # 3 curve pts + 3 right-edge pts


def test_baseline_fill_null_breaks_run() -> None:
    depth = np.arange(1000.0, 1010.0)
    vals = np.array([10.0, 90.0, 95.0, 90.0, 10.0, 10.0, 85.0, 10.0, 10.0, 10.0])
    mask = np.zeros(10, bool)
    mask[2] = True  # breaks the 1-3 run into 1 and 3 (both dropped: 1 pt, and 3-3)
    x_of, y_of = _id_maps()
    polys = baseline_fill_polygons(
        x_of, y_of, 200.0, depth, 1000.0, 1009.0, vals, mask,
        step=1, threshold=80.0, direction="above",
    )
    # Run 1 (1 pt dropped), 3 (1 pt dropped), 6 (1 pt dropped) → none.
    assert polys == []


def test_baseline_fill_none_qualifies_empty() -> None:
    depth = np.arange(1000.0, 1010.0)
    x_of, y_of = _id_maps()
    polys = baseline_fill_polygons(
        x_of, y_of, 200.0, depth, 1000.0, 1009.0,
        np.full(10, 10.0), np.zeros(10, bool),
        step=1, threshold=80.0, direction="above",
    )
    assert polys == []


def test_baseline_fill_threshold_none_empty() -> None:
    depth = np.arange(1000.0, 1010.0)
    x_of, y_of = _id_maps()
    polys = baseline_fill_polygons(
        x_of, y_of, 200.0, depth, 1000.0, 1009.0,
        np.full(10, 90.0), np.zeros(10, bool),
        step=1, threshold=None, direction="above",
    )
    assert polys == []


# -- ScaleSpec fill fields -------------------------------------------


def test_parse_scale_reads_fill() -> None:
    s = _parse_scale(
        {"mode": "linear", "min": 0, "max": 150,
         "fill_threshold": 80.0, "fill_direction": "above"}
    )
    assert s is not None
    assert s.fill_threshold == 80.0
    assert s.fill_direction == "above"


def test_parse_scale_legacy_defaults_fill_off() -> None:
    s = _parse_scale({"mode": "linear", "min": 0, "max": 150})
    assert s is not None
    assert s.fill_threshold is None
    assert s.fill_direction == "above"


def test_override_snapshot_apply_roundtrips_fill() -> None:
    depth = np.array([0.0, 1.0])
    track = BoundTrack(
        id="gr", role="curve", title="GR", width_fraction=1.0,
        scale=ScaleSpec(min=0.0, max=150.0, fill_threshold=80.0,
                        fill_direction="below"),
    )
    pres = HostPresentation(
        template_id="t", template_name="t", well_document_id="w",
        well_name="W", depth=depth, depth_unit="m", tracks=[track],
    )
    snap = track_overrides_snapshot(pres)
    assert snap["gr"]["scale_fill_threshold"] == 80.0
    assert snap["gr"]["scale_fill_direction"] == "below"

    track2 = BoundTrack(
        id="gr", role="curve", title="GR", width_fraction=1.0,
        scale=ScaleSpec(min=0.0, max=150.0),
    )
    pres2 = HostPresentation(
        template_id="t", template_name="t", well_document_id="w",
        well_name="W", depth=depth, depth_unit="m", tracks=[track2],
    )
    apply_track_overrides(pres2, snap)
    assert pres2.tracks[0].scale.fill_threshold == 80.0
    assert pres2.tracks[0].scale.fill_direction == "below"


def test_override_creates_scale_with_fill() -> None:
    depth = np.array([0.0, 1.0])
    track = BoundTrack(
        id="gr", role="curve", title="GR", width_fraction=1.0, scale=None
    )
    pres = HostPresentation(
        template_id="t", template_name="t", well_document_id="w",
        well_name="W", depth=depth, depth_unit="m", tracks=[track],
    )
    apply_track_overrides(
        pres, {"gr": {"scale_fill_threshold": 90.0}}
    )
    assert pres.tracks[0].scale is not None
    assert pres.tracks[0].scale.fill_threshold == 90.0


# -- paint smoke: four render sites ----------------------------------


def _presentation(*, fill: bool = True) -> HostPresentation:
    depth = np.array([1000.0, 1010.0, 1020.0, 1030.0, 1040.0])
    vals = np.array([20.0, 90.0, 95.0, 85.0, 20.0])
    track = BoundTrack(
        id="gr", role="curve", title="GR", width_fraction=1.0,
        scale=ScaleSpec(
            min=0.0, max=100.0,
            fill_threshold=80.0 if fill else None,
            fill_direction="above",
        ),
        layers=[
            BoundCurveLayer(
                mnemonic="GR", color="#1a6fb5", unit="gapi",
                values=vals, null_mask=np.zeros(5, bool),
            )
        ],
    )
    return HostPresentation(
        template_id="t", template_name="t", well_document_id="w",
        well_name="W", depth=depth, depth_unit="m", tracks=[track],
    )


def test_multi_track_canvas_fill_paint_smoke(qtbot) -> None:
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_presentation(_presentation(fill=True))
    img = canvas.grab()
    assert img.width() == 600


def test_export_fill_paint_smoke(qtbot) -> None:
    from well_log_workstation.export_plot import _paint_presentation

    img = QImage(600, 400, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    _paint_presentation(painter, _presentation(fill=True), QRectF(0, 0, 600, 400))
    painter.end()
    assert img.width() == 600


def test_correlation_canvas_fill_paint_smoke(qtbot) -> None:
    from well_log_workstation.correlation_canvas import CorrelationCanvas

    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_columns([_presentation(fill=True), _presentation(fill=True)])
    img = canvas.grab()
    assert img.width() == 600


def test_section_canvas_fill_paint_smoke(qtbot) -> None:
    from well_log_workstation.section_canvas import SectionCanvas

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_section([_presentation(fill=True), _presentation(fill=True)])
    img = QImage(600, 400, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    canvas.render_to(painter, QRectF(0, 0, 600, 400))
    painter.end()
    assert img.width() == 600


# -- production paint: fill reaches the high-value edge (#614) ------


def test_paint_curve_baseline_fill_reaches_right_edge() -> None:
    """#614: _paint_curve fill (threshold above) must tint the right edge."""
    from PySide6.QtGui import QColor
    from PySide6.QtWidgets import QApplication

    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    QApplication.instance() or QApplication([])
    canvas = MultiTrackCanvas()
    img = QImage(200, 200, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    depth = np.array([0.0, 50.0, 100.0])
    vals = np.array([90.0, 90.0, 90.0])
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
        vmax=100.0,
        mode="linear",
        color=QColor("#1a6fb5"),
        fill_threshold=80.0,
        fill_direction="above",
        reverse=False,
    )
    painter.end()
    # v=90 → x = 10 + 0.9*180 = 172; fill closes to x=190. Sample mid-fill.
    sample = img.pixelColor(185, 100)
    assert sample != QColor("#ffffff"), "baseline fill must tint the right (high-value) edge"
    # Left of the curve (x≈20) stays the white backdrop.
    assert img.pixelColor(20, 100) == QColor("#ffffff")


# -- shell wiring ----------------------------------------------------


def test_shell_fill_checkbox_updates_scale_and_persists(
    qtbot, tmp_path: Path
) -> None:
    from well_log_workstation.plot_document import create_single_well_plot
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
1001 90
1002 95
1003 30
""",
            encoding="utf-8",
        )
        return path

    ws = create_workspace(tmp_path / "ws")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    wid = win.import_las_path(_las(tmp_path / "a.las", "A"))
    plot = create_single_well_plot(
        ws, well_id=wid, well_name="A", template_id="std-gr-rt-den"
    )
    win.open_plot_document(plot.id)

    # Select the GR track.
    from PySide6.QtCore import Qt

    pres = win._presentation
    gr = next(t for t in pres.tracks if t.role == "curve")
    assert gr.scale is not None and gr.scale.fill_threshold is None
    for i in range(win.track_list.count()):
        if win.track_list.item(i).data(Qt.ItemDataRole.UserRole) == gr.id:
            win.track_list.setCurrentRow(i)
            break

    # Enable fill with threshold 75, direction below.
    win.track_fill_threshold.setValue(75.0)
    win.track_fill_direction.setCurrentIndex(
        win.track_fill_direction.findData("below")
    )
    win.track_fill_enable.setChecked(True)
    assert gr.scale.fill_threshold == 75.0
    assert gr.scale.fill_direction == "below"

    # Unchecking clears the threshold.
    win.track_fill_enable.setChecked(False)
    assert gr.scale.fill_threshold is None
