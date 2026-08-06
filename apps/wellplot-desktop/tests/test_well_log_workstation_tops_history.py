"""Formation tops edit history: undo/redo (#294 / T6)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.tops_history import TopsHistoryBook, snapshot_tops
from well_log_workstation.tops_model import FormationTop, load_tops_for_well
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path) -> Path:
    path.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1004.0
STEP.M 1.0
NULL. -999.25
WELL. H1
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


def test_history_book_undo_redo_roundtrip() -> None:
    book = TopsHistoryBook()
    a = [FormationTop(name="A", depth=1.0, id="a")]
    b = [
        FormationTop(name="A", depth=1.0, id="a"),
        FormationTop(name="B", depth=2.0, id="b"),
    ]
    book.record_before_commit("w1", a)
    assert book.can_undo("w1")
    assert not book.can_redo("w1")
    prev = book.undo("w1", b)
    assert prev is not None
    assert len(prev) == 1
    assert prev[0].name == "A"
    assert book.can_redo("w1")
    nxt = book.redo("w1", prev)
    assert nxt is not None
    assert len(nxt) == 2


def test_shell_add_undo_redo_tops(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="TopsHist")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "h.las"))
    win.apply_template_to_well(well_id, "std-gr-rt-den")

    assert not win._tops_history.can_undo(well_id)
    t1 = win.add_top_at_depth(well_id, "T1", 1001.0)
    t2 = win.add_top_at_depth(well_id, "T2", 1002.0)
    assert len(win._active_tops) == 2
    assert win._tops_history.can_undo(well_id)

    assert win.undo_tops_edit(well_id)
    assert len(win._active_tops) == 1
    assert win._active_tops[0].name == "T1"
    loaded, _ = load_tops_for_well(ws, well_id)
    assert len(loaded) == 1

    assert win.redo_tops_edit(well_id)
    assert len(win._active_tops) == 2
    names = {t.name for t in win._active_tops}
    assert names == {"T1", "T2"}

    # Depth edit + remove
    win.set_top_depth(well_id, t1.id, 1001.5)
    assert any(
        t.id == t1.id and t.depth == pytest.approx(1001.5) for t in win._active_tops
    )
    assert win.remove_top_by_id(well_id, t2.id)
    assert all(t.id != t2.id for t in win._active_tops)
    assert win.undo_tops_edit(well_id)
    assert any(t.id == t2.id for t in win._active_tops)


def test_snapshot_independent() -> None:
    tops = [FormationTop(name="X", depth=1.0, id="x")]
    snap = snapshot_tops(tops)
    assert snap[0].name == "X"
    assert snap is not tops
