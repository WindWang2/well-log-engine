"""Left navigation tree: context-menu host + data→plot drag/drop.

Mime payload is JSON under ``MIME_NAV``:
  ``{"kind":"leaf","leaf_id":"...","well_id":"..."}``
  ``{"kind":"data","well_id":"...","leaf_ids":["..."]}``
"""

from __future__ import annotations

import json
from typing import Any

from PySide6.QtCore import QByteArray, QMimeData, QPoint, Qt, Signal
from PySide6.QtGui import QDrag, QDropEvent
from PySide6.QtWidgets import QAbstractItemView, QTreeWidget, QTreeWidgetItem

MIME_NAV = "application/x-wellplot-nav"


def payload_from_item(item: QTreeWidgetItem) -> dict[str, Any] | None:
    """Build a drag payload from a data-side tree item (leaf or source)."""
    data = item.data(0, Qt.ItemDataRole.UserRole) or {}
    kind = data.get("kind")
    if kind == "leaf":
        leaf_id = str(data.get("id") or "").strip()
        well_id = str(data.get("well_id") or "").strip()
        if not leaf_id or not well_id:
            return None
        return {"kind": "leaf", "leaf_id": leaf_id, "well_id": well_id}
    if kind == "source":
        well_id = str(data.get("well_id") or "").strip()
        if not well_id:
            return None
        leaf_ids: list[str] = []
        for i in range(item.childCount()):
            cdata = item.child(i).data(0, Qt.ItemDataRole.UserRole) or {}
            if cdata.get("kind") == "leaf":
                lid = str(cdata.get("id") or "").strip()
                if lid:
                    leaf_ids.append(lid)
        return {"kind": "data", "well_id": well_id, "leaf_ids": leaf_ids}
    return None


def resolve_plot_drop_target(
    item: QTreeWidgetItem | None,
) -> tuple[str | None, str | None]:
    """Return (plot_id, well_id_hint) for a drop target under 图件."""
    cur = item
    while cur is not None:
        data = cur.data(0, Qt.ItemDataRole.UserRole) or {}
        kind = data.get("kind")
        if kind == "plot":
            return str(data.get("id") or "") or None, str(data.get("well_id") or "") or None
        if kind in ("plot_track", "plot_well"):
            pid = str(data.get("plot_id") or "") or None
            wid = str(data.get("well_id") or "") or None
            if pid:
                return pid, wid
        if kind == "plots_folder":
            return None, None
        cur = cur.parent()
    return None, None


def encode_payload(payload: dict[str, Any]) -> QMimeData:
    mime = QMimeData()
    raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    mime.setData(MIME_NAV, QByteArray(raw))
    mime.setText(json.dumps(payload, ensure_ascii=False))
    return mime


def decode_payload(mime: QMimeData) -> dict[str, Any] | None:
    if mime is None or not mime.hasFormat(MIME_NAV):
        return None
    try:
        raw = bytes(mime.data(MIME_NAV)).decode("utf-8")
        data = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError):
        return None
    return data if isinstance(data, dict) else None


class NavTreeWidget(QTreeWidget):
    """Tree that drags data units/leaves and accepts drops on plot nodes."""

    nav_drop = Signal(dict, str)  # payload, plot_id

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setDragEnabled(True)
        self.setAcceptDrops(True)
        self.setDropIndicatorShown(True)
        self.setDragDropMode(QAbstractItemView.DragDropMode.DragDrop)
        self.setDefaultDropAction(Qt.DropAction.CopyAction)
        self.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)

    def startDrag(self, supportedActions: Qt.DropAction) -> None:  # noqa: N802
        items = self.selectedItems()
        if not items:
            return
        payload = payload_from_item(items[0])
        if not payload:
            return
        if payload.get("kind") == "data" and not payload.get("leaf_ids"):
            return
        drag = QDrag(self)
        drag.setMimeData(encode_payload(payload))
        drag.exec(Qt.DropAction.CopyAction)

    def dragEnterEvent(self, event) -> None:  # noqa: N802
        if event.mimeData() is not None and event.mimeData().hasFormat(MIME_NAV):
            event.acceptProposedAction()
        else:
            event.ignore()

    def dragMoveEvent(self, event) -> None:  # noqa: N802
        if event.mimeData() is None or not event.mimeData().hasFormat(MIME_NAV):
            event.ignore()
            return
        pos = event.position().toPoint() if hasattr(event, "position") else event.pos()
        item = self.itemAt(pos)
        plot_id, _ = resolve_plot_drop_target(item)
        if plot_id:
            event.acceptProposedAction()
        else:
            event.ignore()

    def dropEvent(self, event: QDropEvent) -> None:  # noqa: N802
        payload = decode_payload(event.mimeData())
        if not payload:
            event.ignore()
            return
        pos = event.position().toPoint() if hasattr(event, "position") else event.pos()
        item = self.itemAt(pos)
        plot_id, _ = resolve_plot_drop_target(item)
        if not plot_id:
            event.ignore()
            return
        self.nav_drop.emit(payload, plot_id)
        event.acceptProposedAction()

    def global_pos_for_menu(self, pos: QPoint) -> QPoint:
        return self.viewport().mapToGlobal(pos)
