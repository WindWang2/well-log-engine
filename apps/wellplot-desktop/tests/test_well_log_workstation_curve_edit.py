"""Non-destructive curve edits (FRS §2.x 曲线编辑 Despike / 基线平移).

Covers the pure-numpy despike (median + MAD threshold) and baseline
algorithms, JSON + file persistence, the edit dialog, and the shell
``_apply_curve_edits`` runtime attach (``edited-*`` tracks coexist with
``derived-*`` formula tracks).
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QMouseEvent

from well_log_workstation.curve_edit import (
    CurveEdit,
    append_freehand_points,
    apply_baseline,
    apply_curve_edits,
    apply_freehand,
    despike,
    edits_from_json,
    edits_to_json,
    load_curve_edits_for_well,
    save_curve_edits_for_well,
)


# -- despike algorithm ----------------------------------------------


def test_despike_removes_spike() -> None:
    vals = np.array([1.0, 1.0, 50.0, 1.0, 1.0])
    out, mask = despike(vals, np.zeros(5, bool), window=3, threshold=3.0)
    assert out[2] == 1.0  # spike replaced by local median
    assert not mask.any()


def test_despike_keeps_null() -> None:
    vals = np.array([1.0, np.nan, 50.0, 1.0, 1.0])
    mask = ~np.isfinite(vals)
    out, m2 = despike(vals, mask, window=3, threshold=3.0)
    assert bool(m2[1]) is True  # null preserved
    assert np.isnan(out[1])


def test_despike_flat_line_unchanged() -> None:
    flat = np.ones(9)
    out, _ = despike(flat, np.zeros(9, bool), window=3, threshold=3.0)
    np.testing.assert_array_equal(out, flat)


def test_despike_no_mutation_of_input() -> None:
    vals = np.array([1.0, 1.0, 50.0, 1.0, 1.0])
    orig = vals.copy()
    despike(vals, np.zeros(5, bool), window=3, threshold=3.0)
    np.testing.assert_array_equal(vals, orig)


# -- baseline --------------------------------------------------------


def test_baseline_shifts_values_keeps_null() -> None:
    vals = np.array([1.0, 2.0, np.nan, 4.0])
    mask = ~np.isfinite(vals)
    out, m2 = apply_baseline(vals, mask, 10.0)
    np.testing.assert_allclose(out, [11.0, 12.0, np.nan, 14.0])
    assert bool(m2[2]) is True


# -- ordered application ---------------------------------------------


def test_apply_curve_edits_sequential() -> None:
    depth = _depth_axis(5, start=1000.0, step=1.0)
    vals = np.array([1.0, 1.0, 50.0, 1.0, 1.0])
    edits = [
        CurveEdit(mnemonic="GR", method="despike", window=3, threshold=3.0),
        CurveEdit(mnemonic="GR", method="baseline", shift=100.0),
    ]
    out, _ = apply_curve_edits(depth, vals, np.zeros(5, bool), edits)
    np.testing.assert_allclose(out, [101.0] * 5)


# -- JSON roundtrip --------------------------------------------------


def test_edits_json_roundtrip() -> None:
    edits = [
        CurveEdit(mnemonic="GR", method="despike", window=5, threshold=2.5),
        CurveEdit(mnemonic="RT", method="baseline", shift=-1.5),
    ]
    back = edits_from_json(edits_to_json(edits))
    assert [(e.mnemonic, e.method, e.window, e.shift) for e in back] == [
        ("GR", "despike", 5, 0.0),
        ("RT", "baseline", 5, -1.5),
    ]


def test_edits_from_json_skips_invalid() -> None:
    back = edits_from_json(
        [
            {"mnemonic": "GR", "method": "despike", "window": 5, "threshold": 3},
            {"mnemonic": "", "method": "despike"},
            {"mnemonic": "RT", "method": "bogus"},
            {"mnemonic": "DEN", "method": "baseline", "shift": "x"},
        ]
    )
    assert len(back) == 1
    assert back[0].mnemonic == "GR"


# -- file persistence -------------------------------------------------


def test_curve_edits_file_roundtrip(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws")
    add_well(ws, name="A", path="wells/a", well_id="w1")
    edits = [CurveEdit(mnemonic="GR", method="despike", window=3, threshold=4.0)]
    save_curve_edits_for_well(ws, "w1", edits)
    loaded, diags = load_curve_edits_for_well(ws, "w1")
    assert not diags
    assert len(loaded) == 1
    assert loaded[0].method == "despike" and loaded[0].window == 3


def test_curve_edits_missing_file_defaults_empty(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws")
    add_well(ws, name="A", path="wells/a", well_id="w1")
    loaded, diags = load_curve_edits_for_well(ws, "w1")
    assert loaded == []
    assert diags == []


# -- shell integration ------------------------------------------------


def test_shell_curve_edit_attaches_track_and_coexists_with_formulas(
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
STOP.M 1010.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
~ASCII
1000 1 50
1001 1 51
1002 50 52
1003 1 53
1004 1 54
1005 1 55
1006 1 56
1007 1 57
1008 1 58
1009 1 59
1010 1 60
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

    # Save a despike edit directly, then re-apply.
    save_curve_edits_for_well(
        ws,
        wid,
        [CurveEdit(mnemonic="GR", method="despike", window=3, threshold=3.0)],
    )
    applied, diags = win._apply_curve_edits()
    assert applied == 1
    assert not diags
    edited = next(
        (t for t in win._presentation.tracks if str(t.id).startswith("edited-")),
        None,
    )
    assert edited is not None
    assert edited.title == "GR 校正"
    assert edited.layers[0].values[2] == 1.0  # spike removed
    assert edited.layers[0].color == "#10b981"

    # Formulas-derived tracks coexist: apply a formula, both survive.
    from well_log_workstation.formula import Formula
    from well_log_workstation.tops_model import save_formulas_for_well

    save_formulas_for_well(
        ws, wid, [Formula(name="VSH", expression="(GR - 1) / (50 - 1)")]
    )
    win._apply_derived_curves()
    win._apply_curve_edits()
    ids = {t.id for t in win._presentation.tracks}
    assert any(i.startswith("edited-") for i in ids)
    assert any(i.startswith("derived-") for i in ids)


def test_shell_curve_edit_action_enabled_with_workspace(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    assert win._act_curve_edit.isEnabled() is False
    win.set_workspace(ws)
    assert win._act_curve_edit.isEnabled() is True


def test_canvas_paints_edited_track(qtbot) -> None:
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas
    from well_log_workstation.template_model import (
        BoundCurveLayer,
        BoundTrack,
        HostPresentation,
        ScaleSpec,
    )

    depth = np.array([1000.0, 1010.0, 1020.0])
    track = BoundTrack(
        id="edited-GR",
        role="curve",
        title="GR 校正",
        width_fraction=0.25,
        scale=ScaleSpec(min=0.0, max=60.0),
        layers=[
            BoundCurveLayer(
                mnemonic="GR",
                color="#10b981",
                unit="gapi",
                values=np.array([1.0, 1.0, 1.0]),
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


# -- freehand: apply_freehand ---------------------------------------


def _depth_axis(n: int = 11, start: float = 1000.0, step: float = 1.0) -> np.ndarray:
    return start + np.arange(n, dtype=np.float64) * step


def test_apply_freehand_interpolates_inside_range() -> None:
    depth = _depth_axis()
    vals = np.zeros(11)
    out, _ = apply_freehand(depth, vals, np.zeros(11, bool),
                            [(1002.0, 10.0), (1005.0, 10.0)])
    np.testing.assert_allclose(out[2:6], [10.0] * 4)  # 1002..1005 → 10
    assert out[0] == 0.0 and out[10] == 0.0  # outside unchanged


def test_apply_freehand_linear_interp_midpoint() -> None:
    depth = _depth_axis()
    out, _ = apply_freehand(depth, np.zeros(11), np.zeros(11, bool),
                            [(1000.0, 0.0), (1002.0, 10.0)])
    assert out[1] == 5.0  # depth 1001 → mid value


def test_apply_freehand_keeps_null_and_single_point_noop() -> None:
    depth = _depth_axis()
    mask = np.zeros(11, bool)
    mask[3] = True
    out, m2 = apply_freehand(depth, np.zeros(11), mask,
                             [(1002.0, 10.0), (1005.0, 10.0)])
    assert bool(m2[3]) is True
    assert np.isnan(out[3]) or out[3] == 0.0  # null untouched
    out3, _ = apply_freehand(depth, np.zeros(11), np.zeros(11, bool),
                             [(1002.0, 10.0)])
    assert np.array_equal(out3, np.zeros(11))  # < 2 points no-op


# -- freehand: JSON + merge ------------------------------------------


def test_freehand_json_roundtrip() -> None:
    e = CurveEdit(mnemonic="GR", method="freehand",
                  points=((1002.0, 10.0), (1005.0, 12.0)))
    back = edits_from_json(edits_to_json([e]))[0]
    assert back.method == "freehand"
    assert back.points == ((1002.0, 10.0), (1005.0, 12.0))


def test_freehand_json_missing_points_defaults_empty() -> None:
    back = edits_from_json([{"mnemonic": "GR", "method": "freehand"}])
    assert len(back) == 1 and back[0].points == ()


def test_append_freehand_points_merges() -> None:
    a = append_freehand_points(None, "GR", [(1000.0, 1.0), (1001.0, 2.0)])
    b = append_freehand_points(a, "GR", [(1002.0, 3.0)])
    assert len(b.points) == 3
    assert b.points[-1] == (1002.0, 3.0)


def test_apply_curve_edits_chains_freehand_after_despike() -> None:
    depth = _depth_axis(5, start=1000.0, step=1.0)
    vals = np.array([1.0, 1.0, 50.0, 1.0, 1.0])
    edits = [
        CurveEdit(mnemonic="GR", method="despike", window=3, threshold=3.0),
        CurveEdit(mnemonic="GR", method="freehand",
                  points=((1001.0, 100.0), (1003.0, 100.0))),
    ]
    out, _ = apply_curve_edits(depth, vals, np.zeros(5, bool), edits)
    # despike removes the spike, then freehand overwrites 1001..1003 → 100.
    np.testing.assert_allclose(out[1:4], [100.0] * 3)
    assert out[0] == 1.0 and out[4] == 1.0


# -- freehand: canvas gesture ----------------------------------------


def _mouse_ev2(etype, x, y, btn, buttons) -> "QMouseEvent":
    return QMouseEvent(
        etype, QPointF(x, y), QPointF(x, y), btn, buttons,
        Qt.KeyboardModifier.NoModifier,
    )


def _curve_canvas(qtbot):
    from well_log_workstation.multi_track_canvas import MultiTrackCanvas
    from well_log_workstation.template_model import (
        BoundCurveLayer,
        BoundTrack,
        HostPresentation,
        ScaleSpec,
    )

    canvas = MultiTrackCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    depth = np.array([1000.0, 1010.0, 1020.0, 1030.0])
    track = BoundTrack(
        id="gr", role="curve", title="GR", width_fraction=1.0,
        scale=ScaleSpec(min=0.0, max=100.0),
        layers=[BoundCurveLayer(mnemonic="GR", color="#1a6fb5", unit="gapi",
                                values=np.array([20., 40., 60., 80.]),
                                null_mask=np.zeros(4, bool))],
    )
    pres = HostPresentation(template_id="t", template_name="t",
                            well_document_id="w", well_name="W",
                            depth=depth, depth_unit="m", tracks=[track])
    canvas.set_presentation(pres)
    return canvas


def test_draw_curve_gesture_emits_committed(qtbot) -> None:
    canvas = _curve_canvas(qtbot)
    canvas.set_draw_curve_mode(True)
    committed: list[tuple[str, list]] = []
    canvas.curve_drawn_committed.connect(
        lambda m, pts: committed.append((m, list(pts)))
    )
    top_band, _ = canvas._plot_band()
    y1 = top_band + 10
    canvas.mousePressEvent(
        _mouse_ev2(QMouseEvent.Type.MouseButtonPress, 300.0, y1,
                   Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )
    canvas.mouseMoveEvent(
        _mouse_ev2(QMouseEvent.Type.MouseMove, 310.0, y1 + 5,
                   Qt.MouseButton.NoButton, Qt.MouseButton.LeftButton)
    )
    canvas.mouseMoveEvent(
        _mouse_ev2(QMouseEvent.Type.MouseMove, 320.0, y1 + 10,
                   Qt.MouseButton.NoButton, Qt.MouseButton.LeftButton)
    )
    canvas.mouseReleaseEvent(
        _mouse_ev2(QMouseEvent.Type.MouseButtonRelease, 320.0, y1 + 10,
                   Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )
    assert len(committed) == 1
    mnemonic, pts = committed[0]
    assert mnemonic == "GR"
    assert len(pts) >= 3
    # Points are (depth, value) pairs with finite values.
    assert all(np.isfinite(d) and np.isfinite(v) for d, v in pts)


def test_draw_curve_esc_cancels(qtbot) -> None:
    canvas = _curve_canvas(qtbot)
    canvas.set_draw_curve_mode(True)
    committed: list = []
    canvas.curve_drawn_committed.connect(lambda *_: committed.append(1))
    top_band, _ = canvas._plot_band()
    y1 = top_band + 10
    from PySide6.QtGui import QKeyEvent

    canvas.mousePressEvent(
        _mouse_ev2(QMouseEvent.Type.MouseButtonPress, 300.0, y1,
                   Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )
    canvas.keyPressEvent(
        QKeyEvent(QKeyEvent.Type.KeyPress, Qt.Key.Key_Escape,
                  Qt.KeyboardModifier.NoModifier)
    )
    canvas.mouseReleaseEvent(
        _mouse_ev2(QMouseEvent.Type.MouseButtonRelease, 300.0, y1,
                   Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )
    assert committed == []


def test_draw_curve_mode_paint_smoke(qtbot) -> None:
    canvas = _curve_canvas(qtbot)
    canvas.set_draw_curve_mode(True)
    top_band, _ = canvas._plot_band()
    y1 = top_band + 10
    canvas.mousePressEvent(
        _mouse_ev2(QMouseEvent.Type.MouseButtonPress, 300.0, y1,
                   Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )
    canvas.mouseMoveEvent(
        _mouse_ev2(QMouseEvent.Type.MouseMove, 320.0, y1 + 10,
                   Qt.MouseButton.NoButton, Qt.MouseButton.LeftButton)
    )
    img = canvas.grab()
    assert img.width() == 600
    canvas.mouseReleaseEvent(
        _mouse_ev2(QMouseEvent.Type.MouseButtonRelease, 320.0, y1 + 10,
                   Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton)
    )


# -- freehand: shell integration ------------------------------------


def test_shell_freehand_commit_merges_and_applies(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace

    def _las(path: Path, well: str) -> Path:
        path.write_text(
            f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1010.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 20
1001 21
1002 22
1003 23
1004 24
1005 25
1006 26
1007 27
1008 28
1009 29
1010 30
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
    win._selected_well_id = wid

    # Commit a freehand stroke over depth 1002..1005 → value 10.
    win._on_curve_drawn_committed(
        "GR", [(1002.0, 10.0), (1003.0, 10.0), (1005.0, 10.0)]
    )
    edits, diags = load_curve_edits_for_well(ws, wid)
    assert not diags
    free = [e for e in edits if e.method == "freehand" and e.mnemonic == "GR"]
    assert len(free) == 1
    assert len(free[0].points) == 3

    edited = next(
        (t for t in win._presentation.tracks if str(t.id).startswith("edited-")),
        None,
    )
    assert edited is not None
    assert edited.title == "GR 校正"
    # depth 1002 → 10 (value at index 2), depth 1005 → 10 (index 5).
    assert edited.layers[0].values[2] == 10.0
    assert edited.layers[0].values[5] == 10.0
    # outside the drawn range unchanged (index 0 → 20).
    assert edited.layers[0].values[0] == 20.0

    # Second stroke merges into the same freehand edit.
    win._on_curve_drawn_committed("GR", [(1006.0, 5.0), (1007.0, 5.0)])
    edits2, _ = load_curve_edits_for_well(ws, wid)
    free2 = [e for e in edits2 if e.method == "freehand" and e.mnemonic == "GR"]
    assert len(free2) == 1
    assert len(free2[0].points) == 5


def test_shell_draw_curve_action_mutually_exclusive(qtbot, tmp_path: Path) -> None:
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
    wid = win.import_las_path(_las(tmp_path / "a.las", "A"))

    from well_log_workstation.plot_document import create_single_well_plot

    plot = create_single_well_plot(
        ws, well_id=wid, well_name="A", template_id="std-gr-rt-den"
    )
    win.open_plot_document(plot.id)
    win._selected_well_id = wid
    win.add_top_at_depth(wid, "T1", 1001.0)

    # Enable draw-curve, then depth-shift must turn it off and vice versa.
    win._act_draw_curve.trigger()
    assert win.multi_track_canvas.draw_curve_mode() is True
    win._act_depth_shift.trigger()
    assert win.multi_track_canvas.shift_mode() is True
    assert win.multi_track_canvas.draw_curve_mode() is False
    win._act_pick_tops.trigger()
    assert win.multi_track_canvas.pick_mode() is True
    assert win.multi_track_canvas.shift_mode() is False
