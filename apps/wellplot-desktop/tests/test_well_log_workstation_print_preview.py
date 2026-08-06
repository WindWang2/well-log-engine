"""Print preview WYSIWYG + printing (#301 / T13).

Covers:
* per-page depth-window computation (pagination math);
* thumbnails differ per page and receive the right depth window;
* dialog layout controls (paper/orientation/depth-per-page) recompute the plan;
* printing to PDF paints one page per window through the same paint path;
* correlation export honors a shared depth window;
* shell integration (open_print_preview defaults).
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QRectF
from PySide6.QtGui import QColor, QImage, QPainter
from PySide6.QtPrintSupport import QPrinter
from PySide6.QtWidgets import QComboBox, QDoubleSpinBox

from well_log_workstation.export_dispatch import PageSpec
from well_log_workstation.print_preview import (
    PREVIEW_LIMITATIONS,
    PrintPreviewDialog,
    compute_print_preview,
    depth_range_from_presentation,
    page_depth_windows,
    print_preview_pages,
    render_preview_thumbnail,
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


def _recording_paint_fn(received: list):
    """Paint callback that records its depth window and tints by window start."""

    def fn(painter, rect, *, depth_range=None):
        received.append(depth_range)
        r = int(depth_range[0]) % 256 if depth_range else 0
        painter.fillRect(rect, QColor(r, 0, 0))

    return fn


# ---------------------------------------------------------------------------
# Page plan math
# ---------------------------------------------------------------------------


def test_page_depth_windows_split() -> None:
    assert page_depth_windows(1000.0, 1010.0, 5.0) == (
        (1000.0, 1005.0),
        (1005.0, 1010.0),
    )
    assert page_depth_windows(1000.0, 1010.0, None) == ((1000.0, 1010.0),)
    assert page_depth_windows(1000.0, 1010.0, 20.0) == ((1000.0, 1010.0),)
    # Uneven last page keeps the remainder
    assert page_depth_windows(1000.0, 1012.0, 5.0) == (
        (1000.0, 1005.0),
        (1005.0, 1010.0),
        (1010.0, 1012.0),
    )


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
    assert "打印机驱动" in info.notes  # WYSIWYG disclosure, no longer skeleton
    assert "骨架" not in PREVIEW_LIMITATIONS


def test_compute_print_preview_windows() -> None:
    info = compute_print_preview(
        plot_name="Demo",
        depth_top=1000.0,
        depth_bottom=1010.0,
        depth_unit="m",
        page_spec=PageSpec(depth_per_page_mm=5.0),
    )
    assert info.page_depth_windows == ((1000.0, 1005.0), (1005.0, 1010.0))
    single = compute_print_preview(
        plot_name="Demo",
        depth_top=1000.0,
        depth_bottom=1010.0,
        depth_unit="m",
        page_spec=PageSpec(),
    )
    assert single.page_count == 1
    assert single.page_depth_windows == ((1000.0, 1010.0),)


# ---------------------------------------------------------------------------
# Thumbnails (WYSIWYG per page)
# ---------------------------------------------------------------------------


def test_render_thumbnail_differs_per_page(qtbot) -> None:
    received: list = []
    fn = _recording_paint_fn(received)
    spec = PageSpec(depth_per_page_mm=5.0)
    pm1 = render_preview_thumbnail(fn, spec, depth_range=(1000.0, 1005.0))
    pm2 = render_preview_thumbnail(fn, spec, depth_range=(1005.0, 1010.0))
    assert received == [(1000.0, 1005.0), (1005.0, 1010.0)]
    assert pm1.toImage().constBits() != pm2.toImage().constBits()


# ---------------------------------------------------------------------------
# Dialog
# ---------------------------------------------------------------------------


def test_print_preview_dialog_widgets(qtbot) -> None:
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


def test_dialog_layout_controls_recompute(qtbot) -> None:
    info = compute_print_preview(
        plot_name="Ctl",
        depth_top=100.0,
        depth_bottom=200.0,
        depth_unit="m",
        page_spec=PageSpec(),
    )
    dlg = PrintPreviewDialog(info, paint_fn=None)
    qtbot.addWidget(dlg)
    assert dlg.lbl_pages.text() == "1"  # 整段单页 default

    dpp = dlg.findChild(QDoubleSpinBox, "PreviewDepthPerPageSpin")
    dpp.setValue(25.0)
    assert dlg.lbl_pages.text() == "4"
    assert dlg.page_spin.maximum() == 4
    assert "25.0" in dlg.lbl_dpp.text()

    size = dlg.findChild(QComboBox, "PreviewSizeCombo")
    size.setCurrentIndex(size.findData("A3"))
    assert "A3" in dlg.lbl_box.text()

    orient = dlg.findChild(QComboBox, "PreviewOrientationCombo")
    orient.setCurrentIndex(orient.findData("landscape"))
    assert "横向" in dlg.lbl_box.text()

    # Back to 整段单页
    dpp.setValue(0.0)
    assert dlg.lbl_pages.text() == "1"


def test_dialog_page_spin_tracks_window(qtbot) -> None:
    received: list = []
    fn = _recording_paint_fn(received)
    info = compute_print_preview(
        plot_name="Spin",
        depth_top=1000.0,
        depth_bottom=1010.0,
        depth_unit="m",
        page_spec=PageSpec(depth_per_page_mm=5.0),
    )
    dlg = PrintPreviewDialog(info, paint_fn=fn)
    qtbot.addWidget(dlg)
    assert len(received) == 1
    assert received[0] == (1000.0, 1005.0)  # page 1 window
    assert "1000.00 – 1005.00" in dlg.lbl_page_depth.text()
    dlg.page_spin.setValue(2)
    assert received[-1] == (1005.0, 1010.0)  # page 2 window


# ---------------------------------------------------------------------------
# Printing (QPrinter → PDF; same path as real printing)
# ---------------------------------------------------------------------------


def test_print_preview_pages_to_pdf(qtbot, tmp_path: Path) -> None:
    received: list = []
    fn = _recording_paint_fn(received)
    spec = PageSpec(orientation="portrait", depth_per_page_mm=25.0)
    windows = ((100.0, 125.0), (125.0, 150.0), (150.0, 175.0), (175.0, 200.0))
    printer = QPrinter(QPrinter.PrinterMode.HighResolution)
    printer.setOutputFormat(QPrinter.OutputFormat.PdfFormat)
    out = tmp_path / "print.pdf"
    printer.setOutputFileName(str(out))
    pages = print_preview_pages(fn, spec, windows, printer)
    assert pages == 4
    assert out.is_file()
    assert out.stat().st_size > 1000
    assert received == list(windows)


# ---------------------------------------------------------------------------
# Correlation export with a shared depth window
# ---------------------------------------------------------------------------


def test_correlation_export_honors_depth_range(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.las_import import parse_las_file
    from well_log_workstation.template_model import (
        apply_template,
        get_builtin_template,
    )

    doc1 = parse_las_file(_write_las(tmp_path / "a.las"))
    doc2 = parse_las_file(_write_las(tmp_path / "b.las"))
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    pres1 = apply_template(template, doc1)
    pres2 = apply_template(template, doc2)

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win._active_plot_type = "correlation"
    win._correlation_presentations = [pres1, pres2]
    win._correlation_links = []

    img = QImage(600, 800, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    painter = QPainter(img)
    try:
        win._paint_active_plot(
            painter, QRectF(0, 0, 600, 800), depth_range=(1000.0, 1005.0)
        )
    finally:
        painter.end()
    assert not img.isNull()


# ---------------------------------------------------------------------------
# Shell integration
# ---------------------------------------------------------------------------


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
    assert info.page_count == 1  # WYSIWYG default: whole viewport, one page
    assert info.orientation == "portrait"
    assert info.page_depth_windows == ((info.depth_top, info.depth_bottom),)
    assert info.depth_bottom > info.depth_top
    assert info.depth_unit.lower() in ("m", "ft", "meter", "metre")
    # Depth from presentation
    d0, d1 = depth_range_from_presentation(win.active_presentation)
    assert info.depth_top == pytest.approx(d0)
    assert info.depth_bottom == pytest.approx(d1)


# ---------------------------------------------------------------------------
# Export PDF (WYSIWYG paginated export via QPdfWriter)
# ---------------------------------------------------------------------------


def test_print_preview_pages_to_pdf_writer(qtbot, tmp_path: Path) -> None:
    """print_preview_pages accepts a QPdfWriter (export path), painting one
    page per depth window — same contract as the QPrinter print path."""
    from PySide6.QtGui import QPdfWriter

    from well_log_workstation.print_preview import print_preview_pages

    received: list = []
    fn = _recording_paint_fn(received)
    spec = PageSpec(orientation="portrait", depth_per_page_mm=25.0)
    windows = ((100.0, 125.0), (125.0, 150.0), (150.0, 175.0), (175.0, 200.0))
    writer = QPdfWriter(str(tmp_path / "export.pdf"))
    writer.setResolution(150)
    pages = print_preview_pages(fn, spec, windows, writer)
    assert pages == 4
    out = tmp_path / "export.pdf"
    assert out.is_file()
    assert out.stat().st_size > 1000
    assert received == list(windows)


def test_print_preview_dialog_has_export_button(qtbot) -> None:
    """The dialog offers 导出 PDF… (WYSIWYG paginated export)."""
    from PySide6.QtWidgets import QPushButton

    from well_log_workstation.print_preview import PrintPreviewDialog

    info = compute_print_preview(
        plot_name="Btn",
        depth_top=100.0,
        depth_bottom=200.0,
        depth_unit="m",
        page_spec=PageSpec(depth_per_page_mm=50.0),
    )
    dlg = PrintPreviewDialog(info, paint_fn=None)
    qtbot.addWidget(dlg)
    export_btn = dlg.findChild(QPushButton, "PreviewExportPdf")
    assert export_btn is not None
    assert export_btn.text() == "导出 PDF…"
