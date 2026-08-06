"""T4: Virtualized table projection + Graphic|Table mode (#344)."""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest
from PySide6.QtCore import Qt

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.display_set import (
    default_checks,
    leaf_id_for_curve,
    leaves_from_document,
)
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.table_projection import (
    SOFT_COLUMN_TIP_THRESHOLD,
    LogTableModel,
    build_table_projections,
)
from well_log_workstation.template_model import get_builtin_template
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
WELL. T4
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
RHOB.G/C3
CAL.IN
~ASCII
1000 20 2 2.2 8
1001 30 5 2.3 8
1002 40 10 2.4 8
1003 50 20 2.5 8
1004 60 50 2.6 8
""",
        encoding="utf-8",
    )
    return path


def _doc(tmp_path: Path):
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "t.las"))
    return ws, result


def test_build_projection_depth_plus_checked_columns(tmp_path: Path) -> None:
    _, result = _doc(tmp_path)
    doc = result.document
    tpl = get_builtin_template("std-gr-rt-den")
    assert tpl is not None
    checks = default_checks(leaves_from_document(doc), tpl)
    projs = build_table_projections(doc, checks, tpl)
    assert len(projs) == 1
    p = projs[0]
    assert p.column_count == 1 + 3  # Depth + GR RT DEN
    assert p.row_count == doc.depth.size
    # On-demand cells
    assert p.cell(0, 0) == pytest.approx(1000.0)
    assert p.header(0).startswith("Depth")
    assert p.header(1) in ("GR", "RT", "DEN")


def test_model_virtualized_no_full_grid_state(tmp_path: Path) -> None:
    _, result = _doc(tmp_path)
    doc = result.document
    tpl = get_builtin_template("std-gr-rt-den")
    assert tpl is not None
    checks = default_checks(leaves_from_document(doc), tpl)
    proj = build_table_projections(doc, checks, tpl)[0]
    model = LogTableModel()
    model.set_projection(proj)
    assert model.rowCount() == proj.row_count
    assert model.columnCount() == proj.column_count
    # No dense grid kept as model state
    assert not hasattr(model, "_grid")
    assert not hasattr(model, "_data")
    # data() works without preallocation
    assert model.data(model.index(0, 0)) is not None
    # materialize is explicit test helper only
    grid = model.materialize_full_grid()
    assert grid.shape == (proj.row_count, proj.column_count)


def test_split_tables_when_curve_length_differs(tmp_path: Path) -> None:
    """Different sample counts → separate projections (no resample)."""
    from well_log_workstation.las_import import ImportedCurve, ImportedWellDocument

    depth = np.arange(5, dtype=np.float64)
    short = np.array([1.0, 2.0, 3.0], dtype=np.float64)
    doc = ImportedWellDocument(
        document_id="d1",
        well_name="X",
        source_path="x.las",
        depth=depth,
        depth_unit="m",
        curves=[
            ImportedCurve(
                mnemonic="GR",
                unit="GAPI",
                values=depth * 10,
                null_mask=np.zeros(5, dtype=bool),
            ),
            ImportedCurve(
                mnemonic="SHORT",
                unit="",
                values=short,
                null_mask=np.zeros(3, dtype=bool),
            ),
        ],
    )
    tpl = get_builtin_template("std-gr-rt-den")
    assert tpl is not None
    checks = {
        leaf_id_for_curve("d1", "GR"),
        leaf_id_for_curve("d1", "SHORT"),
    }
    projs = build_table_projections(doc, checks, tpl)
    assert len(projs) == 2
    lens = sorted(p.row_count for p in projs)
    assert lens == [3, 5]


def test_soft_column_tip_threshold() -> None:
    assert SOFT_COLUMN_TIP_THRESHOLD == 64


def test_shell_mode_switch_keeps_display_set(qtbot, tmp_path: Path) -> None:
    ws, result = _doc(tmp_path)
    well_id = result.catalog_well_id
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    win._selected_well_id = well_id
    win.apply_template_to_well(well_id, "std-gr-rt-den")
    ds_before = win.display_set_for(well_id)

    assert win.view_mode_for(well_id) == "graphic"
    win.set_view_mode("table")
    assert win._view_mode == "table"
    assert win.view_mode_stack.currentIndex() == 1
    assert win.display_set_for(well_id) == ds_before
    assert win._primary_table_model.columnCount() >= 2

    win.set_view_mode("graphic")
    assert win.view_mode_stack.currentIndex() == 0
    assert win.display_set_for(well_id) == ds_before


def test_shell_table_columns_follow_display_set(qtbot, tmp_path: Path) -> None:
    ws, result = _doc(tmp_path)
    well_id = result.catalog_well_id
    doc = result.document
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(doc)
    win._selected_well_id = well_id
    gr = leaf_id_for_curve(doc.document_id, "GR")
    win.set_display_set(well_id, {gr}, template_id="std-gr-rt-den")
    win.set_view_mode("table")
    # Depth + GR only
    assert win._primary_table_model.columnCount() == 2
    assert "GR" in str(
        win._primary_table_model.headerData(1, Qt.Orientation.Horizontal)
    )


def test_shell_per_well_mode_memory(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    r1 = import_las_into_workspace(ws, _write_las(tmp_path / "a.las"))
    # second well
    r2 = import_las_into_workspace(ws, _write_las(tmp_path / "b.las"))
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(r1.document)
    win.session.put(r2.document)

    win._selected_well_id = r1.catalog_well_id
    win.apply_template_to_well(r1.catalog_well_id, "std-gr-rt-den")
    win.set_view_mode("table")
    assert win.view_mode_for(r1.catalog_well_id) == "table"

    win._selected_well_id = r2.catalog_well_id
    win.set_view_mode(win.view_mode_for(r2.catalog_well_id))
    assert win._view_mode == "graphic"

    win._selected_well_id = r1.catalog_well_id
    win.set_view_mode(win.view_mode_for(r1.catalog_well_id))
    assert win._view_mode == "table"
