"""QWidget multi-track canvas for a HostPresentation (#219).

Paints depth track + curve tracks with host depth pan/zoom. This is the host
display surface until Python binds engine ScenePresentation / WellLogView
multi-track fully.
"""

from __future__ import annotations

import math

import numpy as np
from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QPainter, QPen, QWheelEvent
from PySide6.QtWidgets import QWidget

from well_log_workstation.template_model import HostPresentation
from well_log_workstation.tops_model import FormationTop


class MultiTrackCanvas(QWidget):
    """Single-well multi-track view with shared depth window (pan/zoom)."""

    depth_range_changed = Signal(float, float)
    # Emitted when user picks a depth for a new formation top (#226)
    top_pick_requested = Signal(float)
    # Semantic sample pick (Reference Depth) for graph↔table selection (T5)
    sample_selected = Signal(float)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("MultiTrackCanvas")
        self.setMinimumSize(400, 400)
        self._presentation: HostPresentation | None = None
        self._tops: list[FormationTop] = []
        self._d0: float | None = None
        self._d1: float | None = None
        self._data_d0: float | None = None
        self._data_d1: float | None = None
        self._drag_y: int | None = None
        self._drag_d0: float | None = None
        self._drag_d1: float | None = None
        self._pick_mode = False
        self._press_y: int | None = None
        self._press_x: int | None = None
        self._did_drag = False
        # Semantic selection marker (Reference Depth, not screen Y)
        self._selection_depth: float | None = None
        self.setStyleSheet("background: #ffffff;")
        self.setFocusPolicy(Qt.FocusPolicy.WheelFocus)
        self.setMouseTracking(True)

    def set_pick_mode(self, enabled: bool) -> None:
        """When True, left-click (no drag) requests a top at that depth."""
        self._pick_mode = bool(enabled)
        if enabled:
            self.setCursor(Qt.CursorShape.CrossCursor)
        else:
            self.unsetCursor()
        self.update()

    def pick_mode(self) -> bool:
        return self._pick_mode

    def depth_at_y(self, y: float) -> float | None:
        """Map widget Y to depth in the current viewport, or None if outside band."""
        if self._d0 is None or self._d1 is None:
            return None
        top, bottom = self._plot_band()
        if bottom <= top:
            return None
        if y < top or y > bottom:
            return None
        t = (y - top) / (bottom - top)
        t = max(0.0, min(1.0, t))
        return self._d0 + t * (self._d1 - self._d0)

    def set_presentation(self, presentation: HostPresentation | None) -> None:
        self._presentation = presentation
        self._fit_depth()
        self.update()

    def set_tops(self, tops: list[FormationTop] | None) -> None:
        self._tops = list(tops or [])
        self.update()

    def tops(self) -> list[FormationTop]:
        return list(self._tops)

    def presentation(self) -> HostPresentation | None:
        return self._presentation

    def set_selection_depth(self, depth: float | None) -> None:
        """Highlight a Reference Depth (semantic selection marker)."""
        if depth is None or not math.isfinite(float(depth)):
            self._selection_depth = None
        else:
            self._selection_depth = float(depth)
        self.update()

    def selection_depth(self) -> float | None:
        return self._selection_depth

    def track_count(self) -> int:
        return 0 if self._presentation is None else self._presentation.track_count

    def depth_range(self) -> tuple[float, float] | None:
        if self._d0 is None or self._d1 is None:
            return None
        return self._d0, self._d1

    def set_depth_range(self, d0: float, d1: float) -> None:
        if d1 <= d0:
            return
        self._d0, self._d1 = float(d0), float(d1)
        self.depth_range_changed.emit(self._d0, self._d1)
        self.update()

    def reset_depth_range(self) -> None:
        """Fit viewport to full data depth (double-click)."""
        if self._data_d0 is not None and self._data_d1 is not None:
            self.set_depth_range(self._data_d0, self._data_d1)
        else:
            self._fit_depth()
            self.update()

    def _fit_depth(self) -> None:
        self._data_d0 = self._data_d1 = None
        self._d0 = self._d1 = None
        if self._presentation is None:
            return
        depth = np.asarray(self._presentation.depth, dtype=np.float64)
        if depth.size < 2:
            return
        d0, d1 = float(np.nanmin(depth)), float(np.nanmax(depth))
        if not math.isfinite(d0) or not math.isfinite(d1) or d1 <= d0:
            d0, d1 = 0.0, 1.0
        self._data_d0, self._data_d1 = d0, d1
        self._d0, self._d1 = d0, d1

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802
        if self._d0 is None or self._d1 is None:
            return
        delta = event.angleDelta().y()
        if delta == 0:
            return
        span = self._d1 - self._d0
        factor = 0.9 if delta > 0 else 1.1
        # Zoom toward cursor depth if possible
        top, bottom = self._plot_band()
        y = float(event.position().y())
        if bottom > top:
            t = (y - top) / (bottom - top)
            t = max(0.0, min(1.0, t))
            anchor = self._d0 + t * span
        else:
            anchor = 0.5 * (self._d0 + self._d1)
        new_span = max(span * factor, 1e-3)
        # Keep anchor under cursor
        if bottom > top:
            t = (y - top) / (bottom - top)
            t = max(0.0, min(1.0, t))
            new_d0 = anchor - t * new_span
            new_d1 = new_d0 + new_span
        else:
            mid = 0.5 * (self._d0 + self._d1)
            new_d0, new_d1 = mid - new_span / 2, mid + new_span / 2
        self.set_depth_range(new_d0, new_d1)
        event.accept()

    def mousePressEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self._press_y = int(event.position().y())
            self._press_x = int(event.position().x())
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
        # In pick mode, ignore small moves; treat larger moves as pan cancel pick
        dy = int(event.position().y()) - self._drag_y
        dx = int(event.position().x()) - (self._press_x or 0)
        if abs(dy) > 3 or abs(dx) > 3:
            self._did_drag = True
        if self._pick_mode and not self._did_drag:
            return
        # Shift+click pick path: don't pan while Shift held without drag intent
        if (
            not self._pick_mode
            and event.modifiers() & Qt.KeyboardModifier.ShiftModifier
            and not self._did_drag
        ):
            return
        top, bottom = self._plot_band()
        h = max(1, bottom - top)
        span = self._drag_d1 - self._drag_d0
        shift = (dy / h) * span
        self.set_depth_range(self._drag_d0 + shift, self._drag_d1 + shift)
        event.accept()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            y = float(event.position().y())
            shift_held = bool(
                event.modifiers() & Qt.KeyboardModifier.ShiftModifier
            )
            if not self._did_drag and (self._pick_mode or shift_held):
                depth = self.depth_at_y(y)
                if depth is not None:
                    self.top_pick_requested.emit(depth)
                    event.accept()
                    self._drag_y = None
                    self._press_y = None
                    return
            # Plain click (not tops pick): semantic sample selection (T5)
            if (
                not self._did_drag
                and not self._pick_mode
                and not shift_held
                and self._presentation is not None
            ):
                depth = self.depth_at_y(y)
                if depth is not None:
                    self.sample_selected.emit(depth)
                    event.accept()
                    self._drag_y = None
                    self._press_y = None
                    return
        self._drag_y = None
        self._press_y = None
        self._did_drag = False
        event.accept()

    def mouseDoubleClickEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            if self._pick_mode:
                # Double-click in pick mode: still allow reset via Ctrl+double
                if event.modifiers() & Qt.KeyboardModifier.ControlModifier:
                    self.reset_depth_range()
                event.accept()
                return
            self.reset_depth_range()
            event.accept()
            return
        super().mouseDoubleClickEvent(event)

    def _header_lines(self, pres) -> list[str]:
        hdr = getattr(pres, "header", None)
        if hdr is None:
            return [pres.template_name or "单井图"]
        return hdr.header_lines(
            well_name=pres.well_name,
            template_name=pres.template_name,
            depth_unit=pres.depth_unit or "m",
            scale_summary=pres.scale_summary(),
        )

    def _plot_band(self) -> tuple[int, int]:
        h = self.height()
        n_lines = 2
        if self._presentation is not None:
            n_lines = max(1, len(self._header_lines(self._presentation)))
        title_h = 6 + n_lines * 16
        track_hdr = 26
        footer_h = 18
        return title_h + track_hdr, h - footer_h

    def paintEvent(self, event) -> None:  # noqa: N802
        super().paintEvent(event)
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        if self._presentation is None or not self._presentation.tracks:
            p.setPen(QColor("#888"))
            p.drawText(
                self.rect(),
                Qt.AlignmentFlag.AlignCenter,
                "选择井并应用多图道图版",
            )
            p.end()
            return

        pres = self._presentation
        # Empty Display Set: depth track only (or no visible curve layers)
        if pres.curve_track_count < 1:
            p.setPen(QColor("#666"))
            p.drawText(
                self.rect(),
                Qt.AlignmentFlag.AlignCenter,
                "显示集为空\n勾选井道以显示图形",
            )
            p.end()
            return

        depth = np.asarray(pres.depth, dtype=np.float64)
        if depth.size < 2:
            p.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "深度数据不足")
            p.end()
            return

        if self._d0 is None or self._d1 is None:
            self._fit_depth()
        d0 = self._d0 if self._d0 is not None else 0.0
        d1 = self._d1 if self._d1 is not None else 1.0
        if d1 <= d0:
            d0, d1 = 0.0, 1.0

        # Plot header band (#293)
        header_lines = self._header_lines(pres)
        y_text = 14
        for i, line in enumerate(header_lines):
            font = p.font()
            font.setBold(i == 0)
            font.setPointSize(10 if i == 0 else 8)
            p.setFont(font)
            p.setPen(QColor("#222"))
            p.drawText(8, y_text, line[:120])
            y_text += 16 if i == 0 else 14

        top, bottom = self._plot_band()
        left_margin = 8
        usable_w = max(40, w - left_margin - 8)
        paint_tracks = list(pres.visible_tracks)
        if not paint_tracks:
            p.setPen(QColor("#888"))
            p.drawText(
                self.rect(),
                Qt.AlignmentFlag.AlignCenter,
                "全部图道已隐藏（右栏属性中恢复「可见」）",
            )
            p.end()
            return
        total_frac = sum(max(0.05, t.width_fraction) for t in paint_tracks) or 1.0

        track_hdr_y = top - 26
        x = left_margin
        track_left = left_margin
        track_right = left_margin
        for track in paint_tracks:
            tw = max(24, int(usable_w * (max(0.05, track.width_fraction) / total_frac)))
            # per-track column header
            p.setPen(QPen(QColor("#333"), 1))
            p.drawRect(x, track_hdr_y, tw - 4, 24)
            p.drawText(x + 4, track_hdr_y + 16, track.title[:12])

            # track body
            p.setPen(QPen(QColor("#bbbbbb"), 1))
            p.drawRect(x, top, tw - 4, bottom - top)

            if track.role == "depth":
                p.setPen(QColor("#444"))
                for frac in (0.0, 0.25, 0.5, 0.75, 1.0):
                    yy = top + int((bottom - top) * frac)
                    depth_v = d0 + (d1 - d0) * frac
                    p.drawLine(x, yy, x + 6, yy)
                    p.drawText(x + 8, yy + 4, f"{depth_v:.1f}")
            else:
                for layer in track.layers:
                    self._paint_curve(
                        p,
                        x + 2,
                        top,
                        tw - 8,
                        bottom - top,
                        depth,
                        d0,
                        d1,
                        layer.values,
                        layer.null_mask,
                        track.scale.min if track.scale else 0.0,
                        track.scale.max if track.scale else 100.0,
                        track.scale.mode if track.scale else "linear",
                        QColor(layer.color),
                    )
            track_right = x + tw - 4
            x += tw

        # Formation tops as depth markers across tracks
        if self._tops:
            th = max(1, bottom - top)
            for ft in self._tops:
                if not math.isfinite(ft.depth) or ft.depth < d0 or ft.depth > d1:
                    continue
                yy = top + int(((ft.depth - d0) / (d1 - d0)) * th)
                pen = QPen(QColor(ft.color), 1.2, Qt.PenStyle.DashLine)
                p.setPen(pen)
                p.drawLine(track_left, yy, track_right, yy)
                p.setPen(QColor(ft.color))
                p.drawText(track_left + 4, yy - 2, ft.name[:16])

        # Semantic selection marker (Reference Depth — T5)
        if (
            self._selection_depth is not None
            and math.isfinite(self._selection_depth)
            and d0 <= self._selection_depth <= d1
        ):
            th = max(1, bottom - top)
            yy = top + int(((self._selection_depth - d0) / (d1 - d0)) * th)
            pen = QPen(QColor("#e74c3c"), 1.5, Qt.PenStyle.SolidLine)
            p.setPen(pen)
            p.drawLine(track_left, yy, track_right, yy)
            p.drawText(
                track_left + 4,
                yy - 2,
                f"选中 {self._selection_depth:.2f}",
            )

        # Footer (#293) — template footer + interaction hint
        p.setPen(QColor("#666"))
        font = p.font()
        font.setBold(False)
        font.setPointSize(8)
        p.setFont(font)
        footer = ""
        hdr = getattr(pres, "header", None)
        if hdr is not None:
            footer = hdr.footer_line(
                depth_range=(d0, d1),
                depth_unit=pres.depth_unit or "m",
            )
        tops_note = f"层位 {len(self._tops)}" if self._tops else ""
        pick_note = "拾取层位中" if self._pick_mode else "滚轮缩放/拖动平移/双击复位"
        tail = " · ".join(x for x in (footer, tops_note, pick_note) if x)
        p.drawText(8, h - 4, tail[:160] if tail else f"深度 {d0:.1f}–{d1:.1f}")
        p.end()

    def _paint_curve(
        self,
        p: QPainter,
        x0: int,
        y0: int,
        tw: int,
        th: int,
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
        vals = np.asarray(values, dtype=np.float64)
        n = min(depth.size, vals.size, null_mask.size if null_mask is not None else vals.size)
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
            t = max(0.0, min(1.0, t))
            return x0 + t * tw

        def y_map(d: float) -> float:
            t = (d - d0) / (d1 - d0)
            return y0 + t * th

        pen = QPen(color, 1.5)
        p.setPen(pen)
        prev = None
        step = max(1, n // 2000)
        for i in range(0, n, step):
            if null_mask is not None and bool(null_mask[i]):
                prev = None
                continue
            d = float(depth[i])
            if d < d0 or d > d1:
                prev = None
                continue
            v = float(vals[i])
            xx, yy = x_map(v), y_map(d)
            if not math.isfinite(xx) or not math.isfinite(yy):
                prev = None
                continue
            if prev is not None:
                p.drawLine(int(prev[0]), int(prev[1]), int(xx), int(yy))
            prev = (xx, yy)
