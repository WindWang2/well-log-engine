"""Context menu actions + data→plot drag payload / drop binding."""

from __future__ import annotations

import os
from pathlib import Path

import pytest
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QTreeWidgetItem

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.display_set import leaf_id_for_curve
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.nav_tree import (
    MIME_NAV,
    decode_payload,
    encode_payload,
    payload_from_item,
    resolve_plot_drop_target,
)
from well_log_workstation.plot_document import create_single_well_plot
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path) -> Path:
    path.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1002.0
STEP.M 1.0
NULL. -999.25
WELL. NAV-1
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
~ASCII
1000 20 2
1001 30 5
1002 40 10
""",
        encoding="utf-8",
    )
    return path


def _open(qtbot, tmp_path: Path):
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "a.las"))
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    win._selected_well_id = result.catalog_well_id
    win._refresh_tree()
    return win, result


def _items(tree, kind: str) -> list[QTreeWidgetItem]:
    out: list[QTreeWidgetItem] = []

    def walk(item: QTreeWidgetItem) -> None:
        d = item.data(0, Qt.ItemDataRole.UserRole) or {}
        if d.get("kind") == kind:
            out.append(item)
        for i in range(item.childCount()):
            walk(item.child(i))

    for i in range(tree.topLevelItemCount()):
        walk(tree.topLevelItem(i))
    return out


def test_payload_and_resolve_helpers(qtbot, tmp_path: Path) -> None:
    win, result = _open(qtbot, tmp_path)
    well_id = result.catalog_well_id
    doc = result.document
    gr = leaf_id_for_curve(doc.document_id, "GR")
    plot = create_single_well_plot(
        win.workspace,
        well_id=well_id,
        well_name="NAV-1",
        name="目标图",
        template_id="std-gr-rt-den",
    )
    win._refresh_tree()

    leaf = next(
        i
        for i in _items(win.workspace_tree, "leaf")
        if (i.data(0, Qt.ItemDataRole.UserRole) or {}).get("id") == gr
    )
    payload = payload_from_item(leaf)
    assert payload == {"kind": "leaf", "leaf_id": gr, "well_id": well_id}

    mime = encode_payload(payload)
    assert mime.hasFormat(MIME_NAV)
    assert decode_payload(mime) == payload

    source = _items(win.workspace_tree, "source")[0]
    data_payload = payload_from_item(source)
    assert data_payload is not None
    assert data_payload["kind"] == "data"
    assert gr in data_payload["leaf_ids"]

    plot_item = next(
        i
        for i in _items(win.workspace_tree, "plot")
        if (i.data(0, Qt.ItemDataRole.UserRole) or {}).get("id") == plot.id
    )
    pid, _ = resolve_plot_drop_target(plot_item)
    assert pid == plot.id

    # Dropping on plots folder is not a plot target
    folder = win.workspace_tree.topLevelItem(1)
    assert resolve_plot_drop_target(folder) == (None, None)


def test_drop_handler_binds_leaf(qtbot, tmp_path: Path) -> None:
    win, result = _open(qtbot, tmp_path)
    well_id = result.catalog_well_id
    doc = result.document
    gr = leaf_id_for_curve(doc.document_id, "GR")
    plot = create_single_well_plot(
        win.workspace,
        well_id=well_id,
        well_name="NAV-1",
        name="拖放图",
        template_id="std-gr-rt-den",
    )
    win.set_display_set(well_id, frozenset(), template_id="std-gr-rt-den", plot_id=plot.id)
    win._on_nav_drop(
        {"kind": "leaf", "leaf_id": gr, "well_id": well_id}, plot.id
    )
    ds = win.display_set_for(well_id) or frozenset()
    assert gr in ds
    assert win.active_presentation is not None
    assert win.active_presentation.curve_track_count >= 1


def test_context_add_and_remove(qtbot, tmp_path: Path) -> None:
    win, result = _open(qtbot, tmp_path)
    well_id = result.catalog_well_id
    doc = result.document
    gr = leaf_id_for_curve(doc.document_id, "GR")
    rt = leaf_id_for_curve(doc.document_id, "RT")
    plot = create_single_well_plot(
        win.workspace,
        well_id=well_id,
        well_name="NAV-1",
        name="右键图",
        template_id="std-gr-rt-den",
    )
    win._ctx_add_leaf(gr, well_id, plot.id)
    win._ctx_add_leaf(rt, well_id, plot.id)
    assert gr in (win.display_set_for(well_id) or frozenset())
    win._ctx_remove_plot_track(gr, well_id, plot.id)
    ds = win.display_set_for(well_id) or frozenset()
    assert gr not in ds
    assert rt in ds


def test_tree_has_context_menu_policy(qtbot, tmp_path: Path) -> None:
    win, _ = _open(qtbot, tmp_path)
    assert (
        win.workspace_tree.contextMenuPolicy()
        == Qt.ContextMenuPolicy.CustomContextMenu
    )
    from well_log_workstation.nav_tree import NavTreeWidget

    assert isinstance(win.workspace_tree, NavTreeWidget)
