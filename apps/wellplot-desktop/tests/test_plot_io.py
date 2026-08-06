"""Plot definition XML/Excel + data_bindings (model A)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.display_set import leaf_id_for_curve
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.plot_io import (
    export_plot_excel,
    export_plot_xml,
    import_plot_excel,
    import_plot_xml,
)
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path) -> Path:
    path.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1002.0
STEP.M 1.0
NULL. -999.25
WELL. IO1
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
~ASCII
1000 20 2
1001 30 5
1002 40 10
""",
        encoding="utf-8",
    )
    return path


def test_xml_roundtrip_bindings(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "a.las"))
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    well_id = result.catalog_well_id
    plot = win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    gr = leaf_id_for_curve(result.document.document_id, "GR")
    win.set_display_set(well_id, {gr}, template_id="std-gr-rt-den", plot_id=plot.id)
    doc = load_plot_document(ws, plot.id)
    assert doc.data_bindings
    assert all(b.binding_id and b.plot_id == plot.id for b in doc.data_bindings)
    assert all(b.well_id == well_id for b in doc.data_bindings)

    xml_path = tmp_path / "plot.xml"
    export_plot_xml(doc, xml_path)
    # New plot from xml
    imported = import_plot_xml(ws, xml_path, plot_id="imported-plot-1")
    assert imported.id == "imported-plot-1"
    assert imported.well_ids == [well_id]
    assert gr in imported.display_set
    assert imported.data_bindings
    assert imported.data_bindings[0].leaf_id == gr
    assert imported.data_bindings[0].plot_id == "imported-plot-1"


def test_xlsx_roundtrip(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "a.las"))
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    well_id = result.catalog_well_id
    plot = win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    gr = leaf_id_for_curve(result.document.document_id, "GR")
    rt = leaf_id_for_curve(result.document.document_id, "RT")
    win.set_display_set(
        well_id, {gr, rt}, template_id="std-gr-rt-den", plot_id=plot.id
    )
    doc = load_plot_document(ws, plot.id)
    xlsx = tmp_path / "plot.xlsx"
    export_plot_excel(doc, xlsx)
    imported = import_plot_excel(ws, xlsx, plot_id="from-xlsx")
    assert set(imported.display_set) == {gr, rt}
    assert len(imported.data_bindings) == 2
    assert {b.leaf_id for b in imported.data_bindings} == {gr, rt}


def test_import_leaf_to_plot_adds_binding(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "a.las"))
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    well_id = result.catalog_well_id
    plot = win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    gr = leaf_id_for_curve(result.document.document_id, "GR")
    win.set_display_set(well_id, frozenset(), template_id="std-gr-rt-den", plot_id=plot.id)
    win.import_leaf_to_active_plot(gr)
    doc = load_plot_document(ws, plot.id)
    assert gr in doc.display_set
    assert any(b.leaf_id == gr and b.binding_id for b in doc.data_bindings)
