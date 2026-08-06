"""Correlation datum flatten + layout undo (#296 / T8)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.tops_model import FormationTop, save_tops_for_well
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path, well: str) -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1004.0
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
1004 50 5
""",
        encoding="utf-8",
    )
    return path


def test_horizon_flatten_aligns_named_tops(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="Datum")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    # Same top name at different MD → horizon flatten should zero both.
    save_tops_for_well(
        ws,
        id1,
        [FormationTop(name="T1", depth=1001.0, id="t1a")],
    )
    save_tops_for_well(
        ws,
        id2,
        [FormationTop(name="T1", depth=1002.5, id="t1b")],
    )
    plot = win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    win.set_correlation_datum(mode="horizon", horizon="T1", persist=True)

    shifts = win.correlation_canvas.depth_shifts()
    assert shifts[id1] == pytest.approx(-1001.0)
    assert shifts[id2] == pytest.approx(-1002.5)
    # Display depth of T1 is 0 for both wells
    assert 1001.0 + shifts[id1] == pytest.approx(0.0)
    assert 1002.5 + shifts[id2] == pytest.approx(0.0)

    loaded = load_plot_document(ws, plot.id)
    assert loaded.datum_mode == "horizon"
    assert loaded.datum_horizon == "T1"

    # Undo restores md
    assert win.undo_correlation_layout()
    loaded2 = load_plot_document(ws, plot.id)
    assert loaded2.datum_mode == "md"
    assert win.correlation_canvas.depth_shifts().get(id1, 0.0) == pytest.approx(0.0)


def test_link_change_is_undoable(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws2", name="LinksUndo")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "c.las", "C"))
    id2 = win.import_las_path(_write_las(tmp_path / "d.las", "D"))
    save_tops_for_well(
        ws, id1, [FormationTop(name="T1", depth=1001.0, id="x1")]
    )
    save_tops_for_well(
        ws, id2, [FormationTop(name="T1", depth=1001.2, id="x2")]
    )
    plot = win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    # Opening may auto-create links; clear then auto again to exercise undo.
    n_before = len(win.correlation_canvas.links())
    win.clear_correlation_links()
    assert len(win.correlation_canvas.links()) == 0
    win.auto_link_correlation_tops()
    assert len(win.correlation_canvas.links()) >= 1
    assert win.undo_correlation_layout()
    # After undo of auto-link, back to cleared
    assert len(win.correlation_canvas.links()) == 0
    loaded = load_plot_document(ws, plot.id)
    assert loaded.links == []
    _ = n_before
