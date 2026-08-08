"""Core domain editor dialog (Epic C / FRS §3.x 岩心).

Two-pane editor: the upper table lists core runs (drilling barrels: top /
bottom / depth domain / unit / recovered length / label); the lower table
lists the samples of the currently selected run. Samples are kept in memory
per run row and flushed when the selection changes or on accept, so editing
one run's samples never discards another run's edits.

Photo linkage stays with CorePhotoModel — the sample table only references
photo segments by id (photo_segment_id column).
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

from well_log_workstation.core_model import (
    DEPTH_DOMAINS,
    CoreModel,
    CoreRun,
    CoreSample,
)
from well_log_workstation.stratigraphy import StratigraphicDictionary
from well_log_workstation.unit_combo import make_unit_combo

# Runs table columns.
R_TOP, R_BOTTOM, R_DOMAIN, R_UNIT, R_RECOVERY, R_LABEL = range(6)
# Samples table columns.
S_DEPTH, S_DESC, S_UNIT_REF, S_POROSITY, S_PERM, S_DENSITY, S_PHOTO, S_SOURCE, S_VERSION = range(9)

RUN_HEADERS = ["顶深", "底深", "深度域", "单位", "取芯段长(m)", "备注"]
SAMPLE_HEADERS = [
    "深度", "描述", "层位单元", "孔隙度", "渗透率(mD)", "密度(g/cm³)",
    "照片段", "来源", "版本",
]


class CoreDialog(QDialog):
    """Edit the core runs + samples for the selected well."""

    def __init__(
        self,
        current: CoreModel | None = None,
        *,
        dictionary: StratigraphicDictionary | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("岩心数据编辑")
        self.setObjectName("CoreDialog")
        self.resize(860, 560)
        self._unit = (current.unit if current is not None else "m") or "m"
        self._well_id = current.well_id if current is not None else ""
        self._dictionary = dictionary
        # run row index -> list[CoreSample] for rows not currently visible.
        self._samples: dict[int, list[CoreSample]] = {}
        # the run row whose samples the lower table currently displays.
        self._active_run_row = -1

        layout = QVBoxLayout(self)
        layout.addWidget(
            QLabel(
                "上表：取芯筒（每次取芯一段）；下表：所选筒的样品。"
                "每个深度对象都显式声明深度域与单位（C5）。"
            )
        )

        self.runs = QTableWidget(0, 6)
        self.runs.setObjectName("CoreRunsTable")
        self.runs.setHorizontalHeaderLabels(RUN_HEADERS)
        self.runs.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        run_header = self.runs.horizontalHeader()
        for col in range(6):
            run_header.setSectionResizeMode(
                col,
                QHeaderView.ResizeMode.ResizeToContents
                if col != R_LABEL
                else QHeaderView.ResizeMode.Stretch,
            )
        self.runs.itemSelectionChanged.connect(self._on_run_selection_changed)
        layout.addWidget(self.runs)

        run_buttons = QHBoxLayout()
        add_run_btn = QPushButton("添加筒")
        add_run_btn.setObjectName("CoreAddRun")
        add_run_btn.clicked.connect(self._add_run)
        run_buttons.addWidget(add_run_btn)
        del_run_btn = QPushButton("删除筒")
        del_run_btn.setObjectName("CoreDeleteRun")
        del_run_btn.clicked.connect(self._delete_selected_run)
        run_buttons.addWidget(del_run_btn)
        up_btn = QPushButton("上移")
        up_btn.setObjectName("CoreRunMoveUp")
        up_btn.clicked.connect(lambda: self._move_run(-1))
        run_buttons.addWidget(up_btn)
        down_btn = QPushButton("下移")
        down_btn.setObjectName("CoreRunMoveDown")
        down_btn.clicked.connect(lambda: self._move_run(1))
        run_buttons.addWidget(down_btn)
        run_buttons.addStretch(1)
        layout.addLayout(run_buttons)

        self.samples = QTableWidget(0, 9)
        self.samples.setObjectName("CoreSamplesTable")
        self.samples.setHorizontalHeaderLabels(SAMPLE_HEADERS)
        sample_header = self.samples.horizontalHeader()
        for col in range(9):
            sample_header.setSectionResizeMode(
                col,
                QHeaderView.ResizeMode.ResizeToContents
                if col not in (S_DESC,)
                else QHeaderView.ResizeMode.Stretch,
            )
        layout.addWidget(self.samples)

        sample_buttons = QHBoxLayout()
        add_sample_btn = QPushButton("添加样品")
        add_sample_btn.setObjectName("CoreAddSample")
        add_sample_btn.clicked.connect(self._add_sample)
        sample_buttons.addWidget(add_sample_btn)
        del_sample_btn = QPushButton("删除样品")
        del_sample_btn.setObjectName("CoreDeleteSample")
        del_sample_btn.clicked.connect(self._delete_selected_samples)
        sample_buttons.addWidget(del_sample_btn)
        sample_buttons.addStretch(1)
        layout.addLayout(sample_buttons)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for run in (current.runs if current is not None else []):
            self._add_run_row(run)

    # ------------------------------------------------------------------
    # Runs table
    # ------------------------------------------------------------------
    def _domain_combo(self, domain: str = "MD") -> QComboBox:
        combo = QComboBox()
        combo.setObjectName("CoreDomainCombo")
        combo.addItems(DEPTH_DOMAINS)
        idx = combo.findText(domain)
        if idx >= 0:
            combo.setCurrentIndex(idx)
        return combo

    def _add_run(self) -> None:
        self._flush_samples()
        r = self.runs.rowCount()
        self.runs.insertRow(r)
        for col in (R_TOP, R_BOTTOM, R_UNIT, R_RECOVERY, R_LABEL):
            self.runs.setItem(r, col, QTableWidgetItem(""))
        self.runs.setCellWidget(r, R_DOMAIN, self._domain_combo())
        self.runs.setItem(r, R_UNIT, QTableWidgetItem(self._unit))
        # stable id: rides on the row and survives reorder swaps
        self.runs.item(r, R_TOP).setData(Qt.ItemDataRole.UserRole, str(uuid.uuid4()))
        self._set_run_meta(r, {})
        self._samples[r] = []
        self.runs.selectRow(r)

    def _add_run_row(self, run: CoreRun) -> None:
        r = self.runs.rowCount()
        self.runs.insertRow(r)
        self.runs.setItem(r, R_TOP, QTableWidgetItem(f"{run.top:g}"))
        self.runs.setItem(r, R_BOTTOM, QTableWidgetItem(f"{run.bottom:g}"))
        self.runs.setCellWidget(r, R_DOMAIN, self._domain_combo(run.depth_domain))
        self.runs.setItem(r, R_UNIT, QTableWidgetItem(run.unit))
        self.runs.setItem(
            r, R_RECOVERY,
            QTableWidgetItem("" if run.recovery_m is None else f"{run.recovery_m:g}"),
        )
        self.runs.setItem(r, R_LABEL, QTableWidgetItem(run.label))
        self.runs.item(r, R_TOP).setData(
            Qt.ItemDataRole.UserRole, run.id or str(uuid.uuid4())
        )
        self._set_run_meta(
            r,
            {
                "source": run.source,
                "version": run.version,
                "photo_segment_ids": run.photo_segment_ids,
            },
        )
        self._samples[r] = list(run.samples)
        if r == 0:
            self.runs.selectRow(0)

    def _delete_selected_run(self) -> None:
        rows = sorted({i.row() for i in self.runs.selectedIndexes()}, reverse=True)
        for r in rows:
            self._samples.pop(r, None)
            self.runs.removeRow(r)
        self._active_run_row = -1
        self.samples.setRowCount(0)
        if self.runs.rowCount() > 0:
            self.runs.selectRow(max(0, self.runs.currentRow()))

    def _move_run(self, delta: int) -> None:
        rows = sorted({i.row() for i in self.runs.selectedIndexes()})
        if not rows:
            return
        r = rows[0]
        target = r + delta
        if target < 0 or target >= self.runs.rowCount():
            return
        active = self._active_run_row
        self._swap_rows(self.runs, r, target, self._samples)
        # The run (with its samples) moved; keep the samples table bound to
        # the same run, not the same row index.
        if active == r:
            self._active_run_row = target
        elif active == target:
            self._active_run_row = r
        self._load_samples(self._active_run_row)
        self.runs.selectRow(target)

    @staticmethod
    def _swap_rows(
        table: QTableWidget,
        a: int,
        b: int,
        samples: dict[int, list[CoreSample]],
    ) -> None:
        samples[a], samples[b] = samples[b], samples[a]
        for col in range(table.columnCount()):
            item_a = table.takeItem(a, col)
            item_b = table.takeItem(b, col)
            if item_a is not None:
                table.setItem(b, col, item_a)
            if item_b is not None:
                table.setItem(a, col, item_b)
        for col in range(table.columnCount()):
            widget_a = table.cellWidget(a, col)
            widget_b = table.cellWidget(b, col)
            if widget_a is not None:
                table.setCellWidget(b, col, widget_a)
            if widget_b is not None:
                table.setCellWidget(a, col, widget_b)

    # ------------------------------------------------------------------
    # Samples table (bound to the selected run)
    # ------------------------------------------------------------------
    def _on_run_selection_changed(self) -> None:
        """Bind the samples table to the newly selected run."""
        new_row = self.runs.currentRow()
        if new_row == self._active_run_row:
            return
        self._flush_samples()
        self._active_run_row = new_row
        self._load_samples(new_row)

    def _flush_samples(self) -> None:
        """Store the visible samples rows into the active run's slot."""
        if self._active_run_row < 0:
            return
        samples: list[CoreSample] = []
        for r in range(self.samples.rowCount()):
            sample = self._sample_from_row(r)
            if sample is not None:
                samples.append(sample)
        self._samples[self._active_run_row] = samples

    def _load_samples(self, run_row: int) -> None:
        self.samples.setRowCount(0)
        for sample in self._samples.get(run_row, []):
            self._add_sample_row(sample)

    def _add_sample(self) -> None:
        if self.runs.currentRow() < 0:
            QMessageBox.information(self, "岩心数据", "请先在筒列表中选择一个筒。")
            return
        self._flush_samples()
        r = self.samples.rowCount()
        self.samples.insertRow(r)
        for col in (S_DEPTH, S_DESC, S_POROSITY, S_PERM, S_DENSITY,
                    S_PHOTO, S_SOURCE, S_VERSION):
            self.samples.setItem(r, col, QTableWidgetItem(""))
        self.samples.item(r, S_DEPTH).setData(
            Qt.ItemDataRole.UserRole, str(uuid.uuid4())
        )
        self._set_sample_meta(r, {})
        self.samples.setCellWidget(
            r, S_UNIT_REF, make_unit_combo(self._dictionary)
        )
        self.samples.setCurrentCell(r, S_DEPTH)

    def _add_sample_row(self, sample: CoreSample) -> None:
        r = self.samples.rowCount()
        self.samples.insertRow(r)
        self.samples.setItem(r, S_DEPTH, QTableWidgetItem(f"{sample.depth:g}"))
        self.samples.item(r, S_DEPTH).setData(
            Qt.ItemDataRole.UserRole, sample.id or str(uuid.uuid4())
        )
        self._set_sample_meta(
            r,
            {
                "lab_report_ref": sample.lab_report_ref,
                "attachment_ref": sample.attachment_ref,
            },
        )
        self.samples.setItem(r, S_DESC, QTableWidgetItem(sample.description))
        self.samples.setCellWidget(
            r, S_UNIT_REF, make_unit_combo(self._dictionary, sample.lithology_unit_id)
        )
        for col, value in (
            (S_POROSITY, sample.porosity),
            (S_PERM, sample.permeability_md),
            (S_DENSITY, sample.density_gcc),
        ):
            self.samples.setItem(
                r, col,
                QTableWidgetItem("" if value is None else f"{value:g}"),
            )
        self.samples.setItem(r, S_PHOTO, QTableWidgetItem(sample.photo_segment_id))
        self.samples.setItem(r, S_SOURCE, QTableWidgetItem(sample.source))
        self.samples.setItem(r, S_VERSION, QTableWidgetItem(sample.version))

    def _delete_selected_samples(self) -> None:
        rows = sorted({i.row() for i in self.samples.selectedIndexes()}, reverse=True)
        for r in rows:
            self.samples.removeRow(r)

    # ------------------------------------------------------------------
    # Accept / value
    # ------------------------------------------------------------------
    def _on_accept(self) -> None:
        self._flush_samples()
        for r in range(self.runs.rowCount()):
            top = self._run_text(r, R_TOP)
            bottom = self._run_text(r, R_BOTTOM)
            if not top and not bottom and not self._run_text(r, R_LABEL):
                continue  # fully empty runs are dropped
            try:
                t = float(top)
                b = float(bottom)
            except (TypeError, ValueError):
                QMessageBox.warning(self, "岩心筒不完整", f"第 {r + 1} 行：顶深/底深必须是数字。")
                return
            if b <= t:
                QMessageBox.warning(self, "岩心筒无效", f"第 {r + 1} 行：底深必须大于顶深。")
                return
            recovery = self._run_text(r, R_RECOVERY)
            if recovery:
                try:
                    float(recovery)
                except ValueError:
                    QMessageBox.warning(self, "岩心筒无效", f"第 {r + 1} 行：取芯段长必须是数字。")
                    return
        self.accept()

    def value(self) -> CoreModel:
        """Validated model (empty runs dropped, runs sorted by top)."""
        self._flush_samples()
        runs: list[CoreRun] = []
        for r in range(self.runs.rowCount()):
            top = self._run_text(r, R_TOP)
            bottom = self._run_text(r, R_BOTTOM)
            if not top or not bottom:
                continue
            try:
                t = float(top)
                b = float(bottom)
            except (TypeError, ValueError):
                continue
            if b <= t:
                continue
            recovery = self._run_text(r, R_RECOVERY)
            meta = self._run_meta(r)
            run = CoreRun(
                id=str(self.runs.item(r, R_TOP).data(Qt.ItemDataRole.UserRole) or ""),
                top=t,
                bottom=b,
                unit=self._run_text(r, R_UNIT).strip() or self._unit,
                depth_domain=self._run_combo_text(r, R_DOMAIN) or "MD",
                recovery_m=float(recovery) if recovery else None,
                label=self._run_text(r, R_LABEL).strip(),
                samples=list(self._samples.get(r, [])),
                source=meta.get("source") or "",
                version=meta.get("version") or "",
                photo_segment_ids=list(meta.get("photo_segment_ids") or []),
            )
            run.samples.sort(key=lambda s: (s.depth, s.id))
            runs.append(run)
        runs.sort(key=lambda run: (run.top, run.id))
        return CoreModel(well_id=self._well_id, unit=self._unit, runs=runs)

    def _sample_from_row(self, r: int) -> CoreSample | None:
        depth = self._sample_text(r, S_DEPTH)
        if not depth:
            return None
        try:
            d = float(depth)
        except (TypeError, ValueError):
            return None
        porosity = self._sample_text(r, S_POROSITY)
        perm = self._sample_text(r, S_PERM)
        density = self._sample_text(r, S_DENSITY)
        sid = str(self.samples.item(r, S_DEPTH).data(Qt.ItemDataRole.UserRole) or "")
        meta = self._sample_meta(r)
        return CoreSample(
            id=sid or str(uuid.uuid4()),
            depth=d,
            unit=self._unit,
            depth_domain="MD",  # samples follow the run's context by default
            description=self._sample_text(r, S_DESC).strip(),
            lithology_unit_id=str(self._sample_combo_data(r, S_UNIT_REF) or ""),
            porosity=float(porosity) if porosity else None,
            permeability_md=float(perm) if perm else None,
            density_gcc=float(density) if density else None,
            photo_segment_id=self._sample_text(r, S_PHOTO).strip(),
            lab_report_ref=meta.get("lab_report_ref") or "",
            attachment_ref=meta.get("attachment_ref") or "",
        )

    def _run_text(self, row: int, col: int) -> str:
        item = self.runs.item(row, col)
        return item.text().strip() if item is not None else ""

    def _run_combo_text(self, row: int, col: int) -> str:
        widget = self.runs.cellWidget(row, col)
        if isinstance(widget, QComboBox):
            return str(widget.currentText()).strip()
        return ""

    def _sample_text(self, row: int, col: int) -> str:
        item = self.samples.item(row, col)
        return item.text().strip() if item is not None else ""

    def _sample_combo_data(self, row: int, col: int) -> object:
        widget = self.samples.cellWidget(row, col)
        if isinstance(widget, QComboBox):
            return widget.currentData()
        return None

    # ------------------------------------------------------------------
    # Row meta (source/version/refs survive round-trips untouched)
    # ------------------------------------------------------------------
    @staticmethod
    def _meta_json(item: QTableWidgetItem | None) -> dict:
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

    @staticmethod
    def _set_meta_json(item: QTableWidgetItem | None, meta: dict) -> None:
        if item is not None:
            item.setData(
                Qt.ItemDataRole.UserRole + 1,
                json.dumps(meta, ensure_ascii=False),
            )

    def _set_run_meta(self, row: int, meta: dict) -> None:
        self._set_meta_json(self.runs.item(row, R_TOP), meta)

    def _run_meta(self, row: int) -> dict:
        return self._meta_json(self.runs.item(row, R_TOP))

    def _set_sample_meta(self, row: int, meta: dict) -> None:
        self._set_meta_json(self.samples.item(row, S_DEPTH), meta)

    def _sample_meta(self, row: int) -> dict:
        return self._meta_json(self.samples.item(row, S_DEPTH))
