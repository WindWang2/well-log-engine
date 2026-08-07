"""QWidget multi-track canvas for a HostPresentation (#219).

Paints depth track + curve tracks with host depth pan/zoom. This is the host
display surface until Python binds engine ScenePresentation / WellLogView
multi-track fully.
"""

from __future__ import annotations

import math

import numpy as np
from PySide6.QtCore import QPointF, QRect, QRectF, Qt, Signal
from PySide6.QtGui import QBrush, QColor, QImage, QPainter, QPen, QPixmap, QWheelEvent
from PySide6.QtWidgets import QWidget

from well_log_workstation.template_model import HostPresentation, ScaleSpec
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


def paint_core_photos(
    painter: QPainter,
    x: int,
    y_top: int,
    tw: int,
    th: int,
    d0: float,
    d1: float,
    track,
    resolve_path,
) -> None:
    """Draw depth-ranged core photographs into a track body (FRS §2.x).

    Shared by the interactive canvas and plot export. Each segment's image
    is scaled to fill its depth band (aspect-ratio ignored - core photos
    are columnar). ``resolve_path(seg.image_path)`` returns the absolute
    path to the image file (or None if unresolvable). Segments whose image
    cannot be loaded fall back to a gray tint. Images are cached per paint
    by absolute path.
    """
    if tw < 4 or th < 4:
        return
    span = d1 - d0
    if span <= 0:
        return
    segments = getattr(track, "core_photo_segments", None) or []
    img_cache: dict[str, QImage | None] = {}
    painter.save()
    painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
    for seg in segments:
        if not math.isfinite(seg.top) or not math.isfinite(seg.bottom):
            continue
        if seg.bottom < d0 or seg.top > d1:
            continue
        y0 = max(float(y_top), y_top + ((seg.top - d0) / span) * th)
        y1 = min(float(y_top + th), y_top + ((seg.bottom - d0) / span) * th)
        if y1 - y0 < 0.5:
            continue
        abs_path = resolve_path(seg.image_path)
        if abs_path and abs_path not in img_cache:
            img = QImage(abs_path)
            img_cache[abs_path] = img if not img.isNull() else None
        img = img_cache.get(abs_path) if abs_path else None
        rect = QRectF(x, y0, tw, y1 - y0)
        if img is not None:
            painter.drawImage(rect, img)
        else:
            painter.fillRect(rect, QColor("#cbd5e1"))
        # Band boundary.
        if y0 > y_top + 0.5:
            painter.setPen(QPen(QColor("#8a8a8a"), 1))
            painter.drawLine(x, int(round(y0)), x + tw, int(round(y0)))
        # Label when the track is wide enough.
        if tw >= 44 and seg.label:
            painter.setPen(QColor("#222222"))
            painter.drawText(
                QRectF(x + 2, y0 + 2, tw - 4, 14),
                Qt.AlignmentFlag.AlignLeft,
                seg.label,
            )
    painter.restore()


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
    # Emitted after a track-header right-edge width drag (FRS §2.x): the
    # track id and its new width_fraction. The shell persists it via the
    # existing track_overrides path.
    track_width_changed = Signal(str, float)
    # Emitted after an interactive depth-shift drag (FRS §2.x 交互深度校正):
    # (top_id, new_depth). The shell commits it as a tops edit (undoable).
    depth_shift_committed = Signal(str, float)
    # Emitted after a freehand curve stroke (FRS §2.x 手绘曲线):
    # (mnemonic, points) where points is a list of (depth, value) pairs.
    # The shell merges them into the well's curve_edits.json and reapplies.
    curve_drawn_committed = Signal(str, object)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("MultiTrackCanvas")
        self.setMinimumSize(400, 400)
        self._presentation: HostPresentation | None = None
        # Core-photo image path resolver (FRS §2.x): maps a segment's
        # relative image_path to an absolute path via the active workspace.
        # Set by the shell when the active well changes; None -> gray tint.
        self._core_photo_resolver = None
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
        # Track-header right-edge width drag (FRS §2.x): visible-track index
        # being resized plus the drag origin and start fraction.
        self._resize_track_index: int | None = None
        self._resize_press_x: int = 0
        self._resize_start_frac: float = 0.0
        self._resize_start_tw: int = 0
        self._pick_mode = False
        self._press_y: int | None = None
        self._press_x: int | None = None
        self._did_drag = False
        # Interactive depth-shift drag (FRS §2.x 交互深度校正): when shift
        # mode is on, press near a top grabs it and vertical drags preview a
        # new depth; release emits depth_shift_committed.
        self._shift_mode = False
        self._shift_top: FormationTop | None = None
        self._shift_drag_depth: float | None = None
        self._shift_start_py: int = 0
        # Freehand curve drawing (FRS §2.x 手绘曲线): press on a curve track
        # body starts a stroke; move collects (depth, value) points; release
        # emits curve_drawn_committed. Esc cancels.
        self._draw_curve_mode = False
        self._draw_curve_mnemonic: str | None = None
        self._draw_curve_scale: ScaleSpec | None = None
        self._draw_curve_body: QRect | None = None
        self._draw_curve_points: list[tuple[float, float]] = []
        self._draw_curve_cursor: tuple[float, float] | None = None
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

    def shift_mode(self) -> bool:
        return self._shift_mode

    def set_shift_mode(self, enabled: bool) -> None:
        """Toggle interactive depth-shift mode (drag a top to edit its depth)."""
        self._shift_mode = bool(enabled)
        if not enabled:
            self._shift_top = None
            self._shift_drag_depth = None
        if enabled:
            self.setCursor(Qt.CursorShape.CrossCursor)
        else:
            self.unsetCursor()
        self.update()

    def draw_curve_mode(self) -> bool:
        return self._draw_curve_mode

    def set_draw_curve_mode(self, enabled: bool) -> None:
        """Toggle freehand curve drawing (drag over a curve track to redraw)."""
        self._draw_curve_mode = bool(enabled)
        if not enabled:
            self._draw_curve_mnemonic = None
            self._draw_curve_scale = None
            self._draw_curve_body = None
            self._draw_curve_points = []
            self._draw_curve_cursor = None
        if enabled:
            self.setCursor(Qt.CursorShape.CrossCursor)
        else:
            self.unsetCursor()
        self.update()

    def hit_test_top(
        self, y: float, *, y_tol_px: float = 10.0
    ) -> FormationTop | None:
        """Return the top nearest to ``y`` (pixel tolerance), or None.

        Mirrors the correlation canvas pick: a depth delta is converted to
        pixels against the current window and compared to ``y_tol_px``; the
        nearest top within range wins.
        """
        if self._d0 is None or self._d1 is None or not self._tops:
            return None
        top, bottom = self._plot_band()
        if bottom <= top:
            return None
        depth_at = self.depth_at_y(y)
        if depth_at is None:
            return None
        best: FormationTop | None = None
        best_dd = float("inf")
        for ft in self._tops:
            dd = abs(float(ft.depth) - depth_at)
            px = (
                abs(dd) / (self._d1 - self._d0) * (bottom - top)
                if self._d1 > self._d0
                else 1e9
            )
            if px <= y_tol_px and dd < best_dd:
                best_dd = dd
                best = ft
        return best

    def _draw_curve_start(
        self, px: float, py: float
    ) -> tuple[float, float] | None:
        """Hit-test a curve track body; remember it and return the first point.

        Returns ``(depth, value)`` for the pressed pixel or None when no
        curve track with layers is under the cursor.
        """
        if self._presentation is None or not self._presentation.tracks:
            return None
        entries, _band = track_header_rects(
            self._presentation, self.width(), self.height()
        )
        depth = self.depth_at_y(py)
        if depth is None:
            return None
        for track, _header, body in entries:
            if track.role != "curve" or not track.layers:
                continue
            if not body.contains(int(px), int(py)):
                continue
            layer = track.layers[0]
            self._draw_curve_mnemonic = layer.mnemonic
            self._draw_curve_scale = track.scale
            self._draw_curve_body = body
            v = self._pixel_to_value(px, py, track.scale, body)
            if v is None:
                return None
            return float(depth), float(v)
        return None

    @staticmethod
    def _pixel_to_value(
        px: float, py: float, scale: ScaleSpec | None, body: QRect
    ) -> float | None:
        """Map a body pixel's x to a curve value using the track scale."""
        vmin = scale.min if scale else 0.0
        vmax = scale.max if scale else 100.0
        mode = scale.mode if scale else "linear"
        reverse = bool(getattr(scale, "reverse", False)) if scale else False
        t = (px - body.x()) / max(1, body.width())
        if reverse:
            t = 1.0 - t  # FRS §2.x 反向刻度: inverse of the right->left mapping
        if mode == "log":
            vmin = max(vmin, 1e-6)
            vmax = max(vmax, vmin * 10)
            log_min, log_max = math.log10(vmin), math.log10(vmax)
            return 10 ** (log_min + t * (log_max - log_min))
        return vmin + t * (vmax - vmin)

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

    def set_core_photo_resolver(self, resolver) -> None:
        """Set the callback mapping a core-photo ``image_path`` -> abs path."""
        self._core_photo_resolver = resolver
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
            # Track-header hit-test first (FRS §2.x): the right edge of a
            # header starts a width drag; the rest of the header reorders.
            if self._presentation is not None and self._presentation.tracks:
                entries, _band = track_header_rects(
                    self._presentation, self.width(), self.height()
                )
                pos = event.position()
                px, py = int(pos.x()), int(pos.y())
                for i, (_track, header, _body) in enumerate(entries):
                    if not header.contains(px, py):
                        continue
                    if px >= header.right() - 6:
                        # Width drag: remember the start geometry so the move
                        # handler can convert pixels back to fractions.
                        self._resize_track_index = i
                        self._resize_press_x = px
                        self._resize_start_frac = entries[i][0].width_fraction
                        self._resize_start_tw = header.width()
                        self.setCursor(Qt.CursorShape.SizeHorCursor)
                        event.accept()
                        return
                    self._drag_track_index = i
                    self._drag_track_target = i
                    self._press_x = px
                    self._press_y = py
                    self.setCursor(Qt.CursorShape.ClosedHandCursor)
                    event.accept()
                    return
            # Interactive depth-shift (FRS §2.x): grab a top near the press.
            if self._shift_mode:
                hit = self.hit_test_top(float(event.position().y()))
                if hit is not None:
                    self._shift_top = hit
                    self._shift_drag_depth = float(hit.depth)
                    self._shift_start_py = int(event.position().y())
                    self._press_y = int(event.position().y())
                    self._press_x = int(event.position().x())
                    self._did_drag = False
                    self.update()
                    event.accept()
                    return
            # Freehand curve drawing (FRS §2.x): press on a curve track body
            # starts a stroke over that track's curve.
            if self._draw_curve_mode:
                pt = self._draw_curve_start(event.position().x(), event.position().y())
                if pt is not None:
                    self._draw_curve_points = [pt]
                    self._draw_curve_cursor = pt
                    self._did_drag = False
                    self._press_y = int(event.position().y())
                    self._press_x = int(event.position().x())
                    self.update()
                    event.accept()
                    return
            self._press_y = int(event.position().y())
            self._press_x = int(event.position().x())
            self._did_drag = False
            self._drag_y = self._press_y
            self._drag_d0, self._drag_d1 = self._d0, self._d1
            event.accept()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        # Track-header width drag: convert the horizontal delta back into a
        # width_fraction (inverse of the paint allocation) and apply it live.
        if self._resize_track_index is not None and self._presentation is not None:
            vis = list(self._presentation.visible_tracks)
            index = self._resize_track_index
            if 0 <= index < len(vis):
                track = vis[index]
                dx = float(event.position().x()) - self._resize_press_x
                tw_new = max(24, self._resize_start_tw + int(dx))
                total_frac = (
                    sum(
                        max(0.05, t.width_fraction)
                        for t in self._presentation.visible_tracks
                    )
                    or 1.0
                )
                usable_w = max(40, self.width() - 8 - 8)
                frac = max(
                    0.05, min(1.0, tw_new * total_frac / usable_w)
                )
                if abs(frac - track.width_fraction) > 1e-6:
                    track.width_fraction = frac
                    self.update()
            event.accept()
            return
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
        # Interactive depth-shift drag: follow the cursor depth, no pan.
        if self._shift_mode and self._shift_top is not None:
            d = self.depth_at_y(float(event.position().y()))
            if d is not None:
                self._shift_drag_depth = float(d)
                self._did_drag = True
                self.update()
            event.accept()
            return
        # Freehand curve stroke: append the cursor point, preview live.
        if self._draw_curve_mode and self._draw_curve_mnemonic is not None:
            d = self.depth_at_y(float(event.position().y()))
            if d is not None:
                v = self._pixel_to_value(
                    float(event.position().x()),
                    float(event.position().y()),
                    self._draw_curve_scale,
                    self._draw_curve_body or QRect(0, 0, self.width(), self.height()),
                )
                pt = (float(d), float(v) if v is not None else 0.0)
                if (
                    not self._draw_curve_points
                    or abs(self._draw_curve_points[-1][0] - pt[0]) > 1e-6
                    or abs(self._draw_curve_points[-1][1] - pt[1]) > 1e-6
                ):
                    self._draw_curve_points.append(pt)
                self._draw_curve_cursor = pt
                self._did_drag = True
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
        # Track-header width drag release: report the new width_fraction so
        # the shell persists it (FRS §2.x).
        if (
            event.button() == Qt.MouseButton.LeftButton
            and self._resize_track_index is not None
            and self._presentation is not None
        ):
            vis = list(self._presentation.visible_tracks)
            index = self._resize_track_index
            self._resize_track_index = None
            self.unsetCursor()
            self.update()
            if 0 <= index < len(vis):
                track = vis[index]
                if abs(track.width_fraction - self._resize_start_frac) > 1e-6:
                    self.track_width_changed.emit(
                        track.id, float(track.width_fraction)
                    )
            event.accept()
            return
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
            # Freehand curve stroke release: commit the drawn points.
            if self._draw_curve_mode and self._draw_curve_mnemonic is not None:
                mnemonic = self._draw_curve_mnemonic
                points = list(self._draw_curve_points)
                self._draw_curve_mnemonic = None
                self._draw_curve_scale = None
                self._draw_curve_body = None
                self._draw_curve_points = []
                self._draw_curve_cursor = None
                self._drag_y = None
                self._press_y = None
                if self._did_drag and len(points) >= 2:
                    self.curve_drawn_committed.emit(mnemonic, points)
                self.update()
                event.accept()
                return
            # Interactive depth-shift release: commit the dragged depth.
            if self._shift_mode and self._shift_top is not None:
                top = self._shift_top
                new_depth = self._shift_drag_depth
                self._shift_top = None
                self._shift_drag_depth = None
                self._drag_y = None
                self._press_y = None
                if (
                    self._did_drag
                    and new_depth is not None
                    and abs(new_depth - float(top.depth)) > 1e-9
                ):
                    self.depth_shift_committed.emit(
                        top.id or top.name, float(new_depth)
                    )
                self.update()
                event.accept()
                return
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

    def keyPressEvent(self, event) -> None:  # noqa: N802
        if (
            self._draw_curve_mode
            and self._draw_curve_mnemonic is not None
            and event.key() == Qt.Key.Key_Escape
        ):
            # Cancel the in-progress stroke.
            self._draw_curve_mnemonic = None
            self._draw_curve_scale = None
            self._draw_curve_body = None
            self._draw_curve_points = []
            self._draw_curve_cursor = None
            self._drag_y = None
            self._press_y = None
            self.update()
            event.accept()
            return
        super().keyPressEvent(event)

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
        # A visible lithology or core-photo track alone is still a usable
        # plot (FRS §2.x).
        if pres.curve_track_count < 1 and not any(
            t.role in ("litho", "image") and t.visible for t in pres.tracks
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
            elif track.role == "image":
                resolver = self._core_photo_resolver or (lambda _p: None)
                paint_core_photos(
                    p, x, top, tw, bottom - top, d0, d1, track, resolver
                )
            else:
                # Crossover fill (FRS §2.x 双曲线交叉充填): dual-curve track
                # enclosed region, under the curve lines.
                from well_log_workstation.crossover_fill import paint_crossover_fill

                paint_crossover_fill(
                    p, track, x, tw - 4, top, bottom, d0, d1, depth
                )
                for layer in track.layers:
                    eff_scale = getattr(layer, "scale", None)
                    if eff_scale is None:
                        eff_scale = track.scale
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
                        eff_scale.min if eff_scale else 0.0,
                        eff_scale.max if eff_scale else 100.0,
                        eff_scale.mode if eff_scale else "linear",
                        QColor(layer.color),
                        bool(getattr(eff_scale, "wrap", False)) if eff_scale else False,
                        getattr(eff_scale, "fill_threshold", None)
                        if eff_scale
                        else None,
                        str(getattr(eff_scale, "fill_direction", "above"))
                        if eff_scale
                        else "above",
                        bool(getattr(eff_scale, "reverse", False)) if eff_scale else False,
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
                # Interactive depth-shift drag preview (FRS §2.x): the grabbed
                # top follows the cursor with a highlighted dashed line.
                is_dragging = (
                    self._shift_mode
                    and self._shift_top is not None
                    and (ft.id or ft.name) == (self._shift_top.id or self._shift_top.name)
                )
                draw_depth = (
                    float(self._shift_drag_depth)
                    if is_dragging and self._shift_drag_depth is not None
                    else float(ft.depth)
                )
                if not math.isfinite(draw_depth) or draw_depth < d0 or draw_depth > d1:
                    continue
                yy = top + int(((draw_depth - d0) / (d1 - d0)) * th)
                if is_dragging:
                    pen = QPen(QColor("#f1c40f"), 2.5, Qt.PenStyle.DashLine)
                else:
                    pen = QPen(QColor(ft.color), 1.2, Qt.PenStyle.DashLine)
                p.setPen(pen)
                p.drawLine(track_left, yy, track_right, yy)
                p.setPen(QColor("#f1c40f") if is_dragging else QColor(ft.color))
                label = (
                    f"{ft.name[:12]} → {draw_depth:.2f}"
                    if is_dragging
                    else ft.name[:16]
                )
                p.drawText(track_left + 4, yy - 2, label)

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

        # Freehand curve stroke preview (FRS §2.x 手绘曲线): live red polyline
        # over the target track body while drawing.
        if (
            self._draw_curve_mode
            and self._draw_curve_mnemonic is not None
            and self._draw_curve_points
            and self._draw_curve_scale is not None
            and self._draw_curve_body is not None
        ):
            body = self._draw_curve_body
            scale = self._draw_curve_scale
            vmin = scale.min if scale else 0.0
            vmax = scale.max if scale else 100.0
            mode = scale.mode if scale else "linear"
            reverse = bool(getattr(scale, "reverse", False)) if scale else False
            if mode == "log":
                vmin = max(vmin, 1e-6)
                vmax = max(vmax, vmin * 10)
                log_min, log_max = math.log10(vmin), math.log10(vmax)
            th = max(1, bottom - top)

            def _preview_pt(d: float, v: float) -> QPointF:
                yy = top + int(((d - d0) / (d1 - d0)) * th)
                if mode == "log":
                    t = (math.log10(max(v, 1e-12)) - log_min) / (log_max - log_min)
                else:
                    t = (v - vmin) / (vmax - vmin) if vmax > vmin else 0.5
                t = max(0.0, min(1.0, t))
                if reverse:
                    t = 1.0 - t  # FRS §2.x 反向刻度: scale runs right->left
                return QPointF(body.x() + t * body.width(), yy)

            pen = QPen(QColor("#e74c3c"), 2.0)
            p.setPen(pen)
            pts = [QPointF(_preview_pt(d, v)) for d, v in self._draw_curve_points]
            if self._draw_curve_cursor is not None:
                pts.append(_preview_pt(*self._draw_curve_cursor))
            for a, b in zip(pts, pts[1:]):
                p.drawLine(a, b)
            p.setBrush(QColor("#e74c3c"))
            for pt in pts:
                p.drawEllipse(pt, 2.5, 2.5)

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
        pick_note = (
            "手绘曲线：按住左键在曲线道上绘制 · 释放保存 · Esc 取消"
            if self._draw_curve_mode
            else "深度校正：拖拽层位线调整深度 · 释放即保存（可撤销）"
            if self._shift_mode
            else "拾取层位中"
            if self._pick_mode
            else "滚轮缩放/拖动平移/双击复位"
        )
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
        wrap: bool = False,
        fill_threshold: float | None = None,
        fill_direction: str = "above",
        reverse: bool = False,
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
            if wrap:
                t = t - math.floor(t)  # fold back instead of clipping
            else:
                t = max(0.0, min(1.0, t))
            if reverse:
                t = 1.0 - t  # FRS §2.x 反向刻度: scale runs right->left
            return x0 + t * tw

        def y_map(d: float) -> float:
            t = (d - d0) / (d1 - d0)
            return y0 + t * th

        # Baseline fill (FRS §2.x 基线充填): semi-transparent area from the
        # curve to the track's right edge where the value passes the
        # threshold, drawn under the curve line.
        if fill_threshold is not None:
            from well_log_workstation.baseline_fill import baseline_fill_polygons

            step_fill = max(1, n // 2000)
            polys = baseline_fill_polygons(
                x_map,
                y_map,
                x0 if reverse else x0 + tw,  # fill to the high-value edge
                depth,
                d0,
                d1,
                vals,
                null_mask if null_mask is not None else np.zeros(n, bool),
                step=step_fill,
                threshold=fill_threshold,
                direction=fill_direction,
            )
            if polys:
                fill = QColor(color)
                fill.setAlpha(96)
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(fill)
                for poly in polys:
                    p.drawPolygon(poly)
                p.setBrush(Qt.BrushStyle.NoBrush)

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
