"""Deviation survey editor dialog (FRS §1.1 / P1-C).

A table editor for a well's MD / inclination / azimuth survey stations. On
accept returns a list of :class:`SurveyStation`. Empty/invalid rows are
dropped.
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

from well_log_workstation.survey import SurveyStation, survey_from_json, survey_to_json


class SurveyDialog(QDialog):
    """Edit the deviation survey stations for one well."""

    def __init__(
        self,
        well_name: str,
        current: list[SurveyStation] | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle(f"测斜数据 · {well_name}")
        self.setObjectName("SurveyDialog")
        layout = QVBoxLayout(self)

        layout.addWidget(
            QLabel(
                "每行一个测斜站：MD（测井深度 m）/ 倾角 Inc（°，0=垂直）/ 方位角 Az（°，0=北）。\n"
                "用于最小曲率法计算 TVD/TVDSS/位移（FRS §1.1）。"
            )
        )

        self.table = QTableWidget(0, 3)
        self.table.setObjectName("SurveyTable")
        self.table.setHorizontalHeaderLabels(["MD (m)", "倾角 (°)", "方位 (°)"])
        header = self.table.horizontalHeader()
        for col in range(3):
            header.setSectionResizeMode(col, QHeaderView.ResizeMode.Stretch)
        layout.addWidget(self.table)

        add_btn = QPushButton("添加测站")
        add_btn.setObjectName("SurveyAddStation")
        add_btn.clicked.connect(self._add_empty_row)
        layout.addWidget(add_btn)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for s in current or []:
            self._add_row(s)
        if self.table.rowCount() == 0:
            # Seed with a vertical first station at MD 0.
            self._add_row(SurveyStation(0.0, 0.0, 0.0))

    def _add_empty_row(self) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, 0, QTableWidgetItem(""))
        self.table.setItem(r, 1, QTableWidgetItem("0"))
        self.table.setItem(r, 2, QTableWidgetItem("0"))

    def _add_row(self, s: SurveyStation) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, 0, QTableWidgetItem(str(s.md)))
        self.table.setItem(r, 1, QTableWidgetItem(str(s.inc_deg)))
        self.table.setItem(r, 2, QTableWidgetItem(str(s.az_deg)))

    def value(self) -> list[SurveyStation]:
        """Return the edited stations (invalid rows dropped)."""
        raw: list[dict] = []
        for r in range(self.table.rowCount()):
            cells = [self.table.item(r, c) for c in range(3)]
            texts = [(c.text().strip() if c is not None else "") for c in cells]
            if not any(texts):
                continue
            raw.append(
                {
                    "md": texts[0],
                    "inc": texts[1] or "0",
                    "az": texts[2] or "0",
                }
            )
        # survey_from_json parses floats tolerantly; pre-validate to keep only
        # rows where MD is numeric (the key field).
        out: list[SurveyStation] = []
        for item in survey_from_json(raw):
            out.append(item)
        return out
