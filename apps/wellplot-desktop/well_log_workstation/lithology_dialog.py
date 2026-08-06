"""Lithology segment editor dialog (FRS §2.x).

A table of depth bands (top / bottom / SY/T 5615 pattern / label). On accept
rows are validated (finite depths, top < bottom, pattern chosen). A demo-fill
button seeds stub segments from the well's depth range when one is supplied.
"""

from __future__ import annotations

from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QMessageBox,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
)

from well_log_workstation.litho_pattern_lib import load_builtin_patterns
from well_log_workstation.lithology_model import (
    LithologyModel,
    LithologySegment,
    make_stub_lithology,
)

COL_TOP, COL_BOTTOM, COL_PATTERN, COL_LABEL = 0, 1, 2, 3


class LithologyDialog(QDialog):
    """Edit the lithology segments for the selected well."""

    def __init__(
        self,
        current: LithologyModel | None = None,
        parent=None,
        *,
        depth_range: tuple[float, float] | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("岩性道编辑")
        self.setObjectName("LithologyDialog")
        self._unit = (current.unit if current is not None else "m") or "m"
        self._well_id = current.well_id if current is not None else ""
        self._patterns = load_builtin_patterns()  # id -> LithoPattern
        layout = QVBoxLayout(self)

        layout.addWidget(
            QLabel(
                "每行一段：顶深 / 底深 / 岩性（SY/T 5615 花纹）/ 备注；保存时按顶深排序。\n"
                "岩性道仅出现在声明了 litho 图道的图版（如「标准岩性图」）。"
            )
        )

        self.table = QTableWidget(0, 4)
        self.table.setObjectName("LithologyTable")
        self.table.setHorizontalHeaderLabels(["顶深", "底深", "岩性", "备注"])
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(COL_TOP, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(COL_BOTTOM, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(COL_PATTERN, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(COL_LABEL, QHeaderView.ResizeMode.Stretch)
        layout.addWidget(self.table)

        row_buttons = QHBoxLayout()
        add_btn = QPushButton("添加段")
        add_btn.setObjectName("LithologyAddRow")
        add_btn.clicked.connect(self._add_empty_row)
        row_buttons.addWidget(add_btn)
        del_btn = QPushButton("删除段")
        del_btn.setObjectName("LithologyDeleteRow")
        del_btn.clicked.connect(self._delete_selected_rows)
        row_buttons.addWidget(del_btn)
        up_btn = QPushButton("上移")
        up_btn.setObjectName("LithologyMoveUp")
        up_btn.clicked.connect(lambda: self._move_selected(-1))
        row_buttons.addWidget(up_btn)
        down_btn = QPushButton("下移")
        down_btn.setObjectName("LithologyMoveDown")
        down_btn.clicked.connect(lambda: self._move_selected(1))
        row_buttons.addWidget(down_btn)
        row_buttons.addStretch(1)
        if depth_range is not None and len(depth_range) == 2:
            stub_btn = QPushButton("生成演示数据")
            stub_btn.setObjectName("LithologyFillStub")
            stub_btn.clicked.connect(
                lambda: self._fill_stub(depth_range[0], depth_range[1])
            )
            row_buttons.addWidget(stub_btn)
        layout.addLayout(row_buttons)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for seg in (current.segments if current is not None else []):
            self._add_row(seg)
        if self.table.rowCount() == 0:
            self._add_empty_row()

    # ------------------------------------------------------------------
    # Row management
    # ------------------------------------------------------------------
    def _pattern_combo(self, pattern_id: str = "") -> QComboBox:
        combo = QComboBox()
        combo.setObjectName("LithologyPatternCombo")
        idx = 0
        for i, (pid, pattern) in enumerate(
            sorted(self._patterns.items(), key=lambda kv: kv[1].name)
        ):
            combo.addItem(f"{pattern.name}（{pid}）", userData=pid)
            if pid == pattern_id:
                idx = i
        combo.setCurrentIndex(idx)
        return combo

    def _add_empty_row(self) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, COL_TOP, QTableWidgetItem(""))
        self.table.setItem(r, COL_BOTTOM, QTableWidgetItem(""))
        self.table.setCellWidget(r, COL_PATTERN, self._pattern_combo())
        self.table.setItem(r, COL_LABEL, QTableWidgetItem(""))

    def _add_row(self, seg: LithologySegment) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, COL_TOP, QTableWidgetItem(f"{seg.top:g}"))
        self.table.setItem(r, COL_BOTTOM, QTableWidgetItem(f"{seg.bottom:g}"))
        self.table.setCellWidget(r, COL_PATTERN, self._pattern_combo(seg.pattern_id))
        self.table.setItem(r, COL_LABEL, QTableWidgetItem(seg.label))

    def _delete_selected_rows(self) -> None:
        rows = sorted({i.row() for i in self.table.selectedIndexes()}, reverse=True)
        for r in rows:
            self.table.removeRow(r)

    def _move_selected(self, delta: int) -> None:
        rows = sorted({i.row() for i in self.table.selectedIndexes()})
        if not rows:
            return
        moved = False
        for r in rows:
            target = r + delta
            if target < 0 or target >= self.table.rowCount():
                continue
            if target in rows:  # adjacent selection — avoid swaps
                continue
            self.table.insertRow(target)
            # insertRow shifts the source when moving up (target < r).
            src = r if r < target else r + 1
            self._copy_row(self.table, src, target)
            self.table.removeRow(src)
            moved = True
        if moved:
            self.table.selectRow(rows[0] + delta)

    @staticmethod
    def _copy_row(table: QTableWidget, src: int, dst: int) -> None:
        for col in (COL_TOP, COL_BOTTOM, COL_LABEL):
            item = table.takeItem(src, col)
            if item is not None:
                table.setItem(dst, col, QTableWidgetItem(item.text()))
        widget = table.cellWidget(src, COL_PATTERN)
        if widget is not None:
            combo = table.cellWidget(dst, COL_PATTERN)
            if combo is None:
                combo = QComboBox()
                table.setCellWidget(dst, COL_PATTERN, combo)
            combo.clear()
            combo.addItem(widget.currentText(), userData=widget.currentData())
            combo.setCurrentIndex(0)

    def _fill_stub(self, d0: float, d1: float) -> None:
        self.table.setRowCount(0)
        for seg in make_stub_lithology(d0, d1):
            self._add_row(seg)

    # ------------------------------------------------------------------
    # Accept / value
    # ------------------------------------------------------------------
    def _on_accept(self) -> None:
        for r in range(self.table.rowCount()):
            top = self._cell_text(r, COL_TOP)
            bottom = self._cell_text(r, COL_BOTTOM)
            pattern_id = self._pattern_id(r)
            if not top and not bottom and not pattern_id:
                continue  # empty rows are dropped
            try:
                t = float(top)
                b = float(bottom)
            except (TypeError, ValueError):
                QMessageBox.warning(self, "岩性段不完整", f"第 {r + 1} 行：顶深/底深必须是数字。")
                return
            if b <= t:
                QMessageBox.warning(self, "岩性段无效", f"第 {r + 1} 行：底深必须大于顶深。")
                return
            if not pattern_id:
                QMessageBox.warning(self, "岩性段不完整", f"第 {r + 1} 行：请选择岩性。")
                return
        self.accept()

    def value(self) -> LithologyModel:
        """Return the validated model (empty rows dropped, sorted by top)."""
        segments: list[LithologySegment] = []
        for r in range(self.table.rowCount()):
            top = self._cell_text(r, COL_TOP)
            bottom = self._cell_text(r, COL_BOTTOM)
            pattern_id = self._pattern_id(r)
            if not top or not bottom or not pattern_id:
                continue
            try:
                t = float(top)
                b = float(bottom)
            except (TypeError, ValueError):
                continue
            if b <= t:
                continue
            segments.append(
                LithologySegment(
                    id="",
                    top=t,
                    bottom=b,
                    pattern_id=pattern_id,
                    label=self._cell_text(r, COL_LABEL).strip(),
                )
            )
        segments.sort(key=lambda s: (s.top, s.bottom))
        return LithologyModel(well_id=self._well_id, unit=self._unit, segments=segments)

    def _cell_text(self, row: int, col: int) -> str:
        item = self.table.item(row, col)
        return item.text().strip() if item is not None else ""

    def _pattern_id(self, row: int) -> str:
        widget = self.table.cellWidget(row, COL_PATTERN)
        if isinstance(widget, QComboBox):
            return str(widget.currentData() or "")
        return ""
