"""QWidget multi-track canvas for a HostPresentation (#219).

Paints depth track + curve tracks with host depth pan/zoom. This is the host
display surface until Python binds engine ScenePresentation / WellLogView
multi-track fully.
"""

from __future__ import annotations

import math

import numpy as np
from PySide6.QtCore import QRect, QRectF, Qt, Signal
from PySide6.QtGui import QBrush, QColor, QPainter, QPen, QWheelEvent
from PySide6.QtWidgets import QWidget

from well_log_workstation.template_model import HostPresentation
from well_log_workstation.tops_model import FormationTop


def track_band(pres: HostPresentation, height: int) -> tuple[int, int]:
    """(top, bottom) of the track band for a presentation (mirrors _plot_band)."""
    hdr = getattr(pres, "header", None)
    if hdr is None:
        n_lines = 1
    else:
        n_lines = max(
            1,
            len(
                hdr.header_lines(
                    well_name=pres.well_name,
                    template_name=pres.template_name,
                    depth_unit=pres.depth_unit or "m",
                    scale_summary=pres.scale_summary(),
                )
            ),
        )
    title_h = 6 + n_lines * 16
    return title_h + 26, height - 18


def track_header_rects(
    pres: HostPresentation, width: int, height: int
) -> tuple[list[tuple], tuple[int, int]]:
    """Per-visible-track ``(track, header_rect, body_rect)`` in paint order.

    Shared by paintEvent and the header drag hit-test so both use the same
    width arithmetic (FRS §2.x canvas drag reorder).
    """
    top, bottom = track_band(pres, height)
    left_margin = 8
    usable_w = max(40, width - left_margin - 8)
    paint_tracks = list(pres.visible_tracks)
    total_frac = sum(max(0.05, t.width_fraction) for t in paint_tracks) or 1.0
    entries: list[tuple] = []
    x = left_margin
    for track in paint_tracks:
        tw = max(24, int(usable_w * (max(0.05, track.width_fraction) / total_frac)))
        entries.append(
            (
                track,
                QRect(x, top - 26, tw - 4, 24),
                QRect(x, top, tw - 4, bottom - top),
            )
        )
        x += tw
    return entries, (top, bottom)


def paint_litho_bands(
    painter: QPainter,
    x: int,
    y_top: int,
    tw: int,
    th: int,
    d0: float,
    d1: float,
    track,
) -> None:
    """Fill a track body with lithology segment bands (SY/T 5615 patterns).

    Shared by the interactive canvas and plot export so both surfaces render
    identically. Bands are clipped to the current depth window; segments
    without a resolvable pattern fall back to a plain tint.
    """
    from well_log_workstation.litho_pattern_lib import get_pattern, make_qbrush

    if tw < 4 or th < 4:
        return
    span = d1 - d0
    if span <= 0:
        return
    segments = getattr(track, "litho_segments", None) or []
    brush_cache: dict[str, QBrush] = {}
    for seg in segments:
        if not math.isfinite(seg.top) or not math.isfinite(seg.bottom):
            continue
        if seg.bottom < d0 or seg.top > d1:
            continue
        y0 = max(float(y_top), y_top + ((seg.top - d0) / span) * th)
        y1 = min(float(y_top + th), y_top + ((seg.bottom - d0) / span) * th)
        if y1 - y0 < 0.5:
            continue
        pattern = get_pattern(seg.pattern_id)
        if seg.pattern_id not in brush_cache:
            brush_cache[seg.pattern_id] = (
                make_qbrush(pattern, fallback_color="#93c5fd")
                if pattern is not None
                else QBrush(QColor("#cbd5e1"))
            )
        painter.fillRect(QRectF(x, y0, tw, y1 - y0), brush_cache[seg.pattern_id])
        # Band boundary (skip the very top edge — the track body border).
        if y0 > y_top + 0.5:
            painter.setPen(QPen(QColor("#8a8a8a"), 1))
            painter.drawLine(x, int(round(y0)), x + tw, int(round(y0)))
        # Centered lithology label when the track is wide enough.
        if tw >= 44:
            text = seg.label or (pattern.name if pattern is not None else "")
            if text:
                mid_y = (y0 + y1) / 2
                painter.setPen(QColor("#222222"))
                painter.drawText(
                    QRectF(x + 2, mid_y - 8, tw - 4, 16),
                    Qt.AlignmentFlag.AlignCenter,
                    text,
                )


class MultiTrackCanvas(QWidget):
    """Single-well multi-track view with shared depth window (pan/zoom)."""

    depth_range_changed = Signal(float, float)
    # Emitted when user picks a depth for a new formation top (#226)
    top_pick_requested = Signal(float)
    # Semantic sample pick (Reference Depth) for graph↔table selection (T5)
    sample_selected = Signal(float)
    # Emitted after a track-header drag reorder (FRS §2.x): the new track id
    # order of the presentation. The shell persists it on the plot document.
    track_order_changed = Signal(list)

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
        # Track-header drag reorder (FRS §2.x): visible-track index being
        # dragged and the current insertion target (0..len(visible)).
        self._drag_track_index: int | None = None
        self._drag_track_target: int | None = None
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
            # Track-header hit-test first (FRS §2.x drag reorder); the header
            # drag takes precedence over the pan gesture below.
            if self._presentation is not None and self._presentation.tracks:
                entries, _band = track_header_rects(
                    self._presentation, self.width(), self.height()
                )
                pos = event.position()
                for i, (_track, header, _body) in enumerate(entries):
                    if header.contains(int(pos.x()), int(pos.y())):
                        self._drag_track_index = i
                        self._drag_track_target = i
                        self._press_x = int(pos.x())
                        self._press_y = int(pos.y())
                        self.setCursor(Qt.CursorShape.ClosedHandCursor)
                        event.accept()
                        return
            self._press_y = int(event.position().y())
            self._press_x = int(event.position().x())
            self._did_drag = False
            self._drag_y = self._press_y
            self._drag_d0, self._drag_d1 = self._d0, self._d1
            event.accept()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        # Track-header drag: recompute the insertion target from the cursor x
        # (header-centre boundaries), update the indicator, no depth pan.
        if self._drag_track_index is not None and self._presentation is not None:
            entries, _band = track_header_rects(
                self._presentation, self.width(), self.height()
            )
            x = float(event.position().x())
            target = len(entries)
            for i, (_track, header, _body) in enumerate(entries):
                if x < header.center().x():
                    target = i
                    break
            if target != self._drag_track_target:
                self._drag_track_target = target
                self.update()
            event.accept()
            return
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
        # Track-header drag release: reorder the presentation and report the
        # new id order so the shell can persist it (FRS §2.x).
        if (
            event.button() == Qt.MouseButton.LeftButton
            and self._drag_track_index is not None
            and self._presentation is not None
        ):
            src = self._drag_track_index
            target = (
                self._drag_track_target
                if self._drag_track_target is not None
                else src
            )
            self._drag_track_index = None
            self._drag_track_target = None
            self.unsetCursor()
            self.update()
            vis = list(self._presentation.visible_tracks)
            if (
                0 <= src < len(vis)
                and target != src
                and target != src + 1  # same spot, no reorder
                and target <= len(vis)
            ):
                src_track = vis[src]
                moved_to_end = target == len(vis)
                anchor = vis[-1] if moved_to_end else vis[target]
                tracks = self._presentation.tracks
                if anchor is not src_track:
                    tracks.remove(src_track)
                    anchor_pos = tracks.index(anchor)
                    tracks.insert(
                        anchor_pos + 1 if moved_to_end else anchor_pos, src_track
                    )
                    self.track_order_changed.emit(
                        [t.id for t in tracks]
                    )
            event.accept()
            return
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
        # Empty Display Set: depth track only (or no visible curve layers).
        # A visible lithology track alone is still a usable plot (FRS §2.x).
        if pres.curve_track_count < 1 and not any(
            t.role == "litho" and t.visible for t in pres.tracks
        ):
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

        entries, (top, bottom) = track_header_rects(pres, w, h)
        if not entries:
            p.setPen(QColor("#888"))
            p.drawText(
                self.rect(),
                Qt.AlignmentFlag.AlignCenter,
                "全部图道已隐藏（右栏属性中恢复「可见」）",
            )
            p.end()
            return
        track_left = entries[0][2].left()
        track_right = entries[-1][2].right()

        for track, header, body in entries:
            x, tw = body.x(), body.width()
            # per-track column header
            p.setPen(QPen(QColor("#333"), 1))
            p.drawRect(header)
            p.drawText(header.x() + 4, header.y() + 16, track.title[:12])

            # track body
            p.setPen(QPen(QColor("#bbbbbb"), 1))
            p.drawRect(body)

            if track.role == "depth":
                p.setPen(QColor("#444"))
                for frac in (0.0, 0.25, 0.5, 0.75, 1.0):
                    yy = top + int((bottom - top) * frac)
                    depth_v = d0 + (d1 - d0) * frac
                    p.drawLine(x, yy, x + 6, yy)
                    p.drawText(x + 8, yy + 4, f"{depth_v:.1f}")
            elif track.role == "litho":
                paint_litho_bands(
                    p, x, top, tw, bottom - top, d0, d1, track
                )
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

        # Track-header drag insertion indicator (FRS §2.x): a vertical line at
        # the target insertion boundary, spanning header + body band.
        if (
            self._drag_track_index is not None
            and self._drag_track_target is not None
        ):
            target = self._drag_track_target
            if target < len(entries):
                x_line = entries[target][1].x()
            else:
                x_line = entries[-1][1].x() + entries[-1][1].width()
            p.setPen(QPen(QColor("#e74c3c"), 2))
            p.drawLine(x_line, top - 26, x_line, bottom)

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
