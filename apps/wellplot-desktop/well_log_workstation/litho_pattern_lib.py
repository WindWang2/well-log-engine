"""SY/T 5615 lithology pattern library — loading + Qt vector rendering.

A *pattern* is a JSON tile definition of line / polyline / circle primitives,
structurally identical to the SDK ``PatternDefinition`` payload consumed by
``welllog.submit_multi_track`` (see ``numpy_bridge.cpp`` pattern parsing).
One definition therefore serves two consumers:

* **Qt host canvas** — :func:`make_qbrush` rasterizes the tile into a
  ``QPixmap`` and returns a tiling ``QBrush`` (true vector primitives, not
  the legacy ``Qt.BrushStyle.Dense4Pattern`` approximation).
* **Engine export** — :func:`pattern_to_engine_payload` returns the dict the
  bridge already understands; no conversion loss.

The catalog lives under ``well_log_workstation/litho_patterns/*.json`` and is
loaded lazily + cached, mirroring ``template_model.list_builtin_templates``.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# Primitive vocab matches the SDK (scene/scene.hpp PatternPrimitive variant).
_PRIMITIVE_KEYS = ("line", "polyline", "circle")


@dataclass(frozen=True)
class PatternPrimitive:
    """One tile primitive: exactly one of line / polyline / circle is set."""

    kind: str  # "line" | "polyline" | "circle"
    line: tuple[float, float, float, float] | None = None  # from_x,from_y,to_x,to_y
    polyline: tuple[list[tuple[float, float]], bool] | None = None  # (points, closed)
    circle: tuple[float, float, float, bool] | None = None  # cx, cy, radius, filled


@dataclass(frozen=True)
class LithoPattern:
    """A repeating lithology tile (SY/T 5615 entry)."""

    id: str
    name: str
    name_en: str
    tile_w: float
    tile_h: float
    rotation: float
    stroke_w: float
    foreground: str
    background: str
    primitives: tuple[PatternPrimitive, ...] = ()


def _patterns_package_dir() -> Path:
    return Path(__file__).resolve().parent / "litho_patterns"


def _parse_primitive(raw: dict[str, Any]) -> PatternPrimitive | None:
    """Parse one primitive dict; reject anything outside the allowed vocab."""
    if not isinstance(raw, dict):
        return None
    for key in _PRIMITIVE_KEYS:
        body = raw.get(key)
        if not isinstance(body, dict):
            continue
        if key == "line":
            try:
                return PatternPrimitive(
                    kind="line",
                    line=(
                        float(body["from_x"]),
                        float(body["from_y"]),
                        float(body["to_x"]),
                        float(body["to_y"]),
                    ),
                )
            except (KeyError, TypeError, ValueError):
                return None
        if key == "polyline":
            raw_pts = body.get("points")
            if not isinstance(raw_pts, list):
                return None
            pts: list[tuple[float, float]] = []
            for p in raw_pts:
                if isinstance(p, (list, tuple)) and len(p) >= 2:
                    try:
                        pts.append((float(p[0]), float(p[1])))
                    except (TypeError, ValueError):
                        continue
            if not pts:
                return None
            closed = bool(body.get("closed", False))
            return PatternPrimitive(
                kind="polyline", polyline=(pts, closed)
            )
        if key == "circle":
            try:
                return PatternPrimitive(
                    kind="circle",
                    circle=(
                        float(body["center_x"]),
                        float(body["center_y"]),
                        float(body["radius_mm"]),
                        bool(body.get("filled", False)),
                    ),
                )
            except (KeyError, TypeError, ValueError):
                return None
    return None


def _parse_pattern(data: dict[str, Any]) -> LithoPattern | None:
    """Parse one pattern dict from the catalog JSON."""
    if not isinstance(data, dict):
        return None
    try:
        pid = str(data["id"])
        prims_raw = data.get("primitives") or []
    except KeyError:
        return None
    prims = tuple(
        p for p in (_parse_primitive(r) for r in prims_raw if isinstance(r, dict))
        if p is not None
    )
    return LithoPattern(
        id=pid,
        name=str(data.get("name") or pid),
        name_en=str(data.get("name_en") or ""),
        tile_w=float(data.get("tile_width_mm", 5.0)),
        tile_h=float(data.get("tile_height_mm", 5.0)),
        rotation=float(data.get("rotation_degrees", 0.0)),
        stroke_w=float(data.get("stroke_width_mm", 0.2)),
        foreground=str(data.get("foreground") or "#374151"),
        background=str(data.get("background") or "#ffffff"),
        primitives=prims,
    )


_CACHE: dict[str, LithoPattern] | None = None


def load_builtin_patterns() -> dict[str, LithoPattern]:
    """Load + cache every pattern from the litho_patterns package dir."""
    global _CACHE
    if _CACHE is not None:
        return dict(_CACHE)
    out: dict[str, LithoPattern] = {}
    root = _patterns_package_dir()
    if root.is_dir():
        for path in sorted(root.glob("*.json")):
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                continue
            entries = data.get("patterns") if isinstance(data, dict) else None
            if not isinstance(entries, list):
                continue
            for raw in entries:
                pat = _parse_pattern(raw)
                if pat is not None:
                    out[pat.id] = pat
    _CACHE = out
    return dict(out)


def get_pattern(pattern_id: str) -> LithoPattern | None:
    """Return one builtin pattern by id, or None."""
    if not pattern_id:
        return None
    return load_builtin_patterns().get(str(pattern_id))


def pattern_to_engine_payload(pat: LithoPattern) -> dict[str, Any]:
    """Convert a LithoPattern to the dict ``welllog.submit_multi_track`` expects.

    Matches the SDK parsing contract in numpy_bridge.cpp (pattern loop): keys
    id/tile_width_mm/tile_height_mm/rotation_degrees/stroke_width_mm/
    foreground/background/primitives[], each primitive a one-key dict.
    """
    prims: list[dict[str, Any]] = []
    for prim in pat.primitives:
        if prim.kind == "line" and prim.line:
            fx, fy, tx, ty = prim.line
            prims.append(
                {"line": {"from_x": fx, "from_y": fy, "to_x": tx, "to_y": ty}}
            )
        elif prim.kind == "polyline" and prim.polyline:
            pts, closed = prim.polyline
            prims.append(
                {
                    "polyline": {
                        "points": [list(p) for p in pts],
                        "closed": 1 if closed else 0,
                    }
                }
            )
        elif prim.kind == "circle" and prim.circle:
            cx, cy, r, filled = prim.circle
            prims.append(
                {
                    "circle": {
                        "center_x": cx,
                        "center_y": cy,
                        "radius_mm": r,
                        "filled": 1 if filled else 0,
                    }
                }
            )
    return {
        "id": pat.id,
        "tile_width_mm": pat.tile_w,
        "tile_height_mm": pat.tile_h,
        "rotation_degrees": pat.rotation,
        "stroke_width_mm": pat.stroke_w,
        "foreground": pat.foreground,
        "background": pat.background,
        "primitives": prims,
    }


def _mm_to_px(mm: float, dpi: float = 96.0) -> int:
    """Convert millimetres to pixels (rounded up, min 1)."""
    return max(1, int(math.ceil(mm * dpi / 25.4)))


def make_qbrush(pat: LithoPattern, fallback_color: str = "#93c5fd", *, dpi: float = 96.0):
    """Rasterize a pattern tile into a tiling QBrush (true vector primitives).

    Returns a plain-color QBrush if PySide6 is unavailable or the pattern has
    no primitives, so callers can use it unconditionally.
    """
    if not pat.primitives:
        from PySide6.QtGui import QBrush, QColor

        return QBrush(QColor(fallback_color))
    from PySide6.QtCore import QPointF
    from PySide6.QtGui import QBrush, QColor, QPainter, QPen, QPixmap

    pw = _mm_to_px(pat.tile_w, dpi)
    ph = _mm_to_px(pat.tile_h, dpi)
    pixmap = QPixmap(pw, ph)
    pixmap.fill(QColor(pat.background))
    p = QPainter(pixmap)
    p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    pen = QPen(QColor(pat.foreground))
    pen.setWidthF(max(1.0, _mm_to_px(pat.stroke_w, dpi)))
    p.setPen(pen)
    if pat.rotation:
        p.translate(pw / 2.0, ph / 2.0)
        p.rotate(pat.rotation)
        p.translate(-pw / 2.0, -ph / 2.0)

    def to_px(x_mm: float, y_mm: float) -> QPointF:
        return QPointF(x_mm / pat.tile_w * pw, y_mm / pat.tile_h * ph)

    for prim in pat.primitives:
        if prim.kind == "line" and prim.line:
            fx, fy, tx, ty = prim.line
            p.drawLine(to_px(fx, fy), to_px(tx, ty))
        elif prim.kind == "polyline" and prim.polyline:
            pts, closed = prim.polyline
            qpts = [to_px(x, y) for x, y in pts]
            if len(qpts) >= 2:
                from PySide6.QtGui import QPolygonF

                poly = QPolygonF(qpts)
                if closed:
                    p.drawPolygon(poly)
                else:
                    p.drawPolyline(poly)
        elif prim.kind == "circle" and prim.circle:
            cx, cy, r, filled = prim.circle
            rx = r / pat.tile_w * pw
            ry = r / pat.tile_h * ph
            center = to_px(cx, cy)
            if filled:
                p.setBrush(QColor(pat.foreground))
            from PySide6.QtCore import QRectF

            p.drawEllipse(QRectF(center.x() - rx, center.y() - ry, 2 * rx, 2 * ry))
            if filled:
                p.setBrush(QBrush())  # reset to no-fill for subsequent prims
    p.end()
    return QBrush(pixmap)
