"""Section erosion/onlap surface editor dialog (FRS §3.x / P1).

A table editor for the reservoir section's unconformity surfaces. One row
per surface: name + mode (erosion keeps below / onlap keeps above) + a depth
cell per participating well. Empty depth = that well has no surface there
(the surface line breaks at that well).
"""

from __future__ import annotations

from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QHeaderView,
    QLabel,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
)

from well_log_workstation.section_geometry.erosion_surface import (
    ErosionSurface2D,
    MODE_EROSION,
    MODE_ONLAP,
    surfaces_from_json,
)


class SectionErosionSurfaceDialog(QDialog):
    """Edit the erosion/onlap surfaces of a reservoir section plot."""

    def __init__(
        self,
        current: list[ErosionSurface2D] | None = None,
        well_count: int = 2,
        well_names: list[str] | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("编辑剥蚀/超覆面")
        self.setObjectName("SectionErosionSurfaceDialog")
        self._well_count = max(2, int(well_count))
        self._well_names = well_names or []
        layout = QVBoxLayout(self)

        layout.addWidget(
            QLabel(
                "不整合/剥蚀面按每井深度定义(留空 = 该井无此面,线在该处断开)。\n"
                "模式：剥蚀 = 保留面下部(上部地层被削去)；超覆 = 保留面上部(地层上超尖灭)。\n"
                "面先于断层/流体界面作用于充填多边形。"
            )
        )

        n_cols = 2 + self._well_count
        self.table = QTableWidget(0, n_cols)
        self.table.setObjectName("SectionErosionSurfaceTable")
        headers = ["名称", "模式"]
        for i in range(self._well_count):
            name = self._well_names[i] if i < len(self._well_names) else f"井{i}"
            headers.append(f"{name} 深(MD)")
        self.table.setHorizontalHeaderLabels(headers)
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        for col in range(2, n_cols):
            header.setSectionResizeMode(col, QHeaderView.ResizeMode.Stretch)
        layout.addWidget(self.table)

        add_btn = QPushButton("添加面")
        add_btn.setObjectName("SectionErosionSurfaceAddRow")
        add_btn.clicked.connect(
            lambda: self._add_row(ErosionSurface2D(name="S1"))
        )
        layout.addWidget(add_btn)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for surface in current or []:
            self._add_row(surface)
        if self.table.rowCount() == 0:
            self._add_row(ErosionSurface2D(name="S1"))

    def _add_row(self, surface: ErosionSurface2D) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, 0, QTableWidgetItem(surface.name))
        combo = QComboBox()
        combo.addItem("剥蚀（保下部）", MODE_EROSION)
        combo.addItem("超覆（保上部）", MODE_ONLAP)
        idx = combo.findData(surface.mode)
        combo.setCurrentIndex(idx if idx >= 0 else 0)
        self.table.setCellWidget(r, 1, combo)
        for i in range(self._well_count):
            d = surface.depths.get(i)
            text = "" if d is None else str(d)
            self.table.setItem(r, 2 + i, QTableWidgetItem(text))

    def value(self) -> list[ErosionSurface2D]:
        """Return the edited surface list (rows with <2 depths dropped)."""
        raw: list[dict] = []
        for r in range(self.table.rowCount()):
            name_item = self.table.item(r, 0)
            name = name_item.text().strip() if name_item is not None else ""
            if not name:
                continue
            combo = self.table.cellWidget(r, 1)
            mode = (
                combo.currentData()
                if isinstance(combo, QComboBox)
                else MODE_EROSION
            )
            depths: list[list] = []
            for i in range(self._well_count):
                item = self.table.item(r, 2 + i)
                text = item.text().strip() if item is not None else ""
                if not text:
                    continue
                try:
                    depths.append([i, float(text)])
                except ValueError:
                    continue
            if len(depths) < 2:
                continue
            raw.append({"name": name, "mode": str(mode), "depths": depths})
        return surfaces_from_json(raw)
