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
from well_log_workstation.tst import (
    BeddingLayerSpec,
    SurfaceGridSpec,
    load_bedding_for_well,
)
from well_log_workstation.tst_dialog import (
    C_AZIMUTH,
    C_BOTTOM,
    C_DIP,
    C_SHAPE,
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


def _deep_vertical_trajectory():
    """400 m vertical well — deep enough to cross the surface unit
    [z=100, z=300] of the surface fixtures."""
    return compute_trajectory(
        [
            SurveyStation(md=0.0, inc_deg=0.0, az_deg=0.0),
            SurveyStation(md=400.0, inc_deg=0.0, az_deg=0.0),
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


# ---------------------------------------------------------------------------
# Surface bedding rows (Epic D high-order extension, bedding.json v2)
# ---------------------------------------------------------------------------


def _surface_spec(height: float) -> SurfaceGridSpec:
    return SurfaceGridSpec(
        x_origin_m=0.0, y_origin_m=0.0, x_step_m=100.0, y_step_m=100.0,
        x_nodes=3, y_nodes=3, z_tvd=(height,) * 9,
    )


def _surface_specs() -> list[BeddingLayerSpec]:
    """One surface-typed layer (horizontal grids 100/300) + one planar."""
    return [
        BeddingLayerSpec(
            top_md=0.0, bottom_md=200.0, dip_deg=0.0,
            top_surface=_surface_spec(100.0), bottom_surface=_surface_spec(300.0),
        ),
        BeddingLayerSpec(top_md=200.0, bottom_md=300.0, dip_deg=0.0),
    ]


def test_surface_row_shape_and_tst(qtbot) -> None:
    dlg = TstDialog("A", _surface_specs(), trajectory=_deep_vertical_trajectory())
    qtbot.addWidget(dlg)
    assert dlg.table.rowCount() == 2
    # 形态 column: 曲面 for the surface-typed row, 平面 for the planar one.
    assert _cell(dlg, 0, C_SHAPE) == "曲面"
    assert _cell(dlg, 1, C_SHAPE) == "平面"
    # Surface row: vertical well through horizontal grids 100/300 → TST 200.
    assert float(_cell(dlg, 0, C_TST)) == pytest.approx(200.0, abs=1e-9)
    # Planar row keeps the classic path model (0.5·40... here 100·1).
    assert float(_cell(dlg, 1, C_TST)) == pytest.approx(100.0, abs=1e-9)


def test_surface_row_value_roundtrip(qtbot) -> None:
    dlg = TstDialog("A", _surface_specs(), trajectory=_vertical_trajectory())
    qtbot.addWidget(dlg)
    values = dlg.value()
    assert len(values) == 2
    assert values[0].top_surface is not None
    assert values[0].bottom_surface is not None
    assert values[0].top_surface.z_tvd == (100.0,) * 9
    assert values[1].top_surface is None and values[1].bottom_surface is None


def test_single_surface_row_stays_unavailable(qtbot) -> None:
    """A layer with exactly one surface is inconsistent: 形态 shows 曲面 but
    TST stays 「—」 (explicit unavailability — never a silent planar
    fallback), and value() preserves the declared surface."""
    one_sided = [
        BeddingLayerSpec(
            top_md=0.0, bottom_md=200.0, dip_deg=0.0,
            top_surface=_surface_spec(100.0),  # no bottom_surface
        )
    ]
    dlg = TstDialog("A", one_sided, trajectory=_vertical_trajectory())
    qtbot.addWidget(dlg)
    assert _cell(dlg, 0, C_SHAPE) == "曲面"
    assert _cell(dlg, 0, C_TST) == NA
    values = dlg.value()
    assert values[0].top_surface is not None
    assert values[0].bottom_surface is None


def test_surface_row_tst_recomputes_on_depth_edit(qtbot) -> None:
    dlg = TstDialog("A", _surface_specs(), trajectory=_deep_vertical_trajectory())
    qtbot.addWidget(dlg)
    # Editing the declared interval keeps the surface TST (the extent comes
    # from the surfaces, not the declared MD metadata).
    dlg.table.item(0, C_BOTTOM).setText("150")
    assert float(_cell(dlg, 0, C_TST)) == pytest.approx(200.0, abs=1e-9)


def test_surface_row_without_survey_unavailable(qtbot) -> None:
    dlg = TstDialog("A", _surface_specs(), trajectory=None)
    qtbot.addWidget(dlg)
    assert _cell(dlg, 0, C_SHAPE) == "曲面"
    assert _cell(dlg, 0, C_TST) == NA  # no path → explicitly unavailable
