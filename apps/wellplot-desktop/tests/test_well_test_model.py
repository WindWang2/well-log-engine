"""Well-test domain model (Epic C / FRS §3.x 试油) — typed payload design.

Covers:
* structured common fields + opaque typed payload (extensible schema — no
  giant fixed nullable struct);
* explicit depth domain/unit on every interval (C5), stratigraphy linkage
  via unit id (C1);
* validation (reversed intervals, un-declared domains, duplicate ids);
* persistence round-trip + missing/corrupt tolerance + typed payload
  survives the round trip.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.well_test_model import (
    TEST_TYPES,
    WellTestInterval,
    WellTestModel,
    load_well_test_for_well,
    save_well_test_for_well,
)


def _model() -> WellTestModel:
    return WellTestModel(
        well_id="w1",
        intervals=[
            WellTestInterval(
                id="t1",
                top=2100.0,
                bottom=2108.0,
                formation_unit_id="f1",
                test_type="DST",
                date="2024-05-12",
                fluid="oil",
                result="工业油流",
                pressure_mpa=24.5,
                flow_rate_m3d=38.2,
                interpretation="沙四段油层",
                # Type-specific data: DST payload (extensible schema).
                payload={
                    "choke_mm": 6.0,
                    "bht_c": 92.0,
                    "static_pressure_mpa": 26.1,
                    "buildup_hours": 12.0,
                },
                attachment_refs=["report-2024-05.pdf"],
            ),
        ],
    )


def test_test_types_extensible() -> None:
    assert "DST" in TEST_TYPES
    # Any string is allowed — the tuple is documentation, not a closed enum.
    WellTestInterval(
        id="x", top=1.0, bottom=2.0, test_type="气举测试"
    )


def test_structure_and_payload() -> None:
    m = _model()
    itv = m.intervals[0]
    assert itv.display_label.startswith("DST · 2100–2108 m")
    assert itv.formation_unit_id == "f1"
    assert itv.depth_domain == "MD" and itv.unit == "m"
    assert itv.payload["choke_mm"] == 6.0
    assert itv.payload["bht_c"] == 92.0


def test_validation() -> None:
    assert _model().validate() == []
    bad = WellTestModel(
        well_id="w1",
        intervals=[
            WellTestInterval(id="a", top=10.0, bottom=5.0),
            WellTestInterval(id="a", top=5.0, bottom=10.0),
            WellTestInterval(id="b", top=5.0, bottom=10.0, depth_domain="?"),
        ],
    )
    problems = bad.validate()
    assert any("顶深大于底深" in p for p in problems)
    assert any("重复 interval" in p for p in problems)
    assert any("深度域" in p for p in problems)


def test_persistence_round_trip(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws", name="TEST")
    well = add_well(ws, name="W1", path="wells/w1.las")
    save_well_test_for_well(ws, well.id, _model())
    loaded, diags = load_well_test_for_well(ws, well.id)
    assert diags == []
    assert len(loaded.intervals) == 1
    itv = loaded.intervals[0]
    assert itv.pressure_mpa == pytest.approx(24.5)
    assert itv.flow_rate_m3d == pytest.approx(38.2)
    # The typed payload survives the round trip verbatim.
    assert itv.payload == {
        "choke_mm": 6.0,
        "bht_c": 92.0,
        "static_pressure_mpa": 26.1,
        "buildup_hours": 12.0,
    }
    assert itv.attachment_refs == ["report-2024-05.pdf"]
    assert itv.date == "2024-05-12"


def test_missing_and_corrupt_files(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws2", name="TEST2")
    well = add_well(ws, name="W1", path="wells/w1.las")
    m, diags = load_well_test_for_well(ws, well.id)
    assert m.intervals == [] and diags == []
    from well_log_workstation.well_test_model import well_test_file_path

    corrupt = well_test_file_path(ws, well.id)
    corrupt.parent.mkdir(parents=True, exist_ok=True)
    corrupt.write_text("{bad", encoding="utf-8")
    m2, diags2 = load_well_test_for_well(ws, well.id)
    assert m2.intervals == [] and diags2 and "损坏" in diags2[0]


def test_intervals_sorted_by_top(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws3", name="TEST3")
    well = add_well(ws, name="W1", path="wells/w1.las")
    model = WellTestModel(
        well_id=well.id,
        intervals=[
            WellTestInterval(id="b", top=2200.0, bottom=2210.0),
            WellTestInterval(id="a", top=2100.0, bottom=2110.0),
        ],
    )
    save_well_test_for_well(ws, well.id, model)
    loaded, _ = load_well_test_for_well(ws, well.id)
    assert [i.id for i in loaded.intervals] == ["a", "b"]
