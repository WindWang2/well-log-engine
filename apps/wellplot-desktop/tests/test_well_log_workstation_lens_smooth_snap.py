"""Section lens snap-to-tops + Chaikin smoothing (FRS §3.x 磁吸 / 手绘平滑).

Covers the pure-numpy ``smooth_ring`` corner-cutter, the per-lens ``smooth``
flag + JSON roundtrip (incl. legacy files without the field), the canvas
``_snap_point`` pixel-tolerance logic, paint-time smoothing (render smoke),
and the shell global toggles wiring to the canvas.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtCore import QRectF
from PySide6.QtGui import QImage, QPainter

from well_log_workstation.section_geometry import (
    LensBody2D,
    finalize_draft,
    lens_from_json,
    lens_to_json,
    make_ellipse_lens,
    smooth_ring,
)
from well_log_workstation.section_canvas import SectionCanvas
from well_log_workstation.template_model import HostPresentation
from well_log_workstation.tops_model import FormationTop


# -- smooth_ring (pure geometry) -------------------------------------


def test_smooth_ring_doubles_vertex_count() -> None:
    sq = np.array([[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]])
    assert smooth_ring(sq, iterations=1).shape == (8, 2)
    assert smooth_ring(sq, iterations=2).shape == (16, 2)
    assert smooth_ring(sq, iterations=0).shape == (4, 2)  # no-op


def test_smooth_ring_corner_cut_points() -> None:
    # Edge (0,0)->(1,0): 1/4 point (0.25,0), 3/4 point (0.75,0).
    sq = np.array([[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]])
    out = smooth_ring(sq, iterations=1)
    # q_i = 0.25*ring + 0.75*nxt lands at interleaved even indices.
    np.testing.assert_allclose(out[0], [0.75, 0.0])  # q0 on edge 0->1
    np.testing.assert_allclose(out[1], [0.25, 0.0])  # r0 on edge 0->1


def test_smooth_ring_passthrough_small_or_nonfinite() -> None:
    small = np.array([[0.0, 0.0], [1.0, 1.0]])
    assert smooth_ring(small, iterations=2).shape == (2, 2)
    nan = np.array([[0.0, np.nan], [1.0, 0.0], [0.5, 1.0]])
    assert smooth_ring(nan, iterations=1).shape == (3, 2)


def test_smooth_ring_does_not_mutate_input() -> None:
    sq = np.array([[0.0, 0.0], [1.0, 0.0], [1.0, 1.0]])
    orig = sq.copy()
    smooth_ring(sq, iterations=2)
    np.testing.assert_array_equal(sq, orig)


# -- LensBody2D.smooth field + JSON ---------------------------------


def test_lens_default_smooth_false() -> None:
    lens = make_ellipse_lens(0.5, 1050.0, 0.3, 20.0)
    assert lens.smooth is False


def test_lens_smooth_json_roundtrip() -> None:
    lens = LensBody2D(
        points=np.array([[0.0, 1000.0], [1.0, 1000.0], [0.5, 1080.0]]),
        label="S",
        smooth=True,
    )
    back = lens_from_json(lens_to_json(lens))
    assert back is not None and back.smooth is True


def test_lens_legacy_json_defaults_smooth_false() -> None:
    # Old file written before the smooth field existed.
    raw = {"label": "L", "points": [[0, 1000.0], [1, 1000.0], [0.5, 1080.0]]}
    back = lens_from_json(raw)
    assert back is not None and back.smooth is False


def test_finalize_draft_smooth_passthrough() -> None:
    lens = finalize_draft(
        [(0.0, 1000.0), (1.0, 1000.0), (0.5, 1080.0)], smooth=True
    )
    assert lens is not None and lens.smooth is True


# -- canvas snap-to-tops --------------------------------------------


def _make_canvas(tops: list[list[FormationTop]]) -> SectionCanvas:
    canvas = SectionCanvas()
    canvas.resize(600, 400)
    depth = np.array([1000.0, 1200.0])
    pres = [
        HostPresentation(
            template_id="t",
            template_name="t",
            well_document_id=str(i),
            well_name=f"W{i}",
            depth=depth,
            depth_unit="m",
            tracks=[],
        )
        for i in range(2)
    ]
    canvas.set_section(pres, tops)
    return canvas


def test_snap_point_disabled_returns_input(qtbot) -> None:
    canvas = _make_canvas([[FormationTop(name="A", depth=1080.0)], []])
    qtbot.addWidget(canvas)
    # snap off by default
    assert canvas._snap_point(0.5, 1081.0) == (0.5, 1081.0)


def test_snap_point_within_tolerance_snaps(qtbot) -> None:
    canvas = _make_canvas(
        [[FormationTop(name="A", depth=1080.0)], [FormationTop(name="A", depth=1090.0)]]
    )
    qtbot.addWidget(canvas)
    canvas.set_snap_tops(True)
    # x_unit=0.5 bounds wells 0 and 1; depth 1080 == well-0 top.
    sx, sy = canvas._snap_point(0.5, 1081.0)
    assert sx == 0.5  # x preserved
    assert sy == 1080.0  # snapped to nearest top depth


def test_snap_point_outside_tolerance_passthrough(qtbot) -> None:
    canvas = _make_canvas([[FormationTop(name="A", depth=1080.0)], []])
    qtbot.addWidget(canvas)
    canvas.set_snap_tops(True)
    # 50m away — well beyond the 10px window for a 200m / ~340px range.
    assert canvas._snap_point(0.5, 1150.0) == (0.5, 1150.0)


def test_snap_point_keeps_nearest_when_two_nearby(qtbot) -> None:
    canvas = _make_canvas(
        [[FormationTop(name="A", depth=1080.0)], [FormationTop(name="A", depth=1082.0)]]
    )
    qtbot.addWidget(canvas)
    canvas.set_snap_tops(True)
    # depth 1081 is 1m from both; nearest (min delta) = 1080 (well 0) or 1082
    # — both within tol, picks the first within range encountered.
    _, sy = canvas._snap_point(0.5, 1081.0)
    assert sy in (1080.0, 1082.0)


# -- canvas paint-time smoothing (render smoke) ----------------------


def test_canvas_smooth_lens_paint_smoke(qtbot) -> None:
    canvas = _make_canvas([])
    qtbot.addWidget(canvas)
    lens = make_ellipse_lens(0.5, 1100.0, 0.3, 20.0, label="S")
    lens.smooth = True
    canvas.set_lenses([lens])
    canvas.set_lens_smooth(True)
    assert canvas.lens_smooth() is True

    canvas.set_draw_lens_mode(True)
    canvas._lens_draft = [(0.2, 1080.0), (0.8, 1080.0), (0.5, 1120.0)]
    canvas._lens_cursor = (0.5, 1100.0)

    img = QImage(600, 400, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    canvas.render_to(painter, QRectF(0, 0, 600, 400))
    painter.end()
    assert img.width() == 600


def test_canvas_toggle_getters_roundtrip(qtbot) -> None:
    canvas = _make_canvas([])
    qtbot.addWidget(canvas)
    canvas.set_snap_tops(True)
    assert canvas.snap_tops() is True
    canvas.set_snap_tops(False)
    assert canvas.snap_tops() is False
    canvas.set_lens_smooth(True)
    assert canvas.lens_smooth() is True


# -- shell wiring ----------------------------------------------------


def test_shell_lens_toggle_wires_canvas(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace

    def _las(path: Path, well: str) -> Path:
        path.write_text(
            f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1002.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 20
1001 30
1002 40
""",
            encoding="utf-8",
        )
        return path

    ws = create_workspace(tmp_path / "ws")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_las(tmp_path / "b.las", "B"))

    from well_log_workstation.plot_document import create_section_plot

    plot = create_section_plot(ws, well_ids=[id1, id2], template_id="gr-only")
    win.open_plot_document(plot.id)

    # Toggles enabled on a section plot.
    assert win.section_lens_snap_check.isEnabled()
    assert win.section_lens_smooth_check.isEnabled()

    win.section_lens_snap_check.setChecked(True)
    assert win.section_canvas.snap_tops() is True
    win.section_lens_smooth_check.setChecked(True)
    assert win.section_canvas.lens_smooth() is True
