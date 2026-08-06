"""LAS import into workspace + host session (#218)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.las_import import (
    LasImportError,
    import_las_into_workspace,
    parse_las_file,
)
from well_log_workstation.session_store import HostSessionStore
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import create_workspace, open_workspace


def _write_minimal_las(path: Path, *, well: str = "DEMO-1") -> Path:
    # LAS 2.0 style minimal ASCII
    text = f"""~VERSION INFORMATION
VERS.                          2.0 : CWLS LOG ASCII STANDARD - VERSION 2.0
WRAP.                          NO  : ONE LINE PER DEPTH STEP
~WELL INFORMATION
STRT.M                      1000.0 : START DEPTH
STOP.M                      1004.0 : STOP DEPTH
STEP.M                         1.0 : STEP
NULL.                       -999.25 : NULL VALUE
WELL.                       {well} : WELL
~CURVE INFORMATION
DEPT.M                             : Depth
GR  .GAPI                          : Gamma Ray
RT  .OHMM                          : Resistivity
~ASCII
1000.0  40.0  10.0
1001.0  45.0  12.0
1002.0  -999.25  11.0
1003.0  50.0  9.0
1004.0  55.0  8.5
"""
    path.write_text(text, encoding="utf-8")
    return path


def _write_las_with_coords(path: Path, *, lat: str, long: str, well: str = "DEMO-2") -> Path:
    """Minimal LAS with LAT/LONG decimal-degree headers (Phase-2 T2 / #246)."""
    text = f"""~VERSION INFORMATION
VERS.                          2.0 : CWLS LOG ASCII STANDARD - VERSION 2.0
WRAP.                          NO  : ONE LINE PER DEPTH STEP
~WELL INFORMATION
STRT.M                      1000.0 : START DEPTH
STOP.M                      1004.0 : STOP DEPTH
STEP.M                         1.0 : STEP
NULL.                       -999.25 : NULL VALUE
WELL.                       {well} : WELL
LAT.                        {lat} : LATITUDE (DEG)
LONG.                       {long} : LONGITUDE (DEG)
~CURVE INFORMATION
DEPT.M                             : Depth
GR  .GAPI                          : Gamma Ray
~ASCII
1000.0  40.0
1001.0  45.0
1002.0  50.0
1003.0  55.0
1004.0  60.0
"""
    path.write_text(text, encoding="utf-8")
    return path


def test_parse_las_curves(tmp_path: Path) -> None:
    las = _write_minimal_las(tmp_path / "demo.las")
    doc = parse_las_file(las)
    assert doc.well_name == "DEMO-1"
    assert doc.depth.size == 5
    assert len(doc.curves) == 2
    gr = doc.curve_by_mnemonic("GR")
    assert gr is not None
    assert gr.values.size == 5
    assert doc.sample_value("GR", 0) == pytest.approx(40.0)
    assert doc.sample_value("GR", 2) is None  # null


def test_import_into_workspace_catalog_and_session(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    las = _write_minimal_las(tmp_path / "src.las", well="Well-A")
    result = import_las_into_workspace(ws, las)

    assert result.catalog_well_id in {w.id for w in ws.wells}
    entry = next(w for w in ws.wells if w.id == result.catalog_well_id)
    assert entry.name == "Well-A"
    assert entry.path.startswith("wells/")
    assert (ws.root / entry.path).is_file()

    # Round-trip catalog
    again = open_workspace(ws.root)
    assert any(w.name == "Well-A" for w in again.wells)

    store = HostSessionStore()
    store.put(result.document)
    assert store.get(result.catalog_well_id) is not None
    assert store.sample_value(result.catalog_well_id, "RT", 1) == pytest.approx(12.0)
    assert len(store.get(result.catalog_well_id).curves) >= 1  # type: ignore[union-attr]


def test_parse_missing_file(tmp_path: Path) -> None:
    with pytest.raises(LasImportError, match="不存在"):
        parse_las_file(tmp_path / "nope.las")


def test_parse_las_lat_long_headers_populate_coords(tmp_path: Path) -> None:
    """Phase-2 T2 (#246): LAT/LONG decimal-degree headers -> lng/lat/crs."""
    las = _write_las_with_coords(tmp_path / "coords.las", lat="30.25", long="116.5")
    doc = parse_las_file(las)
    assert doc.lng == pytest.approx(116.5)
    assert doc.lat == pytest.approx(30.25)
    assert doc.crs == "EPSG:4326"


def test_parse_las_without_coords_marks_none(tmp_path: Path) -> None:
    """Wells without coordinate headers stay in the catalog marked None."""
    las = _write_minimal_las(tmp_path / "nocoords.las", well="NoCoords")
    doc = parse_las_file(las)
    assert doc.lng is None
    assert doc.lat is None
    assert doc.crs == "EPSG:4326"


def test_import_las_coords_flow_into_catalog(tmp_path: Path) -> None:
    """Phase-2 T2 (#246): imported wellhead coords reach the catalog entry."""
    ws = create_workspace(tmp_path / "ws-coords")
    las = _write_las_with_coords(tmp_path / "src-coords.las", lat="30.25", long="116.5")
    result = import_las_into_workspace(ws, las)
    entry = next(w for w in ws.wells if w.id == result.catalog_well_id)
    assert entry.lng == pytest.approx(116.5)
    assert entry.lat == pytest.approx(30.25)
    assert entry.crs == "EPSG:4326"

    again = open_workspace(ws.root)
    persisted = next(w for w in again.wells if w.id == result.catalog_well_id)
    assert persisted.lng == pytest.approx(116.5)
    assert persisted.lat == pytest.approx(30.25)


def test_shell_import_las_updates_tree(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ui-ws", name="UI")
    las = _write_minimal_las(tmp_path / "ui.las", well="ShellWell")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    assert win._act_import_las.isEnabled()  # noqa: SLF001

    well_id = win.import_las_path(las)
    doc = win.session.get(well_id)
    assert doc is not None
    assert doc.well_name == "ShellWell"
    assert win.session.sample_value(well_id, "GR", 4) == pytest.approx(55.0)

    labels: list[str] = []

    def walk(item) -> None:
        labels.append(item.text(0))
        for i in range(item.childCount()):
            walk(item.child(i))

    for i in range(win.workspace_tree.topLevelItemCount()):
        walk(win.workspace_tree.topLevelItem(i))
    assert "ShellWell" in labels
