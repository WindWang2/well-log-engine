"""Section fault editor dialog (FRS §3.3 / P1-A).

A table editor for the reservoir section's faults (position + throw model).
Columns: 名 / 左井 / 右井 / 位置(0..1) / 顶深 / 底深 / 落差. On accept returns
a list of :class:`SectionFault2D`.
"""

from __future__ import annotations

from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QHeaderView,
    QLabel,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
)

from well_log_workstation.section_geometry.fault_section import (
    SectionFault2D,
    faults_from_json,
    faults_to_json,
)


class SectionFaultDialog(QDialog):
    """Edit the faults of a reservoir section plot."""

    def __init__(
        self,
        current: list[SectionFault2D] | None = None,
        well_count: int = 2,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("编辑断层")
        self.setObjectName("SectionFaultDialog")
        self._well_count = max(2, int(well_count))
        layout = QVBoxLayout(self)

        layout.addWidget(
            QLabel(
                "落差 throw：正断 > 0（下盘下降）/ 逆断 < 0（下盘上升）。\n"
                "左井/右井为井列索引（0 起）；位置 0=左井 1=右井。"
            )
        )

        self.table = QTableWidget(0, 7)
        self.table.setObjectName("SectionFaultTable")
        self.table.setHorizontalHeaderLabels(
            ["名", "左井", "右井", "位置(0..1)", "顶深", "底深", "落差"]
        )
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        for col in range(1, 7):
            header.setSectionResizeMode(col, QHeaderView.ResizeMode.ResizeToContents)
        layout.addWidget(self.table)

        add_btn = QPushButton("添加断层")
        add_btn.setObjectName("SectionFaultAddRow")
        add_btn.clicked.connect(self._add_default_row)
        layout.addWidget(add_btn)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for fault in current or []:
            self._add_row(fault)
        if self.table.rowCount() == 0:
            self._add_default_row()

    def _add_default_row(self) -> None:
        self._add_row(
            SectionFault2D(
                name=f"断层 {self.table.rowCount() + 1}",
                between=(0, min(1, self._well_count - 1)),
                x_frac=0.5,
                top_depth=0.0,
                bottom_depth=0.0,
                throw=0.0,
            )
        )

    def _add_row(self, fault: SectionFault2D) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, 0, QTableWidgetItem(fault.name))
        self.table.setItem(r, 1, QTableWidgetItem(str(fault.between[0])))
        self.table.setItem(r, 2, QTableWidgetItem(str(fault.between[1])))
        self.table.setItem(r, 3, QTableWidgetItem(str(fault.x_frac)))
        self.table.setItem(r, 4, QTableWidgetItem(str(fault.top_depth)))
        self.table.setItem(r, 5, QTableWidgetItem(str(fault.bottom_depth)))
        self.table.setItem(r, 6, QTableWidgetItem(str(fault.throw)))

    def value(self) -> list[SectionFault2D]:
        """Return the edited fault list (invalid rows dropped)."""
        raw: list[dict] = []
        for r in range(self.table.rowCount()):
            cells = [self.table.item(r, c) for c in range(7)]
            texts = [(c.text() if c is not None else "") for c in cells]
            name = texts[0].strip()
            try:
                left = int(float(texts[1]))
                right = int(float(texts[2]))
                x_frac = float(texts[3])
                top = float(texts[4])
                bottom = float(texts[5])
                throw = float(texts[6])
            except (TypeError, ValueError):
                continue
            raw.append(
                {
                    "name": name,
                    "between": [left, right],
                    "x_frac": x_frac,
                    "top_depth": top,
                    "bottom_depth": bottom,
                    "throw": throw,
                }
            )
        return faults_from_json(raw)
