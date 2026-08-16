"""Well-test interval editor dialog (Epic C / FRS §3.x 试油).

Table rows are tested intervals with the structured common fields as columns
(type / depths / domain / unit / date / fluid / pressure / rate / result /
stratigraphic unit / interpretation). ``payload`` (typed, schema-extensible)
and attachment references are preserved verbatim on load and written back on
save — they are not editable in this dialog (v1 keeps the table shallow).
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

from well_log_workstation.stratigraphy import StratigraphicDictionary
from well_log_workstation.unit_combo import make_unit_combo
from well_log_workstation.well_test_model import (
    TEST_TYPES,
    DEPTH_DOMAINS,
    WellTestInterval,
    WellTestModel,
)

(
    COL_TEST_TYPE,
    COL_TOP,
    COL_BOTTOM,
    COL_DOMAIN,
    COL_UNIT,
    COL_DATE,
    COL_FLUID,
    COL_PRESSURE,
    COL_RATE,
    COL_RESULT,
    COL_UNIT_REF,
    COL_INTERP,
) = range(12)

FLUIDS = ("", "油", "气", "水", "油水同层", "气水同层", "干层")


class WellTestDialog(QDialog):
    """Edit the well-test intervals for the selected well."""

    def __init__(
        self,
        current: WellTestModel | None = None,
        *,
        dictionary: StratigraphicDictionary | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("试油数据编辑")
        self.setObjectName("WellTestDialog")
        self._unit = (current.unit if current is not None else "m") or "m"
        self._well_id = current.well_id if current is not None else ""
        self._dictionary = dictionary

        layout = QVBoxLayout(self)
        layout.addWidget(
            QLabel(
                "每行一个试油层段：类型 / 顶深 / 底深 / 深度域 / 单位 / 日期 / 流体 / "
                "压力(MPa) / 日产(m³/d) / 结论 / 层位单元 / 解释成果；保存时按顶深排序。\n"
                "层位单元引用工区层序字典；「（自由文本）」表示不引用。"
            )
        )

        self.table = QTableWidget(0, 12)
        self.table.setObjectName("WellTestTable")
        self.table.setHorizontalHeaderLabels(
            ["类型", "顶深", "底深", "深度域", "单位", "日期", "流体",
             "压力(MPa)", "日产(m³/d)", "结论", "层位单元", "解释成果"]
        )
        header = self.table.horizontalHeader()
        for col in range(12):
            header.setSectionResizeMode(
                col,
                QHeaderView.ResizeMode.ResizeToContents
                if col not in (COL_RESULT, COL_INTERP)
                else QHeaderView.ResizeMode.Stretch,
            )
        layout.addWidget(self.table)

        row_buttons = QHBoxLayout()
        add_btn = QPushButton("添加层段")
        add_btn.setObjectName("WellTestAddRow")
        add_btn.clicked.connect(self._add_empty_row)
        row_buttons.addWidget(add_btn)
        del_btn = QPushButton("删除层段")
        del_btn.setObjectName("WellTestDeleteRow")
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
    def _type_combo(self, test_type: str = "") -> QComboBox:
        combo = QComboBox()
        combo.setObjectName("WellTestTypeCombo")
        combo.setEditable(True)  # types are extensible, not a closed enum
        combo.addItems(TEST_TYPES)
        idx = combo.findText(test_type)
        if idx >= 0:
            combo.setCurrentIndex(idx)
        elif test_type:
            combo.addItem(test_type)
            combo.setCurrentIndex(combo.count() - 1)
        return combo

    def _domain_combo(self, domain: str = "MD") -> QComboBox:
        combo = QComboBox()
        combo.setObjectName("WellTestDomainCombo")
        combo.addItems(DEPTH_DOMAINS)
        idx = combo.findText(domain)
        if idx >= 0:
            combo.setCurrentIndex(idx)
        return combo

    def _fluid_combo(self, fluid: str = "") -> QComboBox:
        combo = QComboBox()
        combo.setObjectName("WellTestFluidCombo")
        combo.setEditable(True)
        combo.addItems(FLUIDS)
        idx = combo.findText(fluid)
        if idx >= 0:
            combo.setCurrentIndex(idx)
        elif fluid:
            combo.addItem(fluid)
            combo.setCurrentIndex(combo.count() - 1)
        return combo

    def _add_empty_row(self) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setCellWidget(r, COL_TEST_TYPE, self._type_combo())
        for col in (COL_TOP, COL_BOTTOM, COL_UNIT, COL_DATE,
                    COL_PRESSURE, COL_RATE, COL_RESULT, COL_INTERP):
            self.table.setItem(r, col, QTableWidgetItem(""))
        # stable id rides on the row; meta preserves payload/source/version.
        self.table.item(r, COL_TOP).setData(
            Qt.ItemDataRole.UserRole, str(uuid.uuid4())
        )
        self._set_row_meta(r, {})
        self.table.setCellWidget(r, COL_DOMAIN, self._domain_combo())
        self.table.setCellWidget(r, COL_FLUID, self._fluid_combo())
        self.table.setCellWidget(r, COL_UNIT_REF, make_unit_combo(self._dictionary))

    def _add_row(self, itv: WellTestInterval) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setCellWidget(r, COL_TEST_TYPE, self._type_combo(itv.test_type))
        self.table.setItem(r, COL_TOP, QTableWidgetItem(f"{itv.top:g}"))
        self.table.item(r, COL_TOP).setData(
            Qt.ItemDataRole.UserRole, itv.id or str(uuid.uuid4())
        )
        self._set_row_meta(
            r,
            {
                "payload": itv.payload,
                "source": itv.source,
                "version": itv.version,
                # Round-trip the attachment references verbatim like the
                # perforation dialog does (#600).
                "attachment_refs": list(getattr(itv, "attachment_refs", None) or []),
            },
        )
        self.table.setItem(r, COL_BOTTOM, QTableWidgetItem(f"{itv.bottom:g}"))
        self.table.setCellWidget(r, COL_DOMAIN, self._domain_combo(itv.depth_domain))
        self.table.setItem(r, COL_UNIT, QTableWidgetItem(itv.unit))
        self.table.setItem(r, COL_DATE, QTableWidgetItem(itv.date))
        self.table.setCellWidget(r, COL_FLUID, self._fluid_combo(itv.fluid))
        self.table.setItem(
            r, COL_PRESSURE,
            QTableWidgetItem("" if itv.pressure_mpa is None else f"{itv.pressure_mpa:g}"),
        )
        self.table.setItem(
            r, COL_RATE,
            QTableWidgetItem("" if itv.flow_rate_m3d is None else f"{itv.flow_rate_m3d:g}"),
        )
        self.table.setItem(r, COL_RESULT, QTableWidgetItem(itv.result))
        ref_combo = make_unit_combo(self._dictionary, itv.formation_unit_id)
        self.table.setCellWidget(r, COL_UNIT_REF, ref_combo)
        self.table.setItem(r, COL_INTERP, QTableWidgetItem(itv.interpretation))

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
            if not top and not bottom and not self._cell_text(r, COL_RESULT):
                continue  # fully empty rows are dropped
            try:
                t = float(top)
                b = float(bottom)
            except (TypeError, ValueError):
                QMessageBox.warning(self, "试油层段不完整", f"第 {r + 1} 行：顶深/底深必须是数字。")
                return
            if b <= t:
                QMessageBox.warning(self, "试油层段无效", f"第 {r + 1} 行：底深必须大于顶深。")
                return
            for col, label in ((COL_PRESSURE, "压力"), (COL_RATE, "日产")):
                text = self._cell_text(r, col)
                if not text:
                    continue
                try:
                    float(text)
                except ValueError:
                    QMessageBox.warning(self, "试油层段无效", f"第 {r + 1} 行：{label}必须是数字。")
                    return
        self.accept()

    def value(self) -> WellTestModel:
        """Validated model (empty rows dropped, sorted by top)."""
        intervals: list[WellTestInterval] = []
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
            intervals.append(
                WellTestInterval(
                    id=str(
                        self.table.item(r, COL_TOP).data(Qt.ItemDataRole.UserRole)
                        or ""
                    ),
                    top=t,
                    bottom=b,
                    unit=self._cell_text(r, COL_UNIT).strip() or self._unit,
                    depth_domain=self._combo_text(r, COL_DOMAIN) or "MD",
                    formation_unit_id=str(self._combo_data(r, COL_UNIT_REF) or ""),
                    test_type=self._combo_text(r, COL_TEST_TYPE).strip() or "DST",
                    date=self._cell_text(r, COL_DATE).strip(),
                    fluid=self._combo_text(r, COL_FLUID).strip(),
                    result=self._cell_text(r, COL_RESULT).strip(),
                    pressure_mpa=self._optional_float(r, COL_PRESSURE),
                    flow_rate_m3d=self._optional_float(r, COL_RATE),
                    interpretation=self._cell_text(r, COL_INTERP).strip(),
                    payload=self._row_meta(r).get("payload") or {},
                    source=self._row_meta(r).get("source") or "",
                    version=self._row_meta(r).get("version") or "",
                    attachment_refs=list(
                        self._row_meta(r).get("attachment_refs") or []
                    ),
                )
            )
        intervals.sort(key=lambda i: (i.top, i.bottom))
        return WellTestModel(well_id=self._well_id, unit=self._unit, intervals=intervals)

    # ------------------------------------------------------------------
    # Row meta (payload/source/version survive round-trips untouched)
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

    def _optional_float(self, row: int, col: int) -> float | None:
        text = self._cell_text(row, col)
        if not text:
            return None
        try:
            return float(text)
        except ValueError:
            return None
