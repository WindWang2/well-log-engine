"""Crossover fill (FRS §2.x 双曲线交叉充填).

Covers the pure-geometry ``crossover_fill_polygons`` (upper-minus-lower
rule), per-layer scale parsing + override roundtrip, paint smoke for the
four render sites with a dual-scale track, and the shell checkbox wiring.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QImage, QPainter

from well_log_workstation.crossover_fill import crossover_fill_polygons
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


def test_crossover_fill_upper_right_of_lower() -> None:
    depth = np.arange(1000.0, 1010.0)
    upper_x = np.array([10., 15., 20., 25., 40., 45., 40., 25., 20., 15.])
    lower_x = np.array([30., 30., 30., 30., 30., 30., 30., 30., 30., 30.])
    polys = crossover_fill_polygons(
        lambda v: v, lambda v: v, lambda d: d,
        depth, 1000.0, 1009.0, upper_x, np.zeros(10, bool),
        lower_x, np.zeros(10, bool), step=1,
    )
    # Run at indices 4-6 (upper 40/45/40 > lower 30) → 1 polygon.
    assert len(polys) == 1
    poly = polys[0]
    assert poly.count() == 6  # 3 upper + 3 lower reversed
    assert poly.at(0) == QPointF(40.0, 1004.0)
    assert poly.at(3) == QPointF(30.0, 1006.0)  # lower reversed start


def test_crossover_fill_no_crossing_empty() -> None:
    depth = np.arange(1000.0, 1010.0)
    lo = np.array([10., 10., 10., 10., 10., 10., 10., 10., 10., 10.])
    hi = np.array([30., 30., 30., 30., 30., 30., 30., 30., 30., 30.])
    # Upper (lo, 10) is never to the right of lower (hi, 30) → empty.
    polys = crossover_fill_polygons(
        lambda v: v, lambda v: v, lambda d: d,
        depth, 1000.0, 1009.0, lo, np.zeros(10, bool),
        hi, np.zeros(10, bool), step=1,
    )
    assert polys == []


def test_crossover_fill_null_breaks_run() -> None:
    depth = np.arange(1000.0, 1010.0)
    upper_x = np.array([10., 15., 20., 25., 40., 45., 40., 25., 20., 15.])
    lower_x = np.array([30., 30., 30., 30., 30., 30., 30., 30., 30., 30.])
    mask = np.zeros(10, bool)
    mask[5] = True  # breaks the 4-6 run into singles → dropped (no area)
    polys = crossover_fill_polygons(
        lambda v: v, lambda v: v, lambda d: d,
        depth, 1000.0, 1009.0, upper_x, mask,
        lower_x, np.zeros(10, bool), step=1,
    )
    assert polys == []


# -- per-layer scale + crossover fields ------------------------------


def test_parse_scale_reads_crossover() -> None:
    s = _parse_scale(
        {"mode": "linear", "min": 0, "max": 150,
         "crossover_fill": True, "crossover_color": "#ffcc00"}
    )
    assert s is not None
    assert s.crossover_fill is True
    assert s.crossover_color == "#ffcc00"


def test_parse_scale_legacy_defaults_crossover_off() -> None:
    s = _parse_scale({"mode": "linear", "min": 0, "max": 150})
    assert s is not None
    assert s.crossover_fill is False
    assert s.crossover_color == ""


def test_layer_scale_parse_via_template() -> None:
    """A layer with a per-layer scale dict gets its own ScaleSpec."""
    import numpy as np

    from well_log_workstation.las_import import ImportedCurve, ImportedWellDocument
    from well_log_workstation.template_model import PlotTemplate, apply_template

    doc = ImportedWellDocument(
        document_id="w",
        well_name="W",
        source_path="wells/w/a.las",
        depth=np.array([1000.0, 1010.0]),
        depth_unit="m",
        curves=[
            ImportedCurve(
                mnemonic="RT", unit="OHMM",
                values=np.array([10.0, 20.0]),
                null_mask=np.zeros(2, bool),
            ),
            ImportedCurve(
                mnemonic="RHOB", unit="G/C3",
                values=np.array([2.2, 2.4]),
                null_mask=np.zeros(2, bool),
            ),
        ],
    )
    template = PlotTemplate(
        id="dual",
        name="Dual",
        tracks=[
            {
                "id": "dual",
                "role": "curve",
                "title": "RT/RHOB",
                "width_fraction": 1.0,
                "scale": {"mode": "log", "min": 0.2, "max": 200},
                "layers": [
                    {
                        "type": "curve",
                        "mnemonics": ["RT"],
                        "color": "#c45c26",
                        "scale": {"mode": "log", "min": 0.2, "max": 200},
                    },
                    {
                        "type": "curve",
                        "mnemonics": ["RHOB"],
                        "color": "#2a9d4a",
                        "scale": {"mode": "linear", "min": 1.8, "max": 2.9},
                    },
                ],
            }
        ],
    )
    pres = apply_template(template, doc)
    dual = next(t for t in pres.tracks if t.id == "dual")
    assert len(dual.layers) == 2
    assert dual.layers[0].scale is not None
    assert dual.layers[0].scale.mode == "log"
    assert dual.layers[1].scale is not None
    assert dual.layers[1].scale.mode == "linear"


def test_override_snapshot_apply_roundtrips_crossover() -> None:
    depth = np.array([0.0, 1.0])
    track = BoundTrack(
        id="dual", role="curve", title="D", width_fraction=1.0,
        scale=ScaleSpec(min=0.0, max=100.0, crossover_fill=True,
                        crossover_color="#ffcc00"),
        layers=[
            BoundCurveLayer(mnemonic="A", color="#111111", unit="",
                            values=np.array([1.0, 2.0]),
                            null_mask=np.zeros(2, bool)),
            BoundCurveLayer(mnemonic="B", color="#222222", unit="",
                            values=np.array([2.0, 1.0]),
                            null_mask=np.zeros(2, bool)),
        ],
    )
    pres = HostPresentation(
        template_id="t", template_name="t", well_document_id="w",
        well_name="W", depth=depth, depth_unit="m", tracks=[track],
    )
    snap = track_overrides_snapshot(pres)
    assert snap["dual"]["scale_crossover_fill"] is True
    assert snap["dual"]["scale_crossover_color"] == "#ffcc00"

    track2 = BoundTrack(
        id="dual", role="curve", title="D", width_fraction=1.0,
        scale=ScaleSpec(min=0.0, max=100.0),
        layers=[
            BoundCurveLayer(mnemonic="A", color="#111111", unit="",
                            values=np.array([1.0, 2.0]),
                            null_mask=np.zeros(2, bool)),
            BoundCurveLayer(mnemonic="B", color="#222222", unit="",
                            values=np.array([2.0, 1.0]),
                            null_mask=np.zeros(2, bool)),
        ],
    )
    pres2 = HostPresentation(
        template_id="t", template_name="t", well_document_id="w",
        well_name="W", depth=depth, depth_unit="m", tracks=[track2],
    )
    apply_track_overrides(pres2, snap)
    assert pres2.tracks[0].scale.crossover_fill is True
    assert pres2.tracks[0].scale.crossover_color == "#ffcc00"


# -- paint smoke -----------------------------------------------------


def _dual_presentation() -> HostPresentation:
    depth = np.array([1000.0, 1010.0, 1020.0, 1030.0])
    track = BoundTrack(
        id="dual", role="curve", title="RT/RHOB", width_fraction=1.0,
        scale=ScaleSpec(mode="log", min=0.2, max=200.0, crossover_fill=True),
        layers=[
            BoundCurveLayer(
                mnemonic="RT", color="#c45c26", unit="OHMM",
                values=np.array([10.0, 50.0, 100.0, 150.0]),
                null_mask=np.zeros(4, bool),
                scale=ScaleSpec(mode="log", min=0.2, max=200.0),
            ),
            BoundCurveLayer(
                mnemonic="RHOB", color="#2a9d4a", unit="G/C3",
                values=np.array([2.2, 2.5, 2.0, 1.9]),
                null_mask=np.zeros(4, bool),
                scale=ScaleSpec(mode="linear", min=1.8, max=2.9),
            ),
        ],
    )
    return HostPresentation(
        template_id="t", template_name="t", well_document_id="w",
        well_name="W", depth=depth, depth_unit="m", tracks=[track],
    )


def test_multi_track_canvas_crossover_paint_smoke(qtbot) -> None:
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_presentation(_dual_presentation())
    img = canvas.grab()
    assert img.width() == 600


def test_export_crossover_paint_smoke(qtbot) -> None:
    from well_log_workstation.export_plot import _paint_presentation

    img = QImage(600, 400, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    _paint_presentation(painter, _dual_presentation(), QRectF(0, 0, 600, 400))
    painter.end()
    assert img.width() == 600


def test_correlation_canvas_crossover_paint_smoke(qtbot) -> None:
    from well_log_workstation.correlation_canvas import CorrelationCanvas

    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_columns([_dual_presentation(), _dual_presentation()])
    img = canvas.grab()
    assert img.width() == 600


def test_section_canvas_crossover_paint_smoke(qtbot) -> None:
    from well_log_workstation.section_canvas import SectionCanvas

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_section([_dual_presentation(), _dual_presentation()])
    img = QImage(600, 400, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    canvas.render_to(painter, QRectF(0, 0, 600, 400))
    painter.end()
    assert img.width() == 600


# -- shell wiring ----------------------------------------------------


def test_shell_crossover_checkbox_updates_scale(qtbot, tmp_path: Path) -> None:
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
RT.OHMM
~ASCII
1000 20 10
1001 90 50
1002 95 100
1003 30 150
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

    # Inject a dual-layer track so the crossover checkbox is usable.
    pres = win._presentation
    gr = next(t for t in pres.tracks if t.role == "curve")
    assert len(gr.layers) == 1
    gr.layers.append(
        BoundCurveLayer(
            mnemonic="RT", color="#c45c26", unit="OHMM",
            values=np.array([10.0, 50.0, 100.0, 150.0]),
            null_mask=np.zeros(4, bool),
            scale=ScaleSpec(mode="log", min=0.2, max=200.0),
        )
    )
    win.multi_track_canvas.set_presentation(pres)

    from PySide6.QtCore import Qt

    for i in range(win.track_list.count()):
        if win.track_list.item(i).data(Qt.ItemDataRole.UserRole) == gr.id:
            win.track_list.setCurrentRow(i)
            break

    assert gr.scale is not None
    assert gr.scale.crossover_fill is False
    assert win.track_crossover_fill.isEnabled()  # dual-layer track
    win.track_crossover_fill.setChecked(True)
    assert gr.scale.crossover_fill is True
    win.track_crossover_fill.setChecked(False)
    assert gr.scale.crossover_fill is False
