"""CompositeView — 油藏综合图 host surface (Phase-2, T7 / #251).

Hosts a ``CartographyLayoutWindow`` scene (paper sheet + chrome +
FigurePanelGraphicsItem panels) inside a QGraphicsView. Subscribes to
``plot_bus.plot_changed`` and refreshes the panel whose ``source_plot_id``
matches (live: update() - the QGraphicsProxyWidget repaints itself;
snapshot: re-grab the source widget pixmap).

GL/engine source plots (section / fence_3d) must use snapshot mode; the
host wires the pixmap via ``panel.set_snapshot_pixmap(source.grab())``.
"""

from __future__ import annotations

import shutil
from pathlib import Path
from typing import Any

from PySide6.QtCore import QRectF, Qt
from PySide6.QtWidgets import (
    QComboBox,
    QGraphicsView,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from well_log_workstation.events import plot_bus
from well_log_workstation.plot_document import PanelRef
from well_log_workstation.workspace import Workspace


class CompositeView(QWidget):
    """Paper layout for the composite figure; refreshes embedded panels."""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("CompositeView")
        self._workspace: Workspace | None = None
        self._layout_window: Any | None = None
        self._source_widgets: dict[str, Any] = {}  # plot_id -> source widget
        self._active_plot_id: str | None = None

        root = QVBoxLayout(self)
        bar = QHBoxLayout()
        bar.addWidget(QLabel("来源图件："))
        self._source_combo = QComboBox()
        bar.addWidget(self._source_combo, 1)
        self._render_combo = QComboBox()
        self._render_combo.addItems(["live", "snapshot"])
        bar.addWidget(QLabel("渲染："))
        bar.addWidget(self._render_combo)
        add_btn = QPushButton("添加面板")
        add_btn.clicked.connect(self._on_add_panel)
        bar.addWidget(add_btn)
        save_btn = QPushButton("保存布局")
        save_btn.setToolTip("把当前纸面排版（面板位置 + 图形标注）写入图件文件")
        save_btn.clicked.connect(self.save_layout)
        bar.addWidget(save_btn)
        root.addLayout(bar)

        # Paper view
        self._view_host = QWidget()
        vh = QVBoxLayout(self._view_host)
        vh.setContentsMargins(0, 0, 0, 0)
        self._paper_view: QGraphicsView | None = None
        root.addWidget(self._view_host, 1)

        # Subscribe to the plot bus (T7 dynamic refresh).
        plot_bus.plot_changed.connect(self._on_plot_changed)

    # -- workspace wiring ----------------------------------------------

    def set_workspace(self, ws: Workspace | None) -> None:
        """Bind the workspace; populate the panel source palette."""
        self._workspace = ws
        self._source_combo.clear()
        if ws is None:
            return
        plot_ids = [p.id for p in ws.plots if p.type != "composite"]
        self._source_combo.addItems(plot_ids)
        self._ensure_layout_window()
        if self._layout_window is not None:
            self._layout_window.set_plot_sources(plot_ids)

    def register_source_widget(self, plot_id: str, widget: Any) -> None:
        """Register the live widget for a source plot (for snapshot grabs)."""
        self._source_widgets[plot_id] = widget

    # -- layout window --------------------------------------------------

    def _ensure_layout_window(self):
        """Lazily construct the cartography layout window's scene/view."""
        if self._layout_window is not None:
            return self._layout_window
        try:
            from geoviz_paleo_map.cartography.window import (
                CartographyLayoutWindow,
            )
        except Exception:
            return None
        win = CartographyLayoutWindow()
        self._layout_window = win
        # Re-parent the paper view into this widget.
        view = win.view()
        view.setParent(self._view_host)
        self._view_host.layout().addWidget(view)
        self._paper_view = view
        if self._workspace is not None:
            win.set_plot_sources(
                [p.id for p in self._workspace.plots if p.type != "composite"]
            )
        return win

    # -- panels ---------------------------------------------------------

    def _on_add_panel(self) -> None:
        source = self._source_combo.currentText()
        if not source:
            return
        win = self._ensure_layout_window()
        if win is None:
            return
        render_mode = self._render_combo.currentText()
        source_type = "single_well"
        if self._workspace is not None:
            entry = next(
                (p for p in self._workspace.plots if p.id == source), None
            )
            if entry is not None:
                source_type = entry.type
        item = win.add_figure_panel(
            source,
            source_plot_type=source_type,
            render_mode=render_mode,
        )
        # Snapshot mode: grab the source widget now (if registered).
        if render_mode == "snapshot":
            widget = self._source_widgets.get(source)
            if widget is not None:
                try:
                    item.set_snapshot_pixmap(widget.grab())
                except Exception:
                    pass

    def figure_panels(self):
        win = self._layout_window
        if win is None:
            return []
        return win.figure_panels()

    def add_panel_ref(self, panel: PanelRef) -> None:
        """Add a panel from a persisted PanelRef (open_plot_document path)."""
        win = self._ensure_layout_window()
        if win is None:
            return
        rect = None
        if panel.rect_mm is not None and len(panel.rect_mm) == 4:
            rect = QRectF(*panel.rect_mm)
        item = win.add_figure_panel(
            panel.plot_id,
            source_plot_type=panel.source_plot_type,
            render_mode=panel.render_mode,
            rect_mm=rect,
        )
        if panel.render_mode == "snapshot":
            widget = self._source_widgets.get(panel.plot_id)
            if widget is not None:
                try:
                    item.set_snapshot_pixmap(widget.grab())
                except Exception:
                    pass

    # -- save / restore (free-graphics host, Task 12) ------------------

    def set_active_plot_id(self, plot_id: str) -> None:
        """Remember which composite plot the paper currently shows."""
        self._active_plot_id = plot_id

    def save_layout(self) -> None:
        """Persist the current paper layout (panels + free graphics).

        Loads the active plot document, reconciles the persisted panel
        refs against the live scene geometry (the scene wins), copies
        absolute-path image assets into ``plots/assets/<plot_id>/``, and
        writes the document back.
        """
        if self._workspace is None or self._active_plot_id is None:
            return
        win = self._layout_window
        if win is None:
            return
        # Lazy import: plot_document's schema is being extended in
        # parallel; import here so module import order never blocks this
        # view and the latest fields are picked up at call time.
        from well_log_workstation.plot_document import (
            PanelRef,
            load_plot_document,
            save_plot_document,
        )
        try:
            doc = load_plot_document(self._workspace, self._active_plot_id)
        except Exception as exc:
            QMessageBox.warning(
                self,
                "保存布局",
                f"无法读取当前综合图，布局未保存。\n{exc}",
            )
            return
        doc.panels = [
            PanelRef(**panel)
            for panel in reconcile_panels(doc.panels, win.panels())
        ]
        if hasattr(doc, "free_graphics"):
            doc.free_graphics = rewrite_image_paths(
                win.free_graphics(), self._workspace.root, doc.id
            )
        save_plot_document(self._workspace, doc)

    def restore_free_graphics(self, records) -> int:
        """Restore persisted free-graphic records onto the paper.

        ``win.add_free_graphic`` validates each record and returns None
        for unknown kinds / malformed records; returns the failure count
        so the host can report how many records could not be restored.
        """
        win = self._ensure_layout_window()
        if win is None:
            return len(records)
        failed = 0
        for record in records:
            if win.add_free_graphic(record) is None:
                failed += 1
        return failed

    # -- refresh --------------------------------------------------------

    def _on_plot_changed(self, plot_id: str, revision: int) -> None:
        """Refresh the panel whose source plot changed (T7)."""
        for panel in self.figure_panels():
            if panel.source_plot_id != plot_id:
                continue
            if panel.render_mode == "snapshot":
                widget = self._source_widgets.get(plot_id)
                if widget is not None:
                    try:
                        panel.set_snapshot_pixmap(widget.grab())
                    except Exception:
                        pass
            else:
                panel.refresh()  # live proxy repaints itself; update() only


# ---------------------------------------------------------------------------
# Pure helpers (no Qt): importable with plain /usr/bin/python3 for verification.
# ---------------------------------------------------------------------------


def reconcile_panels(doc_panels, scene_panels) -> list[dict]:
    """Merge persisted panel refs with the live scene geometry.

    The paper scene is the source of truth for layout: every scene panel
    updates the matching persisted panel (keyed by ``plot_id`` + ``slot``);
    scene panels missing from the doc are appended; doc panels no longer on
    the paper are dropped. ``doc_panels`` may be ``PanelRef`` objects or
    plain dicts; the result is always a list of plain dicts in doc-then-new
    order.
    """
    def _key(panel):
        if isinstance(panel, dict):
            return (panel.get("plot_id"), panel.get("slot") or "main")
        return (panel.plot_id, getattr(panel, "slot", "main") or "main")

    scene = {_key(p): p for p in scene_panels}
    merged: list[dict] = []
    seen: set[tuple] = set()
    for panel in doc_panels:
        key = _key(panel)
        if key in scene:
            merged.append(scene[key])
            seen.add(key)
    for key, panel in scene.items():
        if key not in seen:
            merged.append(panel)
    return merged


def rewrite_image_paths(records, workspace_root, plot_id) -> list:
    """Copy absolute-path images into the plot asset dir; return rewritten copies.

    Image records hold their pixel source by path. On save the host copies
    any absolute-path image into ``plots/assets/<plot_id>/`` and rewrites
    ``props["path"]`` to that relative path so the workspace stays
    self-contained. Non-image records and records whose path is already
    relative (or unresolvable) pass through untouched; the input list is
    never mutated.
    """
    root = Path(workspace_root)
    out = []
    for record in records:
        if not isinstance(record, dict) or record.get("kind") != "image":
            out.append(record)
            continue
        props = record.get("props") or {}
        src = props.get("path") if isinstance(props, dict) else None
        if not isinstance(src, str) or not src:
            out.append(record)
            continue
        src_path = Path(src)
        if not src_path.is_absolute() or not src_path.is_file():
            out.append(record)
            continue
        dest_dir = root / "plots" / "assets" / str(plot_id)
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = dest_dir / src_path.name
        shutil.copy2(src_path, dest)
        new_record = dict(record)
        new_props = dict(props)
        new_props["path"] = str(dest.relative_to(root))
        new_record["props"] = new_props
        out.append(new_record)
    return out
