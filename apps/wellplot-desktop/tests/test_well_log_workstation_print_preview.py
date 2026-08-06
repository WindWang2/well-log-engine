"""Print preview skeleton (#301 / T13)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.export_dispatch import PageSpec
from well_log_workstation.print_preview import (
    PREVIEW_LIMITATIONS,
    compute_print_preview,
    depth_range_from_presentation,
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
STOP.M 1010.0
STEP.M 1.0
NULL. -999.25
WELL. PV-1
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
1006 75 80 2.6
1007 80 60 2.5
1008 85 40 2.4
1009 90 30 2.3
1010 95 20 2.2
""",
        encoding="utf-8",
    )
    return path


def test_compute_print_preview_pages() -> None:
    info = compute_print_preview(
        plot_name="Demo",
        depth_top=1000.0,
        depth_bottom=1010.0,
        depth_unit="m",
        page_spec=PageSpec(depth_per_page_mm=5.0),
    )
    assert info.page_count == 2
    assert info.depth_top == 1000.0
    assert info.depth_bottom == 1010.0
    assert info.page_width_mm > 0
    assert info.page_height_mm > 0
    assert "WYSIWYG" in info.notes or "骨架" in PREVIEW_LIMITATIONS


def test_compute_single_page_without_dpp() -> None:
    info = compute_print_preview(
        plot_name="X",
        depth_top=0.0,
        depth_bottom=100.0,
        depth_unit="m",
        page_spec=PageSpec(),
    )
    assert info.page_count == 1


def test_shell_print_preview_from_single_well(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="PV")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(_write_las(tmp_path / "p.las"))
    win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    assert win._act_print_preview.isEnabled()

    info = win.open_print_preview(show=False)
    assert info is not None
    assert info.page_count >= 1
    assert info.depth_bottom > info.depth_top
    assert info.depth_unit.lower() in ("m", "ft", "meter", "metre")
    # Depth from presentation
    d0, d1 = depth_range_from_presentation(win.active_presentation)
    assert info.depth_top == pytest.approx(d0)
    assert info.depth_bottom == pytest.approx(d1)


def test_print_preview_dialog_widgets(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.print_preview import PrintPreviewDialog

    info = compute_print_preview(
        plot_name="Dlg",
        depth_top=100.0,
        depth_bottom=200.0,
        depth_unit="m",
        page_spec=PageSpec(depth_per_page_mm=50.0),
    )
    dlg = PrintPreviewDialog(info, paint_fn=None, parent=None)
    qtbot.addWidget(dlg)
    assert dlg.lbl_pages.text() == "2"
    assert "100.00" in dlg.lbl_depth.text()
    assert "200.00" in dlg.lbl_depth.text()
    assert dlg.page_spin.maximum() == 2
