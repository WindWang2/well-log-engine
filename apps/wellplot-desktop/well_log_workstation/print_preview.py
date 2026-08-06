"""Print preview skeleton (#301 / T13).

Shows page count, physical page box (mm), and depth range for the active
single-well (or correlation) plot. Not full WYSIWYG — layout is a scaled
thumbnail of the host paint path with explicit limitations in the dialog.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Any, Callable

import numpy as np
from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPixmap
from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from well_log_workstation.export_dispatch import PageSpec
from well_log_workstation.template_model import HostPresentation


# Documented limits (shown in UI)
PREVIEW_LIMITATIONS = (
    "骨架预览：非完整 WYSIWYG。缩略图为 Qt 绘制路径的简化缩放，"
    "不含打印机驱动、分页装订线精确对齐或引擎 PDF 字形轮廓差异。"
)


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


def compute_print_preview(
    *,
    plot_name: str,
    depth_top: float,
    depth_bottom: float,
    depth_unit: str,
    page_spec: PageSpec | None = None,
    content_height_mm: float | None = None,
) -> PrintPreviewInfo:
    """Compute page count and box metrics for a depth-axis plot.

    Skeleton rule: one page by default. If ``depth_per_page_mm`` is set on
    PageSpec, page_count = ceil(depth_span / depth_per_page) with span in
    depth units mapped 1:1 to mm for planning only (not a physical scale claim).
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
    else:
        # Skeleton: single page covers full depth range
        page_count = 1
        dpp = span  # effective

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
        depth_per_page_mm=float(dpp) if dpp else None,
        notes=PREVIEW_LIMITATIONS,
    )


def render_preview_thumbnail(
    paint_fn: Callable[[Any, QRectF], None],
    page_spec: PageSpec,
    *,
    max_width_px: int = 360,
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
        paint_fn(painter, content)
    finally:
        painter.end()
    return pm


class PrintPreviewDialog(QDialog):
    """Modal print-preview skeleton: metrics + thumbnail + page spin."""

    def __init__(
        self,
        info: PrintPreviewInfo,
        *,
        paint_fn: Callable[[Any, QRectF], None] | None = None,
        page_spec: PageSpec | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("PrintPreviewDialog")
        self.setWindowTitle("打印预览（骨架）")
        self.resize(520, 560)
        self._info = info
        self._paint_fn = paint_fn
        self._page_spec = page_spec or PageSpec()

        root = QVBoxLayout(self)
        form = QFormLayout()
        self.lbl_name = QLabel(info.plot_name)
        self.lbl_name.setObjectName("PreviewPlotName")
        form.addRow("图件", self.lbl_name)

        self.lbl_pages = QLabel(str(info.page_count))
        self.lbl_pages.setObjectName("PreviewPageCount")
        form.addRow("页数", self.lbl_pages)

        self.lbl_box = QLabel(
            f"{info.page_size} {info.orientation} · "
            f"{info.page_width_mm:.0f}×{info.page_height_mm:.0f} mm"
        )
        self.lbl_box.setObjectName("PreviewPageBox")
        form.addRow("版心/纸张", self.lbl_box)

        m = info.margins_mm
        self.lbl_margins = QLabel(f"上{m[0]:g} 右{m[1]:g} 下{m[2]:g} 左{m[3]:g} mm")
        self.lbl_margins.setObjectName("PreviewMargins")
        form.addRow("页边距", self.lbl_margins)

        self.lbl_depth = QLabel(
            f"{info.depth_top:.2f} – {info.depth_bottom:.2f} {info.depth_unit}"
        )
        self.lbl_depth.setObjectName("PreviewDepthRange")
        form.addRow("深度范围", self.lbl_depth)

        dpp = info.depth_per_page_mm
        self.lbl_dpp = QLabel(
            f"{dpp:.2f} {info.depth_unit}/页" if dpp else "整段单页（骨架）"
        )
        self.lbl_dpp.setObjectName("PreviewDepthPerPage")
        form.addRow("分页规划", self.lbl_dpp)
        root.addLayout(form)

        page_row = QHBoxLayout()
        page_row.addWidget(QLabel("预览页"))
        self.page_spin = QSpinBox()
        self.page_spin.setObjectName("PreviewPageSpin")
        self.page_spin.setMinimum(1)
        self.page_spin.setMaximum(max(1, info.page_count))
        self.page_spin.setValue(1)
        self.page_spin.valueChanged.connect(self._refresh_thumb)
        page_row.addWidget(self.page_spin)
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

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        buttons.accepted.connect(self.accept)
        close_btn = buttons.button(QDialogButtonBox.StandardButton.Close)
        if close_btn is not None:
            close_btn.setObjectName("PreviewClose")
        # Also offer Close as default
        buttons.setStandardButtons(QDialogButtonBox.StandardButton.Close)
        root.addWidget(buttons)

        self._refresh_thumb()

    def _refresh_thumb(self) -> None:
        if self._paint_fn is None:
            self.thumb.setText("（无可预览内容）")
            return
        # Skeleton: same thumbnail for all pages; page spin is informational
        pm = render_preview_thumbnail(self._paint_fn, self._page_spec)
        self.thumb.setPixmap(pm)
