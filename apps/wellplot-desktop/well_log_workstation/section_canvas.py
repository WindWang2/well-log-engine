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

import numpy as np
from PySide6.QtCore import QPointF, Qt, Signal
from PySide6.QtGui import QColor, QPainter, QPen, QPolygonF, QBrush, QWheelEvent
from PySide6.QtWidgets import QWidget

from well_log_workstation.correlation_links import HorizonLink
from well_log_workstation.section_geometry import (
    ContactSegment2D,
    FaultSegment2D,
    TieQuad2D,
)
from well_log_workstation.template_model import HostPresentation
from well_log_workstation.tops_model import FormationTop


class SectionCanvas(QWidget):
    """Side-by-side well columns + section geometry overlays."""

    depth_range_changed = Signal(float, float)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("SectionCanvas")
        self.setMinimumSize(480, 400)
        self.setStyleSheet("background: #ffffff;")
        self._columns: list[HostPresentation] = []
        self._tops_per_column: list[list[FormationTop]] = []
        self._faults: list[FaultSegment2D] = []
        self._contacts: list[ContactSegment2D] = []
        self._tie_quads: list[TieQuad2D] = []
        self._d0: float | None = None
        self._d1: float | None = None
        self._drag_y: int | None = None
        self._drag_d0: float | None = None
        self._drag_d1: float | None = None

    # -- data -----------------------------------------------------------

    def set_section(
        self,
        presentations: list[HostPresentation],
        tops_per_column: list[list[FormationTop]] | None = None,
        faults: list[FaultSegment2D] | None = None,
        contacts: list[ContactSegment2D] | None = None,
        tie_quads: list[TieQuad2D] | None = None,
    ) -> None:
        self._columns = list(presentations)
        self._tops_per_column = list(tops_per_column or [])
        self._faults = list(faults or [])
        self._contacts = list(contacts or [])
        self._tie_quads = list(tie_quads or [])
        self._fit_depth()
        self.update()

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
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_y = int(event.position().y())
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
        h = max(1, self.height() - 48)
        span = self._drag_d1 - self._drag_d0
        shift = (dy / h) * span
        self.set_depth_range(self._drag_d0 + shift, self._drag_d1 + shift)
        event.accept()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        self._drag_y = None
        event.accept()

    # -- paint ----------------------------------------------------------

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
                "创建油藏剖面（≥2 口井）",
            )
            p.end()
            return

        n = len(self._columns)
        gap = 6
        col_w = max(40, (w - 16 - gap * (n - 1)) // n)
        top, bottom = 36, h - 24
        d0, d1 = self._d0, self._d1

        def y_map(d: float) -> float:
            return top + ((d - d0) / (d1 - d0)) * (bottom - top)

        def x_well(i: int) -> float:
            return 8 + i * (col_w + gap) + col_w / 2

        # 0. Tie quads (under everything)
        for quad in self._tie_quads:
            poly = QPolygonF()
            for (qx, qy) in quad.corners:
                # corners are in section-space (x = well index scaled);
                # map x back to column center via fraction of n.
                xi = min(max(qx / max(1.0, n - 1), 0.0), float(n - 1))
                cx = 8 + xi * (col_w + gap) + col_w / 2
                poly.append(QPointF(cx, y_map(qy)))
            p.setPen(Qt.PenStyle.NoPen)
            if quad.pattern_id:
                # Real vector lithology pattern (SY/T 5615). Falls back to a
                # solid color when the id is not in the builtin catalog.
                from well_log_workstation.litho_pattern_lib import (
                    get_pattern,
                    make_qbrush,
                )

                pat = get_pattern(quad.pattern_id)
                brush = (
                    make_qbrush(pat, quad.fill_color)
                    if pat is not None
                    else QBrush(QColor(quad.fill_color))
                )
            else:
                brush = QBrush(QColor(quad.fill_color))
            p.setBrush(brush)
            p.drawPolygon(poly)

        # 1. Well columns + curves (mirror CorrelationCanvas)
        for i, pres in enumerate(self._columns):
            x0 = 8 + i * (col_w + gap)
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

        # 2. Fault polylines (red, dashed per ADR 0050)
        for fault in self._faults:
            pen = QPen(QColor(fault.color), 1.4, Qt.PenStyle.SolidLine)
            if fault.dash_pattern:
                pen.setDashPattern(list(fault.dash_pattern))
            p.setPen(pen)
            prev = None
            for (fx, fy) in fault.points:
                xi = min(max(fx / max(1.0, n - 1), 0.0), float(n - 1))
                cx = 8 + xi * (col_w + gap) + col_w / 2
                yy = y_map(fy)
                if prev is not None:
                    p.drawLine(int(prev[0]), int(prev[1]), int(cx), int(yy))
                prev = (cx, yy)

        # 3. Contact polylines (OWC blue / GOC orange, dotted per ADR 0050)
        for contact in self._contacts:
            pen = QPen(QColor(contact.color), 1.4, Qt.PenStyle.SolidLine)
            if contact.dash_pattern:
                pen.setDashPattern(list(contact.dash_pattern))
            p.setPen(pen)
            prev = None
            for (cx0, cy) in contact.points:
                xi = min(max(cx0 / max(1.0, n - 1), 0.0), float(n - 1))
                cx = 8 + xi * (col_w + gap) + col_w / 2
                yy = y_map(cy)
                if prev is not None:
                    p.drawLine(int(prev[0]), int(prev[1]), int(cx), int(yy))
                prev = (cx, yy)

        p.setPen(QColor("#555"))
        p.drawText(
            8,
            h - 6,
            f"油藏剖面 · {n} 井 · 共享深度 {d0:.1f}–{d1:.1f} · "
            f"断层 {len(self._faults)} · 接触 {len(self._contacts)} · "
            f"充填 {len(self._tie_quads)} · 滚轮缩放 / 拖动平移",
        )
        p.end()
