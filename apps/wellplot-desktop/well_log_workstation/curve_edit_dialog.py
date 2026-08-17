"""Curve edit dialog (FRS §2.x 曲线编辑 Despike / 基线平移).

A table editor mirroring :class:`FormulaDialog`: one row per edit with
curve mnemonic + method + method parameters. On accept the rows are
validated; invalid rows get a warning instead of being silently dropped.
"""

from __future__ import annotations

import math

from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QHeaderView,
    QLabel,
    QMessageBox,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
)

from PySide6.QtCore import Qt

from well_log_workstation.curve_edit import CurveEdit

class CurveEditDialog(QDialog):
    """Edit the non-destructive curve edits for the selected well."""

    def __init__(
        self,
        current: list[CurveEdit] | None = None,
        parent=None,
        *,
        curve_mnemonics: list[str] | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("曲线编辑（去毛刺 / 基线平移）")
        self.setObjectName("CurveEditDialog")
        layout = QVBoxLayout(self)

        layout.addWidget(
            QLabel(
                "编辑为非破坏派生：原始曲线不变，校正曲线以绿色附加显示。\n"
                "去毛刺：邻域中值 ± 阈值×MAD 之外的采样点替换为中值；\n"
                "基线平移：整条曲线加常量偏移。"
            )
        )

        self.table = QTableWidget(0, 4)
        self.table.setObjectName("CurveEditTable")
        self.table.setHorizontalHeaderLabels(
            ["曲线助记符", "方法", "窗口/偏移", "阈值"]
        )
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(3, QHeaderView.ResizeMode.Stretch)
        layout.addWidget(self.table)

        self._mnemonics: list[str] = [str(m) for m in (curve_mnemonics or [])]

        add_btn = QPushButton("添加编辑")
        add_btn.setObjectName("CurveEditAddRow")
        add_btn.clicked.connect(self._add_empty_row)
        layout.addWidget(add_btn)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for e in current or []:
            self._add_row(e)
        if self.table.rowCount() == 0:
            self._add_row(
                CurveEdit(
                    mnemonic=self._mnemonics[0] if self._mnemonics else "GR",
                    method="despike",
                    window=5,
                    threshold=3.0,
                )
            )

    def _add_empty_row(self) -> None:
        self._add_row(
            CurveEdit(
                mnemonic=self._mnemonics[0] if self._mnemonics else "GR",
                method="despike",
                window=5,
                threshold=3.0,
            )
        )

    def _add_row(self, edit: CurveEdit) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)

        combo = QComboBox()
        combo.setObjectName("CurveEditMethodCombo")
        combo.addItem("去毛刺", "despike")
        combo.addItem("基线平移", "baseline")
        combo.addItem("手绘", "freehand")
        idx = combo.findData(edit.method)
        combo.setCurrentIndex(idx if idx >= 0 else 0)
        self.table.setCellWidget(r, 1, combo)

        mnemonic_item = QTableWidgetItem(edit.mnemonic)
        # The original edit is kept on the row so freehand points survive an
        # OK round-trip unchanged (#588).
        mnemonic_item.setData(Qt.ItemDataRole.UserRole, edit)
        self.table.setItem(r, 0, mnemonic_item)
        # Param 1: despike → window (odd); baseline → shift; freehand → none.
        p1 = QTableWidgetItem(
            f"{edit.window}"
            if edit.method == "despike"
            else (f"{edit.shift:g}" if edit.method == "baseline" else "")
        )
        self.table.setItem(r, 2, p1)
        # Param 2: despike → threshold; baseline/freehand → unused.
        p2 = QTableWidgetItem(
            f"{edit.threshold:g}" if edit.method == "despike" else ""
        )
        self.table.setItem(r, 3, p2)

        combo.currentIndexChanged.connect(
            lambda _row=r, _combo=combo: self._sync_params_row(_row, _combo)
        )

    def _sync_params_row(self, row: int, combo: QComboBox) -> None:
        """Reset the parameter cells when the method changes."""
        method = combo.currentData() or "despike"
        p1 = self.table.item(row, 2)
        p2 = self.table.item(row, 3)
        if p1 is not None:
            p1.setText("5" if method == "despike" else ("0" if method == "baseline" else ""))
        if p2 is not None:
            p2.setText("3" if method == "despike" else "")

    def _on_accept(self) -> None:
        for r in range(self.table.rowCount()):
            mn = self.table.item(r, 0)
            combo = self.table.cellWidget(r, 1)
            p1 = self.table.item(r, 2)
            p2 = self.table.item(r, 3)
            mnemonic = mn.text().strip() if mn is not None else ""
            method = combo.currentData() if combo is not None else "despike"
            if not mnemonic:
                # Empty rows are dropped; a name without params is incomplete.
                if all(
                    (it is None or not it.text().strip())
                    for it in (p1, p2)
                ):
                    continue
                QMessageBox.warning(self, "编辑不完整", f"第 {r + 1} 行：曲线助记符不能为空。")
                return
            try:
                window = int(p1.text()) if p1 is not None and p1.text().strip() else 5
            except ValueError:
                window = -1
            try:
                shift = float(p1.text()) if p1 is not None and p1.text().strip() else 0.0
            except ValueError:
                shift = float("nan")
            try:
                threshold = (
                    float(p2.text()) if p2 is not None and p2.text().strip() else 3.0
                )
            except ValueError:
                threshold = float("nan")
            if method == "freehand":
                # Freehand edits carry their points on the original edit and
                # need no numeric parameters (#588).
                continue
            if method == "despike":
                if window < 3 or not math.isfinite(threshold) or threshold <= 0:
                    QMessageBox.warning(
                        self,
                        "参数无效",
                        f"第 {r + 1} 行：去毛刺需要 窗口≥3 且 阈值>0。",
                    )
                    return
            elif method == "baseline":
                if not math.isfinite(shift):
                    QMessageBox.warning(
                        self, "参数无效", f"第 {r + 1} 行：偏移必须是数字。"
                    )
                    return
        self.accept()

    def value(self) -> list[CurveEdit]:
        """Return the validated edits (empty rows dropped)."""
        out: list[CurveEdit] = []
        for r in range(self.table.rowCount()):
            mn = self.table.item(r, 0)
            combo = self.table.cellWidget(r, 1)
            p1 = self.table.item(r, 2)
            p2 = self.table.item(r, 3)
            mnemonic = mn.text().strip() if mn is not None else ""
            method = combo.currentData() if combo is not None else "despike"
            if not mnemonic:
                continue
            try:
                if method == "freehand":
                    # Preserve the original freehand definition verbatim
                    # (points included); only the mnemonic may have changed.
                    # A row that was switched TO freehand has no stored
                    # points and yields a no-op freehand edit (#588).
                    original = (
                        mn.data(Qt.ItemDataRole.UserRole)
                        if mn is not None
                        else None
                    )
                    out.append(
                        CurveEdit(
                            mnemonic=mnemonic,
                            method="freehand",
                            window=(
                                getattr(original, "window", 5)
                                if original is not None
                                else 5
                            ),
                            threshold=(
                                getattr(original, "threshold", 3.0)
                                if original is not None
                                else 3.0
                            ),
                            shift=(
                                getattr(original, "shift", 0.0)
                                if original is not None
                                else 0.0
                            ),
                            points=(
                                tuple(getattr(original, "points", ()) or ())
                                if original is not None
                                else ()
                            ),
                        )
                    )
                elif method == "despike":
                    window = (
                        int(p1.text())
                        if p1 is not None and p1.text().strip()
                        else 5
                    )
                    threshold = (
                        float(p2.text())
                        if p2 is not None and p2.text().strip()
                        else 3.0
                    )
                    out.append(
                        CurveEdit(
                            mnemonic=mnemonic,
                            method="despike",
                            window=max(3, window),
                            threshold=max(0.1, threshold),
                        )
                    )
                else:
                    shift = (
                        float(p1.text())
                        if p1 is not None and p1.text().strip()
                        else 0.0
                    )
                    out.append(
                        CurveEdit(mnemonic=mnemonic, method="baseline", shift=shift)
                    )
            except ValueError:
                continue
        return out
