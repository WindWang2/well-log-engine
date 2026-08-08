"""Stratigraphic dictionary editor dialog (Epic C / FRS §3.x 统一地层层序).

A hierarchical tree editor for the workspace-level dictionary: add child /
sibling units, edit fields (name / level / code / color / line style /
pattern / age / source / version), reorder siblings, delete units. Structural
rules mirror :func:`StratigraphicDictionary.validate`:

* names are required (empty names are rejected by the form);
* a unit with children must be deleted bottom-up (children first);
* units referenced by well data (tops / core samples / well tests /
  perforations — passed in as ``referenced_units`` by the shell) warn before
  deletion; the reference is not silently broken;
* levels are extensible — the eight standard levels are suggested, not
  enforced (any string is allowed, matching the model).

Unit ids are stable across edits (never regenerated), so existing
``unit_id`` references in well sidecars keep pointing at the same unit.

Pure Qt Widgets — headless-testable via qtbot (tests monkeypatch
``_UnitFormDialog`` / ``QMessageBox`` / ``QColorDialog``).
"""

from __future__ import annotations

import uuid
from dataclasses import replace

from PySide6.QtCore import Qt
from PySide6.QtGui import QBrush, QColor
from PySide6.QtWidgets import (
    QColorDialog,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
)

from well_log_workstation.stratigraphy import (
    STANDARD_LEVELS,
    StratigraphicDictionary,
    StratigraphicUnit,
    make_demo_stratigraphy,
)

COL_NAME, COL_LEVEL, COL_CODE, COL_COLOR, COL_AGE, COL_SOURCE, COL_VERSION = range(7)
COLUMN_LABELS = ("名称", "级别", "代码", "颜色", "年代", "来源", "版本")

LINE_STYLES = ("solid", "dashed", "dotted", "dash-dot")


def _next_standard_level(level: str) -> str:
    """The level one step below ``level`` in the standard eight, or level."""
    try:
        idx = STANDARD_LEVELS.index(level)
    except ValueError:
        return level
    if idx + 1 < len(STANDARD_LEVELS):
        return STANDARD_LEVELS[idx + 1]
    return level


class _UnitFormDialog(QDialog):
    """Form for one unit (add or edit); ``values()`` returns the unit."""

    def __init__(
        self,
        unit: StratigraphicUnit | None = None,
        *,
        parent_unit: StratigraphicUnit | None = None,
        default_level: str = "组",
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("StratigraphyUnitForm")
        self.setWindowTitle("编辑地层单元" if unit else "添加地层单元")
        self._unit = unit
        self._color = QColor(unit.color if unit else "#8c8c8c")

        layout = QVBoxLayout(self)
        parent_label = (
            f"父级：{parent_unit.display_label()}"
            if parent_unit is not None
            else "父级：（顶层 / 界）"
        )
        layout.addWidget(QLabel(parent_label))

        form = QFormLayout()
        self.name_edit = QLineEdit(unit.name if unit else "")
        self.name_edit.setObjectName("StratigraphyUnitName")
        form.addRow("名称 *", self.name_edit)

        self.level_combo = QComboBox()
        self.level_combo.setObjectName("StratigraphyUnitLevel")
        self.level_combo.setEditable(True)
        self.level_combo.addItems(STANDARD_LEVELS)
        current_level = unit.level if unit else default_level
        idx = self.level_combo.findText(current_level)
        self.level_combo.setCurrentIndex(idx if idx >= 0 else 0)
        if idx < 0:
            self.level_combo.setEditText(current_level)
        form.addRow("级别", self.level_combo)

        self.code_edit = QLineEdit(unit.code if unit else "")
        self.code_edit.setObjectName("StratigraphyUnitCode")
        form.addRow("代码", self.code_edit)

        self.color_btn = QPushButton()
        self.color_btn.setObjectName("StratigraphyUnitColor")
        self.color_btn.clicked.connect(self._pick_color)
        self._update_color_button()
        form.addRow("颜色", self.color_btn)

        self.style_combo = QComboBox()
        self.style_combo.setObjectName("StratigraphyUnitLineStyle")
        self.style_combo.addItems(LINE_STYLES)
        current_style = unit.line_style if unit else "solid"
        idx = self.style_combo.findText(current_style)
        if idx < 0:
            self.style_combo.addItem(current_style)
            idx = self.style_combo.count() - 1
        self.style_combo.setCurrentIndex(idx)
        form.addRow("线型", self.style_combo)

        self.pattern_edit = QLineEdit(unit.pattern if unit else "")
        self.pattern_edit.setObjectName("StratigraphyUnitPattern")
        form.addRow("花纹", self.pattern_edit)

        self.age_edit = QLineEdit(unit.age if unit else "")
        self.age_edit.setObjectName("StratigraphyUnitAge")
        form.addRow("年代", self.age_edit)

        self.source_edit = QLineEdit(unit.source if unit else "")
        self.source_edit.setObjectName("StratigraphyUnitSource")
        form.addRow("来源", self.source_edit)

        self.version_edit = QLineEdit(unit.version if unit else "")
        self.version_edit.setObjectName("StratigraphyUnitVersion")
        form.addRow("版本", self.version_edit)
        layout.addLayout(form)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    # ------------------------------------------------------------------
    # Color
    # ------------------------------------------------------------------
    def _pick_color(self) -> None:
        color = QColorDialog.getColor(
            self._color, self, "选择单元颜色", QColorDialog.ColorDialogOption.ShowAlphaChannel
        )
        if color.isValid():
            self._color = color
            self._update_color_button()

    def _update_color_button(self) -> None:
        hex_name = self._color.name(QColor.NameFormat.HexRgb)
        self.color_btn.setText(hex_name)
        self.color_btn.setStyleSheet(
            f"background-color: {hex_name}; color: "
            f"{'#ffffff' if self._color.lightness() < 128 else '#000000'};"
        )

    # ------------------------------------------------------------------
    # Value / validation
    # ------------------------------------------------------------------
    def values(self) -> StratigraphicUnit | None:
        """The unit described by the form; ``None`` when the name is empty."""
        name = self.name_edit.text().strip()
        if not name:
            return None
        base = self._unit
        return StratigraphicUnit(
            id=base.id if base is not None else "",
            name=name,
            level=self.level_combo.currentText().strip() or "组",
            parent_id=base.parent_id if base is not None else None,
            code=self.code_edit.text().strip(),
            color=self._color.name(QColor.NameFormat.HexRgb),
            line_style=self.style_combo.currentText().strip() or "solid",
            pattern=self.pattern_edit.text().strip(),
            age=self.age_edit.text().strip(),
            order=base.order if base is not None else 0,
            source=self.source_edit.text().strip(),
            version=self.version_edit.text().strip(),
        )

    def _on_accept(self) -> None:
        if self.values() is None:
            QMessageBox.warning(self, "单元不完整", "名称不能为空。")
            return
        self.accept()


class StratigraphyDialog(QDialog):
    """Tree editor over a workspace ``StratigraphicDictionary``."""

    def __init__(
        self,
        dictionary: StratigraphicDictionary,
        parent=None,
        *,
        referenced_units: set[str] | None = None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("StratigraphyDialog")
        self.setWindowTitle("层序字典编辑")
        self.resize(760, 480)
        self._dictionary = dictionary
        self._referenced = set(referenced_units or ())
        self._items: dict[str, QTreeWidgetItem] = {}

        layout = QVBoxLayout(self)
        layout.addWidget(
            QLabel(
                "工区级地层层序（界→系→统→组→段→小层→砂层组→砂层）。级别可自由扩展；"
                "删除被井数据引用的单元前会先确认。"
            )
        )

        self.tree = QTreeWidget()
        self.tree.setObjectName("StratigraphyTree")
        self.tree.setColumnCount(len(COLUMN_LABELS))
        self.tree.setHeaderLabels(COLUMN_LABELS)
        header = self.tree.header()
        header.setSectionResizeMode(COL_NAME, QHeaderView.ResizeMode.Stretch)
        for col in (COL_LEVEL, COL_CODE, COL_COLOR, COL_AGE, COL_SOURCE, COL_VERSION):
            header.setSectionResizeMode(col, QHeaderView.ResizeMode.ResizeToContents)
        self.tree.itemDoubleClicked.connect(lambda _item, _col: self._edit_selected())
        layout.addWidget(self.tree)

        row_buttons = QHBoxLayout()
        add_child_btn = QPushButton("添加子级")
        add_child_btn.setObjectName("StratigraphyAddChild")
        add_child_btn.clicked.connect(self._add_child)
        row_buttons.addWidget(add_child_btn)
        add_sibling_btn = QPushButton("添加同级")
        add_sibling_btn.setObjectName("StratigraphyAddSibling")
        add_sibling_btn.clicked.connect(self._add_sibling)
        row_buttons.addWidget(add_sibling_btn)
        edit_btn = QPushButton("编辑…")
        edit_btn.setObjectName("StratigraphyEdit")
        edit_btn.clicked.connect(self._edit_selected)
        row_buttons.addWidget(edit_btn)
        del_btn = QPushButton("删除")
        del_btn.setObjectName("StratigraphyDelete")
        del_btn.clicked.connect(self._delete_selected)
        row_buttons.addWidget(del_btn)
        up_btn = QPushButton("上移")
        up_btn.setObjectName("StratigraphyMoveUp")
        up_btn.clicked.connect(lambda: self._move_selected(-1))
        row_buttons.addWidget(up_btn)
        down_btn = QPushButton("下移")
        down_btn.setObjectName("StratigraphyMoveDown")
        down_btn.clicked.connect(lambda: self._move_selected(1))
        row_buttons.addWidget(down_btn)
        row_buttons.addStretch(1)
        demo_btn = QPushButton("生成演示字典")
        demo_btn.setObjectName("StratigraphyDemoFill")
        demo_btn.clicked.connect(self._fill_demo)
        row_buttons.addWidget(demo_btn)
        layout.addLayout(row_buttons)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        self._rebuild()

    # ------------------------------------------------------------------
    # Tree building
    # ------------------------------------------------------------------
    def _rebuild(self) -> None:
        self.tree.clear()
        self._items = {}
        for unit in self._dictionary.root_units():
            self._add_item(None, unit)

    def _add_item(
        self, parent: QTreeWidgetItem | None, unit: StratigraphicUnit
    ) -> None:
        item = QTreeWidgetItem(parent if parent is not None else self.tree)
        item.setData(COL_NAME, Qt.ItemDataRole.UserRole, unit.id)
        item.setText(COL_NAME, unit.name)
        item.setText(COL_LEVEL, unit.level)
        item.setText(COL_CODE, unit.code)
        color = QColor(unit.color or "#8c8c8c")
        item.setBackground(COL_COLOR, QBrush(color))
        item.setForeground(
            COL_COLOR,
            QBrush(QColor("#ffffff") if color.lightness() < 128 else QColor("#000000")),
        )
        item.setText(COL_COLOR, unit.color)
        item.setText(COL_AGE, unit.age)
        item.setText(COL_SOURCE, unit.source)
        item.setText(COL_VERSION, unit.version)
        self._items[unit.id] = item
        for child in self._dictionary.children_of(unit.id):
            self._add_item(item, child)

    def _select(self, unit_id: str) -> None:
        item = self._items.get(unit_id)
        if item is not None:
            self.tree.setCurrentItem(item)
            self.tree.scrollToItem(item)

    def _selected_unit_id(self) -> str | None:
        item = self.tree.currentItem()
        if item is None:
            return None
        return item.data(COL_NAME, Qt.ItemDataRole.UserRole)

    # ------------------------------------------------------------------
    # Mutations
    # ------------------------------------------------------------------
    def _form(self, unit: StratigraphicUnit | None, *, parent_unit: StratigraphicUnit | None = None, default_level: str = "组") -> _UnitFormDialog:
        return _UnitFormDialog(unit, parent_unit=parent_unit, default_level=default_level, parent=self)

    def _add_child(self) -> None:
        selected = self._selected_unit_id()
        if selected is None:
            QMessageBox.information(self, "层序字典", "请先在树中选择父单元。")
            return
        parent = self._dictionary.unit_by_id(selected)
        if parent is None:
            return
        form = self._form(
            None,
            parent_unit=parent,
            default_level=_next_standard_level(parent.level),
        )
        if form.exec() != QDialog.DialogCode.Accepted:
            return
        unit = form.values()
        if unit is None:
            return
        children = self._dictionary.children_of(parent.id)
        new_unit = replace(
            unit,
            id=unit.id or self._new_id(),
            parent_id=parent.id,
            order=len(children),
        )
        self._dictionary.units.append(new_unit)
        self._rebuild()
        self._select(new_unit.id)

    def _add_sibling(self) -> None:
        selected = self._selected_unit_id()
        parent_unit: StratigraphicUnit | None = None
        default_level = "界"
        parent_id: str | None = None
        if selected is not None:
            unit = self._dictionary.unit_by_id(selected)
            if unit is not None:
                parent_id = unit.parent_id
                parent_unit = (
                    self._dictionary.unit_by_id(parent_id) if parent_id else None
                )
                default_level = unit.level
        form = self._form(None, parent_unit=parent_unit, default_level=default_level)
        if form.exec() != QDialog.DialogCode.Accepted:
            return
        unit = form.values()
        if unit is None:
            return
        children = self._dictionary.children_of(parent_id)
        self._dictionary.units.append(
            replace(
                unit,
                id=unit.id or self._new_id(),
                parent_id=parent_id,
                order=len(children),
            )
        )
        self._rebuild()
        self._select(self._dictionary.units[-1].id)

    def _edit_selected(self) -> None:
        unit_id = self._selected_unit_id()
        if unit_id is None:
            QMessageBox.information(self, "层序字典", "请先选择要编辑的单元。")
            return
        unit = self._dictionary.unit_by_id(unit_id)
        if unit is None:
            return
        parent_unit = (
            self._dictionary.unit_by_id(unit.parent_id) if unit.parent_id else None
        )
        form = self._form(unit, parent_unit=parent_unit)
        if form.exec() != QDialog.DialogCode.Accepted:
            return
        new_unit = form.values()
        if new_unit is None:
            return
        idx = self._dictionary.units.index(unit)
        self._dictionary.units[idx] = replace(new_unit, id=unit.id)
        self._rebuild()
        self._select(unit.id)

    def _delete_selected(self) -> None:
        unit_id = self._selected_unit_id()
        if unit_id is None:
            QMessageBox.information(self, "层序字典", "请先选择要删除的单元。")
            return
        unit = self._dictionary.unit_by_id(unit_id)
        if unit is None:
            return
        if self._dictionary.children_of(unit.id):
            QMessageBox.warning(
                self, "无法删除", f"「{unit.name}」还有下级单元，请先删除其子单元。"
            )
            return
        if unit.id in self._referenced:
            answer = QMessageBox.question(
                self,
                "单元被引用",
                f"「{unit.name}」被井数据引用，删除后引用将失效。仍要删除？",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if answer != QMessageBox.StandardButton.Yes:
                return
        self._dictionary.units.remove(unit)
        self._rebuild()

    def _move_selected(self, delta: int) -> None:
        unit_id = self._selected_unit_id()
        if unit_id is None:
            return
        unit = self._dictionary.unit_by_id(unit_id)
        if unit is None:
            return
        siblings = self._dictionary.children_of(unit.parent_id)
        idx = next((i for i, u in enumerate(siblings) if u.id == unit_id), None)
        if idx is None:
            return
        target = idx + delta
        if target < 0 or target >= len(siblings):
            return
        other = siblings[target]
        i1 = self._dictionary.units.index(unit)
        i2 = self._dictionary.units.index(other)
        self._dictionary.units[i1] = replace(unit, order=other.order)
        self._dictionary.units[i2] = replace(other, order=unit.order)
        self._rebuild()
        self._select(unit_id)

    def _fill_demo(self) -> None:
        if self._dictionary.units:
            answer = QMessageBox.question(
                self,
                "生成演示字典",
                "将替换当前字典（8 级示例），继续？",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if answer != QMessageBox.StandardButton.Yes:
                return
        self._dictionary = make_demo_stratigraphy()
        self._rebuild()

    # ------------------------------------------------------------------
    # Accept / value
    # ------------------------------------------------------------------
    def _on_accept(self) -> None:
        # Forms already enforce non-empty names; structural problems (cycles /
        # orphan parents) cannot arise through this UI, but surface any model
        # diagnostics instead of silently writing a broken dictionary.
        problems = self._dictionary.validate()
        if problems:
            QMessageBox.warning(
                self, "层序字典有结构问题", "\n".join(problems[:8])
            )
            return
        self.accept()

    def value(self) -> StratigraphicDictionary:
        """The edited dictionary: ids preserved, order renumbered per level."""
        units: list[StratigraphicUnit] = []

        def walk(parent_id: str | None) -> None:
            for order, unit in enumerate(self._dictionary.children_of(parent_id)):
                units.append(replace(unit, parent_id=parent_id, order=order))
                walk(unit.id)

        walk(None)
        return StratigraphicDictionary(units=units)

    @staticmethod
    def _new_id() -> str:
        return str(uuid.uuid4())
