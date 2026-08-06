"""Section fluid-contact editor dialog (FRS §3.3 / P1-B).

A table editor for the reservoir section's fluid contacts (OWC/GOC). One row
per contact: fluid type + a depth cell per participating well. Empty depth =
that well has no contact for the fluid (the contact line breaks there).
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

from well_log_workstation.section_geometry.contact_section import (
    FluidContact2D,
    contacts_from_json,
    contacts_to_json,
)


class SectionContactDialog(QDialog):
    """Edit the fluid contacts of a reservoir section plot."""

    def __init__(
        self,
        current: list[FluidContact2D] | None = None,
        well_count: int = 2,
        well_names: list[str] | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("编辑流体界面")
        self.setObjectName("SectionContactDialog")
        self._well_count = max(2, int(well_count))
        self._well_names = well_names or []
        layout = QVBoxLayout(self)

        layout.addWidget(
            QLabel(
                "类型：OWC=油水界面 / GOC=气油界面。\n"
                "各井深度留空 = 该井无此界面（接触线在该井处断开）。\n"
                "界面切割充填：上部油(红)/气(黄)、下部水(蓝)；"
                "过渡带厚度>0 时在界面两侧增加混色过渡条带。"
            )
        )

        n_cols = 2 + self._well_count
        self.table = QTableWidget(0, n_cols)
        self.table.setObjectName("SectionContactTable")
        headers = ["类型", "过渡带(m)"]
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

        add_btn = QPushButton("添加界面")
        add_btn.setObjectName("SectionContactAddRow")
        add_btn.clicked.connect(lambda: self._add_row(FluidContact2D("owc")))
        layout.addWidget(add_btn)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for contact in current or []:
            self._add_row(contact)
        if self.table.rowCount() == 0:
            self._add_row(FluidContact2D("owc"))

    def _add_row(self, contact: FluidContact2D) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        combo = QComboBox()
        combo.addItem("OWC（油水）", "owc")
        combo.addItem("GOC（气油）", "goc")
        idx = combo.findData(contact.fluid_type)
        combo.setCurrentIndex(idx if idx >= 0 else 0)
        self.table.setCellWidget(r, 0, combo)
        t_text = (
            ""
            if float(contact.transition_m or 0.0) <= 0.0
            else str(float(contact.transition_m))
        )
        self.table.setItem(r, 1, QTableWidgetItem(t_text))
        for i in range(self._well_count):
            d = contact.depths.get(i)
            text = "" if d is None else str(d)
            self.table.setItem(r, 2 + i, QTableWidgetItem(text))

    def value(self) -> list[FluidContact2D]:
        """Return the edited contact list (rows with <2 depths dropped)."""
        raw: list[dict] = []
        for r in range(self.table.rowCount()):
            combo = self.table.cellWidget(r, 0)
            fluid = (
                combo.currentData()
                if isinstance(combo, QComboBox)
                else "owc"
            )
            t_item = self.table.item(r, 1)
            t_text = t_item.text().strip() if t_item is not None else ""
            try:
                transition_m = max(0.0, float(t_text)) if t_text else 0.0
            except ValueError:
                transition_m = 0.0
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
            entry: dict = {"fluid_type": str(fluid), "depths": depths}
            if transition_m > 0.0:
                entry["transition_m"] = transition_m
            raw.append(entry)
        return contacts_from_json(raw)
