"""Curve-extrema snap + horizon-link drag (FRS §2.x 分层线拖拽吸附).

Covers the pure-numpy extrema detection + snap, the canvas link hit-test,
the drag → link_dragged signal flow (with extrema snapping), and the shell
commit path (plot.links update + undo).
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QMouseEvent

from well_log_workstation.curve_extrema import (
    local_extrema_depths,
    snap_depth_to_extrema,
)
from well_log_workstation.template_model import (
    BoundCurveLayer,
    BoundTrack,
    HostPresentation,
    ScaleSpec,
)


# -- pure numpy: extrema ---------------------------------------------


def test_local_extrema_depths_peaks_and_valleys() -> None:
    depth = np.arange(1000.0, 1010.0)
    vals = np.array([1.0, 3.0, 1.0, 3.0, 1.0, 3.0, 1.0, 3.0, 1.0, 3.0])
    ext = local_extrema_depths(depth, vals, np.zeros(10, bool))
    # Peaks at odd indices 1,3,5,7 (last point has no right sign).
    np.testing.assert_allclose(ext, [1001.0, 1003.0, 1005.0, 1007.0])


def test_local_extrema_null_breaks() -> None:
    depth = np.arange(1000.0, 1010.0)
    vals = np.array([1.0, 3.0, 1.0, 3.0, 1.0, 3.0, 1.0, 3.0, 1.0, 3.0])
    mask = np.zeros(10, bool)
    mask[3] = True
    ext = local_extrema_depths(depth, vals, mask)
    # index 3 null → no extremum there; 1001 and 1005/1007 survive.
    assert 1003.0 not in ext
    assert 1001.0 in ext


def test_local_extrema_flat_none() -> None:
    depth = np.arange(1000.0, 1010.0)
    ext = local_extrema_depths(depth, np.ones(10), np.zeros(10, bool))
    assert ext.size == 0


def test_snap_depth_to_extrema_within_tol() -> None:
    depth = np.arange(1000.0, 1010.0)
    vals = np.array([1.0, 3.0, 1.0, 3.0, 1.0, 3.0, 1.0, 3.0, 1.0, 3.0])
    assert snap_depth_to_extrema(depth, vals, np.zeros(10, bool), 1001.5, tol=1.0) == 1001.0
    # Out of tolerance → unchanged.
    assert snap_depth_to_extrema(depth, vals, np.zeros(10, bool), 1004.0, tol=0.1) == 1004.0


# -- canvas: link hit-test + drag ------------------------------------


def _canvas(qtbot):
    from well_log_workstation.correlation_canvas import CorrelationCanvas
    from well_log_workstation.correlation_links import HorizonLink

    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    # 8 samples, 1 m apart: peaks at 1001/1003/1005/1007.
    depth = np.arange(1000.0, 1008.0)

    def mk(well_id: str, vals: np.ndarray) -> HostPresentation:
        track = BoundTrack(
            id="c", role="curve", title="GR", width_fraction=1.0,
            scale=ScaleSpec(min=0.0, max=4.0),
            layers=[BoundCurveLayer(
                mnemonic="GR", color="#1a6fb5", unit="gapi",
                values=vals, null_mask=np.zeros(depth.size, bool),
            )],
        )
        return HostPresentation(
            template_id="t", template_name="t", well_document_id=well_id,
            well_name=well_id, depth=depth, depth_unit="m", tracks=[track],
        )

    w0 = mk("w0", np.array([1.0, 3.0, 1.0, 3.0, 1.0, 3.0, 1.0, 3.0]))
    w1 = mk("w1", np.full(8, 2.0))
    link = HorizonLink(
        id="l1", left_well_id="w0", right_well_id="w1", name="T1",
        left_depth=1001.0, right_depth=1001.0,
        left_marker_id="", right_marker_id="", color="#c0392b",
    )
    canvas.set_columns([w0, w1], [[], []], [link])
    canvas.set_link_pick_mode(True)
    return canvas, link


def _link_mid_px(canvas) -> tuple[float, float]:
    d0, d1 = canvas._d0, canvas._d1
    top, bottom = 36, canvas.height() - 24
    y = top + ((1001.0 - d0) / (d1 - d0)) * (bottom - top)
    # Painted geometry (#589): the shared _column_layout reserves the
    # depth-ruler strip; the old hardcoded (600-16-6)//2 encoded the
    # ruler-less drift the fix removed.
    col_w, gap = canvas._column_layout()
    lx = canvas._x_well(0, col_w, gap) + col_w / 2 - 4
    rx = canvas._x_well(1, col_w, gap) - col_w / 2 + 2
    return (lx + rx) / 2, y


def _mouse_ev(etype, x, y, btn, buttons) -> QMouseEvent:
    return QMouseEvent(
        etype, QPointF(x, y), QPointF(x, y), btn, buttons,
        Qt.KeyboardModifier.NoModifier,
    )


def test_hit_test_link_hits_segment(qtbot) -> None:
    canvas, _link = _canvas(qtbot)
    mx, my = _link_mid_px(canvas)
    assert canvas.hit_test_link(mx, my) == "l1"
    # Far away → None.
    assert canvas.hit_test_link(10.0, 10.0) is None


def test_snap_drag_depth_snaps_to_peak(qtbot) -> None:
    """Unit-level: dragging near a curve peak snaps the depth to it."""
    canvas, _link = _canvas(qtbot)
    # Peak at 1001; target 1001.02 is within the pixel tolerance → 1001.
    snapped = canvas._snap_drag_depth("w0", 1001.02)
    assert snapped == 1001.0


def test_snap_drag_depth_outside_tolerance_unchanged(qtbot) -> None:
    canvas, _link = _canvas(qtbot)
    # 1002.5 is ~1.5 m from the nearest peak (1003 / 1001) — beyond the
    # ~0.2 m pixel tolerance at this zoom → unchanged.
    snapped = canvas._snap_drag_depth("w0", 1002.5)
    assert snapped == 1002.5


def test_link_drag_emits_committed(qtbot) -> None:
    canvas, _link = _canvas(qtbot)
    committed: list[tuple[str, float, float]] = []
    canvas.link_dragged.connect(
        lambda lid, ld, rd: committed.append((lid, ld, rd))
    )
    mx, my = _link_mid_px(canvas)
    canvas.mousePressEvent(
        _mouse_ev(QMouseEvent.Type.MouseButtonPress, mx, my,
                  Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )
    assert canvas._drag_link_id == "l1"
    # Move down: the committed depth is deeper than the original.
    canvas.mouseMoveEvent(
        _mouse_ev(QMouseEvent.Type.MouseMove, mx, my + 30,
                  Qt.MouseButton.NoButton, Qt.MouseButton.LeftButton)
    )
    canvas.mouseReleaseEvent(
        _mouse_ev(QMouseEvent.Type.MouseButtonRelease, mx, my + 30,
                  Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )
    assert len(committed) == 1
    link_id, ld, rd = committed[0]
    assert link_id == "l1"
    # Both anchors moved by the same offset.
    assert abs(ld - rd) < 1e-6
    assert ld > 1001.0


def test_link_drag_esc_cancels(qtbot) -> None:
    from PySide6.QtGui import QKeyEvent

    canvas, _link = _canvas(qtbot)
    committed: list = []
    canvas.link_dragged.connect(lambda *_: committed.append(1))
    mx, my = _link_mid_px(canvas)
    canvas.mousePressEvent(
        _mouse_ev(QMouseEvent.Type.MouseButtonPress, mx, my,
                  Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )
    canvas.keyPressEvent(
        QKeyEvent(QKeyEvent.Type.KeyPress, Qt.Key.Key_Escape,
                  Qt.KeyboardModifier.NoModifier)
    )
    canvas.mouseReleaseEvent(
        _mouse_ev(QMouseEvent.Type.MouseButtonRelease, mx, my + 30,
                  Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )
    assert committed == []


def test_link_drag_click_without_move_is_top_pick(qtbot) -> None:
    canvas, _link = _canvas(qtbot)
    picked: list = []
    canvas.top_clicked.connect(lambda *_: picked.append(1))
    mx, my = _link_mid_px(canvas)
    canvas.mousePressEvent(
        _mouse_ev(QMouseEvent.Type.MouseButtonPress, mx, my,
                  Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )
    canvas.mouseReleaseEvent(
        _mouse_ev(QMouseEvent.Type.MouseButtonRelease, mx, my,
                  Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )
    # No drag (no move) → falls through to top pick; no top at that spot.
    assert picked == []


# -- shell integration -----------------------------------------------


def test_shell_link_dragged_persists_and_undoes(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.correlation_links import HorizonLink as HL
    from well_log_workstation.plot_document import load_plot_document
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
1000 20 1
1001 50 2
1002 95 3
1003 30 4
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
    plot = win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    win.open_plot_document(plot.id)

    # Seed a link.
    link = HL(
        id="l1", left_well_id=id1, right_well_id=id2, name="T1",
        left_depth=1001.0, right_depth=1001.0,
        left_marker_id="", right_marker_id="", color="#c0392b",
    )
    win._set_correlation_links([link], persist=True)

    # Drag commit via the shell handler.
    win._on_correlation_link_dragged(link.id, 1003.0, 1003.0)
    reloaded = load_plot_document(ws, plot.id)
    assert len(reloaded.links) == 1
    assert abs(reloaded.links[0].left_depth - 1003.0) < 1e-6

    # Undo restores the original depth.
    assert win.undo_correlation_layout() is True
    reloaded2 = load_plot_document(ws, plot.id)
    assert abs(reloaded2.links[0].left_depth - 1001.0) < 1e-6
