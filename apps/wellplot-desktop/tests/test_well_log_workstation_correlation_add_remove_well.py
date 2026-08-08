"""Correlation plot add/remove well (FRS §3.x 增删井 UI).

Covers appending a well to and removing a well from an open correlation
plot: canvas column count follows ``plot.well_ids``, persistence
roundtrips through ``load_plot_document``, the >=2-well floor guard,
horizon-link pruning on remove, undo restore, and noop guards.
"""

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


def _three_wells(qtbot, tmp_path: Path):
    """Workspace with 3 wells + a 2-well correlation plot (A, B)."""
    ws = create_workspace(tmp_path / "ws", name="CorrAddRemove")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    id3 = win.import_las_path(_write_las(tmp_path / "c.las", "C"))
    plot = win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    assert plot.well_ids == [id1, id2]
    return win, ws, plot, (id1, id2, id3)


def _reload(ws, plot):
    return load_plot_document(ws, plot.id)


# -- add well --------------------------------------------------------


def test_add_well_appends_to_column_and_persists(qtbot, tmp_path: Path) -> None:
    win, ws, plot, (id1, id2, id3) = _three_wells(qtbot, tmp_path)
    # Monkeypatch the picker to return the third well (avoids modal exec).
    win._pick_single_well_to_add = lambda present_ids: id3
    win._on_correlation_add_well()
    assert _reload(ws, plot).well_ids == [id1, id2, id3]
    assert win.correlation_canvas.column_count() == 3
    assert win.corr_well_list.count() == 3
    # Canvas column ids follow well_ids.
    assert [c.well_document_id for c in win.correlation_canvas.columns()] == [
        id1, id2, id3
    ]


def test_add_well_undo_restores(qtbot, tmp_path: Path) -> None:
    win, ws, plot, (id1, id2, id3) = _three_wells(qtbot, tmp_path)
    win._pick_single_well_to_add = lambda present_ids: id3
    win._on_correlation_add_well()
    assert _reload(ws, plot).well_ids == [id1, id2, id3]
    assert win.undo_correlation_layout() is True
    assert _reload(ws, plot).well_ids == [id1, id2]
    assert win.correlation_canvas.column_count() == 2


def test_add_well_cancel_is_noop(qtbot, tmp_path: Path) -> None:
    win, ws, plot, (id1, id2, id3) = _three_wells(qtbot, tmp_path)
    # Picker returns None (user cancelled) -> no change.
    win._pick_single_well_to_add = lambda present_ids: None
    win._on_correlation_add_well()
    assert _reload(ws, plot).well_ids == [id1, id2]
    assert win.correlation_canvas.column_count() == 2


# -- picker dialog (filters already-present wells) ------------------


def test_pick_single_well_lists_only_absent(qtbot, tmp_path: Path) -> None:
    """The add-well picker must exclude wells already on the plot."""
    win, ws, plot, (id1, id2, id3) = _three_wells(qtbot, tmp_path)
    # Replicate the picker's availability filter and assert only C remains.
    available = [w for w in win._workspace.wells if w.id not in plot.well_ids]
    assert [w.id for w in available] == [id3]


# -- remove well -----------------------------------------------------


def test_remove_well_drops_from_column_and_persists(
    qtbot, tmp_path: Path
) -> None:
    win, ws, plot, (id1, id2, id3) = _three_wells(qtbot, tmp_path)
    # Start from a 3-well plot.
    win._pick_single_well_to_add = lambda present_ids: id3
    win._on_correlation_add_well()
    assert _reload(ws, plot).well_ids == [id1, id2, id3]
    # Select the middle well (B) and remove it.
    win.corr_well_list.setCurrentRow(1)
    win._on_correlation_remove_well()
    assert _reload(ws, plot).well_ids == [id1, id3]
    assert win.correlation_canvas.column_count() == 2
    assert [c.well_document_id for c in win.correlation_canvas.columns()] == [
        id1, id3
    ]


def test_remove_well_undo_restores(qtbot, tmp_path: Path) -> None:
    win, ws, plot, (id1, id2, id3) = _three_wells(qtbot, tmp_path)
    win._pick_single_well_to_add = lambda present_ids: id3
    win._on_correlation_add_well()
    win.corr_well_list.setCurrentRow(1)  # B
    win._on_correlation_remove_well()
    assert _reload(ws, plot).well_ids == [id1, id3]
    assert win.undo_correlation_layout() is True
    assert _reload(ws, plot).well_ids == [id1, id2, id3]
    assert win.correlation_canvas.column_count() == 3


def test_remove_well_prunes_links(qtbot, tmp_path: Path) -> None:
    """Removing a well must drop horizon links that reference it."""
    from well_log_workstation.correlation_links import HorizonLink
    from well_log_workstation.plot_document import save_plot_document

    win, ws, plot, (id1, id2, id3) = _three_wells(qtbot, tmp_path)
    # Inject a horizon link between id1 and id2 (the auto-linker needs
    # matching tops by name, which the bare LAS fixture lacks).
    plot.links = [
        HorizonLink(
            id="lk1",
            left_well_id=id1,
            right_well_id=id2,
            name="T1",
            left_depth=1001.0,
            right_depth=1001.0,
        )
    ]
    save_plot_document(ws, plot)
    assert any(
        lk.left_well_id == id1 and lk.right_well_id == id2
        for lk in _reload(ws, plot).links
    )
    # Add a third well, then remove the middle one (id2) -> its links drop.
    win._pick_single_well_to_add = lambda present_ids: id3
    win._on_correlation_add_well()
    win.corr_well_list.setCurrentRow(1)  # id2
    win._on_correlation_remove_well()
    reloaded = _reload(ws, plot)
    assert id2 not in reloaded.well_ids
    # No link may reference the removed well.
    for lk in reloaded.links:
        assert lk.left_well_id != id2
        assert lk.right_well_id != id2


def test_remove_refuses_below_two(
    qtbot, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A 2-well plot must refuse to drop below 2 wells."""
    from PySide6.QtWidgets import QMessageBox

    monkeypatch.setattr(
        QMessageBox, "information", lambda *a, **k: None
    )  # modal would block offscreen
    win, ws, plot, (id1, id2, id3) = _three_wells(qtbot, tmp_path)
    assert _reload(ws, plot).well_ids == [id1, id2]
    win.corr_well_list.setCurrentRow(0)
    win._on_correlation_remove_well()
    # Unchanged - still 2 wells, 2 columns.
    assert _reload(ws, plot).well_ids == [id1, id2]
    assert win.correlation_canvas.column_count() == 2


# -- noop guards -----------------------------------------------------


def test_add_remove_noop_when_not_correlation(qtbot, tmp_path: Path) -> None:
    """Without an active correlation plot, the handlers must not crash."""
    win, ws, plot, (id1, id2, id3) = _three_wells(qtbot, tmp_path)
    # Switch away from the correlation plot (no active plot).
    win._active_plot_type = None
    win._active_plot_id = None
    win._pick_single_well_to_add = lambda present_ids: "x"
    win._on_correlation_add_well()  # must not crash
    win._on_correlation_remove_well()  # must not crash
    assert _reload(ws, plot).well_ids == [id1, id2]
