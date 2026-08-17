"""Single-well multi-track depth pan/zoom viewport."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path) -> Path:
    path.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1100.0
STEP.M 10.0
NULL. -999.25
WELL. VP-1
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
~ASCII
1000 10 1
1010 20 2
1020 30 3
1030 40 4
1040 50 5
1050 60 6
1060 70 7
1070 80 8
1080 90 9
1090 100 10
1100 110 11
""",
        encoding="utf-8",
    )
    return path


def test_single_well_depth_pan_zoom_reset(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    las = _write_las(tmp_path / "v.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    canvas = win.multi_track_canvas

    full = canvas.depth_range()
    assert full is not None
    f0, f1 = full
    assert f1 > f0
    assert f0 == pytest.approx(1000.0)
    assert f1 == pytest.approx(1100.0)

    canvas.set_depth_range(1020.0, 1060.0)
    assert canvas.depth_range() == pytest.approx((1020.0, 1060.0))

    # Zoom-like shrink around mid
    canvas.set_depth_range(1030.0, 1050.0)
    assert canvas.depth_range() == pytest.approx((1030.0, 1050.0))

    canvas.reset_depth_range()
    again = canvas.depth_range()
    assert again is not None
    assert again[0] == pytest.approx(1000.0)
    assert again[1] == pytest.approx(1100.0)


def test_invalid_depth_range_ignored(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws2")
    las = _write_las(tmp_path / "v2.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    canvas = win.multi_track_canvas
    before = canvas.depth_range()
    canvas.set_depth_range(50.0, 50.0)  # invalid
    assert canvas.depth_range() == before


def test_depth_track_labels_use_step_precision(qtbot, tmp_path: Path, monkeypatch):
    """#739: sub-unit ticks must not all render as the same integer label."""
    from well_log_workstation import multi_track_canvas as mtc

    ws = create_workspace(tmp_path / "ws-zoom")
    las = _write_las(tmp_path / "vz.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    canvas = win.multi_track_canvas
    seen: list[tuple[float, float]] = []
    orig = mtc.format_depth_label

    def rec(value, step):
        seen.append((float(value), float(step)))
        return orig(value, step)

    monkeypatch.setattr(mtc, "format_depth_label", rec)
    canvas.set_depth_range(1000.0, 1004.0)
    canvas.resize(400, 600)
    canvas.grab()
    assert seen
    steps = {round(s, 6) for _v, s in seen}
    assert any(s < 1.0 for s in steps)


def _dummy_presentation(well_id: str = "w1") -> object:
    import numpy as np

    from well_log_workstation.template_model import (
        BoundCurveLayer,
        BoundTrack,
        HostPresentation,
        ScaleSpec,
    )

    depth = np.array([1000.0, 1050.0, 1100.0])
    vals = np.array([10.0, 20.0, 30.0])
    return HostPresentation(
        template_id="t",
        template_name="T",
        well_document_id=well_id,
        well_name=well_id,
        depth=depth,
        depth_unit="m",
        tracks=[
            BoundTrack(
                id="c",
                role="curve",
                title="GR",
                width_fraction=1.0,
                scale=ScaleSpec(min=0.0, max=100.0, mode="linear", unit="API"),
                layers=[
                    BoundCurveLayer(
                        mnemonic="GR",
                        color="#1a6fb5",
                        unit="API",
                        values=vals,
                        null_mask=np.zeros(3, dtype=bool),
                    )
                ],
            )
        ],
    )


def _wheel_at(widget, x: float, y: float, delta: int = 120) -> None:
    from PySide6.QtCore import QPoint, QPointF, Qt
    from PySide6.QtGui import QWheelEvent

    pos = QPointF(x, y)
    event = QWheelEvent(
        pos,
        pos,
        QPoint(0, 0),
        QPoint(0, delta),
        Qt.MouseButton.NoButton,
        Qt.KeyboardModifier.NoModifier,
        Qt.ScrollPhase.NoScrollPhase,
        False,
    )
    widget.wheelEvent(event)


def test_correlation_wheel_keeps_cursor_depth(qtbot) -> None:
    """#732: zoom must keep the depth under the cursor, not the window mid."""
    from well_log_workstation.correlation_canvas import CorrelationCanvas

    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_columns([_dummy_presentation("a"), _dummy_presentation("b")])
    canvas.set_depth_range(1000.0, 1100.0)
    top, bottom = 36, canvas.height() - 24
    y = top + 0.25 * (bottom - top)
    before = canvas.depth_at_y(y)
    assert before == pytest.approx(1025.0)
    _wheel_at(canvas, 300.0, y, delta=120)
    after = canvas.depth_at_y(y)
    assert after == pytest.approx(before, abs=1e-6)


def test_section_wheel_keeps_cursor_depth(qtbot) -> None:
    """#732 sibling: section canvas must also cursor-anchor wheel zoom."""
    from well_log_workstation.section_canvas import SectionCanvas

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)
    canvas.set_section([_dummy_presentation("a"), _dummy_presentation("b")])
    canvas.set_depth_range(1000.0, 1100.0)
    top, bottom = 36, canvas.height() - 24
    y = top + 0.25 * (bottom - top)
    before = canvas.depth_at_y(y)
    assert before == pytest.approx(1025.0)
    _wheel_at(canvas, 300.0, y, delta=120)
    after = canvas.depth_at_y(y)
    assert after == pytest.approx(before, abs=1e-6)
