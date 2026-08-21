"""Export dispatch — per-plot-type export routing (Phase-2, T8 / #252; Stage 1 / #277).

T8 resolution: no new engine bindings; host-side dispatcher that routes by
``PlotDocument.type``:

- single_well -> Qt paint (SVG/PDF/PNG); UI defaults SVG/PDF to engine when available (T11)
- correlation -> **Qt paint** SVG/PDF + PNG grab (B0 #300; engine multi-well export later)
- section -> Qt paint (SVG/PDF/PNG)
- plane_map -> ``export_professional_figure`` (PaleoMapCanvas contract)
- fence_3d -> PNG only via ``grabFramebuffer()``; SVG/PDF raise
  ``UnsupportedFormatError``
- composite -> cartography window (SVG/PDF/PNG; mixed vector+raster)

Stage 1 (#277) adds an **engine** backend for ``single_well`` that routes
SVG/PDF through the engine vector exporters. Desktop first-ship B0 (#299 / T11)
defaults the **UI export path** to ``backend="engine"`` when WellLogView is
available.

Export B1 / ADR 0053 adds an explicit **PDF text mode**:

- ``pdf_outline`` (default / B0): engine glyph-outline PDF when available;
  non-searchable — see ``ENGINE_PDF_NONSEARCHABLE_DISCLOSURE``.
- ``pdf_searchable`` (B1.PDF.1/2): prefer **engine** PDF with
  ``searchable_text=True`` (B1.PDF.2 Base-14 Helvetica Latin band labels) when
  the bound ``export_scene_pdf`` accepts the flag; otherwise fall back to Qt
  ``QPdfWriter`` (B1.PDF.1). Do not claim full CJK ToUnicode until B1.PDF.3.

Pagination is host-side and only applies to depth-axis types
(single_well / correlation / section).
"""

from __future__ import annotations

import os
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Literal

from well_log_workstation.export_plot import ExportError
from well_log_workstation.plot_document import PlotDocument

ExportFormat = Literal["svg", "pdf", "png", "cgm"]
ExportBackend = Literal["qt", "engine"]
# ADR 0053 dual mode names (product-facing).
PdfTextMode = Literal["outline", "searchable"]

# B1.CGM.2/3 host disclosure (pattern/alpha + continuous vs multi-page — ADR 0054).
CGM_EXPORT_DISCLOSURE = (
    "CGM 导出（B1.CGM.3 / ADR 0054）：引擎自研 CGM V3 Binary 子集；"
    "可选连续单 PICTURE，或固定页高多 PICTURE 分页。"
    "花纹 = 纯色 + 对角 hatch 近似；半透明强制不透明；"
    "非 Latin 标签可能省略。几何入口容差 0.5 mm。"
)


class UnsupportedFormatError(ExportError):
    """Raised when a plot type cannot produce the requested format (T8)."""


# ADR 0047 / ADR 0021 (T6 / #278): the engine PDF backend emits text as
# glyph outlines (non-searchable) — a regression vs the Qt QPdfWriter path,
# whose text is searchable. The host UI MUST surface this disclosure when
# the user selects the engine PDF backend / outline mode.
ENGINE_PDF_NONSEARCHABLE_DISCLOSURE = (
    "引擎 PDF（图形模式 / pdf_outline）将文字渲染为字形轮廓，不可搜索/不可复制"
    "（ADR 0047）。如需可搜索文本，请在导出时选择「可搜索 PDF」（pdf_searchable，"
    "ADR 0053 / B1.PDF.1，当前为 Qt 矢量路径）。"
)

# Shown when the user picks searchable mode (honest about backend, ADR 0053).
PDF_SEARCHABLE_MODE_NOTE = (
    "可搜索 PDF（pdf_searchable）：优先引擎 Latin-1 可提取文本层"
    "（B1.PDF.2/3，Base-14 Helvetica + WinAnsi；CJK 从可搜索层丢弃并计数，"
    "视觉轮廓仍由字形绘制）。若绑定未重建则回退 Qt 矢量路径（B1.PDF.1，含 CJK）。"
    "完整嵌入字体 ToUnicode 子集仍为后续增强。"
)


def engine_pdf_needs_disclosure(backend: ExportBackend, fmt: ExportFormat) -> bool:
    """True iff the selected backend+format requires the non-searchable-text
    disclosure (T6 / #278). The host UI calls this to decide whether to warn
    before routing an export through the engine PDF path."""
    return backend == "engine" and fmt == "pdf"


def prefer_engine_for_single_well(
    fmt: ExportFormat,
    *,
    engine_available: bool,
    force_backend: ExportBackend | None = None,
    pdf_text_mode: PdfTextMode = "outline",
) -> ExportBackend:
    """Resolve export backend for single_well SVG/PDF (T11 / #299 + ADR 0053).

    - Explicit ``force_backend`` wins.
    - PDF + ``pdf_text_mode="searchable"`` → engine when available (B1.PDF.2
      flag; host falls back to Qt if the binding rejects the arg).
    - Else SVG/PDF default to engine when available; PNG always Qt.
    """
    if force_backend is not None:
        return force_backend
    if fmt == "cgm":
        return "engine" if engine_available else "qt"
    if fmt == "pdf" and pdf_text_mode == "searchable":
        return "engine" if engine_available else "qt"
    if fmt in ("svg", "pdf") and engine_available:
        return "engine"
    return "qt"


def resolve_single_well_pdf_export(
    *,
    engine_available: bool,
    pdf_text_mode: PdfTextMode = "outline",
    force_backend: ExportBackend | None = None,
) -> tuple[ExportBackend, str]:
    """Backend + short note for single-well PDF (UI / tests).

    Returns ``(backend, note)`` where note is a user-visible parenthetical.
    """
    backend = prefer_engine_for_single_well(
        "pdf",
        engine_available=engine_available,
        force_backend=force_backend,
        pdf_text_mode=pdf_text_mode,
    )
    if pdf_text_mode == "searchable":
        if backend == "engine":
            return backend, "（可搜索 · 引擎 Latin）"
        return backend, "（可搜索 · Qt）"
    if backend == "engine":
        return backend, "（引擎 · 图形/不可搜索）"
    return backend, "（Qt）"


@dataclass(frozen=True)
class PageSpec:
    """Export page specification (host-side pagination; ADR 0039 mm units)."""

    page_size: Literal["A4", "A3", "A2"] = "A4"
    orientation: Literal["portrait", "landscape"] = "landscape"
    margins_mm: tuple[float, float, float, float] = (10.0, 10.0, 10.0, 10.0)
    depth_per_page_mm: float | None = None

    @property
    def width_mm(self) -> float:
        dims = {"A4": (210.0, 297.0), "A3": (297.0, 420.0), "A2": (420.0, 594.0)}
        w, h = dims[self.page_size]
        return h if self.orientation == "landscape" else w

    @property
    def height_mm(self) -> float:
        dims = {"A4": (210.0, 297.0), "A3": (297.0, 420.0), "A2": (420.0, 594.0)}
        w, h = dims[self.page_size]
        return w if self.orientation == "landscape" else h


def export_plot(
    plot_doc: PlotDocument,
    fmt: ExportFormat,
    *,
    page_spec: PageSpec | None = None,
    backend: ExportBackend = "qt",
    **kwargs: Any,
) -> Path:
    """Export a plot document in the requested format, routed by type.

    Extra kwargs are forwarded to the per-type backend (e.g. the source
    widget / canvas instance for plane_map / fence_3d / composite).

    ``backend="engine"`` routes ``single_well`` SVG/PDF/CGM through the engine
    vector exporters (Stage 1 / #277 + B1.CGM.2), requiring a ``view``
    (WellLogView with a submitted scene) and ``document_id`` in kwargs. Other
    plot types ignore ``backend`` (engine exporters don't cover them yet).
    """
    spec = page_spec or PageSpec()
    match plot_doc.type:
        case "single_well" if backend == "engine" and fmt in (
            "svg",
            "pdf",
            "cgm",
        ):
            return _engine_export(plot_doc, fmt, spec, **kwargs)
        case "single_well" if fmt == "cgm":
            raise UnsupportedFormatError(
                "CGM 需要引擎后端（WellLogView）；当前不可用"
            )
        case "single_well" | "correlation" | "section":
            if fmt == "cgm":
                raise UnsupportedFormatError(
                    f"{plot_doc.type} 暂不支持 CGM（仅单井引擎路径）"
                )
            return _qt_paint_export(plot_doc, fmt, spec, **kwargs)
        case "plane_map":
            return _plane_map_export(plot_doc, fmt, spec, **kwargs)
        case "fence_3d":
            return _fence_3d_export(plot_doc, fmt, spec, **kwargs)
        case "composite":
            return _composite_export(plot_doc, fmt, spec, **kwargs)
        case _:
            raise UnsupportedFormatError(f"未知图件类型: {plot_doc.type}")


def _engine_export(
    plot_doc: PlotDocument,
    fmt: ExportFormat,
    spec: PageSpec,
    **kwargs: Any,
) -> Path:
    """Engine vector-export backend for single_well (Stage 1 / #277).

    Routes SVG/PDF through the engine exporters bound in T1 (#273) and
    T2 (#274): the engine renders to in-memory bytes; the host writes them
    to disk atomically. Requires a ``view`` (WellLogView with a submitted
    prepared scene) and ``document_id`` in kwargs.

    Returns a ``Path`` (same contract as the Qt backend). Default PDF text is
    glyph-outlines / non-searchable (ADR 0047). Pass
    ``pdf_text_mode="searchable"`` for B1.PDF.2 Latin extractable band text
    (``export_scene_pdf(..., searchable_text=True)``); falls back to Qt path
    via the caller if the binding does not accept the flag.
    """
    view = kwargs.get("view")
    document_id = kwargs.get("document_id")
    if view is None or document_id is None:
        raise ExportError(
            "engine 导出需要 view（WellLogView）与 document_id"
        )
    out = Path(kwargs.get("path") or f"export_{plot_doc.id}.{fmt}")
    out.parent.mkdir(parents=True, exist_ok=True)
    pdf_text_mode: PdfTextMode = kwargs.get("pdf_text_mode") or "outline"
    try:
        if fmt == "svg":
            data: bytes = view.export_scene_svg(document_id)
        elif fmt == "cgm":
            export_cgm = getattr(view, "export_scene_cgm", None)
            if export_cgm is None:
                raise ExportError(
                    "engine CGM 需要重建 Python 绑定（export_scene_cgm）"
                )
            # B1.CGM.3: optional page_height_mm enables multi-PICTURE pagination.
            # Old bindings accept only document_id — fall back via TypeError.
            page_height_mm = kwargs.get("page_height_mm")
            if page_height_mm is not None:
                try:
                    data = export_cgm(document_id, float(page_height_mm))
                except TypeError:
                    data = export_cgm(document_id)
            else:
                data = export_cgm(document_id)
        else:  # pdf
            searchable = pdf_text_mode == "searchable"
            # FRS §5 export options (engine-only; the Qt fallback has neither
            # OCG layers nor crop marks). Older bindings reject the extra
            # args — fall back via TypeError like the searchable branch.
            crop_marks = bool(kwargs.get("crop_marks", False))
            layered_pdf = bool(kwargs.get("layered_pdf", False))
            # Epic B (B4): engine single-well PDFs carry the depth ruler
            # (SDK-authoritative ticks) unless explicitly disabled.
            show_ruler = bool(kwargs.get("show_depth_ruler", True))
            if searchable:
                try:
                    data = view.export_scene_pdf(
                        document_id, 0, True, crop_marks, layered_pdf, show_ruler
                    )
                except TypeError:
                    try:
                        data = view.export_scene_pdf(
                            document_id, 0, True, crop_marks, layered_pdf
                        )
                    except TypeError:
                        # Binding built before B1.PDF.2 — signal caller to use Qt.
                        raise ExportError(
                            "engine searchable PDF 需要重建 Python 绑定"
                            "（export_scene_pdf searchable_text）；请改用 Qt 可搜索路径"
                        ) from None
            else:
                try:
                    data = view.export_scene_pdf(
                        document_id, 0, False, crop_marks, layered_pdf, show_ruler
                    )
                except TypeError:
                    try:
                        data = view.export_scene_pdf(
                            document_id, 0, False, crop_marks, layered_pdf
                        )
                    except TypeError:
                        # Binding without crop_marks/layered_pdf (pre-FRS §5).
                        data = view.export_scene_pdf(document_id)
    except ExportError:
        raise
    except Exception as exc:  # typed WellLogError surfaces here
        raise ExportError(f"引擎 {fmt} 导出失败: {exc}") from exc
    min_size = 20 if fmt == "cgm" else 50
    if not isinstance(data, bytes) or len(data) < min_size:
        raise ExportError(f"引擎 {fmt} 导出返回空数据")
    # Atomic write: never leave a half-written export on failure (issue #38).
    tmp = out.with_name(f".{out.name}.{uuid.uuid4().hex}.tmp")
    try:
        tmp.write_bytes(data)
        os.replace(tmp, out)
    except Exception:
        try:
            if tmp.exists():
                tmp.unlink()
        except OSError:
            pass
        raise
    return out

# -- per-type backends ---------------------------------------------------


def _draw_qt_crop_marks(
    painter,
    rect,
    *,
    margin_mm: float,
    mark_mm: float,
    mm_per_unit: float,
) -> None:
    """Four-corner registration marks for the Qt fallback export (FRS §5).

    Mirrors the engine crop marks (5 mm marks just inside the 10 mm margins,
    8 lines per page) so the Qt and engine backends look consistent on the
    printed page. ``rect`` units are mm (SVG/PNG) or device pixels (PDF at
    150 dpi); ``mm_per_unit`` converts.
    """
    from PySide6.QtCore import QPointF
    from PySide6.QtGui import QColor, QPen

    m = margin_mm * mm_per_unit
    ln = mark_mm * mm_per_unit
    x0, y0 = rect.x(), rect.y()
    w, h = rect.width(), rect.height()
    painter.setPen(QPen(QColor("#000000"), max(1.0, 0.3 * mm_per_unit)))
    corners = (
        (x0 + m, y0 + m, -1.0, -1.0),  # top-left: extend left/up
        (x0 + w - m, y0 + m, 1.0, -1.0),  # top-right: right/up
        (x0 + m, y0 + h - m, -1.0, 1.0),  # bottom-left: left/down
        (x0 + w - m, y0 + h - m, 1.0, 1.0),  # bottom-right: right/down
    )
    for cx, cy, sx, sy in corners:
        painter.drawLine(QPointF(cx, cy), QPointF(cx + sx * ln, cy))
        painter.drawLine(QPointF(cx, cy), QPointF(cx, cy + sy * ln))


def _draw_qt_frame_border(
    painter,
    rect,
    *,
    margin_mm: float,
    mm_per_unit: float,
) -> None:
    """Page frame border (图框边线) for the Qt fallback export (FRS §5).

    A single thin rectangle inset by ``margin_mm`` from the page edge — the
    classic publication figure frame. ``rect`` units are mm (SVG/PNG) or
    device pixels (PDF at 150 dpi); ``mm_per_unit`` converts.
    """
    from PySide6.QtCore import QRectF, Qt
    from PySide6.QtGui import QBrush, QColor, QPen

    m = margin_mm * mm_per_unit
    painter.setPen(QPen(QColor("#000000"), max(1.0, 0.3 * mm_per_unit)))
    painter.setBrush(QBrush(Qt.BrushStyle.NoBrush))
    painter.drawRect(
        QRectF(
            rect.x() + m,
            rect.y() + m,
            rect.width() - 2 * m,
            rect.height() - 2 * m,
        )
    )


def _qt_paint_export(
    plot_doc: PlotDocument,
    fmt: ExportFormat,
    spec: PageSpec,
    **kwargs: Any,
) -> Path:
    """Qt-paint path for single_well / correlation / section.

    Requires a ``paint_fn(painter, rect)`` in kwargs (the host renders the
    presentation(s)); wraps it in the requested format. PNG via QPixmap.
    ``crop_marks=True`` draws the four-corner registration marks (FRS §5,
    matching the engine backend geometry).
    """
    paint_fn = kwargs.get("paint_fn")
    if paint_fn is None:
        raise ExportError(f"{plot_doc.type} 导出需要 paint_fn 回调")
    out = Path(kwargs.get("path") or f"export_{plot_doc.id}.{fmt}")
    out.parent.mkdir(parents=True, exist_ok=True)
    crop_marks = bool(kwargs.get("crop_marks", False))
    border_frame = bool(kwargs.get("border_frame", False))

    if fmt == "svg":
        from PySide6.QtSvg import QSvgGenerator
        from PySide6.QtCore import QRectF, QSizeF
        from PySide6.QtGui import QColor, QPainter
        from PySide6.QtWidgets import QApplication

        if QApplication.instance() is None:
            raise ExportError("需要 QApplication 才能导出 SVG")
        w_mm, h_mm = spec.width_mm, spec.height_mm
        gen = QSvgGenerator()
        gen.setFileName(str(out))
        gen.setSize(QSizeF(w_mm * 3.78, h_mm * 3.78).toSize())  # ~96 dpi base
        gen.setViewBox(QRectF(0, 0, w_mm, h_mm))
        gen.setTitle(plot_doc.name)
        painter = QPainter()
        if not painter.begin(gen):
            raise ExportError("无法开始 SVG 绘制")
        try:
            painter.fillRect(QRectF(0, 0, w_mm, h_mm), QColor("#ffffff"))
            paint_fn(painter, QRectF(0, 0, w_mm, h_mm))
            if crop_marks:
                _draw_qt_crop_marks(
                    painter,
                    QRectF(0, 0, w_mm, h_mm),
                    margin_mm=10.0,
                    mark_mm=5.0,
                    mm_per_unit=1.0,
                )
            if border_frame:
                _draw_qt_frame_border(
                    painter,
                    QRectF(0, 0, w_mm, h_mm),
                    margin_mm=10.0,
                    mm_per_unit=1.0,
                )
        finally:
            painter.end()
    elif fmt == "pdf":
        from PySide6.QtGui import QColor, QPainter, QPdfWriter
        from PySide6.QtCore import QRectF
        from PySide6.QtWidgets import QApplication

        if QApplication.instance() is None:
            raise ExportError("需要 QApplication 才能导出 PDF")
        writer = QPdfWriter(str(out))
        writer.setTitle(plot_doc.name)
        writer.setResolution(150)
        painter = QPainter()
        if not painter.begin(writer):
            raise ExportError("无法开始 PDF 绘制")
        try:
            page = painter.viewport()
            painter.fillRect(page, QColor("#ffffff"))
            paint_fn(painter, QRectF(page.x(), page.y(), page.width(), page.height()))
            if crop_marks:
                # PDF paints in device pixels at the writer resolution (150 dpi).
                _draw_qt_crop_marks(
                    painter,
                    QRectF(page.x(), page.y(), page.width(), page.height()),
                    margin_mm=10.0,
                    mark_mm=5.0,
                    mm_per_unit=150.0 / 25.4,
                )
            if border_frame:
                _draw_qt_frame_border(
                    painter,
                    QRectF(page.x(), page.y(), page.width(), page.height()),
                    margin_mm=10.0,
                    mm_per_unit=150.0 / 25.4,
                )
        finally:
            painter.end()
    else:  # png
        from PySide6.QtCore import QRectF
        from PySide6.QtGui import QColor, QPainter, QPixmap
        from PySide6.QtWidgets import QApplication

        if QApplication.instance() is None:
            raise ExportError("需要 QApplication 才能导出 PNG")
        w_mm, h_mm = spec.width_mm, spec.height_mm
        pm = QPixmap(int(w_mm * 4), int(h_mm * 4))  # ~4 px/mm (~100 dpi)
        pm.fill(QColor("#ffffff"))
        painter = QPainter(pm)
        try:
            paint_fn(painter, QRectF(0, 0, w_mm, h_mm))
            if border_frame:
                _draw_qt_frame_border(
                    painter,
                    QRectF(0, 0, w_mm, h_mm),
                    margin_mm=10.0,
                    mm_per_unit=4.0,  # ~4 px/mm (~100 dpi)
                )
        finally:
            painter.end()
        if not pm.save(str(out)):
            raise ExportError(f"无法保存 PNG: {out}")

    if not out.is_file() or out.stat().st_size < 50:
        raise ExportError("导出文件为空")
    return out


def _plane_map_export(
    plot_doc: PlotDocument,
    fmt: ExportFormat,
    spec: PageSpec,
    **kwargs: Any,
) -> Path:
    """plane_map -> export_professional_figure (PaleoMapCanvas contract)."""
    canvas = kwargs.get("canvas")
    if canvas is None:
        raise ExportError("平面图导出需要 canvas（PaleoMapCanvas）")
    out = Path(kwargs.get("path") or f"export_{plot_doc.id}.{fmt}")
    out.parent.mkdir(parents=True, exist_ok=True)
    try:
        from geoviz import export_professional_figure
    except Exception as exc:
        raise ExportError(f"平面图导出需要 geoviz facade: {exc}") from exc
    export_professional_figure(
        canvas,
        out,
        fmt,
        title=plot_doc.name,
        page_size=spec.page_size,
        orientation=spec.orientation,
    )
    return out


def _fence_3d_export(
    plot_doc: PlotDocument,
    fmt: ExportFormat,
    spec: PageSpec,
    **kwargs: Any,
) -> Path:
    """fence_3d -> PNG only (T8 hard constraint; grabFramebuffer)."""
    if fmt != "png":
        raise UnsupportedFormatError(
            "三维栅状图仅支持 PNG 导出（T8：pyqtgraph 无原生矢量导出）"
        )
    view = kwargs.get("view")
    if view is None:
        raise ExportError("栅状图导出需要 view（FenceView）")
    out = Path(kwargs.get("path") or f"export_{plot_doc.id}.png")
    out.parent.mkdir(parents=True, exist_ok=True)
    return view.grab_fence_png(out)


def _composite_export(
    plot_doc: PlotDocument,
    fmt: ExportFormat,
    spec: PageSpec,
    **kwargs: Any,
) -> Path:
    """composite -> cartography window (mixed vector + raster panels)."""
    window = kwargs.get("window")
    if window is None:
        raise ExportError("综合图导出需要 layout window（CartographyLayoutWindow）")
    out = Path(kwargs.get("path") or f"export_{plot_doc.id}.{fmt}")
    out.parent.mkdir(parents=True, exist_ok=True)
    if fmt == "pdf":
        result = window.export_pdf(str(out))
        if result is None:
            raise ExportError("综合图 PDF 导出失败")
    elif fmt == "svg":
        result = window.export_svg(str(out))
        if result is None:
            raise ExportError("综合图 SVG 导出失败")
    else:  # png
        from PySide6.QtCore import QRectF
        from PySide6.QtGui import QPainter, QPixmap
        from PySide6.QtWidgets import QApplication

        if QApplication.instance() is None:
            raise ExportError("需要 QApplication 才能导出 PNG")
        w_mm, h_mm = spec.width_mm, spec.height_mm
        pm = QPixmap(int(w_mm * 4), int(h_mm * 4))
        pm.fill()
        painter = QPainter(pm)
        try:
            window.scene().render(painter, QRectF(), window.scene().paper_rect())
        finally:
            painter.end()
        if not pm.save(str(out)):
            raise ExportError(f"无法保存综合图 PNG: {out}")
    return out
