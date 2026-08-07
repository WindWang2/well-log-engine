"""Well-header fields (KB/GL/MaxMD) + TVDSS plumbing (FRS §1.x).

Covers the new WellCatalogEntry fields + JSON roundtrip, LAS header
capture of KB/GL/STOP, the WellSectionDatum tvdss shift using kb_m
(the previously-dead-code path), and the well-header dialog.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest

from well_log_workstation.datum.well_section_datum import WellSectionDatum
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.workspace import (
    WellCatalogEntry,
    create_workspace,
    open_workspace,
    save_workspace,
)


# -- WellCatalogEntry fields + JSON roundtrip -----------------------


def test_well_catalog_entry_kb_gl_max_md_roundtrip(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="WH")
    entry = WellCatalogEntry(
        id="w1",
        name="A",
        path="wells/w1/a.las",
        lng=116.0,
        lat=39.0,
        crs="EPSG:4326",
        kb_m=50.0,
        gl_m=45.0,
        max_md=2000.0,
    )
    ws.wells.append(entry)
    save_workspace(ws)
    reloaded = open_workspace(ws.root)
    assert reloaded.wells[0].kb_m == 50.0
    assert reloaded.wells[0].gl_m == 45.0
    assert reloaded.wells[0].max_md == 2000.0


def test_well_catalog_entry_legacy_defaults_none(tmp_path: Path) -> None:
    """Old workspace files without the new fields load as None."""
    ws = create_workspace(tmp_path / "ws", name="WH")
    # Simulate a legacy entry by writing raw JSON without kb/gl/max_md.
    import json

    data = {
        "name": "WH",
        "schemaVersion": 2,
        "wells": [{"id": "w1", "name": "A", "path": "", "lng": None, "lat": None, "crs": "EPSG:4326"}],
        "plots": [],
    }
    (ws.root / "workspace.json").write_text(json.dumps(data), encoding="utf-8")
    reloaded = open_workspace(ws.root)
    assert reloaded.wells[0].kb_m is None
    assert reloaded.wells[0].gl_m is None
    assert reloaded.wells[0].max_md is None


# -- LAS header capture ---------------------------------------------


def _las_with_kb(path: Path, well: str, kb: float) -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1003.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
KB.M {kb}
GL.M {kb - 5.0}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 10
1001 20
1002 30
1003 40
""",
        encoding="utf-8",
    )
    return path


def test_las_import_captures_kb_gl_maxmd(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="WH")
    result = import_las_into_workspace(ws, _las_with_kb(tmp_path / "a.las", "A", 50.0))
    doc = result.document
    assert doc.kb_m == 50.0
    assert doc.gl_m == 45.0
    assert doc.max_md == 1003.0  # STOP header
    # Catalog entry also carries the fields.
    entry = next(w for w in ws.wells if w.id == result.catalog_well_id)
    assert entry.kb_m == 50.0
    assert entry.gl_m == 45.0
    assert entry.max_md == 1003.0


def test_las_import_without_kb_headers(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="WH")
    (tmp_path / "b.las").write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1003.0
STEP.M 1.0
NULL. -999.25
WELL. B
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 10
1001 20
1002 30
1003 40
""",
        encoding="utf-8",
    )
    result = import_las_into_workspace(ws, tmp_path / "b.las")
    assert result.document.kb_m is None
    assert result.document.gl_m is None
    # STOP is still captured even without KB/GL.
    assert result.document.max_md == 1003.0


# -- WellSectionDatum tvdss uses kb_m (the dead-code fix) -----------


def test_compute_shifts_tvdss_uses_kb() -> None:
    d = WellSectionDatum(mode="tvdss")
    shifts = d.compute_shifts([{"name": "A", "tops": [], "kb_m": 50.0}])
    assert shifts["A"] == -50.0


def test_compute_shifts_tvdss_zero_without_kb() -> None:
    d = WellSectionDatum(mode="tvdss")
    assert d.compute_shifts([{"name": "A", "tops": []}])["A"] == 0.0
    assert d.compute_shifts([{"name": "A", "tops": [], "kb_m": None}])["A"] == 0.0


def test_compute_shifts_tvdss_multiple_wells() -> None:
    d = WellSectionDatum(mode="tvdss")
    wells = [
        {"name": "A", "tops": [], "kb_m": 100.0},
        {"name": "B", "tops": [], "kb_m": 50.0},
        {"name": "C", "tops": []},  # no KB -> 0
    ]
    shifts = d.compute_shifts(wells)
    assert shifts["A"] == -100.0
    assert shifts["B"] == -50.0
    assert shifts["C"] == 0.0


# -- WellHeaderDialog -----------------------------------------------


def test_well_header_dialog_edits_kb(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.well_header_dialog import WellHeaderDialog

    entry = WellCatalogEntry(id="w1", name="A", kb_m=None)
    dlg = WellHeaderDialog(entry)
    qtbot.addWidget(dlg)
    dlg._kb.setValue(50.0)
    dlg._gl.setValue(45.0)
    dlg._max_md.setValue(2000.0)
    result = dlg.result_entry()
    assert result.kb_m == 50.0
    assert result.gl_m == 45.0
    assert result.max_md == 2000.0
    assert result.id == "w1"
    assert result.name == "A"


def test_well_header_dialog_zero_kb_becomes_none(qtbot) -> None:
    """A neutral 0 KB is stored as None (no shift), not 0.0."""
    from well_log_workstation.well_header_dialog import WellHeaderDialog

    entry = WellCatalogEntry(id="w1", name="A")
    dlg = WellHeaderDialog(entry)
    qtbot.addWidget(dlg)
    # KB stays at the default 0.
    result = dlg.result_entry()
    assert result.kb_m is None


def test_well_header_dialog_preserves_coordinates(qtbot) -> None:
    from well_log_workstation.well_header_dialog import WellHeaderDialog

    entry = WellCatalogEntry(
        id="w1", name="A", lng=116.5, lat=39.9, crs="EPSG:32650"
    )
    dlg = WellHeaderDialog(entry)
    qtbot.addWidget(dlg)
    result = dlg.result_entry()
    assert result.lng == 116.5
    assert result.lat == 39.9
    assert result.crs == "EPSG:32650"


# -- shell menu wiring ----------------------------------------------


def test_shell_well_header_menu_enabled_with_workspace(
    qtbot, tmp_path: Path
) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow

    ws = create_workspace(tmp_path / "ws", name="WH")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    assert win._act_well_header.isEnabled() is False
    win.set_workspace(ws)
    assert win._act_well_header.isEnabled() is True
