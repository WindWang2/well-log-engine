"""Workspace mnemonic alias dictionary editor (FRS §1.2 / P0-A).

A simple two-column table dialog: canonical mnemonic → aliases (comma or
space separated). On accept, returns a normalized ``canonical → [aliases]``
dict that the shell writes back to the workspace and activates.
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

from well_log_workstation.mnemonic_alias import normalize_alias_mapping


class MnemonicAliasDialog(QDialog):
    """Edit the workspace mnemonic alias dictionary."""

    def __init__(self, current: dict[str, list[str]] | None = None, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("测井别名字典")
        self.setObjectName("MnemonicAliasDialog")
        layout = QVBoxLayout(self)

        layout.addWidget(
            QLabel(
                "规范名（canonical） → 别名（逗号或空格分隔）。\n"
                "例：GR → GRD, GAPI, NORM_GR。模板/默认显示将按别名匹配曲线。"
            )
        )

        self.table = QTableWidget(0, 2)
        self.table.setObjectName("MnemonicAliasTable")
        self.table.setHorizontalHeaderLabels(["规范名", "别名（逗号分隔）"])
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        layout.addWidget(self.table)

        add_row_btn = QPushButton("添加一行")
        add_row_btn.setObjectName("MnemonicAliasAddRow")
        add_row_btn.clicked.connect(self._add_empty_row)
        layout.addWidget(add_row_btn)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        self._populate(current or {})

    def _add_empty_row(self) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, 0, QTableWidgetItem(""))
        self.table.setItem(r, 1, QTableWidgetItem(""))

    def _populate(self, mapping: dict[str, list[str]]) -> None:
        for canonical, aliases in sorted(mapping.items()):
            r = self.table.rowCount()
            self.table.insertRow(r)
            self.table.setItem(r, 0, QTableWidgetItem(canonical))
            self.table.setItem(r, 1, QTableWidgetItem(", ".join(aliases)))
        if self.table.rowCount() == 0:
            self._add_empty_row()

    def value(self) -> dict[str, list[str]]:
        """Return the edited mapping, normalized (empty rows dropped)."""
        raw: dict[str, list[str]] = {}
        for r in range(self.table.rowCount()):
            canon_item = self.table.item(r, 0)
            alias_item = self.table.item(r, 1)
            canon = canon_item.text().strip() if canon_item is not None else ""
            if not canon:
                continue
            alias_text = alias_item.text() if alias_item is not None else ""
            # Split on commas / whitespace; normalize_alias_mapping dedupes.
            parts = [p for p in alias_text.replace(",", " ").split() if p]
            if parts:
                raw[canon] = parts
        return normalize_alias_mapping(raw)
