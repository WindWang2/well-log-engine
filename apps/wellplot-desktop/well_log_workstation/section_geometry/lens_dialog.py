"""Edit freehand section lens bodies (list + vertex table)."""

from __future__ import annotations

import numpy as np
from PySide6.QtWidgets import (
    QCheckBox,
    QDialog,
    QDialogButtonBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
)

from well_log_workstation.section_geometry.lens_body import (
    LensBody2D,
    make_ellipse_lens,
)


class SectionLensDialog(QDialog):
    """Manage freehand lenses: select one, edit vertices / colors."""

    def __init__(
        self,
        current: list[LensBody2D] | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("编辑透镜体")
        self.setObjectName("SectionLensDialog")
        self._lenses: list[LensBody2D] = list(current or [])
        # Guard so programmatic checkbox sync in _on_select doesn't recurse.
        self._syncing = False
        root = QHBoxLayout(self)

        left = QVBoxLayout()
        left.addWidget(QLabel("透镜体列表"))
        self.list = QListWidget()
        self.list.setObjectName("SectionLensList")
        self.list.currentRowChanged.connect(self._on_select)
        left.addWidget(self.list, 1)
        add_btn = QPushButton("添加椭圆示例")
        add_btn.setObjectName("SectionLensAddEllipse")
        add_btn.clicked.connect(self._add_ellipse)
        left.addWidget(add_btn)
        del_btn = QPushButton("删除选中")
        del_btn.setObjectName("SectionLensDelete")
        del_btn.clicked.connect(self._delete_selected)
        left.addWidget(del_btn)
        root.addLayout(left, 1)

        right = QVBoxLayout()
        right.addWidget(
            QLabel(
                "顶点（x=井列单位 0..n-1，y=深度）。\n"
                "也可用剖面上「绘制透镜体」：左键加点，双击闭合。"
            )
        )
        self.smooth_check = QCheckBox("平滑边缘（Chaikin 圆角，保留原始顶点）")
        self.smooth_check.setObjectName("SectionLensSmooth")
        self.smooth_check.toggled.connect(self._on_smooth_toggled)
        right.addWidget(self.smooth_check)
        self.table = QTableWidget(0, 2)
        self.table.setObjectName("SectionLensVertexTable")
        self.table.setHorizontalHeaderLabels(["x（井列）", "深度"])
        hdr = self.table.horizontalHeader()
        hdr.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        hdr.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        right.addWidget(self.table, 1)
        row_btns = QHBoxLayout()
        add_v = QPushButton("加顶点")
        add_v.setObjectName("SectionLensAddVertex")
        add_v.clicked.connect(self._add_vertex_row)
        row_btns.addWidget(add_v)
        apply_v = QPushButton("应用顶点到当前")
        apply_v.setObjectName("SectionLensApplyVertices")
        apply_v.clicked.connect(self._apply_vertices)
        row_btns.addWidget(apply_v)
        right.addLayout(row_btns)
        root.addLayout(right, 2)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        right.addWidget(buttons)

        self._reload_list()
        if self.list.count():
            self.list.setCurrentRow(0)

    def _reload_list(self) -> None:
        self.list.clear()
        for i, lens in enumerate(self._lenses):
            name = lens.label or f"透镜体 {i + 1}"
            item = QListWidgetItem(f"{name}（{lens.n_vertices} 点）")
            self.list.addItem(item)

    def _on_select(self, row: int) -> None:
        self.table.setRowCount(0)
        if row < 0 or row >= len(self._lenses):
            self._syncing = True
            self.smooth_check.setChecked(False)
            self.smooth_check.setEnabled(False)
            self._syncing = False
            return
        lens = self._lenses[row]
        for x, y in lens.points:
            r = self.table.rowCount()
            self.table.insertRow(r)
            self.table.setItem(r, 0, QTableWidgetItem(f"{float(x):.4g}"))
            self.table.setItem(r, 1, QTableWidgetItem(f"{float(y):.4g}"))
        self._syncing = True
        self.smooth_check.setEnabled(True)
        self.smooth_check.setChecked(bool(lens.smooth))
        self._syncing = False

    def _on_smooth_toggled(self, checked: bool) -> None:
        if self._syncing:
            return
        row = self.list.currentRow()
        if row < 0 or row >= len(self._lenses):
            return
        old = self._lenses[row]
        self._lenses[row] = LensBody2D(
            points=old.points,
            label=old.label,
            fill_color=old.fill_color,
            stroke_color=old.stroke_color,
            pattern_id=old.pattern_id,
            smooth=bool(checked),
        )

    def _add_ellipse(self) -> None:
        lens = make_ellipse_lens(0.5, 1050.0, 0.35, 25.0, label=f"透镜体{len(self._lenses)+1}")
        self._lenses.append(lens)
        self._reload_list()
        self.list.setCurrentRow(len(self._lenses) - 1)

    def _delete_selected(self) -> None:
        row = self.list.currentRow()
        if row < 0 or row >= len(self._lenses):
            return
        del self._lenses[row]
        self._reload_list()
        self.table.setRowCount(0)

    def _add_vertex_row(self) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, 0, QTableWidgetItem("0.5"))
        self.table.setItem(r, 1, QTableWidgetItem("1050"))

    def _apply_vertices(self) -> None:
        row = self.list.currentRow()
        if row < 0 or row >= len(self._lenses):
            return
        pts: list[list[float]] = []
        for r in range(self.table.rowCount()):
            ix = self.table.item(r, 0)
            iy = self.table.item(r, 1)
            if ix is None or iy is None:
                continue
            try:
                pts.append([float(ix.text()), float(iy.text())])
            except ValueError:
                continue
        if len(pts) < 3:
            return
        old = self._lenses[row]
        self._lenses[row] = LensBody2D(
            points=np.asarray(pts, dtype=np.float64),
            label=old.label,
            fill_color=old.fill_color,
            stroke_color=old.stroke_color,
            pattern_id=old.pattern_id,
            smooth=old.smooth,
        )
        self._reload_list()
        self.list.setCurrentRow(row)

    def value(self) -> list[LensBody2D]:
        return [L for L in self._lenses if L.is_valid()]
