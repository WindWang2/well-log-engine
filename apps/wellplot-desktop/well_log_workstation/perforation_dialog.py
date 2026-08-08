"""Perforation interval editor dialog (Epic C / FRS §3.x 射孔).

Table rows are perforated intervals (top / bottom / depth domain / unit /
date / shot density / phasing / status / stratigraphic unit / completion
reference). Status comes from a fixed catalog (PERFORATION_STATUSES) because
it drives symbols; everything else stays free-form.
"""

from __future__ import annotations

import json
import uuid

from PySide6.QtCore import Qt
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

from well_log_workstation.perforation_model import (
    PERFORATION_STATUSES,
    DEPTH_DOMAINS,
    PerforationInterval,
    PerforationModel,
)
from well_log_workstation.stratigraphy import StratigraphicDictionary
from well_log_workstation.unit_combo import make_unit_combo

(
    COL_TOP,
    COL_BOTTOM,
    COL_DOMAIN,
    COL_UNIT,
    COL_DATE,
    COL_DENSITY,
    COL_PHASING,
    COL_STATUS,
    COL_UNIT_REF,
    COL_COMPLETION,
) = range(10)


class PerforationDialog(QDialog):
    """Edit the perforation intervals for the selected well."""

    def __init__(
        self,
        current: PerforationModel | None = None,
        *,
        dictionary: StratigraphicDictionary | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("射孔数据编辑")
        self.setObjectName("PerforationDialog")
        self._unit = (current.unit if current is not None else "m") or "m"
        self._well_id = current.well_id if current is not None else ""
        self._dictionary = dictionary

        layout = QVBoxLayout(self)
        layout.addWidget(
            QLabel(
                "每行一个射孔层段：顶深 / 底深 / 深度域 / 单位 / 日期 / 孔密(孔/m) / "
                "相位 / 状态 / 层位单元 / 完井引用；保存时按顶深排序。"
            )
        )

        self.table = QTableWidget(0, 10)
        self.table.setObjectName("PerforationTable")
        self.table.setHorizontalHeaderLabels(
            ["顶深", "底深", "深度域", "单位", "日期", "孔密(孔/m)", "相位",
             "状态", "层位单元", "完井引用"]
        )
        header = self.table.horizontalHeader()
        for col in range(10):
            header.setSectionResizeMode(
                col,
                QHeaderView.ResizeMode.ResizeToContents
                if col not in (COL_COMPLETION,)
                else QHeaderView.ResizeMode.Stretch,
            )
        layout.addWidget(self.table)

        row_buttons = QHBoxLayout()
        add_btn = QPushButton("添加层段")
        add_btn.setObjectName("PerforationAddRow")
        add_btn.clicked.connect(self._add_empty_row)
        row_buttons.addWidget(add_btn)
        del_btn = QPushButton("删除层段")
        del_btn.setObjectName("PerforationDeleteRow")
        del_btn.clicked.connect(self._delete_selected_rows)
        row_buttons.addWidget(del_btn)
        row_buttons.addStretch(1)
        layout.addLayout(row_buttons)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for itv in (current.intervals if current is not None else []):
            self._add_row(itv)

    # ------------------------------------------------------------------
    # Row management
    # ------------------------------------------------------------------
    def _status_combo(self, status: str = "已射") -> QComboBox:
        combo = QComboBox()
        combo.setObjectName("PerforationStatusCombo")
        combo.addItems(PERFORATION_STATUSES)
        idx = combo.findText(status)
        if idx >= 0:
            combo.setCurrentIndex(idx)
        elif status:  # tolerate unknown persisted statuses
            combo.addItem(status)
            combo.setCurrentIndex(combo.count() - 1)
        return combo

    def _domain_combo(self, domain: str = "MD") -> QComboBox:
        combo = QComboBox()
        combo.setObjectName("PerforationDomainCombo")
        combo.addItems(DEPTH_DOMAINS)
        idx = combo.findText(domain)
        if idx >= 0:
            combo.setCurrentIndex(idx)
        return combo

    def _add_empty_row(self) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        for col in (COL_TOP, COL_BOTTOM, COL_UNIT, COL_DATE,
                    COL_DENSITY, COL_PHASING, COL_COMPLETION):
            self.table.setItem(r, col, QTableWidgetItem(""))
        # stable id rides on the row; meta preserves source/version/refs.
        self.table.item(r, COL_TOP).setData(
            Qt.ItemDataRole.UserRole, str(uuid.uuid4())
        )
        self._set_row_meta(r, {})
        self.table.setCellWidget(r, COL_DOMAIN, self._domain_combo())
        self.table.setCellWidget(r, COL_STATUS, self._status_combo())
        self.table.setCellWidget(r, COL_UNIT_REF, make_unit_combo(self._dictionary))

    def _add_row(self, itv: PerforationInterval) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, COL_TOP, QTableWidgetItem(f"{itv.top:g}"))
        self.table.item(r, COL_TOP).setData(
            Qt.ItemDataRole.UserRole, itv.id or str(uuid.uuid4())
        )
        self._set_row_meta(
            r,
            {
                "source": itv.source,
                "version": itv.version,
                "attachment_refs": itv.attachment_refs,
            },
        )
        self.table.setItem(r, COL_BOTTOM, QTableWidgetItem(f"{itv.bottom:g}"))
        self.table.setCellWidget(r, COL_DOMAIN, self._domain_combo(itv.depth_domain))
        self.table.setItem(r, COL_UNIT, QTableWidgetItem(itv.unit))
        self.table.setItem(r, COL_DATE, QTableWidgetItem(itv.operation_date))
        self.table.setItem(
            r, COL_DENSITY,
            QTableWidgetItem("" if itv.shot_density is None else f"{itv.shot_density:g}"),
        )
        self.table.setItem(r, COL_PHASING, QTableWidgetItem(itv.phasing))
        self.table.setCellWidget(r, COL_STATUS, self._status_combo(itv.status))
        self.table.setCellWidget(
            r, COL_UNIT_REF, make_unit_combo(self._dictionary, itv.formation_unit_id)
        )
        self.table.setItem(r, COL_COMPLETION, QTableWidgetItem(itv.completion_ref))

    def _delete_selected_rows(self) -> None:
        rows = sorted({i.row() for i in self.table.selectedIndexes()}, reverse=True)
        for r in rows:
            self.table.removeRow(r)

    # ------------------------------------------------------------------
    # Accept / value
    # ------------------------------------------------------------------
    def _on_accept(self) -> None:
        for r in range(self.table.rowCount()):
            top = self._cell_text(r, COL_TOP)
            bottom = self._cell_text(r, COL_BOTTOM)
            if not top and not bottom and not self._cell_text(r, COL_COMPLETION):
                continue  # fully empty rows are dropped
            try:
                t = float(top)
                b = float(bottom)
            except (TypeError, ValueError):
                QMessageBox.warning(self, "射孔层段不完整", f"第 {r + 1} 行：顶深/底深必须是数字。")
                return
            if b <= t:
                QMessageBox.warning(self, "射孔层段无效", f"第 {r + 1} 行：底深必须大于顶深。")
                return
            density = self._cell_text(r, COL_DENSITY)
            if density:
                try:
                    float(density)
                except ValueError:
                    QMessageBox.warning(self, "射孔层段无效", f"第 {r + 1} 行：孔密必须是数字。")
                    return
        self.accept()

    def value(self) -> PerforationModel:
        """Validated model (empty rows dropped, sorted by top)."""
        intervals: list[PerforationInterval] = []
        for r in range(self.table.rowCount()):
            top = self._cell_text(r, COL_TOP)
            bottom = self._cell_text(r, COL_BOTTOM)
            if not top or not bottom:
                continue
            try:
                t = float(top)
                b = float(bottom)
            except (TypeError, ValueError):
                continue
            if b <= t:
                continue
            density = self._cell_text(r, COL_DENSITY)
            intervals.append(
                PerforationInterval(
                    id=str(
                        self.table.item(r, COL_TOP).data(Qt.ItemDataRole.UserRole)
                        or ""
                    ),
                    top=t,
                    bottom=b,
                    unit=self._cell_text(r, COL_UNIT).strip() or self._unit,
                    depth_domain=self._combo_text(r, COL_DOMAIN) or "MD",
                    operation_date=self._cell_text(r, COL_DATE).strip(),
                    shot_density=float(density) if density else None,
                    phasing=self._cell_text(r, COL_PHASING).strip(),
                    status=self._combo_text(r, COL_STATUS).strip() or "已射",
                    completion_ref=self._cell_text(r, COL_COMPLETION).strip(),
                    formation_unit_id=str(self._combo_data(r, COL_UNIT_REF) or ""),
                    source=self._row_meta(r).get("source") or "",
                    version=self._row_meta(r).get("version") or "",
                    attachment_refs=list(
                        self._row_meta(r).get("attachment_refs") or []
                    ),
                )
            )
        intervals.sort(key=lambda i: (i.top, i.bottom))
        return PerforationModel(
            well_id=self._well_id, unit=self._unit, intervals=intervals
        )

    # ------------------------------------------------------------------
    # Row meta (source/version/refs survive round-trips untouched)
    # ------------------------------------------------------------------
    def _set_row_meta(self, row: int, meta: dict) -> None:
        item = self.table.item(row, COL_TOP)
        if item is not None:
            item.setData(
                Qt.ItemDataRole.UserRole + 1,
                json.dumps(meta, ensure_ascii=False),
            )

    def _row_meta(self, row: int) -> dict:
        item = self.table.item(row, COL_TOP)
        if item is None:
            return {}
        raw = item.data(Qt.ItemDataRole.UserRole + 1)
        if not raw:
            return {}
        try:
            value = json.loads(str(raw))
        except (TypeError, ValueError):
            return {}
        return value if isinstance(value, dict) else {}

    def _cell_text(self, row: int, col: int) -> str:
        item = self.table.item(row, col)
        return item.text().strip() if item is not None else ""

    def _combo_text(self, row: int, col: int) -> str:
        widget = self.table.cellWidget(row, col)
        if isinstance(widget, QComboBox):
            return str(widget.currentText()).strip()
        return ""

    def _combo_data(self, row: int, col: int) -> object:
        widget = self.table.cellWidget(row, col)
        if isinstance(widget, QComboBox):
            return widget.currentData()
        return None
