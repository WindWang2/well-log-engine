"""SectionCanvas — 油藏剖面 host-side render (Phase-2, T4 / #248).

Modeled on CorrelationCanvas: side-by-side well columns + shared depth
window (pan/zoom). On top of the well columns it paints the section
geometry overlays computed by ``section_geometry`` (fault polylines red,
OWC/GOC contact polylines blue/orange, tie quads solid fill) with datum
shifts applied (WellSectionDatum: md / tvdss / horizon).

Host-side render decision (session 2026-08-04): the welllog numpy_bridge
has no custom-primitive parsing, so the engine Custom Layer path is NOT
used; the section paints with QPainter and exports via the same host Qt
paint path (T8).
"""

from __future__ import annotations

import math
from typing import Any

import numpy as np
from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QPainter, QPen, QPolygonF, QBrush, QWheelEvent
from PySide6.QtWidgets import QWidget

from well_log_workstation.correlation_links import HorizonLink
from well_log_workstation.section_geometry import (
    FluidContact2D,
    LensBody2D,
    SectionFault2D,
    TieQuad2D,
    append_vertex,
    contact_segment_2d,
    fault_polyline,
    finalize_draft,
    split_quad_composite,
)
from well_log_workstation.template_model import HostPresentation
from well_log_workstation.tops_model import FormationTop


class SectionCanvas(QWidget):
    """Side-by-side well columns + section geometry overlays."""

    depth_range_changed = Signal(float, float)
    # Freehand lens: emitted when a draft polygon is closed (≥3 vertices).
    lens_completed = Signal(object)  # LensBody2D

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("SectionCanvas")
        self.setMinimumSize(480, 400)
        self.setStyleSheet("background: #ffffff;")
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._columns: list[HostPresentation] = []
        self._tops_per_column: list[list[FormationTop]] = []
        self._faults: list[SectionFault2D] = []
        self._contacts: list[FluidContact2D] = []
        self._surfaces: list[Any] = []
        self._lenses: list[LensBody2D] = []
        self._tie_quads: list[TieQuad2D] = []
        self._d0: float | None = None
        self._d1: float | None = None
        self._drag_y: int | None = None
        self._drag_d0: float | None = None
        self._drag_d1: float | None = None
        # Well trajectory display (P1-C / FRS §3.1): per-well lateral offsets
        # in well-index units (0 = equal spacing) and per-well trajectory
        # polylines [x_offset_units, display_depth].
        self._well_x_offsets: list[float] | None = None
        self._well_trajectories: list[np.ndarray | None] | None = None
        # Publication ornaments (P2-C / FRS §5): legend/title/location map.
        self._show_ornaments: bool = False
        self._ornament_data: Any = None
        # Freehand lens drawing (FRS §3.x 透镜体手绘).
        self._draw_lens_mode = False
        self._lens_draft: list[tuple[float, float]] = []
        self._lens_cursor: tuple[float, float] | None = None

    # -- data -----------------------------------------------------------

    def set_section(
        self,
        presentations: list[HostPresentation],
        tops_per_column: list[list[FormationTop]] | None = None,
        faults: list[SectionFault2D] | None = None,
        contacts: list[FluidContact2D] | None = None,
        tie_quads: list[TieQuad2D] | None = None,
        surfaces: list[Any] | None = None,
        lenses: list[LensBody2D] | None = None,
    ) -> None:
        self._columns = list(presentations)
        self._tops_per_column = list(tops_per_column or [])
        self._faults = list(faults or [])
        self._contacts = list(contacts or [])
        self._surfaces = list(surfaces or [])
        self._lenses = list(lenses or [])
        self._tie_quads = list(tie_quads or [])
        self._fit_depth()
        self.update()

    def lenses(self) -> list[LensBody2D]:
        return list(self._lenses)

    def set_lenses(self, lenses: list[LensBody2D] | None) -> None:
        self._lenses = list(lenses or [])
        self.update()

    def draw_lens_mode(self) -> bool:
        return self._draw_lens_mode

    def set_draw_lens_mode(self, enabled: bool) -> None:
        """Toggle freehand lens capture (left-click vertices, double-click close)."""
        self._draw_lens_mode = bool(enabled)
        if not enabled:
            self._lens_draft = []
            self._lens_cursor = None
        self.setCursor(
            Qt.CursorShape.CrossCursor
            if self._draw_lens_mode
            else Qt.CursorShape.ArrowCursor
        )
        self.update()

    def set_well_x_offsets(self, offsets: list[float] | None) -> None:
        """Set per-well lateral offsets in well-index units (0 = equal)."""
        if offsets is None:
            self._well_x_offsets = None
        else:
            self._well_x_offsets = [float(o) for o in offsets]
        self.update()

    def set_well_trajectories(
        self, trajectories: list[np.ndarray | None] | None
    ) -> None:
        """Set per-well trajectory polylines ``[x_offset_units, depth]``."""
        if trajectories is None:
            self._well_trajectories = None
        else:
            self._well_trajectories = list(trajectories)
        self.update()

    def well_x_offsets(self) -> list[float] | None:
        return None if self._well_x_offsets is None else list(self._well_x_offsets)

    def well_trajectories(self) -> list[np.ndarray | None] | None:
        return (
            None
            if self._well_trajectories is None
            else list(self._well_trajectories)
        )

    def show_ornaments(self) -> bool:
        return self._show_ornaments

    def set_show_ornaments(self, enabled: bool) -> None:
        self._show_ornaments = bool(enabled)
        self.update()

    def set_ornament_data(self, data: Any) -> None:
        """Bind publication ornament data (legend/title/location map)."""
        self._ornament_data = data
        self.update()

    def ornament_data(self) -> Any:
        return self._ornament_data

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

    def _fit_depth(self) -> None:
        mins: list[float] = []
        maxs: list[float] = []
        for pres in self._columns:
            depth = np.asarray(pres.depth, dtype=np.float64)
            if depth.size:
                mins.append(float(np.nanmin(depth)))
                maxs.append(float(np.nanmax(depth)))
        if mins and maxs:
            self._d0, self._d1 = min(mins), max(maxs)
        else:
            self._d0, self._d1 = 0.0, 1.0

    # -- interaction (mirror CorrelationCanvas) -------------------------

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
        if self._draw_lens_mode and event.button() == Qt.MouseButton.LeftButton:
            mapped = self._pixel_to_section(event.position().x(), event.position().y())
            if mapped is not None:
                self._lens_draft = append_vertex(
                    self._lens_draft, mapped[0], mapped[1]
                )
                self.update()
            event.accept()
            return
        if self._draw_lens_mode and event.button() == Qt.MouseButton.RightButton:
            # Cancel draft
            self._lens_draft = []
            self._lens_cursor = None
            self.update()
            event.accept()
            return
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_y = int(event.position().y())
            self._drag_d0, self._drag_d1 = self._d0, self._d1
            event.accept()

    def mouseDoubleClickEvent(self, event) -> None:  # noqa: N802
        if self._draw_lens_mode and event.button() == Qt.MouseButton.LeftButton:
            mapped = self._pixel_to_section(event.position().x(), event.position().y())
            if mapped is not None:
                self._lens_draft = append_vertex(
                    self._lens_draft, mapped[0], mapped[1]
                )
            lens = finalize_draft(self._lens_draft)
            if lens is not None:
                self._lenses.append(lens)
                self._lens_draft = []
                self._lens_cursor = None
                self.lens_completed.emit(lens)
                self.update()
            event.accept()
            return
        super().mouseDoubleClickEvent(event)

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        if self._draw_lens_mode:
            mapped = self._pixel_to_section(event.position().x(), event.position().y())
            self._lens_cursor = mapped
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
        dy = int(event.position().y()) - self._drag_y
        h = max(1, self.height() - 48)
        span = self._drag_d1 - self._drag_d0
        shift = (dy / h) * span
        self.set_depth_range(self._drag_d0 + shift, self._drag_d1 + shift)
        event.accept()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        if not self._draw_lens_mode:
            self._drag_y = None
        event.accept()

    def keyPressEvent(self, event) -> None:  # noqa: N802
        if self._draw_lens_mode and event.key() == Qt.Key.Key_Escape:
            self._lens_draft = []
            self._lens_cursor = None
            self.update()
            event.accept()
            return
        if self._draw_lens_mode and event.key() in (
            Qt.Key.Key_Return,
            Qt.Key.Key_Enter,
        ):
            lens = finalize_draft(self._lens_draft)
            if lens is not None:
                self._lenses.append(lens)
                self._lens_draft = []
                self._lens_cursor = None
                self.lens_completed.emit(lens)
                self.update()
            event.accept()
            return
        super().keyPressEvent(event)

    def _layout_metrics(
        self, width: int, height: int
    ) -> tuple[int, int, int, int, int, float, float] | None:
        """Return (n, col_w, gap, top, bottom, d0, d1) or None if not ready."""
        if not self._columns or self._d0 is None or self._d1 is None:
            return None
        n = len(self._columns)
        gap = 6
        col_w = max(40, (width - 16 - gap * (n - 1)) // n)
        top, bottom = 36, height - 24
        return n, col_w, gap, top, bottom, self._d0, self._d1

    def _pixel_to_section(
        self, px: float, py: float
    ) -> tuple[float, float] | None:
        """Map widget pixel → (well-index unit, depth)."""
        m = self._layout_metrics(self.width(), self.height())
        if m is None:
            return None
        n, col_w, gap, top, bottom, d0, d1 = m
        if bottom <= top or d1 <= d0:
            return None
        # Inverse of unit_to_pixel without offset: solve base first, then
        # approximate unit by linear scan (offsets make exact invert hard).
        best_u = 0.0
        best_err = 1e30
        steps = max(n * 20, 20)
        for i in range(steps + 1):
            u = (i / steps) * max(n - 1, 1)
            x = self.unit_to_pixel(u, col_w, gap)
            err = abs(x - px)
            if err < best_err:
                best_err = err
                best_u = u
        t = (py - top) / (bottom - top)
        depth = d0 + t * (d1 - d0)
        return float(best_u), float(depth)

    # -- paint ----------------------------------------------------------

    def paintEvent(self, event) -> None:  # noqa: N802
        super().paintEvent(event)
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        self._paint(p, self.width(), self.height())
        p.end()

    def render_to(
        self,
        painter: QPainter,
        rect: QRectF,
        *,
        depth_range: tuple[float, float] | None = None,
    ) -> None:
        """Paint the section into ``rect`` for export/preview (WYSIWYG).

        ``depth_range`` optionally restricts the depth window (per-page print
        preview); geometry crossing the window edges is clipped by the clip
        rect, consistent with the curve-segment break at the window edge. The
        interactive viewport is restored afterwards.
        """
        saved = (self._d0, self._d1)
        if depth_range is not None:
            d0, d1 = float(depth_range[0]), float(depth_range[1])
            if d1 > d0:
                self._d0, self._d1 = d0, d1
        try:
            painter.save()
            painter.translate(rect.x(), rect.y())
            painter.setClipRect(0.0, 0.0, rect.width(), rect.height())
            self._paint(painter, int(rect.width()), int(rect.height()))
        finally:
            painter.restore()
            self._d0, self._d1 = saved

    def _paint(self, p: QPainter, width: int, height: int) -> None:
        """Paint the section into ``(0, 0, width, height)``.

        Shared by the interactive paintEvent and render_to (export/preview);
        the painter is owned by the caller.
        """
        w, h = width, height
        if not self._columns or self._d0 is None or self._d1 is None:
            p.setPen(QColor("#888"))
            p.drawText(
                QRectF(0, 0, w, h),
                Qt.AlignmentFlag.AlignCenter,
                "创建油藏剖面（≥2 口井）",
            )
            return

        n = len(self._columns)
        gap = 6
        col_w = max(40, (w - 16 - gap * (n - 1)) // n)
        top, bottom = 36, h - 24
        d0, d1 = self._d0, self._d1

        def y_map(d: float) -> float:
            return top + ((d - d0) / (d1 - d0)) * (bottom - top)

        def x_well(i: int) -> float:
            return self.unit_to_pixel(i, col_w, gap)

        def x_unit(u: float) -> float:
            return self.unit_to_pixel(u, col_w, gap)

        # 0. Tie quads (under everything). Apply fault throws to the corners
        # so quads crossing a fault plane show the down-dip offset; then split
        # by any fluid contact that passes through (dual oil/gas vs water fill).
        from well_log_workstation.litho_pattern_lib import (
            get_pattern,
            make_qbrush,
        )

        def _paint_quad(item: TieQuad2D) -> None:
            poly = QPolygonF()
            for (qx, qy) in item.corners:
                xi = min(max(qx / max(1.0, n - 1), 0.0), float(n - 1))
                cx = x_unit(xi)
                poly.append(QPointF(cx, y_map(qy)))
            p.setPen(Qt.PenStyle.NoPen)
            if item.pattern_id:
                pat = get_pattern(item.pattern_id)
                brush = (
                    make_qbrush(pat, item.fill_color)
                    if pat is not None
                    else QBrush(QColor(item.fill_color))
                )
            else:
                brush = QBrush(QColor(item.fill_color))
            p.setBrush(brush)
            p.drawPolygon(poly)

        for quad in self._tie_quads:
            # Composite split: surfaces → every fault → every contact.
            for piece in split_quad_composite(
                quad,
                self._faults,
                self._contacts,
                n,
                surfaces=self._surfaces,
            ):
                _paint_quad(piece)

        # Freehand lens bodies (FRS §3.x 透镜体手绘) — over quads, under wells.
        for lens in self._lenses:
            if not lens.is_valid():
                continue
            poly = QPolygonF()
            for (qx, qy) in lens.points:
                xi = min(max(float(qx), 0.0), float(max(n - 1, 0)))
                poly.append(QPointF(x_unit(xi), y_map(float(qy))))
            fill = QColor(lens.fill_color)
            fill.setAlpha(140)
            p.setBrush(QBrush(fill))
            p.setPen(QPen(QColor(lens.stroke_color), 1.5))
            p.drawPolygon(poly)
            if lens.label:
                cx = float(np.mean(lens.points[:, 0]))
                cy = float(np.mean(lens.points[:, 1]))
                p.setPen(QColor(lens.stroke_color))
                p.drawText(
                    QPointF(x_unit(min(max(cx, 0.0), float(max(n - 1, 0)))), y_map(cy)),
                    lens.label[:16],
                )
        p.setBrush(Qt.BrushStyle.NoBrush)

        # Live freehand draft
        if self._draw_lens_mode and self._lens_draft:
            draft_poly = QPolygonF()
            for (qx, qy) in self._lens_draft:
                draft_poly.append(
                    QPointF(
                        x_unit(min(max(qx, 0.0), float(max(n - 1, 0)))),
                        y_map(qy),
                    )
                )
            if self._lens_cursor is not None:
                cu, cd = self._lens_cursor
                draft_poly.append(
                    QPointF(
                        x_unit(min(max(cu, 0.0), float(max(n - 1, 0)))),
                        y_map(cd),
                    )
                )
            pen = QPen(QColor("#5b21b6"), 1.5, Qt.PenStyle.DashLine)
            p.setPen(pen)
            p.setBrush(Qt.BrushStyle.NoBrush)
            if draft_poly.count() >= 2:
                p.drawPolyline(draft_poly)
            for i in range(draft_poly.count()):
                pt = draft_poly.at(i)
                p.drawEllipse(pt, 3.0, 3.0)

        # 1. Well columns + curves (mirror CorrelationCanvas)
        for i, pres in enumerate(self._columns):
            x0 = x_well(i) - col_w / 2
            p.setPen(QPen(QColor("#333"), 1))
            p.drawRect(x0, 8, col_w - 2, 22)
            p.drawText(x0 + 4, 24, pres.well_name[:18])
            p.setPen(QPen(QColor("#ccc"), 1))
            p.drawRect(x0, top, col_w - 2, bottom - top)

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

            def x_map(v: float) -> float:
                if not math.isfinite(v):
                    return float("nan")
                t = (v - vmin) / (vmax - vmin) if vmax > vmin else 0.5
                return x0 + 4 + max(0.0, min(1.0, t)) * (col_w - 12)

            p.setPen(QPen(QColor(layer.color), 1.5))
            prev = None
            npts = min(depth.size, vals.size, nulls.size)
            step = max(1, npts // 1500)
            for j in range(0, npts, step):
                if bool(nulls[j]):
                    prev = None
                    continue
                d = float(depth[j])
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

        # 2. Well trajectory polylines (P1-C / FRS §3.1): the deviated /
        # horizontal segments of each well, shown grey dashed. Drawn before
        # fault lines so structural overlays stay on top.
        if self._well_trajectories:
            pen = QPen(QColor("#94a3b8"), 1.2, Qt.PenStyle.DashLine)
            p.setPen(pen)
            for i, traj in enumerate(self._well_trajectories):
                if traj is None or traj.shape[0] < 2 or i >= n:
                    continue
                prev = None
                for (tx, ty) in traj:
                    u = min(max(float(i) + float(tx), 0.0), float(n - 1))
                    cx = x_unit(u)
                    yy = y_map(float(ty))
                    if prev is not None:
                        p.drawLine(int(prev[0]), int(prev[1]), int(cx), int(yy))
                    prev = (cx, yy)

        # 3. Fault polylines (red, dashed — FRS §3.3 standard for faults).
        for fault in self._faults:
            pts = fault_polyline(fault, n)
            if pts.shape[0] < 2:
                continue
            pen = QPen(QColor(fault.color), 1.4, Qt.PenStyle.SolidLine)
            if fault.dash_pattern:
                pen.setDashPattern(list(fault.dash_pattern))
            p.setPen(pen)
            prev = None
            for (fx, fy) in pts:
                xi = min(max(fx / max(1.0, n - 1), 0.0), float(n - 1))
                cx = x_unit(xi)
                yy = y_map(fy)
                if prev is not None:
                    p.drawLine(int(prev[0]), int(prev[1]), int(cx), int(yy))
                prev = (cx, yy)

        # 4. Contact polylines (OWC blue / GOC orange, dotted per ADR 0050)
        for contact in self._contacts:
            pen = QPen(QColor(contact.resolved_color()), 1.4, Qt.PenStyle.SolidLine)
            pen.setDashPattern([1.0, 1.0])  # dotted — fluid-contact convention
            p.setPen(pen)
            for seg in contact_segment_2d(contact, n):
                prev = None
                for (cx0, cy) in seg:
                    xi = min(max(cx0 / max(1.0, n - 1), 0.0), float(n - 1))
                    cx = x_unit(xi)
                    yy = y_map(cy)
                    if prev is not None:
                        p.drawLine(int(prev[0]), int(prev[1]), int(cx), int(yy))
                    prev = (cx, yy)

        # 5. Erosion/onlap surface lines (FRS §3.x P1): dark brown dash-dot.
        if self._surfaces:
            from well_log_workstation.section_geometry.erosion_surface import (
                surface_segment_2d,
            )

            pen = QPen(QColor("#92400e"), 1.4, Qt.PenStyle.SolidLine)
            pen.setDashPattern([3.0, 1.5, 0.5, 1.5])  # dash-dot — unconformity
            p.setPen(pen)
            for surface in self._surfaces:
                for seg in surface_segment_2d(surface, n):
                    prev = None
                    for (sx, sy) in seg:
                        xi = min(max(sx / max(1.0, n - 1), 0.0), float(n - 1))
                        cx = x_unit(xi)
                        yy = y_map(sy)
                        if prev is not None:
                            p.drawLine(
                                int(prev[0]), int(prev[1]), int(cx), int(yy)
                            )
                        prev = (cx, yy)

        spacing_note = "地理井距" if self._well_x_offsets else "等井距"
        p.setPen(QColor("#555"))
        p.drawText(
            8,
            h - 6,
            f"油藏剖面 · {n} 井 · {spacing_note} · 共享深度 {d0:.1f}–{d1:.1f} · "
            f"断层 {len(self._faults)} · 接触 {len(self._contacts)} · "
            f"剥蚀/超覆 {len(self._surfaces)} · 透镜体 {len(self._lenses)} · "
            f"充填 {len(self._tie_quads)} · "
            + (
                "绘制中：左键加点 · 双击/Enter 闭合 · 右键/Esc 取消"
                if self._draw_lens_mode
                else "滚轮缩放 / 拖动平移"
            ),
        )

        # Publication ornaments (P2-C / FRS §5): legend + location map +
        # title block in the bottom-right corner (interactive preview).
        if self._show_ornaments and self._ornament_data is not None:
            from well_log_workstation.ornament import OrnamentData, draw_ornaments

            data = (
                self._ornament_data
                if isinstance(self._ornament_data, OrnamentData)
                else OrnamentData()
            )
            if not data.is_empty():
                ow = min(w * 0.42, 300.0)
                oh = min(h * 0.55, 190.0)
                draw_ornaments(
                    p,
                    QRectF(w - ow - 8, h - oh - 26, ow, oh),
                    data,
                )

    def unit_to_pixel(self, u: float, col_w: int, gap: int) -> float:
        """Map a well-index unit (0..n-1, possibly fractional) to pixels.

        With per-well lateral offsets set, the offset is linearly interpolated
        between the two bounding wells so quads/faults/contacts spanning a gap
        follow the offset wells. Without offsets this is the classic
        ``8 + u*(col_w+gap) + col_w/2`` mapping.
        """
        u = max(0.0, min(float(u), float(max(len(self._columns) - 1, 0))))
        base = 8 + u * (col_w + gap) + col_w / 2
        if not self._well_x_offsets:
            return base
        n = len(self._columns)
        if n < 2:
            return base
        i = int(math.floor(u))
        j = min(i + 1, n - 1)
        frac = u - i
        off_i = self._well_x_offsets[i]
        off_j = self._well_x_offsets[j]
        interp = off_i + frac * (off_j - off_i)
        return base + interp * (col_w + gap)
