"""TST computation dialog (Epic D slice 4 — UI).

Covers:
* row population + live computed columns (表观厚度 / TVD厚度 / TST) against a
  vertical-well trajectory with hand-computed values;
* live recompute on edit (dip change, bottom change);
* missing survey → TVD/TST show 「—」(explicit unavailability);
* validation (non-numeric, inverted interval, dip out of range);
* value() round-trip (unit_id preserved, sorted, empty rows dropped);
* shell handler persists the bedding sidecar through the real save path.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QDialog, QMessageBox

from well_log_workstation.stratigraphy import (
    StratigraphicDictionary,
    StratigraphicUnit,
)
from well_log_workstation.survey import SurveyStation, compute_trajectory
from well_log_workstation.tst import BeddingLayerSpec, load_bedding_for_well
from well_log_workstation.tst_dialog import (
    C_AZIMUTH,
    C_BOTTOM,
    C_DIP,
    C_TOP,
    C_TST,
    C_TVD,
    NA,
    TstDialog,
)

KS2 = pytest.approx(0.7071067811865476)


def _dictionary() -> StratigraphicDictionary:
    return StratigraphicDictionary(
        units=[StratigraphicUnit(id="f1", name="沙四段", level="组")]
    )


def _vertical_trajectory():
    return compute_trajectory(
        [
            SurveyStation(md=0.0, inc_deg=0.0, az_deg=0.0),
            SurveyStation(md=100.0, inc_deg=0.0, az_deg=0.0),
        ]
    )


def _specs() -> list[BeddingLayerSpec]:
    return [
        BeddingLayerSpec(top_md=0.0, bottom_md=40.0, dip_deg=0.0, unit_id="f1"),
        BeddingLayerSpec(
            top_md=40.0, bottom_md=100.0, dip_deg=45.0, dip_azimuth_deg=0.0
        ),
    ]


def _cell(dlg: TstDialog, row: int, col: int) -> str:
    return dlg.table.item(row, col).text().strip()


def test_rows_populated_with_computed_values(qtbot) -> None:
    dlg = TstDialog(
        "A", _specs(), trajectory=_vertical_trajectory(), dictionary=_dictionary()
    )
    qtbot.addWidget(dlg)
    assert dlg.table.rowCount() == 2
    # Layer 1: 0-40 m horizontal → apparent 40, TVD 40, TST 40.
    assert _cell(dlg, 0, C_TOP) == "0"
    assert _cell(dlg, 0, 5) == "40"          # 表观厚度
    assert _cell(dlg, 0, C_TVD) == "40"      # TVD 厚度
    assert _cell(dlg, 0, C_TST) == "40"      # TST
    # Layer 2: 40-100 m dipping 45° → apparent 60, TVD 60, TST 60·cos45.
    assert _cell(dlg, 1, 5) == "60"
    assert _cell(dlg, 1, C_TVD) == "60"
    assert float(_cell(dlg, 1, C_TST)) == pytest.approx(60.0 * 0.7071067811865476)
    # Total reflects both layers.
    assert "合计 TST" in dlg.total_label.text()
    total = float(dlg.total_label.text().split("：")[1].split(" ")[0])
    assert total == pytest.approx(40.0 + 60.0 * 0.7071067811865476)


def test_live_recompute_on_edit(qtbot) -> None:
    dlg = TstDialog("A", _specs(), trajectory=_vertical_trajectory())
    qtbot.addWidget(dlg)
    # Flatten the second layer: dip 45 → 0 ⇒ TST becomes 60.
    dlg.table.item(1, C_DIP).setText("0")
    assert _cell(dlg, 1, C_TST) == "60"
    # Narrow the layer: bottom 100 → 80 ⇒ apparent/TVD/TST 40.
    dlg.table.item(1, C_BOTTOM).setText("80")
    assert _cell(dlg, 1, 5) == "40"
    assert _cell(dlg, 1, C_TST) == "40"
    # Invalid interval → computed cells turn 「—」.
    dlg.table.item(1, C_BOTTOM).setText("30")
    assert _cell(dlg, 1, C_TST) == NA
    dlg.table.item(1, C_BOTTOM).setText("80")


def test_missing_survey_shows_unavailable(qtbot) -> None:
    dlg = TstDialog("A", _specs(), trajectory=None)
    qtbot.addWidget(dlg)
    assert _cell(dlg, 0, 5) == "40"      # apparent always available
    assert _cell(dlg, 0, C_TVD) == NA    # TVD/TST explicitly unavailable
    assert _cell(dlg, 0, C_TST) == NA
    assert "暂无测斜轨迹" in "".join(
        lbl.text() for lbl in dlg.findChildren(type(dlg.total_label))
    ) or "需测斜" in dlg.total_label.text()


def test_validation_on_accept(qtbot, monkeypatch: pytest.MonkeyPatch) -> None:
    warnings: list[tuple] = []
    monkeypatch.setattr(
        QMessageBox, "warning", lambda *a, **k: warnings.append(a)
    )
    dlg = TstDialog("A", _specs(), trajectory=_vertical_trajectory())
    qtbot.addWidget(dlg)
    # Inverted interval.
    dlg.table.item(1, C_TOP).setText("120")
    dlg._on_accept()
    assert warnings and "底深必须大于顶深" in str(warnings[-1][2])
    assert dlg.result() != QDialog.DialogCode.Accepted
    # Non-numeric depth.
    dlg.table.item(1, C_TOP).setText("abc")
    dlg._on_accept()
    assert warnings and "必须是数字" in str(warnings[-1][2])
    # Dip out of range.
    dlg.table.item(1, C_TOP).setText("40")
    dlg.table.item(1, C_DIP).setText("95")
    dlg._on_accept()
    assert warnings and "0–90" in str(warnings[-1][2])


def test_value_roundtrip_and_sorting(qtbot) -> None:
    dlg = TstDialog("A", _specs(), trajectory=_vertical_trajectory())
    qtbot.addWidget(dlg)
    # Add a shallow third layer out of order; empty rows are dropped.
    dlg._add_empty_row()
    r = dlg.table.rowCount() - 1
    dlg.table.item(r, C_TOP).setText("100")
    dlg.table.item(r, C_BOTTOM).setText("120")
    dlg.table.item(r, C_DIP).setText("10")
    values = dlg.value()
    assert [s.top_md for s in values] == [0.0, 40.0, 100.0]  # sorted
    assert values[0].unit_id == "f1"
    assert values[0].dip_deg == 0.0
    assert values[1].dip_deg == 45.0
    assert values[1].dip_azimuth_deg == 0.0


def test_shell_tst_handler_persists_bedding(
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

    from well_log_workstation.tst_dialog import TstDialog

    class FakeDialog:
        def __init__(self, well_name, specs, *, trajectory=None,
                     dictionary=None, parent=None) -> None:
            self._specs = specs or []

        def exec(self) -> int:
            return QDialog.DialogCode.Accepted

        def value(self) -> list[BeddingLayerSpec]:
            return [
                BeddingLayerSpec(
                    top_md=1002.0, bottom_md=1005.0, dip_deg=20.0,
                    dip_azimuth_deg=0.0,
                )
            ]

    monkeypatch.setattr(
        "well_log_workstation.tst_dialog.TstDialog", FakeDialog
    )
    monkeypatch.setattr(win, "_pick_wells_for_correlation", lambda: [wid])
    assert win._act_tst.isEnabled()
    win._on_tst_calc()
    specs, diags = load_bedding_for_well(ws, wid)
    assert not diags
    assert len(specs) == 1
    assert specs[0].top_md == 1002.0
    assert specs[0].dip_deg == 20.0
