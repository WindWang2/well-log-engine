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
