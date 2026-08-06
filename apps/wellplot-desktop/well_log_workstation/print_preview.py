"""Print preview + printing for single-well / correlation plots (#301 / T13).

WYSIWYG: thumbnails render each page's own depth window through the same host
paint path used for export; the print action sends those exact pages to the
printer (or a PDF file). Layout controls (page size / orientation / depth per
page) recompute the page plan live.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Any, Callable, Sequence

import numpy as np
from PySide6.QtCore import QMarginsF, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QPageLayout,
    QPageSize,
    QPainter,
    QPixmap,
)
from PySide6.QtPrintSupport import QPrintDialog, QPrinter
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from well_log_workstation.export_dispatch import PageSpec
from well_log_workstation.template_model import HostPresentation

# Documented limits (shown in UI)
PREVIEW_LIMITATIONS = (
    "预览与打印共用 Qt 绘制路径;实际打印效果可能因打印机驱动、纸张进给与装订线而略有差异。"
)

# PageSpec page_size string → QPageSize id
_PAGE_SIZE_IDS: dict[str, QPageSize.PageSizeId] = {
    "A4": QPageSize.PageSizeId.A4,
    "A3": QPageSize.PageSizeId.A3,
    "A2": QPageSize.PageSizeId.A2,
}


@dataclass(frozen=True)
class PrintPreviewInfo:
    """Readable print plan for the dialog."""

    plot_name: str
    page_count: int
    page_size: str
    orientation: str
    page_width_mm: float
    page_height_mm: float
    margins_mm: tuple[float, float, float, float]
    depth_top: float
    depth_bottom: float
    depth_unit: str
    content_height_mm: float
    depth_per_page_mm: float | None
    # One depth window per page, in page order (WYSIWYG pagination).
    page_depth_windows: tuple[tuple[float, float], ...] = ()
    notes: str = PREVIEW_LIMITATIONS


def depth_range_from_presentation(
    presentation: HostPresentation,
    *,
    override: tuple[float, float] | None = None,
) -> tuple[float, float]:
    if override is not None:
        return float(override[0]), float(override[1])
    depth = np.asarray(presentation.depth, dtype=np.float64)
    if depth.size < 1:
        return 0.0, 1.0
    d0 = float(np.nanmin(depth))
    d1 = float(np.nanmax(depth))
    if not math.isfinite(d0) or not math.isfinite(d1) or d1 <= d0:
        return 0.0, 1.0
    return d0, d1


def page_depth_windows(
    depth_top: float,
    depth_bottom: float,
    depth_per_page: float | None,
) -> tuple[tuple[float, float], ...]:
    """Split a depth span into per-page windows (page order).

    ``depth_per_page`` is in depth units (mapped 1:1 to mm for planning only,
    not a physical scale claim). ``None`` → the whole span on one page.
    """
    d0, d1 = float(depth_top), float(depth_bottom)
    if d1 < d0:
        d0, d1 = d1, d0
    span = max(d1 - d0, 1e-9)
    dpp = depth_per_page
    if dpp is None or dpp <= 0:
        return ((d0, d1),)
    out: list[tuple[float, float]] = []
    start = d0
    while start < d1 - 1e-9:
        end = min(start + dpp, d1)
        out.append((start, end))
        start = end
    return tuple(out) if out else ((d0, d1),)


def compute_print_preview(
    *,
    plot_name: str,
    depth_top: float,
    depth_bottom: float,
    depth_unit: str,
    page_spec: PageSpec | None = None,
    content_height_mm: float | None = None,
) -> PrintPreviewInfo:
    """Compute page count, box metrics, and per-page depth windows.

    If ``depth_per_page_mm`` is set on PageSpec, the span is split into
    ``ceil(span / depth_per_page)`` pages; otherwise one page covers the whole
    depth range.
    """
    spec = page_spec or PageSpec()
    d0, d1 = float(depth_top), float(depth_bottom)
    if d1 < d0:
        d0, d1 = d1, d0
    span = max(d1 - d0, 1e-9)
    # Content height for planning: default = printable page height
    m_t, m_r, m_b, m_l = spec.margins_mm
    printable_h = max(10.0, spec.height_mm - m_t - m_b)
    content_h = float(content_height_mm) if content_height_mm else printable_h

    dpp = spec.depth_per_page_mm
    if dpp is not None and dpp > 0:
        page_count = max(1, int(math.ceil(span / dpp)))
        eff_dpp = float(dpp)
    else:
        page_count = 1
        eff_dpp = span  # effective

    return PrintPreviewInfo(
        plot_name=plot_name or "plot",
        page_count=page_count,
        page_size=spec.page_size,
        orientation=spec.orientation,
        page_width_mm=spec.width_mm,
        page_height_mm=spec.height_mm,
        margins_mm=spec.margins_mm,
        depth_top=d0,
        depth_bottom=d1,
        depth_unit=depth_unit or "m",
        content_height_mm=content_h,
        depth_per_page_mm=eff_dpp if dpp else None,
        page_depth_windows=page_depth_windows(d0, d1, dpp),
    )


def render_preview_thumbnail(
    paint_fn: Callable[[Any, QRectF, ...], None],
    page_spec: PageSpec,
    *,
    max_width_px: int = 360,
    depth_range: tuple[float, float] | None = None,
) -> QPixmap:
    """Render a small thumbnail of one page via the host paint callback."""
    spec = page_spec
    # Scale page mm to pixels
    scale = max_width_px / max(spec.width_mm, 1.0)
    w = max(40, int(spec.width_mm * scale))
    h = max(40, int(spec.height_mm * scale))
    pm = QPixmap(w, h)
    pm.fill(QColor("#f8fafc"))
    painter = QPainter(pm)
    try:
        painter.fillRect(0, 0, w, h, QColor("#ffffff"))
        # Light page border
        painter.setPen(QColor("#94a3b8"))
        painter.drawRect(0, 0, w - 1, h - 1)
        m_t, m_r, m_b, m_l = spec.margins_mm
        content = QRectF(
            m_l * scale,
            m_t * scale,
            max(10.0, (spec.width_mm - m_l - m_r) * scale),
            max(10.0, (spec.height_mm - m_t - m_b) * scale),
        )
        paint_fn(painter, content, depth_range=depth_range)
    finally:
        painter.end()
    return pm


def print_preview_pages(
    paint_fn: Callable[[Any, QRectF, ...], None],
    page_spec: PageSpec,
    depth_windows: Sequence[tuple[float, float]],
    printer: QPrinter,
) -> int:
    """Paint one page per depth window onto a QPrinter (or PDF file).

    Returns the number of pages painted. The page geometry is derived from
    the same PageSpec the preview thumbnails use, so printed output matches
    the preview (WYSIWYG).
    """
    printer.setPageSize(QPageSize(_PAGE_SIZE_IDS[page_spec.page_size]))
    orientation = (
        QPageLayout.Orientation.Landscape
        if page_spec.orientation == "landscape"
        else QPageLayout.Orientation.Portrait
    )
    printer.setPageOrientation(orientation)
    m_t, m_r, m_b, m_l = page_spec.margins_mm
    printer.setPageMargins(
        QMarginsF(m_l, m_t, m_r, m_b), QPageLayout.Unit.Millimeter
    )
    painter = QPainter()
    if not painter.begin(printer):
        raise RuntimeError("无法开始打印（QPainter.begin 失败）")
    try:
        pages = 0
        for i, window in enumerate(depth_windows):
            if i:
                printer.newPage()
            page = printer.pageRect(QPrinter.Unit.DevicePixel)
            paint_fn(painter, QRectF(page), depth_range=window)
            pages += 1
    finally:
        painter.end()
    return pages


class PrintPreviewDialog(QDialog):
    """WYSIWYG print preview: per-page thumbnails + layout controls + print."""

    def __init__(
        self,
        info: PrintPreviewInfo,
        *,
        paint_fn: Callable[[Any, QRectF, ...], None] | None = None,
        page_spec: PageSpec | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("PrintPreviewDialog")
        self.setWindowTitle("打印预览")
        self.resize(560, 620)
        self._info = info
        self._paint_fn = paint_fn
        self._page_spec = page_spec or PageSpec()
        self._unit = info.depth_unit or "m"
        self._span = max(info.depth_bottom - info.depth_top, 1e-9)

        root = QVBoxLayout(self)
        form = QFormLayout()
        self.lbl_name = QLabel(info.plot_name)
        self.lbl_name.setObjectName("PreviewPlotName")
        form.addRow("图件", self.lbl_name)

        self.lbl_pages = QLabel(str(info.page_count))
        self.lbl_pages.setObjectName("PreviewPageCount")
        form.addRow("页数", self.lbl_pages)

        self.lbl_box = QLabel(
            f"{info.page_size} {self._orient_label(info.orientation)} · "
            f"{info.page_width_mm:.0f}×{info.page_height_mm:.0f} mm"
        )
        self.lbl_box.setObjectName("PreviewPageBox")
        form.addRow("版心/纸张", self.lbl_box)

        m = info.margins_mm
        self.lbl_margins = QLabel(f"上{m[0]:g} 右{m[1]:g} 下{m[2]:g} 左{m[3]:g} mm")
        self.lbl_margins.setObjectName("PreviewMargins")
        form.addRow("页边距", self.lbl_margins)

        self.lbl_depth = QLabel(
            f"{info.depth_top:.2f} – {info.depth_bottom:.2f} {self._unit}"
        )
        self.lbl_depth.setObjectName("PreviewDepthRange")
        form.addRow("深度范围", self.lbl_depth)

        self.lbl_dpp = QLabel("")
        self.lbl_dpp.setObjectName("PreviewDepthPerPage")
        form.addRow("分页规划", self.lbl_dpp)
        root.addLayout(form)

        # Layout controls (WYSIWYG): recompute the page plan live.
        spec_row = QHBoxLayout()
        spec_row.addWidget(QLabel("纸张"))
        self.size_combo = QComboBox()
        self.size_combo.setObjectName("PreviewSizeCombo")
        for size in ("A4", "A3", "A2"):
            self.size_combo.addItem(size, userData=size)
        self.size_combo.setCurrentIndex(max(0, self.size_combo.findData(info.page_size)))
        self.size_combo.currentIndexChanged.connect(self._recompute)
        spec_row.addWidget(self.size_combo)

        spec_row.addWidget(QLabel("方向"))
        self.orient_combo = QComboBox()
        self.orient_combo.setObjectName("PreviewOrientationCombo")
        self.orient_combo.addItem("纵向", userData="portrait")
        self.orient_combo.addItem("横向", userData="landscape")
        self.orient_combo.setCurrentIndex(
            max(0, self.orient_combo.findData(info.orientation))
        )
        self.orient_combo.currentIndexChanged.connect(self._recompute)
        spec_row.addWidget(self.orient_combo)

        spec_row.addWidget(QLabel("每页深度"))
        self.dpp_spin = QDoubleSpinBox()
        self.dpp_spin.setObjectName("PreviewDepthPerPageSpin")
        self.dpp_spin.setRange(0.0, max(1.0, self._span * 2))
        self.dpp_spin.setDecimals(1)
        self.dpp_spin.setSingleStep(max(1.0, self._span / 10))
        self.dpp_spin.setSuffix(f" {self._unit}")
        self.dpp_spin.setSpecialValueText("整段单页")
        dpp = info.depth_per_page_mm
        self.dpp_spin.setValue(dpp if dpp is not None else 0.0)
        self.dpp_spin.valueChanged.connect(self._recompute)
        spec_row.addWidget(self.dpp_spin)
        spec_row.addStretch(1)
        root.addLayout(spec_row)

        page_row = QHBoxLayout()
        page_row.addWidget(QLabel("预览页"))
        self.page_spin = QSpinBox()
        self.page_spin.setObjectName("PreviewPageSpin")
        self.page_spin.setMinimum(1)
        self.page_spin.setMaximum(max(1, info.page_count))
        self.page_spin.setValue(1)
        self.page_spin.valueChanged.connect(self._refresh_thumb)
        page_row.addWidget(self.page_spin)
        self.lbl_page_depth = QLabel("")
        self.lbl_page_depth.setObjectName("PreviewPageDepthRange")
        page_row.addWidget(self.lbl_page_depth)
        page_row.addStretch(1)
        root.addLayout(page_row)

        self.thumb = QLabel()
        self.thumb.setObjectName("PreviewThumbnail")
        self.thumb.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.thumb.setMinimumHeight(280)
        self.thumb.setStyleSheet("background:#e2e8f0;border:1px solid #94a3b8;")
        root.addWidget(self.thumb, 1)

        notes = QLabel(info.notes)
        notes.setObjectName("PreviewLimitations")
        notes.setWordWrap(True)
        notes.setStyleSheet("color:#64748b;font-size:11px;")
        root.addWidget(notes)

        buttons = QDialogButtonBox()
        print_btn = buttons.addButton(
            "打印…", QDialogButtonBox.ButtonRole.ActionRole
        )
        print_btn.setObjectName("PreviewPrint")
        print_btn.clicked.connect(self._on_print)
        buttons.addButton(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        buttons.accepted.connect(self.accept)
        root.addWidget(buttons)

        self._recompute()

    # ------------------------------------------------------------------
    # Page plan
    # ------------------------------------------------------------------
    @staticmethod
    def _orient_label(orientation: str) -> str:
        return "纵向" if orientation == "portrait" else "横向"

    def _recompute(self) -> None:
        """Rebuild the PageSpec from controls and refresh labels/thumbnails."""
        size = str(self.size_combo.currentData() or "A4")
        orientation = str(self.orient_combo.currentData() or "portrait")
        dpp = self.dpp_spin.value()
        spec = PageSpec(
            page_size=size,  # type: ignore[arg-type]
            orientation=orientation,  # type: ignore[arg-type]
            depth_per_page_mm=dpp if dpp > 0 else None,
        )
        self._page_spec = spec
        info = compute_print_preview(
            plot_name=self._info.plot_name,
            depth_top=self._info.depth_top,
            depth_bottom=self._info.depth_bottom,
            depth_unit=self._unit,
            page_spec=spec,
        )
        self._info = info
        self.lbl_pages.setText(str(info.page_count))
        self.lbl_box.setText(
            f"{info.page_size} {self._orient_label(info.orientation)} · "
            f"{info.page_width_mm:.0f}×{info.page_height_mm:.0f} mm"
        )
        dpp_label = info.depth_per_page_mm
        self.lbl_dpp.setText(
            f"{dpp_label:.2f} {self._unit}/页" if dpp_label else "整段单页"
        )
        self.page_spin.setMaximum(max(1, info.page_count))
        if self.page_spin.value() > info.page_count:
            self.page_spin.setValue(info.page_count)
        self._refresh_thumb()

    def _refresh_thumb(self) -> None:
        if self._paint_fn is None:
            self.thumb.setText("（无可预览内容）")
            return
        page = self.page_spin.value() - 1
        windows = self._info.page_depth_windows
        window = windows[page] if 0 <= page < len(windows) else None
        pm = render_preview_thumbnail(
            self._paint_fn, self._page_spec, depth_range=window
        )
        self.thumb.setPixmap(pm)
        if window is not None:
            self.lbl_page_depth.setText(
                f"当前页 {window[0]:.2f} – {window[1]:.2f} {self._unit}"
            )
        else:
            self.lbl_page_depth.setText("")

    def _on_print(self) -> None:
        if self._paint_fn is None or not self._info.page_depth_windows:
            QMessageBox.information(self, "打印", "（无可打印内容）")
            return
        printer = QPrinter(QPrinter.PrinterMode.HighResolution)
        dialog = QPrintDialog(printer, self)
        dialog.setWindowTitle("打印")
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        try:
            print_preview_pages(
                self._paint_fn,
                self._page_spec,
                self._info.page_depth_windows,
                printer,
            )
        except RuntimeError as exc:
            QMessageBox.warning(self, "打印失败", str(exc))
