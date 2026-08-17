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


def test_snapshot_preserves_unit_id_and_semantic() -> None:
    """#599: undo/redo snapshots must not drop unit_id / semantic."""
    from well_log_workstation.tops_model import FormationTop

    tops = [
        FormationTop(
            name="T1", depth=100.0, id="t1", unit_id="u-42", semantic="fault"
        )
    ]
    snap = snapshot_tops(tops)
    assert snap[0].unit_id == "u-42"
    assert snap[0].semantic == "fault"
    assert snap[0].id == "t1"


def test_tops_json_roundtrip_preserves_unit_id_and_semantic(tmp_path: Path) -> None:
    """#599: save/load must carry unit_id and semantic through."""
    from well_log_workstation.tops_model import (
        FormationTop,
        load_tops_for_well,
        save_tops_for_well,
    )
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws-top")
    from well_log_workstation.workspace import add_well

    well = add_well(ws, name="W1", path="wells/w1/data.las", well_id="well-1")
    (ws.root / "wells" / "w1").mkdir(parents=True)
    tops = [
        FormationTop(
            name="CSG",
            depth=1001.0,
            id="00000000-0000-0000-0000-000000000001",
            unit_id="u-7",
            semantic="casing_shoe",
        )
    ]
    save_tops_for_well(ws, well.id, tops)
    loaded, diags = load_tops_for_well(ws, well.id)
    assert not diags
    assert loaded[0].unit_id == "u-7"
    assert loaded[0].semantic == "casing_shoe"


def test_undo_save_failure_keeps_history(qtbot, tmp_path: Path, monkeypatch) -> None:
    """#743: OSError from save_tops_for_well must not drop the undo snapshot."""
    from PySide6.QtWidgets import QMessageBox

    import well_log_workstation.shell as shell_mod

    ws = create_workspace(tmp_path / "ws-undo-fail")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "u.las"))
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    win.add_top_at_depth(well_id, "T1", 1001.0)
    assert win._tops_history.can_undo(well_id)
    assert not win._tops_history.can_redo(well_id)

    warnings: list[tuple] = []
    monkeypatch.setattr(
        QMessageBox, "warning", lambda *a, **k: warnings.append(a)
    )

    def _boom(*_a, **_k):
        raise OSError("disk full")

    monkeypatch.setattr(shell_mod, "save_tops_for_well", _boom)
    assert win.undo_tops_edit(well_id) is False
    assert win._tops_history.can_undo(well_id)
    assert not win._tops_history.can_redo(well_id)
    assert warnings
    loaded, _ = load_tops_for_well(ws, well_id)
    assert len(loaded) == 1
    assert loaded[0].name == "T1"


def test_redo_save_failure_keeps_history(qtbot, tmp_path: Path, monkeypatch) -> None:
    """#743: redo save failure must restore the redo stack."""
    from PySide6.QtWidgets import QMessageBox

    import well_log_workstation.shell as shell_mod

    ws = create_workspace(tmp_path / "ws-redo-fail")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "r.las"))
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    win.add_top_at_depth(well_id, "T1", 1001.0)
    win.add_top_at_depth(well_id, "T2", 1002.0)
    assert win.undo_tops_edit(well_id)
    assert win._tops_history.can_redo(well_id)

    warnings: list[tuple] = []
    monkeypatch.setattr(
        QMessageBox, "warning", lambda *a, **k: warnings.append(a)
    )

    def _boom(*_a, **_k):
        raise OSError("readonly")

    monkeypatch.setattr(shell_mod, "save_tops_for_well", _boom)
    assert win.redo_tops_edit(well_id) is False
    assert win._tops_history.can_redo(well_id)
    assert warnings
    loaded, _ = load_tops_for_well(ws, well_id)
    assert [t.name for t in loaded] == ["T1"]


def test_set_top_depth_preserves_unit_id(qtbot, tmp_path: Path) -> None:
    """#599: set_top_depth rebuilds the top with every field intact."""
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.tops_model import (
        FormationTop,
        load_tops_for_well,
        save_tops_for_well,
    )
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws-depth")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "d.las"))
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    save_tops_for_well(
        ws,
        well_id,
        [
            FormationTop(
                name="T1",
                depth=1000.0,
                id="00000000-0000-0000-0000-000000000001",
                unit_id="u-7",
                semantic="formation_top",
            )
        ],
    )
    updated = win.set_top_depth(
        well_id, "00000000-0000-0000-0000-000000000001", 1005.0
    )
    assert updated.unit_id == "u-7"
    assert updated.semantic == "formation_top"
    tops, diags = load_tops_for_well(ws, well_id)
    assert not diags
    assert tops[0].depth == 1005.0
    assert tops[0].unit_id == "u-7"
    assert tops[0].semantic == "formation_top"
