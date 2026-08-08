"""Unified stratigraphic dictionary + FormationTop unit references (Epic C).

Covers:
* hierarchy navigation — root/children ordering, path_to ancestor chain over
  the standard eight levels, unknown ids;
* validation — duplicate ids, orphan parents, cycles;
* persistence round-trip (workspace-level stratigraphy.json, tolerant read);
* FormationTop.unit_id — write/read round-trip and old tops.json (without the
  field) stays readable (no schema bump, additive optional field).
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.stratigraphy import (
    STANDARD_LEVELS,
    StratigraphicDictionary,
    StratigraphicUnit,
    load_stratigraphy,
    save_stratigraphy,
)


def _dictionary() -> StratigraphicDictionary:
    """Standard eight-level hierarchy: 界 → 系 → 统 → 组 → 段 → 小层 →
    砂层组 → 砂层 (one unit per level)."""
    units = [
        StratigraphicUnit(id="e1", name="新生界", level="界", order=0),
        StratigraphicUnit(id="s1", name="古近系", level="系", parent_id="e1", order=0),
        StratigraphicUnit(id="u1", name="始新统", level="统", parent_id="s1", order=0),
        StratigraphicUnit(id="f1", name="沙四段", level="组", parent_id="u1", order=0),
        StratigraphicUnit(id="m1", name="沙四上亚段", level="段", parent_id="f1", order=0),
        StratigraphicUnit(id="b1", name="1 小层", level="小层", parent_id="m1", order=0),
        StratigraphicUnit(id="g1", name="砂层组 A", level="砂层组", parent_id="b1", order=0),
        StratigraphicUnit(id="z1", name="砂层 1", level="砂层", parent_id="g1", order=0),
        # A second 组 sibling with explicit ordering.
        StratigraphicUnit(
            id="f2", name="沙三段", level="组", parent_id="u1", order=1,
            code="E2s3", color="#3b6fb5", age="始新世",
        ),
    ]
    return StratigraphicDictionary(units=units)


def test_standard_levels_order() -> None:
    assert STANDARD_LEVELS == (
        "界", "系", "统", "组", "段", "小层", "砂层组", "砂层",
    )


def test_hierarchy_navigation() -> None:
    d = _dictionary()
    assert [u.name for u in d.root_units()] == ["新生界"]
    # children ordered by (order, name).
    assert [u.name for u in d.children_of("u1")] == ["沙四段", "沙三段"]
    # Full ancestor chain down to a sand layer.
    chain = d.path_to("z1")
    assert [u.level for u in chain] == [
        "界", "系", "统", "组", "段", "小层", "砂层组", "砂层",
    ]
    assert chain[0].name == "新生界" and chain[-1].name == "砂层 1"
    assert d.path_to("no-such-id") == []


def test_validation_detects_structure_problems() -> None:
    d = _dictionary()
    assert d.validate() == []

    dup = StratigraphicDictionary(units=[d.units[0], d.units[0]])
    assert any("重复 id" in p for p in dup.validate())

    orphan = StratigraphicDictionary(
        units=[StratigraphicUnit(id="x1", name="孤", parent_id="missing")]
    )
    assert any("父节点 missing 不存在" in p for p in orphan.validate())

    # Cycle: a → b → a.
    cyclic = StratigraphicDictionary(
        units=[
            StratigraphicUnit(id="a", name="A", parent_id="b"),
            StratigraphicUnit(id="b", name="B", parent_id="a"),
        ]
    )
    assert any("环" in p or "层级链" in p for p in cyclic.validate())
    assert cyclic.path_to("a") == []


def test_persistence_round_trip(tmp_path: Path) -> None:
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws", name="STRAT")
    d = _dictionary()
    save_stratigraphy(ws, d)
    loaded, diags = load_stratigraphy(ws)
    assert diags == []
    assert len(loaded.units) == len(d.units)
    assert loaded.path_to("z1")[-1].level == "砂层"
    f2 = loaded.unit_by_id("f2")
    assert f2 is not None
    assert f2.code == "E2s3" and f2.color == "#3b6fb5" and f2.age == "始新世"
    assert f2.order == 1


def test_missing_file_returns_empty(tmp_path: Path) -> None:
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws2", name="EMPTY")
    d, diags = load_stratigraphy(ws)
    assert d.units == []
    assert diags == []


def test_corrupt_file_returns_diagnostics(tmp_path: Path) -> None:
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws3", name="CORRUPT")
    (ws.root / "stratigraphy.json").write_text("{not json", encoding="utf-8")
    d, diags = load_stratigraphy(ws)
    assert d.units == []
    assert diags and "损坏" in diags[0]


def test_formation_top_unit_id_round_trip(tmp_path: Path) -> None:
    from well_log_workstation.tops_model import (
        FormationTop,
        load_tops_for_well,
        save_tops_for_well,
    )
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws4", name="TOPS")
    well = add_well(ws, name="W1", path="wells/w1.las")
    save_tops_for_well(
        ws,
        well.id,
        [FormationTop(name="沙四上亚段", depth=2100.0, id="t1", unit_id="m1")],
    )
    tops, diags = load_tops_for_well(ws, well.id)
    assert diags == []
    assert tops[0].unit_id == "m1"
    # Free-text tops (no unit_id) round-trip unchanged.
    save_tops_for_well(
        ws, well.id, [FormationTop(name="T1", depth=900.0, id="t2")]
    )
    tops2, _ = load_tops_for_well(ws, well.id)
    assert tops2[0].unit_id == ""


def test_old_tops_json_without_unit_id_stays_readable(tmp_path: Path) -> None:
    import json

    from well_log_workstation.tops_model import load_tops_for_well
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws5", name="OLD")
    well = add_well(ws, name="W1", path="wells/w1.las")
    dir_path = ws.root / "wells" / well.id
    dir_path.mkdir(parents=True, exist_ok=True)
    (dir_path / "tops.json").write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "well_id": well.id,
                "tops": [
                    {"id": "t1", "name": "老层位", "depth": 1200.0,
                     "unit": "m", "color": "#c0392b"},
                ],
            }
        ),
        encoding="utf-8",
    )
    tops, diags = load_tops_for_well(ws, well.id)
    assert diags == []
    assert tops[0].name == "老层位"
    assert tops[0].unit_id == ""
