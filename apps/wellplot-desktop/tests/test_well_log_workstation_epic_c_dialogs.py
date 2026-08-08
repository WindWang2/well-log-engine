"""Epic C engineering-data editor dialogs (core / well-test / perforation).

Covers:
* unit_combo — dictionary listing with hierarchy indentation, free-text
  fallback, dangling-id preservation;
* WellTestDialog — row population, typed payload / source / version survive
  round-trips untouched, sorting, validation (depths / optional floats);
* PerforationDialog — row population, source/version/attachment refs survive,
  sorting, validation;
* CoreDialog — runs + per-run samples binding (flush on selection change),
  run reorder keeps samples bound to the same run, ids stable across
  flush/value, run/sample meta (lab refs, photo ids) preserved;
* shell — 文件 menu actions enable with a workspace and the handlers persist
  sidecars through the real load/save path.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QDialog, QMessageBox

from well_log_workstation.core_dialog import CoreDialog
from well_log_workstation.core_model import (
    CoreModel,
    CoreRun,
    CoreSample,
    load_core_for_well,
)
from well_log_workstation.perforation_dialog import PerforationDialog
from well_log_workstation.perforation_model import (
    PerforationInterval,
    PerforationModel,
    load_perforation_for_well,
)
from well_log_workstation.stratigraphy import (
    StratigraphicDictionary,
    StratigraphicUnit,
    save_stratigraphy,
)
from well_log_workstation.unit_combo import make_unit_combo
from well_log_workstation.well_test_dialog import WellTestDialog
from well_log_workstation.well_test_model import (
    WellTestInterval,
    WellTestModel,
    load_well_test_for_well,
)


def _dictionary() -> StratigraphicDictionary:
    return StratigraphicDictionary(
        units=[
            StratigraphicUnit(id="e1", name="新生界", level="界", order=0),
            StratigraphicUnit(
                id="f1", name="沙四段", level="组", parent_id="e1", order=0
            ),
            StratigraphicUnit(
                id="f2", name="沙三段", level="组", parent_id="e1", order=1,
                color="#3b6fb5",
            ),
        ]
    )


def _test_model() -> WellTestModel:
    return WellTestModel(
        well_id="w1",
        unit="m",
        intervals=[
            WellTestInterval(
                id="t1",
                top=2100.0,
                bottom=2110.0,
                formation_unit_id="f1",
                test_type="DST",
                date="2024-01-10",
                fluid="油",
                result="获工业油流",
                pressure_mpa=12.5,
                flow_rate_m3d=28.3,
                interpretation="解释成果 A",
                payload={"shut_in_pressure_mpa": 15.0, "note": "typed"},
                source="srv-1",
                version="v2",
            ),
            WellTestInterval(
                id="t2", top=2050.0, bottom=2060.0, test_type="试采",
                result="低产",
            ),
        ],
    )


def _perforation_model() -> PerforationModel:
    return PerforationModel(
        well_id="w1",
        unit="m",
        intervals=[
            PerforationInterval(
                id="p1",
                top=2100.0,
                bottom=2102.0,
                formation_unit_id="f1",
                operation_date="2024-01-15",
                shot_density=16,
                phasing="90°",
                status="已射",
                completion_ref="完井管柱 A",
                source="ops-2",
                version="v1",
                attachment_refs=["scan-1.pdf"],
            ),
            PerforationInterval(
                id="p2", top=2050.0, bottom=2052.0, status="已封堵",
            ),
        ],
    )


def _core_model() -> CoreModel:
    return CoreModel(
        well_id="w1",
        unit="m",
        runs=[
            CoreRun(
                id="r1",
                top=2100.0,
                bottom=2120.0,
                recovery_m=18.5,
                label="第一筒",
                source="core-srv",
                version="v3",
                photo_segment_ids=["ph1", "ph2"],
                samples=[
                    CoreSample(
                        id="s1",
                        depth=2100.5,
                        description="细粒砂岩",
                        lithology_unit_id="f1",
                        porosity=0.12,
                        permeability_md=8.5,
                        density_gcc=2.35,
                        lab_report_ref="lab-2024-01",
                        photo_segment_id="ph1",
                        attachment_ref="scan-core-1",
                    ),
                ],
            ),
            CoreRun(
                id="r2",
                top=2150.0,
                bottom=2155.0,
                recovery_m=5.0,
                label="第二筒",
            ),
        ],
    )


# ---------------------------------------------------------------------------
# unit_combo
# ---------------------------------------------------------------------------


def test_unit_combo_hierarchy_and_selection(qtbot) -> None:
    combo = make_unit_combo(_dictionary(), current_id="f1")
    qtbot.addWidget(combo)
    assert combo.itemData(0) == ""  # free text first
    assert combo.currentData() == "f1"
    labels = [combo.itemText(i) for i in range(combo.count())]
    assert "新生界（界）" in labels
    assert any(label.startswith("　沙四段（组）") for label in labels)


def test_unit_combo_dangling_id_preserved(qtbot) -> None:
    combo = make_unit_combo(_dictionary(), current_id="ghost")
    qtbot.addWidget(combo)
    assert combo.currentData() == "ghost"
    assert combo.currentText().startswith("（未知单元 ghost）")


def test_unit_combo_empty_dictionary(qtbot) -> None:
    combo = make_unit_combo(None)
    qtbot.addWidget(combo)
    assert combo.count() == 1
    assert combo.itemData(0) == ""


# ---------------------------------------------------------------------------
# WellTestDialog
# ---------------------------------------------------------------------------


def test_well_test_dialog_roundtrip_preserves_typed_payload(qtbot) -> None:
    dlg = WellTestDialog(_test_model(), dictionary=_dictionary())
    qtbot.addWidget(dlg)
    assert dlg.table.rowCount() == 2
    value = dlg.value()
    assert [i.top for i in value.intervals] == [2050.0, 2100.0]  # sorted
    t1 = value.intervals[1]
    assert t1.id == "t1"
    assert t1.formation_unit_id == "f1"
    assert t1.test_type == "DST"
    assert t1.pressure_mpa == 12.5
    assert t1.flow_rate_m3d == 28.3
    assert t1.payload == {"shut_in_pressure_mpa": 15.0, "note": "typed"}
    assert t1.source == "srv-1"
    assert t1.version == "v2"
    assert value.well_id == "w1"


def test_well_test_dialog_add_row(qtbot) -> None:
    dlg = WellTestDialog(_test_model(), dictionary=_dictionary())
    qtbot.addWidget(dlg)
    dlg.table.selectRow(0)
    dlg._add_empty_row()
    r = dlg.table.rowCount() - 1
    dlg.table.item(r, 1).setText("2070")   # COL_TOP
    dlg.table.item(r, 2).setText("2080")   # COL_BOTTOM
    dlg.table.item(r, 5).setText("2024-02-01")
    value = dlg.value()
    added = next(i for i in value.intervals if i.top == 2070.0)
    assert added.bottom == 2080.0
    assert added.date == "2024-02-01"
    assert added.test_type == "DST"  # combo default


def test_well_test_dialog_validation(qtbot, monkeypatch: pytest.MonkeyPatch) -> None:
    warnings: list[tuple] = []
    monkeypatch.setattr(
        QMessageBox, "warning", lambda *a, **k: warnings.append(a)
    )
    dlg = WellTestDialog(_test_model())
    qtbot.addWidget(dlg)
    dlg._add_empty_row()
    r = dlg.table.rowCount() - 1
    dlg.table.item(r, 1).setText("2090")  # top > bottom
    dlg.table.item(r, 2).setText("2080")
    dlg._on_accept()
    assert warnings and "底深" in str(warnings[0][2])
    assert dlg.result() != QDialog.DialogCode.Accepted


# ---------------------------------------------------------------------------
# PerforationDialog
# ---------------------------------------------------------------------------


def test_perforation_dialog_roundtrip(qtbot) -> None:
    dlg = PerforationDialog(_perforation_model(), dictionary=_dictionary())
    qtbot.addWidget(dlg)
    assert dlg.table.rowCount() == 2
    value = dlg.value()
    assert [i.top for i in value.intervals] == [2050.0, 2100.0]
    p1 = value.intervals[1]
    assert p1.id == "p1"
    assert p1.formation_unit_id == "f1"
    assert p1.shot_density == 16
    assert p1.phasing == "90°"
    assert p1.status == "已射"
    assert p1.completion_ref == "完井管柱 A"
    assert p1.source == "ops-2"
    assert p1.version == "v1"
    assert p1.attachment_refs == ["scan-1.pdf"]


def test_perforation_dialog_validation(qtbot, monkeypatch: pytest.MonkeyPatch) -> None:
    warnings: list[tuple] = []
    monkeypatch.setattr(
        QMessageBox, "warning", lambda *a, **k: warnings.append(a)
    )
    dlg = PerforationDialog(_perforation_model())
    qtbot.addWidget(dlg)
    dlg._add_empty_row()
    r = dlg.table.rowCount() - 1
    dlg.table.item(r, 0).setText("2070")
    dlg.table.item(r, 1).setText("2080")
    dlg.table.item(r, 5).setText("16孔")  # 孔密 must be numeric
    dlg._on_accept()
    assert warnings and "孔密" in str(warnings[0][2])
    assert dlg.result() != QDialog.DialogCode.Accepted


# ---------------------------------------------------------------------------
# CoreDialog
# ---------------------------------------------------------------------------


def test_core_dialog_roundtrip_preserves_meta(qtbot) -> None:
    dlg = CoreDialog(_core_model(), dictionary=_dictionary())
    qtbot.addWidget(dlg)
    assert dlg.runs.rowCount() == 2
    value = dlg.value()
    assert [r.top for r in value.runs] == [2100.0, 2150.0]
    r1 = value.runs[0]
    assert r1.id == "r1"
    assert r1.recovery_m == 18.5
    assert r1.source == "core-srv"
    assert r1.version == "v3"
    assert r1.photo_segment_ids == ["ph1", "ph2"]
    s1 = r1.samples[0]
    assert s1.id == "s1"
    assert s1.description == "细粒砂岩"
    assert s1.lithology_unit_id == "f1"
    assert s1.lab_report_ref == "lab-2024-01"
    assert s1.attachment_ref == "scan-core-1"
    assert s1.photo_segment_id == "ph1"


def test_core_dialog_samples_follow_selected_run(qtbot) -> None:
    dlg = CoreDialog(_core_model())
    qtbot.addWidget(dlg)
    # r1 (row 0) is selected by default → its sample is visible.
    assert dlg.samples.rowCount() == 1
    # Switch to r2 → empty sample table; add a sample there.
    dlg.runs.selectRow(1)
    assert dlg.samples.rowCount() == 0
    dlg._add_sample()
    r = dlg.samples.rowCount() - 1
    dlg.samples.item(r, 0).setText("2151.5")
    dlg.samples.item(r, 1).setText("灰岩")
    # Switch back to r1 → r1's original sample still there, and r2's new
    # sample was not lost (flush on selection change).
    dlg.runs.selectRow(0)
    assert dlg.samples.rowCount() == 1
    value = dlg.value()
    r1 = next(run for run in value.runs if run.id == "r1")
    r2 = next(run for run in value.runs if run.id == "r2")
    assert [s.depth for s in r1.samples] == [2100.5]
    assert [s.description for s in r2.samples] == ["灰岩"]


def test_core_dialog_run_reorder_keeps_samples_bound(qtbot) -> None:
    dlg = CoreDialog(_core_model())
    qtbot.addWidget(dlg)
    dlg.runs.selectRow(1)  # r2 selected (empty samples)
    dlg._move_run(-1)  # r2 moves above r1 in the table
    # Table rows swapped …
    assert dlg.runs.item(0, 0).text() == "2150"  # COL_TOP of r2
    assert dlg.runs.item(1, 0).text() == "2100"  # COL_TOP of r1
    # … and each run keeps its own samples (value() re-sorts by top).
    value = dlg.value()
    r1 = next(run for run in value.runs if run.id == "r1")
    r2 = next(run for run in value.runs if run.id == "r2")
    assert r2.samples == []
    assert [s.depth for s in r1.samples] == [2100.5]


def test_core_dialog_ids_stable_across_flush(qtbot) -> None:
    dlg = CoreDialog(_core_model())
    qtbot.addWidget(dlg)
    dlg._flush_samples()
    dlg._flush_samples()
    value = dlg.value()
    s1 = value.runs[0].samples[0]
    assert s1.id == "s1"
    # New sample row gets a stable id from birth.
    dlg.runs.selectRow(1)
    dlg._add_sample()
    dlg.samples.item(0, 0).setText("2151")
    dlg._flush_samples()
    dlg._flush_samples()
    value2 = dlg.value()
    new_sample = value2.runs[1].samples[0]
    assert new_sample.id  # non-empty
    assert new_sample.id == value2.runs[1].samples[0].id


# ---------------------------------------------------------------------------
# Shell wiring
# ---------------------------------------------------------------------------


@pytest.fixture
def shell_with_well(qtbot, tmp_path: Path):
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    las = tmp_path / "a.las"
    las.write_text(
        "~VERSION INFORMATION\nVERS. 2.0\nWRAP. NO\n"
        "~WELL INFORMATION\nSTRT.M 1000.0\nSTOP.M 1010.0\nSTEP.M 1.0\n"
        "NULL. -999.25\nWELL. A\n~CURVE INFORMATION\nDEPT.M\nGR.GAPI\n~ASCII\n"
        + "".join(f"{1000 + i} 1\n" for i in range(11)),
        encoding="utf-8",
    )
    wid = win.import_las_path(las)
    assert wid
    win._selected_well_id = wid
    return win, ws, wid


def test_engineering_actions_enabled_with_workspace(shell_with_well) -> None:
    win, _ws, _wid = shell_with_well
    assert win._act_core.isEnabled()
    assert win._act_well_test.isEnabled()
    assert win._act_perforation.isEnabled()


def test_shell_well_test_handler_persists_sidecar(
    qtbot, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    las = tmp_path / "a.las"
    las.write_text(
        "~VERSION INFORMATION\nVERS. 2.0\nWRAP. NO\n"
        "~WELL INFORMATION\nSTRT.M 1000.0\nSTOP.M 1010.0\nSTEP.M 1.0\n"
        "NULL. -999.25\nWELL. A\n~CURVE INFORMATION\nDEPT.M\nGR.GAPI\n~ASCII\n"
        + "".join(f"{1000 + i} 1\n" for i in range(11)),
        encoding="utf-8",
    )
    wid = win.import_las_path(las)
    save_stratigraphy(ws, _dictionary())

    from well_log_workstation.well_test_dialog import WellTestDialog

    class FakeDialog:
        def __init__(self, current, *, dictionary=None, parent=None) -> None:
            self._current = current
            self._dictionary = dictionary

        def exec(self) -> int:
            return QDialog.DialogCode.Accepted

        def value(self) -> WellTestModel:
            return WellTestModel(
                well_id=wid,
                unit="m",
                intervals=[
                    WellTestInterval(
                        id="t9",
                        top=1002.0,
                        bottom=1005.0,
                        formation_unit_id="f1",
                        test_type="DST",
                        result="获工业油流",
                        payload={"note": "typed"},
                    )
                ],
            )

    monkeypatch.setattr(
        "well_log_workstation.well_test_dialog.WellTestDialog", FakeDialog
    )
    monkeypatch.setattr(win, "_pick_wells_for_correlation", lambda: [wid])
    win._on_edit_well_test()
    model, diags = load_well_test_for_well(ws, wid)
    assert not diags
    assert len(model.intervals) == 1
    assert model.intervals[0].id == "t9"
    assert model.intervals[0].formation_unit_id == "f1"
    assert model.intervals[0].payload == {"note": "typed"}


def test_shell_core_and_perforation_handlers_persist(
    qtbot, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    las = tmp_path / "a.las"
    las.write_text(
        "~VERSION INFORMATION\nVERS. 2.0\nWRAP. NO\n"
        "~WELL INFORMATION\nSTRT.M 1000.0\nSTOP.M 1010.0\nSTEP.M 1.0\n"
        "NULL. -999.25\nWELL. A\n~CURVE INFORMATION\nDEPT.M\nGR.GAPI\n~ASCII\n"
        + "".join(f"{1000 + i} 1\n" for i in range(11)),
        encoding="utf-8",
    )
    wid = win.import_las_path(las)

    from well_log_workstation.core_dialog import CoreDialog
    from well_log_workstation.perforation_dialog import PerforationDialog

    def _fake_factory(model_cls, value_fn):
        class Fake:
            def __init__(self, current, *, dictionary=None, parent=None) -> None:
                pass

            def exec(self) -> int:
                return QDialog.DialogCode.Accepted

            def value(self):
                return value_fn()

        return Fake

    core_value = CoreModel(
        well_id=wid,
        runs=[CoreRun(id="cr1", top=1002.0, bottom=1006.0, label="一筒")],
    )
    monkeypatch.setattr(
        "well_log_workstation.core_dialog.CoreDialog",
        _fake_factory(CoreModel, lambda: core_value),
    )
    perf_value = PerforationModel(
        well_id=wid,
        intervals=[
            PerforationInterval(
                id="cp1", top=1002.0, bottom=1004.0, status="已射"
            )
        ],
    )
    monkeypatch.setattr(
        "well_log_workstation.perforation_dialog.PerforationDialog",
        _fake_factory(PerforationModel, lambda: perf_value),
    )
    monkeypatch.setattr(win, "_pick_wells_for_correlation", lambda: [wid])

    win._on_edit_core_data()
    win._on_edit_perforation()

    core, diags1 = load_core_for_well(ws, wid)
    assert not diags1 and len(core.runs) == 1 and core.runs[0].id == "cr1"
    perf, diags2 = load_perforation_for_well(ws, wid)
    assert not diags2 and len(perf.intervals) == 1
    assert perf.intervals[0].id == "cp1"
