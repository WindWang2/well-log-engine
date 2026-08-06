"""Cold-start welcome page when no workspace is open (#291 / T3)."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from well_log_workstation.branding import PRODUCT_NAME
from well_log_workstation.recent_workspaces import load_recent, remove_recent


class StartupPage(QWidget):
    """Centered welcome: new / open workspace + recent list."""

    new_requested = Signal()
    open_requested = Signal()
    recent_open_requested = Signal(str)  # absolute or stored path string

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("StartupPage")

        root = QVBoxLayout(self)
        root.setAlignment(Qt.AlignmentFlag.AlignCenter)
        root.setSpacing(16)

        title = QLabel(PRODUCT_NAME)
        title.setObjectName("StartupTitle")
        title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        font = title.font()
        font.setPointSize(18)
        font.setBold(True)
        title.setFont(font)
        root.addWidget(title)

        sub = QLabel("打开或新建工区以开始测井绘图")
        sub.setObjectName("StartupSubtitle")
        sub.setAlignment(Qt.AlignmentFlag.AlignCenter)
        root.addWidget(sub)

        btn_row = QHBoxLayout()
        btn_row.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.new_btn = QPushButton("新建工区…")
        self.new_btn.setObjectName("StartupNewWorkspace")
        self.new_btn.clicked.connect(self.new_requested.emit)
        self.open_btn = QPushButton("打开工区…")
        self.open_btn.setObjectName("StartupOpenWorkspace")
        self.open_btn.clicked.connect(self.open_requested.emit)
        btn_row.addWidget(self.new_btn)
        btn_row.addWidget(self.open_btn)
        root.addLayout(btn_row)

        recent_label = QLabel("最近工区")
        recent_label.setObjectName("StartupRecentLabel")
        recent_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        root.addWidget(recent_label)

        self.recent_list = QListWidget()
        self.recent_list.setObjectName("StartupRecentList")
        self.recent_list.setMaximumWidth(480)
        self.recent_list.setMinimumHeight(160)
        self.recent_list.setMaximumHeight(220)
        self.recent_list.itemDoubleClicked.connect(self._on_double_click)
        root.addWidget(self.recent_list, 0, Qt.AlignmentFlag.AlignHCenter)

        clear_row = QHBoxLayout()
        clear_row.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.open_selected_btn = QPushButton("打开所选")
        self.open_selected_btn.setObjectName("StartupOpenSelected")
        self.open_selected_btn.clicked.connect(self._on_open_selected)
        self.remove_btn = QPushButton("从列表移除")
        self.remove_btn.setObjectName("StartupRemoveSelected")
        self.remove_btn.clicked.connect(self._on_remove_selected)
        clear_row.addWidget(self.open_selected_btn)
        clear_row.addWidget(self.remove_btn)
        root.addLayout(clear_row)

        self.refresh_recent()

    def refresh_recent(self) -> None:
        self.recent_list.clear()
        paths = load_recent()
        if not paths:
            item = QListWidgetItem("（暂无最近工区）")
            item.setFlags(Qt.ItemFlag.NoItemFlags)
            self.recent_list.addItem(item)
            return
        for p in paths:
            exists = Path(p).expanduser().is_dir()
            label = p if exists else f"{p}  （路径无效）"
            item = QListWidgetItem(label)
            item.setData(Qt.ItemDataRole.UserRole, p)
            if not exists:
                item.setForeground(Qt.GlobalColor.gray)
            self.recent_list.addItem(item)

    def _selected_path(self) -> str | None:
        item = self.recent_list.currentItem()
        if item is None:
            return None
        path = item.data(Qt.ItemDataRole.UserRole)
        return str(path) if path else None

    def _on_double_click(self, item: QListWidgetItem) -> None:
        path = item.data(Qt.ItemDataRole.UserRole)
        if path:
            self.recent_open_requested.emit(str(path))

    def _on_open_selected(self) -> None:
        path = self._selected_path()
        if path:
            self.recent_open_requested.emit(path)

    def _on_remove_selected(self) -> None:
        path = self._selected_path()
        if not path:
            return
        remove_recent(path)
        self.refresh_recent()
