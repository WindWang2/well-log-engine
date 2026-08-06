"""T5: Graph↔table semantic selection (#345 / ADR 0024)."""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest
from PySide6.QtCore import Qt

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.display_set import leaf_id_for_curve
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.semantic_selection import (
    SemanticSelection,
    nearest_sample_index,
    selection_from_depth,
    selection_from_row,
)
from well_log_workstation.shell import WellLogWorkstationWindow
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
WELL. T5
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
~ASCII
1000 20 2
1001 30 5
1002 40 10
1003 50 20
1004 60 50
""",
        encoding="utf-8",
    )
    return path


def test_nearest_sample_and_selection_identity() -> None:
    depth = np.array([1000.0, 1001.0, 1002.0, 1003.0, 1004.0])
    assert nearest_sample_index(depth, 1001.4) == 1
    sel = selection_from_depth(
        well_id="w1",
        depth=depth,
        reference_depth=1002.1,
        curve_mnemonic="GR",
        leaf_id="doc:GR",
    )
    assert sel is not None
    assert sel.well_id == "w1"
    assert sel.sample_index == 2
    assert sel.reference_depth == pytest.approx(1002.0)
    assert sel.curve_mnemonic == "GR"
    # Not screen coords
    assert not hasattr(sel, "pixel_y")
    assert not hasattr(sel, "display_depth")

    sel2 = selection_from_row(
        well_id="w1", depth=depth, sample_index=3, curve_mnemonic="RT"
    )
    assert sel2 is not None
    assert sel2.sample_index == 3
    assert sel2.reference_depth == pytest.approx(1003.0)


def test_shell_round_trip_table_to_canvas(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "t.las"))
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    well_id = result.catalog_well_id
    win._selected_well_id = well_id
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    win.set_view_mode("table")

    depth = np.asarray(result.document.depth, dtype=np.float64)
    sel = selection_from_row(
        well_id=well_id,
        depth=depth,
        sample_index=2,
        curve_mnemonic="GR",
        leaf_id=leaf_id_for_curve(result.document.document_id, "GR"),
    )
    assert sel is not None
    win.apply_semantic_selection(sel)

    assert win.semantic_selection is not None
    assert win.semantic_selection.sample_index == 2
    assert win.semantic_selection.reference_depth == pytest.approx(1002.0)
    assert win.multi_track_canvas.selection_depth() == pytest.approx(1002.0)

    # Table row selected
    rows = win._primary_table_view.selectionModel().selectedRows()
    assert len(rows) == 1
    assert rows[0].row() == 2


def test_shell_canvas_depth_selects_table_row(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "t.las"))
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    well_id = result.catalog_well_id
    win._selected_well_id = well_id
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    win.set_view_mode("table")

    # Simulate graphic pick via shell handler (no pixel events)
    win._on_canvas_sample_selected(1003.2)
    sel = win.semantic_selection
    assert sel is not None
    assert sel.sample_index == 3
    assert sel.reference_depth == pytest.approx(1003.0)
    rows = win._primary_table_view.selectionModel().selectedRows()
    assert rows[0].row() == 3


def test_mode_switch_preserves_selection(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "t.las"))
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    well_id = result.catalog_well_id
    win._selected_well_id = well_id
    win.apply_template_to_well(well_id, "std-gr-rt-den")

    sel = SemanticSelection(
        well_id=well_id,
        sample_index=1,
        reference_depth=1001.0,
        curve_mnemonic="GR",
    )
    win.apply_semantic_selection(sel)
    win.set_view_mode("table")
    assert win.semantic_selection is not None
    assert win.semantic_selection.sample_index == 1
    win.set_view_mode("graphic")
    assert win.semantic_selection.sample_index == 1
    assert win.multi_track_canvas.selection_depth() == pytest.approx(1001.0)
