"""Publication ornaments for exported figures (FRS §5 / P2-C).

Title block (责任表), legend block (图例栏), location map (接合图 + 指北针)
and scale bar (比例尺) rendered with plain QPainter so the same layer can be
drawn on the interactive section canvas and on the Qt-paint export path.

Layout helpers are pure (no QPainter) and headless-testable.
"""

from __future__ import annotations

import datetime
from dataclasses import dataclass, field
from typing import Any

from PySide6.QtCore import QPointF, QRectF
from PySide6.QtGui import QColor, QFont, QPainter, QPen, QPolygonF


@dataclass
class OrnamentData:
    """Everything the ornament layer needs to draw."""

    title_fields: dict[str, str] = field(default_factory=dict)
    # (sample, label): sample is a hex colour string OR a QBrush (pattern).
    legend_items: list[tuple[Any, str]] = field(default_factory=list)
    # Location map: well points (lng, lat) + indices of the section wells.
    map_points: list[tuple[float, float]] = field(default_factory=list)
    map_highlight: list[int] = field(default_factory=list)
    scale_text: str = ""

    def is_empty(self) -> bool:
        return not (self.title_fields or self.legend_items or self.map_points)


# ---------------------------------------------------------------------------
# Pure layout helpers
# ---------------------------------------------------------------------------


def title_block_fields(
    title: str,
    workspace_name: str,
    scale_text: str = "",
    date: str | None = None,
) -> dict[str, str]:
    """Build the title-block fields (date defaults to today)."""
    return {
        "title": title,
        "workspace": workspace_name,
        "scale": scale_text,
        "date": date or datetime.date.today().isoformat(),
    }


def layout_legend_items(
    items: list[Any],
    rect: QRectF,
    item_w: float,
    item_h: float,
    cols: int,
) -> list[tuple[float, float, Any]]:
    """Grid-layout legend items into (x, y, item) positions within rect."""
    out: list[tuple[float, float, Any]] = []
    if not items or cols <= 0 or item_w <= 0 or item_h <= 0:
        return out
    x0 = float(rect.x())
    y0 = float(rect.y())
    right = float(rect.x()) + float(rect.width())
    bottom = float(rect.y()) + float(rect.height())
    for i, item in enumerate(items):
        col = i % cols
        row = i // cols
        x = x0 + col * item_w
        y = y0 + row * item_h
        if x + item_w > right or y + item_h > bottom:
            continue
        out.append((x, y, item))
    return out


# ---------------------------------------------------------------------------
# QPainter drawing
# ---------------------------------------------------------------------------


def draw_title_block(p: QPainter, rect: QRectF, fields: dict[str, str]) -> None:
    """Draw a 4-row title block (图名/工区/比例尺/日期)."""
    p.save()
    p.setPen(QPen(QColor("#334155"), 1.0))
    rows = [
        ("图名", fields.get("title", "")),
        ("工区", fields.get("workspace", "")),
        ("比例尺", fields.get("scale", "")),
        ("日期", fields.get("date", "")),
    ]
    row_h = float(rect.height()) / max(1, len(rows))
    font = QFont()
    font.setPointSizeF(max(6.0, row_h * 0.45))
    p.setFont(font)
    for i, (label, value) in enumerate(rows):
        y = float(rect.y()) + i * row_h
        p.drawRect(QRectF(float(rect.x()), y, float(rect.width()), row_h))
        p.drawText(
            QRectF(float(rect.x()) + 4, y, float(rect.width()) - 8, row_h),
            f"{label}: {value}",
        )
    p.restore()


def draw_legend_block(
    p: QPainter,
    rect: QRectF,
    items: list[tuple[Any, str]],
    cols: int,
    item_w: float = 22.0,
    item_h: float = 14.0,
) -> None:
    """Draw a legend: sample swatch (colour or pattern brush) + label."""
    p.save()
    if not items or rect.width() <= 0 or rect.height() <= 0:
        p.restore()
        return
    placed = layout_legend_items(items, rect, item_w, item_h, cols)
    font = QFont()
    font.setPointSizeF(7.0)
    p.setFont(font)
    for x, y, item in placed:
        sample, label = item
        swatch = QRectF(x, y + 2, 10, 10)
        if isinstance(sample, str):
            p.fillRect(swatch, QColor(sample))
        else:
            # A QBrush (e.g. a lithology pattern pixmap brush).
            p.fillRect(swatch, sample)
        p.setPen(QColor("#475569"))
        p.drawRect(swatch)
        p.drawText(
            QRectF(x + 14, y, item_w - 14, item_h),
            str(label)[:14],
        )
    p.restore()


def draw_location_map(
    p: QPainter,
    rect: QRectF,
    points: list[tuple[float, float]],
    highlight: list[int] | None = None,
    label: str = "接合图",
) -> None:
    """Draw a small location map: well scatter + highlighted section wells."""
    p.save()
    p.setPen(QPen(QColor("#94a3b8"), 1.0))
    p.drawRect(rect)
    if len(points) < 2:
        p.drawText(rect, "接合图（井位不足）")
        p.restore()
        return
    lngs = [pt[0] for pt in points]
    lats = [pt[1] for pt in points]
    lng_min, lng_max = min(lngs), max(lngs)
    lat_min, lat_max = min(lats), max(lats)
    span_lng = (lng_max - lng_min) or 1.0
    span_lat = (lat_max - lat_min) or 1.0
    pad = 6.0
    w = float(rect.width()) - 2 * pad
    h = float(rect.height()) - 2 * pad

    def to_xy(pt: tuple[float, float]) -> QPointF:
        x = float(rect.x()) + pad + (pt[0] - lng_min) / span_lng * w
        y = float(rect.y()) + pad + (1.0 - (pt[1] - lat_min) / span_lat) * h
        return QPointF(x, y)

    hl = set(highlight or [])
    # Section wells first (drawn on top with a connector line).
    section_pts = [to_xy(pt) for i, pt in enumerate(points) if i in hl]
    if len(section_pts) >= 2:
        pen = QPen(QColor("#dc2626"), 1.4)
        p.setPen(pen)
        for a, b in zip(section_pts, section_pts[1:]):
            p.drawLine(a, b)
    for i, pt in enumerate(points):
        xy = to_xy(pt)
        if i in hl:
            p.setBrush(QColor("#dc2626"))
            p.setPen(QPen(QColor("#7f1d1d"), 1.0))
        else:
            p.setBrush(QColor("#cbd5e1"))
            p.setPen(QPen(QColor("#64748b"), 1.0))
        p.drawEllipse(xy, 2.2, 2.2)
    # North arrow (top-right corner).
    p.setPen(QPen(QColor("#475569"), 1.0))
    nx = float(rect.right()) - 8
    ny = float(rect.top()) + 8
    p.drawLine(QPointF(nx, ny + 5), QPointF(nx, ny - 5))
    p.drawPolygon(
        QPolygonF(
            [QPointF(nx, ny - 7), QPointF(nx - 3, ny - 1), QPointF(nx + 3, ny - 1)]
        )
    )
    p.drawText(QPointF(nx - 3, ny - 9), "N")
    p.drawText(QPointF(float(rect.x()) + 3, float(rect.bottom()) - 3), label)
    p.restore()


def draw_scale_bar(p: QPainter, rect: QRectF, scale_text: str) -> None:
    """Draw a horizontal scale bar (分段条 + 标注)."""
    p.save()
    p.setPen(QPen(QColor("#475569"), 1.0))
    y = float(rect.center().y())
    x0 = float(rect.x())
    x1 = float(rect.right())
    p.drawLine(QPointF(x0, y), QPointF(x1, y))
    # 4 segments.
    seg = (x1 - x0) / 4.0
    for i in range(5):
        xx = x0 + i * seg
        p.drawLine(QPointF(xx, y - 3), QPointF(xx, y + 3))
    if scale_text:
        p.drawText(QPointF(x0, y - 6), scale_text)
    p.restore()


def draw_ornaments(p: QPainter, rect: QRectF, data: OrnamentData) -> None:
    """Composite ornament layer: legend + location map + title block.

    Layout within ``rect`` (typically the bottom-right of the figure):
    legend (top-left, 1 column), location map (right), title block (bottom).
    """
    p.save()
    if data.is_empty():
        p.restore()
        return
    w = float(rect.width())
    h = float(rect.height())
    # Title block: bottom strip.
    tb_h = min(56.0, h * 0.45)
    tb_rect = QRectF(rect.x(), rect.bottom() - tb_h, w, tb_h)
    draw_title_block(p, tb_rect, data.title_fields)
    # Remaining area: legend left, location map right.
    top_h = h - tb_h
    if top_h <= 0:
        p.restore()
        return
    top_rect = QRectF(rect.x(), rect.y(), w, top_h)
    loc_w = min(w * 0.45, 110.0)
    loc_rect = QRectF(top_rect.right() - loc_w, top_rect.y(), loc_w, top_h)
    legend_rect = QRectF(top_rect.x(), top_rect.y(), w - loc_w, top_h)
    if data.map_points:
        draw_location_map(p, loc_rect, data.map_points, data.map_highlight)
    if data.legend_items:
        draw_legend_block(p, legend_rect, data.legend_items, cols=1)
    if data.scale_text:
        draw_scale_bar(
            p,
            QRectF(rect.x() + 4, top_rect.y() + 2, min(w * 0.3, 80.0), 14),
            data.scale_text,
        )
    p.restore()
