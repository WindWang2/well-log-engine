"""Model A: data on well; display_set on single-well plot (schema v6)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.display_set import leaf_id_for_curve
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.plot_document import (
    PLOT_SCHEMA_VERSION,
    load_plot_document,
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
STOP.M 1003.0
STEP.M 1.0
NULL. -999.25
WELL. MODEL-A
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
RHOB.G/C3
CAL.IN
~ASCII
1000 20 2 2.2 8
1001 30 5 2.3 8
1002 40 10 2.4 8
1003 50 20 2.5 8
""",
        encoding="utf-8",
    )
    return path


def test_create_plot_seeds_and_persists_display_set(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "a.las"))
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)

    plot = win.create_single_well_plot_document(
        result.catalog_well_id, "std-gr-rt-den"
    )
    assert PLOT_SCHEMA_VERSION >= 6
    reloaded = load_plot_document(ws, plot.id)
    assert reloaded.display_set, "plot should seed display_set from template matches"
    # leaf ids point at well document curves — not embedded samples
    assert all(":" in x for x in reloaded.display_set)

    gr = leaf_id_for_curve(result.document.document_id, "GR")
    cal = leaf_id_for_curve(result.document.document_id, "CAL")
    win.set_display_set(
        result.catalog_well_id, {gr, cal}, template_id="std-gr-rt-den", plot_id=plot.id
    )
    again = load_plot_document(ws, plot.id)
    assert set(again.display_set) == {gr, cal}

    # Re-open restores plot-scoped checks (not a second well-level default)
    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(ws)
    win2.session.put(result.document)
    win2.open_plot_document(plot.id)
    assert win2.display_set_for(result.catalog_well_id) == frozenset({gr, cal})
    assert win2.active_presentation is not None
    mnemos = {
        ly.mnemonic
        for t in win2.active_presentation.tracks
        if t.role == "curve"
        for ly in t.layers
    }
    assert mnemos == {"GR", "CAL"}


def test_two_plots_same_well_independent_display_sets(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "a.las"))
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    well_id = result.catalog_well_id
    doc = result.document

    p1 = win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    p2 = win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    gr = leaf_id_for_curve(doc.document_id, "GR")
    rt = leaf_id_for_curve(doc.document_id, "RT")

    win.set_display_set(well_id, {gr}, template_id="std-gr-rt-den", plot_id=p1.id)
    win.set_display_set(well_id, {rt}, template_id="std-gr-rt-den", plot_id=p2.id)

    assert set(load_plot_document(ws, p1.id).display_set) == {gr}
    assert set(load_plot_document(ws, p2.id).display_set) == {rt}
