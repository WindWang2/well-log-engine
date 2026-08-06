"""Unified left tree: 井 → 数据源 → 井道 (no dual tabs)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication, QTreeWidgetItem

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.display_set import leaf_id_for_curve
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path, *, single: bool = False) -> Path:
    if single:
        body = """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1002.0
STEP.M 1.0
NULL. -999.25
WELL. SINGLE-1
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 20
1001 30
1002 40
"""
    else:
        body = """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1003.0
STEP.M 1.0
NULL. -999.25
WELL. MULTI-T3
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
RHOB.G/C3
CAL.IN
AT10.OHMM
AT90.OHMM
~ASCII
1000 20 2 2.2 8 1 9
1001 30 5 2.3 8 2 10
1002 40 10 2.4 8 3 11
1003 50 20 2.5 8 4 12
"""
    path.write_text(body, encoding="utf-8")
    return path


def _open_win(qtbot, tmp_path: Path, *, single: bool = False):
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(
        ws, _write_las(tmp_path / ("s.las" if single else "m.las"), single=single)
    )
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    win._selected_well_id = result.catalog_well_id
    win._refresh_tree()
    return win, result


def _leaf_items(tree) -> list[QTreeWidgetItem]:
    out: list[QTreeWidgetItem] = []

    def walk(item: QTreeWidgetItem) -> None:
        data = item.data(0, Qt.ItemDataRole.UserRole) or {}
        if data.get("kind") == "leaf":
            out.append(item)
        for i in range(item.childCount()):
            walk(item.child(i))

    for i in range(tree.topLevelItemCount()):
        walk(tree.topLevelItem(i))
    return out


def _source_items(tree) -> list[QTreeWidgetItem]:
    out: list[QTreeWidgetItem] = []

    def walk(item: QTreeWidgetItem) -> None:
        data = item.data(0, Qt.ItemDataRole.UserRole) or {}
        if data.get("kind") == "source":
            out.append(item)
        for i in range(item.childCount()):
            walk(item.child(i))

    for i in range(tree.topLevelItemCount()):
        walk(tree.topLevelItem(i))
    return out


def test_no_dual_tabs_single_workspace_tree(qtbot, tmp_path: Path) -> None:
    win, _ = _open_win(qtbot, tmp_path)
    assert getattr(win, "left_tabs", None) is None
    assert win.workspace_tree.objectName() == "WorkspaceTree"
    assert win.well_content_tree is win.workspace_tree
    assert win.workspace_tree.findItems(
        "井", Qt.MatchFlag.MatchContains | Qt.MatchFlag.MatchRecursive
    )
    assert win.workspace_tree.findItems(
        "图件", Qt.MatchFlag.MatchContains | Qt.MatchFlag.MatchRecursive
    )


def test_content_under_well_two_levels(qtbot, tmp_path: Path) -> None:
    win, _ = _open_win(qtbot, tmp_path, single=False)
    tree = win.workspace_tree
    sources = _source_items(tree)
    assert len(sources) >= 1
    leaves = _leaf_items(tree)
    mnemos = {
        (c.data(0, Qt.ItemDataRole.UserRole) or {}).get("mnemonic") for c in leaves
    }
    assert "GR" in mnemos and "AT10" in mnemos and "AT90" in mnemos
    assert "DEPT" not in mnemos

    win2, _ = _open_win(qtbot, tmp_path / "s", single=True)
    assert len(_leaf_items(win2.workspace_tree)) == 1


def test_check_leaf_live_updates_plot(qtbot, tmp_path: Path) -> None:
    win, result = _open_win(qtbot, tmp_path)
    well_id = result.catalog_well_id
    doc = result.document
    win.set_display_set(well_id, frozenset(), template_id="std-gr-rt-den")
    win._refresh_tree()
    assert win.active_presentation is not None
    assert win.active_presentation.curve_track_count == 0

    gr_id = leaf_id_for_curve(doc.document_id, "GR")
    leaves = _leaf_items(win.workspace_tree)
    gr_item = next(
        c
        for c in leaves
        if (c.data(0, Qt.ItemDataRole.UserRole) or {}).get("id") == gr_id
    )
    gr_item.setCheckState(0, Qt.CheckState.Checked)
    QApplication.processEvents()
    pres = win.active_presentation
    assert pres is not None
    assert pres.curve_track_count == 1
    assert gr_id in (win.display_set_for(well_id) or frozenset())


def test_parent_source_batch_toggle(qtbot, tmp_path: Path) -> None:
    win, result = _open_win(qtbot, tmp_path)
    well_id = result.catalog_well_id
    win.set_display_set(well_id, frozenset(), template_id="std-gr-rt-den")
    win._refresh_tree()
    sources = _source_items(win.workspace_tree)
    assert sources
    src = sources[0]
    n_leaves = len(_leaf_items(win.workspace_tree))
    src.setCheckState(0, Qt.CheckState.Checked)
    QApplication.processEvents()
    ds = win.display_set_for(well_id) or frozenset()
    assert len(ds) == n_leaves
    assert win.active_presentation is not None
    assert win.active_presentation.curve_track_count == n_leaves


def test_derived_track_list_matches_display_set(qtbot, tmp_path: Path) -> None:
    win, result = _open_win(qtbot, tmp_path)
    well_id = result.catalog_well_id
    doc = result.document
    gr = leaf_id_for_curve(doc.document_id, "GR")
    cal = leaf_id_for_curve(doc.document_id, "CAL")
    win.set_display_set(well_id, {gr, cal}, template_id="std-gr-rt-den")
    texts = [
        win.track_list.item(i).text() for i in range(win.track_list.count())
    ]
    joined = " ".join(texts)
    assert "GR" in joined
    assert "CAL" in joined
