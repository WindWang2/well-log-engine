"""T2: Single-well plot follows Display Set (#342)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.display_set import (
    default_checks,
    leaf_id_for_curve,
    leaves_from_document,
    presentation_from_display_set,
)
from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.template_model import get_builtin_template
from well_log_workstation.workspace import create_workspace


def _write_multi_las(path: Path) -> Path:
    path.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1005.0
STEP.M 1.0
NULL. -999.25
WELL. MULTI-T2
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
RHOB.G/C3
CAL.IN
AT10.OHMM
~ASCII
1000 20 2 2.2 8 1
1001 30 5 2.3 8 2
1002 40 10 2.4 8 3
1003 50 20 2.5 8 4
1004 60 50 2.6 8 5
1005 70 100 2.7 8 6
""",
        encoding="utf-8",
    )
    return path


def _import_multi(tmp_path: Path):
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_multi_las(tmp_path / "m.las"))
    return ws, result


def test_presentation_from_display_set_defaults_match_template(tmp_path: Path) -> None:
    _, result = _import_multi(tmp_path)
    doc = result.document
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    leaves = leaves_from_document(doc)
    checks = default_checks(leaves, template)
    # Template slots + preferred fill (≤10); DEN alias RHOB matches den slot
    assert leaf_id_for_curve(doc.document_id, "GR") in checks
    assert leaf_id_for_curve(doc.document_id, "RT") in checks
    assert leaf_id_for_curve(doc.document_id, "RHOB") in checks
    assert leaf_id_for_curve(doc.document_id, "CAL") in checks
    assert len(checks) <= 10
    assert len(checks) == len(leaves)  # 5 curves, under cap

    pres = presentation_from_display_set(template, doc, checks)
    assert pres.curve_track_count == len(checks)
    titles = [t.title for t in pres.tracks if t.role == "curve"]
    # Template-styled first (GR/RT/DEN), then extras in compose order
    assert titles[:3] == ["GR", "RT", "DEN"]


def test_presentation_empty_display_set_depth_only(tmp_path: Path) -> None:
    _, result = _import_multi(tmp_path)
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    pres = presentation_from_display_set(template, result.document, frozenset())
    assert pres.curve_track_count == 0
    assert any(t.role == "depth" for t in pres.tracks)


def test_shell_apply_defaults_and_live_display_set(qtbot, tmp_path: Path) -> None:
    ws, result = _import_multi(tmp_path)
    well_id = result.catalog_well_id
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win._workspace = ws
    win.session.put(result.document)
    # Open main shell stack past welcome
    win._main_stack.setCurrentIndex(1)

    pres = win.apply_template_to_well(well_id, "std-gr-rt-den")
    # Default batch: all available curves if ≤10 (MULTI has GR/RT/RHOB/CAL/AT10)
    assert pres.curve_track_count == 5
    ds = win.display_set_for(well_id)
    assert ds is not None
    assert len(ds) == 5
    assert len(ds) <= 10

    # Live: only GR
    gr_id = leaf_id_for_curve(result.document.document_id, "GR")
    pres2 = win.set_display_set(well_id, {gr_id}, template_id="std-gr-rt-den")
    assert pres2.curve_track_count == 1
    assert [t.title for t in pres2.tracks if t.role == "curve"] == ["GR"]
    assert win.display_set_for(well_id) == frozenset({gr_id})


def test_user_display_set_over_ten_not_capped(qtbot, tmp_path: Path) -> None:
    """Persisted / explicit display_set is never truncated to the default max."""
    from well_log_workstation.plot_document import create_single_well_plot

    # Build a LAS with many curves
    body_curves = "\n".join(f"C{i}.U" for i in range(15))
    body_ascii = "\n".join(
        " ".join([str(1000 + r)] + [str(i + r) for i in range(15)]) for r in range(3)
    )
    las = tmp_path / "many.las"
    las.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1002.0
STEP.M 1.0
NULL. -999.25
WELL. MANY
~CURVE INFORMATION
DEPT.M
{body_curves}
~ASCII
{body_ascii}
""",
        encoding="utf-8",
    )
    ws = create_workspace(tmp_path / "ws-many")
    result = import_las_into_workspace(ws, las)
    well_id = result.catalog_well_id
    leaves = leaves_from_document(result.document)
    assert len(leaves) >= 12
    plot = create_single_well_plot(
        ws,
        well_id=well_id,
        well_name="MANY",
        template_id="std-gr-rt-den",
        name="多井道图",
    )
    all_ids = {leaf.id for leaf in leaves}
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.session.put(result.document)
    # Explicit user set of >10 tracks
    win.set_display_set(well_id, all_ids, template_id="std-gr-rt-den", plot_id=plot.id)
    assert len(win.display_set_for(well_id) or ()) > 10
    # Re-open should keep user set (not re-default to ≤10)
    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(ws)
    win2.session.put(result.document)
    win2.open_plot_document(plot.id)
    ds2 = win2.display_set_for(well_id) or frozenset()
    assert len(ds2) > 10
    assert ds2 == all_ids


def test_shell_empty_display_set_guidance_caption(qtbot, tmp_path: Path) -> None:
    ws, result = _import_multi(tmp_path)
    well_id = result.catalog_well_id
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win._workspace = ws
    win.session.put(result.document)
    win._main_stack.setCurrentIndex(1)

    win.apply_template_to_well(well_id, "std-gr-rt-den")
    pres = win.set_display_set(well_id, frozenset(), template_id="std-gr-rt-den")
    assert pres.curve_track_count == 0
    cap = win.plot_caption.text()
    assert "显示集为空" in cap
    assert win.multi_track_canvas.presentation() is not None


def test_template_switch_keeps_display_set(qtbot, tmp_path: Path) -> None:
    ws, result = _import_multi(tmp_path)
    well_id = result.catalog_well_id
    doc = result.document
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win._workspace = ws
    win.session.put(doc)
    win._main_stack.setCurrentIndex(1)

    gr = leaf_id_for_curve(doc.document_id, "GR")
    cal = leaf_id_for_curve(doc.document_id, "CAL")
    win.set_display_set(well_id, {gr, cal}, template_id="std-gr-rt-den")
    before = win.display_set_for(well_id)
    assert before == frozenset({gr, cal})

    # Switch template without resetting checks
    pres = win.apply_template_to_well(well_id, "gr-only")
    assert win.display_set_for(well_id) == before
    # GR template-styled, CAL still present with default style
    curve_mnemos = [
        ly.mnemonic
        for t in pres.tracks
        if t.role == "curve"
        for ly in t.layers
    ]
    assert set(curve_mnemos) == {"GR", "CAL"}
    assert pres.curve_track_count == 2
