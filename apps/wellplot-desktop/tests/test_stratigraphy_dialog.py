"""Stratigraphy dictionary editor dialog (Epic C / C1 UI).

Covers:
* tree building from the hierarchy (columns, sibling order);
* add child / sibling — level suggestion (next standard level / same level),
  root add when nothing is selected;
* edit — id preserved, fields replaced;
* delete — leaf removal, children-first block, referenced-unit confirmation;
* sibling reordering (上移/下移) and order renumbering in ``value()``;
* 生成演示字典 demo fill;
* the unit form itself — required name, color pick.

``_UnitFormDialog`` is monkeypatched with a fake in dialog-level tests so
``exec()`` never blocks; the real form is exercised directly in form tests.
"""

from __future__ import annotations

import os
from dataclasses import replace

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QColorDialog, QDialog, QMessageBox

from well_log_workstation import stratigraphy_dialog as sd
from well_log_workstation.stratigraphy import (
    StratigraphicDictionary,
    StratigraphicUnit,
    make_demo_stratigraphy,
)
from well_log_workstation.stratigraphy_dialog import (
    COL_AGE,
    COL_CODE,
    COL_COLOR,
    COL_LEVEL,
    COL_NAME,
    StratigraphyDialog,
    _UnitFormDialog,
)

STANDARD = ("界", "系", "统", "组", "段", "小层", "砂层组", "砂层")


def _dictionary() -> StratigraphicDictionary:
    """One 界→…→砂层 chain plus a second 组 sibling (same as model tests)."""
    units = [
        StratigraphicUnit(id="e1", name="新生界", level="界", order=0),
        StratigraphicUnit(id="s1", name="古近系", level="系", parent_id="e1", order=0),
        StratigraphicUnit(id="u1", name="始新统", level="统", parent_id="s1", order=0),
        StratigraphicUnit(id="f1", name="沙四段", level="组", parent_id="u1", order=0),
        StratigraphicUnit(id="m1", name="沙四上亚段", level="段", parent_id="f1", order=0),
        StratigraphicUnit(id="b1", name="1 小层", level="小层", parent_id="m1", order=0),
        StratigraphicUnit(id="g1", name="砂层组 A", level="砂层组", parent_id="b1", order=0),
        StratigraphicUnit(id="z1", name="砂层 1", level="砂层", parent_id="g1", order=0),
        StratigraphicUnit(
            id="f2", name="沙三段", level="组", parent_id="u1", order=1,
            code="E2s3", color="#3b6fb5", age="始新世",
        ),
    ]
    return StratigraphicDictionary(units=units)


class FakeForm:
    """Drop-in for ``_UnitFormDialog``: records constructor args, auto-accepts."""

    last: "FakeForm | None" = None
    mutate: bool = False

    def __init__(
        self,
        unit: StratigraphicUnit | None = None,
        *,
        parent_unit: StratigraphicUnit | None = None,
        default_level: str = "组",
        parent=None,
    ) -> None:
        self.unit = unit
        self.parent_unit = parent_unit
        self.default_level = default_level
        FakeForm.last = self

    def exec(self) -> int:
        return QDialog.DialogCode.Accepted

    def values(self) -> StratigraphicUnit | None:
        if self.unit is None:
            return StratigraphicUnit(id="", name="新单元", level=self.default_level)
        if FakeForm.mutate:
            return replace(self.unit, name="沙三段改", color="#ff0000")
        return self.unit


@pytest.fixture
def fake_form(monkeypatch: pytest.MonkeyPatch) -> None:
    FakeForm.mutate = False
    FakeForm.last = None
    monkeypatch.setattr(sd, "_UnitFormDialog", FakeForm)


# ---------------------------------------------------------------------------
# Model-level demo dictionary
# ---------------------------------------------------------------------------


def test_demo_dictionary_structure() -> None:
    d = make_demo_stratigraphy()
    assert d.validate() == []
    assert len(d.units) == 9
    assert [u.name for u in d.root_units()] == ["新生界"]
    leaf = next(u for u in d.units if u.level == "砂层")
    chain = [u.name for u in d.path_to(leaf.id)]
    assert len(chain) == 8  # one unit per standard level


def test_next_standard_level() -> None:
    assert sd._next_standard_level("组") == "段"
    assert sd._next_standard_level("砂层") == "砂层"  # last level stays
    assert sd._next_standard_level("未知级") == "未知级"  # extensible


# ---------------------------------------------------------------------------
# Dialog: tree building
# ---------------------------------------------------------------------------


def test_tree_builds_hierarchy(qtbot) -> None:
    dlg = StratigraphyDialog(_dictionary())
    qtbot.addWidget(dlg)
    tree = dlg.tree
    assert tree.topLevelItemCount() == 1
    e1 = tree.topLevelItem(0)
    assert e1.text(COL_NAME) == "新生界"
    assert e1.text(COL_LEVEL) == "界"
    s1 = e1.child(0)
    assert s1.text(COL_NAME) == "古近系"
    u1 = s1.child(0)
    assert u1.childCount() == 2
    assert [u1.child(i).text(COL_NAME) for i in range(2)] == ["沙四段", "沙三段"]
    f2 = u1.child(1)
    assert f2.text(COL_CODE) == "E2s3"
    assert f2.text(COL_AGE) == "始新世"
    assert f2.text(COL_COLOR) == "#3b6fb5"


# ---------------------------------------------------------------------------
# Dialog: add child / sibling
# ---------------------------------------------------------------------------


def test_add_child_suggests_next_level(qtbot, fake_form) -> None:
    dlg = StratigraphyDialog(_dictionary())
    qtbot.addWidget(dlg)
    dlg._select("f1")  # 组
    dlg._add_child()
    assert FakeForm.last is not None
    assert FakeForm.last.default_level == "段"  # next standard level
    assert FakeForm.last.parent_unit is not None
    assert FakeForm.last.parent_unit.id == "f1"
    value = dlg.value()
    # find by name (id is auto-assigned)
    added = next((u for u in value.units if u.name == "新单元"), None)
    assert added is not None
    assert added.parent_id == "f1"
    assert added.level == "段"
    assert value.validate() == []


def test_add_sibling_shares_level(qtbot, fake_form) -> None:
    dlg = StratigraphyDialog(_dictionary())
    qtbot.addWidget(dlg)
    dlg._select("f2")
    dlg._add_sibling()
    assert FakeForm.last.default_level == "组"
    added = next((u for u in dlg.value().units if u.name == "新单元"), None)
    assert added is not None
    assert added.parent_id == "u1"
    assert added.order == 2  # third 组 under u1


def test_add_root_without_selection(qtbot, fake_form) -> None:
    dlg = StratigraphyDialog(_dictionary())
    qtbot.addWidget(dlg)
    dlg._add_sibling()  # nothing selected → root unit
    assert FakeForm.last.default_level == "界"
    assert FakeForm.last.parent_unit is None
    roots = dlg.value().root_units()
    assert [u.name for u in roots] == ["新生界", "新单元"]


# ---------------------------------------------------------------------------
# Dialog: edit
# ---------------------------------------------------------------------------


def test_edit_keeps_id_changes_fields(qtbot, fake_form) -> None:
    FakeForm.mutate = True
    dlg = StratigraphyDialog(_dictionary())
    qtbot.addWidget(dlg)
    dlg._select("f2")
    dlg._edit_selected()
    value = dlg.value()
    edited = value.unit_by_id("f2")
    assert edited is not None
    assert edited.name == "沙三段改"
    assert edited.color == "#ff0000"
    assert edited.code == "E2s3"  # untouched fields preserved
    assert value.validate() == []


# ---------------------------------------------------------------------------
# Dialog: delete
# ---------------------------------------------------------------------------


def test_delete_leaf(qtbot) -> None:
    dlg = StratigraphyDialog(_dictionary())
    qtbot.addWidget(dlg)
    dlg._select("z1")
    dlg._delete_selected()
    assert dlg.value().unit_by_id("z1") is None
    assert dlg.value().validate() == []


def test_delete_parent_with_children_blocked(qtbot, monkeypatch: pytest.MonkeyPatch) -> None:
    warnings: list[tuple] = []
    monkeypatch.setattr(
        QMessageBox, "warning", lambda *a, **k: warnings.append(a)
    )
    dlg = StratigraphyDialog(_dictionary())
    qtbot.addWidget(dlg)
    dlg._select("f1")  # has child m1
    dlg._delete_selected()
    assert warnings, "expected a children-first warning"
    assert dlg.value().unit_by_id("f1") is not None


def test_delete_referenced_unit_confirms(qtbot, monkeypatch: pytest.MonkeyPatch) -> None:
    answers = iter(
        [QMessageBox.StandardButton.No, QMessageBox.StandardButton.Yes]
    )
    monkeypatch.setattr(
        QMessageBox, "question", lambda *a, **k: next(answers)
    )
    dlg = StratigraphyDialog(_dictionary(), referenced_units={"z1"})
    qtbot.addWidget(dlg)

    dlg._select("z1")
    dlg._delete_selected()
    assert dlg.value().unit_by_id("z1") is not None  # declined

    dlg._select("z1")
    dlg._delete_selected()
    assert dlg.value().unit_by_id("z1") is None  # confirmed


def test_delete_unreferenced_leaf_no_question(qtbot, monkeypatch: pytest.MonkeyPatch) -> None:
    questions: list[tuple] = []
    monkeypatch.setattr(
        QMessageBox, "question", lambda *a, **k: questions.append(a) or QMessageBox.StandardButton.Yes
    )
    dlg = StratigraphyDialog(_dictionary(), referenced_units={"zzz"})
    qtbot.addWidget(dlg)
    dlg._select("z1")
    dlg._delete_selected()
    assert questions == []  # not referenced → no confirmation


# ---------------------------------------------------------------------------
# Dialog: reorder + demo fill + value
# ---------------------------------------------------------------------------


def test_move_up_down_reorders_siblings(qtbot) -> None:
    dlg = StratigraphyDialog(_dictionary())
    qtbot.addWidget(dlg)

    def u1_children() -> list[str]:
        return [u.name for u in dlg.value().children_of("u1")]

    assert u1_children() == ["沙四段", "沙三段"]
    dlg._select("f2")
    dlg._move_selected(-1)
    assert u1_children() == ["沙三段", "沙四段"]
    dlg._move_selected(1)
    assert u1_children() == ["沙四段", "沙三段"]


def test_value_renumbers_order_sequentially(qtbot, fake_form) -> None:
    dlg = StratigraphyDialog(_dictionary())
    qtbot.addWidget(dlg)
    dlg._select("f2")
    dlg._move_selected(-1)  # f2 order 0, f1 order 1
    dlg._select("u1")
    dlg._add_child()  # new unit appended as u1's third child
    value = dlg.value()
    orders = [u.order for u in value.children_of("u1")]
    assert orders == [0, 1, 2]


def test_demo_fill_replaces_dictionary(qtbot, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(
        QMessageBox,
        "question",
        lambda *a, **k: QMessageBox.StandardButton.Yes,
    )
    dlg = StratigraphyDialog(_dictionary())
    qtbot.addWidget(dlg)
    dlg._fill_demo()
    value = dlg.value()
    assert len(value.units) == 9
    assert value.validate() == []
    assert value.root_units()[0].name == "新生界"


# ---------------------------------------------------------------------------
# Unit form (real widget)
# ---------------------------------------------------------------------------


def test_form_requires_name(qtbot, monkeypatch: pytest.MonkeyPatch) -> None:
    warnings: list[tuple] = []
    monkeypatch.setattr(
        QMessageBox, "warning", lambda *a, **k: warnings.append(a)
    )
    form = _UnitFormDialog(StratigraphicUnit(id="u9", name="旧名", level="组"))
    qtbot.addWidget(form)
    form.name_edit.clear()
    form._on_accept()
    assert warnings  # empty name rejected
    assert form.result() != QDialog.DialogCode.Accepted

    form.name_edit.setText("新名")
    form._on_accept()
    assert form.result() == QDialog.DialogCode.Accepted
    unit = form.values()
    assert unit is not None
    assert unit.id == "u9"  # id preserved for edits
    assert unit.name == "新名"


def test_form_level_presets_and_extensible(qtbot) -> None:
    form = _UnitFormDialog(None, default_level="组")
    qtbot.addWidget(form)
    assert form.level_combo.currentText() == "组"
    form.name_edit.setText("亚组单元")
    # Extensible level: any string accepted.
    form.level_combo.setEditText("亚组")
    assert form.values() is not None
    assert form.values().level == "亚组"


def test_form_color_pick(qtbot, monkeypatch: pytest.MonkeyPatch) -> None:
    from PySide6.QtGui import QColor

    monkeypatch.setattr(
        QColorDialog,
        "getColor",
        lambda *a, **k: QColor("#123456"),
    )
    form = _UnitFormDialog(StratigraphicUnit(id="u9", name="旧名"))
    qtbot.addWidget(form)
    form._pick_color()
    assert form.values().color == "#123456"
    assert "#123456" in form.color_btn.text()
