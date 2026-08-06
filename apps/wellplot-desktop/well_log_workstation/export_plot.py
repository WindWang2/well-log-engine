"""Export HostPresentation multi-track plots to SVG/PDF (#221)."""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
from PySide6.QtCore import QRectF, QSizeF, Qt
from PySide6.QtGui import QColor, QPainter, QPdfWriter, QPen
from PySide6.QtWidgets import QApplication

from well_log_workstation.template_model import HostPresentation


class ExportError(Exception):
    """User-visible export failure."""


def _paint_presentation(
    painter: QPainter,
    pres: HostPresentation,
    rect: QRectF,
    *,
    depth_range: tuple[float, float] | None = None,
) -> None:
    """Shared paint path for canvas export (multi-track)."""
    w, h = rect.width(), rect.height()
    x_origin, y_origin = rect.x(), rect.y()
    if not pres.tracks:
        painter.setPen(QColor("#888"))
        painter.drawText(rect, Qt.AlignmentFlag.AlignCenter, "空图")
        return

    depth = np.asarray(pres.depth, dtype=np.float64)
    if depth.size < 2:
        painter.drawText(rect, Qt.AlignmentFlag.AlignCenter, "深度不足")
        return

    if depth_range is not None:
        d0, d1 = depth_range
    else:
        d0, d1 = float(np.nanmin(depth)), float(np.nanmax(depth))
    if not math.isfinite(d0) or not math.isfinite(d1) or d1 <= d0:
        d0, d1 = 0.0, 1.0

    # Plot header (#293) — same fields as on-screen canvas
    hdr = getattr(pres, "header", None)
    if hdr is not None:
        header_lines = hdr.header_lines(
            well_name=pres.well_name,
            template_name=pres.template_name,
            depth_unit=pres.depth_unit or "m",
            scale_summary=pres.scale_summary(),
        )
    else:
        header_lines = [pres.template_name or "单井图"]
    title_band = 8.0 + max(1, len(header_lines)) * 16.0
    y_text = y_origin + 14.0
    for i, line in enumerate(header_lines):
        font = painter.font()
        font.setBold(i == 0)
        font.setPointSize(10 if i == 0 else 8)
        painter.setFont(font)
        painter.setPen(QColor("#222"))
        painter.drawText(int(x_origin + 16), int(y_text), line[:120])
        y_text += 16.0 if i == 0 else 14.0

    track_hdr_h = 28.0
    top = y_origin + title_band + track_hdr_h
    bottom = y_origin + h - 28
    left = x_origin + 16
    usable_w = max(40.0, w - 32)
    paint_tracks = list(pres.visible_tracks)
    if not paint_tracks:
        painter.setPen(QColor("#888"))
        painter.drawText(rect, Qt.AlignmentFlag.AlignCenter, "全部图道已隐藏")
        return
    total_frac = sum(max(0.05, t.width_fraction) for t in paint_tracks) or 1.0

    x = left
    track_hdr_y = top - track_hdr_h
    for track in paint_tracks:
        tw = max(28.0, usable_w * (max(0.05, track.width_fraction) / total_frac))
        painter.setPen(QPen(QColor("#333"), 1))
        painter.drawRect(QRectF(x, track_hdr_y, tw - 6, track_hdr_h - 6))
        painter.drawText(
            QRectF(x + 4, track_hdr_y + 2, tw - 12, track_hdr_h - 10),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            track.title[:16],
        )
        painter.setPen(QPen(QColor("#bbb"), 1))
        painter.drawRect(QRectF(x, top, tw - 6, bottom - top))

        if track.role == "depth":
            painter.setPen(QColor("#333"))
            for frac in (0.0, 0.25, 0.5, 0.75, 1.0):
                yy = top + (bottom - top) * frac
                depth_v = d0 + (d1 - d0) * frac
                painter.drawLine(int(x), int(yy), int(x + 8), int(yy))
                painter.drawText(int(x + 10), int(yy + 4), f"{depth_v:.1f}")
        else:
            for layer in track.layers:
                _paint_curve(
                    painter,
                    x + 3,
                    top,
                    tw - 12,
                    bottom - top,
                    depth,
                    d0,
                    d1,
                    np.asarray(layer.values, dtype=np.float64),
                    np.asarray(layer.null_mask, dtype=bool),
                    track.scale.min if track.scale else 0.0,
                    track.scale.max if track.scale else 100.0,
                    track.scale.mode if track.scale else "linear",
                    QColor(layer.color),
                )
        x += tw

    painter.setPen(QColor("#444"))
    font = painter.font()
    font.setBold(False)
    font.setPointSize(8)
    painter.setFont(font)
    footer = ""
    if hdr is not None:
        footer = hdr.footer_line(
            depth_range=(d0, d1),
            depth_unit=pres.depth_unit or "m",
        )
    if not footer:
        footer = (
            f"{pres.well_name} · {pres.template_name} · "
            f"{pres.track_count} tracks · {pres.depth_unit}"
        )
    painter.drawText(int(x_origin + 16), int(y_origin + h - 10), footer[:160])


def _paint_curve(
    painter: QPainter,
    x0: float,
    y0: float,
    tw: float,
    th: float,
    depth: np.ndarray,
    d0: float,
    d1: float,
    values: np.ndarray,
    null_mask: np.ndarray,
    vmin: float,
    vmax: float,
    mode: str,
    color: QColor,
) -> None:
    n = min(depth.size, values.size, null_mask.size)
    if n < 2 or tw < 4 or th < 4:
        return
    if mode == "log":
        vmin = max(vmin, 1e-6)
        vmax = max(vmax, vmin * 10)
        log_min, log_max = math.log10(vmin), math.log10(vmax)

    def x_map(v: float) -> float:
        if mode == "log":
            if v <= 0 or not math.isfinite(v):
                return float("nan")
            t = (math.log10(v) - log_min) / (log_max - log_min)
        else:
            if not math.isfinite(v):
                return float("nan")
            t = (v - vmin) / (vmax - vmin) if vmax > vmin else 0.5
        return x0 + max(0.0, min(1.0, t)) * tw

    def y_map(d: float) -> float:
        return y0 + ((d - d0) / (d1 - d0)) * th

    painter.setPen(QPen(color, 1.2))
    prev = None
    step = max(1, n // 2500)
    for i in range(0, n, step):
        if bool(null_mask[i]):
            prev = None
            continue
        xx, yy = x_map(float(values[i])), y_map(float(depth[i]))
        if not math.isfinite(xx) or not math.isfinite(yy):
            prev = None
            continue
        if prev is not None:
            painter.drawLine(int(prev[0]), int(prev[1]), int(xx), int(yy))
        prev = (xx, yy)


def export_presentation_svg(
    presentation: HostPresentation,
    path: Path | str,
    *,
    width: int = 900,
    height: int = 1200,
) -> Path:
    """Write SVG via Qt SVG generator."""
    from PySide6.QtSvg import QSvgGenerator

    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    if QApplication.instance() is None:
        raise ExportError("需要 QApplication 才能导出 SVG")

    gen = QSvgGenerator()
    gen.setFileName(str(out))
    gen.setSize(QSizeF(width, height).toSize())
    gen.setViewBox(QRectF(0, 0, width, height))
    gen.setTitle(presentation.well_name)
    gen.setDescription(presentation.template_name)

    painter = QPainter()
    if not painter.begin(gen):
        raise ExportError("无法开始 SVG 绘制")
    try:
        painter.fillRect(QRectF(0, 0, width, height), QColor("#ffffff"))
        _paint_presentation(painter, presentation, QRectF(0, 0, width, height))
    finally:
        painter.end()

    if not out.is_file() or out.stat().st_size < 50:
        raise ExportError("SVG 导出文件为空")
    return out


def export_presentation_pdf(
    presentation: HostPresentation,
    path: Path | str,
    *,
    width_mm: float = 210,
    height_mm: float = 297,
) -> Path:
    """Write PDF via Qt QPdfWriter."""
    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    if QApplication.instance() is None:
        raise ExportError("需要 QApplication 才能导出 PDF")

    writer = QPdfWriter(str(out))
    writer.setTitle(presentation.well_name)
    # A4-ish
    writer.setPageSize(
        writer.pageLayout().pageSize()  # keep default; set resolution
    )
    writer.setResolution(150)

    painter = QPainter()
    if not painter.begin(writer):
        raise ExportError("无法开始 PDF 绘制")
    try:
        page = painter.viewport()
        painter.fillRect(page, QColor("#ffffff"))
        _paint_presentation(
            painter,
            presentation,
            QRectF(page.x(), page.y(), page.width(), page.height()),
        )
    finally:
        painter.end()

    if not out.is_file() or out.stat().st_size < 50:
        raise ExportError("PDF 导出文件为空")
    return out
