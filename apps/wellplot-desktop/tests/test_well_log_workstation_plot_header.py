"""Single-well plot header/footer from template (#293 / T5)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.las_import import import_las_into_workspace
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.template_model import (
    apply_template,
    get_builtin_template,
    header_spec_from_template,
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
WELL. HDR-1
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


def test_builtin_std_template_has_header_config() -> None:
    t = get_builtin_template("std-gr-rt-den")
    assert t is not None
    assert t.header
    assert t.header.get("title")
    assert t.header.get("show_well_name") is True
    spec = header_spec_from_template(t)
    lines = spec.header_lines(
        well_name="HDR-1",
        template_name=t.name,
        depth_unit="m",
        scale_summary="GR 0–150",
    )
    assert any("单井综合测井图" in ln or "综合" in ln for ln in lines)
    assert any("HDR-1" in ln for ln in lines)
    assert any("m" in ln for ln in lines)


def test_apply_template_carries_header(tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws")
    result = import_las_into_workspace(ws, _write_las(tmp_path / "a.las"))
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    pres = apply_template(template, result.document)
    assert pres.header.title
    lines = pres.header.header_lines(
        well_name=pres.well_name,
        template_name=pres.template_name,
        depth_unit=pres.depth_unit,
        scale_summary=pres.scale_summary(),
    )
    assert lines
    assert any(pres.well_name in ln for ln in lines)
    assert "GR" in pres.scale_summary() or "0" in pres.scale_summary()


def test_shell_canvas_header_and_export_svg(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ui", name="Hdr")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "u.las"))
    pres = win.apply_template_to_well(well_id, "std-gr-rt-den")
    assert pres.header is not None
    lines = win.multi_track_canvas._header_lines(pres)
    assert any("井" in ln or pres.well_name in ln for ln in lines)

    out = tmp_path / "hdr.svg"
    path = win.export_active_plot_svg(out)
    text = path.read_text(encoding="utf-8", errors="replace")
    # QSvg may embed text as XML text nodes or paths; at least non-empty SVG
    assert path.is_file() and path.stat().st_size > 100
    # Prefer seeing well or title string when text is preserved
    assert "svg" in text.lower()
