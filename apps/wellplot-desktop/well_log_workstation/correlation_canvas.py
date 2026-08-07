"""Correlation-lite multi-well canvas with shared depth (#222)."""

from __future__ import annotations

import math

import numpy as np
from PySide6.QtCore import QPointF, Qt, Signal
from PySide6.QtGui import (
    QColor,
    QPainter,
    QPainterPath,
    QPen,
    QPolygonF,
    QWheelEvent,
)
from PySide6.QtWidgets import QWidget

from well_log_workstation.correlation_links import HorizonLink
from well_log_workstation.interwell_fill import (
    PINCH_LEFT,
    PINCH_OFF,
    PINCH_RIGHT,
    PINCHOUT_MODE_OFF,
    PINCHOUT_MODES,
    build_interwell_fill_bands,
)
from well_log_workstation.template_model import HostPresentation
from well_log_workstation.tops_model import FormationTop


class CorrelationCanvas(QWidget):
    """Side-by-side well columns; shared depth window (pan/zoom)."""

    depth_range_changed = Signal(float, float)
    # Manual link pick: well_document_id, top name, depth, top id (#231)
    top_clicked = Signal(str, str, float, str)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("CorrelationCanvas")
        self.setMinimumSize(480, 400)
        self.setStyleSheet("background: #ffffff;")
        self._columns: list[HostPresentation] = []
        # Parallel to columns: tops per well column
        self._tops_per_column: list[list[FormationTop]] = []
        self._links: list[HorizonLink] = []
        self._d0: float | None = None
        self._d1: float | None = None
        self._drag_y: int | None = None
        self._drag_d0: float | None = None
        self._drag_d1: float | None = None
        self._link_pick_mode = False
        self._press_y: int | None = None
        self._did_drag = False
        self._highlight: tuple[str, str] | None = None  # well_id, top name
        # Pixel gap between well columns (#295 / T7); default matches legacy paint.
        self._column_gap: int = 6
        # Per-well display-depth shift (MD + shift = display) for datum flatten (#296).
        self._depth_shifts: dict[str, float] = {}
        # Inter-well fill bands (#297 / T9)
        self._show_interwell_fill: bool = False
        self._fill_color: str = "#93c5fd80"  # light blue, semi via alpha in paint
        # Pinchout wedges (FRS §3.3): off by default; per-plot toggle.
        self._pinchout_mode: str = PINCHOUT_MODE_OFF
        self._pinchout_factor: float = 0.5
        self._pinchout_smooth: bool = False
        # Lithology pattern map: top-name → pattern id (SY/T 5615). Empty
        # means solid-color fills only.
        self._fill_pattern_map: dict[str, str] = {}
        # Real well distance (FRS §3.x 实际井距): per-well lateral offsets in
        # well-index units (None = equal spacing). Mirrors the section canvas
        # convention; offsets are linearly interpolated between columns.
        self._well_x_offsets: list[float] | None = None
        # Vertical exaggeration (FRS §3.x 纵横比例尺解耦): depth-axis display
        # stretch factor; 1.0 = unchanged. Independent of pan/zoom (d0/d1).
        self._vertical_exaggeration: float = 1.0

    def column_gap(self) -> int:
        return self._column_gap

    def set_column_gap(self, gap_px: int) -> None:
        """Set horizontal spacing between columns (pixels, clamped)."""
        self._column_gap = max(0, min(200, int(gap_px)))
        self.update()

    def depth_shifts(self) -> dict[str, float]:
        return dict(self._depth_shifts)

    def set_depth_shifts(self, shifts: dict[str, float] | None) -> None:
        """Apply per-well display shifts (well_document_id → additive offset)."""
        self._depth_shifts = {
            str(k): float(v) for k, v in (shifts or {}).items()
        }
        self._fit_depth()
        self.update()

    def _shift_for(self, well_id: str) -> float:
        return float(self._depth_shifts.get(well_id, 0.0))

    def _x_well(self, i: int, col_w: int, gap: int) -> float:
        """X-centre (px) of well column ``i``.

        Without per-well offsets this is the classic ``8 + i*(col_w+gap) +
        col_w/2``. With offsets (well-index units), each well's centre is
        shifted by ``offset_i`` strides so columns spread to real ground
        distance — same convention as the section canvas ``unit_to_pixel``.
        """
        stride = col_w + gap
        base = 8 + i * stride + col_w / 2
        if not self._well_x_offsets or i < 0:
            return base
        if i < len(self._well_x_offsets):
            return base + self._well_x_offsets[i] * stride
        return base

    def show_interwell_fill(self) -> bool:
        return self._show_interwell_fill

    def set_show_interwell_fill(self, enabled: bool) -> None:
        self._show_interwell_fill = bool(enabled)
        self.update()

    def set_fill_color(self, color: str) -> None:
        self._fill_color = str(color or "#93c5fd80")
        self.update()

    def pinchout_mode(self) -> str:
        return self._pinchout_mode

    def pinchout_factor(self) -> float:
        return self._pinchout_factor

    def pinchout_smooth(self) -> bool:
        return self._pinchout_smooth

    def set_pinchout(
        self, mode: str, factor: float, smooth: bool
    ) -> None:
        """Configure pinchout wedges for unilateral intervals (FRS §3.3).

        ``mode`` ∈ {"off","linear"}; ``factor`` is the apex x fraction across
        the inter-column gap (clamped by the geometry helper).
        """
        self._pinchout_mode = mode if mode in PINCHOUT_MODES else PINCHOUT_MODE_OFF
        try:
            self._pinchout_factor = max(0.05, min(1.0, float(factor)))
        except (TypeError, ValueError):
            self._pinchout_factor = 0.5
        self._pinchout_smooth = bool(smooth)
        self.update()

    def fill_pattern_map(self) -> dict[str, str]:
        return dict(self._fill_pattern_map)

    def set_fill_pattern_map(self, mapping: dict[str, str] | None) -> None:
        """Map top names → lithology pattern ids for fill rendering."""
        self._fill_pattern_map = {
            str(k): str(v) for k, v in (mapping or {}).items() if k and v
        }
        self.update()

    def well_x_offsets(self) -> list[float] | None:
        return None if self._well_x_offsets is None else list(self._well_x_offsets)

    def set_well_x_offsets(self, offsets: list[float] | None) -> None:
        """Set per-well lateral offsets (well-index units, None = equal)."""
        if offsets is None:
            self._well_x_offsets = None
        else:
            self._well_x_offsets = [float(o) for o in offsets]
        self.update()

    def vertical_exaggeration(self) -> float:
        return self._vertical_exaggeration

    def set_vertical_exaggeration(self, ve: float) -> None:
        """Set depth-axis display stretch factor (clamped 0.1–20.0)."""
        self._vertical_exaggeration = max(0.1, min(20.0, float(ve)))
        self.update()

    def set_columns(
        self,
        presentations: list[HostPresentation],
        tops_per_column: list[list[FormationTop]] | None = None,
        links: list[HorizonLink] | None = None,
    ) -> None:
        self._columns = list(presentations)
        if tops_per_column is None:
            self._tops_per_column = [[] for _ in self._columns]
        else:
            # Pad / trim to column count
            self._tops_per_column = []
            for i in range(len(self._columns)):
                if i < len(tops_per_column):
                    self._tops_per_column.append(list(tops_per_column[i]))
                else:
                    self._tops_per_column.append([])
        if links is not None:
            self._links = list(links)
        self._fit_depth()
        self.update()

    def set_tops_per_column(self, tops_per_column: list[list[FormationTop]]) -> None:
        self._tops_per_column = []
        for i in range(len(self._columns)):
            if i < len(tops_per_column):
                self._tops_per_column.append(list(tops_per_column[i]))
            else:
                self._tops_per_column.append([])
        self.update()

    def set_links(self, links: list[HorizonLink] | None) -> None:
        self._links = list(links or [])
        self.update()

    def links(self) -> list[HorizonLink]:
        return list(self._links)

    def tops_per_column(self) -> list[list[FormationTop]]:
        return [list(t) for t in self._tops_per_column]

    def columns(self) -> list[HostPresentation]:
        return list(self._columns)

    def column_count(self) -> int:
        return len(self._columns)

    def depth_range(self) -> tuple[float, float] | None:
        if self._d0 is None or self._d1 is None:
            return None
        return self._d0, self._d1

    def set_depth_range(self, d0: float, d1: float) -> None:
        if d1 <= d0:
            return
        self._d0, self._d1 = d0, d1
        self.depth_range_changed.emit(d0, d1)
        self.update()

    def set_link_pick_mode(self, enabled: bool) -> None:
        self._link_pick_mode = bool(enabled)
        if enabled:
            self.setCursor(Qt.CursorShape.CrossCursor)
        else:
            self.unsetCursor()
            self._highlight = None
        self.update()

    def link_pick_mode(self) -> bool:
        return self._link_pick_mode

    def set_pick_highlight(self, well_id: str | None, top_name: str | None) -> None:
        if well_id and top_name:
            self._highlight = (well_id, top_name)
        else:
            self._highlight = None
        self.update()

    def hit_test_top(
        self, x: float, y: float, *, y_tol_px: float = 10.0
    ) -> tuple[str, FormationTop] | None:
        """Return (well_document_id, top) nearest to click, or None."""
        if self._d0 is None or self._d1 is None or not self._columns:
            return None
        n = len(self._columns)
        w, h = self.width(), self.height()
        gap = self._column_gap
        col_w = max(40, (w - 16 - gap * (n - 1)) // n) if n else 40
        top_band, bottom = 36, h - 24
        if y < top_band or y > bottom or bottom <= top_band:
            return None
        # Column index from x: scan each column centre (offsets break the
        # linear stride assumption, so a direct divide won't do).
        idx = -1
        for ci in range(n):
            cx = self._x_well(ci, col_w, gap)
            if x < cx - col_w / 2 or x > cx + col_w / 2:
                continue
            idx = ci
            break
        if idx < 0:
            return None
        d0, d1 = self._d0, self._d1
        ve = self._vertical_exaggeration
        depth_at = (
            d0 + ((y - top_band) / (bottom - top_band)) * (d1 - d0) / ve
            if ve > 0
            else d0
        )
        well_id = self._columns[idx].well_document_id
        shift = self._shift_for(well_id)
        best: FormationTop | None = None
        best_dd = float("inf")
        for ft in self._tops_per_column[idx] if idx < len(self._tops_per_column) else []:
            dd = abs((ft.depth + shift) - depth_at)
            # convert depth delta to pixels (VE stretches the y axis)
            px = abs(dd) / (d1 - d0) * (bottom - top_band) * ve if d1 > d0 else 1e9
            if px <= y_tol_px and dd < best_dd:
                best_dd = dd
                best = ft
        if best is None:
            return None
        return well_id, best

    def _fit_depth(self) -> None:
        mins: list[float] = []
        maxs: list[float] = []
        for pres in self._columns:
            depth = np.asarray(pres.depth, dtype=np.float64)
            if depth.size:
                shift = self._shift_for(pres.well_document_id)
                mins.append(float(np.nanmin(depth)) + shift)
                maxs.append(float(np.nanmax(depth)) + shift)
        if mins and maxs:
            self._d0, self._d1 = min(mins), max(maxs)
        else:
            self._d0, self._d1 = 0.0, 1.0

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802
        if self._d0 is None or self._d1 is None:
            return
        delta = event.angleDelta().y()
        if delta == 0:
            return
        span = self._d1 - self._d0
        factor = 0.9 if delta > 0 else 1.1
        mid = 0.5 * (self._d0 + self._d1)
        new_span = max(span * factor, 1e-3)
        self.set_depth_range(mid - new_span / 2, mid + new_span / 2)
        event.accept()

    def mousePressEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self._press_y = int(event.position().y())
            self._did_drag = False
            self._drag_y = self._press_y
            self._drag_d0, self._drag_d1 = self._d0, self._d1
            event.accept()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        if (
            self._drag_y is None
            or self._drag_d0 is None
            or self._drag_d1 is None
            or self._d0 is None
            or self._d1 is None
        ):
            return
        dy = int(event.position().y()) - self._drag_y
        if abs(dy) > 3:
            self._did_drag = True
        if self._link_pick_mode and not self._did_drag:
            return
        h = max(1, self.height() - 48)
        span = self._drag_d1 - self._drag_d0
        shift = (dy / h) * span
        self.set_depth_range(self._drag_d0 + shift, self._drag_d1 + shift)
        event.accept()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton and not self._did_drag:
            if self._link_pick_mode or (
                event.modifiers() & Qt.KeyboardModifier.ShiftModifier
            ):
                hit = self.hit_test_top(
                    float(event.position().x()), float(event.position().y())
                )
                if hit is not None:
                    well_id, top = hit
                    self.top_clicked.emit(
                        well_id, top.name, float(top.depth), top.id or ""
                    )
                    event.accept()
                    self._drag_y = None
                    self._did_drag = False
                    return
        self._drag_y = None
        self._did_drag = False
        event.accept()

    def paintEvent(self, event) -> None:  # noqa: N802
        super().paintEvent(event)
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        if not self._columns or self._d0 is None or self._d1 is None:
            p.setPen(QColor("#888"))
            p.drawText(
                self.rect(),
                Qt.AlignmentFlag.AlignCenter,
                "创建地层对比图（≥2 口井）",
            )
            p.end()
            return

        n = len(self._columns)
        gap = self._column_gap
        col_w = max(40, (w - 16 - gap * (n - 1)) // n) if n else 40
        top, bottom = 36, h - 24
        d0, d1 = self._d0, self._d1

        # Inter-well fill behind curves (#297)
        if self._show_interwell_fill and n >= 2:
            self._paint_interwell_fills(p, n, gap, col_w, top, bottom, d0, d1)

        for i, pres in enumerate(self._columns):
            x0 = self._x_well(i, col_w, gap) - col_w / 2
            well_shift = self._shift_for(pres.well_document_id)
            p.setPen(QPen(QColor("#333"), 1))
            p.drawRect(x0, 8, col_w - 2, 22)
            p.drawText(x0 + 4, 24, pres.well_name[:18])
            p.setPen(QPen(QColor("#ccc"), 1))
            p.drawRect(x0, top, col_w - 2, bottom - top)

            # primary curve layer from first curve track
            curve_track = next(
                (t for t in pres.tracks if t.role == "curve" and t.layers),
                None,
            )
            depth = np.asarray(pres.depth, dtype=np.float64)
            if curve_track is None or depth.size < 2:
                continue
            layer = curve_track.layers[0]
            vals = np.asarray(layer.values, dtype=np.float64)
            nulls = np.asarray(layer.null_mask, dtype=bool)
            scale = curve_track.scale
            vmin = scale.min if scale else 0.0
            vmax = scale.max if scale else 100.0
            mode = scale.mode if scale else "linear"
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
                return x0 + 4 + max(0.0, min(1.0, t)) * (col_w - 12)

            def y_map(d_display: float) -> float:
                return top + ((d_display - d0) / (d1 - d0)) * (
                    bottom - top
                ) * self._vertical_exaggeration

            p.setPen(QPen(QColor(layer.color), 1.5))
            prev = None
            npts = min(depth.size, vals.size, nulls.size)
            step = max(1, npts // 1500)
            for j in range(0, npts, step):
                if bool(nulls[j]):
                    prev = None
                    continue
                d = float(depth[j]) + well_shift
                if d < d0 or d > d1:
                    prev = None
                    continue
                xx, yy = x_map(float(vals[j])), y_map(d)
                if not math.isfinite(xx) or not math.isfinite(yy):
                    prev = None
                    continue
                if prev is not None:
                    p.drawLine(int(prev[0]), int(prev[1]), int(xx), int(yy))
                prev = (xx, yy)

            # Per-column formation tops as depth references
            col_tops = (
                self._tops_per_column[i]
                if i < len(self._tops_per_column)
                else []
            )
            for ft in col_tops:
                d_disp = float(ft.depth) + well_shift
                if not math.isfinite(ft.depth) or d_disp < d0 or d_disp > d1:
                    continue
                yy = int(y_map(d_disp))
                hl = (
                    self._highlight is not None
                    and self._highlight[0] == pres.well_document_id
                    and self._highlight[1] == ft.name
                )
                pen = QPen(
                    QColor("#f1c40f" if hl else ft.color),
                    2.5 if hl else 1.2,
                    Qt.PenStyle.DashLine,
                )
                p.setPen(pen)
                p.drawLine(x0 + 2, yy, x0 + col_w - 4, yy)
                p.setPen(QColor("#f1c40f" if hl else ft.color))
                p.drawText(x0 + 4, yy - 2, ft.name[:12])

        # Horizon links between columns (#229)
        if self._links and n >= 2:
            well_index = {
                pres.well_document_id: i for i, pres in enumerate(self._columns)
            }
            for link in self._links:
                li = well_index.get(link.left_well_id)
                ri = well_index.get(link.right_well_id)
                if li is None or ri is None:
                    continue
                ld = float(link.left_depth) + self._shift_for(link.left_well_id)
                rd = float(link.right_depth) + self._shift_for(link.right_well_id)
                if ld < d0 or ld > d1:
                    continue
                if rd < d0 or rd > d1:
                    continue
                lx0 = self._x_well(li, col_w, gap) - col_w / 2
                rx0 = self._x_well(ri, col_w, gap) - col_w / 2
                x_left = lx0 + col_w - 4
                x_right = rx0 + 2
                y_left = top + ((ld - d0) / (d1 - d0)) * (bottom - top) * (
                    self._vertical_exaggeration
                )
                y_right = top + ((rd - d0) / (d1 - d0)) * (bottom - top) * (
                    self._vertical_exaggeration
                )
                pen = QPen(QColor(link.color), 1.4, Qt.PenStyle.SolidLine)
                p.setPen(pen)
                p.drawLine(int(x_left), int(y_left), int(x_right), int(y_right))
                mid_x = int(0.5 * (x_left + x_right))
                mid_y = int(0.5 * (y_left + y_right))
                p.drawText(mid_x - 10, mid_y - 2, link.name[:10])

        tops_n = sum(len(t) for t in self._tops_per_column)
        links_n = len(self._links)
        all_bands = (
            build_interwell_fill_bands(
                self._tops_per_column,
                pinchout_mode=self._pinchout_mode,
                pinchout_factor=self._pinchout_factor,
                pinchout_smooth=self._pinchout_smooth,
            )
            if self._show_interwell_fill
            else []
        )
        fill_n = sum(1 for b in all_bands if b.pinch == PINCH_OFF)
        wedge_n = len(all_bands) - fill_n
        pick_note = (
            " · 点选连线中(先后点两井层位)"
            if self._link_pick_mode
            else " · Shift+点层位连线"
        )
        parts = [f"充填 {fill_n}"]
        if wedge_n:
            parts.append(f"尖灭 {wedge_n}")
        fill_note = f" · {' · '.join(parts)}" if self._show_interwell_fill else ""
        spacing_note = "实际井距" if self._well_x_offsets else "等井距"
        ve_note = f" · VE {self._vertical_exaggeration:g}" if (
            abs(self._vertical_exaggeration - 1.0) > 1e-9
        ) else ""
        p.setPen(QColor("#555"))
        p.drawText(
            8,
            h - 6,
            f"对比-lite · {n} 井 · {spacing_note} · 共享深度 {d0:.1f}–{d1:.1f} · "
            f"层位 {tops_n} · 连线 {links_n}{fill_note}{ve_note} · 滚轮缩放 / 拖动平移"
            f"{pick_note}",
        )
        p.end()

    def _paint_interwell_fills(
        self,
        p: QPainter,
        n: int,
        gap: int,
        col_w: int,
        top: int,
        bottom: int,
        d0: float,
        d1: float,
    ) -> None:
        """Paint fill quads + pinchout wedges between adjacent wells."""
        if d1 <= d0:
            return
        bands = build_interwell_fill_bands(
            self._tops_per_column,
            pinchout_mode=self._pinchout_mode,
            pinchout_factor=self._pinchout_factor,
            pinchout_smooth=self._pinchout_smooth,
        )
        color = QColor(self._fill_color)
        if color.alpha() == 255:
            color.setAlpha(96)
        p.setPen(Qt.PenStyle.NoPen)

        # Lazily resolve lithology patterns (top name → QBrush), cached for
        # the duration of this paint so repeated names share one pixmap.
        pat_cache: dict[str, object] = {}
        from well_log_workstation.litho_pattern_lib import get_pattern, make_qbrush

        def brush_for(band) -> object:
            from PySide6.QtGui import QBrush

            pid = self._fill_pattern_map.get(band.top_name) or (
                self._fill_pattern_map.get(band.bottom_name)
            )
            if not pid:
                return QBrush(color)
            if pid in pat_cache:
                return pat_cache[pid]
            pat = get_pattern(pid)
            brush = make_qbrush(pat, self._fill_color) if pat else QBrush(color)
            pat_cache[pid] = brush
            return brush

        def y_of(d_disp: float) -> float:
            return top + ((d_disp - d0) / (d1 - d0)) * (
                bottom - top
            ) * self._vertical_exaggeration

        for band in bands:
            if band.left_col >= n or band.right_col >= n:
                continue
            p.setBrush(brush_for(band))
            left_id = self._columns[band.left_col].well_document_id
            right_id = self._columns[band.right_col].well_document_id
            ls = self._shift_for(left_id)
            rs = self._shift_for(right_id)
            lt = band.left_top_depth + ls
            rt = band.right_top_depth + rs
            lb = band.left_bottom_depth + ls
            rb = band.right_bottom_depth + rs
            apex_d = band.apex_depth + (
                ls if band.pinch == PINCH_RIGHT else rs
            )
            # Viewport cull (apex included for wedges)
            depths = (lt, rt, lb, rb) + (
                (apex_d,) if band.pinch != PINCH_OFF else ()
            )
            if max(depths) < d0 or min(depths) > d1:
                continue
            x_l = self._x_well(band.left_col, col_w, gap) + col_w / 2 - 2
            x_r = self._x_well(band.right_col, col_w, gap) - col_w / 2 + 2
            if band.pinch == PINCH_OFF:
                poly = QPolygonF(
                    [
                        QPointF(x_l, y_of(lt)),
                        QPointF(x_r, y_of(rt)),
                        QPointF(x_r, y_of(rb)),
                        QPointF(x_l, y_of(lb)),
                    ]
                )
                p.drawPolygon(poly)
                continue
            # Wedge: apex on the missing side, full interval on the present side.
            x_a = x_l + (x_r - x_l) * band.apex_frac
            y_a = y_of(apex_d)
            y_top_pres = y_of(rt if band.pinch == PINCH_LEFT else lt)
            y_bot_pres = y_of(rb if band.pinch == PINCH_LEFT else lb)
            if band.smooth:
                # Bézier from the present side toward the apex, with the
                # control point pulled partway back so the well-side
                # thickness is preserved and the edge rounds into the apex.
                path = QPainterPath()
                path.moveTo(x_l if band.pinch == PINCH_RIGHT else x_r, y_top_pres)
                if band.pinch == PINCH_RIGHT:
                    ctrl_top = QPointF(x_l + 0.7 * (x_a - x_l), y_top_pres)
                    path.quadTo(ctrl_top, QPointF(x_a, y_a))
                    ctrl_bot = QPointF(x_l + 0.7 * (x_a - x_l), y_bot_pres)
                    path.quadTo(ctrl_bot, QPointF(x_l, y_bot_pres))
                else:
                    ctrl_top = QPointF(x_r + 0.7 * (x_a - x_r), y_top_pres)
                    path.quadTo(ctrl_top, QPointF(x_a, y_a))
                    ctrl_bot = QPointF(x_r + 0.7 * (x_a - x_r), y_bot_pres)
                    path.quadTo(ctrl_bot, QPointF(x_r, y_bot_pres))
                path.closeSubpath()
                p.drawPath(path)
            else:
                # Straight-edge wedge (triangle): apex shared by top & bottom.
                if band.pinch == PINCH_RIGHT:
                    corners = [
                        QPointF(x_l, y_of(lt)),
                        QPointF(x_a, y_a),
                        QPointF(x_l, y_of(lb)),
                    ]
                else:
                    corners = [
                        QPointF(x_r, y_of(rt)),
                        QPointF(x_a, y_a),
                        QPointF(x_r, y_of(rb)),
                    ]
                p.drawPolygon(QPolygonF(corners))
        p.setBrush(Qt.BrushStyle.NoBrush)
