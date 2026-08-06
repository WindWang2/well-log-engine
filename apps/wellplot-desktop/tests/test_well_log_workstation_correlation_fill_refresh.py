"""T9 interwell fill + T10 single-well→correlation tops refresh."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.interwell_fill import build_interwell_fill_bands
from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.tops_model import FormationTop, load_tops_for_well, save_tops_for_well
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path, well: str) -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1005.0
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
1005 60 6
""",
        encoding="utf-8",
    )
    return path


def test_build_interwell_fill_bands_between_shared_tops() -> None:
    cols = [
        [
            FormationTop(name="T1", depth=1001.0, id="a"),
            FormationTop(name="T2", depth=1003.0, id="b"),
        ],
        [
            FormationTop(name="T1", depth=1001.5, id="c"),
            FormationTop(name="T2", depth=1003.5, id="d"),
        ],
    ]
    bands = build_interwell_fill_bands(cols)
    assert len(bands) == 1
    assert bands[0].top_name == "T1"
    assert bands[0].bottom_name == "T2"
    assert bands[0].left_col == 0
    assert bands[0].right_col == 1


def test_t10_auto_refresh_correlation_after_tops_edit(qtbot, tmp_path: Path) -> None:
    """Single-well tops edit auto-updates open correlation tops (#298)."""
    ws = create_workspace(tmp_path / "ws", name="Refresh")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    save_tops_for_well(
        ws,
        id1,
        [
            FormationTop(name="T1", depth=1001.0, id="t1a"),
            FormationTop(name="T2", depth=1003.0, id="t2a"),
        ],
    )
    save_tops_for_well(
        ws,
        id2,
        [
            FormationTop(name="T1", depth=1001.2, id="t1b"),
            FormationTop(name="T2", depth=1003.2, id="t2b"),
        ],
    )
    win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    tops0 = win.correlation_canvas.tops_per_column()
    assert any(t.depth == pytest.approx(1001.0) for t in tops0[0])

    # Edit well 1 tops while correlation is open
    win.set_top_depth(id1, "t1a", 1001.8)
    tops1 = win.correlation_canvas.tops_per_column()
    assert any(t.depth == pytest.approx(1001.8) for t in tops1[0])
    # Link depths should follow name T1
    for lk in win.correlation_canvas.links():
        if lk.name == "T1" and lk.left_well_id == id1:
            assert lk.left_depth == pytest.approx(1001.8)


def test_t9_fill_toggle_persists_and_paints(qtbot, tmp_path: Path) -> None:
    """Interwell fill checkbox persists and enables canvas fill (#297)."""
    ws = create_workspace(tmp_path / "ws2", name="Fill")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "c.las", "C"))
    id2 = win.import_las_path(_write_las(tmp_path / "d.las", "D"))
    save_tops_for_well(
        ws,
        id1,
        [
            FormationTop(name="T1", depth=1001.0, id="x1"),
            FormationTop(name="T2", depth=1003.0, id="x2"),
        ],
    )
    save_tops_for_well(
        ws,
        id2,
        [
            FormationTop(name="T1", depth=1001.5, id="y1"),
            FormationTop(name="T2", depth=1003.5, id="y2"),
        ],
    )
    plot = win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    assert win.correlation_canvas.show_interwell_fill() is False

    win.corr_fill_check.setChecked(True)
    assert win.correlation_canvas.show_interwell_fill() is True
    loaded = load_plot_document(ws, plot.id)
    assert loaded.show_interwell_fill is True

    # Manual refresh still works
    win.refresh_correlation_from_sources(reason="manual")
    assert len(win.correlation_canvas.tops_per_column()) == 2

    # Reopen restores fill flag
    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(ws)
    win2.open_plot_document(plot.id)
    assert win2.correlation_canvas.show_interwell_fill() is True
