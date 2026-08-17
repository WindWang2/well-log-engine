"""Publication frame border (图框边线) for Qt export (FRS §5.x 责任表/图框).

Covers the ``_draw_qt_frame_border`` paint helper, the dialog checkbox +
4-tuple ``value()``, and the SVG export integration (border_frame honoured
on the Qt paint path; default off keeps output unchanged).
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest
from PySide6.QtCore import QRectF
from PySide6.QtGui import QImage, QPainter

from well_log_workstation.export_dispatch import _draw_qt_frame_border


# -- _draw_qt_frame_border paint helper -----------------------------


def test_draw_qt_frame_border_paints_without_crash() -> None:
    img = QImage(400, 300, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    p = QPainter(img)
    _draw_qt_frame_border(p, QRectF(0, 0, 400, 300), margin_mm=10.0, mm_per_unit=4.0)
    p.end()
    assert img.width() == 400
    # The frame draws a black border inside the margins; confirm some non-white
    # pixels now exist near the margin edge (border was drawn).
    # Frame is inset by 10mm * 4 px/mm = 40px; sample the top edge, not (45, 5)
    # which sits in the unpainted margin (where red()<256 is a tautology).
    border_pixel = img.pixelColor(45, 40)
    assert border_pixel.red() < 240
    assert border_pixel.green() < 240


def test_draw_qt_frame_border_default_off_keeps_blank() -> None:
    # Sanity: a blank image without the frame stays all-white.
    img = QImage(200, 200, QImage.Format.Format_ARGB32)
    img.fill(0xFFFFFFFF)
    assert img.pixelColor(45, 5).red() == 255


# -- PdfExportOptionsDialog border_frame checkbox + 4-tuple ---------


def test_dialog_border_frame_default_off(qtbot) -> None:
    from well_log_workstation.export_options_dialog import PdfExportOptionsDialog

    dlg = PdfExportOptionsDialog()
    qtbot.addWidget(dlg)
    assert dlg.chk_border_frame.isChecked() is False
    # 4-tuple; border_frame is the 4th element.
    val = dlg.value()
    assert len(val) == 4
    assert val[3] is False


def test_dialog_border_frame_toggle_reflected_in_value(qtbot) -> None:
    from well_log_workstation.export_options_dialog import PdfExportOptionsDialog

    dlg = PdfExportOptionsDialog()
    qtbot.addWidget(dlg)
    dlg.chk_border_frame.setChecked(True)
    assert dlg.value()[3] is True


def test_dialog_border_frame_available_for_correlation(qtbot) -> None:
    """border_frame is honoured on the Qt paint path for all plot types
    (not gated on engine_options, unlike text mode / layered PDF)."""
    from well_log_workstation.export_options_dialog import PdfExportOptionsDialog

    dlg = PdfExportOptionsDialog(plot_type="correlation")
    qtbot.addWidget(dlg)
    assert dlg.chk_border_frame.isEnabled()
    dlg.chk_border_frame.setChecked(True)
    dlg.chk_crop_marks.setChecked(True)
    # correlation: engine_options off → text_mode outline, layered False,
    # but crop_marks + border_frame honoured.
    assert dlg.value() == ("outline", True, False, True)


def test_dialog_border_frame_init_param(qtbot) -> None:
    from well_log_workstation.export_options_dialog import PdfExportOptionsDialog

    dlg = PdfExportOptionsDialog(border_frame=True)
    qtbot.addWidget(dlg)
    assert dlg.chk_border_frame.isChecked() is True
    assert dlg.value()[3] is True


# -- SVG export integration (border_frame honoured, default off) -----


def test_svg_export_border_frame_off_vs_on(tmp_path: Path) -> None:
    """SVG with border_frame=False must not contain the frame rect; with True
    it must. This guards both the wiring and the default-off contract."""
    from well_log_workstation.plot_document import PlotDocument
    from well_log_workstation.export_dispatch import PageSpec, export_plot

    def _blank_paint(painter, rect) -> None:
        # Minimal paint_fn so the SVG is otherwise empty.
        return

    plot = PlotDocument(
        id="p1",
        name="frame",
        type="section",
        well_ids=["a", "b"],
        path="plots/p1.json",
        template_id="t",
    )
    spec = PageSpec()
    off = export_plot(
        plot, "svg", path=str(tmp_path / "off.svg"), page_spec=spec,
        backend="qt", paint_fn=_blank_paint, border_frame=False,
    )
    on = export_plot(
        plot, "svg", path=str(tmp_path / "on.svg"), page_spec=spec,
        backend="qt", paint_fn=_blank_paint, border_frame=True,
    )
    off_text = off.read_text(encoding="utf-8")
    on_text = on.read_text(encoding="utf-8")
    # Both valid SVGs.
    assert "<svg" in off_text and "<svg" in on_text
    # The frame adds a <rect> element with the border; the off variant has no
    # extra rect beyond whatever the blank paint produced (none here).
    assert on_text.count("<rect") > off_text.count("<rect")
