"""Correlation well order + column gap persistence (#295 / T7)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path, well: str) -> Path:
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
1000 10 1
1001 20 2
1002 30 3
1003 40 4
""",
        encoding="utf-8",
    )
    return path


def test_reorder_wells_and_gap_persist(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="CorrLayout")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    plot = win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    assert plot.well_ids == [id1, id2]
    assert win.correlation_canvas.column_count() == 2
    assert win.corr_well_list.count() == 2
    assert win.correlation_canvas.column_gap() == 6

    # Select first well and move down → order becomes [id2, id1]
    win.corr_well_list.setCurrentRow(0)
    win._move_correlation_well(1)
    assert win.correlation_canvas.columns()[0].well_document_id == id2
    assert win.correlation_canvas.columns()[1].well_document_id == id1

    win.corr_gap_spin.setValue(24)
    assert win.correlation_canvas.column_gap() == 24

    loaded = load_plot_document(ws, plot.id)
    assert loaded.well_ids == [id2, id1]
    assert loaded.column_gap_px == 24

    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(ws)
    win2.open_plot_document(plot.id)
    assert [c.well_document_id for c in win2.correlation_canvas.columns()] == [
        id2,
        id1,
    ]
    assert win2.correlation_canvas.column_gap() == 24
    assert win2.corr_gap_spin.value() == 24


def test_column_gap_clamped_on_canvas() -> None:
    from well_log_workstation.correlation_canvas import CorrelationCanvas

    c = CorrelationCanvas()
    c.set_column_gap(-5)
    assert c.column_gap() == 0
    c.set_column_gap(999)
    assert c.column_gap() == 200
