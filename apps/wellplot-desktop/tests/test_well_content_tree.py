"""Left tree: 数据 inventory + 图件 plot→tracks (no dual tabs)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication, QTreeWidgetItem

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.display_set import leaf_id_for_curve
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.plot_document import create_single_well_plot
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


def _items_by_kind(tree, kind: str) -> list[QTreeWidgetItem]:
    out: list[QTreeWidgetItem] = []

    def walk(item: QTreeWidgetItem) -> None:
        data = item.data(0, Qt.ItemDataRole.UserRole) or {}
        if data.get("kind") == kind:
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
    tree = win.workspace_tree
    assert tree.topLevelItemCount() == 2
    assert tree.topLevelItem(0).text(0) == "数据"
    assert tree.topLevelItem(1).text(0) == "图件"
    for i in range(tree.topLevelItemCount()):
        assert "工区" not in tree.topLevelItem(i).text(0)


def test_content_under_well_two_levels(qtbot, tmp_path: Path) -> None:
    win, _ = _open_win(qtbot, tmp_path, single=False)
    tree = win.workspace_tree
    sources = _items_by_kind(tree, "source")
    assert len(sources) >= 1
    leaves = _items_by_kind(tree, "leaf")
    mnemos = {
        (c.data(0, Qt.ItemDataRole.UserRole) or {}).get("mnemonic") for c in leaves
    }
    assert "GR" in mnemos and "AT10" in mnemos and "AT90" in mnemos
    assert "DEPT" not in mnemos
    # Data leaves are not checkable (inventory only)
    for leaf in leaves:
        assert not (leaf.flags() & Qt.ItemFlag.ItemIsUserCheckable)

    win2, _ = _open_win(qtbot, tmp_path / "s", single=True)
    assert len(_items_by_kind(win2.workspace_tree, "leaf")) == 1


def test_data_unit_shows_plot_ref_not_on_leaves(qtbot, tmp_path: Path) -> None:
    """Refs are on the data unit name only, not on track leaves."""
    win, result = _open_win(qtbot, tmp_path)
    well_id = result.catalog_well_id
    doc = result.document
    gr = leaf_id_for_curve(doc.document_id, "GR")
    plot = create_single_well_plot(
        win.workspace,
        well_id=well_id,
        well_name=result.document.well_name or "W",
        name="引用测试图",
        template_id="std-gr-rt-den",
    )
    win.open_plot_document(plot.id)
    win.set_display_set(well_id, {gr}, template_id="std-gr-rt-den", plot_id=plot.id)
    win._refresh_tree()

    sources = _items_by_kind(win.workspace_tree, "source")
    assert sources
    assert any("引用测试图" in s.text(0) and "→" in s.text(0) for s in sources)

    data_leaves = _items_by_kind(win.workspace_tree, "leaf")
    gr_item = next(
        c
        for c in data_leaves
        if (c.data(0, Qt.ItemDataRole.UserRole) or {}).get("id") == gr
    )
    # Leaf stays plain mnemonic — no plot ref suffix
    assert gr_item.text(0) == "GR"
    assert "引用测试图" not in gr_item.text(0)

    plot_tracks = _items_by_kind(win.workspace_tree, "plot_track")
    assert any(
        (c.data(0, Qt.ItemDataRole.UserRole) or {}).get("id") == gr for c in plot_tracks
    )


def test_plot_tree_is_plot_to_tracks(qtbot, tmp_path: Path) -> None:
    win, result = _open_win(qtbot, tmp_path)
    well_id = result.catalog_well_id
    doc = result.document
    gr = leaf_id_for_curve(doc.document_id, "GR")
    cal = leaf_id_for_curve(doc.document_id, "CAL")
    plot = create_single_well_plot(
        win.workspace,
        well_id=well_id,
        well_name=result.document.well_name or "W",
        name="结构图",
        template_id="std-gr-rt-den",
    )
    win.set_display_set(
        well_id, {gr, cal}, template_id="std-gr-rt-den", plot_id=plot.id
    )
    win._refresh_tree()

    plots_folder = win.workspace_tree.topLevelItem(1)
    assert plots_folder.text(0) == "图件"
    assert plots_folder.childCount() >= 1
    plot_item = plots_folder.child(0)
    assert "结构图" in plot_item.text(0)
    # Children of plot are tracks (or single well group); not empty
    assert plot_item.childCount() >= 1
    track_kinds = set()
    for i in range(plot_item.childCount()):
        d = plot_item.child(i).data(0, Qt.ItemDataRole.UserRole) or {}
        track_kinds.add(d.get("kind"))
    assert "plot_track" in track_kinds or "plot_well" in track_kinds


def test_uncheck_plot_track_updates_display_set(qtbot, tmp_path: Path) -> None:
    win, result = _open_win(qtbot, tmp_path)
    well_id = result.catalog_well_id
    doc = result.document
    gr = leaf_id_for_curve(doc.document_id, "GR")
    cal = leaf_id_for_curve(doc.document_id, "CAL")
    plot = create_single_well_plot(
        win.workspace,
        well_id=well_id,
        well_name=result.document.well_name or "W",
        name="勾选图",
        template_id="std-gr-rt-den",
    )
    win.open_plot_document(plot.id)
    win.set_display_set(
        well_id, {gr, cal}, template_id="std-gr-rt-den", plot_id=plot.id
    )
    win._refresh_tree()
    assert win.active_presentation is not None
    assert win.active_presentation.curve_track_count == 2

    tracks = _items_by_kind(win.workspace_tree, "plot_track")
    gr_track = next(
        c
        for c in tracks
        if (c.data(0, Qt.ItemDataRole.UserRole) or {}).get("id") == gr
    )
    gr_track.setCheckState(0, Qt.CheckState.Unchecked)
    QApplication.processEvents()
    ds = win.display_set_for(well_id) or frozenset()
    assert gr not in ds
    assert cal in ds
    assert win.active_presentation is not None
    assert win.active_presentation.curve_track_count == 1


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
