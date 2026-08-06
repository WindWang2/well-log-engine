"""Single-well track property panel + persistence (#292 / T4)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.template_model import (
    apply_template,
    apply_track_overrides,
    get_builtin_template,
    track_overrides_snapshot,
)
from well_log_workstation.workspace import create_workspace


def _write_las(path: Path) -> Path:
    path.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1005.0
STEP.M 1.0
NULL. -999.25
WELL. TP-1
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
RHOB.G/C3
~ASCII
1000 20 2 2.2
1001 30 5 2.3
1002 40 10 2.4
1003 50 20 2.5
1004 60 50 2.6
1005 70 100 2.7
""",
        encoding="utf-8",
    )
    return path


def test_apply_track_overrides_mutates_scale_and_visible(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "a.las"))
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    pres = apply_template(template, result.document)
    curve = next(t for t in pres.tracks if t.role == "curve")
    apply_track_overrides(
        pres,
        {
            curve.id: {
                "visible": False,
                "scale_min": 10.0,
                "scale_max": 200.0,
                "scale_mode": "log",
            }
        },
    )
    assert curve.visible is False
    assert curve.scale is not None
    assert curve.scale.min == 10.0
    assert curve.scale.max == 200.0
    assert curve.scale.mode == "log"
    assert curve.id not in {t.id for t in pres.visible_tracks}


def test_shell_edit_track_scale_updates_canvas(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ui", name="Props")
    las = _write_las(tmp_path / "u.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.apply_template_to_well(well_id, "std-gr-rt-den")

    assert win.track_list.count() >= 2
    # Select first curve track
    for i in range(win.track_list.count()):
        item = win.track_list.item(i)
        tid = item.data(0)  # Qt.UserRole may need ItemDataRole
        from PySide6.QtCore import Qt

        tid = item.data(Qt.ItemDataRole.UserRole)
        track = next(t for t in win.active_presentation.tracks if t.id == tid)
        if track.role == "curve":
            win.track_list.setCurrentRow(i)
            break

    assert win.track_scale_min.isEnabled()
    win.track_scale_max.setValue(250.0)
    track = win._find_bound_track(win._selected_track_id())
    assert track is not None and track.scale is not None
    assert track.scale.max == 250.0
    # Canvas holds same presentation object
    assert win.multi_track_canvas.presentation() is win.active_presentation


def test_shell_hide_track_and_persist_on_plot(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "persist", name="Persist")
    las = _write_las(tmp_path / "p.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    plot = win.create_single_well_plot_document(well_id, "std-gr-rt-den")

    from PySide6.QtCore import Qt

    curve_id = None
    for i in range(win.track_list.count()):
        item = win.track_list.item(i)
        tid = str(item.data(Qt.ItemDataRole.UserRole))
        track = next(t for t in win.active_presentation.tracks if t.id == tid)
        if track.role == "curve":
            curve_id = tid
            win.track_list.setCurrentRow(i)
            break
    assert curve_id is not None

    win.track_visible.setChecked(False)
    win.track_scale_min.setValue(5.0)
    win.track_scale_max.setValue(150.0)

    loaded = load_plot_document(ws, plot.id)
    assert curve_id in loaded.track_overrides
    ov = loaded.track_overrides[curve_id]
    assert ov.get("visible") is False
    assert float(ov.get("scale_min")) == 5.0
    assert float(ov.get("scale_max")) == 150.0

    # Re-open plot in a fresh window — overrides reapplied
    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(ws)
    win2.open_plot_document(plot.id)
    restored = next(t for t in win2.active_presentation.tracks if t.id == curve_id)
    assert restored.visible is False
    assert restored.scale is not None
    assert restored.scale.min == 5.0
    assert restored.scale.max == 150.0


def test_snapshot_roundtrip(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "snap")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "s.las"))
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    pres = apply_template(template, result.document)
    snap = track_overrides_snapshot(pres)
    assert any(k for k in snap)
    # Mutate then re-apply snapshot on a fresh apply
    pres2 = apply_template(template, result.document)
    for t in pres2.tracks:
        t.visible = False
    apply_track_overrides(pres2, snap)
    for a, b in zip(pres.tracks, pres2.tracks, strict=True):
        assert a.visible == b.visible
        if a.scale and b.scale:
            assert a.scale.min == b.scale.min
            assert a.scale.max == b.scale.max
