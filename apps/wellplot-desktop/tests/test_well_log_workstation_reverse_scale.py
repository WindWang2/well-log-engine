"""Curve reverse scale (FRS §2.x 反向刻度, 密度对道).

Covers the ``ScaleSpec.reverse`` flag: parse + override roundtrip, the
paint-time ``t = 1 - t`` flip (axis runs right->left so high values map
to the left edge - the standard density/neutron facing layout),
baseline fill following the high-value edge, the freehand inverse map,
per-layer reverse for crossover facing, paint smoke for all four render
sites, and the shell track-props checkbox.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import math

import numpy as np
import pytest
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QImage, QPainter

from well_log_workstation.template_model import (
    BoundCurveLayer,
    BoundTrack,
    HostPresentation,
    ScaleSpec,
    _parse_scale,
    apply_track_overrides,
    track_overrides_snapshot,
)


# -- ScaleSpec.reverse persistence ----------------------------------


def test_parse_scale_reads_reverse() -> None:
    s = _parse_scale({"mode": "linear", "min": 1.8, "max": 2.9, "reverse": True})
    assert s is not None and s.reverse is True


def test_parse_scale_legacy_defaults_reverse_false() -> None:
    s = _parse_scale({"mode": "linear", "min": 1.8, "max": 2.9})
    assert s is not None and s.reverse is False


def test_override_snapshot_apply_roundtrips_reverse() -> None:
    depth = np.array([0.0, 1.0])
    track = BoundTrack(
        id="gr",
        role="curve",
        title="GR",
        width_fraction=1.0,
        scale=ScaleSpec(min=0.0, max=150.0, reverse=True),
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
    assert snap["gr"]["scale_reverse"] is True

    # Apply onto a fresh track without reverse -> reverse restored.
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
    assert pres2.tracks[0].scale.reverse is True


def test_override_creates_scale_with_reverse_when_absent() -> None:
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
    apply_track_overrides(pres, {"gr": {"scale_reverse": True}})
    assert pres.tracks[0].scale is not None
    assert pres.tracks[0].scale.reverse is True


# -- reverse flip math (unit test of the mapping) ------------------


def test_reverse_flips_t_linear() -> None:
    """Reverse must mirror the normalized position: high value -> left."""

    def x_of(v: float, vmin: float, vmax: float, reverse: bool) -> float:
        t = (v - vmin) / (vmax - vmin) if vmax > vmin else 0.5
        t = max(0.0, min(1.0, t))
        if reverse:
            t = 1.0 - t
        return t

    vmin, vmax = 0.0, 150.0
    # v=0 (min) -> normal t=0 (left), reverse t=1 (right).
    assert x_of(0.0, vmin, vmax, False) == 0.0
    assert x_of(0.0, vmin, vmax, True) == 1.0
    # v=150 (max) -> normal t=1 (right), reverse t=0 (left).
    assert x_of(150.0, vmin, vmax, False) == 1.0
    assert x_of(150.0, vmin, vmax, True) == 0.0
    # Midpoint is unchanged.
    assert abs(x_of(75.0, vmin, vmax, True) - 0.5) < 1e-9


def test_reverse_flips_t_wrap_mirrors_sawtooth() -> None:
    """Reverse + wrap: ``1 - (t - floor(t))`` gives a mirrored fold."""

    def x_of(v: float, vmin: float, vmax: float, reverse: bool) -> float:
        t = (v - vmin) / (vmax - vmin)
        t = t - math.floor(t)  # wrap
        if reverse:
            t = 1.0 - t
        return t

    vmin, vmax = 0.0, 150.0
    # v=250 -> wrap t=0.667 -> reverse t=0.333 (mirror).
    t_wrap = (250.0 - vmin) / (vmax - vmin)
    t_wrap = t_wrap - math.floor(t_wrap)
    assert abs(x_of(250.0, vmin, vmax, True) - (1.0 - t_wrap)) < 1e-9


def test_reverse_flips_t_log_min_on_right() -> None:
    """Reverse + log: the log axis min sits on the right edge."""

    def x_of(v: float, vmin: float, vmax: float, reverse: bool) -> float:
        vmin = max(vmin, 1e-6)
        vmax = max(vmax, vmin * 10)
        log_min, log_max = math.log10(vmin), math.log10(vmax)
        t = (math.log10(v) - log_min) / (log_max - log_min)
        t = max(0.0, min(1.0, t))
        if reverse:
            t = 1.0 - t
        return t

    # v=vmin (decade floor) -> reverse t=1 (min on the right).
    assert abs(x_of(0.2, 0.2, 200.0, True) - 1.0) < 1e-9
    # v=vmax (decade ceiling) -> reverse t=0 (max on the left).
    assert abs(x_of(200.0, 0.2, 200.0, True) - 0.0) < 1e-9


# -- baseline fill follows the high-value edge when reversed --------


def test_baseline_fill_follows_left_edge_when_reversed() -> None:
    """With reverse on, the high-value edge is the left edge (x0).

    ``baseline_fill_polygons`` closes runs against the ``edge_x`` the
    caller passes; the canvas/export/corr/section sites pass ``x0`` when
    reverse is on. This test pins the geometry contract directly: a
    qualifying run closes to the supplied left edge, not the right.
    """
    from well_log_workstation.baseline_fill import baseline_fill_polygons

    depth = np.array([1000.0, 1001.0, 1002.0])
    vals = np.array([90.0, 90.0, 90.0])  # above threshold 80
    mask = np.zeros(3, bool)
    # identity mappers: x_of maps value->value, y_of maps depth->depth.
    edge_right = 200.0
    edge_left = 0.0

    polys_right = baseline_fill_polygons(
        lambda v: v, lambda d: d, edge_right, depth, 1000.0, 1002.0,
        vals, mask, step=1, threshold=80.0, direction="above",
    )
    # Normal: run closes to right edge (200).
    assert len(polys_right) == 1
    assert polys_right[0].at(3) == QPointF(edge_right, 1002.0)

    polys_left = baseline_fill_polygons(
        lambda v: v, lambda d: d, edge_left, depth, 1000.0, 1002.0,
        vals, mask, step=1, threshold=80.0, direction="above",
    )
    # Reversed: caller passes left edge (0); run closes to it.
    assert len(polys_left) == 1
    assert polys_left[0].at(3) == QPointF(edge_left, 1002.0)


# -- freehand inverse map (pixel x -> value) -----------------------


def test_pixel_to_value_reverse_inverts_t() -> None:
    """When reverse is on, a pixel at the right edge maps to the scale min
    (not the max), mirroring the forward ``t = 1 - t`` flip."""
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas
    from PySide6.QtCore import QRect

    body = QRect(0, 0, 100, 200)
    scale_normal = ScaleSpec(min=0.0, max=150.0)
    scale_reverse = ScaleSpec(min=0.0, max=150.0, reverse=True)
    # Left edge pixel (x=0): normal -> min (0), reverse -> max (150).
    assert MultiTrackCanvas._pixel_to_value(0, 0, scale_normal, body) == 0.0
    assert (
        MultiTrackCanvas._pixel_to_value(0, 0, scale_reverse, body) == 150.0
    )
    # Right edge pixel (x=100): normal -> max (150), reverse -> min (0).
    assert (
        MultiTrackCanvas._pixel_to_value(100, 0, scale_normal, body) == 150.0
    )
    assert MultiTrackCanvas._pixel_to_value(100, 0, scale_reverse, body) == 0.0


# -- per-layer reverse + crossover facing ---------------------------


def test_layer_reverse_parse_via_template() -> None:
    """A layer with a per-layer reverse scale gets its own ScaleSpec."""
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
                mnemonic="NPHI", unit="V/V",
                values=np.array([0.3, 0.2]),
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
                "title": "NPHI/RHOB",
                "width_fraction": 1.0,
                "scale": {"mode": "linear", "min": 0.0, "max": 100},
                "layers": [
                    {
                        "type": "curve",
                        "mnemonics": ["NPHI"],
                        "color": "#1a6fb5",
                        "scale": {"mode": "linear", "min": 0.0, "max": 0.6},
                    },
                    {
                        "type": "curve",
                        "mnemonics": ["RHOB"],
                        "color": "#2a9d4a",
                        "scale": {
                            "mode": "linear", "min": 1.8, "max": 2.9,
                            "reverse": True,
                        },
                    },
                ],
            }
        ],
    )
    pres = apply_template(template, doc)
    dual = next(t for t in pres.tracks if t.id == "dual")
    assert len(dual.layers) == 2
    # layers[0] (NPHI) per-layer scale, not reversed.
    assert dual.layers[0].scale is not None
    assert dual.layers[0].scale.reverse is False
    # layers[1] (RHOB) per-layer scale, reversed (density 对道).
    assert dual.layers[1].scale is not None
    assert dual.layers[1].scale.reverse is True
    # Track-level scale is not reverse.
    assert dual.scale is not None
    assert dual.scale.reverse is False


def test_crossover_fill_layer1_reversed_faces_layer0() -> None:
    """With layers[1] reversed, the facing lobe (layers[1] pulled left past
    layers[0]) is filled - the ``xu > xl`` rule follows facing geometry."""
    from well_log_workstation.crossover_fill import _layer_x_map

    # layers[0] normal: v in [0,100] -> x in [0,100]; v=50 -> x=50.
    normal = _layer_x_map(0.0, 100.0, None, ScaleSpec(min=0.0, max=100.0))
    # layers[1] reversed: v in [0,100] -> x in [100,0]; v=50 -> x=50.
    rev = _layer_x_map(
        0.0, 100.0, ScaleSpec(min=0.0, max=100.0, reverse=True),
        ScaleSpec(min=0.0, max=100.0),
    )
    # At v=50 both map to x=50 (they meet).
    assert abs(normal(50.0) - 50.0) < 1e-9
    assert abs(rev(50.0) - 50.0) < 1e-9
    # At v=80: normal -> x=80 (right), reversed -> x=20 (left). Facing.
    assert abs(normal(80.0) - 80.0) < 1e-9
    assert abs(rev(80.0) - 20.0) < 1e-9


# -- paint smoke: all four render sites -----------------------------


def _presentation(*, reverse: bool) -> HostPresentation:
    depth = np.array([1000.0, 1010.0, 1020.0])
    vals = np.array([30.0, 90.0, 60.0])
    track = BoundTrack(
        id="gr",
        role="curve",
        title="GR",
        width_fraction=1.0,
        scale=ScaleSpec(min=0.0, max=150.0, reverse=reverse),
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


def test_single_well_canvas_reverse_paint_smoke(qtbot) -> None:
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_presentation(_presentation(reverse=True))
    img = canvas.grab()
    assert img.width() == 600


def test_correlation_canvas_reverse_paint_smoke(qtbot) -> None:
    from well_log_workstation.correlation_canvas import CorrelationCanvas

    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_columns([_presentation(reverse=True), _presentation(reverse=True)])
    img = canvas.grab()
    assert img.width() == 600


def test_section_canvas_reverse_paint_smoke(qtbot) -> None:
    from well_log_workstation.section_canvas import SectionCanvas

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_section([_presentation(reverse=True), _presentation(reverse=True)])
    img = QImage(600, 400, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    canvas.render_to(painter, QRectF(0, 0, 600, 400))
    painter.end()
    assert img.width() == 600


def test_export_reverse_paint_smoke(qtbot) -> None:
    from well_log_workstation.export_plot import _paint_presentation

    canvas = QImage(600, 400, QImage.Format.Format_ARGB32)
    canvas.fill(0xFFFFFFFF)
    painter = QPainter(canvas)
    _paint_presentation(
        painter,
        _presentation(reverse=True),
        QRectF(0, 0, 600, 400),
    )
    painter.end()
    assert canvas.width() == 600


def test_reverse_with_baseline_fill_paint_smoke(qtbot) -> None:
    """Reverse + baseline fill must not crash (fill follows left edge)."""
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    depth = np.array([1000.0, 1010.0, 1020.0])
    vals = np.array([30.0, 90.0, 60.0])
    track = BoundTrack(
        id="gr",
        role="curve",
        title="GR",
        width_fraction=1.0,
        scale=ScaleSpec(
            min=0.0, max=150.0, reverse=True, fill_threshold=80.0,
            fill_direction="above",
        ),
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
    pres = HostPresentation(
        template_id="t",
        template_name="t",
        well_document_id="w",
        well_name="W",
        depth=depth,
        depth_unit="m",
        tracks=[track],
    )
    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_presentation(pres)
    img = canvas.grab()
    assert img.width() == 600


# -- production paint_curve: reverse puts vmax on the left (#614) ---


def _dark_in_x_band(img: QImage, x_lo: int, x_hi: int) -> bool:
    for y in range(img.height()):
        for x in range(x_lo, x_hi):
            if img.pixelColor(x, y).lightness() < 200:
                return True
    return False


def test_curve_stroke_vertices_reverse_puts_vmax_on_left() -> None:
    """Production mapping (not a test-local formula) places v=vmax at x0."""
    from well_log_workstation.curve_paint import curve_stroke_vertices

    depth = np.array([0.0, 1.0, 2.0])
    vals = np.array([150.0, 150.0, 150.0])
    reversed_verts, _ = curve_stroke_vertices(
        depth, vals, None, 0.0, 2.0,
        vmin=0.0, vmax=150.0, reverse=True,
        x0=10.0, y0=0.0, tw=180.0, th=100.0,
    )
    normal_verts, _ = curve_stroke_vertices(
        depth, vals, None, 0.0, 2.0,
        vmin=0.0, vmax=150.0, reverse=False,
        x0=10.0, y0=0.0, tw=180.0, th=100.0,
    )
    assert reversed_verts and normal_verts
    assert all(abs(x - 10.0) < 1e-6 for x, _y, _i, _s in reversed_verts)
    assert all(abs(x - 190.0) < 1e-6 for x, _y, _i, _s in normal_verts)


def test_paint_curve_reverse_puts_vmax_on_the_left() -> None:
    """#614: MultiTrackCanvas._paint_curve + paint_curve map v=vmax to x0."""
    from PySide6.QtWidgets import QApplication

    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    QApplication.instance() or QApplication([])
    canvas = MultiTrackCanvas()
    img = QImage(200, 200, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    depth = np.array([0.0, 50.0, 100.0])
    vals = np.array([150.0, 150.0, 150.0])
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
        reverse=True,
    )
    painter.end()
    # reverse=True: v=vmax → t=0 → x=10. Pen 1.5px → columns ~8-13.
    assert _dark_in_x_band(img, 7, 16), "vmax must paint on the left when reversed"
    # Un-reversed vmax would sit at x=190; that edge must stay white.
    assert not _dark_in_x_band(img, 175, 195), (
        "reversed vmax must not paint the right (normal) edge"
    )


def test_export_paint_curve_reverse_puts_vmax_on_the_left() -> None:
    """#614: export _paint_curve uses the same reverse mapping."""
    from PySide6.QtWidgets import QApplication

    from well_log_workstation.export_plot import _paint_curve as export_paint_curve

    QApplication.instance() or QApplication([])
    img = QImage(200, 200, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    depth = np.array([0.0, 50.0, 100.0])
    vals = np.array([150.0, 150.0, 150.0])
    export_paint_curve(
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
        reverse=True,
    )
    painter.end()
    assert _dark_in_x_band(img, 7, 16), "export reverse must put vmax on the left"
    assert not _dark_in_x_band(img, 175, 195)


# -- shell track-props checkbox -------------------------------------


def test_shell_track_reverse_checkbox_wires_scale(
    qtbot, tmp_path: Path
) -> None:
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

    # Select the GR track (first curve track) and toggle reverse on.
    pres = win._presentation
    assert pres is not None
    gr = next(t for t in pres.tracks if t.role == "curve")
    assert gr.scale is not None and gr.scale.reverse is False
    # Find the track in the list and select it, then toggle the checkbox.
    for i in range(win.track_list.count()):
        if win.track_list.item(i).data(Qt.ItemDataRole.UserRole) == gr.id:
            win.track_list.setCurrentRow(i)
            break
    win.track_scale_reverse.setChecked(True)
    assert gr.scale.reverse is True
    # And the override snapshot persists it.
    snap = track_overrides_snapshot(pres)
    assert snap[gr.id]["scale_reverse"] is True
