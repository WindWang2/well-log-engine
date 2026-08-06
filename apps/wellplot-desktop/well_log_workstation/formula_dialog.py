"""Formula calculator editor dialog (FRS §2.4 / P2-A).

A two-column table (name / expression). On accept the expressions are
pre-validated by the parser; invalid rows get a warning instead of being
silently dropped.
"""

from __future__ import annotations

from PySide6.QtWidgets import (
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

from well_log_workstation.formula import Formula, FormulaError, parse_expression


class FormulaDialog(QDialog):
    """Edit the derived-curve formulas for the selected well."""

    def __init__(self, current: list[Formula] | None = None, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("公式计算器")
        self.setObjectName("FormulaDialog")
        layout = QVBoxLayout(self)

        layout.addWidget(
            QLabel(
                "名称 = 表达式；变量为曲线助记符（如 GR）。\n"
                "例：VSH = (GR - 25) / (150 - 25)\n"
                "函数：log10(x) ln(x) exp(x) sqrt(x) abs(x) round(x) "
                "min(a,b) max(a,b)；^ 幂、/ 除、括号。"
            )
        )

        self.table = QTableWidget(0, 2)
        self.table.setObjectName("FormulaTable")
        self.table.setHorizontalHeaderLabels(["名称", "表达式"])
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        layout.addWidget(self.table)

        add_btn = QPushButton("添加公式")
        add_btn.setObjectName("FormulaAddRow")
        add_btn.clicked.connect(self._add_empty_row)
        layout.addWidget(add_btn)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        for f in current or []:
            self._add_row(f)
        if self.table.rowCount() == 0:
            self._add_row(Formula("VSH", "(GR - 25) / (150 - 25)"))

    def _add_empty_row(self) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, 0, QTableWidgetItem(""))
        self.table.setItem(r, 1, QTableWidgetItem(""))

    def _add_row(self, f: Formula) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, 0, QTableWidgetItem(f.name))
        self.table.setItem(r, 1, QTableWidgetItem(f.expression))

    def _on_accept(self) -> None:
        # Validate every expression before accepting.
        for r in range(self.table.rowCount()):
            name_item = self.table.item(r, 0)
            expr_item = self.table.item(r, 1)
            name = name_item.text().strip() if name_item is not None else ""
            expr = expr_item.text().strip() if expr_item is not None else ""
            if not name and not expr:
                continue  # empty rows are dropped
            if not name or not expr:
                QMessageBox.warning(self, "公式不完整", f"第 {r + 1} 行：名称与表达式均不能为空。")
                return
            try:
                parse_expression(expr)
            except FormulaError as exc:
                QMessageBox.warning(
                    self, "公式语法错误",
                    f"第 {r + 1} 行（{name}）：{exc}",
                )
                return
        self.accept()

    def value(self) -> list[Formula]:
        """Return the validated formulas (empty rows dropped)."""
        out: list[Formula] = []
        for r in range(self.table.rowCount()):
            name_item = self.table.item(r, 0)
            expr_item = self.table.item(r, 1)
            name = name_item.text().strip() if name_item is not None else ""
            expr = expr_item.text().strip() if expr_item is not None else ""
            if name and expr:
                out.append(Formula(name=name, expression=expr))
        return out
