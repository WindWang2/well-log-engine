"""Shared 层位单元 selector for the Epic C editors (tops / core / well-test /
perforation).

One combo widget built from the workspace stratigraphic dictionary; empty
value = historic free text. A dangling current id (reference to a unit that
was deleted from the dictionary) is listed as 「（未知单元 …）」 so existing
data stays visible and the reference is preserved verbatim instead of being
silently reset.
"""

from __future__ import annotations

from PySide6.QtWidgets import QComboBox

from well_log_workstation.stratigraphy import StratigraphicDictionary

FREETEXT_ITEM = "（自由文本）"


def make_unit_combo(
    dictionary: StratigraphicDictionary | None, current_id: str = ""
) -> QComboBox:
    """Hierarchy-indented unit combo; returns a fresh widget each call."""
    combo = QComboBox()
    combo.setObjectName("UnitRefCombo")
    combo.addItem(FREETEXT_ITEM, userData="")
    dictionary = dictionary if dictionary is not None else StratigraphicDictionary()
    if dictionary.units:
        def walk(parent_id: str | None, depth: int) -> None:
            for unit in dictionary.children_of(parent_id):
                combo.addItem("　" * depth + unit.display_label(), userData=unit.id)
                walk(unit.id, depth + 1)

        walk(None, 0)
    if current_id and dictionary.unit_by_id(current_id) is None:
        combo.addItem(f"（未知单元 {current_id}）", userData=current_id)
    idx = combo.findData(current_id)
    if idx >= 0:
        combo.setCurrentIndex(idx)
    return combo
