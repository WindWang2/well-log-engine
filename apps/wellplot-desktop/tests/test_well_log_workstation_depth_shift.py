"""Interactive depth shift (FRS §2.x 交互深度校正).

Covers the single-well canvas shift mode: hit-testing tops with a pixel
tolerance, the drag → depth_shift_committed signal flow, paint smoke with
the drag preview, and the shell commit path (tops.json edit + undo).
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QImage, QMouseEvent, QPainter

from well_log_workstation.template_model import HostPresentation
from well_log_workstation.tops_model import FormationTop


def _canvas(qtbot, tops: list[FormationTop]):
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas

    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    depth = np.array([1000.0, 1010.0, 1020.0])
    pres = HostPresentation(
        template_id="t",
        template_name="t",
        well_document_id="w",
        well_name="W",
        depth=depth,
        depth_unit="m",
        tracks=[],
    )
    canvas.set_presentation(pres)
    canvas.set_tops(tops)
    return canvas


def _top(name: str, depth: float, top_id: str) -> FormationTop:
    return FormationTop(name=name, depth=depth, id=top_id)


def _depth_to_y(canvas, depth: float) -> float:
    """Map a depth to widget Y within the canvas plot band (test helper)."""
    top_band, bottom = canvas._plot_band()
    d0, d1 = canvas._d0, canvas._d1
    return top_band + ((depth - d0) / (d1 - d0)) * (bottom - top_band)


def _mouse_ev(
    etype, y: float, btn: Qt.MouseButton, buttons: Qt.MouseButton
) -> "QMouseEvent":
    """Build a QMouseEvent with the modern (non-deprecated) constructor."""
    return QMouseEvent(
        etype,
        QPointF(300.0, y),
        QPointF(300.0, y),
        btn,
        buttons,
        Qt.KeyboardModifier.NoModifier,
    )


# -- hit_test_top ----------------------------------------------------


def test_hit_test_top_within_tolerance(qtbot) -> None:
    canvas = _canvas(qtbot, [_top("A", 1005.0, "t-a"), _top("B", 1015.0, "t-b")])
    # depth window = 1000..1020 over the plot band; a 1m delta is well
    # within 10px, so pressing near depth 1005 must hit top A.
    hit = canvas.hit_test_top(_depth_to_y(canvas, 1005.0))
    assert hit is not None and (hit.id == "t-a")


def test_hit_test_top_outside_tolerance_none(qtbot) -> None:
    canvas = _canvas(qtbot, [_top("A", 1005.0, "t-a")])
    # Press far from the top (e.g. at the band's bottom edge).
    top_band, bottom = canvas._plot_band()
    hit = canvas.hit_test_top(float(bottom))
    assert hit is None


def test_hit_test_top_nearest_wins(qtbot) -> None:
    # Two tops 0.2 m apart both within the 10px tolerance (~0.6 m at this
    # zoom): the closer one wins.
    canvas = _canvas(qtbot, [_top("A", 1005.0, "t-a"), _top("B", 1005.2, "t-b")])
    hit = canvas.hit_test_top(_depth_to_y(canvas, 1005.1))
    assert hit is not None and hit.id == "t-a"


# -- shift mode drag → signal ----------------------------------------


def test_shift_mode_getter_roundtrip(qtbot) -> None:
    canvas = _canvas(qtbot, [])
    canvas.set_shift_mode(True)
    assert canvas.shift_mode() is True
    canvas.set_shift_mode(False)
    assert canvas.shift_mode() is False


def test_shift_drag_emits_committed(qtbot) -> None:
    canvas = _canvas(qtbot, [_top("A", 1005.0, "t-a")])
    canvas.set_shift_mode(True)
    committed: list[tuple[str, float]] = []
    canvas.depth_shift_committed.connect(
        lambda tid, nd: committed.append((tid, nd))
    )
    canvas.resize(600, 400)
    # Press near top A's y (depth 1005), drag down to depth ~1015, release.
    press_y = _depth_to_y(canvas, 1005.0)
    release_y = _depth_to_y(canvas, 1015.0)

    canvas.mousePressEvent(
        _mouse_ev(
            QMouseEvent.Type.MouseButtonPress,
            press_y,
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.LeftButton,
        )
    )
    canvas.mouseMoveEvent(
        _mouse_ev(
            QMouseEvent.Type.MouseMove,
            release_y,
            Qt.MouseButton.NoButton,
            Qt.MouseButton.LeftButton,
        )
    )
    canvas.mouseReleaseEvent(
        _mouse_ev(
            QMouseEvent.Type.MouseButtonRelease,
            release_y,
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.LeftButton,
        )
    )

    assert len(committed) == 1
    top_id, new_depth = committed[0]
    assert top_id == "t-a"
    assert abs(new_depth - 1015.0) < 1.0  # pixel-to-depth roundtrip tolerance


def test_shift_click_without_drag_does_not_emit(qtbot) -> None:
    canvas = _canvas(qtbot, [_top("A", 1005.0, "t-a")])
    canvas.set_shift_mode(True)
    committed: list = []
    canvas.depth_shift_committed.connect(lambda *_: committed.append(1))
    press_y = _depth_to_y(canvas, 1005.0)

    canvas.mousePressEvent(
        _mouse_ev(
            QMouseEvent.Type.MouseButtonPress,
            press_y,
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.LeftButton,
        )
    )
    canvas.mouseReleaseEvent(
        _mouse_ev(
            QMouseEvent.Type.MouseButtonRelease,
            press_y,
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.LeftButton,
        )
    )
    assert committed == []


def test_non_shift_mode_drag_is_pan_no_emit(qtbot) -> None:
    canvas = _canvas(qtbot, [_top("A", 1005.0, "t-a")])
    committed: list = []
    canvas.depth_shift_committed.connect(lambda *_: committed.append(1))

    canvas.mousePressEvent(
        _mouse_ev(
            QMouseEvent.Type.MouseButtonPress,
            200.0,
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.LeftButton,
        )
    )
    canvas.mouseMoveEvent(
        _mouse_ev(
            QMouseEvent.Type.MouseMove,
            250.0,
            Qt.MouseButton.NoButton,
            Qt.MouseButton.LeftButton,
        )
    )
    canvas.mouseReleaseEvent(
        _mouse_ev(
            QMouseEvent.Type.MouseButtonRelease,
            250.0,
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.LeftButton,
        )
    )
    assert committed == []


def test_shift_mode_paint_smoke_with_drag_preview(qtbot) -> None:
    canvas = _canvas(qtbot, [_top("A", 1005.0, "t-a")])
    canvas.set_shift_mode(True)
    canvas._shift_top = canvas._tops[0]
    canvas._shift_drag_depth = 1012.0
    img = canvas.grab()
    assert img.width() == 600


# -- shell commit path (tops.json edit + undo) -----------------------


def test_shell_depth_shift_commits_and_undoes(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.tops_model import load_tops_for_well
    from well_log_workstation.workspace import create_workspace

    def _las(path: Path, well: str) -> Path:
        path.write_text(
            f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1020.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 20
1010 50
1020 80
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

    # Seed a top and enter shift mode (QAction.setChecked does not trigger,
    # so call trigger() to exercise the real toggle path).
    win.add_top_at_depth(wid, "T1", 1005.0)
    tops, _ = load_tops_for_well(ws, wid)
    top_id = tops[0].id
    assert win._act_depth_shift.isEnabled()
    win._act_depth_shift.trigger()
    assert win.multi_track_canvas.shift_mode() is True

    # Commit a drag via the shell handler.
    win._on_canvas_depth_shift_committed(top_id, 1012.0)
    tops2, _ = load_tops_for_well(ws, wid)
    assert any(abs(t.depth - 1012.0) < 1e-6 for t in tops2)

    # Undo restores the original depth.
    assert win.undo_tops_edit(wid) is True
    tops3, _ = load_tops_for_well(ws, wid)
    assert any(abs(t.depth - 1005.0) < 1e-6 for t in tops3)
